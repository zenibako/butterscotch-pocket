/*
 * openfpgaOS audio backend for Butterscotch.
 *
 * Every sound the game plays is converted offline by mkmusic into music.bin
 * (mono IMA ADPCM, see of_music_pack.h): the external .ogg files it streams
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
#include "gettime.h"
#include "of_hooks.h"
#include "of_perf.h"
#include "log.h"
#include "stb_ds.h"
#include "utils.h"

#include "of_music_pack.h"
#include "of_snapshot_parts.h"
#include "of_snapshot_bytes.h"
#include "of_state_requests.h"
#include <math.h>

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OF_MUSIC_PATH "music.bin"

/* Id ranges shared with Butterscotch's other backends. */
#define OF_INSTANCE_ID_BASE 100000
#define OF_STREAM_INDEX_BASE 300000

#define OF_MAX_STREAMS 32
#define OF_MAX_VOICES 16
/* Voices that can stream at once; each has a read-ahead buffer. */
#define OF_MAX_STREAMED 4

/* Tracks up to this many compressed bytes (6 s) are played from memory. */
#define OF_MEMORY_SOUND_BYTES (96u * 1024u)
/* Memory kept for cached short sounds before the least recently played go. */
#define OF_SOUND_CACHE_BYTES (3u * 1024u * 1024u)

/* Compressed bytes buffered per voice: 4 s at 32 kHz. Blocking file reads
 * elsewhere (room and texture loads) are bridged from this buffer. */
#define OF_READAHEAD 65536
/* The read-ahead is topped up in pieces this size as soon as one fits, so
 * no single read holds a frame up for long. Reads end on a 512-byte boundary
 * of the pack so that the next one starts on a whole sector. */
#define OF_READ_PIECE 16384
#define OF_READ_ALIGN 512
/* With direct reads (see readDirect) a stream that is not running low is
 * topped up in pieces this small instead: about 6 ms each, measured, where
 * a 16 KB piece through fread costs about 28 ms and shows as a stutter. */
#define OF_READ_STEADY 4096
/* A direct read that has not finished after this long is given up, and
 * direct reads are not used again. In practice this is the Pocket saving a
 * screenshot: a direct read issued while the host is busy with that never
 * completes (nothing after 10 s, measured). The limit has to be longer than
 * the 2 s after which the OS itself drops a stalled read: until the OS has
 * dropped it, every ordinary read fails with "busy", and a build that gave up
 * after 1.5 s and carried on crashed within seconds of a screenshot. */
#define OF_DIRECT_TIMEOUT_NANOS 3000000000ull
#define OF_DIRECT_SETTLE_NANOS 3000000000ull

/* Output queued ahead of the DAC. A sound is heard this long after it is
 * started, so it is a trade against dropouts when a frame runs long. */
#define OF_QUEUE_TARGET_PAIRS (OF_AUDIO_RATE / 10)
#define OF_QUEUE_ROOM_CHANGE_PAIRS (OF_AUDIO_RATE / 4)
#define OF_MIX_CHUNK_PAIRS 256
#define OF_GAIN_ONE 4096

typedef struct {
    bool active;
    int32_t track;
    float gain;
    float pitch;
} OfStream;

typedef struct {
    bool active;
    bool paused;
    bool loop;
    bool finished; /* a non-looping track has played its last sample */
    int32_t instanceId;
    int32_t sourceIndex; /* SOND index or stream index this voice was started from */
    int32_t track;
    bool music; /* a stream or an external file, not an embedded effect; see Muting */

    /* Compressed data being decoded: a stream slot's read-ahead buffer, or a
     * whole cached sound. */
    int32_t streamSlot; /* index into streamBuffers, or -1 when played from memory */
    uint8_t *data;
    uint32_t bufferLen;
    uint32_t bufferPos;
    uint32_t fileBytePos; /* streamed: next byte of the track to read into the buffer */

    /* ADPCM decode. */
    OfAdpcmState adpcm;
    uint32_t samplesDecoded;
    bool startedThisFrame; /* no read-ahead yet; see refillStreams */
    uint32_t pendingCode;
    bool havePendingCode;

    /* Linear resampling from the track rate to the output rate, 16.16. */
    int32_t sample0, sample1;
    uint32_t frac;
    uint32_t step;
    float pitch;

    /* Gain ramp, in OF_GAIN_ONE units scaled by 256 for sub-step precision. */
    int32_t gainNow;
    int32_t gainTarget;
    int32_t gainStepPerPair;
} OfVoice;

/* A voice's ring on the hardware mixer; see Hardware mixer. */
typedef struct {
    bool running;
    bool held;   /* paused: rate and volume at 0 */
    bool padded; /* the track has ended and silence follows it */
    bool haveCarry;
    int16_t carry; /* a decoded sample waiting for its pair */
    int32_t instanceId;
    int voice;
    of_mixer_handle_t handle;
    uint32_t wptr;    /* next ring sample to write */
    uint32_t endWptr; /* where the silence starts, when padded */
    uint32_t rate;
    int volume;
} OfHwVoice;

typedef struct {
    AudioSystem base;
    FILE *file;
    OfMusicTrack *tracks;
    uint32_t trackCount;
    OfStream streams[OF_MAX_STREAMS];
    OfVoice voices[OF_MAX_VOICES];
    uint8_t streamBuffers[OF_MAX_STREAMED][OF_READAHEAD];
    /* Short sounds held in memory, indexed by track; NULL when not loaded. */
    uint8_t **cachedSounds;
    uint32_t *cachedLastUse;
    /* Derived from cached bytes, never serialized. NULL disables the fast path. */
    uint32_t *cachedSilentFrom;
    uint32_t cachedBytes;
    uint32_t useCounter;
    int32_t nextInstanceId;
    int16_t *snapshotPcm; /* shadow of accepted output for exact queued-audio restore */
    uint64_t snapshotWritten;
    int32_t queueCapacity;
    int32_t snapshotReservedPairs;
    float masterGain;
    bool allPaused;
    FILE *dump; /* desktop only: raw copy of everything written to the output */
    /* Direct reads of the pack; directStage is NULL when they are not in use. */
    uint8_t *directStage;
    uint32_t directSlot;
    /* The hardware mixer, when it plays the voices instead of mixAndWrite. */
    bool hwMixer;
    OfHwVoice hw[OF_MAX_VOICES];
    int16_t *hwRings[OF_MAX_VOICES];
    uint64_t hwLastPumpNanos;
    /* From a Memory: what each voice's ring held but had not played, put
     * back in front of its decoder when it starts again. */
    int16_t *hwRestore[OF_MAX_VOICES];
    uint32_t hwRestoreCount[OF_MAX_VOICES];
    bool hwRestoreCarry[OF_MAX_VOICES];
    int16_t hwRestoreCarryValue[OF_MAX_VOICES];
    /* A Memory made with the software mix restores its queued output through
     * the OS; voices wait until that has played. */
    uint64_t hwStartAfterNanos;
} OfAudioSystem;

/* The file idle hook has no user pointer, so the one instance is global. */
static OfAudioSystem *g_audio = NULL;
static bool g_snapshotLocked = false;
static OfAudioMode g_audioMode = OF_AUDIO_NORMAL; /* see Muting, below */
static bool voiceSkipped(const OfAudioSystem *ut, const OfVoice *voice);
static bool silent(const OfAudioSystem *ut);

/* ===[ Pack lookup ]=== */

#ifndef OF_PC
static void openDirect(OfAudioSystem *ut);
#endif

static void openPack(OfAudioSystem *ut) {
    FILE *file = fopen(OF_MUSIC_PATH, "rb");
    if (file == NULL) {
        logInfo("Audio: no %s, sound is disabled.\n", OF_MUSIC_PATH);
        return;
    }

    char magic[4];
    uint32_t count = 0;
    if (fread(magic, 1, 4, file) != 4 || memcmp(magic, OF_MUSIC_MAGIC, 4) != 0 ||
        fread(&count, sizeof(count), 1, file) != 1 || count == 0 || count > 4096) {
        logWarn("Audio: %s is not an audio pack, sound is disabled.\n", OF_MUSIC_PATH);
        fclose(file);
        return;
    }

    OfMusicTrack *tracks = malloc(count * sizeof(OfMusicTrack));
    if (tracks == NULL || fread(tracks, sizeof(OfMusicTrack), count, file) != count) {
        logWarn("Audio: %s index is truncated, sound is disabled.\n", OF_MUSIC_PATH);
        free(tracks);
        fclose(file);
        return;
    }

    /* Reads are explicit, buffer-sized chunks; stdio buffering would only add a copy. */
    setvbuf(file, NULL, _IONBF, 0);
    ut->file = file;
#ifndef OF_PC
    openDirect(ut);
#endif
    ut->tracks = tracks;
    ut->trackCount = count;
    ut->cachedSounds = safeCalloc(count, sizeof(uint8_t *));
    ut->cachedLastUse = safeCalloc(count, sizeof(uint32_t));
    ut->cachedSilentFrom = malloc((size_t)count * sizeof(uint32_t));
    if (ut->cachedSilentFrom)
        for (uint32_t i = 0; i < count; ++i) ut->cachedSilentFrom[i] = UINT32_MAX;
    logInfo("Audio: pack with %u tracks.\n", (unsigned) count);
}

