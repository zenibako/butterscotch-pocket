/* Compare the row fast path with the general per-pixel compositor, including
 * transparent destinations, odd alignment/length and clipped surface pitches. */
#include "sw/sw_drawing.c"
#include <stdio.h>

static unsigned long checks;
static bool negativeControl;

static void checkFloweyPipeBackdrop(void)
{
    SWTexture* pipe = swrCreateTextureEx(NULL, 192, 183, false);
    for (int y = 41; y < 43; ++y)
        for (int x = 1; x < 3; ++x)
            pipe->buffer[y * pipe->width + x] = 0xffff;
    SWTexture* art = swrMakeFloweyPipeBackdrop(pipe, 1, 1);
    if (art == NULL || art->width != 320 || !art->immutable ||
        !swrIsOpaque(art->buffer[10 * art->width + 200]) ||
        art->buffer[7 * art->width + 109] != art->buffer[10 * art->width + 200] ||
        !swrIsOpaque(art->buffer[20 * art->width + 229]) ||
        art->buffer[17 * art->width + 80] != art->buffer[20 * art->width + 229] ||
        art->buffer[20 * art->width + 229] == art->buffer[10 * art->width + 200] ||
        art->buffer[10 * art->width + 199] || art->buffer[21 * art->width + 229]) {
        fprintf(stderr, "FAIL: static Flowey pipe placement and masking\n");
        exit(1);
    }
    swrFreeTexture(art);
    swrFreeTexture(pipe);
}

static void checkFloweyArt(void)
{
    enum { WIDTH = 6, HEIGHT = 4 };
    const uintpixel_t colors[] = {0, 0xfc00, 0x83e0, 0x801f};
    SWTexture* texture = swrCreateTextureEx(NULL, 9, 5, false);
    texture->immutable = true;
    for (int y = 1; y < 5; ++y)
        for (int x = 1; x < 9; ++x)
            texture->buffer[y * 9 + x] = colors[(x - 1) / 2];
    uintpixel_t actual[WIDTH * HEIGHT] = {0};
    SWRenderer swr = {0};
    swr.fb = swr.mainFb = actual;
    swr.fbPitch = WIDTH;
    swr.portW = swr.maxX = WIDTH;
    swr.portH = swr.maxY = HEIGHT;
    swr.scaleX = swr.scaleY = 0.5f;
    swr.blendMode = bm_normal;
    if (!swrDrawFloweyArt(&swr.base, texture, 6, -2, -8, 4, 1, 1, 8, 4, SWR_FLOWEY_ART_ARM) ||
        !swrDrawFloweyArt(&swr.base, texture, 6, 2, 8, 4, 1, 1, 8, 4, SWR_FLOWEY_ART_ARM) ||
        !swrIsOpaque(actual[0]) || !swrIsOpaque(actual[1]) || actual[2] || actual[3] ||
        actual[WIDTH + 3] || actual[WIDTH + 4] != colors[1] ||
        actual[WIDTH + 5] != actual[0] || actual[3 * WIDTH + 5]) {
        fprintf(stderr, "FAIL: Flowey art opacity and mirrored clipping\n");
        exit(1);
    }
    checks += WIDTH * HEIGHT;
    swrFreeTexture(texture);
}

static void checkFloweyTvPalette(void)
{
    enum { WIDTH = 65, SOURCE_WIDTH = 90 };
    SWTexture* texture = swrCreateTextureEx(NULL, SOURCE_WIDTH, 2, false);
    texture->immutable = true;
    for (int i = 0; i < 2 * SOURCE_WIDTH; ++i)
        texture->buffer[i] = i % 3 ? 0x8000 : 0xffff;
    uintpixel_t actual[2 * WIDTH] = {0};
    SWRenderer swr = {0};
    swr.fb = swr.mainFb = actual;
    swr.fbPitch = swr.portW = swr.maxX = WIDTH;
    swr.portH = swr.maxY = 2;
    swr.scaleX = swr.scaleY = 0.5f;
    swr.blendMode = bm_normal;
    if (!swrDrawFloweyArt(&swr.base, texture, 0, 0, 130, 4, 0, 0, SOURCE_WIDTH, 2, SWR_FLOWEY_ART_SCREEN))
        exit(1);
    int step = (SOURCE_WIDTH << 14) / WIDTH;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            if (actual[y * WIDTH + x] == texture->buffer[y * SOURCE_WIDTH + ((x * step) >> 14)]) continue;
            fprintf(stderr, "FAIL: Flowey TV nearest-neighbor palette\n");
            exit(1);
        }
    }
    checks += 2 * WIDTH;
    swrFreeTexture(texture);
}

