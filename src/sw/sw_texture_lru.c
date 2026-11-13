#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef OF_PC
#include <stdio.h>
#endif
#include "sw_renderer_private.h"
#include "sw_texture_pack.h"
#include "image/image_decoder.h"
#include "gettime.h"

#ifdef PLATFORM_PACK_READ
void platformTextureLoadTime(uint64_t nanos);
#endif

static unsigned finishTextureLoad(uint64_t start)
{
    uint64_t elapsed = nowNanos() - start;
#ifdef PLATFORM_PACK_READ
    platformTextureLoadTime(elapsed);
#endif
    return (unsigned) (elapsed / 1000000u);
}

#if defined(OF_NATIVE_SAVESTATES) && !defined(OF_PC)
/* Only native save-state builds link this port hook. */
extern int platformHeapCanReserve(size_t bytes);
#endif

// Texture pages are decoded on first use and kept until the cache goes over
// TEXTURE_CACHE_BYTES, at which point the least recently used page goes.
//
// Pages drawn during the current frame are never evicted to make room: a
// frame that needs more pages than the budget allows runs over budget
// instead of re-decoding every page every frame. They are only given up when
// an allocation actually fails.

static size_t pageBytes(const SWTexture* texture)
{
    return (size_t) texture->width * texture->height * sizeof(uintpixel_t);
}

static size_t cachedBytes(const SWRenderer* swr)
{
    size_t total = swr->itemBytes;
    for (size_t i = 0; i < swr->textureCount; i++) {
        if (swr->textures[i]) total += pageBytes(swr->textures[i]);
    }
    return total;
}

void swrEvictTextureFromCache(SWRenderer* swr, int textureIndex)
{
    swrOverlayFlush(swr); // held draws may still point at it
    SWTexture* texture = swr->textures[textureIndex];
    swr->textures[textureIndex] = NULL;
    
    swrFreeTexture(texture);
}

static void evictItem(SWRenderer* swr, size_t index)
{
    swrOverlayFlush(swr); // held draws may still point at it
    SWTexture* texture = swr->itemTextures[index];
    swr->itemTextures[index] = NULL;
    swr->itemBytes -= pageBytes(texture);
    swrFreeTexture(texture);
}

// Evicts the least recently used page or item texture. Returns false if nothing qualified.
static bool evictLeastRecentlyUsed(SWRenderer* swr, bool includeCurrentFrame)
{
    int victim = -1;
    bool victimIsItem = false;
    uint32_t oldest = 0;
    
    for (size_t i = 0; i < swr->textureCount; i++) {
        const SWTexture* texture = swr->textures[i];
        if (!texture) continue;
        if (!includeCurrentFrame && texture->lastUsedFrame == swr->frameCounter) continue;
        
        uint32_t age = swr->frameCounter - texture->lastUsedFrame;
        if (victim == -1 || age > oldest) {
            victim = (int) i;
            oldest = age;
        }
    }
    for (size_t j = 0; j < swr->itemCount && swr->itemBytes > 0; j++) {
        const SWTexture* texture = swr->itemTextures[j];
        if (!texture) continue;
        if (!includeCurrentFrame && texture->lastUsedFrame == swr->frameCounter) continue;
        
        uint32_t age = swr->frameCounter - texture->lastUsedFrame;
        if (victim == -1 || age > oldest) {
            victim = (int) j;
            victimIsItem = true;
            oldest = age;
        }
    }
    
    if (victim == -1) return false;
    
    if (victimIsItem) {
        evictItem(swr, (size_t) victim);
        return true;
    }
    logInfo("SWR: Unloaded TXTR page %d%s\n", victim, includeCurrentFrame ? " (in use, out of memory)" : "");
    swrEvictTextureFromCache(swr, victim);
    return true;
}

static void makeRoomFor(SWRenderer* swr, size_t bytes)
{
    while (cachedBytes(swr) + bytes > TEXTURE_CACHE_BYTES) {
        if (!evictLeastRecentlyUsed(swr, false)) break;
    }
}

// Allocates, giving up cached pages (even ones in use this frame) and then
// the renderer's other rebuildable pictures rather than failing.
static void* allocOrEvict(SWRenderer* swr, size_t bytes)
{
    for (;;) {
        void* ptr = malloc(bytes);
        if (ptr) return ptr;
        if (evictLeastRecentlyUsed(swr, true)) continue;
        if (!swrTileRunsFree()) return NULL;
        logInfo("SWR: Dropped the tile pictures (out of memory)\n");
    }
}

