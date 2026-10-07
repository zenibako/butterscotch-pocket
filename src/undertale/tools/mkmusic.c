/*
 * mkmusic — build music.bin, the audio pack: every sound the game plays,
 * converted to mono IMA ADPCM.
 *
 *   mkmusic <data.win> <dir of .ogg files> <music.bin> [sample rate, default 32000]
 *
 * Two sources are combined:
 *   - the external .ogg files the game streams (music and long sounds), named
 *     after the file;
 *   - the sounds embedded in data.win's AUDO chunk (short effects, WAV or
 *     Ogg), named after the sound.
 * See platform/ut_music_pack.h for the format.
 *
 * Host tool only; assumes a little-endian machine.
 */

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/ut_music_pack.h"

int stb_vorbis_decode_filename(const char *filename, int *channels, int *sample_rate, short **output);
int stb_vorbis_decode_memory(const unsigned char *mem, int len, int *channels, int *sample_rate, short **output);

static uint32_t readU32(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint8_t *readFile(const char *path, size_t *outSize) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t) size);
    if (!data || fread(data, 1, (size_t) size, f) != (size_t) size) {
        fprintf(stderr, "%s: read failed\n", path);
        exit(1);
    }
    fclose(f);
    *outSize = (size_t) size;
    return data;
}

static const uint8_t *findChunk(const uint8_t *data, size_t size, const char *name, uint32_t *outLen) {
    size_t pos = 8;
    while (pos + 8 <= size) {
        uint32_t len = readU32(data + pos + 4);
        if (memcmp(data + pos, name, 4) == 0) {
            *outLen = len;
            return data + pos + 8;
        }
        pos += 8 + (size_t) len;
    }
    return NULL;
}

/* Decodes an embedded sound (RIFF/WAVE PCM or Ogg Vorbis) to interleaved
 * 16-bit samples. Returns the frame count, or 0 if the format is not handled. */
static int decodeEmbedded(const uint8_t *blob, uint32_t len, int *channels, int *rate, short **pcm) {
    if (len >= 4 && memcmp(blob, "OggS", 4) == 0) {
        int frames = stb_vorbis_decode_memory(blob, (int) len, channels, rate, pcm);
        return frames > 0 ? frames : 0;
    }
    if (len < 12 || memcmp(blob, "RIFF", 4) != 0 || memcmp(blob + 8, "WAVE", 4) != 0) return 0;

    int format = 0, bits = 0;
    *channels = 0;
    *rate = 0;
    uint32_t pos = 12;
    while (pos + 8 <= len) {
        uint32_t size = readU32(blob + pos + 4);
        const uint8_t *body = blob + pos + 8;
        if (size > len - pos - 8) size = len - pos - 8;

        if (memcmp(blob + pos, "fmt ", 4) == 0 && size >= 16) {
            format = body[0] | (body[1] << 8);
            *channels = body[2] | (body[3] << 8);
            *rate = (int) readU32(body + 4);
            bits = body[14] | (body[15] << 8);
        } else if (memcmp(blob + pos, "data", 4) == 0) {
            if (format != 1 || *channels < 1 || *rate <= 0 || (bits != 8 && bits != 16)) return 0;
            int frames = (int) (size / (uint32_t) (*channels * bits / 8));
            short *out = malloc((size_t) (frames > 0 ? frames : 1) * (size_t) *channels * sizeof(short));
            for (int i = 0; i < frames * *channels; i++) {
                if (bits == 16) out[i] = (short) (body[i * 2] | (body[i * 2 + 1] << 8));
                else out[i] = (short) ((body[i] - 128) << 8);
            }
            *pcm = out;
            return frames;
        }
        pos += 8 + size + (size & 1);
    }
    return 0;
}

/* Mono mixdown plus resampling. Averages the source samples each output
 * sample covers when going down in rate, interpolates when going up. */
