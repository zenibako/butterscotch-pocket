/*
 * openfpgaOS audio backend for Butterscotch.
 *
 * Every sound the game plays is converted offline by mkmusic into music.bin
 * (mono IMA ADPCM, see ut_music_pack.h): the external .ogg files it streams
 * and the short effects embedded in data.win. This backend decodes and
 * resamples them to the 48 kHz output, mixes them in software and feeds the
 * result to of_audio_write().
 *
 * Long tracks are streamed from the pack through a read-ahead buffer. Short
 * ones are loaded whole the first time they play and kept in a small cache,
 * so an effect that fires every few frames costs no file access.
 */

#include "of.h"

#include "audio_system.h"
#include "log.h"
#include "stb_ds.h"
#include "utils.h"

#include "ut_music_pack.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UT_MUSIC_PATH "music.bin"

/* Id ranges shared with Butterscotch's other backends. */
#define UT_INSTANCE_ID_BASE 100000
#define UT_STREAM_INDEX_BASE 300000

#define UT_MAX_STREAMS 32
#define UT_MAX_VOICES 16
/* Voices that can stream at once; each has a read-ahead buffer. */
#define UT_MAX_STREAMED 4

/* Tracks up to this many compressed bytes (6 s) are played from memory. */
#define UT_MEMORY_SOUND_BYTES (96u * 1024u)
/* Memory kept for cached short sounds before the least recently played go. */
#define UT_SOUND_CACHE_BYTES (3u * 1024u * 1024u)

/* Compressed bytes buffered per voice: 4 s at 32 kHz. Blocking file reads
 * elsewhere (room and texture loads) are bridged from this buffer. */
#define UT_READAHEAD 65536
#define UT_REFILL_BELOW (UT_READAHEAD / 2)

/* Output queued ahead of the DAC. A sound is heard this long after it is
 * started, so it is a trade against dropouts when a frame runs long. */
#define UT_QUEUE_TARGET_PAIRS (OF_AUDIO_RATE / 10)
#define UT_MIX_CHUNK_PAIRS 256
#define UT_GAIN_ONE 4096

typedef struct {
    bool active;
    int32_t track;
    float gain;
    float pitch;
} UtStream;

typedef struct {
    bool active;
    bool paused;
    bool loop;
    bool finished; /* a non-looping track has played its last sample */
    int32_t instanceId;
    int32_t sourceIndex; /* SOND index or stream index this voice was started from */
    int32_t track;

    /* Compressed data being decoded: a stream slot's read-ahead buffer, or a
     * whole cached sound. */
    int32_t streamSlot; /* index into streamBuffers, or -1 when played from memory */
    uint8_t *data;
    uint32_t bufferLen;
    uint32_t bufferPos;
    uint32_t fileBytePos; /* streamed: next byte of the track to read into the buffer */

    /* ADPCM decode. */
    UtAdpcmState adpcm;
    uint32_t samplesDecoded;
    uint32_t pendingCode;
    bool havePendingCode;

    /* Linear resampling from the track rate to the output rate, 16.16. */
    int32_t sample0, sample1;
    uint32_t frac;
    uint32_t step;
    float pitch;

    /* Gain ramp, in UT_GAIN_ONE units scaled by 256 for sub-step precision. */
    int32_t gainNow;
    int32_t gainTarget;
    int32_t gainStepPerPair;
} UtVoice;

typedef struct {
    AudioSystem base;
    FILE *file;
    UtMusicTrack *tracks;
    uint32_t trackCount;
    UtStream streams[UT_MAX_STREAMS];
    UtVoice voices[UT_MAX_VOICES];
    uint8_t streamBuffers[UT_MAX_STREAMED][UT_READAHEAD];
    /* Short sounds held in memory, indexed by track; NULL when not loaded. */
    uint8_t **cachedSounds;
    uint32_t *cachedLastUse;
    uint32_t cachedBytes;
    uint32_t useCounter;
    int32_t nextInstanceId;
    int32_t queueCapacity;
    float masterGain;
    bool allPaused;
    FILE *dump; /* desktop only: raw copy of everything written to the output */
} UtAudioSystem;