// The largest block malloc will hand out right now, to within 64 KB. Only
// called when a page failed to load, to say how far short the heap is.
static size_t largestFreeBlock(size_t limit)
{
    size_t low = 0, high = limit;
    while (high - low > 64 * 1024) {
        size_t middle = low + (high - low) / 2;
        void* probe = malloc(middle);
        if (probe) {
            free(probe);
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}

// A page that could not be loaded is not tried again for this many frames.
// Trying on every draw call empties the cache each time (every other page is
// given up to make room, then reloaded for the next sprite) and the scene
// runs at a crawl; skipping the page's sprites for a few seconds does not.
#define TEXTURE_RETRY_FRAMES 150
#define TEXTURE_RETRY_PAGES 256
static uint32_t retryAtFrame[TEXTURE_RETRY_PAGES];

static SWTexture* loadFromPack(SWRenderer* swr, uint32_t pageId)
{
    int w, h;
    if (!swrTexturePackGetSize(pageId, &w, &h)) return NULL;
    
    size_t bytes = (size_t) w * h * sizeof(uintpixel_t);
    makeRoomFor(swr, bytes);
    
    SWTexture* texture = (SWTexture*) allocOrEvict(swr, sizeof(SWTexture));
    uintpixel_t* buffer = (uintpixel_t*) allocOrEvict(swr, bytes);
    if (!texture || !buffer || !swrTexturePackDecode(pageId, buffer)) {
        logWarn("SWR: Failed to load TXTR page %u from the texture pack.\n", pageId);
        free(buffer);
        free(texture);
        return NULL;
    }
    
    texture->buffer = buffer;
    texture->width = (uint16_t) w;
    texture->height = (uint16_t) h;
    texture->halfBuffer = NULL;
    texture->glyphHints = NULL;
    texture->buttonCache = NULL;
    texture->rotatedRasters = NULL;
    texture->eyeRasters = NULL;
    texture->stationaryRasters = NULL;
    texture->vineGrowthRows = NULL;
    texture->rowBounds = NULL;
    texture->boxRows = NULL;
    texture->floweyArtRows = NULL;
    texture->pipeSolidBounds = NULL;
    texture->halfRowBounds = NULL;
    texture->halfFullBounds = NULL;
    texture->spans = texture->halfSpans = NULL;
    texture->immutable = true;
    texture->halfCoverage = NULL;
    texture->solid = texture->halfSolid = 0;
    texture->halfPhaseX = texture->halfPhaseY = 0;
    texture->originX = texture->originY = 0;
    return texture;
}

static SWTexture* loadFromDataWin(SWRenderer* swr, uint32_t pageId)
{
    DataWin* dw = swr->base.dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];
    bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
    
    int w, h;
    uint8_t* pixels = NULL;
    
    for (;;) {
        if (!txtr->blobData) {
            DataWin_loadTxtrIfNeeded(dw, pageId);
        }
        
        if (txtr->blobData) {
            pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t) txtr->blobSize, gm2022_5, &w, &h);
            if (pixels) {
                if (!txtr->mapped) {
                    free(txtr->blobData);
                    txtr->blobData = NULL;
                } else
                    dropMappedRange(txtr->blobData, 0, txtr->blobSize);
                break;
            }
        }
        
        // Most likely out of memory: give up a cached page and try again.
        if (!evictLeastRecentlyUsed(swr, true)) {
            logError("SWR: Failed to decode TXTR page %u.\n", pageId);
            return NULL;
        }
    }
    
    makeRoomFor(swr, (size_t) w * h * sizeof(uintpixel_t));
    SWTexture* texture = swrCreateTexture(pixels, w, h);
    free(pixels);
    if (texture) texture->immutable = true;
    return texture;
}

#ifdef TEXTURE_CACHE_RESERVE_BYTES
// On targets with a small fixed heap the byte budget alone is a guess: how
// much the game itself needs varies by scene. After each load, make sure a
// block of TEXTURE_CACHE_RESERVE_BYTES can still be allocated, and give up
// the least recently used pages until it can.
static void keepHeapReserve(SWRenderer* swr)
{
    for (;;) {
#if defined(OF_NATIVE_SAVESTATES) && !defined(OF_PC)
        if (platformHeapCanReserve(TEXTURE_CACHE_RESERVE_BYTES) > 0) return;
#endif
        void* probe = malloc(TEXTURE_CACHE_RESERVE_BYTES);
        if (probe) {
            free(probe);
            return;
        }
        if (!evictLeastRecentlyUsed(swr, false)) return;
    }
}
#endif