/* Maps "music/mus_story.ogg" or "mus_story.ogg" to a track index, or -1. */
static int32_t findTrack(const OfAudioSystem *ut, const char *filename) {
    if (filename == NULL || ut->trackCount == 0) return -1;

    const char *base = filename;
    for (const char *p = filename; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    char name[OF_MUSIC_NAME_LEN] = {0};
    size_t len = 0;
    while (base[len] != '\0' && base[len] != '.' && len < OF_MUSIC_NAME_LEN - 1) {
        name[len] = (char) tolower((unsigned char) base[len]);
        len++;
    }

    int32_t lo = 0, hi = (int32_t) ut->trackCount - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        int cmp = strncmp(name, ut->tracks[mid].name, OF_MUSIC_NAME_LEN);
        if (cmp == 0) return mid;
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

/* ===[ Voice streaming and decode ]=== */

static uint32_t trackBytes(const OfMusicTrack *track) {
    return track->sampleCount / 2;
}

static void updateStep(const OfAudioSystem *ut, OfVoice *voice) {
    float rate = (float) ut->tracks[voice->track].sampleRate * voice->pitch;
    float step = rate * 65536.0f / (float) OF_AUDIO_RATE;
    if (step < 1.0f) step = 1.0f;
    if (step > 8.0f * 65536.0f) step = 8.0f * 65536.0f;
    voice->step = (uint32_t) step;
}

/* ===[ Reading the pack ]===
 *
 * fread on openfpgaOS is served from the OS's file cache, which fetches a
 * whole 32 KB block from the Pocket's host for any miss: about 28 ms however
 * little was asked for. of_file_read_async fetches exactly what is asked,
 * into staging memory the OS provides, at about 3.5 ms plus 0.6 ms per KB.
 *
 * The read is started and then waited for here, so it is not asynchronous
 * in effect. It must not be: while one is in flight the OS fails every
 * ordinary read with "busy" instead of queueing it, and Butterscotch reads
 * files from many places. Finishing before returning means the two can
 * never overlap. */
#ifndef OF_PC
static volatile int g_directDone;
static volatile int g_directResult;

/* Runs from the read-completion interrupt. */
static void directCallback(int token, int result) {
    (void) token;
    g_directResult = result;
    g_directDone = 1;
}

/* Reads `want` bytes at `filePos` of the pack into ut->directStage. */
static bool readDirect(OfAudioSystem *ut, uint32_t filePos, uint32_t want) {
    g_directDone = 0;
    g_directResult = 0;
    uint64_t start = nowNanos();
    /* Refused while the host is busy with something else; that passes quickly. */
    while (of_file_read_async((int) ut->directSlot, filePos, ut->directStage, want, directCallback) < 0) {
        if (nowNanos() - start > 50000000ull) return false;
    }
    /* The poll is the OS's fallback for a lost interrupt. */
    bool sawStateRequest = false;
    while (!g_directDone && of_file_async_poll() != 1) {
        if (!sawStateRequest && ofSavestateDeferFileReads()) {
            sawStateRequest = true;
            logInfo("State request seen during music read, %llu ms after submission\n",
                (unsigned long long)((nowNanos() - start) / 1000000));
        }
        if (nowNanos() - start > OF_DIRECT_TIMEOUT_NANOS) {
            /* Do not go back to ordinary reads while the OS still counts
             * this one as in flight. */
            uint64_t gaveUp = nowNanos();
            while (of_file_async_busy() == 1 && nowNanos() - gaveUp < OF_DIRECT_SETTLE_NANOS) of_file_async_poll();
#ifdef OF_NATIVE_SAVESTATES
            logWarn("Audio: direct read timed out; deferring streamed refill.\n");
#else
            logWarn("Audio: a direct read timed out; using fread from here on.\n");
            ut->directStage = NULL;
#endif
            return false;
        }
    }
    return g_directDone && g_directResult >= 0;
}

static void openDirect(OfAudioSystem *ut) {
    uint32_t slot = 0;
    if (of_file_async_max_read() < OF_READ_PIECE || of_file_slot_find(OF_MUSIC_PATH, &slot) != 0) return;
    ut->directStage = of_file_dma_stage_alloc(OF_READ_PIECE, 4096);
    ut->directSlot = slot;
    if (ut->directStage != NULL) logInfo("Audio: music is read directly, %u KB at a time.\n", OF_READ_STEADY / 1024u);
}
#endif

/* Reads `want` bytes at `filePos` of the pack into `dest`; returns the count read. */
static size_t readPack(OfAudioSystem *ut, uint32_t filePos, uint8_t *dest, uint32_t want, bool optional) {
#ifndef OF_PC
    if (ut->directStage != NULL) {
        if (readDirect(ut, filePos, want)) {
            memcpy(dest, ut->directStage, want);
            return want;
        }
#ifdef OF_NATIVE_SAVESTATES
        /* Watchdog completion does not cancel the host transfer. Never
         * replace a failed optional async read with another host command. */
        if (optional) return 0;
#endif
    }
#endif
    if (optional && ofSavestateDeferFileReads()) return 0;
    if (fseek(ut->file, (long) filePos, SEEK_SET) != 0) return 0;
    /* Read into static memory and copy: on openfpgaOS a read straight
     * into the heap (where the voices live) is about ten times slower. */
    static uint8_t scratch[OF_READ_PIECE] __attribute__((aligned(512)));
    size_t got = fread(scratch, 1, want, ut->file);
    memcpy(dest, scratch, got);
    return got;
}

/* Reads a file stored in the pack (see OF_MUSIC_RAW_FILE) by its base name. The caller frees *outData, which
 * is NUL terminated past its size so that text can be used directly. Main loop only: this blocks. */
static const OfMusicTrack *findPackFile(const char *path) {
    OfAudioSystem *ut = g_audio;
    if (ut == NULL || ut->file == NULL || path == NULL) return NULL;

    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    char name[OF_MUSIC_NAME_LEN] = {0};
    size_t len = strlen(base);
    if (len == 0 || len >= OF_MUSIC_NAME_LEN) return NULL;
    for (size_t i = 0; i < len; i++) name[i] = (char) tolower((unsigned char) base[i]);

    for (uint32_t i = 0; i < ut->trackCount; i++) {
        if (ut->tracks[i].sampleRate == OF_MUSIC_RAW_FILE && strncmp(name, ut->tracks[i].name, OF_MUSIC_NAME_LEN) == 0)
            return &ut->tracks[i];
    }
    return NULL;
}

bool ofAudioHasPackFile(const char *path) {
    return findPackFile(path) != NULL;
}

bool ofAudioReadPackFile(const char *path, uint8_t **outData, uint32_t *outSize) {
    OfAudioSystem *ut = g_audio;
    const OfMusicTrack *track = findPackFile(path);
    if (track == NULL) return false;

    uint32_t size = track->sampleCount;
    uint8_t *data = malloc((size_t) size + 1);
    if (data == NULL) return false;
    for (uint32_t done = 0; done < size;) {
        uint32_t want = size - done < OF_READ_PIECE ? size - done : OF_READ_PIECE;
        if (readPack(ut, track->offset + done, data + done, want, false) != want) {
            free(data);
            return false;
        }
        done += want;
    }
    data[size] = '\0';
    *outData = data;
    *outSize = size;
    return true;
}

/* Tops up a voice's read-ahead from the pack. Main loop only: this blocks. */
static void refillVoice(OfAudioSystem *ut, OfVoice *voice) {
    if (voice->streamSlot < 0) return;
    /* The host may stop serving files while its menu/state command is busy.
     * Keep the buffered stream intact and reach the snapshot boundary first. */
    if (ofSavestateDeferFileReads()) return;
    uint32_t piece = OF_READ_PIECE;
    if (ut->directStage != NULL && voice->bufferLen - voice->bufferPos >= OF_READ_PIECE / 2) piece = OF_READ_STEADY;
    if (voice->bufferLen - voice->bufferPos > OF_READAHEAD - piece) return;

    const OfMusicTrack *track = &ut->tracks[voice->track];
    uint32_t total = trackBytes(track);
    if (total == 0) return;

    uint32_t remaining = voice->bufferLen - voice->bufferPos;
    memmove(voice->data, voice->data + voice->bufferPos, remaining);
    voice->bufferPos = 0;
    voice->bufferLen = remaining;

    /* One piece per call; see refillStreams for how calls are rationed. */
    int pieces = 1;

    while (voice->bufferLen < OF_READAHEAD && pieces-- > 0) {
        if (voice->fileBytePos >= total) {
            if (!voice->loop) break;
            voice->fileBytePos = 0;
        }
        uint32_t want = OF_READAHEAD - voice->bufferLen;
        if (want > piece) want = piece;
        uint32_t filePos = track->offset + voice->fileBytePos;
        if (want >= total - voice->fileBytePos) {
            want = total - voice->fileBytePos;
        } else {
            uint32_t past = (filePos + want) % OF_READ_ALIGN;
            if (past >= want) break; /* less than a sector of room left */
            want -= past;
        }

        /* Anything that mixes from this voice meanwhile only touches bytes
         * below bufferLen, which is not advanced until after the read. */
        uint64_t readStart = nowNanos();
        size_t got = readPack(ut, filePos, voice->data + voice->bufferLen, want, true);
        ofPerfAddLoad(OF_LOAD_MUSIC, nowNanos() - readStart);
        if (got == 0) break;
        voice->bufferLen += (uint32_t) got;
        voice->fileBytePos += (uint32_t) got;
    }
}

/* Produces the next decoded sample, or false if none is available right now
 * (end of a non-looping track, or the read-ahead ran dry). */
static bool nextSample(const OfAudioSystem *ut, OfVoice *voice, int32_t *out) {
    const OfMusicTrack *track = &ut->tracks[voice->track];

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

    *out = ofAdpcmDecode(&voice->adpcm, code);
    voice->samplesDecoded++;
    return true;
}

/* What a 4-bit code adds to or takes from the predictor at each step size:
 * ofAdpcmDecode's sum of step fractions, worked out once. Looked up it is one
 * load where the decoder had three branches on bits of the sound itself,
 * which the CPU cannot predict. */
static uint16_t g_adpcmDiff[89 * 8];

static void adpcmDiffInit(void) {
    for (int index = 0; index < 89; index++) {
        int32_t step = ofAdpcmStepTable[index];
        for (int code = 0; code < 8; code++) {
            int32_t diff = step >> 3;
            if (code & 4) diff += step;
            if (code & 2) diff += step >> 1;
            if (code & 1) diff += step >> 2;
            g_adpcmDiff[index * 8 + code] = (uint16_t) diff;
        }
    }
}

/* Adds `pairs` output samples of one voice into the mono mix buffer.
 * The decoder, resampler and gain state are kept in locals for as long as
 * samples come straight out of the buffer, which is nearly always; the end
 * of the buffer or of the track goes through nextSample, with the voice
 * brought up to date before and read back after. */
static int32_t gainToFixed(float gain);

static void mixVoiceScalar(const OfAudioSystem *ut, OfVoice *voice, int32_t *mix, int pairs) {
    int32_t sample0 = voice->sample0, sample1 = voice->sample1;
    uint32_t frac = voice->frac;
    const uint32_t step = voice->step;
    int32_t gainNow = voice->gainNow;
    const int32_t gainTarget = voice->gainTarget, gainStep = voice->gainStepPerPair;
    const int32_t groupGain = gainToFixed(AudioSystem_soundGroupGain(&ut->base, voice->sourceIndex)) >> 8;

    const uint32_t sampleCount = ut->tracks[voice->track].sampleCount;
    const uint8_t *const data = voice->data;
    const uint32_t bufferLen = voice->bufferLen;
    uint32_t bufferPos = voice->bufferPos, samplesDecoded = voice->samplesDecoded, pendingCode = voice->pendingCode;
    bool havePendingCode = voice->havePendingCode;
    int32_t predictor = voice->adpcm.predictor, stepIndex = voice->adpcm.stepIndex;
#define OF_VOICE_STORE() do { \
        voice->bufferPos = bufferPos; voice->samplesDecoded = samplesDecoded; voice->pendingCode = pendingCode; \
        voice->havePendingCode = havePendingCode; voice->adpcm.predictor = predictor; voice->adpcm.stepIndex = stepIndex; \
    } while (0)

    for (int i = 0; i < pairs; i++) {
        while (frac >= 65536) {
            int32_t next;
            if (samplesDecoded < sampleCount && (havePendingCode || bufferPos < bufferLen)) {
                uint32_t code;
                if (havePendingCode) {
                    code = pendingCode;
                    havePendingCode = false;
                } else {
                    uint8_t byte = data[bufferPos++];
                    code = byte & 0x0F;
                    pendingCode = byte >> 4;
                    havePendingCode = true;
                }
                int32_t diff = g_adpcmDiff[stepIndex * 8 + (int32_t) (code & 7)];
                int32_t sign = -(int32_t) (code >> 3);
                predictor += (diff ^ sign) - sign;
                if (predictor > 32767) predictor = 32767;
                if (predictor < -32768) predictor = -32768;
                stepIndex += ofAdpcmIndexTable[code];
                if (stepIndex < 0) stepIndex = 0;
                if (stepIndex > 88) stepIndex = 88;
                samplesDecoded++;
                next = predictor;
            } else {
                OF_VOICE_STORE();
                bool got = nextSample(ut, voice, &next);
                bufferPos = voice->bufferPos; samplesDecoded = voice->samplesDecoded; pendingCode = voice->pendingCode;
                havePendingCode = voice->havePendingCode; predictor = voice->adpcm.predictor; stepIndex = voice->adpcm.stepIndex;
                if (!got) {
                    if (!voice->finished) goto out; /* starved: resume here once refilled */
                    next = 0;
                    if (sample1 == 0) {
                        voice->active = false;
                        goto out;
                    }
                }
            }
            sample0 = sample1;
            sample1 = next;
            frac -= 65536;
        }

        int32_t sample = sample0 + (((sample1 - sample0) * (int32_t) (frac >> 4)) >> 12);
        frac += step;

        if (gainNow != gainTarget) {
            int32_t delta = gainTarget - gainNow;
            if (delta > gainStep) delta = gainStep;
            if (delta < -gainStep) delta = -gainStep;
            gainNow += delta;
        }
        sample = (sample * (gainNow >> 8)) / OF_GAIN_ONE;
        if (groupGain != OF_GAIN_ONE) sample = (sample * groupGain) / OF_GAIN_ONE;
        mix[i] += sample;
    }
out:
    OF_VOICE_STORE();
#undef OF_VOICE_STORE
    voice->sample0 = sample0;
    voice->sample1 = sample1;
    voice->frac = frac;
    voice->gainNow = gainNow;
}

/* Codes 0 and 8 preserve predictor/index zero. The interpolation endpoints
 * must also be zero before that content proof permits skipping any output. */
static uint32_t silentSuffix(const uint8_t *data, uint32_t bytes) {
    while (bytes > 0 && (data[bytes - 1] & 0x77) == 0) --bytes;
    return bytes;
}

static bool advanceSilentVoice(const OfAudioSystem *ut, OfVoice *voice, int pairs) {
    if (pairs <= 0 || pairs > OF_MIX_CHUNK_PAIRS || voice->streamSlot >= 0 || voice->loop ||
        voice->sample0 != 0 || voice->sample1 != 0 || voice->adpcm.predictor != 0 ||
        voice->adpcm.stepIndex != 0 || voice->step == 0 || voice->step > 8u * 65536u ||
        voice->frac >= 9u * 65536u || voice->gainStepPerPair < 0 ||
        voice->track < 0 || (uint32_t)voice->track >= ut->trackCount ||
        !ut->cachedSilentFrom || !ut->cachedSounds || !voice->data ||
        voice->data != ut->cachedSounds[voice->track]) return false;

    const OfMusicTrack *track = &ut->tracks[voice->track];
    uint32_t from = ut->cachedSilentFrom[voice->track];
    if (from == UINT32_MAX || voice->bufferLen != trackBytes(track) ||
        voice->bufferPos > voice->bufferLen || voice->bufferPos < from ||
        voice->samplesDecoded > track->sampleCount ||
        (voice->havePendingCode && (voice->bufferPos <= from ||
            voice->pendingCode > 15 || (voice->pendingCode & 7) != 0))) return false;

    /* The bounded chunk/step/phase keep these products in uint32. Decode is
     * before each output, so the last output's phase increment is deferred. */
    uint32_t consumed = (voice->frac + (uint32_t)(pairs - 1) * voice->step) >> 16;
    uint32_t remaining = track->sampleCount - voice->samplesDecoded;
    uint64_t available = (uint64_t)(voice->bufferLen - voice->bufferPos) * 2 + voice->havePendingCode;
    if (consumed >= remaining || consumed > available) return false;

    uint32_t codes = consumed;
    if (voice->havePendingCode && codes > 0) {
        --codes;
        voice->havePendingCode = false;
    }
    if (codes > 0) {
        voice->bufferPos += (codes + 1) / 2;
        voice->pendingCode = voice->data[voice->bufferPos - 1] >> 4;
        voice->havePendingCode = (codes & 1) != 0;
    }
    voice->samplesDecoded += consumed;
    voice->frac += (uint32_t)pairs * voice->step - (consumed << 16);

    int64_t delta = (int64_t)voice->gainTarget - voice->gainNow;
    int64_t advance = (int64_t)voice->gainStepPerPair * pairs;
    if (delta > 0) voice->gainNow += (int32_t)(advance < delta ? advance : delta);
    else if (delta < 0) voice->gainNow -= (int32_t)(advance < -delta ? advance : -delta);
    return true;
}

/* Return optimized voice-output pairs for diagnostics, without extra clocks. */
static unsigned mixVoice(const OfAudioSystem *ut, OfVoice *voice, int32_t *mix, int pairs) {
    if (advanceSilentVoice(ut, voice, pairs)) return (unsigned)pairs;
    mixVoiceScalar(ut, voice, mix, pairs);
    return 0;
}

/* Preconditions: snapshotPcm exists, queueCapacity > 0, pairs >= 0. */
static void rememberAcceptedPcm(OfAudioSystem *ut, const int16_t *pcm, int pairs) {
    if (ut->snapshotWritten > UINT64_MAX - (uint64_t)pairs) {
        /* Preserve scalar indexing across uint64 wrap for arbitrary capacities. */
        for (int i = 0; i < pairs; ++i) {
            size_t at = (size_t)(ut->snapshotWritten++ %
                                (uint64_t)ut->queueCapacity) * 2;
            ut->snapshotPcm[at] = pcm[i * 2];
            ut->snapshotPcm[at + 1] = pcm[i * 2 + 1];
        }
        return;
    }
    size_t capacity = (size_t)ut->queueCapacity;
    size_t at = (size_t)(ut->snapshotWritten % (uint64_t)capacity);
    while (pairs > 0) {
        size_t count = capacity - at;
        if (count > (size_t)pairs) count = (size_t)pairs;
        memcpy(ut->snapshotPcm + at * 2, pcm, count * 2 * sizeof(*pcm));
        ut->snapshotWritten += (uint64_t)count;
        pcm += count * 2;
        pairs -= (int)count;
        at = 0;
    }
}

static void mixAndWrite(OfAudioSystem *ut, int pairs) {
    static int32_t mix[OF_MIX_CHUNK_PAIRS];
    static int16_t out[OF_MIX_CHUNK_PAIRS * 2];
    int32_t master = (int32_t) (ut->masterGain * (float) OF_GAIN_ONE);
    const bool timed = ofPerfDetail; /* a clock read costs more than mixing a few samples */
    uint64_t mixStart = timed ? nowNanos() : 0;
    uint64_t voicesNanos = 0, writeNanos = 0, voicePairs = 0, silentPairs = 0;
    int allPairs = pairs;

    while (pairs > 0) {
        int chunk = pairs < OF_MIX_CHUNK_PAIRS ? pairs : OF_MIX_CHUNK_PAIRS;
        memset(mix, 0, (size_t) chunk * sizeof(int32_t));

        if (!ut->allPaused) {
            uint64_t voicesStart = timed ? nowNanos() : 0;
            for (int v = 0; v < OF_MAX_VOICES; v++) {
                OfVoice *voice = &ut->voices[v];
                if (voice->active && !voice->paused && !voiceSkipped(ut, voice)) {
                    silentPairs += mixVoice(ut, voice, mix, chunk);
                    voicePairs += (uint64_t) chunk;
                }
            }
            if (timed) voicesNanos += nowNanos() - voicesStart;
        }

        for (int i = 0; i < chunk; i++) {
            /* Clamp before applying the master gain so the product fits in 32 bits. */
            int32_t sample = mix[i];
            if (sample > 65535) sample = 65535;
            if (sample < -65535) sample = -65535;
            if (master != OF_GAIN_ONE) sample = (sample * master) / OF_GAIN_ONE;
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            out[i * 2] = (int16_t) sample;
            out[i * 2 + 1] = (int16_t) sample;
        }

        if (ut->dump != NULL) fwrite(out, sizeof(int16_t) * 2, (size_t) chunk, ut->dump);
        uint64_t writeStart = timed ? nowNanos() : 0;
        int accepted = of_audio_write(out, chunk);
        if (ut->snapshotPcm && accepted > 0 && accepted <= chunk) {
            rememberAcceptedPcm(ut, out, accepted);
        }
        if (timed) writeNanos += nowNanos() - writeStart;
        pairs -= chunk;
    }
    if (timed) ofPerfAddLoad(OF_LOAD_MIX, nowNanos() - mixStart);
    ofPerfAddLoad(OF_LOAD_MIX_VOICES, voicesNanos);
    ofPerfAddLoad(OF_LOAD_MIX_WRITE, writeNanos);
    ofPerfAddLoad(OF_LOAD_MIX_PAIRS, (uint64_t) allPairs);
    ofPerfAddLoad(OF_LOAD_MIX_VOICE_PAIRS, voicePairs);
    ofPerfAddLoad(OF_LOAD_MIX_SILENT_PAIRS, silentPairs);
}

static void hwPump(OfAudioSystem *ut, int target);

/* Keeps the output queue topped up to the target. Does no file I/O, so it is
 * safe to call from the file idle hook. */
static void pumpOutputTo(OfAudioSystem *ut, int target) {
    if (ut->hwMixer) {
        hwPump(ut, target);
        if (ut->hwMixer) return; /* else it gave up, and the software mix takes over */
    }
    /* Some targets have a ring smaller than the target; never aim past 3/4 of it. */
    if (target > ut->queueCapacity * 3 / 4) target = ut->queueCapacity * 3 / 4;

    int queued = ut->queueCapacity - of_audio_free();
    int want = target - queued;
    if (want > 0) mixAndWrite(ut, want);
}

static void pumpOutput(OfAudioSystem *ut) {
    pumpOutputTo(ut, OF_QUEUE_TARGET_PAIRS);
}

/* On the Pocket the OS does not call the file idle hook during an app's own
 * reads, and nothing covers long stretches of drawing, so the loaders and
 * the draw profiler also call this between pieces of work. Without it a frame longer than the
 * queue is deep (100 ms) leaves a gap in the sound. */
void platformBusyTick(void) {
    /* A dump follows game time instead of the queue; see updateAudio. */
    if (!g_snapshotLocked && g_audio != NULL && g_audio->file != NULL && g_audio->dump == NULL && !silent(g_audio)) pumpOutput(g_audio);
}

/* A room change is the one long stretch of work with nowhere to feed the
 * queue from: tens of ms of setting the room up, then its first draw. The
 * log hook calls this when the change starts, to queue enough to cover it.
 * Sounds that start in the next quarter second are heard that much late. */
void ofAudioRoomChange(void) {
    if (!g_snapshotLocked && g_audio != NULL && g_audio->file != NULL && g_audio->dump == NULL && !silent(g_audio)) pumpOutputTo(g_audio, OF_QUEUE_ROOM_CHANGE_PAIRS);
}

#ifndef OF_PC
static void idleHook(void) {
    if (!g_snapshotLocked && g_audio != NULL && g_audio->file != NULL && !silent(g_audio)) pumpOutput(g_audio);
}
#endif

/* ===[ Short-sound cache ]=== */

static bool soundInUse(const OfAudioSystem *ut, int32_t track) {
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        if (ut->voices[v].active && ut->voices[v].streamSlot < 0 && ut->voices[v].track == track) return true;
    }
    return false;
}

