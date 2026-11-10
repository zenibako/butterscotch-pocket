#ifndef _SW_DRAWING_H
#define _SW_DRAWING_H

enum {
    SWR_LINE_ALIGN_CENTER,
    SWR_LINE_ALIGN_M1,
    SWR_LINE_ALIGN_P1,
};

bool swrSwitchToSurface(Renderer* renderer, int32_t targetSurfaceId, bool restoreOldView);
void swrFreeRotatedRasters(SWTexture* texture);
// When true, sprites drawn at roughly half size are box-filtered instead of point-sampled.
extern bool swrSmoothMinify;
// Frame skipping: while true, nothing is drawn to the screen. The game's Draw
// events still run (games keep logic in them), and drawing to surfaces still
// happens because a surface outlives the frame; only the pixel work for the
// screen itself is left out. The platform sets it before a frame it will not
// present and must not change it between beginFrame and endFrameEnd.
extern bool swrSkipFrame;
#define SWR_SKIPPED(swr) (swrSkipFrame && !(swr)->drawingToSurface)
extern bool swrMirrorMerge;
extern int swrMirrorFaintAlpha;
extern bool swrFavorSpeed;
// When false, GMS2 tile layers are drawn cell by cell through drawSpritePart (for comparing).
extern bool swrTileLayerFast;
bool swrMirrorHoldFill(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t pxcolor, float alpha);

void swrPlotPixel(Renderer* renderer, float x, float y, uintpixel_t color, float alpha);
void swrDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uintpixel_t color, uintpixel_t color2, float alpha, int alignment);
SWTexture* swrMakeFloweyPipeBackdrop(const SWTexture* pipe, int sx, int sy);
SWTexture* swrMakeFloweyEyePipes(SWTexture* pipe, int sx, int sy);
void swrAddFloweyBackdropPipeSegments(SWTexture* art, SWTexture* pipe, int sx, int sy);
enum { SWR_FLOWEY_BACKDROP_VINE_LAYERS = 3 };
void swrAddFloweyBackdropVines(SWTexture* art, const SWTexture* vine, int sx, int sy,
                               const TexturePageItem* item, unsigned layer);
bool swrDrawFloweyStalkHalf(Renderer* renderer, SWTexture* texture, float dx, float dy,
                           int sx, int sy, int sw, int sh, bool mirrored);
enum { SWR_FLOWEY_ART_NONE, SWR_FLOWEY_ART_ARM, SWR_FLOWEY_ART_CASE, SWR_FLOWEY_ART_SCREEN };
bool swrDrawFloweyArt(Renderer* renderer, SWTexture* texture, float dx, float dy, float dw, float dh,
                      int sx, int sy, int sw, int sh, int kind);
void swrDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color, float alpha);
void swrDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, uintpixel_t color4, float alpha);
void swrFillRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color, float alpha);
void swrFillRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, uintpixel_t color4, float alpha);
// Draws whatever solid overlay swrDrawSprite is holding back. Anything else
// that reads or writes pixels, or changes where they go, must call this first.
void swrOverlayFlush(SWRenderer* swr);
// Ends a sprite trail being thinned with speed favoured (see swrDrawSprite).
void swrTrailEnd(void);
// Draws the tile pictures being held (sw_renderer.c); NULL for whichever renderer holds them.
void swrTileRunsFlush(SWRenderer* swr);
void swrClearSettle(SWRenderer* swr);
bool swrDrawSpriteTiledRows(SWRenderer* swr, SWTexture* texture, int sx, int sy, int sw, int sh,
                            int firstX, int firstY, int tileW, int tileH, int countX, int countY,
                            uint32_t color, float alphaf);
bool swrTiledHold(SWRenderer* swr, SWTexture* texture, int sx, int sy, int sw, int sh,
                  int firstX, int firstY, int tileW, int tileH, int countX, int countY,
                  uint32_t color, float alphaf);
void swrTiledHeldWrite(SWRenderer* swr, uintpixel_t color);
bool swrFillHold(SWRenderer* swr, uintpixel_t pxcolor, int alphaInt);
void swrFillHeldDrop(void);
void swrGridFlush(SWRenderer* swr);
bool swrGridHeld(void);
void swrHeldUnderDiscard(SWRenderer* swr);
bool swrFillCoversMain(SWRenderer* swr, float x1, float y1, float x2, float y2, float alpha);
bool swrFillCoversWhole(SWRenderer* swr, float x1, float y1, float x2, float y2);
void swrOverlayFlushForState(SWRenderer* swr);
void swrDrawSprite(Renderer* renderer, float dx, float dy, float dw, float dh, SWTexture* texture, int sx, int sy, int sw, int sh, uint32_t tintColor, float alpha);
enum { SWR_CONTRAST_NONE, SWR_CONTRAST_GLYPH, SWR_CONTRAST_BUTTON, SWR_CONTRAST_UI_GLYPH, SWR_CONTRAST_BUBBLE_GLYPH };
void swrDrawContrastSprite(Renderer* renderer, float dx, float dy, float dw, float dh, SWTexture* texture,
                           int sx, int sy, int sw, int sh, uint32_t tintColor, float alpha, int contrast);
// Returns false without drawing if the selected source or transform is unsupported.
bool swrDrawHintedGlyph(Renderer* renderer, float dx, float dy, float dw, float dh, SWTexture* texture,
                        int sx, int sy, int sw, int sh, uint32_t tintColor, float alpha,
                        const SWHintedGlyphAsset* asset);
// Point-samples a verified doubled font at exact half size; otherwise draws normally.
void swrDrawDoubledGlyph(Renderer* renderer, float dx, float dy, float dw, float dh, SWTexture* texture, int sx, int sy, int sw, int sh, uint32_t tintColor, float alpha);
void swrDrawSpriteRotated(Renderer* renderer, float dx, float dy, float dw, float dh, SWTexture* texture, int sx, int sy, int sw, int sh, uint32_t tintColor, float alpha, float angleDeg, float pivotX, float pivotY, bool floweyVine);
void swrDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha);

#endif//_SW_DRAWING_H