static void checkFloweyStalkHalf(void)
{
    enum { WIDTH = 4, HEIGHT = 3, TEX_WIDTH = 8, TEX_HEIGHT = 6 };
    const uintpixel_t colors[] = { 0xfc00, 0x83e0, 0x801f };
    SWTexture* texture = swrCreateTextureEx(NULL, TEX_WIDTH, TEX_HEIGHT, false);
    texture->immutable = true;
    for (int y = 1; y < 5; ++y)
        for (int x = 1; x < 7; ++x)
            texture->buffer[y * TEX_WIDTH + x] = colors[(x - 1) / 2];
    uintpixel_t actual[WIDTH * HEIGHT] = {0};
    SWRenderer swr = {0};
    swr.fb = swr.mainFb = actual;
    swr.fbPitch = WIDTH;
    swr.portW = swr.maxX = WIDTH;
    swr.portH = swr.maxY = HEIGHT;
    swr.scaleX = swr.scaleY = 0.5f;
    swr.blendMode = bm_normal;
    if (!swrDrawFloweyStalkHalf(&swr.base, texture, 4, -2, 1, 1, 6, 4, true) ||
        !swrDrawFloweyStalkHalf(&swr.base, texture, 4, 2, 1, 1, 6, 4, false) ||
        actual[0] != colors[1] || actual[1] != colors[0] || actual[2] || actual[3] ||
        actual[6] != colors[0] || actual[7] != colors[1] ||
        actual[10] != colors[0] || actual[11] != colors[1]) {
        fprintf(stderr, "FAIL: Flowey stalk half-size cache and clipping\n");
        exit(1);
    }
    checks += WIDTH * HEIGHT;
    swrFreeTexture(texture);
}

static void checkBackdropRows(void)
{
    enum { WIDTH = 112, HEIGHT = 140, SRC_W = 171, SRC_H = 275 };
    static uintpixel_t actual[WIDTH * HEIGHT], expected[WIDTH * HEIGHT];
    SWTexture* texture = swrCreateTextureEx(NULL, SRC_W + 2, SRC_H + 2, false);
    texture->immutable = true;
    for (int y = 0; y < SRC_H + 2; ++y) {
        for (int x = 0; x < SRC_W + 2; ++x) {
            if (x >= y % 9 && (x > 70 || (x + 2 * y) % 11 != 0))
                texture->buffer[y * (SRC_W + 2) + x] = (uintpixel_t) (0x8000 | ((x * 37 + y * 571) & 0x7fff));
        }
    }
    SWTexture* referenceTexture = swrCopyTexture(texture);
    SWRenderer batch = {0};
    batch.fb = batch.mainFb = actual;
    batch.fbPitch = WIDTH;
    batch.portW = batch.maxX = WIDTH;
    batch.portH = batch.maxY = HEIGHT;
    batch.blendMode = bm_normal;
    SWRenderer reference = batch;
    reference.fb = reference.mainFb = expected;
    swrFavorSpeed = swrSmoothMinify = true;
    const int placements[][4] = {
        {2, 3, 86, 129}, {38, 24, 86, 135}, {38, -8, 86, 130},
        {-3, 1, 86, 134}, {1, 2, 84, 134},
    };
    const void* cache = NULL;
    for (int i = 0; i < (int) (sizeof(placements) / sizeof(placements[0])); ++i) {
        int dx = placements[i][0], dy = placements[i][1], dw = placements[i][2], dh = placements[i][3];
        for (int n = 0; n < WIDTH * HEIGHT; ++n)
            actual[n] = expected[n] = (uintpixel_t) (0x8000 | ((n * 7919) & 0x7fff));
        swrDrawSpriteInternal(&batch.base, dx, dy, dw, dh, texture, 1, 1, SRC_W, SRC_H,
                              0xffff, 256, false, SWR_CONTRAST_NONE);
        swrDrawSpriteInternal(&reference.base, dx, dy, dw, dh, referenceTexture, 1, 1, SRC_W, SRC_H,
                              0xffff, 256, false, SWR_CONTRAST_NONE);
        if (i == 0) cache = texture->boxRows;
        if (cache == NULL || texture->boxRows != cache || memcmp(actual, expected, sizeof(actual))) {
            fprintf(stderr, "FAIL: backdrop box row draw %d\n", i);
            exit(1);
        }
        checks += WIDTH * HEIGHT;
    }
    swrFavorSpeed = swrSmoothMinify = false;
    swrFreeTexture(referenceTexture);
    swrFreeTexture(texture);
}

