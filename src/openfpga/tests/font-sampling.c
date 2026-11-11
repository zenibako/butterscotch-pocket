#include "sw_renderer_private.h"
#include "sw_drawing.h"
#include <stdio.h>

enum { SIZE = 16, GLYPH_SIZE = 8 };
static const uintpixel_t background = 0x8c63;
static unsigned checked;

/* One regular font on a cropped atlas item, without game files or a window. */
static void setup(SWRenderer* swr, DataWin* data, Font* font, FontGlyph* glyph,
                  TexturePageItem* item, SWTexture* texture, SWTexture** pages)
{
    memset(swr, 0, sizeof(*swr));
    swr->base.dataWin = data;
    swr->base.drawFont = 0;
    swr->width = swr->height = swr->mainWidth = swr->mainHeight = SIZE;
    swr->fbPitch = swr->mainPitch = SIZE;
    swr->portW = swr->portH = swr->maxX = swr->maxY = SIZE;
    swr->blendMode = bm_normal;
    swr->writeMask = WRITE_MASK_ALL;
    swr->scaleX = swr->scaleY = 0.5f;
    swr->fb = swr->mainFb = calloc(SIZE * SIZE, sizeof(uintpixel_t));
    if (swr->fb == NULL) abort();
    swr->totalTextureCount = 1;
    swr->textures = pages;
    data->font.count = data->tpag.count = 1;
    data->font.fonts = font;
    data->tpag.items = item;
    font->present = true;
    font->scaleX = font->scaleY = 1.0f;
    font->glyphCount = 1;
    font->glyphs = glyph;
    font->maxGlyphHeight = GLYPH_SIZE;
    glyph->character = 'A';
    glyph->sourceWidth = glyph->sourceHeight = glyph->shift = GLYPH_SIZE;
    Font_buildGlyphLUT(font);
    texture->width = texture->height = GLYPH_SIZE;
    texture->originX = item->sourceX = 5;
    texture->originY = item->sourceY = 7;
    item->sourceWidth = item->sourceHeight = GLYPH_SIZE;
    texture->immutable = true;
    texture->buffer = calloc(GLYPH_SIZE * GLYPH_SIZE, sizeof(uintpixel_t));
    if (texture->buffer == NULL) abort();
}

/* Doubled pixel art with transparent padding and independently shifted axes. */
static void makeGlyph(SWTexture* texture, int phaseX, int phaseY, bool doubled)
{
    memset(texture->buffer, 0, GLYPH_SIZE * GLYPH_SIZE * sizeof(uintpixel_t));
    const unsigned rows[] = { 7, 5, 7 };
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            if (rows[y] & (1u << x))
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        texture->buffer[(phaseY + y * 2 + dy) * GLYPH_SIZE + phaseX + x * 2 + dx] = 0xffff;
    if (!doubled) texture->buffer[phaseY * GLYPH_SIZE + phaseX] = 0x83e0;
}