/* The file idle hook has no user pointer, so the one instance is global. */
static UtAudioSystem *g_audio = NULL;

/* ===[ Pack lookup ]=== */

static void openPack(UtAudioSystem *ut) {
    FILE *file = fopen(UT_MUSIC_PATH, "rb");
    if (file == NULL) {
        logInfo("Audio: no %s, sound is disabled.\n", UT_MUSIC_PATH);
        return;
    }

    char magic[4];
    uint32_t count = 0;
    if (fread(magic, 1, 4, file) != 4 || memcmp(magic, UT_MUSIC_MAGIC, 4) != 0 ||
        fread(&count, sizeof(count), 1, file) != 1 || count == 0 || count > 4096) {
        logWarn("Audio: %s is not an audio pack, sound is disabled.\n", UT_MUSIC_PATH);
        fclose(file);
        return;
    }

    UtMusicTrack *tracks = malloc(count * sizeof(UtMusicTrack));
    if (tracks == NULL || fread(tracks, sizeof(UtMusicTrack), count, file) != count) {
        logWarn("Audio: %s index is truncated, sound is disabled.\n", UT_MUSIC_PATH);
        free(tracks);
        fclose(file);
        return;
    }

    /* Reads are explicit, buffer-sized chunks; stdio buffering would only add a copy. */
    setvbuf(file, NULL, _IONBF, 0);
    ut->file = file;
    ut->tracks = tracks;
    ut->trackCount = count;
    ut->cachedSounds = safeCalloc(count, sizeof(uint8_t *));
    ut->cachedLastUse = safeCalloc(count, sizeof(uint32_t));
    logInfo("Audio: pack with %u tracks.\n", (unsigned) count);
}

