/* Positional Pocket prompts. Pixels are shared by game text, the port menu,
 * and the graphical attack prompt. No game textures or files are modified. */
#include "of_buttons.h"
#include "of_hooks.h"
#include "runner_keyboard.h"
#include "sw_renderer_private.h"
#include "text_utils.h"

bool ofButtonCodepoint(uint16_t ch) {
    return ch >= OF_ICON_FIRST && ch < OF_ICON_FIRST + OF_ICON_COUNT;
}

static bool circle(int x, int y, int cx, int cy, bool filled) {
    int dx = abs(x - cx), dy = abs(y - cy);
    if (dx > 2 || dy > 2 || (dx == 2 && dy == 2)) return false;
    return filled || dx == 2 || dy == 2;
}

bool ofButtonPixel(uint16_t ch, int x, int y) {
    if (!ofButtonCodepoint(ch) || x < 0 || x >= OF_ICON_SIZE || y < 0 || y >= OF_ICON_SIZE) return false;
    unsigned icon = ch - OF_ICON_FIRST;
    if (icon < 4) {
        return circle(x, y, 10, 6, icon == 0) || circle(x, y, 6, 10, icon == 1) ||
               circle(x, y, 6, 2, icon == 2) || circle(x, y, 2, 6, icon == 3);
    }
    if (icon == 4 || icon == 5) {
        /* Three small circular buttons below the screen: Select, Pocket
         * menu, Start. Only the active outer button is filled. The middle
         * button is positional context, not an app input. */
        int cx = x < 4 ? 1 : x < 9 ? 6 : 11;
        bool filled = (icon == 4 && cx == 11) || (icon == 5 && cx == 1);
        int distance = abs(x - cx) + abs(y - 7);
        return distance == 1 || (distance == 0 && filled);
    }
    if (icon == 6 || icon == 7) {
        /* Shoulder pair, seen from above. */
        bool left = x <= 5;
        int xx = left ? x : x - 7;
        return xx >= 0 && xx <= 5 && y >= 2 && y <= 5 &&
               (((icon == 6) == left) || xx == 0 || xx == 5 || y == 2 || y == 5);
    }
    /* Cross-shaped D-pad, with one arm filled for directional prompts. */
    bool cross = (x >= 4 && x <= 8) || (y >= 4 && y <= 8);
    if (!cross) return false;
    bool edge = x == 0 || x == 12 || y == 0 || y == 12 ||
                ((x == 4 || x == 8) && (y < 4 || y > 8)) ||
                ((y == 4 || y == 8) && (x < 4 || x > 8));
    return edge || (icon == 9 && y < 4) || (icon == 10 && y > 8) ||
           (icon == 11 && x < 4) || (icon == 12 && x > 8);
}

const char *ofButtonForKey(int32_t key) {
    switch (key) {
        case 'Z': return OF_ICON_A " ";
        case 'X': case VK_SHIFT: return OF_ICON_B " ";
        case 'C': case VK_CONTROL: return OF_ICON_X " ";
        case VK_ENTER: return OF_ICON_START " ";
        case VK_UP: return OF_ICON_UP " ";
        case VK_DOWN: return OF_ICON_DOWN " ";
        case VK_LEFT: return OF_ICON_LEFT " ";
        case VK_RIGHT: return OF_ICON_RIGHT " ";
        default: return NULL;
    }
}

void ofButtonsInstallFonts(DataWin *dw) {
    for (uint32_t f = 0; f < dw->font.count; f++) {
        Font *font = &dw->font.fonts[f];
        if (!font->present || font->isSpriteFont || TextUtils_findGlyph(font, OF_ICON_FIRST) != NULL) continue;
        /* Deltarune's fonts store doubled pixels. Keep the icon on that grid. */
        int step = font->maxGlyphHeight >= 26 ? 2 : 1;
        uint32_t oldCount = font->glyphCount;
        font->glyphs = safeRealloc(font->glyphs, (oldCount + OF_ICON_COUNT) * sizeof(FontGlyph));
        memset(font->glyphs + oldCount, 0, OF_ICON_COUNT * sizeof(FontGlyph));
        for (uint32_t i = 0; i < OF_ICON_COUNT; i++) {
            FontGlyph *glyph = &font->glyphs[oldCount + i];
            glyph->character = (uint16_t)(OF_ICON_FIRST + i);
            glyph->sourceWidth = glyph->sourceHeight = (uint16_t)(OF_ICON_SIZE * step);
            glyph->shift = (int16_t)((OF_ICON_SIZE + 1) * step);
        }
        font->glyphCount += OF_ICON_COUNT;
        /* Reallocation invalidates all of the ASCII pointers. */
        Font_buildGlyphLUT(font);
    }
}

void *platformButtonGlyphTexture(uint16_t ch) {
    static SWTexture textures[OF_ICON_COUNT];
    static uintpixel_t pixels[OF_ICON_COUNT][OF_ICON_SIZE * OF_ICON_SIZE];
    static bool ready = false;
    if (!ofButtonCodepoint(ch)) return NULL;
    if (!ready) {
        for (unsigned i = 0; i < OF_ICON_COUNT; i++) {
            textures[i].buffer = pixels[i];
            textures[i].width = textures[i].height = OF_ICON_SIZE;
            for (int y = 0; y < OF_ICON_SIZE; y++)
                for (int x = 0; x < OF_ICON_SIZE; x++)
                    pixels[i][y * OF_ICON_SIZE + x] = ofButtonPixel((uint16_t)(OF_ICON_FIRST + i), x, y) ?
                        swrConvertPixelTexture(0xffffffffu) : 0;
        }
        ready = true;
    }
    return &textures[ch - OF_ICON_FIRST];
}

void ofButtonDraw(Renderer *renderer, uint16_t ch, float x, float y, int size, uint32_t color, float alpha) {
    SWRenderer *swr = (SWRenderer *)renderer;
    if (SWR_SKIPPED(swr)) return;
    SWTexture *texture = platformButtonGlyphTexture(ch);
    if (texture == NULL) return;
    swrOverlayFlush(swr);
    swrDrawSprite(renderer, x, y, (float)size, (float)size, texture, 0, 0, OF_ICON_SIZE, OF_ICON_SIZE, color, alpha);
}