/* Compare the whole frame with an ordinary sprite draw using the required filter. */
static bool check(int phaseX, int phaseY, bool doubled, bool favorSpeed, bool smooth,
                  bool warmHalf, bool immutable, bool spriteFont, float scale, float angle,
                  uint32_t color, float alpha, int left, int top, bool antialiased,
                  bool mixedFont, bool emptyFont)
{
    SWRenderer swr;
    DataWin data = { 0 };
    Font font = { 0 };
    FontGlyph glyphs[2] = { 0 };
    TexturePageItem item = { 0 };
    SWTexture texture = { 0 };
    SWTexture* pages[] = { &texture };
    Sprite sprite = { 0 };
    int32_t spriteItem = 0;
    uintpixel_t expected[SIZE * SIZE];
    setup(&swr, &data, &font, &glyphs[0], &item, &texture, pages);
    makeGlyph(&texture, phaseX, phaseY, doubled);
    if (mixedFont) {
        glyphs[1] = glyphs[0];
        glyphs[1].character = 'B';
        glyphs[1].sourceX = (uint16_t) phaseX;
        glyphs[1].sourceY = (uint16_t) phaseY;
        glyphs[1].sourceWidth = glyphs[1].sourceHeight = 1;
        font.glyphCount = 2;
        Font_buildGlyphLUT(&font);
    }
    if (emptyFont) memset(texture.buffer, 0, GLYPH_SIZE * GLYPH_SIZE * sizeof(uintpixel_t));
    texture.immutable = immutable;
    swr.scaleX = swr.scaleY = scale;
    font.isSpriteFont = spriteFont;
    font.antiAliasing = antialiased ? 1 : 0;
    if (spriteFont) {
        data.sprt.sprites = &sprite;
        data.sprt.count = sprite.textureCount = 1;
        sprite.tpagIndices = &spriteItem;
    }
    swrFavorSpeed = favorSpeed;
    swrSmoothMinify = smooth;
    if (warmHalf) {
        swrDrawSprite(&swr.base, 0, 0, GLYPH_SIZE, GLYPH_SIZE, &texture,
                      0, 0, GLYPH_SIZE, GLYPH_SIZE, 0xffffff, 1.0f);
        swrOverlayFlush(&swr);
    }
    uintpixel_t* halfBefore = texture.halfBuffer;
    swrFillPixels(swr.fb, SIZE * SIZE, background);
    swrDrawText(&swr, "A", (float) left, (float) top, 1, 1, angle,
                (int32_t) color, alpha, -1);
    swrOverlayFlush(&swr);
    memcpy(expected, swr.fb, sizeof(expected));
    bool keepPixels = doubled && immutable && !spriteFont && !antialiased && !mixedFont && !emptyFont &&
                      angle == 0 && scale == 0.5f && favorSpeed && smooth;
    if (keepPixels && texture.halfBuffer != halfBefore) {
        fprintf(stderr, "FAIL: font draw created or changed the atlas half cache\n");
        return false;
    }
    /* The reference uses the existing point sampler for doubled glyphs only. */
    swrSmoothMinify = smooth && !keepPixels;
    swrFavorSpeed = favorSpeed && !keepPixels;
    swrFillPixels(swr.fb, SIZE * SIZE, background);
    if (angle != 0)
        swrDrawSpriteRotated(&swr.base, (float) left, (float) top, GLYPH_SIZE, GLYPH_SIZE,
                             &texture, 0, 0, GLYPH_SIZE, GLYPH_SIZE, color, alpha, angle, 0, 0, false);
    else
        swrDrawSprite(&swr.base, (float) left, (float) top, GLYPH_SIZE, GLYPH_SIZE,
                      &texture, 0, 0, GLYPH_SIZE, GLYPH_SIZE, color, alpha);
    swrOverlayFlush(&swr);
    for (int i = 0; i < SIZE * SIZE; ++i) {
        if (expected[i] != swr.fb[i]) {
            fprintf(stderr, "FAIL: font phase %d/%d doubled %d speed %d smooth %d warm %d immutable %d sprite %d scale %.2f angle %.1f pixel %d,%d: %04x != %04x\n",
                    phaseX, phaseY, doubled, favorSpeed, smooth, warmHalf, immutable, spriteFont,
                    (double) scale, (double) angle, i % SIZE, i / SIZE, expected[i], swr.fb[i]);
            return false;
        }
    }
    /* A repeated draw uses the cached classification after the reference's mode change. */
    swrFavorSpeed = favorSpeed;
    swrSmoothMinify = smooth;
    swrFillPixels(swr.fb, SIZE * SIZE, background);
    swrDrawText(&swr, "A", (float) left, (float) top, 1, 1, angle,
                (int32_t) color, alpha, -1);
    swrOverlayFlush(&swr);
    if (memcmp(expected, swr.fb, sizeof(expected)) != 0) {
        fprintf(stderr, "FAIL: repeated font draw changed after a mode switch\n");
        return false;
    }
    free(swr.fb);
    free(texture.halfBuffer);
    free(texture.rowBounds);
    free(texture.buffer);
    ++checked;
    return true;
}

