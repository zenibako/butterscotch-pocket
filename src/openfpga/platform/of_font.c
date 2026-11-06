/*
 * The game's own font for the port's screens; see of_font.h.
 *
 * GameMaker keeps a font as one picture on a texture page plus, for each
 * character, where its glyph sits in that picture and how far to move on
 * after it. The first time text is drawn, the printable ASCII glyphs of one
 * of the game's fonts are copied out of the page into a bitmap of one byte
 * per pixel, and drawn from there.
 *
 * Which font: the first of g_fontNames the game has, else the first plain
 * (not sprite) font with a capital A. Undertale's fonts are drawn at 320x240;
 * Deltarune's are drawn at 640x480 with every pixel doubled, so a font whose
 * glyphs are all made of 2x2 blocks is copied at half size. Either way the
 * copy is at 320x240 size, and ofFontScale doubles it for 640x480 frames.
 */

#include "of_font.h"
#include "of_buttons.h"

#include "data_win.h"
#include "log.h"
#include "of_perf.h"
#include "runner.h"
#include "sw_renderer_private.h"
#include "sw_texture_lru.h"
#include "text_utils.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define OF_FONT_FIRST ' '
#define OF_FONT_LAST  '~'
#define OF_FONT_COUNT (OF_FONT_LAST - OF_FONT_FIRST + 1)
/* A font taller than this at 320x240 would leave too few menu rows. */
#define OF_FONT_MAX_HEIGHT 24
#define OF_FONT_MIN_HEIGHT 6

/* Undertale's and Deltarune's dialogue font comes first. */
static const char *const g_fontNames[] = { "fnt_main", "fnt_maintext", "fnt_small" };

typedef struct {
    uint8_t w, h;
    int8_t offset;  /* from the pen position to the glyph's left edge */
    int8_t shift;   /* how far the pen moves on */
    uint32_t start; /* first pixel in g_ink */
} OfGlyph;

static Runner *g_runner = NULL;
static bool g_ready = false;    /* glyphs copied */
static bool g_gaveUp = false;   /* the game has no font that fits: use the log font */
static OfGlyph g_glyphs[OF_FONT_COUNT];
static uint8_t *g_ink = NULL;   /* 1 where a glyph pixel is drawn */
static int g_lineHeight = 0;

void ofFontSetRunner(Runner *runner) {
    g_runner = runner;
}

static bool usable(const Font *font) {
    if (!font->present || font->isSpriteFont || font->tpagIndex < 0 || font->glyphCount == 0) return false;
    return TextUtils_findGlyph((Font *) font, 'A') != NULL;
}

static Font *chooseFont(DataWin *dw) {
    for (size_t n = 0; n < sizeof(g_fontNames) / sizeof(g_fontNames[0]); n++) {
        for (uint32_t i = 0; i < dw->font.count; i++) {
            Font *font = &dw->font.fonts[i];
            if (font->name != NULL && strcmp(font->name, g_fontNames[n]) == 0 && usable(font)) return font;
        }
    }
    for (uint32_t i = 0; i < dw->font.count; i++) {
        if (usable(&dw->font.fonts[i])) return &dw->font.fonts[i];
    }
    return NULL;
}

/* Ink at (x, y) of a glyph's picture on the page, or false outside the texture. */
static bool inkAt(const SWTexture *texture, int pageX, int pageY) {
    int x = pageX - texture->originX;
    int y = pageY - texture->originY;
    if (x < 0 || y < 0 || x >= texture->width || y >= texture->height) return false;
    return (texture->buffer[y * texture->width + x] & TRANSPARENT_MASK) != 0;
}

/* Whether every glyph is drawn in 2x2 blocks: a font made for 640x480 with
 * its pixels doubled. */
static bool doubledPixels(const Font *font, const TexturePageItem *tpag, const SWTexture *texture) {
    bool anyInk = false;
    for (int c = OF_FONT_FIRST; c <= OF_FONT_LAST; c++) {
        const FontGlyph *glyph = TextUtils_findGlyph((Font *) font, (uint16_t) c);
        if (glyph == NULL) continue;
        int left = tpag->sourceX + glyph->sourceX, top = tpag->sourceY + glyph->sourceY;
        for (int y = 0; y < glyph->sourceHeight; y++) {
            for (int x = 0; x < glyph->sourceWidth; x++) {
                bool ink = inkAt(texture, left + x, top + y);
                anyInk |= ink;
                if (ink != inkAt(texture, left + (x & ~1), top + (y & ~1))) return false;
            }
        }
    }
    return anyInk;
}