static void checkPipeLayers(void)
{
    enum { WIDTH = 200, HEIGHT = 156, SRC_W = 190, SRC_H = 181 };
    static uintpixel_t actual[WIDTH * HEIGHT], expected[WIDTH * HEIGHT];
    SWTexture* texture = swrCreateTextureEx(NULL, SRC_W, SRC_H, false);
    texture->immutable = true;
    for (int y = 0; y < SRC_H; ++y) {
        for (int x = 0; x < SRC_W; ++x) {
            if (x > 9 + y % 23 && x < SRC_W - 6 - y % 29 && (x + 2 * y) % 31 != 0)
                texture->buffer[y * SRC_W + x] = (uintpixel_t) (0x8000 | ((x * 57 + y * 277) & 0x7fff));
        }
    }
    for (int speed = 0; speed < 2; ++speed) {
        swrFavorSpeed = swrSmoothMinify = speed != 0;
        for (int flip = 0; flip < 2; ++flip) {
            for (int interrupt = 0; interrupt < 2; ++interrupt) {
                SWRenderer batch = {0};
                batch.fb = batch.mainFb = actual;
                batch.fbPitch = WIDTH;
                batch.portX = batch.viewX = 7;
                batch.portY = batch.viewY = 3;
                batch.maxX = WIDTH - 4;
                batch.maxY = HEIGHT - 6;
                batch.portW = batch.maxX - batch.portX;
                batch.portH = batch.maxY - batch.portY;
                batch.scaleX = batch.scaleY = 1.0f;
                batch.blendMode = bm_normal;
                batch.overlayMergeAllowed = true;
                SWRenderer reference = batch;
                reference.fb = reference.mainFb = expected;
                SWTexture* referenceTexture = swrCopyTexture(texture);
                referenceTexture->immutable = true;
                for (int i = 0; i < WIDTH * HEIGHT; ++i)
                    actual[i] = expected[i] = (uintpixel_t) (0x8000 | ((i * 7919) & 0x7fff));
                for (int layer = 0; layer < 3; ++layer) {
                    int w = (speed ? 95 : 190) + layer - 1;
                    int h = speed ? 91 : 181;
                    int x = (flip ? 73 + w - layer * 10 : -10 + layer * 11);
                    int y = -5 + layer * 7;
                    uint32_t tint = layer == 2 ? 0xffffff : 0x999999;
                    swrDrawSprite(&batch.base, x, y, flip ? -w : w, h, texture, 0, 0, SRC_W, SRC_H, tint, 1.0f);
                    swrDrawSpriteInternal(&reference.base, x, y, flip ? -w : w, h, referenceTexture,
                                          0, 0, SRC_W, SRC_H, swrConvertPixel(tint), 256, false, SWR_CONTRAST_NONE);
                    int pending = interrupt && layer == 2 ? 1 : (layer + 1) % 3;
                    if (batch.pipeCount != pending) exit(1);
                    if (interrupt && layer == 1) swrOverlayFlush(&batch);
                }
                swrOverlayFlush(&batch);
                swrOverlayFlush(&reference);
                if (memcmp(actual, expected, sizeof(actual))) {
                    fprintf(stderr, "FAIL: pipe composite speed=%d flip=%d interrupted=%d\n", speed, flip, interrupt);
                    exit(1);
                }
                checks += WIDTH * HEIGHT;
                swrFreeTexture(referenceTexture);
            }
        }
    }
    swrFavorSpeed = swrSmoothMinify = false;
    swrFreeTexture(texture);
}

