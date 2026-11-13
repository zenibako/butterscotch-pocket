#ifndef _SW_RENDERER_H
#define _SW_RENDERER_H

#include "renderer.h"

Renderer* SWRenderer_create(void);

typedef struct {
    int blendMode;
    int writeMask;
    bool usingAlphaBlendState;
} SWRendererSnapshotState;

/* Returns false only when an SW renderer still has in-flight draw work. */
bool SWRenderer_snapshotGetState(Renderer* renderer, SWRendererSnapshotState* state);
void SWRenderer_snapshotApplyState(Renderer* renderer, const SWRendererSnapshotState* state);

void SWRenderer_clearFrameBuffer(Renderer* renderer, uint32_t color);
/* Screen draws can be omitted on a skip. User surfaces still receive draws. */
bool SWRenderer_isSkippingScreen(const Renderer* renderer);

// Platform opt-in for selected UI assets; only exact half-size Speed draws are adjusted.
void SWRenderer_setContrastAssets(Renderer* renderer, const char* const* sprites, size_t spriteCount,
                                 const char* const* fonts, size_t fontCount);
// Add transparent-ink sprites and white HUD fonts to the configured asset set.
void SWRenderer_setContrastGlyphAssets(Renderer* renderer, const char* const* sprites, size_t spriteCount,
                                      const char* const* fonts, size_t fontCount);
// Exact, immutable white labels only: source <=64x16, sampled at 2:1.
// The fingerprint is FNV-1a over little-endian RGB555 texels. Definitions must
// outlive the renderer; mismatches keep the ordinary selected-UI sampler.
typedef struct {
    const char* spriteName;
    uint16_t width, height;
    uint32_t sourceHash;
    uint8_t patchX, patchWidth;
    uint8_t patchRows[8]; // optional <=8-pixel-wide replacement, MSB at patchWidth-1
} SWHintedGlyphAsset;
void SWRenderer_setHintedGlyphAssets(Renderer* renderer, const SWHintedGlyphAsset* assets, size_t count);
void SWRenderer_setContrastEnabled(Renderer* renderer, bool enabled);
void SWRenderer_setFloweySpeedAssets(Renderer* renderer);
void SWRenderer_setFloweySpeedRoom(Renderer* renderer, bool active);
void SWRenderer_drawFloweyStalkEyePipes(Renderer* renderer);
/* Loads a TPAG item ahead of its first draw, into free cache room only; see
 * swrPrefetchItem. Returns its SwrPrefetch result: 0 loaded, 1 already there,
 * 2 not one loaded on its own (or it failed), 3 no free room. */
int SWRenderer_prefetchItem(Renderer* renderer, int32_t tpagIndex);
/* Items loaded for a draw since the last call (not prefetched), and their time. */
void SWRenderer_takeItemLoads(unsigned* count, uint64_t* nanos);
/* Whether a filled draw_circle/draw_ellipse with this centre and these radii would put nothing on the target: it
 * lies wholly outside, beyond a pixel's margin. False for any renderer but this one. Such a call still settles held
 * layers, so a caller may leave one out only when a draw since the last thing held has done that already. */
bool SWRenderer_filledEllipseOutside(Renderer* renderer, float cx, float cy, float rx, float ry);
/* Renderer_drawCircle(renderer, cx, cy, radius, false) less the settling of held layers, for a caller that knows a
 * draw since the last thing held has settled them already. False, having drawn nothing, for any renderer but this. */
bool SWRenderer_drawFilledCircleSettled(Renderer* renderer, float cx, float cy, float radius);
// The renderer clears a platform display buffer only where it drew since that
// buffer's last clear. A platform that writes into one of those buffers itself
// (an overlay, a menu, a copy) calls this, so the next clear is a whole one.
void swrFramebufferTouched(void);
// Tiles (32x16) marked as drawn in the buffer the renderer cleared last, before that clear: a measure of how much of
// the screen the frame drawn in it touched. 0 when the buffer was not tracked.
extern uint32_t swrDirtyLastTiles;

#endif//_SW_RENDERER_H