static int16_t *toMono(const short *pcm, int frames, int channels, int rate, int outRate, uint32_t *outCount) {
    double ratio = (double) rate / (double) outRate;
    uint32_t count = (uint32_t) ((double) frames / ratio);
    count &= ~1u;
    int16_t *out = malloc((count ? count : 2) * sizeof(int16_t));

    for (uint32_t i = 0; i < count; i++) {
        double start = i * ratio;
        double end = start + ratio;
        double sum = 0.0, weight = 0.0;
        if (ratio > 1.0) {
            for (int f = (int) start; f < frames && f < (int) ceil(end); f++) {
                double lo = f > start ? f : start;
                double hi = f + 1 < end ? f + 1 : end;
                double w = hi - lo;
                if (w <= 0.0) continue;
                double s = 0.0;
                for (int c = 0; c < channels; c++) s += pcm[f * channels + c];
                sum += (s / channels) * w;
                weight += w;
            }
        } else {
            int f0 = (int) start;
            int f1 = f0 + 1 < frames ? f0 + 1 : f0;
            double t = start - f0;
            double s0 = 0.0, s1 = 0.0;
            for (int c = 0; c < channels; c++) {
                s0 += pcm[f0 * channels + c];
                s1 += pcm[f1 * channels + c];
            }
            sum = (s0 * (1.0 - t) + s1 * t) / channels;
            weight = 1.0;
        }
        double v = weight > 0.0 ? sum / weight : 0.0;
        if (v > 32767.0) v = 32767.0;
        if (v < -32768.0) v = -32768.0;
        out[i] = (int16_t) lrint(v);
    }

    *outCount = count;
    return out;
}

static uint32_t encodeSample(UtAdpcmState *state, int16_t sample) {
    int32_t step = utAdpcmStepTable[state->stepIndex];
    int32_t diff = sample - state->predictor;
    uint32_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    if (diff >= step) { code |= 4; diff -= step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; }
    if (diff >= step >> 2) { code |= 1; }
    utAdpcmDecode(state, code);
    return code;
}

/* One pending conversion: either an .ogg path or an embedded blob. */
typedef struct {
    char name[UT_MUSIC_NAME_LEN];
    char path[2048];
    const uint8_t *blob;
    uint32_t blobLen;
} Source;

static int compareSources(const void *a, const void *b) {
    return strncmp(((const Source *) a)->name, ((const Source *) b)->name, UT_MUSIC_NAME_LEN);
}