/* Maps "music/mus_story.ogg" or "mus_story.ogg" to a track index, or -1. */
static int32_t findTrack(const UtAudioSystem *ut, const char *filename) {
    if (filename == NULL || ut->trackCount == 0) return -1;

    const char *base = filename;
    for (const char *p = filename; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    char name[UT_MUSIC_NAME_LEN] = {0};
    size_t len = 0;
    while (base[len] != '\0' && base[len] != '.' && len < UT_MUSIC_NAME_LEN - 1) {
        name[len] = (char) tolower((unsigned char) base[len]);
        len++;
    }

    int32_t lo = 0, hi = (int32_t) ut->trackCount - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        int cmp = strncmp(name, ut->tracks[mid].name, UT_MUSIC_NAME_LEN);
        if (cmp == 0) return mid;
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

/* ===[ Voice streaming and decode ]=== */

static uint32_t trackBytes(const UtMusicTrack *track) {
    return track->sampleCount / 2;
}

static void updateStep(const UtAudioSystem *ut, UtVoice *voice) {
    float rate = (float) ut->tracks[voice->track].sampleRate * voice->pitch;
    float step = rate * 65536.0f / (float) OF_AUDIO_RATE;
    if (step < 1.0f) step = 1.0f;
    if (step > 8.0f * 65536.0f) step = 8.0f * 65536.0f;
    voice->step = (uint32_t) step;
}

/* Tops up a voice's read-ahead from the pack. Main loop only: this blocks. */
static void refillVoice(UtAudioSystem *ut, UtVoice *voice) {
    if (voice->streamSlot < 0) return;
    if (voice->bufferLen - voice->bufferPos >= UT_REFILL_BELOW) return;

    const UtMusicTrack *track = &ut->tracks[voice->track];
    uint32_t total = trackBytes(track);
    if (total == 0) return;

    uint32_t remaining = voice->bufferLen - voice->bufferPos;
    memmove(voice->data, voice->data + voice->bufferPos, remaining);
    voice->bufferPos = 0;
    voice->bufferLen = remaining;

    while (voice->bufferLen < UT_READAHEAD) {
        if (voice->fileBytePos >= total) {
            if (!voice->loop) break;
            voice->fileBytePos = 0;
        }
        uint32_t want = UT_READAHEAD - voice->bufferLen;
        if (want > total - voice->fileBytePos) want = total - voice->fileBytePos;

        if (fseek(ut->file, (long) (track->offset + voice->fileBytePos), SEEK_SET) != 0) break;
        /* Read into static memory and copy: on openfpgaOS a read straight
         * into the heap (where the voices live) is about ten times slower.
         * The idle hook may mix from this voice while fread blocks; it only
         * touches bytes below bufferLen, which is not advanced until after. */
        static uint8_t scratch[UT_READAHEAD] __attribute__((aligned(512)));
        size_t got = fread(scratch, 1, want, ut->file);
        if (got == 0) break;
        memcpy(voice->data + voice->bufferLen, scratch, got);
        voice->bufferLen += (uint32_t) got;
        voice->fileBytePos += (uint32_t) got;
    }
}

/* Produces the next decoded sample, or false if none is available right now
 * (end of a non-looping track, or the read-ahead ran dry). */
static bool nextSample(const UtAudioSystem *ut, UtVoice *voice, int32_t *out) {
    const UtMusicTrack *track = &ut->tracks[voice->track];

    if (voice->samplesDecoded >= track->sampleCount) {
        if (!voice->loop || track->sampleCount == 0) {
            voice->finished = true;
            return false;
        }
        voice->samplesDecoded = 0;
        voice->adpcm.predictor = 0;
        voice->adpcm.stepIndex = 0;
        voice->havePendingCode = false;
        /* A stream's refill wraps the data for us; a cached sound starts over. */
        if (voice->streamSlot < 0) voice->bufferPos = 0;
    }

    uint32_t code;
    if (voice->havePendingCode) {
        code = voice->pendingCode;
        voice->havePendingCode = false;
    } else {
        if (voice->bufferPos >= voice->bufferLen) return false;
        uint8_t byte = voice->data[voice->bufferPos++];
        code = byte & 0x0F;
        voice->pendingCode = byte >> 4;
        voice->havePendingCode = true;
    }

    *out = utAdpcmDecode(&voice->adpcm, code);
    voice->samplesDecoded++;
    return true;
}

/* Adds `pairs` output samples of one voice into the mono mix buffer. */
static void mixVoice(const UtAudioSystem *ut, UtVoice *voice, int32_t *mix, int pairs) {
    for (int i = 0; i < pairs; i++) {
        while (voice->frac >= 65536) {
            int32_t next;
            if (!nextSample(ut, voice, &next)) {
                if (!voice->finished) return; /* starved: resume here once refilled */
                next = 0;
                if (voice->sample1 == 0) {
                    voice->active = false;
                    return;
                }
            }
            voice->sample0 = voice->sample1;
            voice->sample1 = next;
            voice->frac -= 65536;
        }

        int32_t sample = voice->sample0 + (((voice->sample1 - voice->sample0) * (int32_t) (voice->frac >> 4)) >> 12);
        voice->frac += voice->step;

        if (voice->gainNow != voice->gainTarget) {
            int32_t delta = voice->gainTarget - voice->gainNow;
            if (delta > voice->gainStepPerPair) delta = voice->gainStepPerPair;
            if (delta < -voice->gainStepPerPair) delta = -voice->gainStepPerPair;
            voice->gainNow += delta;
        }
        mix[i] += (sample * (voice->gainNow >> 8)) / UT_GAIN_ONE;
    }
}

static void mixAndWrite(UtAudioSystem *ut, int pairs) {
    static int32_t mix[UT_MIX_CHUNK_PAIRS];
    static int16_t out[UT_MIX_CHUNK_PAIRS * 2];
    int32_t master = (int32_t) (ut->masterGain * (float) UT_GAIN_ONE);

    while (pairs > 0) {
        int chunk = pairs < UT_MIX_CHUNK_PAIRS ? pairs : UT_MIX_CHUNK_PAIRS;
        memset(mix, 0, (size_t) chunk * sizeof(int32_t));

        if (!ut->allPaused) {
            for (int v = 0; v < UT_MAX_VOICES; v++) {
                UtVoice *voice = &ut->voices[v];
                if (voice->active && !voice->paused) mixVoice(ut, voice, mix, chunk);
            }
        }

        for (int i = 0; i < chunk; i++) {
            /* Clamp before applying the master gain so the product fits in 32 bits. */
            int32_t sample = mix[i];
            if (sample > 65535) sample = 65535;
            if (sample < -65535) sample = -65535;
            sample = (sample * master) / UT_GAIN_ONE;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            out[i * 2] = (int16_t) sample;
            out[i * 2 + 1] = (int16_t) sample;
        }

        if (ut->dump != NULL) fwrite(out, sizeof(int16_t) * 2, (size_t) chunk, ut->dump);
        of_audio_write(out, chunk);
        pairs -= chunk;
    }
}

/* Keeps the output queue topped up to the target. Does no file I/O, so it is
 * safe to call from the file idle hook. */
static void pumpOutput(UtAudioSystem *ut) {
    /* Some targets have a ring smaller than the target; never aim past 3/4 of it. */
    int target = UT_QUEUE_TARGET_PAIRS;
    if (target > ut->queueCapacity * 3 / 4) target = ut->queueCapacity * 3 / 4;

    int queued = ut->queueCapacity - of_audio_free();
    int want = target - queued;
    if (want > 0) mixAndWrite(ut, want);
}

#ifndef OF_PC
static void idleHook(void) {
    if (g_audio != NULL && g_audio->file != NULL) pumpOutput(g_audio);
}
#endif

/* ===[ Short-sound cache ]=== */

static bool soundInUse(const UtAudioSystem *ut, int32_t track) {
    for (int v = 0; v < UT_MAX_VOICES; v++) {
        if (ut->voices[v].active && ut->voices[v].streamSlot < 0 && ut->voices[v].track == track) return true;
    }
    return false;
}

static void evictSounds(UtAudioSystem *ut, uint32_t needed) {
    while (ut->cachedBytes + needed > UT_SOUND_CACHE_BYTES) {
        int32_t victim = -1;
        for (uint32_t t = 0; t < ut->trackCount; t++) {
            if (ut->cachedSounds[t] == NULL || soundInUse(ut, (int32_t) t)) continue;
            if (victim < 0 || ut->cachedLastUse[t] < ut->cachedLastUse[victim]) victim = (int32_t) t;
        }
        if (victim < 0) return;
        free(ut->cachedSounds[victim]);
        ut->cachedSounds[victim] = NULL;
        ut->cachedBytes -= trackBytes(&ut->tracks[victim]);
    }
}

/* Returns the whole compressed sound, loading it on first use. */
static uint8_t *cachedSound(UtAudioSystem *ut, int32_t track) {
    ut->cachedLastUse[track] = ++ut->useCounter;
    if (ut->cachedSounds[track] != NULL) return ut->cachedSounds[track];

    uint32_t bytes = trackBytes(&ut->tracks[track]);
    if (bytes == 0) return NULL;
    evictSounds(ut, bytes);

    uint8_t *data = malloc(bytes);
    if (data == NULL) return NULL;
    if (fseek(ut->file, (long) ut->tracks[track].offset, SEEK_SET) != 0 || fread(data, 1, bytes, ut->file) != bytes) {
        free(data);
        return NULL;
    }
    ut->cachedSounds[track] = data;
    ut->cachedBytes += bytes;
    return data;
}

/* ===[ Voice lookup helpers ]=== */

static bool voiceMatches(const UtVoice *voice, int32_t soundOrInstance) {
    if (!voice->active) return false;
    if (soundOrInstance >= UT_INSTANCE_ID_BASE && soundOrInstance < UT_STREAM_INDEX_BASE)
        return voice->instanceId == soundOrInstance;
    return voice->sourceIndex == soundOrInstance;
}

static UtStream *streamFor(UtAudioSystem *ut, int32_t index) {
    int32_t slot = index - UT_STREAM_INDEX_BASE;
    if (slot < 0 || slot >= UT_MAX_STREAMS || !ut->streams[slot].active) return NULL;
    return &ut->streams[slot];
}

static int32_t gainToFixed(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
    return (int32_t) (gain * (float) UT_GAIN_ONE) << 8;
}

/* ===[ Vtable ]=== */

static void utInit(AudioSystem *audio, DataWin *dataWin, FileSystem *fileSystem) {
    (void) fileSystem;
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    arrput(audio->audioGroups, dataWin);

    of_audio_init();
    ut->queueCapacity = of_audio_free();
    ut->masterGain = 1.0f;
    ut->nextInstanceId = UT_INSTANCE_ID_BASE;
    openPack(ut);

#ifdef OF_PC
    const char *dumpPath = getenv("UT_AUDIO_DUMP");
    if (dumpPath != NULL) ut->dump = fopen(dumpPath, "wb");
#else
    of_file_set_idle_hook(idleHook);
#endif
}

static void utDestroy(AudioSystem *audio) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
#ifndef OF_PC
    of_file_set_idle_hook(NULL);
#endif
    g_audio = NULL;
    if (ut->file != NULL) fclose(ut->file);
    if (ut->dump != NULL) fclose(ut->dump);
    if (ut->cachedSounds != NULL) {
        for (uint32_t t = 0; t < ut->trackCount; t++) free(ut->cachedSounds[t]);
    }
    free(ut->cachedSounds);
    free(ut->cachedLastUse);
    free(ut->tracks);
    free(ut);
}

static void utUpdate(AudioSystem *audio, float deltaTime) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    if (ut->file == NULL) return;

    for (int v = 0; v < UT_MAX_VOICES; v++) {
        if (ut->voices[v].active) refillVoice(ut, &ut->voices[v]);
    }

    if (ut->dump != NULL) {
        /* Dump mode follows game time rather than the output queue, so the
         * captured audio is the same on every run. */
        mixAndWrite(ut, (int) (deltaTime * (float) OF_AUDIO_RATE + 0.5f));
        return;
    }
    pumpOutput(ut);
}