// Lazily loads a TXTR page on first access.
// Returns true if the texture is ready, false if it failed to load.
bool swrEnsureTextureIsLoaded(SWRenderer* swr, uint32_t pageId)
{
    SWTexture* texture = swr->textures[pageId];
    if (LIKELY(texture != NULL)) {
        texture->lastUsedFrame = swr->frameCounter;
        return true;
    }
    
    // Only real texture pages can be loaded on demand.
    if (pageId >= swr->textureCount) return false;
    if (pageId < TEXTURE_RETRY_PAGES && retryAtFrame[pageId] != 0) {
        if (swr->frameCounter < retryAtFrame[pageId]) return false;
        retryAtFrame[pageId] = 0;
    }
    
    uint64_t loadStart = nowNanos();
    const char* source = "pack";
    int packW = 0, packH = 0;
    bool inPack = swrTexturePackGetSize(pageId, &packW, &packH);
    texture = loadFromPack(swr, pageId);
    // A page the pack holds can only have failed for want of memory, and
    // decoding its PNG needs several times more.
    if (!texture && !inPack) {
        source = "PNG";
        texture = loadFromDataWin(swr, pageId);
    }
    if (!texture) {
        finishTextureLoad(loadStart);
        size_t needed = (size_t) packW * packH * sizeof(uintpixel_t);
        logError("SWR: TXTR page %u needs %u KB; largest free block %u KB, cache %u KB\n", pageId, (unsigned) (needed / 1024),
                 (unsigned) (largestFreeBlock(needed ? needed : 16u * 1024u * 1024u) / 1024), (unsigned) (cachedBytes(swr) / 1024));
        if (pageId < TEXTURE_RETRY_PAGES) retryAtFrame[pageId] = swr->frameCounter + TEXTURE_RETRY_FRAMES;
        return false;
    }
    
    texture->lastUsedFrame = swr->frameCounter;
    swr->textures[pageId] = texture;
#ifdef TEXTURE_CACHE_RESERVE_BYTES
    keepHeapReserve(swr);
#endif
    
    logInfo("SWR: Loaded TXTR page %u (%dx%d, %s) took %u ms, cache %u KB\n", pageId, texture->width, texture->height,
            source, finishTextureLoad(loadStart), (unsigned)(cachedBytes(swr) / 1024));
    return true;
}

// ===[ Per-item textures ]===

void swrFreeItemTextures(SWRenderer* swr)
{
    for (size_t i = 0; i < swr->itemCount; i++) {
        if (swr->itemTextures[i]) evictItem(swr, i);
    }
    free(swr->itemTextures);
    swr->itemTextures = NULL;
    swr->itemCount = 0;
}

#if defined(OF_PC) && PIXEL_SIZE == 16 && !defined IS_BIG_ENDIAN
static void captureFloweyOpeningItem(int32_t item, uint32_t page, int x, int y, int w, int h,
                                     const uintpixel_t* buffer)
{
    static const char* dir;
    static bool checked;
    if (!checked) {
        dir = getenv("BS_FLOWEY_OPENING_OUT");
        checked = true;
    }
    if (!dir) return;

    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%d.rgb555", dir, (int) item);
    if (length < 0 || length >= (int) sizeof(path)) return;
    FILE* out = fopen(path, "wbx");
    if (!out) return;

    uint16_t shape[] = {(uint16_t) page, (uint16_t) x, (uint16_t) y, (uint16_t) w, (uint16_t) h};
    size_t pixels = (size_t) w * h;
    bool ok = fwrite(shape, sizeof(*shape), 5, out) == 5 && fwrite(buffer, sizeof(*buffer), pixels, out) == pixels;
    if (fclose(out) != 0 || !ok) remove(path);
}
#endif

