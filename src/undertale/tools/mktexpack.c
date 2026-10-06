/*
 * mktexpack — build a pre-converted texture pack from a GameMaker data.win.
 *
 *   mktexpack data.win textures.bin
 *
 * Decodes every TXTR page with Butterscotch's own image decoder, converts it
 * with the software renderer's own 16-bit pixel conversion (so the result is
 * exactly what the renderer would have produced on the device), run-length
 * encodes it and writes the pack format described in sw_texture_pack.h.
 *
 * Host tool only; assumes a little-endian machine.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_decoder.h"
#include "sw_texture_pack.h"

typedef struct {
    uint16_t width, height;
    uint32_t offset, size;
} PackEntry;

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

/* Finds a chunk in the FORM container; returns its payload and length. */
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

/* Run-length encodes `count` pixels into `out` (worst case count + count/32768 + 1 words). */
static size_t encodePage(const uint16_t *pixels, size_t count, uint16_t *out) {
    size_t n = 0;
    size_t i = 0;
    while (i < count) {
        size_t run = 1;
        while (i + run < count && pixels[i + run] == pixels[i] && run < SW_TEXTURE_PACK_MAX_COUNT) run++;

        if (run >= 3) {
            out[n++] = (uint16_t) (SW_TEXTURE_PACK_RUN_FLAG | (run - 1));
            out[n++] = pixels[i];
            i += run;
            continue;
        }

        /* Literal span: extend until the next run of three or more. */
        size_t start = i;
        while (i < count && i - start < SW_TEXTURE_PACK_MAX_COUNT) {
            if (i + 2 < count && pixels[i] == pixels[i + 1] && pixels[i] == pixels[i + 2]) break;
            i++;
        }
        out[n++] = (uint16_t) (i - start - 1);
        memcpy(out + n, pixels + start, (i - start) * sizeof(uint16_t));
        n += i - start;
    }
    return n;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <data.win> <textures.bin>\n", argv[0]);
        return 2;
    }

    size_t size;
    uint8_t *data = readFile(argv[1], &size);
    if (size < 16 || memcmp(data, "FORM", 4) != 0) {
        fprintf(stderr, "%s: not a GameMaker data file\n", argv[1]);
        return 1;
    }

    uint32_t txtrLen;
    const uint8_t *txtr = findChunk(data, size, "TXTR", &txtrLen);
    if (!txtr) {
        fprintf(stderr, "%s: no TXTR chunk\n", argv[1]);
        return 1;
    }
    const uint8_t *txtrEnd = txtr + txtrLen;

    /* WAD 16 layout: count, then pointers to { uint32 scaled; uint32 blobOffset; }. */
    uint32_t count = readU32(txtr);
    uint32_t *blobOffsets = calloc(count, sizeof(uint32_t));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = readU32(txtr + 4 + 4 * i);
        blobOffsets[i] = readU32(data + entry + 4);
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }

    PackEntry *entries = calloc(count, sizeof(PackEntry));
    uint32_t dataStart = 8 + count * (uint32_t) sizeof(PackEntry);
    fseek(out, (long) dataStart, SEEK_SET);

    uint32_t offset = dataStart;
    size_t rawTotal = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (blobOffsets[i] == 0) continue;

        /* A blob runs to the next blob, or to the end of the chunk. */
        const uint8_t *blob = data + blobOffsets[i];
        const uint8_t *blobEnd = txtrEnd;
        for (uint32_t j = 0; j < count; j++) {
            const uint8_t *other = data + blobOffsets[j];
            if (blobOffsets[j] != 0 && other > blob && other < blobEnd) blobEnd = other;
        }

        int w, h;
        uint8_t *rgba = ImageDecoder_decodeToRgba(blob, (size_t) (blobEnd - blob), false, &w, &h);
        if (!rgba) {
            fprintf(stderr, "page %u: decode failed\n", i);
            return 1;
        }
        if (w > 0xFFFF || h > 0xFFFF) {
            fprintf(stderr, "page %u: %dx%d is too large\n", i, w, h);
            return 1;
        }

        size_t pixelCount = (size_t) w * (size_t) h;
        uint16_t *pixels = malloc(pixelCount * sizeof(uint16_t));
        const uint32_t *src = (const uint32_t *) rgba;
        for (size_t p = 0; p < pixelCount; p++) pixels[p] = swrConvertPixelTexture(src[p]);

        uint16_t *encoded = malloc((pixelCount + pixelCount / SW_TEXTURE_PACK_MAX_COUNT + 2) * sizeof(uint16_t));
        size_t words = encodePage(pixels, pixelCount, encoded);
        fwrite(encoded, sizeof(uint16_t), words, out);

        entries[i].width = (uint16_t) w;
        entries[i].height = (uint16_t) h;
        entries[i].offset = offset;
        entries[i].size = (uint32_t) (words * sizeof(uint16_t));
        offset += entries[i].size;
        rawTotal += pixelCount * sizeof(uint16_t);

        free(encoded);
        free(pixels);
        free(rgba);
    }

    fseek(out, 0, SEEK_SET);
    fwrite(SW_TEXTURE_PACK_MAGIC, 1, 4, out);
    fwrite(&count, sizeof(count), 1, out);
    fwrite(entries, sizeof(PackEntry), count, out);
    fclose(out);

    printf("%s: %u pages, %.1f MB raw -> %.1f MB\n", argv[2], (unsigned) count,
           (double) rawTotal / 1048576.0, (double) offset / 1048576.0);
    return 0;
}