static int32_t utPlaySound(AudioSystem *audio, int32_t soundIndex, int32_t priority, bool loop) {
    (void) priority;
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    if (ut->file == NULL) return -1;

    int32_t track = -1;
    float gain = 1.0f, pitch = 1.0f;

    if (soundIndex >= UT_STREAM_INDEX_BASE) {
        UtStream *stream = streamFor(ut, soundIndex);
        if (stream == NULL) return -1;
        track = stream->track;
        gain = stream->gain;
        pitch = stream->pitch;
    } else {
        DataWin *dw = audio->audioGroups[0];
        if (soundIndex < 0 || (uint32_t) soundIndex >= dw->sond.count) return -1;
        const Sound *sound = &dw->sond.sounds[soundIndex];
        /* Streamed sounds are packed under their file name, embedded ones
         * under the sound's own name. */
        bool embedded = (sound->flags & (AUDIO_ENTRY_FLAG_IS_EMBEDDED | AUDIO_ENTRY_FLAG_IS_COMPRESSED)) != 0;
        track = findTrack(ut, embedded ? sound->name : sound->file);
        if (track < 0) track = findTrack(ut, embedded ? sound->file : sound->name);
        gain = sound->volume;
        pitch = sound->pitch > 0.0f ? sound->pitch : 1.0f;
    }
    if (track < 0) return -1;

    bool streamed = trackBytes(&ut->tracks[track]) > UT_MEMORY_SOUND_BYTES;
    uint8_t *soundData = NULL;
    if (!streamed) {
        soundData = cachedSound(ut, track);
        if (soundData == NULL) return -1;
    }

    /* Take a free voice. Failing that, take over the oldest one of the same
     * kind, so effects never cut off the music and the reverse. */
    UtVoice *voice = NULL;
    int streamedCount = 0;
    for (int v = 0; v < UT_MAX_VOICES; v++) {
        if (!ut->voices[v].active) {
            if (voice == NULL) voice = &ut->voices[v];
        } else if (ut->voices[v].streamSlot >= 0) {
            streamedCount++;
        }
    }
    if (voice == NULL || (streamed && streamedCount >= UT_MAX_STREAMED)) {
        voice = NULL;
        for (int v = 0; v < UT_MAX_VOICES; v++) {
            UtVoice *candidate = &ut->voices[v];
            if (!candidate->active || (candidate->streamSlot >= 0) != streamed) continue;
            if (voice == NULL || candidate->instanceId < voice->instanceId) voice = candidate;
        }
        if (voice == NULL) return -1;
        voice->active = false;
    }

    int32_t streamSlot = -1;
    if (streamed) {
        for (int32_t slot = 0; slot < UT_MAX_STREAMED && streamSlot < 0; slot++) {
            bool taken = false;
            for (int v = 0; v < UT_MAX_VOICES; v++)
                taken = taken || (ut->voices[v].active && ut->voices[v].streamSlot == slot);
            if (!taken) streamSlot = slot;
        }
        if (streamSlot < 0) return -1;
    }

    memset(voice, 0, sizeof(*voice));
    voice->streamSlot = streamSlot;
    if (streamed) {
        voice->data = ut->streamBuffers[streamSlot];
    } else {
        voice->data = soundData;
        voice->bufferLen = trackBytes(&ut->tracks[track]);
    }
    voice->frac = 65536;
    voice->loop = loop;
    voice->track = track;
    voice->sourceIndex = soundIndex;
    voice->instanceId = ut->nextInstanceId++;
    voice->pitch = pitch;
    voice->gainNow = voice->gainTarget = gainToFixed(gain);
    voice->gainStepPerPair = 0;
    updateStep(ut, voice);
    voice->active = true;

    refillVoice(ut, voice);
    /* Effects fire constantly; only log the long tracks (UT_AUDIO_LOG=1 on
     * desktop logs everything). */
#ifdef OF_PC
    static int logAll = -1;
    if (logAll < 0) logAll = getenv("UT_AUDIO_LOG") != NULL;
    if (logAll && !streamed) logInfo("Audio: sfx %s gain %.2f pitch %.2f\n", ut->tracks[track].name, (double) gain, (double) pitch);
#endif
    if (streamed)
        logInfo("Audio: playing %s%s gain %.2f pitch %.2f\n", ut->tracks[track].name, loop ? " (loop)" : "",
                (double) gain, (double) pitch);
    return voice->instanceId;
}