static void checkBoxSampleSpan(void)
{
    enum { UNIT = 1 << 14, WIDTH = 96 };
    const uint16_t bounds[] = { 4, 25, 18, 52, 0, 0 };
    for (int a = 0; a < 3; ++a) {
        for (int rows = 1; rows <= 2; ++rows) {
            int b = rows == 2 && a < 2 ? a + 1 : -1;
            for (int left = 0; left < 54; left += 3) {
                for (int right = left; right < 55; right += 5) {
                    for (int step = 25000; step < 50000; step += 4223) {
                        int from = 0, to = WIDTH;
                        swrBoxSampleSpan(bounds, a, b, left, right, step, UNIT, &from, &to);
                        for (int x = 0; x < WIDTH; ++x) {
                            int col = left + x * step / UNIT;
                            bool touched = false;
                            for (int k = 0; k < 2; ++k) {
                                int row = k == 0 ? a : b;
                                if (row < 0) continue;
                                for (int tap = col; tap <= col + 1 && tap <= right; ++tap)
                                    touched |= tap >= bounds[2 * row] && tap < bounds[2 * row + 1];
                            }
                            if (touched && (x < from || x >= to)) {
                                fprintf(stderr, "FAIL: box sample excluded opaque tap at %d\n", x);
                                exit(1);
                            }
                            ++checks;
                        }
                    }
                }
            }
        }
    }
    int from = 0, to = WIDTH;
    swrBoxSampleSpan(bounds, 0, 1, 2, 95, UNIT * 2, UNIT, &from, &to);
    if (from != 1 || to != 25) exit(1);
}

static void checkPointSampleSpan(void)
{
    enum { UNIT = 1 << 14, WIDTH = 19 };
    for (int flip = 0; flip < 2; ++flip) {
        for (int left = 0; left < 62; left += 3) {
            for (int end = left; end <= 65; end += 5) {
                for (int step = UNIT; step <= 3 * UNIT; step += 4097) {
                    const uint16_t bounds[] = { (uint16_t) left, (uint16_t) end };
                    int from = 0, to = WIDTH;
                    swrPointSampleSpan(bounds, 7, WIDTH, step, UNIT, flip != 0, &from, &to);
                    for (int x = 0; x < WIDTH; ++x) {
                        int col = 7 + (flip ? WIDTH - 1 - x : x) * step / UNIT;
                        if ((col >= left && col < end) != (x >= from && x < to)) {
                            fprintf(stderr, "FAIL: point sample span flip=%d at %d\n", flip, x);
                            exit(1);
                        }
                        ++checks;
                    }
                }
            }
        }
    }
}

