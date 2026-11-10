#include <stdlib.h>
#include <limits.h>
#include <float.h>
#include "text_utils.h"
#include "sw_renderer_private.h"

#ifdef SW_PLATFORM_BUTTON_GLYPHS
extern void* platformButtonGlyphTexture(uint16_t codepoint);
#endif

#define SWR_DEBUG_FONT_LINE_HEIGHT  16
#define SWR_DEBUG_FONT_CHAR_WIDTH   8

// ==== Internal structures ====

typedef struct
{
    Font* font;
    TexturePageItem* fontTpag; // single TPAG for regular fonts (NULL for sprite fonts)
    int fontTpagIndex;
    int fontPageId;
    Sprite* spriteFontSprite; // source sprite for sprite fonts (NULL for regular fonts)
}
SwrFontState;

// ==== Internal functions ====

/* Some pixel fonts have a transparent row before their doubled strokes.
 * Averaging from the glyph rectangle's first row splits those strokes into
 * gray edges. Verify every glyph's complete 2x2 pixel grid (either phase on
 * each axis), so half-size text can use the ordinary point sampler instead.
 * Checking the whole font avoids mistaking a small font's one doubled glyph
 * for a doubled font. The font owns this check across texture cache evictions. */
static bool swrGlyphHasPixelBlocks(const SWTexture* texture, int sx, int sy, int sw, int sh, bool* ink)
{
#if PIXEL_SIZE == 16
    for (int phase = 0; phase < 4; ++phase) {
        bool matches = true, anyInk = false;
        for (int y = 0; y < sh && matches; ++y) {
            int partnerY = y + (((y + (phase >> 1)) & 1) ? -1 : 1);
            for (int x = 0; x < sw; ++x) {
                int partnerX = x + (((x + (phase & 1)) & 1) ? -1 : 1);
                uintpixel_t pixel = texture->buffer[(sy + y) * texture->width + sx + x];
                uintpixel_t px = partnerX >= 0 && partnerX < sw ?
                    texture->buffer[(sy + y) * texture->width + sx + partnerX] : 0;
                uintpixel_t py = partnerY >= 0 && partnerY < sh ?
                    texture->buffer[(sy + partnerY) * texture->width + sx + x] : 0;
                anyInk |= (pixel & TRANSPARENT_MASK) != 0;
                if (!(pixel & TRANSPARENT_MASK)) pixel = 0;
                if (!(px & TRANSPARENT_MASK)) px = 0;
                if (!(py & TRANSPARENT_MASK)) py = 0;
                if (pixel != px || pixel != py) { matches = false; break; }
            }
        }
        if (matches) { *ink |= anyInk; return true; }
    }
#else
    (void) texture; (void) sx; (void) sy; (void) sw; (void) sh; (void) ink;
#endif
    return false;
}

static bool swrFontHasDoubledPixels(Font* font, const TexturePageItem* item, const SWTexture* texture)
{
    if (font->swrDoubledPixels != 0) return font->swrDoubledPixels == 1;
    bool ink = false;
    for (uint32_t i = 0; i < font->glyphCount; ++i) {
        const FontGlyph* glyph = &font->glyphs[i];
#ifdef SW_PLATFORM_BUTTON_GLYPHS
        // Generated prompts live outside this font's atlas.
        if (platformButtonGlyphTexture(glyph->character) != NULL) continue;
#endif
        int sx = item->sourceX + glyph->sourceX - texture->originX;
        int sy = item->sourceY + glyph->sourceY - texture->originY;
        int sw = glyph->sourceWidth, sh = glyph->sourceHeight;
        if (sw == 0 || sh == 0) continue;
        if (sx < 0 || sy < 0 || sx + sw > texture->width || sy + sh > texture->height) return false;
        if (!swrGlyphHasPixelBlocks(texture, sx, sy, sw, sh, &ink)) {
            font->swrDoubledPixels = 2;
            return false;
        }
    }
    font->swrDoubledPixels = ink ? 1 : 2;
    return ink;
}

