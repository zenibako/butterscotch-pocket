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
// The renderer clears a platform display buffer only where it drew since that
// buffer's last clear. A platform that writes into one of those buffers itself
// (an overlay, a menu, a copy) calls this, so the next clear is a whole one.
void swrFramebufferTouched(void);

#endif//_SW_RENDERER_H
