#ifndef _SW_TEXTURE_LRU_H
#define _SW_TEXTURE_LRU_H

void swrEvictTextureFromCache(SWRenderer* swr, int textureIndex);
bool swrEnsureTextureIsLoaded(SWRenderer* swr, uint32_t pageId);

// Returns a texture holding TPAG item `tpagIndex`, or NULL if it cannot be
// loaded. Subtract its originX/originY from page coordinates to index it.
//
// When the texture pack has the item's page, only the item's own rectangle
// (plus a pixel all round) is read and kept, so a few sprites on a large page
// do not cost the whole page in memory and load time. Otherwise, or if the
// whole page is loaded already, this is the page itself with a zero origin.
SWTexture* swrTextureForItem(SWRenderer* swr, int32_t tpagIndex);

SWTexture* swrLoadPackedFloweyBackdrop(SWRenderer* swr);
SWTexture* swrLoadPackedFloweyStalk(SWRenderer* swr, unsigned pose, int width, int height);

// Frees every per-item texture, e.g. before the renderer is destroyed.
void swrFreeItemTextures(SWRenderer* swr);

#endif//_SW_TEXTURE_LRU_H