static void evictSounds(OfAudioSystem *ut, uint32_t needed) {
    while (ut->cachedBytes + needed > OF_SOUND_CACHE_BYTES) {
        int32_t victim = -1;
        for (uint32_t t = 0; t < ut->trackCount; t++) {
            if (ut->cachedSounds[t] == NULL || soundInUse(ut, (int32_t) t)) continue;
            if (victim < 0 || ut->cachedLastUse[t] < ut->cachedLastUse[victim]) victim = (int32_t) t;
        }
        if (victim < 0) return;
        free(ut->cachedSounds[victim]);
        ut->cachedSounds[victim] = NULL;
        if (ut->cachedSilentFrom) ut->cachedSilentFrom[victim] = UINT32_MAX;
        ut->cachedBytes -= trackBytes(&ut->tracks[victim]);
    }
}

/* Returns the whole compressed sound, loading it on first use. */
static uint8_t *cachedSound(OfAudioSystem *ut, int32_t track) {
    ut->cachedLastUse[track] = ++ut->useCounter;
    if (ut->cachedSounds[track] != NULL) return ut->cachedSounds[track];

    uint32_t bytes = trackBytes(&ut->tracks[track]);
    if (bytes == 0) return NULL;
    evictSounds(ut, bytes);

    uint8_t *data = malloc(bytes);
    if (data == NULL) return NULL;
    uint64_t start = nowNanos();
    if (fseek(ut->file, (long) ut->tracks[track].offset, SEEK_SET) != 0 || fread(data, 1, bytes, ut->file) != bytes) {
        free(data);
        return NULL;
    }
    ofPerfAddLoad(OF_LOAD_SOUND, nowNanos() - start);
    ut->cachedSounds[track] = data;
    if (ut->cachedSilentFrom) ut->cachedSilentFrom[track] = silentSuffix(data, bytes);
    ut->cachedBytes += bytes;
    return data;
}

/* ===[ Hardware mixer ]===
 *
 * os25 has a 32-voice mixer in the FPGA. It plays 16-bit PCM from SDRAM,
 * resamples it with linear interpolation, ramps and applies each voice's
 * volume, sums the voices and feeds the DAC, all without the CPU. It cannot
 * decode ADPCM, so the CPU still does that: each playing voice is decoded
 * into a ring of its own, a little ahead of where the mixer has got to, and
 * the mixer plays the ring. The resampling, gain and summing that cost most
 * of the software mix are gone, and so is the write of every output pair to
 * the OS.
 *
 * A ring runs in the mixer's stream mode: the mixer never plays past the
 * last sample written (the write pointer), fades out when it nears it and
 * holds there in silence until more arrives. A long frame therefore leaves a
 * gap, as with the software mix, and never replays old ring contents.
 *
 * openfpgaOS offers stream mode only to its own of_audio_* voice, so the
 * ring's voice is set up here through the mixer's registers once the OS has
 * allocated it (hwPortStart). Everything else goes through of_mixer_*.
 *
 * The decoder state in OfVoice is the ring's write side, ahead of what is
 * heard by up to the lead; hwPlayedSample works out what is heard. A Memory
 * keeps that decoder state and, beside it, what each ring holds but has not
 * played (see Snapshots), so a restore continues with the very next sample.
 * --sw-mixer selects the software mix. */

#define OF_HW_RING_SAMPLES 32768u /* per voice; a power of two */
#define OF_HW_RING_MASK (OF_HW_RING_SAMPLES - 1u)
/* The mixer fades a stream voice with less than 256 samples left. Silence
 * after a sound's end lets its last samples play at full volume. */
#define OF_HW_PAD_SAMPLES 512u
/* Never decode less ahead than this, for the same reason. */
#define OF_HW_MIN_LEAD 1024u
/* The mixer's stream-mode hold is sized for rates up to 2.0. */
#define OF_HW_MAX_RATE 0x20000u

static bool g_softwareMixer = false;

void ofAudioUseSoftwareMixer(bool software) { g_softwareMixer = software; }