// Loads the item's rectangle, grown by a pixel on each side where the page
// allows: scaled draws may sample one texel past the item, and should find
// what is on the page there, as they would with the whole page loaded.
static SWTexture* loadItem(SWRenderer* swr, const TexturePageItem* tpag, uint32_t pageId, int pageW, int pageH,
                           int32_t tpagIndex, bool* outOfMemory)
{
    *outOfMemory = false;
    int left = tpag->sourceX > 0 ? tpag->sourceX - 1 : 0;
    int top = tpag->sourceY > 0 ? tpag->sourceY - 1 : 0;
    int right = tpag->sourceX + tpag->sourceWidth + 1;
    int bottom = tpag->sourceY + tpag->sourceHeight + 1;
    if (right > pageW) right = pageW;
    if (bottom > pageH) bottom = pageH;
    if (left >= right || top >= bottom) return NULL;
    
    int w = right - left, h = bottom - top;
    size_t bytes = (size_t) w * h * sizeof(uintpixel_t);
    makeRoomFor(swr, bytes);
    
    SWTexture* texture = (SWTexture*) allocOrEvict(swr, sizeof(SWTexture));
    uintpixel_t* buffer = (uintpixel_t*) allocOrEvict(swr, bytes);
    *outOfMemory = !texture || !buffer;
    bool openingItem = tpagIndex >= 0 && swr->floweySpeedRoom && swrSmoothMinify;
    bool decoded = !*outOfMemory &&
                   ((openingItem && swrTexturePackDecodeFloweyOpeningItem(tpagIndex, pageId, left, top, w, h, buffer)) ||
                    swrTexturePackDecodeRect(pageId, left, top, w, h, buffer));
    if (!decoded) {
        free(buffer);
        free(texture);
        return NULL;
    }
#if defined(OF_PC) && PIXEL_SIZE == 16 && !defined IS_BIG_ENDIAN
    if (openingItem) captureFloweyOpeningItem(tpagIndex, pageId, left, top, w, h, buffer);
#endif
    
    texture->buffer = buffer;
    texture->width = (uint16_t) w;
    texture->height = (uint16_t) h;
    texture->halfBuffer = NULL;
    texture->glyphHints = NULL;
    texture->buttonCache = NULL;
    texture->rotatedRasters = NULL;
    texture->eyeRasters = NULL;
    texture->stationaryRasters = NULL;
    texture->vineGrowthRows = NULL;
    texture->rowBounds = NULL;
    texture->boxRows = NULL;
    texture->floweyArtRows = NULL;
    texture->pipeSolidBounds = NULL;
    texture->halfRowBounds = NULL;
    texture->halfFullBounds = NULL;
    texture->spans = texture->halfSpans = NULL;
    texture->immutable = true;
    texture->halfCoverage = NULL;
    texture->solid = texture->halfSolid = 0;
    texture->halfPhaseX = texture->halfPhaseY = 0;
    texture->originX = (uint16_t) left;
    texture->originY = (uint16_t) top;
    return texture;
}

SWTexture* swrLoadPackedFloweyBackdrop(SWRenderer* swr)
{
    uint32_t pageId = (uint32_t) swr->textureCount;
    if (!swrTexturePackHasFloweyBackdrop(pageId)) return NULL;

    TexturePageItem item;
    ZERO_STRUCT(item);
    item.sourceWidth = SWR_FLOWEY_BACKDROP_WIDTH;
    item.sourceHeight = SWR_FLOWEY_BACKDROP_HEIGHT;
    uint64_t start = nowNanos();
    SwrTexturePackStats stats;
    swrTexturePackTakeStats(&stats);
    bool outOfMemory;
    SWTexture* texture = loadItem(swr, &item, pageId, item.sourceWidth, item.sourceHeight, -1, &outOfMemory);
    unsigned ms = finishTextureLoad(start);
    swrTexturePackTakeStats(&stats);
    if (texture != NULL)
        logInfo("SWR: Loaded baked Flowey backdrop: %u ms, %u reads, %u bytes\n",
                ms, (unsigned) stats.reads, (unsigned) stats.bytes);
    return texture;
}

SWTexture* swrLoadPackedFloweyStalk(SWRenderer* swr, unsigned pose, int width, int height)
{
    if (!swrTexturePackHasFloweyStalk(pose, width, height)) return NULL;

    uint64_t start = nowNanos();
    size_t bytes = (size_t) width * height * sizeof(uintpixel_t);
    makeRoomFor(swr, bytes);
    SWTexture* texture = (SWTexture*) allocOrEvict(swr, sizeof(*texture));
    uintpixel_t* pixels = texture ? (uintpixel_t*) allocOrEvict(swr, bytes) : NULL;
    if (!pixels || !swrTexturePackDecodeFloweyStalk(pose, width, height, pixels)) {
        free(pixels);
        free(texture);
        finishTextureLoad(start);
        return NULL;
    }
    ZERO_STRUCT(*texture);
    texture->buffer = pixels;
    texture->width = (uint16_t) width;
    texture->height = (uint16_t) height;
    texture->solid = 2;
    unsigned ms = finishTextureLoad(start);
    logInfo("SWR: Loaded baked Flowey stalk pose %u in %u ms\n", pose, ms);
    return texture;
}