static void utStopSound(AudioSystem *audio, int32_t soundOrInstance) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    for (int v = 0; v < UT_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) ut->voices[v].active = false;
    }
}

static void utStopAll(AudioSystem *audio) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    for (int v = 0; v < UT_MAX_VOICES; v++) ut->voices[v].active = false;
}

static bool utIsPlaying(AudioSystem *audio, int32_t soundOrInstance) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    for (int v = 0; v < UT_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) return true;
    }
    return false;
}

static void setPaused(UtAudioSystem *ut, int32_t soundOrInstance, bool paused) {
    for (int v = 0; v < UT_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) ut->voices[v].paused = paused;
    }
}

static void utPauseSound(AudioSystem *audio, int32_t soundOrInstance) {
    setPaused((UtAudioSystem *) audio, soundOrInstance, true);
}

static void utResumeSound(AudioSystem *audio, int32_t soundOrInstance) {
    setPaused((UtAudioSystem *) audio, soundOrInstance, false);
}

static void utPauseAll(AudioSystem *audio) {
    ((UtAudioSystem *) audio)->allPaused = true;
}

static void utResumeAll(AudioSystem *audio) {
    ((UtAudioSystem *) audio)->allPaused = false;
}

static void utSetSoundGain(AudioSystem *audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    UtStream *stream = streamFor(ut, soundOrInstance);
    if (stream != NULL) stream->gain = gain;

    for (int v = 0; v < UT_MAX_VOICES; v++) {
        UtVoice *voice = &ut->voices[v];
        if (!voiceMatches(voice, soundOrInstance)) continue;

        voice->gainTarget = gainToFixed(gain);
        int32_t pairs = (int32_t) ((uint64_t) timeMs * OF_AUDIO_RATE / 1000u);
        if (pairs <= 0) {
            voice->gainNow = voice->gainTarget;
            continue;
        }
        int32_t distance = voice->gainTarget - voice->gainNow;
        if (distance < 0) distance = -distance;
        voice->gainStepPerPair = distance / pairs;
        if (voice->gainStepPerPair < 1) voice->gainStepPerPair = 1;
    }
}