#ifdef OF_HW_MIXER_FAKE
/* Tests supply a mixer. */
bool hwPortAvailable(void);
bool hwPortStart(OfHwVoice *hw, int16_t *ring, uint32_t samples, uint32_t wptr, uint32_t rate);
void hwPortSetWptr(const OfHwVoice *hw, uint32_t wptr);
int32_t hwPortPosition(const OfHwVoice *hw);
void hwPortSetRate(const OfHwVoice *hw, uint32_t rate);
void hwPortSetVolume(const OfHwVoice *hw, int volume);
void hwPortStop(const OfHwVoice *hw);
static volatile uint32_t *hwPortRingWords(int16_t *ring) { return (volatile uint32_t *) ring; }
#elif !defined(OF_PC)
/* A voice's registers (openfpgaOS hal/regs.h, MIX_VOICE_FIELD). */
#define OF_HW_MIX_FIELD(v, f) (*(volatile uint32_t *) (uintptr_t) (0x48000000u + ((uint32_t) (v) << 6) + ((uint32_t) (f) << 2)))
#define OF_HW_MIX_RATE 2
#define OF_HW_MIX_CTRL 3       /* [0] active [1] stereo [2] loop [3] stream */
#define OF_HW_MIX_LOOP_END 7
#define OF_HW_MIX_LOOP_START 8
#define OF_HW_MIX_WPTR 11

static bool hwPortAvailable(void) { return of_has_feature(OF_HW_MIXER_HW) != 0; }

/* The mixer reads SDRAM behind the data cache, so the ring is written
 * through the uncached alias: each store has reached SDRAM before the next
 * instruction, as the OS's own stream voice does it (targets/pocket/audio.c). */
static volatile uint32_t *hwPortRingWords(int16_t *ring) { return (volatile uint32_t *) of_uncached(ring); }

static bool hwPortStart(OfHwVoice *hw, int16_t *ring, uint32_t samples, uint32_t wptr, uint32_t rate) {
    /* Started silent at 48 kHz from sample 0, which is already written, so
     * the moment before stream mode is set below plays nothing wrong. Passing
     * the uncached alias spares the OS a cache flush. */
    of_mixer_handle_t handle = of_mixer_play_h((const uint8_t *) of_uncached(ring), samples, OF_AUDIO_RATE, 1, 0);
    if (handle == OF_MIXER_HANDLE_INVALID) return false;
    int voice = of_mixer_handle_voice(handle);
    if (voice < 0) {
        of_mixer_stop_h(handle);
        return false;
    }
    /* Stream mode requires the loop to be the whole ring. */
    OF_HW_MIX_FIELD(voice, OF_HW_MIX_WPTR) = wptr;
    OF_HW_MIX_FIELD(voice, OF_HW_MIX_LOOP_START) = 0;
    OF_HW_MIX_FIELD(voice, OF_HW_MIX_LOOP_END) = samples;
    OF_HW_MIX_FIELD(voice, OF_HW_MIX_RATE) = rate;
    OF_HW_MIX_FIELD(voice, OF_HW_MIX_CTRL) = 1u | 4u | 8u;
    hw->handle = handle;
    hw->voice = voice;
    return true;
}

static void hwPortSetWptr(const OfHwVoice *hw, uint32_t wptr) { OF_HW_MIX_FIELD(hw->voice, OF_HW_MIX_WPTR) = wptr; }
static int32_t hwPortPosition(const OfHwVoice *hw) { return of_mixer_get_position_h(hw->handle); }
static void hwPortSetRate(const OfHwVoice *hw, uint32_t rate) { of_mixer_set_rate_raw_h(hw->handle, rate); }
static void hwPortSetVolume(const OfHwVoice *hw, int volume) { of_mixer_set_vol_lr_h(hw->handle, volume, volume); }
static void hwPortStop(const OfHwVoice *hw) { of_mixer_stop_h(hw->handle); }
#else
static bool hwPortAvailable(void) { return false; }
static bool hwPortStart(OfHwVoice *hw, int16_t *ring, uint32_t samples, uint32_t wptr, uint32_t rate) {
    (void) hw; (void) ring; (void) samples; (void) wptr; (void) rate;
    return false;
}
static void hwPortSetWptr(const OfHwVoice *hw, uint32_t wptr) { (void) hw; (void) wptr; }
static int32_t hwPortPosition(const OfHwVoice *hw) { (void) hw; return -1; }
static void hwPortSetRate(const OfHwVoice *hw, uint32_t rate) { (void) hw; (void) rate; }
static void hwPortSetVolume(const OfHwVoice *hw, int volume) { (void) hw; (void) volume; }
static void hwPortStop(const OfHwVoice *hw) { (void) hw; }
static volatile uint32_t *hwPortRingWords(int16_t *ring) { return (volatile uint32_t *) ring; }
#endif

/* Chooses the backend, once, at start-up. */
static void hwMixerInit(OfAudioSystem *ut) {
    ut->hwMixer = false;
    if (g_softwareMixer || ut->file == NULL || !hwPortAvailable()) return;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        ut->hwRings[v] = aligned_alloc(64, OF_HW_RING_SAMPLES * sizeof(int16_t));
        if (ut->hwRings[v] == NULL) {
            for (int u = 0; u < v; u++) free(ut->hwRings[u]);
            memset(ut->hwRings, 0, sizeof(ut->hwRings));
            logWarn("Audio: no memory for the hardware mixer's rings, mixing in software.\n");
            return;
        }
    }
    ut->hwMixer = true;
}

static void hwDropRestore(OfAudioSystem *ut, int v) {
    free(ut->hwRestore[v]);
    ut->hwRestore[v] = NULL;
    ut->hwRestoreCount[v] = 0;
    ut->hwRestoreCarry[v] = false;
}

/* Also forgets the samples a Memory kept for the voice: they belong to the
 * sound and position it had before a stop, a new sound or a seek. */
static void hwStopVoice(OfAudioSystem *ut, int v) {
    OfHwVoice *hw = &ut->hw[v];
    if (hw->running) hwPortStop(hw);
    memset(hw, 0, sizeof(*hw));
    hwDropRestore(ut, v);
}

static uint32_t hwBacklog(const OfHwVoice *hw, int32_t position) {
    return (hw->wptr - (uint32_t) position) & OF_HW_RING_MASK;
}

/* The track sample being heard: the decoder's position less what is
 * written but not yet played. */
static uint32_t hwPlayedSample(const OfAudioSystem *ut, const OfVoice *voice, const OfHwVoice *hw) {
    uint32_t count = ut->tracks[voice->track].sampleCount;
    int32_t position = hwPortPosition(hw);
    if (position < 0) return voice->samplesDecoded;
    uint32_t backlog = hwBacklog(hw, position);
    if (hw->padded) {
        uint32_t pad = (hw->wptr - hw->endWptr) & OF_HW_RING_MASK;
        backlog = backlog > pad ? backlog - pad : 0;
    }
    if (hw->haveCarry) backlog++;
    int64_t played = (int64_t) voice->samplesDecoded - (int64_t) backlog;
    if (played < 0) played = voice->loop && count > 0 ? played + count : 0;
    if (played < 0) played = 0;
    return (uint32_t) played;
}

/* Puts the decoder at a sample of the track, as a seek does. */
static void seekVoice(const OfAudioSystem *ut, OfVoice *voice, uint32_t sample) {
    const OfMusicTrack *track = &ut->tracks[voice->track];
    sample &= ~1u;
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
}

static void hwPutWord(volatile uint32_t *ring, uint32_t wptr, int32_t s0, int32_t s1) {
    ring[(wptr & OF_HW_RING_MASK) >> 1] = (uint32_t) (uint16_t) s0 | (uint32_t) (uint16_t) s1 << 16;
}

/* Decodes up to `want` samples (even) into the ring. A track that has ended
 * gets its silence and is marked padded; a stream whose read-ahead has run
 * dry just stops early, and goes on from there next time. */
static void hwDecode(OfAudioSystem *ut, OfVoice *voice, OfHwVoice *hw, volatile uint32_t *ring, uint32_t want) {
    const uint32_t sampleCount = ut->tracks[voice->track].sampleCount;
    uint32_t wptr = hw->wptr;
    const uint32_t end = wptr + want;

    while (wptr < end) {
        /* Nearly always: two codes from one byte make one word of the ring. */
        if (!hw->haveCarry && !voice->havePendingCode) {
            const uint8_t *const data = voice->data;
            uint32_t bufferPos = voice->bufferPos;
            uint32_t bytes = voice->bufferLen - bufferPos;
            uint32_t left = (sampleCount - voice->samplesDecoded) / 2;
            uint32_t words = (end - wptr) / 2;
            if (bytes > left) bytes = left;
            if (bytes > words) bytes = words;
            int32_t predictor = voice->adpcm.predictor, stepIndex = voice->adpcm.stepIndex;
            for (uint32_t i = 0; i < bytes; i++) {
                uint8_t byte = data[bufferPos++];
                int32_t out[2];
                for (int half = 0; half < 2; half++) {
                    uint32_t code = half ? byte >> 4 : byte & 0x0F;
                    int32_t diff = g_adpcmDiff[stepIndex * 8 + (int32_t) (code & 7)];
                    int32_t sign = -(int32_t) (code >> 3);
                    predictor += (diff ^ sign) - sign;
                    if (predictor > 32767) predictor = 32767;
                    if (predictor < -32768) predictor = -32768;
                    stepIndex += ofAdpcmIndexTable[code];
                    if (stepIndex < 0) stepIndex = 0;
                    if (stepIndex > 88) stepIndex = 88;
                    out[half] = predictor;
                }
                hwPutWord(ring, wptr, out[0], out[1]);
                wptr += 2;
            }
            voice->bufferPos = bufferPos;
            voice->samplesDecoded += bytes * 2;
            voice->adpcm.predictor = predictor;
            voice->adpcm.stepIndex = stepIndex;
            if (wptr >= end) break;
        }

        /* The end of the buffer or the track, or an odd sample left over. */
        int32_t pair[2];
        int have = 0;
        if (hw->haveCarry) {
            pair[have++] = hw->carry;
            hw->haveCarry = false;
        }
        bool ended = false;
        while (have < 2) {
            int32_t sample;
            if (nextSample(ut, voice, &sample)) {
                pair[have++] = sample;
            } else {
                ended = voice->finished;
                break;
            }
        }
        if (have == 2) {
            hwPutWord(ring, wptr, pair[0], pair[1]);
            wptr += 2;
            continue;
        }
        if (!ended) {
            /* Starved: keep the odd sample for next time. */
            if (have == 1) {
                hw->carry = (int16_t) pair[0];
                hw->haveCarry = true;
            }
            break;
        }
        /* The track is over: its last sample, then silence. */
        hw->endWptr = wptr + (uint32_t) have;
        hwPutWord(ring, wptr, have ? pair[0] : 0, 0);
        wptr += 2;
        for (uint32_t i = 2; i < OF_HW_PAD_SAMPLES; i += 2, wptr += 2) hwPutWord(ring, wptr, 0, 0);
        hw->padded = true;
        break;
    }
    hw->wptr = wptr & OF_HW_RING_MASK;
}

static int hwVolume(const OfAudioSystem *ut, const OfVoice *voice) {
    int64_t gain = voice->gainNow >> 8; /* OF_GAIN_ONE units */
    int32_t group = gainToFixed(AudioSystem_soundGroupGain(&ut->base, voice->sourceIndex)) >> 8;
    int32_t master = (int32_t) (ut->masterGain * (float) OF_GAIN_ONE);
    gain = gain * group / OF_GAIN_ONE * master / OF_GAIN_ONE;
    int64_t volume = (gain * 255 + OF_GAIN_ONE / 2) / OF_GAIN_ONE;
    return volume > 255 ? 255 : (int) volume;
}

/* The software mix moves a gain ramp on with every output pair; here it
 * moves on by the time since the last pump. */
static void hwAdvanceGain(OfVoice *voice, uint32_t pairs) {
    if (voice->gainNow == voice->gainTarget || pairs == 0) return;
    int64_t delta = (int64_t) voice->gainTarget - voice->gainNow;
    int64_t advance = (int64_t) voice->gainStepPerPair * pairs;
    if (delta > 0) voice->gainNow += (int32_t) (advance < delta ? advance : delta);
    else voice->gainNow -= (int32_t) (advance < -delta ? advance : -delta);
}

static uint32_t hwRate(const OfVoice *voice) {
    return voice->step > OF_HW_MAX_RATE ? OF_HW_MAX_RATE : voice->step;
}

/* Samples to keep written ahead of the mixer: `pairs` of output time. */
static uint32_t hwLead(const OfVoice *voice, int pairs) {
    uint32_t lead = (uint32_t) (((uint64_t) (pairs > 0 ? pairs : 0) * hwRate(voice)) >> 16) + 2;
    if (lead < OF_HW_MIN_LEAD) lead = OF_HW_MIN_LEAD;
    if (lead > OF_HW_RING_SAMPLES / 2) lead = OF_HW_RING_SAMPLES / 2;
    return (lead + 1) & ~1u;
}

