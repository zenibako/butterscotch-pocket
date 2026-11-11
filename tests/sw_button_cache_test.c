/* Exercises real texture derivatives with allocation failure and an independent
 * channel oracle. Removing reuse, the budget, or texture-owned cleanup breaks it. */
#include "sw_renderer_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

void platformLog(logType type, const char *format, va_list args) {
    (void) type;
    vfprintf(stderr, format, args);
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
/* This fixture creates only button derivatives; drawing-owned caches stay empty. */
void swrFreeRotatedRasters(SWTexture *texture) {
    CHECK(!texture->rotatedRasters && !texture->stationaryRasters &&
          !texture->eyeRasters && !texture->vineGrowthRows);
}
void swrFreeBoxRows(SWTexture *texture) {
    CHECK(!texture->boxRows);
}
static bool failAllocation;
static unsigned allocations;
static struct { void *pointer; size_t bytes; } live[64];
static size_t liveBytes;
static size_t cacheBudget(void);
static void* cacheMalloc(size_t bytes) {
    ++allocations;
    void *pointer = failAllocation ? NULL : malloc(bytes);
    if (pointer) {
        unsigned i;
        for (i = 0; i < 64 && live[i].pointer; ++i) {}
        CHECK(i < 64);
        live[i].pointer = pointer; live[i].bytes = bytes;
        liveBytes += bytes;
        CHECK(liveBytes <= cacheBudget());
    }
    return pointer;
}
static void cacheFree(void *pointer) {
    for (unsigned i = 0; i < 64; ++i) {
        if (pointer && live[i].pointer == pointer) {
            liveBytes -= live[i].bytes; live[i].pointer = NULL;
            break;
        }
    }
    free(pointer);
}
#define malloc cacheMalloc
#define free cacheFree
#include "../src/sw/sw_texture.c"
#undef malloc
#undef free
static size_t cacheBudget(void) { return SWR_BUTTON_CACHE_BYTES; }

#ifdef SW_HAS_PREMUL_BLEND
static void verify(const SWButtonCache *cache, SWTexture *texture, int sx, int sy, int w, int h) {
    CHECK(cache != NULL);
    for (int y = 0; y < (h + 1) / 2; ++y) {
        for (int x = 0; x < (w + 1) / 2; ++x) {
            unsigned r = 0, g = 0, b = 0, coverage = 0;
            int cx = x * 2 + 1 < w ? x * 2 + 1 : w - 1;
            int cy = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            for (int k = 0; k < 5; ++k) {
                int tx = k == 0 ? cx : x * 2 + (k - 1) % 2;
                int ty = k == 0 ? cy : y * 2 + (k - 1) / 2;
                if (tx >= w || ty >= h) continue;
                unsigned p = texture->buffer[(sy + ty) * texture->width + sx + tx];
                if (!(p & 0x8000)) continue;
                unsigned weight = k == 0 ? 4 : 1;
                r += ((p >> 10) & 31) * weight;
                g += ((p >> 5) & 31) * weight;
                b += (p & 31) * weight;
                coverage += weight;
            }
            const SWButtonPixel *pixel = &cache->pixels[y * ((w + 1) / 2) + x];
            CHECK(pixel->covered == coverage);
            CHECK(pixel->redBlue == (r * 32u * 65536u + b * 32u));
            CHECK(pixel->green == g * 32u);
            if (coverage) CHECK(pixel->keep == 256 - coverage * 32);
        }
    }
}

int main(void) {
    SWTexture *texture = swrCreateTextureEx(NULL, 40, 40, false);
    texture->immutable = true;
    for (int i = 0; i < 1600; ++i)
        texture->buffer[i] = (uintpixel_t) ((i % 5 ? 0x8000 : 0) | ((i * 7919u) & 0x7fff));
    uintpixel_t original[1600];
    memcpy(original, texture->buffer, sizeof(original));
    uintpixel_t *sourceBuffer = texture->buffer;
    failAllocation = true;
    CHECK(swrButtonCache(texture, 0, 0, 31, 32) == NULL);
    CHECK(!texture->buttonCache && !liveBytes);
    CHECK(texture->buffer == sourceBuffer && !memcmp(original, texture->buffer, sizeof(original)));
    failAllocation = false;
    for (int phase = 0; phase < 4; ++phase) {
        for (int edge = 0; edge < 4; ++edge) {
            int x = phase & 1, y = phase >> 1, w = 31 + (edge & 1), h = 31 + (edge >> 1);
            const SWButtonCache *cache = swrButtonCache(texture, x, y, w, h);
            verify(cache, texture, x, y, w, h);
            unsigned before = allocations;
            CHECK(swrButtonCache(texture, x, y, w, h) == cache);
            CHECK(allocations == before);
        }
        swrFreeButtonCache(texture);
    }
    CHECK(swrButtonCache(texture, -1, 0, 4, 4) == NULL);
    CHECK(swrButtonCache(texture, 39, 0, 4, 4) == NULL);
    SWTexture *wide = swrCreateTextureEx(NULL, SWR_BUTTON_CACHE_SIDE + 2, SWR_BUTTON_CACHE_SIDE + 2, false);
    wide->immutable = true;
    for (int i = 0; i < wide->width * wide->height; ++i)
        wide->buffer[i] = (uintpixel_t) ((i % 3 ? 0x8000 : 0) | ((i * 7919u) & 0x7fff));
    verify(swrButtonCache(wide, 1, 0, 110, 42), wide, 1, 0, 110, 42); /* Undertale's battle buttons */
    CHECK(swrButtonCache(wide, 0, 0, SWR_BUTTON_CACHE_SIDE + 1, 4) == NULL);
    CHECK(swrButtonCache(wide, 0, 0, 4, SWR_BUTTON_CACHE_SIDE + 1) == NULL);
    swrFreeTexture(wide);
    CHECK(swrButtonCache(texture, 0, -1, 4, 4) == NULL);
    CHECK(swrButtonCache(texture, 0, 39, 4, 4) == NULL);
    CHECK(swrButtonCache(texture, 0, 0, 4, 0) == NULL);
    texture->immutable = false;
    CHECK(swrButtonCache(texture, 0, 0, 4, 4) == NULL);
    texture->immutable = true;
    /* Distinct textures exhaust the global budget, then destruction makes room
     * for a cold rebuild. Copies must never inherit immutable derivatives. */
    SWTexture *holders[40] = {0};
    unsigned cached = 0;
    for (unsigned i = 0; i < 40; ++i) {
        holders[i] = swrCreateTextureEx(NULL, SWR_BUTTON_CACHE_SIDE, SWR_BUTTON_CACHE_SIDE, false);
        holders[i]->immutable = true;
        if (swrButtonCache(holders[i], 0, 0, SWR_BUTTON_CACHE_SIDE, SWR_BUTTON_CACHE_SIDE)) ++cached;
    }
    CHECK(cached > 0 && cached < 40);
    SWTexture *late = swrCreateTextureEx(NULL, SWR_BUTTON_CACHE_SIDE, SWR_BUTTON_CACHE_SIDE, false);
    late->immutable = true;
    unsigned before = allocations;
    CHECK(swrButtonCache(late, 0, 0, SWR_BUTTON_CACHE_SIDE, SWR_BUTTON_CACHE_SIDE) == NULL);
    CHECK(allocations == before);
    swrFreeTexture(late);
    CHECK(texture->buffer == sourceBuffer && !memcmp(original, texture->buffer, sizeof(original)));
    const SWButtonCache *warm = holders[0]->buttonCache;
    CHECK(warm && swrButtonCache(holders[0], 0, 0, SWR_BUTTON_CACHE_SIDE, SWR_BUTTON_CACHE_SIDE) == warm);
    CHECK(allocations == before);
    for (unsigned i = 0; i < 40; ++i) swrFreeTexture(holders[i]);
    verify(swrButtonCache(texture, 0, 0, 31, 32), texture, 0, 0, 31, 32);
    SWTexture *copy = swrCopyTexture(texture);
    CHECK(!copy->immutable && !copy->buttonCache);
    CHECK(copy->buffer != texture->buffer);
    copy->buffer[0] ^= 0x7fff;
    CHECK(swrButtonCache(copy, 0, 0, 31, 32) == NULL);
    CHECK(!memcmp(original, texture->buffer, sizeof(original)));
    swrFreeTexture(copy);
    swrFreeTexture(texture);
    CHECK(liveBytes == 0);
    puts("PASS: exact button intermediates, reuse, failure, bounds and eviction");
    return 0;
}

#else
int main(void) {
    SWTexture *texture = swrCreateTextureEx(NULL, 4, 4, false);
    texture->immutable = true;
    CHECK(swrButtonCache(texture, 0, 0, 4, 4) == NULL);
    CHECK(!texture->buttonCache && !allocations && !liveBytes);
    swrFreeTexture(texture);
    puts("PASS: unsupported format declines button cache without allocation");
    return 0;
}
#endif