// Whether TPAG item `tpagIndex` is one loaded on its own (an item the game
// was shipped with, on a page the texture pack holds); if so, its page and
// the page's size.
static bool packedItem(SWRenderer* swr, int32_t tpagIndex, const TexturePageItem** outTpag, int* pageW, int* pageH)
{
    DataWin* dw = swr->base.dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return false;
    const TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    *outTpag = tpag;
    return pageId >= 0 && (size_t) tpagIndex < swr->originalTPagCount && (uint32_t) pageId < swr->textureCount &&
           tpag->sourceWidth > 0 && tpag->sourceHeight > 0 && swrTexturePackGetSize((uint32_t) pageId, pageW, pageH);
}

static void keepItem(SWRenderer* swr, int32_t tpagIndex, SWTexture* item)
{
    item->lastUsedFrame = swr->frameCounter;
    swr->itemTextures[tpagIndex] = item;
    swr->itemBytes += pageBytes(item);
}

static void ensureItemTable(SWRenderer* swr)
{
    if (swr->itemTextures == NULL) {
        swr->itemTextures = (SWTexture**) safeCalloc(swr->originalTPagCount, sizeof(SWTexture*));
        swr->itemCount = swr->originalTPagCount;
    }
}

// Items loaded by a first draw (not by swrPrefetchItem), for the benchmark.
static SwrItemLoads drawLoads;

void swrTakeItemLoads(SwrItemLoads* out)
{
    *out = drawLoads;
    memset(&drawLoads, 0, sizeof(drawLoads));
}

SwrPrefetch swrPrefetchItem(SWRenderer* swr, int32_t tpagIndex)
{
    const TexturePageItem* tpag;
    int pageW, pageH;
    // Flowey's fight decodes some items its own way (loadItem's openingItem).
    if (swr->floweySpeedRoom || !packedItem(swr, tpagIndex, &tpag, &pageW, &pageH)) return SWR_PREFETCH_SKIPPED;
    uint32_t pageId = (uint32_t) tpag->texturePageId;
    if (swr->textures[pageId] != NULL || (swr->itemTextures != NULL && swr->itemTextures[tpagIndex] != NULL))
        return SWR_PREFETCH_PRESENT;
    if (pageId < TEXTURE_RETRY_PAGES && retryAtFrame[pageId] != 0) return SWR_PREFETCH_SKIPPED;
    
    // Only into room that is free: nothing cached is given up for it, and the
    // heap keeps its reserve. loadItem's makeRoomFor and allocOrEvict then
    // have nothing to evict.
    int left = tpag->sourceX > 0 ? tpag->sourceX - 1 : 0;
    int top = tpag->sourceY > 0 ? tpag->sourceY - 1 : 0;
    int right = tpag->sourceX + tpag->sourceWidth + 1 < pageW ? tpag->sourceX + tpag->sourceWidth + 1 : pageW;
    int bottom = tpag->sourceY + tpag->sourceHeight + 1 < pageH ? tpag->sourceY + tpag->sourceHeight + 1 : pageH;
    if (left >= right || top >= bottom) return SWR_PREFETCH_SKIPPED;
    size_t bytes = (size_t) (right - left) * (size_t) (bottom - top) * sizeof(uintpixel_t);
    if (cachedBytes(swr) + bytes > TEXTURE_CACHE_BYTES) return SWR_PREFETCH_FULL;
#ifdef TEXTURE_CACHE_RESERVE_BYTES
#if defined(OF_NATIVE_SAVESTATES) && !defined(OF_PC)
    if (platformHeapCanReserve(TEXTURE_CACHE_RESERVE_BYTES + bytes) <= 0) return SWR_PREFETCH_FULL;
#else
    void* probe = malloc(TEXTURE_CACHE_RESERVE_BYTES + bytes);
    if (probe == NULL) return SWR_PREFETCH_FULL;
    free(probe);
#endif
#endif
    
    ensureItemTable(swr);
    bool outOfMemory;
    SWTexture* item = loadItem(swr, tpag, pageId, pageW, pageH, tpagIndex, &outOfMemory);
    if (item == NULL) return SWR_PREFETCH_SKIPPED; // a draw will try again
    keepItem(swr, tpagIndex, item);
    return SWR_PREFETCH_LOADED;
}