static float utGetSoundGain(AudioSystem *audio, int32_t soundOrInstance) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    for (int v = 0; v < UT_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance))
            return (float) (ut->voices[v].gainNow >> 8) / (float) UT_GAIN_ONE;
    }
    UtStream *stream = streamFor(ut, soundOrInstance);
    return stream != NULL ? stream->gain : 1.0f;
}

static void utSetSoundPitch(AudioSystem *audio, int32_t soundOrInstance, float pitch) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    if (pitch <= 0.0f) pitch = 0.01f;
    UtStream *stream = streamFor(ut, soundOrInstance);
    if (stream != NULL) stream->pitch = pitch;

    for (int v = 0; v < UT_MAX_VOICES; v++) {
        UtVoice *voice = &ut->voices[v];
        if (!voiceMatches(voice, soundOrInstance)) continue;
        voice->pitch = pitch;
        updateStep(ut, voice);
    }
}

static float utGetSoundPitch(AudioSystem *audio, int32_t soundOrInstance) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    for (int v = 0; v < UT_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) return ut->voices[v].pitch;
    }
    UtStream *stream = streamFor(ut, soundOrInstance);
    return stream != NULL ? stream->pitch : 1.0f;
}

static float utGetTrackPosition(AudioSystem *audio, int32_t soundOrInstance) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    for (int v = 0; v < UT_MAX_VOICES; v++) {
        const UtVoice *voice = &ut->voices[v];
        if (voiceMatches(voice, soundOrInstance))
            return (float) voice->samplesDecoded / (float) ut->tracks[voice->track].sampleRate;
    }
    return 0.0f;
}