/* Lets a voice go, leaving its decoder at what was heard. */
static void hwReleaseVoice(OfAudioSystem *ut, int v) {
    OfHwVoice *hw = &ut->hw[v];
    if (!hw->running) return;
    uint32_t played = hwPlayedSample(ut, &ut->voices[v], hw);
    hwStopVoice(ut, v);
    seekVoice(ut, &ut->voices[v], played);
}

/* The OS would not give us a voice: mix in software from here on. */
static void hwGiveUp(OfAudioSystem *ut) {
    logWarn("Audio: the hardware mixer would not start a voice, mixing in software.\n");
    for (int v = 0; v < OF_MAX_VOICES; v++) hwReleaseVoice(ut, v);
    ut->hwMixer = false;
}

/* Returns the samples decoded, for the sound table. */
static uint32_t hwPumpVoice(OfAudioSystem *ut, int v, int target, uint32_t elapsedPairs) {
    OfVoice *voice = &ut->voices[v];
    OfHwVoice *hw = &ut->hw[v];

    if (!voice->active || (hw->running && hw->instanceId != voice->instanceId)) hwStopVoice(ut, v);
    if (!voice->active) {
        hwDropRestore(ut, v);
        return 0;
    }
    if (voiceSkipped(ut, voice)) {
        /* Muted: from here skipVoice moves it on, from what was heard. */
        hwReleaseVoice(ut, v);
        hwDropRestore(ut, v);
        return 0;
    }
    if (voice->data == NULL) return 0;
    bool audible = !voice->paused && !ut->allPaused;
    volatile uint32_t *ring = hwPortRingWords(ut->hwRings[v]);

    if (!hw->running) {
        if (!audible) return 0;
        if (ut->hwStartAfterNanos != 0) {
            if (nowNanos() < ut->hwStartAfterNanos) return 0;
            ut->hwStartAfterNanos = 0;
        }
        memset(hw, 0, sizeof(*hw));
        OfVoice before = *voice;
        uint32_t restored = 0;
        if (ut->hwRestore[v] != NULL || ut->hwRestoreCarry[v]) {
            /* The samples a Memory kept, then on from the decoder. */
            const int16_t *pcm = ut->hwRestore[v];
            restored = ut->hwRestoreCount[v];
            for (uint32_t i = 0; i < restored; i += 2) hwPutWord(ring, i, pcm[i], pcm[i + 1]);
            hw->wptr = restored;
            hw->haveCarry = ut->hwRestoreCarry[v];
            hw->carry = ut->hwRestoreCarryValue[v];
            hwDropRestore(ut, v);
        }
        uint32_t lead = hwLead(voice, target);
        if (restored < lead) hwDecode(ut, voice, hw, ring, (lead - restored) & ~1u);
        if (hw->wptr == 0 && !hw->padded) {
            if (voice->finished) voice->active = false;
            return 0; /* nothing read yet */
        }
        uint32_t decoded = hw->padded ? hw->endWptr : hw->wptr;
        hw->rate = hwRate(voice);
        if (!hwPortStart(hw, ut->hwRings[v], OF_HW_RING_SAMPLES, hw->wptr, hw->rate)) {
            /* Back to where this voice was, for the software mix. */
            memset(hw, 0, sizeof(*hw));
            *voice = before;
            hwGiveUp(ut);
            return 0;
        }
        hw->running = true;
        hw->instanceId = voice->instanceId;
        hw->volume = hwVolume(ut, voice);
        hwPortSetVolume(hw, hw->volume);
        return decoded;
    }

    int32_t position = hwPortPosition(hw);
    if (position < 0) {
        /* The OS has let the voice go; start it again next time. */
        memset(hw, 0, sizeof(*hw));
        return 0;
    }
    uint32_t backlog = hwBacklog(hw, position);
    if (hw->padded && backlog <= ((hw->wptr - hw->endWptr) & OF_HW_RING_MASK)) {
        hwStopVoice(ut, v);
        voice->active = false;
        return 0;
    }

    if (!audible) {
        if (!hw->held) {
            hwPortSetRate(hw, 0);
            hwPortSetVolume(hw, 0);
            hw->held = true;
        }
        return 0;
    }
    if (hw->held) {
        hw->held = false;
        hw->rate = UINT32_MAX;
        hw->volume = -1;
    }

    hwAdvanceGain(voice, elapsedPairs);
    uint32_t rate = hwRate(voice);
    if (rate != hw->rate) {
        hwPortSetRate(hw, rate);
        hw->rate = rate;
    }
    int volume = hwVolume(ut, voice);
    if (volume != hw->volume) {
        hwPortSetVolume(hw, volume);
        hw->volume = volume;
    }

    if (hw->padded) return 0;
    uint32_t lead = hwLead(voice, target);
    if (backlog >= lead) return 0;
    uint32_t before = hw->wptr;
    hwDecode(ut, voice, hw, ring, (lead - backlog) & ~1u);
    if (hw->wptr != before) hwPortSetWptr(hw, hw->wptr);
    return (hw->wptr - before) & OF_HW_RING_MASK;
}

static void hwPump(OfAudioSystem *ut, int target) {
    const bool timed = ofPerfDetail;
    uint64_t now = nowNanos();
    uint32_t elapsedPairs = 0;
    if (ut->hwLastPumpNanos != 0 && now > ut->hwLastPumpNanos) {
        uint64_t pairs = (now - ut->hwLastPumpNanos) * OF_AUDIO_RATE / 1000000000ull;
        elapsedPairs = pairs > OF_AUDIO_RATE ? OF_AUDIO_RATE : (uint32_t) pairs;
    }
    ut->hwLastPumpNanos = now;

    uint64_t decoded = 0, running = 0;
    for (int v = 0; v < OF_MAX_VOICES && ut->hwMixer; v++) {
        decoded += hwPumpVoice(ut, v, target, elapsedPairs);
        running += ut->hw[v].running && !ut->hw[v].held;
    }
    /* The sound table: mix and voices are the decoding, write is nothing,
     * pairs the output time that passed and voices those times the voices
     * the mixer was playing. */
    uint64_t nanos = timed ? nowNanos() - now : 0;
    if (timed) ofPerfAddLoad(OF_LOAD_MIX, nanos);
    ofPerfAddLoad(OF_LOAD_MIX_VOICES, nanos);
    ofPerfAddLoad(OF_LOAD_MIX_PAIRS, elapsedPairs);
    ofPerfAddLoad(OF_LOAD_MIX_VOICE_PAIRS, running * elapsedPairs);
    (void) decoded;
}

/* ===[ Voice lookup helpers ]=== */

static bool voiceMatches(const OfVoice *voice, int32_t soundOrInstance) {
    if (!voice->active) return false;
    if (soundOrInstance >= OF_INSTANCE_ID_BASE && soundOrInstance < OF_STREAM_INDEX_BASE)
        return voice->instanceId == soundOrInstance;
    return voice->sourceIndex == soundOrInstance;
}

static OfStream *streamFor(OfAudioSystem *ut, int32_t index) {
    int32_t slot = index - OF_STREAM_INDEX_BASE;
    if (slot < 0 || slot >= OF_MAX_STREAMS || !ut->streams[slot].active) return NULL;
    return &ut->streams[slot];
}

static int32_t gainToFixed(float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
    return (int32_t) (gain * (float) OF_GAIN_ONE) << 8;
}

/* ===[ Vtable ]=== */

static void ofInit(AudioSystem *audio, DataWin *dataWin, FileSystem *fileSystem) {
    (void) fileSystem;
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    audio->dw = dataWin;
    arrput(audio->audioGroups, dataWin);

    adpcmDiffInit();
    of_audio_init();
    ut->queueCapacity = of_audio_free();
#ifndef OF_PC
    ut->snapshotReservedPairs = 1;
#endif
    if (ut->queueCapacity > 0 && ut->queueCapacity <= OF_AUDIO_RATE * 4)
        ut->snapshotPcm = calloc((size_t)ut->queueCapacity * 2, sizeof(int16_t));
    ut->masterGain = 1.0f;
    ut->nextInstanceId = OF_INSTANCE_ID_BASE;
    openPack(ut);
    hwMixerInit(ut);
    if (ut->file != NULL) logInfo("Audio: mixing in %s.\n", ut->hwMixer ? "the hardware mixer" : "software");

#ifdef OF_PC
    const char *dumpPath = getenv("BS_AUDIO_DUMP");
    if (dumpPath != NULL) ut->dump = fopen(dumpPath, "wb");
#else
    of_file_set_idle_hook(idleHook);
#endif
}

static void ofDestroy(AudioSystem *audio) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
#ifndef OF_PC
    of_file_set_idle_hook(NULL);
#endif
    g_audio = NULL;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        hwStopVoice(ut, v);
        hwDropRestore(ut, v);
        free(ut->hwRings[v]);
    }
    if (ut->file != NULL) fclose(ut->file);
    if (ut->dump != NULL) fclose(ut->dump);
    if (ut->cachedSounds != NULL) {
        for (uint32_t t = 0; t < ut->trackCount; t++) free(ut->cachedSounds[t]);
    }
    free(ut->cachedSounds);
    free(ut->cachedLastUse);
    free(ut->cachedSilentFrom);
    free(ut->tracks);
    free(ut->snapshotPcm);
    free(audio->groupGains);
    free(ut);
}

static void updateAudio(OfAudioSystem *ut, float deltaTime);

static void ofUpdate(AudioSystem *audio, float deltaTime) {
    ofPerfPhase(OF_PHASE_AUDIO);
    if (!((OfAudioSystem *) audio)->allPaused) AudioSystem_updateGroupGains(audio, deltaTime);
    updateAudio((OfAudioSystem *) audio, deltaTime);
    ofPerfPhase(OF_PHASE_DRAW);
}

/* Reads music ahead, a little per frame. A piece is a second of sound and a
 * frame plays a thirtieth of that, so one piece per frame, given to the
 * stream with the least buffered, keeps every stream full. A track that
 * starts is not read at all until the next frame: tracks start on room
 * changes, and filling two read-aheads there cost 160 ms of an already slow
 * frame. Its sound begins a frame late instead. A stream that is empty or
 * about to run dry is read regardless: that is every track on the frame
 * after it starts, and otherwise only happens after a very long frame.
 * Nothing is mixed between these reads, so tracks started together (the
 * layered house music) stay in step. */
static void refillStreams(OfAudioSystem *ut) {
    OfVoice *neediest = NULL;
    uint32_t least = OF_READAHEAD;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        OfVoice *voice = &ut->voices[v];
        if (!voice->active || voice->streamSlot < 0 || voiceSkipped(ut, voice)) continue;
        if (voice->startedThisFrame) {
            voice->startedThisFrame = false;
            continue;
        }
        uint32_t buffered = voice->bufferLen - voice->bufferPos;
        if (buffered < OF_READ_PIECE / 2) {
            refillVoice(ut, voice);
            continue;
        }
        if (buffered < least) {
            least = buffered;
            neediest = voice;
        }
    }
    if (neediest != NULL) refillVoice(ut, neediest);
}

/* ===[ Muting ]===
 * The audio mode (a debug setting, to see what sound costs) mutes everything,
 * the music or the sound effects. Music here is what the game streams or
 * keeps in a file of its own, and an effect is a sound embedded in its data,
 * however long either is. A muted voice is not decoded, mixed or read from the
 * card, and with everything muted nothing is handed to the OS. Muted sounds
 * still run their course, since the game asks whether they are playing and
 * where they have got to: each frame moves every muted voice on by the game's
 * time, as a seek would. A seek cannot carry the decoder's state, so a sound
 * that was playing when its mute came off is rough for a moment, as after any
 * seek. A short sound started while muted is not loaded until its mute comes
 * off. */

/* mixVoice keeps samplesDecoded * 65536 + frac moving by exactly step per
 * output pair: fetching a sample adds 65536 to one side and takes it from the
 * other. So a skip moves that sum on and splits it again, putting the decoder
 * on the even sample at or below it (a byte boundary) and leaving any odd
 * sample in frac for the mixer to fetch, as it would have. */