FORCE_INLINE void swrPlotPixel_(Renderer* renderer, int x, int y, uintpixel_t color, int blendmode, int alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (x < swr->portX || y < swr->portY) return;
    if (x >= swr->maxX || y >= swr->maxY) return;
    
    swrDirtyRow(swr, y, x, x + 1);
    alphaBlend(&swr->fb[y * swr->fbPitch + x], color, blendmode, alpha);
}

static bool swrResolveFontState(SWRenderer* swr, DataWin* dw, Font* font, SwrFontState* state)
{
    state->font = font;
    state->fontTpag = NULL;
    state->fontTpagIndex = 0;
    state->spriteFontSprite = NULL;
    
    if (font->isSpriteFont)
    {
        state->spriteFontSprite = &dw->sprt.sprites[font->spriteIndex];
    }
    else
    {
        state->fontTpagIndex = font->tpagIndex;
        if (state->fontTpagIndex < 0) return false;
        
        state->fontTpag = &dw->tpag.items[state->fontTpagIndex];
        int16_t pageId = state->fontTpag->texturePageId;
        if (0 > pageId || (uint32_t) pageId >= swr->totalTextureCount) return false;
        if (!swrTextureForItem(swr, state->fontTpagIndex)) return false;
        
        state->fontPageId = pageId;
    }
    
    return true;
}

static bool swrResolveGlyph(
    SWRenderer* swr, DataWin* dw, SwrFontState* state, FontGlyph* glyph, float cursorX, float cursorY,
    int* tpagIndex, int* pageId, int* sx, int* sy, int* sw, int* sh, float* dx, float* dy
)
{
    Font* font = state->font;
    if (font->isSpriteFont && state->spriteFontSprite != NULL)
    {
        Sprite* sprite = state->spriteFontSprite;
        int32_t glyphIndex = (int32_t) (glyph - font->glyphs);
        if (0 > glyphIndex ||  glyphIndex >= (int32_t) sprite->textureCount) return false;

        int32_t tpagIdx = sprite->tpagIndices[glyphIndex];
        if (0 > tpagIdx) return false;

        TexturePageItem* glyphTpag = &dw->tpag.items[tpagIdx];
        int16_t pid = glyphTpag->texturePageId;
        if (0 > pid || (uint32_t) pid >= swr->totalTextureCount) return false;
        if (!swrTextureForItem(swr, tpagIdx)) return false;

        *tpagIndex = tpagIdx;
        *pageId = glyphTpag->texturePageId;
        
        *sx = glyphTpag->sourceX;
        *sy = glyphTpag->sourceY;
        *sw = glyphTpag->sourceWidth;
        *sh = glyphTpag->sourceHeight;
        
        *dx = cursorX + glyph->offset;
        *dy = cursorY + glyphTpag->targetY - sprite->originY;
    }
    else
    {
        *tpagIndex = state->fontTpagIndex;
        *pageId = state->fontPageId;
        
        *sx = state->fontTpag->sourceX + glyph->sourceX;
        *sy = state->fontTpag->sourceY + glyph->sourceY;
        *sw = glyph->sourceWidth;
        *sh = glyph->sourceHeight;
        
        *dx = cursorX + glyph->offset;
        *dy = cursorY;
    }
    
    return true;
}

#ifdef SW_ENABLE_DEBUG_FONT

#include "sw_debug_font.h"