static bool copyGlyphs(void) {
    if (g_runner == NULL || g_runner->renderer == NULL) return false;
    DataWin *dw = g_runner->dataWin;
    Font *font = chooseFont(dw);
    if (font == NULL) {
        logInfo("Font: the game has no font to use; the port's screens use the log font\n");
        g_gaveUp = true;
        return false;
    }
    /* A texture that cannot be read now (no memory, a busy card) is tried
     * again on the next draw. */
    SWTexture *texture = swrTextureForItem((SWRenderer *) g_runner->renderer, font->tpagIndex);
    if (texture == NULL) return false;
    const TexturePageItem *tpag = &dw->tpag.items[font->tpagIndex];

    int step = doubledPixels(font, tpag, texture) ? 2 : 1;
    int lineHeight = (int) font->maxGlyphHeight / step;
    if (lineHeight < OF_FONT_MIN_HEIGHT || lineHeight > OF_FONT_MAX_HEIGHT) {
        logInfo("Font: %s is %d px high; the port's screens use the log font\n", font->name, lineHeight);
        g_gaveUp = true;
        return false;
    }

    size_t total = 0;
    for (int c = OF_FONT_FIRST; c <= OF_FONT_LAST; c++) {
        const FontGlyph *glyph = TextUtils_findGlyph(font, (uint16_t) c);
        if (glyph != NULL) total += (size_t) (glyph->sourceWidth / step) * (size_t) (glyph->sourceHeight / step);
    }
    uint8_t *ink = (uint8_t *) malloc(total > 0 ? total : 1);
    if (ink == NULL) return false;

    uint32_t at = 0;
    const FontGlyph *space = TextUtils_findGlyph(font, ' ');
    for (int c = OF_FONT_FIRST; c <= OF_FONT_LAST; c++) {
        OfGlyph *out = &g_glyphs[c - OF_FONT_FIRST];
        const FontGlyph *glyph = TextUtils_findGlyph(font, (uint16_t) c);
        if (glyph == NULL) glyph = TextUtils_findGlyph(font, '?');
        if (glyph == NULL) {
            /* Nothing to draw: move on by a space. */
            memset(out, 0, sizeof(*out));
            out->shift = (int8_t) (space != NULL ? space->shift / step : lineHeight / 2);
            continue;
        }
        out->w = (uint8_t) (glyph->sourceWidth / step);
        out->h = (uint8_t) (glyph->sourceHeight / step);
        out->offset = (int8_t) (glyph->offset / step);
        out->shift = (int8_t) (glyph->shift / step);
        out->start = at;
        int left = tpag->sourceX + glyph->sourceX, top = tpag->sourceY + glyph->sourceY;
        for (int y = 0; y < out->h; y++)
            for (int x = 0; x < out->w; x++) ink[at++] = inkAt(texture, left + x * step, top + y * step);
    }

    g_ink = ink;
    g_lineHeight = lineHeight;
    g_ready = true;
    logInfo("Font: %s, %d px lines%s\n", font->name, lineHeight, step == 2 ? " (drawn doubled by the game)" : "");
    return true;
}

static bool ready(void) {
    if (g_ready) return true;
    if (g_gaveUp) return false;
    return copyGlyphs();
}

bool ofFontReady(void) {
    return g_runner != NULL && ready();
}

int ofFontScale(int frameWidth) {
    return frameWidth >= 640 ? 2 : 1;
}

int ofFontLineHeight(int scale) {
    return ready() ? g_lineHeight * scale : OF_LOG_CELL_H;
}

int ofFontWidth(const char *text, int scale) {
    if (!ready()) return (int) strlen(text) * OF_LOG_CELL_W;
    int width = 0;
    int32_t pos = 0, len = (int32_t)strlen(text);
    while (pos < len) {
        int c = TextUtils_decodeUtf8(text, len, &pos);
        if (ofButtonCodepoint((uint16_t)c)) {
            width += (OF_ICON_SIZE + 1) * scale;
            continue;
        }
        if (c < OF_FONT_FIRST || c > OF_FONT_LAST) c = '?';
        width += g_glyphs[c - OF_FONT_FIRST].shift * scale;
    }
    return width;
}

int ofFontDraw(uint16_t *fb, int width, int height, int x, int y, const char *text, uint16_t color, int scale) {
    if (!ready()) return ofPerfDrawLogText(fb, width, height, x, y, text, color);
    int32_t pos = 0, len = (int32_t)strlen(text);
    while (pos < len) {
        int c = TextUtils_decodeUtf8(text, len, &pos);
        if (ofButtonCodepoint((uint16_t)c)) {
            for (int iy = 0; iy < OF_ICON_SIZE * scale; iy++) {
                for (int ix = 0; ix < OF_ICON_SIZE * scale; ix++) {
                    int px = x + ix, py = y + iy;
                    if (px >= 0 && px < width && py >= 0 && py < height &&
                        ofButtonPixel((uint16_t)c, ix / scale, iy / scale))
                        fb[py * width + px] = color;
                }
            }
            x += (OF_ICON_SIZE + 1) * scale;
            continue;
        }
        if (c < OF_FONT_FIRST || c > OF_FONT_LAST) c = '?';
        const OfGlyph *glyph = &g_glyphs[c - OF_FONT_FIRST];
        const uint8_t *ink = g_ink + glyph->start;
        int left = x + glyph->offset * scale;
        for (int gy = 0; gy < glyph->h; gy++) {
            for (int gx = 0; gx < glyph->w; gx++) {
                if (!ink[gy * glyph->w + gx]) continue;
                for (int sy = 0; sy < scale; sy++) {
                    int py = y + gy * scale + sy;
                    if (py < 0 || py >= height) continue;
                    for (int sx = 0; sx < scale; sx++) {
                        int px = left + gx * scale + sx;
                        if (px >= 0 && px < width) fb[py * width + px] = color;
                    }
                }
            }
        }
        x += glyph->shift * scale;
    }
    return x;
}
