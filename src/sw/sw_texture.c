#include <stdlib.h>
#include "sw_renderer_private.h"

SWTexture* swrCreateTextureEx(const void* srcBuffer, int width, int height, bool convert)
{
    SWTexture* txt = (SWTexture*) safeMalloc(sizeof(SWTexture));
    txt->buffer = (uintpixel_t*) safeMalloc(width * height * sizeof(uintpixel_t));
    
    size_t sz = width * height;
    
    if (srcBuffer)
    {
        if (convert) {
            const uint32_t* rgbaSrc = (const uint32_t*) srcBuffer;
            for (size_t i = 0; i < sz; i++)
                txt->buffer[i] = swrConvertPixelTexture(rgbaSrc[i]);
        }
        else {
            const uintpixel_t* rgbaSrc = (const uintpixel_t*) srcBuffer;
            for (size_t i = 0; i < sz; i++)
                txt->buffer[i] = rgbaSrc[i];
        }
    }
    else
    {
        memset(txt->buffer, 0, width * height * sizeof(uintpixel_t));
    }
    
    txt->width = (uint16_t) width;
    txt->height = (uint16_t) height;
    txt->originX = txt->originY = 0;
    txt->lastUsedFrame = 0;
    txt->halfBuffer = NULL;
    txt->glyphHints = NULL;
    txt->buttonCache = NULL;
    txt->rotatedRasters = NULL;
    txt->eyeRasters = NULL;
    txt->stationaryRasters = NULL;
    txt->rowBounds = NULL;
    txt->boxRows = NULL;
    txt->pipeSolidBounds = NULL;
    txt->halfRowBounds = NULL;
    txt->halfFullBounds = NULL;
    txt->spans = txt->halfSpans = NULL;
    txt->immutable = false;
    txt->halfCoverage = NULL;
    txt->solid = txt->halfSolid = 0;
    txt->halfPhaseX = txt->halfPhaseY = 0;
    
    return txt;
}

SWTexture* swrCreateTexture(const uint8_t* srcBuffer, int width, int height)
{
    return swrCreateTextureEx(srcBuffer, width, height, true);
}

SWTexture* swrCopyTexture(SWTexture* texture)
{
    return swrCreateTextureEx(texture->buffer, texture->width, texture->height, false);
}

void swrFreeGlyphHints(SWTexture* texture)
{
    while (texture->glyphHints) {
        SWGlyphHintCache* next = texture->glyphHints->next;
        free(texture->glyphHints);
        texture->glyphHints = next;
    }
}

// Includes entry metadata and pixels across all textures/renderers. The ordinary
// sampler remains available when this optional budget or malloc is exhausted.
// The side limit admits Undertale's 110x42 FIGHT/ACT/ITEM/MERCY buttons, which
// otherwise take the per-pixel hybrid sampler on every half-size frame (8.7 ms
// a frame on the Pocket for about 2.6 draws); each costs some 9 KB here.
MAYBE_UNUSED static size_t swrButtonCacheBytes;
enum { SWR_BUTTON_CACHE_BYTES = 192 * 1024, SWR_BUTTON_CACHE_SIDE = 128 };

const SWButtonCache* swrButtonCache(SWTexture* texture, int sx, int sy, int sw, int sh)
{
#ifndef SW_HAS_PREMUL_BLEND
    (void) texture; (void) sx; (void) sy; (void) sw; (void) sh;
    return NULL;
#else
    if (!texture->immutable || sw <= 0 || sh <= 0 || sw > SWR_BUTTON_CACHE_SIDE || sh > SWR_BUTTON_CACHE_SIDE ||
        sx < 0 || sy < 0 || sx > texture->width - sw || sy > texture->height - sh)
        return NULL;
    for (SWButtonCache* cache = texture->buttonCache; cache; cache = cache->next)
        if (cache->sx == sx && cache->sy == sy && cache->sw == sw && cache->sh == sh)
            return cache;
    int width = (sw + 1) / 2, height = (sh + 1) / 2;
    size_t bytes = sizeof(SWButtonCache) + (size_t) width * height * sizeof(SWButtonPixel);
    if (bytes > SWR_BUTTON_CACHE_BYTES - swrButtonCacheBytes) return NULL;
    SWButtonCache* cache = malloc(bytes);
    if (!cache) return NULL; // Never evict the source texture while sampling it.
    cache->sx = sx; cache->sy = sy; cache->sw = sw; cache->sh = sh;
    for (int y = 0; y < height; ++y) {
        int y0 = sy + y * 2, y1 = y0 + 1 < sy + sh ? y0 + 1 : y0;
        for (int x = 0; x < width; ++x) {
            int x0 = sx + x * 2, x1 = x0 + 1 < sx + sw ? x0 + 1 : x0;
            uintpixel_t taps[5] = {
                texture->buffer[y1 * texture->width + x1],
                texture->buffer[y0 * texture->width + x0],
                x0 + 1 < sx + sw ? texture->buffer[y0 * texture->width + x0 + 1] : 0,
                y0 + 1 < sy + sh ? texture->buffer[(y0 + 1) * texture->width + x0] : 0,
                x0 + 1 < sx + sw && y0 + 1 < sy + sh ? texture->buffer[(y0 + 1) * texture->width + x0 + 1] : 0
            };
            uint32_t redBlue = 0, green = 0, covered = 0;
            for (int tap = 0; tap < 5; ++tap) {
                if (!swrIsOpaque(taps[tap])) continue;
                unsigned weight = tap == 0 ? 4 : 1;
                redBlue += swrSpreadRedBlue(taps[tap]) * weight;
                green += swrGreen(taps[tap]) * weight;
                covered += weight;
            }
            cache->pixels[y * width + x] = (SWButtonPixel) {
                .redBlue = redBlue << 5, .green = (uint16_t) (green << 5),
                .keep = covered ? (uint8_t) (256 - covered * 32) : 0, .covered = (uint8_t) covered
            };
        }
    }
    cache->next = texture->buttonCache;
    texture->buttonCache = cache;
    swrButtonCacheBytes += bytes;
    return cache;
#endif
}

