/* Asset-free regression: keyboard labels become positional button glyphs. */
#include "of_game.h"
#include "of_strings.h"
#include "of_buttons.h"
#include "text_utils.h"
#include "sw_renderer_private.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
const OfGame ofGame = {0};
int main(void) {
    char confirm[] = "[Z]", cancel[] = "[X]", menu[] = "[C]";
    char name[] = "\\[C]", ordinary[] = "Z";
    char fullscreenKey[] = "[F4]", fullscreen[] = "Fullscreen";
    const char *strings[] = { confirm, cancel, menu, name, ordinary, fullscreenKey, fullscreen };
    DataWin dw = {0};
    dw.strg.count = sizeof(strings) / sizeof(strings[0]); dw.strg.strings = strings;
    ofPatchStrings(&dw);
    assert(strcmp(strings[0], "\xee\x80\x80 ") == 0);
    assert(strcmp(strings[1], "\xee\x80\x81 ") == 0);
    assert(strcmp(strings[2], "\xee\x80\x82 ") == 0);
    assert(strcmp(strings[3], "\\[C]") == 0);
    assert(strcmp(strings[4], "Z") == 0);
    assert(strcmp(strings[5], OF_ICON_SELECT) == 0);
    assert(strcmp(strings[6], "Butterscotch menu") == 0);
    /* Three small circular controls: Select left, Start right. Centers are
     * five pixels apart, leaving two empty columns between the circles. */
    for (int icon = 4; icon <= 5; icon++) {
        for (int y = 0; y < OF_ICON_SIZE; y++) {
            for (int x = 0; x < OF_ICON_SIZE; x++) {
                bool expected = false;
                for (int button = 0; button < 3; button++) {
                    int distance = abs(x - (1 + 5 * button)) + abs(y - 7);
                    bool active = button == (icon == 5 ? 0 : 2);
                    if (distance == 1 || (distance == 0 && active)) expected = true;
                }
                assert(ofButtonPixel((uint16_t)(OF_ICON_FIRST + icon), x, y) == expected);
            }
        }
    }
    Font font = {0};
    font.present = true; font.maxGlyphHeight = 16; font.glyphCount = 1;
    font.glyphs = calloc(1, sizeof(FontGlyph));
    font.glyphs[0].character = 'A'; font.glyphs[0].shift = 8;
    Font_buildGlyphLUT(&font);
    dw.font.count = 1; dw.font.fonts = &font;
    ofButtonsInstallFonts(&dw);
    assert(TextUtils_findGlyph(&font, 'A') == &font.glyphs[0]);
    assert(TextUtils_measureLineWidth(&font, OF_ICON_A, 3) == 14);
    ofButtonsInstallFonts(&dw); /* repeat initialization must not append twice */
    assert(font.glyphCount == 1 + OF_ICON_COUNT);
    assert(ofButtonPixel(0xe000, 10, 6));
    assert(!ofButtonPixel(0xe000, 6, 10));
    assert(ofButtonPixel(0xe001, 6, 10));
    assert(!ofButtonPixel(0xe001, 10, 6));
    assert(ofButtonPixel(0xe002, 6, 2));
    assert(!ofButtonPixel(0xe002, -1, 0));
    SWRenderer renderer = {0};
    uintpixel_t pixels[20 * 20] = {0};
    renderer.fb = pixels; renderer.fbPitch = 20;
    renderer.width = renderer.height = 20;
    renderer.maxX = renderer.maxY = 20;
    renderer.portW = renderer.portH = 20;
    renderer.writeMask = WRITE_MASK_ALL;
    renderer.scaleX = renderer.scaleY = 1;
    swrSkipFrame = true;
    ofButtonDraw(&renderer.base, 0xe000, 0, 0, 13, 0xffffff, 1);
    assert(pixels[6 * 20 + 10] == 0);
    swrSkipFrame = false;
    ofButtonDraw(&renderer.base, 0xe000, 0, 0, 13, 0xffffff, 1);
    assert(pixels[6 * 20 + 10] != 0); /* physical right button */
    assert(pixels[10 * 20 + 6] == 0); /* hollow bottom button */
    /* Exercise the actual text path too: icon codepoints must use their
     * generated texture, not accidentally sample the game's font atlas. */
    TexturePageItem tpag = {0};
    SWTexture atlas = {0};
    SWTexture *pages[] = { &atlas };
    dw.tpag.count = 1; dw.tpag.items = &tpag;
    font.scaleX = font.scaleY = 1;
    renderer.base.dataWin = &dw;
    renderer.textures = pages; renderer.totalTextureCount = 1;
    memset(pixels, 0, sizeof(pixels));
    swrDrawText(&renderer, OF_ICON_A, 0, 0, 1, 1, 0, 0xffffff, 1, -1);
    assert(pixels[6 * 20 + 10] != 0);
    assert(pixels[10 * 20 + 6] == 0);
    memset(pixels, 0, sizeof(pixels));
    /* Select's three circles must also reach the real text renderer. */
    swrDrawText(&renderer, OF_ICON_SELECT, 0, 0, 1, 1, 0, 0xffffff, 1, -1);
    assert(pixels[7 * 20 + 1] != 0);
    assert(pixels[7 * 20 + 6] == 0 && pixels[7 * 20 + 11] == 0);
    assert(pixels[6 * 20 + 6] != 0 && pixels[6 * 20 + 11] != 0);
    memset(pixels, 0, sizeof(pixels));
    /* Right alignment uses the 14px advance, including the one-pixel gap. */
    renderer.base.drawHalign = 2;
    swrDrawText(&renderer, OF_ICON_A, 20, 0, 1, 1, 0, 0xffffff, 1, -1);
    assert(pixels[6 * 20 + 16] != 0);
    assert(pixels[10 * 20 + 12] == 0);
    Font doubled = {0};
    doubled.present = true; doubled.maxGlyphHeight = 32;
    doubled.scaleX = doubled.scaleY = 1;
    dw.font.fonts = &doubled;
    ofButtonsInstallFonts(&dw);
    assert(TextUtils_measureLineWidth(&doubled, OF_ICON_A, 3) == 28);
    uintpixel_t large[40 * 40] = {0};
    renderer.fb = large; renderer.fbPitch = 40;
    renderer.width = renderer.height = 40;
    renderer.maxX = renderer.maxY = renderer.portW = renderer.portH = 40;
    renderer.base.drawHalign = 0;
    swrDrawText(&renderer, OF_ICON_A, 0, 0, 1, 1, 0, 0xffffff, 1, -1);
    assert(large[12 * 40 + 20] != 0);
    assert(large[20 * 40 + 12] == 0);
    memset(large, 0, sizeof(large));
    swrDrawText(&renderer, OF_ICON_SELECT, 0, 0, 1, 1, 0, 0xffffff, 1, -1);
    assert(large[14 * 40 + 2] != 0);
    assert(large[14 * 40 + 12] == 0 && large[14 * 40 + 22] == 0);
    assert(large[12 * 40 + 12] != 0 && large[12 * 40 + 22] != 0);
    /* Adding external icons must not change atlas-only sampling decisions.
     * The atlas has doubled A strokes and unrelated single-pixel art at 0,0. */
    Font sampled = {0};
    sampled.present = true; sampled.maxGlyphHeight = 8;
    sampled.scaleX = sampled.scaleY = 1;
    sampled.glyphCount = 1; sampled.glyphs = calloc(1, sizeof(FontGlyph));
    sampled.glyphs[0].character = 'A';
    sampled.glyphs[0].sourceX = sampled.glyphs[0].sourceY = 16;
    sampled.glyphs[0].sourceWidth = sampled.glyphs[0].sourceHeight = 8;
    sampled.glyphs[0].shift = 8;
    dw.font.fonts = &sampled;
    ofButtonsInstallFonts(&dw);
    uintpixel_t atlasPixels[32 * 32] = {0};
    atlasPixels[0] = 0xffff;
    for (int y = 16; y < 22; y++)
        for (int x = 16; x < 22; x++) atlasPixels[y * 32 + x] = 0xffff;
    atlas.buffer = atlasPixels; atlas.width = atlas.height = 32; atlas.immutable = true;
    renderer.scaleX = renderer.scaleY = 0.5f;
    swrFavorSpeed = swrSmoothMinify = true;
    swrDrawText(&renderer, "A", 0, 0, 1, 1, 0, 0xffffff, 1, -1);
    assert(sampled.swrDoubledPixels == 1);
    free(sampled.glyphs); free(atlas.halfBuffer); free(atlas.rowBounds);
    free(doubled.glyphs);
    free(font.glyphs);
    puts("button prompts: passed");
    return 0;
}