/* Odd rectangles and mirrored text retain the generic sprite sampler. */
static bool checkGeometry(bool odd, float xscale, float yscale, bool clipped, bool reload)
{
    SWRenderer swr;
    DataWin data = { 0 };
    Font font = { 0 };
    FontGlyph glyph = { 0 };
    TexturePageItem item = { 0 };
    SWTexture texture = { 0 };
    SWTexture* pages[] = { &texture };
    uintpixel_t expected[SIZE * SIZE];
    setup(&swr, &data, &font, &glyph, &item, &texture, pages);
    makeGlyph(&texture, 0, 1, true);
    if (odd) glyph.sourceWidth = glyph.sourceHeight = GLYPH_SIZE - 1;
    if (clipped) {
        swr.portX = swr.portY = 5;
        swr.portW = swr.portH = 2;
        swr.maxX = swr.maxY = 7;
    }
    float left = clipped ? (xscale < 0 ? 6 : -2) : 8;
    float top = clipped ? (yscale < 0 ? 6 : -2) : 8;
    swrFavorSpeed = swrSmoothMinify = true;
    swrFillPixels(swr.fb, SIZE * SIZE, background);
    swrDrawText(&swr, "A", left, top, xscale, yscale, 0, 0xffffff, 1, -1);
    swrOverlayFlush(&swr);
    memcpy(expected, swr.fb, sizeof(expected));
    if (clipped) {
        bool visible = false;
        for (int y = 0; y < SIZE; ++y)
            for (int x = 0; x < SIZE; ++x) {
                if (expected[y * SIZE + x] == background) continue;
                visible = true;
                if (x < swr.portX || x >= swr.maxX || y < swr.portY || y >= swr.maxY) {
                    fprintf(stderr, "FAIL: clipped font wrote outside its viewport\n");
                    return false;
                }
            }
        if (!visible) {
            fprintf(stderr, "FAIL: clipped font case drew no pixels\n");
            return false;
        }
    }
    if (reload) {
        free(texture.buffer);
        texture.buffer = calloc(GLYPH_SIZE * GLYPH_SIZE, sizeof(uintpixel_t));
        if (texture.buffer == NULL) abort();
        makeGlyph(&texture, 0, 1, true);
        swrFillPixels(swr.fb, SIZE * SIZE, background);
        swrDrawText(&swr, "A", left, top, xscale, yscale, 0, 0xffffff, 1, -1);
        swrOverlayFlush(&swr);
        if (memcmp(expected, swr.fb, sizeof(expected)) != 0) {
            fprintf(stderr, "FAIL: reloaded font texture changed the glyph\n");
            return false;
        }
    }
    bool point = !odd && xscale == 1 && yscale == 1;
    swrFavorSpeed = swrSmoothMinify = !point;
    swrFillPixels(swr.fb, SIZE * SIZE, background);
    swrDrawSprite(&swr.base, left, top, glyph.sourceWidth * xscale, glyph.sourceHeight * yscale,
                  &texture, 0, 0, glyph.sourceWidth, glyph.sourceHeight, 0xffffff, 1);
    swrOverlayFlush(&swr);
    if (memcmp(expected, swr.fb, sizeof(expected)) != 0) {
        fprintf(stderr, "FAIL: font geometry odd=%d scale=%g,%g clipped=%d reload=%d\n",
                odd, (double) xscale, (double) yscale, clipped, reload);
        return false;
    }
    free(swr.fb);
    free(texture.halfBuffer);
    free(texture.rowBounds);
    free(texture.buffer);
    ++checked;
    return true;
}

int main(void)
{
    if (!check(0, 1, true, true, true, true, true, false, 0.5f, 0,
               0xffffff, 1, 6, 6, false, false, false)) return 1;
    const float scales[] = { 0.5f, 1.0f, 0.75f };
    const int positions[] = { -2, 0, 6, 28 };
    for (int phase = 0; phase < 4; ++phase)
        for (int warm = 0; warm < 2; ++warm)
            for (unsigned p = 0; p < sizeof(positions) / sizeof(positions[0]); ++p)
                for (int a = 0; a < 2; ++a)
                    if (!check(phase & 1, phase >> 1, true, true, true, warm, true, false,
                               0.5f, 0, a ? 0xffff00 : 0xffffff, a ? 0.63f : 1.0f,
                               positions[p], positions[p], false, false, false)) return 1;
    for (unsigned s = 0; s < sizeof(scales) / sizeof(scales[0]); ++s)
        for (int mode = 0; mode < 4; ++mode)
            for (int variant = 0; variant < 7; ++variant)
                if (!check(0, 1, variant != 0, (mode & 1) != 0, (mode & 2) != 0,
                           false, variant != 1, variant == 2, scales[s], variant == 3 ? 12 : 0,
                           0xffffff, 1, 6, 6, variant == 4, variant == 5, variant == 6)) return 1;
    for (int odd = 0; odd < 2; ++odd)
        for (int flip = 0; flip < 4; ++flip)
            for (int clip = 0; clip < 2; ++clip)
                if (!checkGeometry(odd, flip & 1 ? -1 : 1, flip & 2 ? -1 : 1, clip, false)) return 1;
    if (!checkGeometry(false, 1, 1, false, true)) return 1;
    printf("PASS: %u font sampling cases preserve doubled pixels and existing filters\n", checked);
    return 0;
}