static void skipVoice(const OfAudioSystem *ut, OfVoice *voice, int pairs) {
    voice->startedThisFrame = false; /* its first read is due once it is heard */
    const OfMusicTrack *track = &ut->tracks[voice->track];
    uint64_t count = track->sampleCount;
    uint64_t position = ((uint64_t) voice->samplesDecoded << 16) + voice->frac + (uint64_t) voice->step * (uint64_t) pairs;
    uint64_t sample = position >> 16;
    if (sample >= count) {
        if (voice->loop && count > 0) {
            position %= count << 16; /* the mixer starts over on needing sample count */
            sample = position >> 16;
        } else if (count == 0 || sample >= count + 2) {
            /* The mixer fetches a zero after the last sample, then stops. */
            voice->finished = true;
            voice->active = false;
            return;
        } else {
            sample = count; /* nothing left to decode; the next fetch ends the voice */
            voice->finished = true;
        }
    }
    if (sample < count) sample &= ~(uint64_t) 1;
    voice->frac = (uint32_t) (position - (sample << 16));
    if (voice->streamSlot >= 0) {
        voice->bufferLen = voice->bufferPos = 0;
        voice->fileBytePos = (uint32_t) (sample / 2);
    } else {
        voice->bufferPos = (uint32_t) (sample / 2);
    }
    voice->samplesDecoded = (uint32_t) sample;
    voice->adpcm.predictor = voice->adpcm.stepIndex = 0;
    voice->havePendingCode = false;
    voice->sample0 = voice->sample1 = 0;
    voice->gainNow = voice->gainTarget;
}

/* Whether a kind of voice is skipped rather than mixed. A desktop dump mixes
 * whatever the mode, so its output is the same in all of them. */
static bool kindSkipped(const OfAudioSystem *ut, bool music) {
    if (ut->dump != NULL) return false;
    switch (g_audioMode) {
        case OF_AUDIO_DISABLED:   return true;
        case OF_AUDIO_MUSIC_ONLY: return !music;
        case OF_AUDIO_SOUND_ONLY: return music;
        default:                  return false;
    }
}

static bool voiceSkipped(const OfAudioSystem *ut, const OfVoice *voice) {
    return kindSkipped(ut, voice->music);
}

/* Nothing is heard at all, so nothing is queued. */
static bool silent(const OfAudioSystem *ut) {
    return g_audioMode == OF_AUDIO_DISABLED && ut->dump == NULL;
}

void ofAudioSetMode(OfAudioMode mode) {
    g_audioMode = mode;
    if (g_audio == NULL || g_audio->file == NULL) return;
    OfAudioSystem *ut = g_audio;
    /* Muted voices leave the hardware mixer at once; nothing pumps them while all is muted. */
    if (ut->hwMixer) {
        for (int v = 0; v < OF_MAX_VOICES; v++) {
            if (ut->voices[v].active && voiceSkipped(ut, &ut->voices[v])) hwReleaseVoice(ut, v);
        }
    }
    /* Load the short sounds that were started while muted. */
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        OfVoice *voice = &ut->voices[v];
        if (!voice->active || voice->streamSlot >= 0 || voice->data != NULL || voiceSkipped(ut, voice)) continue;
        voice->data = cachedSound(ut, voice->track);
        if (voice->data == NULL) voice->active = false;
    }
}

OfAudioMode ofAudioMode(void) {
    return g_audioMode;
}

static void updateAudio(OfAudioSystem *ut, float deltaTime) {
    if (ut->file == NULL) return;

    if (kindSkipped(ut, false) || kindSkipped(ut, true)) {
        int pairs = (int) (deltaTime * (float) OF_AUDIO_RATE + 0.5f);
        if (pairs > 0 && !ut->allPaused) {
            for (int v = 0; v < OF_MAX_VOICES; v++) {
                OfVoice *voice = &ut->voices[v];
                if (voice->active && !voice->paused && voiceSkipped(ut, voice)) skipVoice(ut, voice, pairs);
            }
        }
        if (silent(ut)) return;
    }

    refillStreams(ut);

    if (ut->dump != NULL) {
        /* Dump mode follows game time rather than the output queue, so the
         * captured audio is the same on every run. */
        mixAndWrite(ut, (int) (deltaTime * (float) OF_AUDIO_RATE + 0.5f));
        return;
    }
    pumpOutput(ut);
}

static int32_t ofPlaySound(AudioSystem *audio, int32_t soundIndex, int32_t priority, bool loop) {
    (void) priority;
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    if (ut->file == NULL) return -1;
    if (ut->nextInstanceId < OF_INSTANCE_ID_BASE || ut->nextInstanceId >= OF_STREAM_INDEX_BASE - 1)
        return -1;

    int32_t track = -1;
    float gain = 1.0f, pitch = 1.0f;
    bool music = true; /* a stream */

    if (soundIndex >= OF_STREAM_INDEX_BASE) {
        OfStream *stream = streamFor(ut, soundIndex);
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
        music = !embedded;
        track = findTrack(ut, embedded ? sound->name : sound->file);
        if (track < 0) track = findTrack(ut, embedded ? sound->file : sound->name);
        gain = sound->volume;
        pitch = sound->pitch > 0.0f ? sound->pitch : 1.0f;
    }
    if (track < 0) return -1;

    bool streamed = trackBytes(&ut->tracks[track]) > OF_MEMORY_SOUND_BYTES;
    uint8_t *soundData = NULL;
    if (!streamed && kindSkipped(ut, music)) {
        /* Not read until the mute comes off; see ofAudioSetMode. */
        if (trackBytes(&ut->tracks[track]) == 0) return -1;
    } else if (!streamed) {
        soundData = cachedSound(ut, track);
        if (soundData == NULL) return -1;
    }

    /* Take a free voice. Failing that, take over the oldest one of the same
     * kind, so effects never cut off the music and the reverse. */
    OfVoice *voice = NULL;
    int streamedCount = 0;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        if (!ut->voices[v].active) {
            if (voice == NULL) voice = &ut->voices[v];
        } else if (ut->voices[v].streamSlot >= 0) {
            streamedCount++;
        }
    }
    if (voice == NULL || (streamed && streamedCount >= OF_MAX_STREAMED)) {
        voice = NULL;
        for (int v = 0; v < OF_MAX_VOICES; v++) {
            OfVoice *candidate = &ut->voices[v];
            if (!candidate->active || (candidate->streamSlot >= 0) != streamed) continue;
            if (voice == NULL || candidate->instanceId < voice->instanceId) voice = candidate;
        }
        if (voice == NULL) return -1;
        voice->active = false;
    }
    if (ut->hwMixer) hwStopVoice(ut, (int) (voice - ut->voices));

    int32_t streamSlot = -1;
    if (streamed) {
        for (int32_t slot = 0; slot < OF_MAX_STREAMED && streamSlot < 0; slot++) {
            bool taken = false;
            for (int v = 0; v < OF_MAX_VOICES; v++)
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
    voice->music = music;
    voice->sourceIndex = soundIndex;
    voice->instanceId = ut->nextInstanceId++;
    voice->pitch = pitch;
    voice->gainNow = voice->gainTarget = gainToFixed(gain);
    voice->gainStepPerPair = 0;
    updateStep(ut, voice);
    voice->active = true;
    voice->startedThisFrame = true;
    /* Effects fire constantly; only log the long tracks (BS_AUDIO_LOG=1 on
     * desktop logs everything). */
#ifdef OF_PC
    static int logAll = -1;
    if (logAll < 0) logAll = getenv("BS_AUDIO_LOG") != NULL;
    if (logAll && !streamed) logInfo("Audio: sfx %s gain %.2f pitch %.2f\n", ut->tracks[track].name, (double) gain, (double) pitch);
#endif
    if (streamed)
        logInfo("Audio: playing %s%s gain %.2f pitch %.2f\n", ut->tracks[track].name, loop ? " (loop)" : "",
                (double) gain, (double) pitch);
    return voice->instanceId;
}

static void ofStopSound(AudioSystem *audio, int32_t soundOrInstance) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) {
            ut->voices[v].active = false;
            if (ut->hwMixer) hwStopVoice(ut, v);
        }
    }
}

static void ofStopAll(AudioSystem *audio) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        ut->voices[v].active = false;
        if (ut->hwMixer) hwStopVoice(ut, v);
    }
}

static bool ofIsPlaying(AudioSystem *audio, int32_t soundOrInstance) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) return true;
    }
    return false;
}

static void setPaused(OfAudioSystem *ut, int32_t soundOrInstance, bool paused) {
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) ut->voices[v].paused = paused;
    }
}

static void ofPauseSound(AudioSystem *audio, int32_t soundOrInstance) {
    setPaused((OfAudioSystem *) audio, soundOrInstance, true);
}

static void ofResumeSound(AudioSystem *audio, int32_t soundOrInstance) {
    setPaused((OfAudioSystem *) audio, soundOrInstance, false);
}

static void ofPauseAll(AudioSystem *audio) {
    ((OfAudioSystem *) audio)->allPaused = true;
}

static void ofResumeAll(AudioSystem *audio) {
    ((OfAudioSystem *) audio)->allPaused = false;
}

static bool ofIsAllPaused(AudioSystem *audio) {
    return ((OfAudioSystem *) audio)->allPaused;
}

static void ofSetSoundGain(AudioSystem *audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    OfStream *stream = streamFor(ut, soundOrInstance);
    if (stream != NULL) stream->gain = gain;

    for (int v = 0; v < OF_MAX_VOICES; v++) {
        OfVoice *voice = &ut->voices[v];
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

static float ofGetSoundGain(AudioSystem *audio, int32_t soundOrInstance) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance))
            return (float) (ut->voices[v].gainNow >> 8) / (float) OF_GAIN_ONE;
    }
    OfStream *stream = streamFor(ut, soundOrInstance);
    return stream != NULL ? stream->gain : 1.0f;
}

static void ofSetSoundPitch(AudioSystem *audio, int32_t soundOrInstance, float pitch) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    if (pitch <= 0.0f) pitch = 0.01f;
    OfStream *stream = streamFor(ut, soundOrInstance);
    if (stream != NULL) stream->pitch = pitch;

    for (int v = 0; v < OF_MAX_VOICES; v++) {
        OfVoice *voice = &ut->voices[v];
        if (!voiceMatches(voice, soundOrInstance)) continue;
        voice->pitch = pitch;
        updateStep(ut, voice);
    }
}

static float ofGetSoundPitch(AudioSystem *audio, int32_t soundOrInstance) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) return ut->voices[v].pitch;
    }
    OfStream *stream = streamFor(ut, soundOrInstance);
    return stream != NULL ? stream->pitch : 1.0f;
}

static float ofGetTrackPosition(AudioSystem *audio, int32_t soundOrInstance) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        const OfVoice *voice = &ut->voices[v];
        if (!voiceMatches(voice, soundOrInstance)) continue;
        uint32_t sample = ut->hwMixer && ut->hw[v].running ? hwPlayedSample(ut, voice, &ut->hw[v]) : voice->samplesDecoded;
        return (float) sample / (float) ut->tracks[voice->track].sampleRate;
    }
    return 0.0f;
}

/* Seeks by restarting the decoder at the target byte. ADPCM state is not
 * stored per position, so the first few milliseconds after a seek to
 * anywhere but the start are approximate. */
static void ofSetTrackPosition(AudioSystem *audio, int32_t soundOrInstance, float positionSeconds) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        OfVoice *voice = &ut->voices[v];
        if (!voiceMatches(voice, soundOrInstance)) continue;

        if (positionSeconds < 0.0f) positionSeconds = 0.0f;
        seekVoice(ut, voice, (uint32_t) (positionSeconds * (float) ut->tracks[voice->track].sampleRate));
        /* What the ring holds is from before the seek. */
        if (ut->hwMixer) hwStopVoice(ut, v);
    }
}

static float ofGetSoundLength(AudioSystem *audio, int32_t soundOrInstance) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    int32_t track = -1;

    for (int v = 0; v < OF_MAX_VOICES && track < 0; v++) {
        if (voiceMatches(&ut->voices[v], soundOrInstance)) track = ut->voices[v].track;
    }
    if (track < 0) {
        OfStream *stream = streamFor(ut, soundOrInstance);
        if (stream != NULL) {
            track = stream->track;
        } else if (soundOrInstance >= 0 && soundOrInstance < OF_INSTANCE_ID_BASE) {
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

static void ofSetMasterGain(AudioSystem *audio, float gain) {
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 2.0f) gain = 2.0f;
    ((OfAudioSystem *) audio)->masterGain = gain;
}

static void ofSetMasterGainForListener(AudioSystem *audio, float gain, int32_t listenerId) {
    (void) listenerId;
    ofSetMasterGain(audio, gain);
}

static void ofSetChannelCount(AudioSystem *audio, int32_t count) {
    (void) audio;
    (void) count;
}

static void ofGroupLoad(AudioSystem *audio, int32_t groupIndex) {
    (void) audio;
    (void) groupIndex;
}

static bool ofGroupIsLoaded(AudioSystem *audio, int32_t groupIndex) {
    (void) audio;
    (void) groupIndex;
    return true;
}

static int32_t ofCreateStream(AudioSystem *audio, const char *filename) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    int32_t track = findTrack(ut, filename);
    if (track < 0) {
        if (ut->trackCount > 0) logWarn("Audio: '%s' is not in the music pack.\n", filename);
        return -1;
    }

    for (int32_t i = 0; i < OF_MAX_STREAMS; i++) {
        if (ut->streams[i].active) continue;
        ut->streams[i].active = true;
        ut->streams[i].track = track;
        ut->streams[i].gain = 1.0f;
        ut->streams[i].pitch = 1.0f;
        return OF_STREAM_INDEX_BASE + i;
    }
    logWarn("Audio: no free stream slots for '%s'.\n", filename);
    return -1;
}