static void swrDrawDebugFontChar(SWRenderer* swr, char chr, int ax, int ay, uint32_t color, float alphaf)
{
    if (chr == ' ')
        return;
    if (chr < '!' || chr > '~')
        chr = '?';
    
    int alpha = swrIntAlpha(alphaf);
    int blendmode = swr->blendMode;
    
    uintpixel_t actualColor = swrConvertPixel(color);
    for (int y = 0; y < SWR_DEBUG_FONT_LINE_HEIGHT; y++)
    {
        for (int x = 0; x < SWR_DEBUG_FONT_CHAR_WIDTH; x++)
        {
            uint8_t row = swrDebugFont1bpp[chr * SWR_DEBUG_FONT_LINE_HEIGHT + y];
            
            if (row & (1 << (SWR_DEBUG_FONT_CHAR_WIDTH - 1 - x)))
                swrPlotPixel_(&swr->base, ax + x, ay + y, actualColor, blendmode, alpha);
        }
    }
}

#endif

// ==== Exposed interface ====

void swrDrawText(SWRenderer* swr, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t color, float alpha, float lineSeparation)
{
    Renderer* renderer = &swr->base;
    DataWin* dwin = renderer->dataWin;
    
    int32_t fontIndex = renderer->drawFont;
    if (0 > fontIndex || dwin->font.count <= (uint32_t) fontIndex) return;

    Font* font = &dwin->font.fonts[fontIndex];
    
    SwrFontState fontState;
    memset(&fontState, 0, sizeof fontState); // silence warning treated as error
    
    if (!swrResolveFontState(swr, dwin, font, &fontState)) return;
    
    // TODO: do we need to mirror the way the text scrolls too?!
    float cosA = 1.0f, sinA = 0.0f, angleRad = 0.0f;
    bool mustRotate = swrMustRotateSensitive(angleDeg);
    if (UNLIKELY(mustRotate))
    {
        angleRad = -angleDeg * M_PI / 180.0f;
        cosA = cosf(angleRad);
        sinA = sinf(angleRad);
    }
    
    int textLen = (int) strlen(text);
    int lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));

    // Vertical alignment offset
    float totalHeight = (float) lineCount * lineStride;
    float valignOffset = 0;
    if (renderer->drawValign == 1) valignOffset = -totalHeight / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -totalHeight;
    
    xscale *= font->scaleX;
    yscale *= font->scaleY;

    // Iterate through lines. HTML5 subtracts ascenderOffset from the per-line y offset
    // (see yyFont.GR_Text_Draw), shifting glyphs up so the baseline aligns with the drawn y.
    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;

    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        // Find end of current line
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) {
            lineEnd++;
        }
        int32_t lineLen = lineEnd - lineStart;

        // Horizontal alignment offset for this line
        float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
        float halignOffset = 0;
        if (renderer->drawHalign == 1) halignOffset = -lineWidth / 2.0f;
        else if (renderer->drawHalign == 2) halignOffset = -lineWidth;

        float cursorX = halignOffset;

        // Render each glyph in the line - decode each codepoint once and carry it forward as next iteration's ch (also used for kerning)
        int32_t pos = 0;
        uint16_t ch = 0;
        bool hasCh = false;
        if (lineLen > pos) {
            ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
            hasCh = true;
        }

        while (hasCh) {
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);

            uint16_t nextCh = 0;
            bool hasNext = lineLen > pos;
            if (hasNext) nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);

            if (glyph != nullptr) {
                bool drewSuccessfully = false;
                if (glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                    int fontTpagIndex = 0, pageId = 0;
                    int sx, sy, sw, sh, dw, dh;
                    float dx, dy;
                    SWTexture* texture = NULL;
                    bool resolved = false;
#ifdef SW_PLATFORM_BUTTON_GLYPHS
                    texture = (SWTexture*) platformButtonGlyphTexture(ch);
                    if (texture != NULL) {
                        sx = sy = 0;
                        sw = texture->width; sh = texture->height;
                        dx = cursorX + glyph->offset; dy = cursorY;
                        resolved = true;
                    } else
#endif
                    if (swrResolveGlyph(swr, dwin, &fontState, glyph, cursorX, cursorY,
                            &fontTpagIndex, &pageId, &sx, &sy, &sw, &sh, &dx, &dy)) {
                        texture = swrTextureForItem(swr, fontTpagIndex);
                        resolved = texture != NULL;
                    }
                    if (resolved)
                    {
                        dx *= xscale; dx += x;
                        dy *= xscale; dy += y;
                        dw = swrCeiling(xscale * glyph->sourceWidth);
                        dh = swrCeiling(yscale * glyph->sourceHeight);
                        
                        // TODO: at 640x480, for some reason, without this fixup the
                        // letters in the "Name the fallen human." screen don't shake
                        dx = roundf(dx * 2) / 2;
                        dy = roundf(dy * 2) / 2;
                        
                        // Resolving the glyph has already loaded this; it is the cached texture.
                        if (texture != NULL)
                        {
                            sx -= texture->originX;
                            sy -= texture->originY;
                        
                            if (UNLIKELY(mustRotate))
                            {
                                dx -= x;
                                dy -= y;
                                float ndx = cosA * dx - sinA * dy;
                                float ndy = sinA * dx + cosA * dy;
                                ndx += x;
                                ndy += y;
                                swrDrawSpriteRotated(renderer, ndx, ndy, dw, dh, texture, sx, sy, sw, sh, color, alpha, angleDeg, 0.0f, 0.0f, false);
                            }
                            else
                            {
                                unsigned fontFlags = (uint32_t) renderer->drawFont < swr->contrastFontCount ?
                                    swr->contrastFonts[renderer->drawFont] : 0;
                                bool doubled = swrFavorSpeed && swrSmoothMinify && !font->isSpriteFont &&
                                    font->antiAliasing == 0 && texture->immutable &&
                                    xscale * swr->scaleX == 0.5f && yscale * swr->scaleY == 0.5f &&
                                    swrFontHasDoubledPixels(font, fontState.fontTpag, texture);
                                if (doubled)
                                    swrDrawDoubledGlyph(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha);
                                // The platform opts bubble fonts into black ink and HUD fonts into
                                // white ink independently; other colors retain their ordinary sampling.
                                else if (!font->isSpriteFont && font->antiAliasing == 0 &&
                                         (((fontFlags & SWR_FONT_BUBBLE_CONTRAST) && (color & 0xFFFFFF) == 0) ||
                                          ((fontFlags & SWR_FONT_WHITE_CONTRAST) && (color & 0xFFFFFF) == 0xFFFFFF)))
                                    swrDrawContrastSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha,
                                                          (color & 0xFFFFFF) == 0 ? SWR_CONTRAST_BUBBLE_GLYPH : SWR_CONTRAST_UI_GLYPH);
                                else
                                    swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha);
                            }
                        
                            drewSuccessfully = true;
                        }
                    }
                }

                cursorX += glyph->shift;
                if (drewSuccessfully && hasNext) {
                    cursorX += TextUtils_getKerningOffset(glyph, nextCh);
                }
            }

            ch = nextCh;
            hasCh = hasNext;
        }

        cursorY += lineStride;
        // Skip past the newline, treating \r\n and \n\r as single breaks
        if (textLen > lineEnd) {
            lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
        } else {
            lineStart = lineEnd;
        }
    }
}

#ifdef SW_ENABLE_DEBUG_FONT

void swrDrawDebugText(SWRenderer* swr, const char* text, int x, int y, uint32_t color, float alpha)
{
    int px = 0;
    int py = 0;
    
    while (*text)
    {
        char chr = *text;
        text++;
        
        if (chr == '\n') {
            px = 0;
            py += SWR_DEBUG_FONT_LINE_HEIGHT;
            continue;
        }
        
        swrDrawDebugFontChar(swr, chr, x + px, y + py, color, alpha);
        px += SWR_DEBUG_FONT_CHAR_WIDTH;
    }
}

#else

void swrDrawDebugText(SWRenderer* swr, const char* text, int x, int y, uint32_t color, float alpha)
{
    (void) swr;
    (void) text;
    (void) x;
    (void) y;
    (void) color;
    (void) alpha;
}

#endif