static void checkTranslatedRotation(int sourceWidth, int sourceHeight, bool eye)
{
    enum { WIDTH = 64, HEIGHT = 56 };
    int texWidth = sourceWidth + 2, texHeight = sourceHeight + 2;
    uintpixel_t source[47 * 42];
    uintpixel_t actual[WIDTH * HEIGHT], expected[WIDTH * HEIGHT];
    for (int i = 0; i < texWidth * texHeight; ++i)
        source[i] = (i % 7 < 3) ? 0 : (uintpixel_t) (0x8000 | ((i * 491) & 0x7fff));
    SWTexture texture = { .buffer = source, .width = (uint16_t) texWidth, .height = (uint16_t) texHeight, .immutable = true };
    SWRenderer swr = {0};
    swr.fb = swr.mainFb = actual;
    swr.fbPitch = WIDTH;
    swr.portX = 2;
    swr.portY = 3;
    swr.maxX = WIDTH - 2;
    swr.maxY = HEIGHT - 3;
    swr.portW = swr.maxX - swr.portX;
    swr.portH = swr.maxY - swr.portY;
    swr.viewX = swr.portX;
    swr.viewY = swr.portY;
    swr.scaleX = swr.scaleY = 1.0f;
    swr.blendMode = bm_normal;

    static const int places[][2] = { {18, 12}, {-9, 2}, {56, 45}, {3, -7} };
    for (int flip = 0; flip < 2; ++flip) {
        for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); ++i) {
            SWDirtyBuffer dirty = { .width = WIDTH, .height = HEIGHT };
            swr.dirtyMark = &dirty;
            for (int j = 0; j < WIDTH * HEIGHT; ++j)
                actual[j] = expected[j] = (uintpixel_t) (0x8000 | ((j * 7919) & 0x7fff));
            int w = eye ? 20 : flip ? 22 : 23, h = eye ? 20 : 16;
            int x = places[i][0] + (flip ? w : 0), y = places[i][1];
            int width = flip ? -w : w;
            uint32_t color = eye ? 0xffffff : 0x808080;
            float px = eye ? 10.0f : 10.5f, py = eye ? 10.0f : 8.0f;
            swr.fb = actual;
            swrDrawSpriteRotated(&swr.base, x, y, width, h, &texture, 1, 1, sourceWidth, sourceHeight,
                                 color, 1.0f, 37.0f, px, py, false);
            swr.fb = expected;
            swrDrawSpriteRotatedInternal(&swr.base, x, y, width, h, &texture, 1, 1, sourceWidth, sourceHeight,
                                         swrConvertPixel(color), 256, 37.0f, px, py, NULL);
            SWRotatedRasterCache* cache = eye ? texture.eyeRasters : texture.rotatedRasters;
            if (memcmp(actual, expected, sizeof(actual)) || cache == NULL || cache->count != flip + 1) {
                fprintf(stderr, "FAIL: translated raster flip=%d placement=%u\n", flip, i);
                exit(1);
            }
            for (int j = 0; j < WIDTH * HEIGHT; ++j) {
                uintpixel_t under = (uintpixel_t) (0x8000 | ((j * 7919) & 0x7fff));
                int row = (j / WIDTH) >> SWR_DIRTY_TILE_H_SHIFT;
                uint32_t tile = 1u << ((j % WIDTH) >> SWR_DIRTY_TILE_W_SHIFT);
                if (actual[j] != under && !(dirty.rows[row] & tile)) {
                    fprintf(stderr, "FAIL: translated raster missed dirty tile at pixel %d\n", j);
                    exit(1);
                }
            }
            checks += WIDTH * HEIGHT;
        }
    }
    swrFreeRotatedRasters(&texture);
    if (texture.rotatedRasters != NULL || texture.eyeRasters != NULL || swrRotatedRasterBytes != 0) exit(1);
}

static void checkStationaryVine(bool speedVine)
{
    enum { WIDTH = 112, HEIGHT = 96, SRC_W = 513, SRC_H = 35 };
    uintpixel_t source[SRC_W * SRC_H], actual[WIDTH * HEIGHT], expected[WIDTH * HEIGHT];
    for (int i = 0; i < SRC_W * SRC_H; ++i)
        source[i] = i % 9 < 3 ? 0 : (uintpixel_t) (0x8000 | ((i * 491) & 0x7fff));
    SWTexture texture = { .buffer = source, .width = SRC_W, .height = SRC_H, .immutable = true };
    SWRenderer swr = {0};
    swr.fbPitch = WIDTH;
    swr.scaleX = swr.scaleY = 1.0f;
    swr.blendMode = bm_normal;
    swr.portH = swr.maxY = HEIGHT;
    const uint32_t tints[] = {0xffffff, 0xffa0a0, 0xffcfcf, 0xffeaea};
    for (int frame = 0; frame < 4; ++frame) {
        swr.frameCounter = (uint32_t) frame;
        swr.portX = swr.viewX = frame < 2 ? 0 : 15;
        swr.maxX = WIDTH;
        swr.portW = WIDTH - swr.portX;
        for (int i = 0; i < WIDTH * HEIGHT; ++i)
            actual[i] = expected[i] = (uintpixel_t) (0x8000 | ((i * 7919) & 0x7fff));
        swr.fb = swr.mainFb = actual;
        uint32_t color = speedVine ? tints[frame] : 0xffffff;
        swrDrawSpriteRotated(&swr.base, -10, 70, 256, 17, &texture, 1, 1, 511, 33,
                             color, 1.0f, 37.2f, 6.5f, 5.5f, speedVine);
        swr.fb = expected;
        swrDrawSpriteRotatedInternal(&swr.base, -10, 70, 256, 17, &texture, 1, 1, 511, 33,
                                     swrConvertPixel(color), 256, 37.2f, 6.5f, 5.5f, NULL);
        if (memcmp(actual, expected, sizeof(actual)) || texture.stationaryRasters == NULL ||
            texture.stationaryRasters->count != 1 + frame / 2) {
            fprintf(stderr, "FAIL: stationary vine speed=%d frame=%d\n", speedVine, frame);
            exit(1);
        }
        checks += WIDTH * HEIGHT;
    }
    swrFreeRotatedRasters(&texture);
    if (texture.stationaryRasters != NULL || swrRotatedRasterBytes != 0) exit(1);
}