static bool ofDestroyStream(AudioSystem *audio, int32_t streamIndex) {
    OfAudioSystem *ut = (OfAudioSystem *) audio;
    OfStream *stream = streamFor(ut, streamIndex);
    if (stream == NULL) return false;
    ofStopSound(audio, streamIndex);
    stream->active = false;
    return true;
}

/* The Pocket mixer is non-spatial. Keep positional APIs usable without
 * changing the voice's gain, pitch or decoder position. */
static void ofSetSoundSpatial(AudioSystem *audio, int32_t instanceId,
                              float x, float y, float z, float vx, float vy, float vz,
                              float ref, float max, float factor) {
    (void)audio; (void)instanceId;
    (void)x; (void)y; (void)z; (void)vx; (void)vy; (void)vz;
    (void)ref; (void)max; (void)factor;
}

static void ofSetListenerPosition(AudioSystem *audio, float x, float y, float z) {
    audio->listenerX = x; audio->listenerY = y; audio->listenerZ = z;
}

static AudioSystemVtable ofVtable = {
    .init = ofInit,
    .destroy = ofDestroy,
    .update = ofUpdate,
    .playSound = ofPlaySound,
    .setSoundSpatial = ofSetSoundSpatial,
    .setListenerPosition = ofSetListenerPosition,
    .stopSound = ofStopSound,
    .stopAll = ofStopAll,
    .isPlaying = ofIsPlaying,
    .pauseSound = ofPauseSound,
    .resumeSound = ofResumeSound,
    .pauseAll = ofPauseAll,
    .resumeAll = ofResumeAll,
    .suspend = ofPauseAll,
    .resume = ofResumeAll,
    .setSoundGain = ofSetSoundGain,
    .getSoundGain = ofGetSoundGain,
    .setSoundPitch = ofSetSoundPitch,
    .getSoundPitch = ofGetSoundPitch,
    .getTrackPosition = ofGetTrackPosition,
    .setTrackPosition = ofSetTrackPosition,
    .getSoundLength = ofGetSoundLength,
    .setMasterGain = ofSetMasterGain,
    .setMasterGainForListener = ofSetMasterGainForListener,
    .setChannelCount = ofSetChannelCount,
    .setGroupGain = AudioSystem_setGroupGain,
    .groupLoad = ofGroupLoad,
    .groupIsLoaded = ofGroupIsLoaded,
    .createStream = ofCreateStream,
    .destroyStream = ofDestroyStream,
    .pauseStopsFades = true,
    .isAllPaused = ofIsAllPaused,
};

AudioSystem *platformCreateAudioSystem(void) {
    OfAudioSystem *ut = safeCalloc(1, sizeof(OfAudioSystem));
    ut->base.vtable = &ofVtable;
    g_audio = ut;
    return &ut->base;
}


/* Decoder cursors and the unread stream tail make restoring sample-exact;
 * short sound bytes are prepared from the immutable pack, never pointer-saved. */
#define OF_AUDIO_SNAPSHOT_MAGIC 0x31554155u
#define OF_VOICE_U32(F) \
    F(instanceId) F(sourceIndex) F(track) F(streamSlot) \
    F(bufferLen) F(bufferPos) F(fileBytePos) F(samplesDecoded) F(pendingCode) \
    F(sample0) F(sample1) F(frac) F(step) F(gainNow) F(gainTarget) F(gainStepPerPair)

typedef struct {
    OfStream streams[OF_MAX_STREAMS];
    OfVoice voices[OF_MAX_VOICES];
    uint8_t streamBuffers[OF_MAX_STREAMED][OF_READAHEAD];
    uint8_t **sounds;
    uint32_t trackCount;
    AudioGroupGain *gains;
    uint32_t gainCount;
    int32_t nextInstanceId;
    float masterGain, listener[3];
    bool allPaused;
    OfAudioMode mode;
    int16_t *pcm;
    uint32_t pcmPairs;
    /* Hardware mixer only: each voice's unplayed ring samples. */
    int16_t *hwPcm[OF_MAX_VOICES];
    uint32_t hwCount[OF_MAX_VOICES];
    bool hwCarry[OF_MAX_VOICES];
    int16_t hwCarryValue[OF_MAX_VOICES];
} OfAudioSnapshot;

/* Follows the software queue in a state made with the hardware mixer; a
 * state without it (older, or the software mix) has nothing after the queue. */
#define OF_AUDIO_HW_SNAPSHOT_MAGIC 0x31574855u /* "UHW1" */

/* The samples written to a voice's ring that it has not played yet, oldest
 * first, made even by leaving out the oldest: it is being played as the
 * state is taken. Silence after a track's end is left out; the decoder,
 * which has finished, writes it again. */
static uint32_t hwUnplayed(const OfAudioSystem *ut, int v, uint32_t *from) {
    const OfHwVoice *hw = &ut->hw[v];
    if (!ut->hwMixer || !hw->running || hw->instanceId != ut->voices[v].instanceId) return 0;
    int32_t position = hwPortPosition(hw);
    if (position < 0) return 0;
    uint32_t backlog = hwBacklog(hw, position);
    if (hw->padded) {
        uint32_t pad = (hw->wptr - hw->endWptr) & OF_HW_RING_MASK;
        backlog = backlog > pad ? backlog - pad : 0;
    }
    backlog &= ~1u;
    uint32_t end = hw->padded ? hw->endWptr : hw->wptr;
    *from = (end - backlog) & OF_HW_RING_MASK;
    return backlog;
}

void ofAudioSnapshotLock(bool locked) { g_snapshotLocked = locked; }

bool ofAudioSnapshot(AudioSystem *audio, uint8_t **data, size_t *size, const char **reason) {
    OfAudioSystem *ut = (OfAudioSystem *)audio;
    OfSnapshotWriter w = {.ok = true};
    *data = NULL; *size = 0;
    uint32_t queued = 0;
    int freePairs = 0;
    const char *failure = "encoding/allocation";
    int failureVoice = -1;
    if (ut->queueCapacity > 0) {
        freePairs = of_audio_free();
        if (freePairs < 0 || freePairs > ut->queueCapacity) { failure = "queue capacity"; goto bad; }
        queued = (uint32_t)(ut->queueCapacity - freePairs);
        /* Pocket reserves one pair once the stream voice is activated. */
        if (ut->snapshotWritten && queued >= (uint32_t)ut->snapshotReservedPairs)
            queued -= (uint32_t)ut->snapshotReservedPairs;
        if (queued && (!ut->snapshotPcm || queued > ut->snapshotWritten)) { failure = "queue shadow"; goto bad; }
    }
    ofSnapU32(&w, OF_AUDIO_SNAPSHOT_MAGIC);
    ofSnapU32(&w, ut->trackCount);
    ofSnapU32(&w, ofSnapHash(ut->tracks, (size_t)ut->trackCount * sizeof(OfMusicTrack)));
    ofSnapU32(&w, (uint32_t)ut->nextInstanceId);
    ofSnapFloat(&w, ut->masterGain); ofSnapU32(&w, ut->allPaused);
    ofSnapU32(&w, (uint32_t)g_audioMode);
    ofSnapFloat(&w, audio->listenerX); ofSnapFloat(&w, audio->listenerY); ofSnapFloat(&w, audio->listenerZ);
    ofSnapU32(&w, audio->groupGainCount);
    for (uint32_t i = 0; i < audio->groupGainCount; i++) {
        AudioGroupGain *g = &audio->groupGains[i];
        ofSnapFloat(&w, g->current); ofSnapFloat(&w, g->start); ofSnapFloat(&w, g->target);
        ofSnapFloat(&w, g->timeRemaining); ofSnapFloat(&w, g->totalTime);
    }
    for (unsigned i = 0; i < OF_MAX_STREAMS; i++) {
        OfStream *t = &ut->streams[i];
        ofSnapU32(&w, t->active); ofSnapU32(&w, (uint32_t)t->track);
        ofSnapFloat(&w, t->gain); ofSnapFloat(&w, t->pitch);
    }
    for (unsigned i = 0; i < OF_MAX_VOICES; i++) {
        OfVoice v = ut->voices[i];
        if (!v.active) memset(&v, 0, sizeof(v));
        if (v.active && (v.track < 0 || (uint32_t)v.track >= ut->trackCount ||
            v.bufferPos > v.bufferLen || v.streamSlot >= OF_MAX_STREAMED)) {
            failure = "voice cursor"; failureVoice = (int)i; goto bad;
        }
        uint32_t flags = (uint32_t)v.active | (uint32_t)v.paused << 1 | (uint32_t)v.loop << 2 |
            (uint32_t)v.finished << 3 | (uint32_t)v.music << 4 |
            (uint32_t)v.startedThisFrame << 5 | (uint32_t)v.havePendingCode << 6;
        ofSnapU32(&w, flags);
#define WRITE_VOICE(f) ofSnapU32(&w, (uint32_t)v.f);
        OF_VOICE_U32(WRITE_VOICE)
#undef WRITE_VOICE
        ofSnapU32(&w, (uint32_t)v.adpcm.predictor); ofSnapU32(&w, (uint32_t)v.adpcm.stepIndex);
        ofSnapFloat(&w, v.pitch);
        if (v.active && v.streamSlot >= 0) {
            if (v.bufferLen > OF_READAHEAD || (v.bufferLen > v.bufferPos && !v.data)) {
                failure = "stream buffer"; failureVoice = (int)i; goto bad;
            }
            ofSnapWrite(&w, v.data ? v.data + v.bufferPos : NULL, v.bufferLen - v.bufferPos);
        }
    }
    ofSnapU32(&w, queued);
    for (uint32_t i = 0; i < queued; i++) {
        size_t at = (size_t)((ut->snapshotWritten - queued + i) % (uint64_t)ut->queueCapacity) * 2;
        uint32_t pair = (uint16_t)ut->snapshotPcm[at] | (uint32_t)(uint16_t)ut->snapshotPcm[at + 1] << 16;
        ofSnapU32(&w, pair);
    }
    if (ut->hwMixer) {
        ofSnapU32(&w, OF_AUDIO_HW_SNAPSHOT_MAGIC);
        for (int v = 0; v < OF_MAX_VOICES; v++) {
            const OfHwVoice *hw = &ut->hw[v];
            bool active = ut->voices[v].active;
            if (active && !hw->running) {
                /* Not started again since a Memory was loaded (paused, or
                 * waiting): keep what that Memory kept. */
                uint32_t count = ut->hwRestoreCount[v];
                bool carry = ut->hwRestoreCarry[v];
                ofSnapU32(&w, count);
                ofSnapU32(&w, (uint32_t) carry | (uint32_t) (uint16_t) (carry ? ut->hwRestoreCarryValue[v] : 0) << 16);
                const int16_t *pcm = ut->hwRestore[v];
                for (uint32_t i = 0; i < count; i += 2)
                    ofSnapU32(&w, (uint32_t) (uint16_t) pcm[i] | (uint32_t) (uint16_t) pcm[i + 1] << 16);
                continue;
            }
            uint32_t from = 0, count = active ? hwUnplayed(ut, v, &from) : 0;
            bool carry = active && hw->running && hw->instanceId == ut->voices[v].instanceId && hw->haveCarry;
            ofSnapU32(&w, count);
            ofSnapU32(&w, (uint32_t) carry | (uint32_t) (uint16_t) (carry ? hw->carry : 0) << 16);
            const int16_t *ring = (const int16_t *) hwPortRingWords(ut->hwRings[v]);
            for (uint32_t i = 0; i < count; i += 2) {
                int16_t s0 = ring[(from + i) & OF_HW_RING_MASK], s1 = ring[(from + i + 1) & OF_HW_RING_MASK];
                ofSnapU32(&w, (uint32_t) (uint16_t) s0 | (uint32_t) (uint16_t) s1 << 16);
            }
        }
    }
    if (!w.ok) goto bad;
    *data = w.data; *size = w.size; return true;
bad:
    if (failureVoice >= 0) {
        const OfVoice *v = &ut->voices[failureVoice];
        logInfo("State audio rejected: %s, voice %d track %d slot %d cursor %u/%u file %u data %d\n",
            failure, failureVoice, v->track, v->streamSlot, v->bufferPos, v->bufferLen,
            v->fileBytePos, v->data != NULL);
    }
    logInfo("State audio capture: %s, capacity %d free %d queued %u reserved %d written %llu shadow %d encoded %zu/%zu\n",
        failure, ut->queueCapacity, freePairs, queued, ut->snapshotReservedPairs,
        (unsigned long long)ut->snapshotWritten, ut->snapshotPcm != NULL, w.size, w.capacity);
    free(w.data); *reason = "Audio cannot be captured safely"; return false;
}