/* Seeks by restarting the decoder at the target byte. ADPCM state is not
 * stored per position, so the first few milliseconds after a seek to
 * anywhere but the start are approximate. */
static void utSetTrackPosition(AudioSystem *audio, int32_t soundOrInstance, float positionSeconds) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    for (int v = 0; v < UT_MAX_VOICES; v++) {
        UtVoice *voice = &ut->voices[v];
        if (!voiceMatches(voice, soundOrInstance)) continue;

        const UtMusicTrack *track = &ut->tracks[voice->track];
        if (positionSeconds < 0.0f) positionSeconds = 0.0f;
        uint32_t sample = (uint32_t) (positionSeconds * (float) track->sampleRate) & ~1u;
        if (sample >= track->sampleCount) sample = 0;

        if (voice->streamSlot >= 0) {
            voice->bufferLen = voice->bufferPos = 0;
            voice->fileBytePos = sample / 2;
        } else {
            voice->bufferPos = sample / 2;
        }
        voice->samplesDecoded = sample;
        voice->adpcm.predictor = voice->adpcm.stepIndex = 0;
        voice->havePendingCode = false;
        voice->finished = false;
        refillVoice(ut, voice);
    }
}

static float utGetSoundLength(AudioSystem *audio, int32_t soundOrInstance) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    int32_t track = -1;

    for (int v = 0; v < UT_MAX_VOICES && track < 0; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) track = ut->voices[v].track;
    }
    if (track < 0) {
        UtStream *stream = streamFor(ut, soundOrInstance);
        if (stream != NULL) {
            track = stream->track;
        } else if (soundOrInstance >= 0 && soundOrInstance < UT_INSTANCE_ID_BASE) {
            DataWin *dw = audio->audioGroups[0];
            if ((uint32_t) soundOrInstance < dw->sond.count) {
                track = findTrack(ut, dw->sond.sounds[soundOrInstance].file);
                if (track < 0) track = findTrack(ut, dw->sond.sounds[soundOrInstance].name);
            }
        }
    }
    if (track < 0) return 0.0f;
    return (float) ut->tracks[track].sampleCount / (float) ut->tracks[track].sampleRate;
}