static void checkHalfTint(void)
{
    for (unsigned src = 0; src <= 0xffff; src++) {
        uintpixel_t expected = tint(0xc210, (uintpixel_t) src);
        if (swrTintHalf((uintpixel_t) src) != expected) {
            fprintf(stderr, "FAIL: half tint src=%x\n", src);
            exit(1);
        }
        ++checks;
    }
}

static void checkDoubledSprite(void)
{
    enum { SRC_PITCH = 9, DST_PITCH = 20, ROWS = 10 };
    uintpixel_t source[SRC_PITCH * ROWS];
    _Alignas(uint32_t) uintpixel_t actual[DST_PITCH * ROWS];
    uintpixel_t expected[DST_PITCH * ROWS];
    for (int i = 0; i < SRC_PITCH * ROWS; i++)
        source[i] = (uintpixel_t) ((i % 3 ? 0x8000 : 0) | ((i * 491) & 0x7fff));

    for (int width = 2; width <= 16; width += 2) {
        for (int height = 1; height <= 8; height++) {
            for (int flip = 0; flip < 2; flip++) {
                for (int i = 0; i < DST_PITCH * ROWS; i++)
                    actual[i] = expected[i] = (uintpixel_t) (i * 7919);
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        int col = flip ? width - 1 - x : x;
                        uintpixel_t texel = source[(1 + y / 2) * SRC_PITCH + 1 + col / 2];
                        if (swrIsOpaque(texel)) expected[(1 + y) * DST_PITCH + 2 + x] = texel;
                    }
                }
                swrCopyDoubledSprite(actual + DST_PITCH + 2, DST_PITCH, source + SRC_PITCH + 1,
                                     SRC_PITCH, width, height, flip != 0);
                if (memcmp(actual, expected, sizeof(actual))) {
                    fprintf(stderr, "FAIL: doubled sprite width=%d height=%d flip=%d\n", width, height, flip);
                    exit(1);
                }
                checks += DST_PITCH * ROWS;
            }
        }
    }
}

static void checkHalfBlend(void)
{
    // The packed average must never carry across channels or use the opacity bit.
    for (unsigned i = 0; i < 65536; i++) {
        for (unsigned c = 0; c < 32; c++) {
            uintpixel_t src = (uintpixel_t) (0x8000u | (c << 10) | ((31u - c) << 5) | c);
            uintpixel_t expected = (uintpixel_t) i;
            alphaBlend(&expected, src, bm_normal, 128);
            if (swrBlendHalf((uintpixel_t) i, src) != expected) {
                fprintf(stderr, "FAIL: half blend dst=%x src=%x\n", i, (unsigned) src);
                exit(1);
            }
            ++checks;
        }
    }
}