void swrFreeButtonCache(SWTexture* texture)
{
    while (texture->buttonCache) {
        SWButtonCache* cache = texture->buttonCache;
        texture->buttonCache = cache->next;
        swrButtonCacheBytes -= sizeof(*cache) + (size_t) ((cache->sw + 1) / 2) * ((cache->sh + 1) / 2) * sizeof(SWButtonPixel);
        free(cache);
    }
}

void swrFreeTexture(SWTexture* texture)
{
    if (UNLIKELY(!texture))
        return;
    
    swrFreeGlyphHints(texture);
    swrFreeButtonCache(texture);
    swrFreeRotatedRasters(texture);
    free(texture->halfBuffer); // the coverage, if any, is in the same allocation
    free(texture->rowBounds);
    swrFreeBoxRows(texture);
    free(texture->pipeSolidBounds);
    free(texture->spans);
    free(texture->halfSpans);
    free(texture->buffer);
    free(texture);
}

SWTexture* swrCropSectionFromTexture(SWTexture* src, int32_t width, int32_t height, int32_t cropLeft, int32_t cropTop, int32_t cropRight, int32_t cropBottom)
{
    if (width <= 0 || height <= 0) {
        logError("SWR: Cannot resize texture to %dx%d.\n", width, height);
        return NULL;
    }
    
    if (cropLeft < 0) cropLeft = 0;
    if (cropTop < 0) cropTop = 0;
    if (cropRight >= src->width) cropRight = src->width;
    if (cropBottom >= src->height) cropBottom = src->height;
    
    if (cropLeft >= cropRight || cropTop >= cropBottom) {
        logError("SWR: Invalid crop coordinates for resize.\n");
        return NULL;
    }
    
    int cropWidth = cropRight - cropLeft;
    int cropHeight = cropBottom - cropTop;
    
    SWTexture* dst = swrCreateTexture(NULL, width, height);
    int32_t *mapRows = NULL, *mapCols = NULL;
    
    if (cropWidth != width) {
        mapCols = (int32_t *)safeCalloc(width, sizeof(int32_t));
        for (int32_t i = 0; i < width; i++)
            mapCols[i] = i * (cropRight - cropLeft) / width + cropLeft;
    }
    if (cropHeight != height) {
        mapRows = (int32_t *)safeCalloc(height, sizeof(int32_t));
        for (int32_t i = 0; i < height; i++)
            mapRows[i] = i * (cropBottom - cropTop) / height + cropTop;
    }
    
    for (int32_t y = 0, y1 = cropTop; y < height; y++, y1++)
    {
        uintpixel_t* dstbuf = &dst->buffer[y * width];
        const uintpixel_t* srcbuf = &src->buffer[(mapRows ? mapRows[y] : y1) * src->width];
        
        if (width == cropWidth)
        {
            for (int32_t x = 0, x1 = cropLeft; x < width; x++, x1++) {
                dstbuf[x] = srcbuf[x1];
            }
        }
        else
        {
            for (int32_t x = 0; x < width; x++) {
                dstbuf[x] = srcbuf[mapCols[x]];
            }
        }
    }
    
    if (mapCols) free(mapCols);
    if (mapRows) free(mapRows);
    
    return dst;
}

// Emulates the `removeback` flag from `sprite_create_from_surface`.
void swrRemoveBackgroundFromTexture(SWTexture* texture)
{
    // bottom left pixel
    uintpixel_t background = texture->buffer[texture->width * (texture->height - 1)];
#ifdef TRANSPARENT_MASK
    background &= ~TRANSPARENT_MASK;
#endif
    
    size_t widthheight = texture->width * texture->height;
    for (size_t i = 0; i < widthheight; i++)
    {
#ifdef TRANSPARENT_MASK
        if ((texture->buffer[i] & ~TRANSPARENT_MASK) == background)
#else
        if (texture->buffer[i] == background)
#endif
        {
#ifdef PXL_TRANSPARENT
            texture->buffer[i] = PXL_TRANSPARENT;
#else
            texture->buffer[i] = 0;
#endif
        }
    }
}