SWTexture* swrTextureForItem(SWRenderer* swr, int32_t tpagIndex)
{
    DataWin* dw = swr->base.dataWin;
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dw->tpag.count) return NULL;
    
    const TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) return NULL;
    
    // The whole page, if something has loaded it.
    SWTexture* page = swr->textures[pageId];
    if (page != NULL) {
        page->lastUsedFrame = swr->frameCounter;
        return page;
    }
    
    // Items the game was shipped with, on pages the pack holds, are loaded on their own.
    int pageW, pageH;
    if (packedItem(swr, tpagIndex, &tpag, &pageW, &pageH))
    {
        ensureItemTable(swr);
        
        SWTexture* item = swr->itemTextures[tpagIndex];
        if (LIKELY(item != NULL)) {
            item->lastUsedFrame = swr->frameCounter;
            return item;
        }
        
        if ((uint32_t) pageId < TEXTURE_RETRY_PAGES && retryAtFrame[pageId] != 0) {
            if (swr->frameCounter < retryAtFrame[pageId]) return NULL;
            retryAtFrame[pageId] = 0;
        }
        
        uint64_t loadStart = nowNanos();
        SwrTexturePackStats stats;
        swrTexturePackTakeStats(&stats);
        bool outOfMemory;
        item = loadItem(swr, tpag, (uint32_t) pageId, pageW, pageH, tpagIndex, &outOfMemory);
        if (item == NULL) {
            finishTextureLoad(loadStart);
            // Out of memory is worth backing off from: making room has cost
            // other textures. A read that failed is tried again on the next
            // draw; that passes (the storage was busy) and holding the page
            // back left a whole attack's bullets undrawn.
            int readError = errno;
            static uint32_t lastLoggedFrame = 0;
            if (outOfMemory || swr->frameCounter - lastLoggedFrame >= 30) {
                lastLoggedFrame = swr->frameCounter;
                if (outOfMemory)
                    logError("SWR: No memory for TPAG item %d (%dx%d) of page %d; cache %u KB\n", (int) tpagIndex,
                             (int) tpag->sourceWidth, (int) tpag->sourceHeight, (int) pageId, (unsigned) (cachedBytes(swr) / 1024));
                else
                    logError("SWR: TPAG item %d of page %d: %s error %d\n", (int) tpagIndex, (int) pageId,
                             swrTexturePackLastError(), readError);
            }
            if (outOfMemory && (uint32_t) pageId < TEXTURE_RETRY_PAGES) retryAtFrame[pageId] = swr->frameCounter + TEXTURE_RETRY_FRAMES;
            return NULL;
        }
        
        keepItem(swr, tpagIndex, item);
#ifdef TEXTURE_CACHE_RESERVE_BYTES
        keepHeapReserve(swr);
#endif
        // Keep the log short. The benchmark counts fast loads too.
        drawLoads.count++;
        drawLoads.nanos += nowNanos() - loadStart;
        unsigned tookMs = finishTextureLoad(loadStart);
        if (tookMs >= 20) {
            swrTexturePackTakeStats(&stats);
            logInfo("SWR: Loaded TXTR item %d of page %d (%dx%d, pack) took %u ms, cache %u KB\n", (int) tpagIndex, (int) pageId,
                    (int) item->width, (int) item->height, (unsigned) tookMs, (unsigned) (cachedBytes(swr) / 1024));
            // ms to one decimal: reads (count, KB), waiting to be accepted, audio between reads, decoding.
            logInfo("  item %d: %u reads %u KB in %u.%u ms (wait %u.%u), audio %u.%u, decode %u.%u\n", (int) tpagIndex,
                    (unsigned) stats.reads, (unsigned) (stats.bytes / 1024),
                    (unsigned) (stats.readNanos / 1000000u), (unsigned) (stats.readNanos / 100000u % 10u),
                    (unsigned) (stats.refusedNanos / 1000000u), (unsigned) (stats.refusedNanos / 100000u % 10u),
                    (unsigned) (stats.tickNanos / 1000000u), (unsigned) (stats.tickNanos / 100000u % 10u),
                    (unsigned) (stats.decodeNanos / 1000000u), (unsigned) (stats.decodeNanos / 100000u % 10u));
        }
        return swr->itemTextures[tpagIndex];
    }
    
    if (!swrEnsureTextureIsLoaded(swr, (uint32_t) pageId)) return NULL;
    return swr->textures[pageId];
}