static void utSetMasterGain(AudioSystem *audio, float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 2.0f) gain = 2.0f;
    ((UtAudioSystem *) audio)->masterGain = gain;
}

static void utSetMasterGainForListener(AudioSystem *audio, float gain, int32_t listenerId) {
    (void) listenerId;
    utSetMasterGain(audio, gain);
}

static void utSetChannelCount(AudioSystem *audio, int32_t count) {
    (void) audio;
    (void) count;
}

static void utGroupLoad(AudioSystem *audio, int32_t groupIndex) {
    (void) audio;
    (void) groupIndex;
}

static bool utGroupIsLoaded(AudioSystem *audio, int32_t groupIndex) {
    (void) audio;
    (void) groupIndex;
    return true;
}

static int32_t utCreateStream(AudioSystem *audio, const char *filename) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    int32_t track = findTrack(ut, filename);
    if (track < 0) {
        if (ut->trackCount > 0) logWarn("Audio: '%s' is not in the music pack.\n", filename);
        return -1;
    }

    for (int32_t i = 0; i < UT_MAX_STREAMS; i++) {
        if (ut->streams[i].active) continue;
        ut->streams[i].active = true;
        ut->streams[i].track = track;
        ut->streams[i].gain = 1.0f;
        ut->streams[i].pitch = 1.0f;
        return UT_STREAM_INDEX_BASE + i;
    }
    logWarn("Audio: no free stream slots for '%s'.\n", filename);
    return -1;
}

static bool utDestroyStream(AudioSystem *audio, int32_t streamIndex) {
    UtAudioSystem *ut = (UtAudioSystem *) audio;
    UtStream *stream = streamFor(ut, streamIndex);
    if (stream == NULL) return false;
    utStopSound(audio, streamIndex);
    stream->active = false;
    return true;
}

static AudioSystemVtable utVtable = {
    .init = utInit,
    .destroy = utDestroy,
    .update = utUpdate,
    .playSound = utPlaySound,
    .stopSound = utStopSound,
    .stopAll = utStopAll,
    .isPlaying = utIsPlaying,
    .pauseSound = utPauseSound,
    .resumeSound = utResumeSound,
    .pauseAll = utPauseAll,
    .resumeAll = utResumeAll,
    .suspend = utPauseAll,
    .resume = utResumeAll,
    .setSoundGain = utSetSoundGain,
    .getSoundGain = utGetSoundGain,
    .setSoundPitch = utSetSoundPitch,
    .getSoundPitch = utGetSoundPitch,
    .getTrackPosition = utGetTrackPosition,
    .setTrackPosition = utSetTrackPosition,
    .getSoundLength = utGetSoundLength,
    .setMasterGain = utSetMasterGain,
    .setMasterGainForListener = utSetMasterGainForListener,
    .setChannelCount = utSetChannelCount,
    .groupLoad = utGroupLoad,
    .groupIsLoaded = utGroupIsLoaded,
    .createStream = utCreateStream,
    .destroyStream = utDestroyStream,
};

AudioSystem *platformCreateAudioSystem(void) {
    UtAudioSystem *ut = safeCalloc(1, sizeof(UtAudioSystem));
    ut->base.vtable = &utVtable;
    g_audio = ut;
    return &ut->base;
}