static void checkRow(int offset, int width, int alpha, uintpixel_t color, unsigned first)
{
    enum { COUNT = 260 };
    uintpixel_t actual[COUNT], expected[COUNT];
    for (unsigned i = 0; i < COUNT; i++)
        actual[i] = expected[i] = (uintpixel_t) (first + i);
    SWRenderer swr = {0};
    swr.fb = actual;
    swr.fbPitch = COUNT;
    swr.portX = offset;
    swr.maxX = offset + width;
    swr.maxY = 1;
    swr.blendMode = bm_normal;
    for (int i = offset; i < offset + width; i++)
        alphaBlend(&expected[i], color, bm_normal, alpha);
    swrDrawHLineInt(&swr.base, offset - 2, 0, width + 4, color, color, alpha);
    if (negativeControl) actual[offset] ^= 1;
    if (memcmp(actual, expected, sizeof(actual))) {
        fprintf(stderr, "FAIL: offset=%d width=%d alpha=%d color=%x first=%u\n",
                offset, width, alpha, color, first);
        exit(1);
    }
    checks += COUNT;
}

static void checkPitch(int pitch, int alpha, uintpixel_t color, int pattern)
{
    uintpixel_t actual[5 * 24], expected[5 * 24];
    for (unsigned i = 0; i < sizeof(actual) / sizeof(actual[0]); i++) {
        uintpixel_t under = pattern == 0 ? 0x8000 : pattern == 1 ? 0xffff :
                            pattern == 2 ? (i & 1 ? 0x7fff : 0x8421) : (uintpixel_t) (i * 7919);
        actual[i] = expected[i] = under;
    }
    SWRenderer swr = {0};
    swr.fb = actual;
    swr.fbPitch = (uint16_t) pitch;
    swr.portX = 2;
    swr.maxX = 21;
    swr.portY = 1;
    swr.maxY = 4;
    swr.blendMode = bm_normal;
    for (int y = 1; y < 4; y++)
        for (int x = 2; x < 21; x++)
            alphaBlend(&expected[y * pitch + x], color, bm_normal, alpha);
    for (int y = 0; y < 5; y++)
        swrDrawHLineInt(&swr.base, -1, y, 25, color, color, alpha);
    if (memcmp(actual, expected, sizeof(actual))) {
        fprintf(stderr, "FAIL: pitch=%d alpha=%d color=%x pattern=%d\n", pitch, alpha, color, pattern);
        exit(1);
    }
    checks += sizeof(actual) / sizeof(actual[0]);
}

int main(int argc, char** argv)
{
    (void) argv;
    checkFloweyPipeBackdrop();
    checkFloweyArt();
    checkFloweyTvPalette();
    checkFloweyStalkHalf();
    checkBackdropRows();
    checkPipeLayers();
    checkBoxSampleSpan();
    checkPointSampleSpan();
    checkTranslatedRotation(45, 31, false);
    checkTranslatedRotation(40, 40, true);
    checkStationaryVine(false);
    checkStationaryVine(true);
    checkHalfTint();
    checkDoubledSprite();
    checkHalfBlend();
    negativeControl = argc > 1;
    if (negativeControl) checkRow(1, 1, 133, 0x8000, 0);
    static const uintpixel_t colors[] = {0, 0x8000, 0xffff, 0xfc00, 0x83e0, 0x801f, 0x1234};
    /* Every 16-bit destination at every partial opacity, for each source. */
    for (unsigned c = 0; c < sizeof(colors) / sizeof(colors[0]); c++)
        for (int alpha = 4; alpha <= 253; alpha++)
            for (unsigned first = 0; first < 65536; first += 256)
                checkRow(2, 256, alpha, colors[c], first - 2);
    for (int offset = 1; offset <= 2; offset++)
        for (int width = 0; width < 20; width++)
            for (int alpha = 0; alpha <= 256; alpha++)
                checkRow(offset, width, alpha, 0x8d67, 0x7ff0);
    for (int pitch = 23; pitch <= 24; pitch++)
        for (int alpha = 0; alpha <= 256; alpha++)
            for (unsigned c = 0; c < sizeof(colors) / sizeof(colors[0]); c++)
                for (int pattern = 0; pattern < 4; pattern++)
                    checkPitch(pitch, alpha, colors[c], pattern);
    printf("solid-fill: %lu pixel/guard comparisons passed\n", checks);
    return 0;
}