void ofAudioFreeSnapshot(void *ptr) {
    OfAudioSnapshot *p = ptr;
    if (!p) return;
    if (p->sounds) for (uint32_t i = 0; i < p->trackCount; i++) free(p->sounds[i]);
    for (int v = 0; v < OF_MAX_VOICES; v++) free(p->hwPcm[v]);
    free(p->sounds); free(p->gains); free(p->pcm); free(p);
}

void *ofAudioPrepareSnapshot(AudioSystem *audio, const uint8_t *data, size_t size, const char **reason) {
    OfAudioSystem *ut = (OfAudioSystem *)audio;
    OfSnapshotReader r = {data, size, 0, true};
    OfAudioSnapshot *p = NULL;
    if (ofSnapGetU32(&r) != OF_AUDIO_SNAPSHOT_MAGIC || ofSnapGetU32(&r) != ut->trackCount ||
        ofSnapGetU32(&r) != ofSnapHash(ut->tracks, (size_t)ut->trackCount * sizeof(OfMusicTrack))) goto bad;
    p = calloc(1, sizeof(*p));
    if (!p) goto bad;
    p->trackCount = ut->trackCount;
    p->sounds = calloc(p->trackCount ? p->trackCount : 1, sizeof(uint8_t *));
    if (!p->sounds) goto bad;
    p->nextInstanceId = (int32_t)ofSnapGetU32(&r);
    if (p->nextInstanceId < OF_INSTANCE_ID_BASE || p->nextInstanceId >= OF_STREAM_INDEX_BASE) goto bad;
    p->masterGain = ofSnapGetFloat(&r); uint32_t paused = ofSnapGetU32(&r);
    p->mode = (OfAudioMode)ofSnapGetU32(&r); p->allPaused = paused != 0;
    if (paused > 1 || (uint32_t)p->mode >= OF_AUDIO_MODE_COUNT || !isfinite(p->masterGain) ||
        p->masterGain < 0 || p->masterGain > 4) goto bad;
    for (unsigned i = 0; i < 3; i++) { p->listener[i] = ofSnapGetFloat(&r); if (!isfinite(p->listener[i])) goto bad; }
    p->gainCount = ofSnapGetU32(&r);
    if (p->gainCount > 1024 || p->gainCount > (r.size - r.offset) / 20) goto bad;
    if (p->gainCount) {
        p->gains = calloc(p->gainCount, sizeof(AudioGroupGain));
        if (!p->gains) goto bad;
    }
    for (uint32_t i = 0; i < p->gainCount; i++) {
        AudioGroupGain *g = &p->gains[i];
        g->current = ofSnapGetFloat(&r); g->start = ofSnapGetFloat(&r); g->target = ofSnapGetFloat(&r);
        g->timeRemaining = ofSnapGetFloat(&r); g->totalTime = ofSnapGetFloat(&r);
        if (!isfinite(g->current) || !isfinite(g->start) || !isfinite(g->target) ||
            !isfinite(g->timeRemaining) || !isfinite(g->totalTime)) goto bad;
    }
    for (unsigned i = 0; i < OF_MAX_STREAMS; i++) {
        OfStream *t = &p->streams[i]; uint32_t active = ofSnapGetU32(&r);
        t->active = active != 0; t->track = (int32_t)ofSnapGetU32(&r);
        t->gain = ofSnapGetFloat(&r); t->pitch = ofSnapGetFloat(&r);
        if (active > 1 || !isfinite(t->gain) || !isfinite(t->pitch) ||
            (active && (t->track < 0 || (uint32_t)t->track >= ut->trackCount))) goto bad;
    }
    bool used[OF_MAX_STREAMED] = {0};
    for (unsigned i = 0; i < OF_MAX_VOICES; i++) {
        OfVoice *v = &p->voices[i]; uint32_t flags = ofSnapGetU32(&r);
        v->active = (flags & 1) != 0; v->paused = (flags & 2) != 0; v->loop = (flags & 4) != 0;
        v->finished = (flags & 8) != 0; v->music = (flags & 16) != 0;
        v->startedThisFrame = (flags & 32) != 0; v->havePendingCode = (flags & 64) != 0;
#define READ_VOICE(f) v->f = (__typeof__(v->f))ofSnapGetU32(&r);
        OF_VOICE_U32(READ_VOICE)
#undef READ_VOICE
        v->adpcm.predictor = (int32_t)ofSnapGetU32(&r); v->adpcm.stepIndex = (int32_t)ofSnapGetU32(&r);
        v->pitch = ofSnapGetFloat(&r);
        if (!r.ok || flags > 127) goto bad;
        if (!v->active) continue;
        if (v->instanceId < OF_INSTANCE_ID_BASE || v->instanceId >= p->nextInstanceId) goto bad;
        for (unsigned j = 0; j < i; j++)
            if (p->voices[j].active && p->voices[j].instanceId == v->instanceId) goto bad;
        if (v->track < 0 || (uint32_t)v->track >= ut->trackCount || v->streamSlot < -1 ||
            v->streamSlot >= OF_MAX_STREAMED || v->bufferPos > v->bufferLen ||
            v->adpcm.predictor < -32768 || v->adpcm.predictor > 32767 ||
            v->adpcm.stepIndex < 0 || v->adpcm.stepIndex > 88 || v->pendingCode > 15 ||
            !isfinite(v->pitch) || v->pitch <= 0 || v->samplesDecoded > ut->tracks[v->track].sampleCount ||
            v->step < 1 || v->step > 8u * 65536u || v->frac > 9u * 65536u ||
            v->sample0 < -32768 || v->sample0 > 32767 || v->sample1 < -32768 || v->sample1 > 32767 ||
            v->gainNow < 0 || v->gainNow > 4 * OF_GAIN_ONE * 256 ||
            v->gainTarget < 0 || v->gainTarget > 4 * OF_GAIN_ONE * 256 || v->gainStepPerPair < 0) goto bad;
        if (v->streamSlot >= 0) {
            if (used[v->streamSlot] || v->bufferLen > OF_READAHEAD ||
                v->fileBytePos > trackBytes(&ut->tracks[v->track])) goto bad;
            used[v->streamSlot] = true;
            uint32_t remaining = v->bufferLen - v->bufferPos;
            if (!ofSnapRead(&r, p->streamBuffers[v->streamSlot], remaining)) goto bad;
            v->bufferLen = remaining; v->bufferPos = 0;
        } else {
            uint32_t bytes = trackBytes(&ut->tracks[v->track]);
            if (v->bufferLen > bytes || v->bufferPos > bytes || !ut->file || !ut->cachedSounds) goto bad;
            if (!p->sounds[v->track]) {
                p->sounds[v->track] = malloc(bytes ? bytes : 1);
                if (!p->sounds[v->track]) goto bad;
                bool locked = g_snapshotLocked; g_snapshotLocked = true;
                bool ok = fseek(ut->file, (long)ut->tracks[v->track].offset, SEEK_SET) == 0 &&
                    fread(p->sounds[v->track], 1, bytes, ut->file) == bytes;
                g_snapshotLocked = locked;
                if (!ok) goto bad;
            }
        }
    }
    p->pcmPairs = ofSnapGetU32(&r);
    if (p->pcmPairs > (uint32_t)(ut->queueCapacity > 0 ? ut->queueCapacity : 0) ||
        p->pcmPairs > (r.size - r.offset) / 4 || (p->pcmPairs && !ut->snapshotPcm)) goto bad;
    if (p->pcmPairs) { p->pcm = malloc((size_t)p->pcmPairs * 4); if (!p->pcm) goto bad; }
    for (uint32_t i = 0; i < p->pcmPairs; i++) {
        uint32_t pair = ofSnapGetU32(&r);
        p->pcm[i * 2] = (int16_t)pair; p->pcm[i * 2 + 1] = (int16_t)(pair >> 16);
    }
    if (r.ok && r.offset < r.size) {
        if (ofSnapGetU32(&r) != OF_AUDIO_HW_SNAPSHOT_MAGIC) goto bad;
        for (unsigned v = 0; v < OF_MAX_VOICES; v++) {
            uint32_t count = ofSnapGetU32(&r), carry = ofSnapGetU32(&r);
            if (!r.ok || (count & 1) || count > OF_HW_RING_SAMPLES / 2 || count > (r.size - r.offset) / 2 ||
                (carry & 0xFFFEu) != 0 || ((count || (carry & 1)) && !p->voices[v].active)) goto bad;
            p->hwCount[v] = count;
            p->hwCarry[v] = (carry & 1) != 0;
            p->hwCarryValue[v] = (int16_t) (carry >> 16);
            if (count) {
                p->hwPcm[v] = malloc((size_t) count * sizeof(int16_t));
                if (!p->hwPcm[v]) goto bad;
                for (uint32_t i = 0; i < count; i += 2) {
                    uint32_t pair = ofSnapGetU32(&r);
                    p->hwPcm[v][i] = (int16_t) pair; p->hwPcm[v][i + 1] = (int16_t) (pair >> 16);
                }
            }
        }
    }
    if (!r.ok || r.offset != r.size) goto bad;
    return p;
bad:
    ofAudioFreeSnapshot(p); *reason = "Invalid or incompatible audio state"; return NULL;
}

void ofAudioApplySnapshot(AudioSystem *audio, void *ptr) {
    OfAudioSystem *ut = (OfAudioSystem *)audio;
    OfAudioSnapshot *p = ptr;
    for (int v = 0; v < OF_MAX_VOICES; v++) {
        hwStopVoice(ut, v);
        hwDropRestore(ut, v);
        /* The software mix cannot play these; it goes on from the decoder,
         * a little later in each sound than the state was made. */
        if (!ut->hwMixer) continue;
        ut->hwRestore[v] = p->hwPcm[v]; p->hwPcm[v] = NULL;
        ut->hwRestoreCount[v] = p->hwCount[v];
        ut->hwRestoreCarry[v] = p->hwCarry[v];
        ut->hwRestoreCarryValue[v] = p->hwCarryValue[v];
    }
    ut->hwStartAfterNanos = 0;
    for (uint32_t i = 0; i < p->trackCount; i++) if (p->sounds[i]) {
        if (ut->cachedSounds[i]) { free(ut->cachedSounds[i]); ut->cachedBytes -= trackBytes(&ut->tracks[i]); }
        ut->cachedSounds[i] = p->sounds[i]; p->sounds[i] = NULL;
        if (ut->cachedSilentFrom)
            ut->cachedSilentFrom[i] = silentSuffix(ut->cachedSounds[i], trackBytes(&ut->tracks[i]));
        ut->cachedBytes += trackBytes(&ut->tracks[i]);
    }
    memcpy(ut->streamBuffers, p->streamBuffers, sizeof(ut->streamBuffers));
    memcpy(ut->voices, p->voices, sizeof(ut->voices)); memcpy(ut->streams, p->streams, sizeof(ut->streams));
    for (unsigned i = 0; i < OF_MAX_VOICES; i++) {
        OfVoice *v = &ut->voices[i];
        if (v->active) v->data = v->streamSlot >= 0 ? ut->streamBuffers[v->streamSlot] : ut->cachedSounds[v->track];
    }
    ut->masterGain = p->masterGain; ut->nextInstanceId = p->nextInstanceId; ut->allPaused = p->allPaused;
    g_audioMode = p->mode;
    audio->listenerX = p->listener[0]; audio->listenerY = p->listener[1]; audio->listenerZ = p->listener[2];
    free(audio->groupGains); audio->groupGains = p->gains; audio->groupGainCount = p->gainCount; p->gains = NULL;
#ifdef OF_PC
    ofPcAudioSnapshotReset();
#else
    of_audio_init();
#endif
    ut->snapshotWritten = 0;
    if (p->pcmPairs) {
        int written = of_audio_write(p->pcm, (int)p->pcmPairs);
        if (written > 0 && written <= (int)p->pcmPairs) {
            memcpy(ut->snapshotPcm, p->pcm, (size_t)written * 4); ut->snapshotWritten = (uint64_t)written;
            if (ut->hwMixer) ut->hwStartAfterNanos = nowNanos() + (uint64_t) written * 1000000000ull / OF_AUDIO_RATE;
        }
    }
}
