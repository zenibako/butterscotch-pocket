#ifndef UT_MUSIC_PACK_H
#define UT_MUSIC_PACK_H

#include <stdint.h>

/*
 * music.bin — every external .ogg the game streams, converted offline to
 * mono IMA ADPCM so the device never has to decode Vorbis.
 *
 * Layout (little-endian):
 *   char     magic[4]   "UTM1"
 *   uint32_t trackCount
 *   trackCount x UtMusicTrack, sorted by name
 *   ADPCM data
 *
 * Each track is one continuous ADPCM stream, 4 bits per sample, low nibble
 * first, starting from predictor 0 / step index 0. sampleCount is always
 * even, so a track is exactly sampleCount / 2 bytes and looping restarts on
 * a byte boundary.
 */
#define UT_MUSIC_MAGIC "UTM1"
#define UT_MUSIC_NAME_LEN 32

typedef struct {
    char name[UT_MUSIC_NAME_LEN]; /* file name without extension, lower case, NUL padded */
    uint32_t offset;              /* absolute file offset of the ADPCM data */
    uint32_t sampleCount;
    uint32_t sampleRate;
    uint32_t reserved;
} UtMusicTrack;

static const int8_t utAdpcmIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int16_t utAdpcmStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};

typedef struct {
    int32_t predictor;
    int32_t stepIndex;
} UtAdpcmState;

/* Decodes one 4-bit code and returns the new sample. */
static inline int16_t utAdpcmDecode(UtAdpcmState *state, uint32_t code) {
    int32_t step = utAdpcmStepTable[state->stepIndex];
    int32_t diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    if (code & 8) diff = -diff;

    int32_t predictor = state->predictor + diff;
    if (predictor > 32767) predictor = 32767;
    if (predictor < -32768) predictor = -32768;
    state->predictor = predictor;

    int32_t index = state->stepIndex + utAdpcmIndexTable[code];
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    state->stepIndex = index;

    return (int16_t) predictor;
}

#endif /* UT_MUSIC_PACK_H */