static bool setName(Source *source, const char *text, size_t len) {
    if (len == 0 || len >= UT_MUSIC_NAME_LEN) return false;
    memset(source->name, 0, UT_MUSIC_NAME_LEN);
    for (size_t i = 0; i < len; i++) source->name[i] = (char) tolower((unsigned char) text[i]);
    return true;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <data.win> <dir of .ogg files> <music.bin> [sample rate]\n", argv[0]);
        return 2;
    }
    int outRate = argc > 4 ? atoi(argv[4]) : 32000;

    Source *sources = NULL;
    uint32_t count = 0;

    /* External .ogg files. */
    DIR *dir = opendir(argv[2]);
    if (!dir) { perror(argv[2]); return 1; }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 5 || strcmp(entry->d_name + len - 4, ".ogg") != 0) continue;
        sources = realloc(sources, (count + 1) * sizeof(Source));
        memset(&sources[count], 0, sizeof(Source));
        if (!setName(&sources[count], entry->d_name, len - 4)) {
            fprintf(stderr, "skipping %s: name too long\n", entry->d_name);
            continue;
        }
        snprintf(sources[count].path, sizeof(sources[count].path), "%s/%s", argv[2], entry->d_name);
        count++;
    }
    closedir(dir);
    uint32_t externalCount = count;

    /* Sounds embedded in data.win: SOND names them, AUDO holds the data. */
    size_t winSize;
    uint8_t *win = readFile(argv[1], &winSize);
    uint32_t sondLen, audoLen;
    const uint8_t *sond = findChunk(win, winSize, "SOND", &sondLen);
    const uint8_t *audo = findChunk(win, winSize, "AUDO", &audoLen);
    if (sond && audo) {
        uint32_t soundCount = readU32(sond);
        uint32_t audioCount = readU32(audo);
        for (uint32_t i = 0; i < soundCount; i++) {
            const uint8_t *sound = win + readU32(sond + 4 + 4 * i);
            uint32_t flags = readU32(sound + 4);
            int32_t audioFile = (int32_t) readU32(sound + 32);
            if (!(flags & 3) || audioFile < 0 || (uint32_t) audioFile >= audioCount) continue;

            const char *name = (const char *) (win + readU32(sound));
            const uint8_t *audio = win + readU32(audo + 4 + 4 * (uint32_t) audioFile);

            sources = realloc(sources, (count + 1) * sizeof(Source));
            memset(&sources[count], 0, sizeof(Source));
            if (!setName(&sources[count], name, strlen(name))) continue;

            /* A streamed file of the same name wins. */
            bool duplicate = false;
            for (uint32_t j = 0; j < count && !duplicate; j++)
                duplicate = strncmp(sources[j].name, sources[count].name, UT_MUSIC_NAME_LEN) == 0;
            if (duplicate) continue;

            sources[count].blobLen = readU32(audio);
            sources[count].blob = audio + 4;
            count++;
        }
    } else {
        fprintf(stderr, "%s: no SOND/AUDO chunks, packing the .ogg files only\n", argv[1]);
    }
    if (count == 0) {
        fprintf(stderr, "nothing to pack\n");
        return 1;
    }
    qsort(sources, count, sizeof(Source), compareSources);

    FILE *out = fopen(argv[3], "wb");
    if (!out) { perror(argv[3]); return 1; }
    UtMusicTrack *tracks = calloc(count, sizeof(UtMusicTrack));
    uint32_t offset = 8 + count * (uint32_t) sizeof(UtMusicTrack);
    fseek(out, (long) offset, SEEK_SET);

    double seconds = 0.0, noise = 0.0, signal = 0.0;
    for (uint32_t t = 0; t < count; t++) {
        memcpy(tracks[t].name, sources[t].name, UT_MUSIC_NAME_LEN);
        tracks[t].offset = offset;
        tracks[t].sampleRate = (uint32_t) outRate;

        int channels = 0, rate = 0;
        short *pcm = NULL;
        int frames = sources[t].blob
            ? decodeEmbedded(sources[t].blob, sources[t].blobLen, &channels, &rate, &pcm)
            : stb_vorbis_decode_filename(sources[t].path, &channels, &rate, &pcm);
        if (frames <= 0) {
            fprintf(stderr, "%s: could not decode, writing an empty track\n", sources[t].name);
            continue;
        }

        uint32_t samples;
        int16_t *mono = toMono(pcm, frames, channels, rate, outRate, &samples);
        free(pcm);

        uint8_t *bytes = malloc(samples / 2 + 1);
        UtAdpcmState state = {0, 0};
        for (uint32_t i = 0; i < samples; i += 2) {
            uint32_t lo = encodeSample(&state, mono[i]);
            double e0 = (double) mono[i] - state.predictor;
            uint32_t hi = encodeSample(&state, mono[i + 1]);
            double e1 = (double) mono[i + 1] - state.predictor;
            bytes[i / 2] = (uint8_t) (lo | (hi << 4));
            noise += e0 * e0 + e1 * e1;
            signal += (double) mono[i] * mono[i] + (double) mono[i + 1] * mono[i + 1];
        }
        fwrite(bytes, 1, samples / 2, out);

        tracks[t].sampleCount = samples;
        offset += samples / 2;
        seconds += (double) samples / outRate;

        free(bytes);
        free(mono);
    }

    fseek(out, 0, SEEK_SET);
    fwrite(UT_MUSIC_MAGIC, 1, 4, out);
    fwrite(&count, sizeof(count), 1, out);
    fwrite(tracks, sizeof(UtMusicTrack), count, out);
    fclose(out);

    printf("%s: %u tracks (%u streamed files, %u embedded sounds), %.0f min at %d Hz mono, %.1f MB, ADPCM SNR %.1f dB\n",
           argv[3], (unsigned) count, (unsigned) externalCount, (unsigned) (count - externalCount), seconds / 60.0,
           outRate, (double) offset / 1048576.0, noise > 0.0 ? 10.0 * log10(signal / noise) : 99.0);
    return 0;
}
