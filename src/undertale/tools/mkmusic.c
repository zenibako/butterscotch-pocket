/*
 * mkmusic — convert a directory of .ogg files into music.bin.
 *
 *   mkmusic <dir> <music.bin> [sample rate, default 32000]
 *
 * Each file is decoded with stb_vorbis, mixed down to mono, resampled and
 * encoded as IMA ADPCM. See platform/ut_music_pack.h for the format.
 *
 * Host tool only; assumes a little-endian machine.
 */

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/ut_music_pack.h"

int stb_vorbis_decode_filename(const char *filename, int *channels, int *sample_rate, short **output);

static int compareTracks(const void *a, const void *b) {
    return strncmp(((const UtMusicTrack *) a)->name, ((const UtMusicTrack *) b)->name, UT_MUSIC_NAME_LEN);
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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <dir> <music.bin> [sample rate]\n", argv[0]);
        return 2;
    }
    int outRate = argc > 3 ? atoi(argv[3]) : 32000;

    DIR *dir = opendir(argv[1]);
    if (!dir) { perror(argv[1]); return 1; }

    UtMusicTrack *tracks = NULL;
    uint32_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 5 || strcmp(entry->d_name + len - 4, ".ogg") != 0) continue;
        if (len - 4 >= UT_MUSIC_NAME_LEN) {
            fprintf(stderr, "skipping %s: name too long\n", entry->d_name);
            continue;
        }
        tracks = realloc(tracks, (count + 1) * sizeof(UtMusicTrack));
        memset(&tracks[count], 0, sizeof(UtMusicTrack));
        for (size_t i = 0; i < len - 4; i++) tracks[count].name[i] = (char) tolower((unsigned char) entry->d_name[i]);
        count++;
    }
    closedir(dir);
    if (count == 0) {
        fprintf(stderr, "%s: no .ogg files\n", argv[1]);
        return 1;
    }
    qsort(tracks, count, sizeof(UtMusicTrack), compareTracks);

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    uint32_t offset = 8 + count * (uint32_t) sizeof(UtMusicTrack);
    fseek(out, (long) offset, SEEK_SET);

    double seconds = 0.0, noise = 0.0, signal = 0.0;
    for (uint32_t t = 0; t < count; t++) {
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s.ogg", argv[1], tracks[t].name);

        int channels, rate;
        short *pcm;
        int frames = stb_vorbis_decode_filename(path, &channels, &rate, &pcm);
        if (frames <= 0) {
            fprintf(stderr, "%s: decode failed, writing an empty track\n", path);
            tracks[t].offset = offset;
            tracks[t].sampleRate = (uint32_t) outRate;
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

        tracks[t].offset = offset;
        tracks[t].sampleCount = samples;
        tracks[t].sampleRate = (uint32_t) outRate;
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

    printf("%s: %u tracks, %.0f min at %d Hz mono, %.1f MB, ADPCM SNR %.1f dB\n", argv[2], (unsigned) count,
           seconds / 60.0, outRate, (double) offset / 1048576.0,
           noise > 0.0 ? 10.0 * log10(signal / noise) : 99.0);
    return 0;
}
