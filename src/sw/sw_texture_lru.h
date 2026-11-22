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
// Whether a draw of TPAG item `tpagIndex` would find its texture in already: no load, so nothing given up for one.
bool swrTextureForItemIsIn(SWRenderer* swr, int32_t tpagIndex);

SWTexture* swrLoadPackedFloweyBackdrop(SWRenderer* swr);
SWTexture* swrLoadPackedFloweyStalk(SWRenderer* swr, unsigned pose, int width, int height);

// Loads TPAG item `tpagIndex` as swrTextureForItem would on its first draw,
// ahead of it (a battle's items while its encounter flashes). Only into free
// room: it evicts nothing and leaves the heap's reserve, and an item already
// there is left as it is. What is drawn does not change, only when items are read.
typedef enum { SWR_PREFETCH_LOADED, SWR_PREFETCH_PRESENT, SWR_PREFETCH_SKIPPED, SWR_PREFETCH_FULL, SWR_PREFETCH_PENDING } SwrPrefetch;
SwrPrefetch swrPrefetchItem(SWRenderer* swr, int32_t tpagIndex);
// One staged tile per call; state owns private partial pixels until completion.
SwrPrefetch swrPrefetchItemStep(SWRenderer* swr, int32_t tpagIndex, void** state);
void swrPrefetchCancel(void** state);
// The pack reads swrPrefetchItem would make for the item, to read them ahead
// (see swrTexturePackPlanRect): how many, -1 when it would not load it now
// (swrPrefetchItem says why), or -2 when they cannot be planned.
struct SwrPackSpan;
int swrPlanItem(SWRenderer* swr, int32_t tpagIndex, struct SwrPackSpan* spans, int max);

// Items swrTextureForItem has loaded for a draw since the last call, and the
// time they took; the count starts again from zero.
typedef struct { unsigned count; uint64_t nanos; } SwrItemLoads;
void swrTakeItemLoads(SwrItemLoads* out);

// Frees every per-item texture, e.g. before the renderer is destroyed.
void swrFreeItemTextures(SWRenderer* swr);

#endif//_SW_TEXTURE_LRU_H
