#ifndef _SW_RENDERER_PRIVATE_H
#define _SW_RENDERER_PRIVATE_H

#include "sw_renderer.h"
#include "sw_defines.h"
#include "sw_pixel_convert.h"

enum { SWR_FONT_BUBBLE_CONTRAST = 1, SWR_FONT_WHITE_CONTRAST = 2 };

// Frees the composed tile-run pictures (sw_renderer.c); they are rebuilt on
// the next draw. Returns false if there were none.
bool swrTileRunsFree(void);


// Unimplemented Functions
#define UNIMP() do { logWarn("NYI %s\n", __func__); } while (0)
//#define UNIMP() do { } while (0)
#define UNIMP2() do { } while (0)

// Provide PI if not specified
#ifndef M_PI
#define M_PI 3.1415926535897932384626
#endif

// Cached masks are private derivatives; original and half-size texels stay intact.
typedef struct SWGlyphHintCache {
    struct SWGlyphHintCache* next;
    SWHintedGlyphAsset asset;
    int sx, sy;
    bool valid;
    uintpixel_t pixels[32 * 8];
} SWGlyphHintCache;
typedef struct {
    int32_t item;
    const SWHintedGlyphAsset* asset;
} SWHintedGlyphBinding;

// Exact hybrid-filter intermediates for white, alpha-256 button draws. Channel
// sums are premultiplied without rounding to RGB555; edges still blend with the
// destination. Entries belong to their immutable source, not a frame or surface.
typedef struct {
    uint32_t redBlue;
    uint16_t green;
    uint8_t keep, covered;
} SWButtonPixel;
typedef struct SWButtonCache {
    struct SWButtonCache* next;
    int sx, sy, sw, sh;
    SWButtonPixel pixels[];
} SWButtonCache;

// Struct Definitions
typedef struct
{
    uintpixel_t* buffer;
    uint16_t width, height;
    // Where buffer[0] is on its texture page. Zero for a whole page; a texture
    // that holds only one TPAG item's part of a page (see swrTextureForItem)
    // starts at that part's corner, so page coordinates minus this index it.
    uint16_t originX, originY;
    uint32_t lastUsedFrame; // SWRenderer.frameCounter when last drawn, for cache eviction
    // A copy at half the size each way, made the first time the texture is
    // drawn at half scale with swrFavorSpeed set (see swrHalfTexture), so
    // that such draws need no averaging. Only for a texture whose pixels never
    // change (immutable: one loaded from the game's data, not a surface).
    SWGlyphHintCache* glyphHints; // freed with the immutable source texture
    SWButtonCache* buttonCache;
    struct SWRotatedRasterCache* rotatedRasters;
    struct SWRotatedRasterCache* eyeRasters;
    struct SWRotatedRasterCache* stationaryRasters;
    uintpixel_t* halfBuffer;
    uint8_t* halfCoverage;  // how many of its four texels each copied one stands for, 0..4; NULL when every one is 0 or 4
    bool immutable;
    uint8_t halfPhaseX, halfPhaseY; // 0 or 1: where the 2x2 blocks start (see swrHalfTexture)
    // Whether every texel is opaque, so that an opaque draw can copy rows
    // without looking at them: 0 not looked at yet, 1 yes, 2 no. Looked at
    // only for an immutable texture; halfSolid is the same for the half-size copy.
    uint8_t solid, halfSolid;
    // For each row, the first column that has an opaque texel and the one
    // after the last (both 0 for an empty row), so that a draw can leave out
    // the transparent ends of the row without looking at them: most of a row
    // when the texture is a few thin lines. Made on first need, for an
    // immutable texture only (swrRowBounds); the half-size copy's are made
    // with it and live in its allocation.
    uint16_t* rowBounds;
    struct SWBoxRows* boxRows;
    struct SWFloweyArtRows* floweyArtRows;
    uint16_t* pipeSolidBounds; // longest fully opaque point and 2x2 runs per row
    uint16_t* halfRowBounds;
    uint16_t* halfFullBounds; // per row of the half-size copy, the longest run of texels standing for all four of theirs: [start, end)
    // For each row, the runs of opaque texels other than spansColor, so that a
    // draw onto tiles still holding only that colour (see SWDirtyBuffer) reads
    // and writes just those (swrColorSpans); halfSpans the same for the
    // half-size copy. Made on first need, for an immutable texture only.
    struct SWColorSpans* spans;
    struct SWColorSpans* halfSpans;
}
SWTexture;

const SWButtonCache* swrButtonCache(SWTexture* texture, int sx, int sy, int sw, int sh);
void swrFreeButtonCache(SWTexture* texture);
void swrFreeBoxRows(SWTexture* texture);

typedef struct
{
    // used for almost all intents and purposes.
    SWTexture* texture;
    // upon a gpuSetColorWriteEnable change, the shadow texture is used for writing instead.
    SWTexture* shadowTexture;
}
SWSurface;

#define WRITE_MASK_ALL   (15)
#define WRITE_MASK_RED   (1)
#define WRITE_MASK_GREEN (2)
#define WRITE_MASK_BLUE  (4)
#define WRITE_MASK_ALPHA (8)

typedef struct {
    float x, y;
    uintpixel_t color;
} SWVertex;

// The arguments of one swrDrawSpriteInternal call, kept so it can be issued later.
#define SW_MIRROR_MAX_LAYERS 12
typedef struct {
    int dx, dy, dw, dh;
    SWTexture* texture;
    int sx, sy, sw, sh;
    uintpixel_t tintColor;
    int alpha;
} SWSpriteCall;

// One layer of a mirrored stack: a sprite drawn as four quarters, or a solid
// fill of the whole mirrored area that followed them.
typedef struct {
    SWSpriteCall calls[4];      // the quarters as asked for; a fill uses calls[0]'s tintColor and alpha
    int sx, sy, xstep, ystep;   // how the unflipped quarter samples its texture, after clipping
    bool solid;
} SWMirrorLayer;

typedef struct {
    uint32_t angleBits;
    float cosine, sine;
} SWRotation;

#define SWR_ROTATION_CACHE_SIZE 64

typedef struct
{
    Renderer base;
    
    // Window Properties
    uint16_t width;
    uint16_t height;
    // Framebuffer
    uintpixel_t* fb;
    uint16_t fbPitch; // in sizeof(uintpixel_t) units, NOT in bytes!
    
    bool drawingToSurface;
    // Optional platform-selected UI assets. These flags never belong to a shared texture cache.
    bool contrastEnabled;
    SWHintedGlyphBinding* hintedGlyphs;
    size_t hintedGlyphCount;
    uint8_t* contrastItems;
    uint8_t* contrastFonts;
    uint32_t contrastItemCount, contrastFontCount;
    bool floweySpeedRoom;
    int32_t floweyVineItem;
    int32_t floweyStalkSprite, floweyStalkFirst, floweyStalkLast;
    int32_t floweyArmItem, floweyTvCaseItem, floweyTvScreenSprite;
    int32_t floweyOuterEyeShadow;
    int32_t floweyPipeItem;
    uint32_t floweyPipeFrame;
    SWTexture* floweyPipeBackdrop;
    SWTexture* floweyStalkEyePipes;
    int32_t floweyUprightItems[4];
    unsigned floweyUprightCount;
    int32_t floweyBackdropItems[4];
    unsigned floweyBackdropCount;
    uintpixel_t* mainFb;
    uint16_t mainWidth;
    uint16_t mainHeight;
    uint16_t mainPitch;
    int lastViewX, lastViewY, lastViewW, lastViewH;
    int lastPortX, lastPortY, lastPortW, lastPortH;
    int lastGameW, lastGameH, lastMaxX, lastMaxY;
    float lastScaleX, lastScaleY;
    
    SWTexture** textures;
    // Per-TPAG-item textures for pages that are not loaded whole; see swrTextureForItem.
    SWTexture** itemTextures;
    size_t itemCount;
    size_t itemBytes;
    SWSurface** surfaces;
    uint32_t frameCounter;
    bool pendingClear;      // clearFrameBuffer was requested but not done yet (see swrFlushPendingClear)
    uintpixel_t pendingClearColor;
    bool clearHeld;         // a clear of the whole main buffer is still to be done (see swrClearSettle)
    uintpixel_t clearHeldColor;
    // Tiled passes held over the held clear, to be written with it row by row
    // (see swrTiledHold); only ever non-zero while clearHeld is set.
    int tiledHeldCount;
    bool clearHeldForFill;  // SWRenderer_drawRectangle took the held clear for the swrFillRectangle call it is making
    bool fbIsPlatform; // mainFb belongs to the platform (SW_PLATFORM_FRAMEBUFFER), not to us
    // What is known of the buffer being drawn into (see swrClearMain), or
    // NULL when nothing is tracked: then every clear is a whole one.
    struct SWDirtyBuffer* dirty;
    struct SWDirtyBuffer* dirtyMark; // `dirty` while there is anything left to mark; NULL once all of it is
    struct SWDirtyBuffer* dirtyBuffers; // SWR_DIRTY_BUFFERS of them, one per platform buffer
    uint8_t baseMode; // what the draw being made does with its buffer's backdrop (see SWBaseKey): 0 nothing, 1 records it, 2 redraws it
    
    // A solid-colour sprite draw that has been held back so that identical
    // draws stacked directly on top of it fold into one blend (see swrOverlayFlush).
    bool overlayMergeAllowed;   // set only while SWRenderer_drawSprite is running
    int overlayCount;           // draws held; 0 when nothing is pending
    uintpixel_t* overlayFb;
    int overlayPitch;
    int overlayX, overlayY, overlayW, overlayH;
    uintpixel_t overlayFirstColor;  // the first draw, kept exactly so that a lone draw is unchanged
    int overlayFirstAlpha;
    float overlayKeep;              // share of the destination that still shows through
    float overlayRed, overlayGreen, overlayBlue; // accumulated colour, 5-bit channel * 256

    SWSpriteCall pipeCalls[3];
    int pipeCount;
    struct SWPipeLayer* pipePlan;
    
    // Translucent layers drawn as four mirrored quarters, held back so that one
    // quarter can be blended and copied to the other three (see swrMirrorFlush).
    bool mirrorReplaying;       // held draws are being issued; hold nothing
    int mirrorLayers;           // complete layers held
    int mirrorStage;            // quarters of the next layer seen so far, 0..3
    uintpixel_t* mirrorFb;
    int mirrorPitch;
    int mirrorPort[6];          // portX, portY, portW, portH, maxX, maxY when the first quarter was held
    int mirrorCx, mirrorCy, mirrorW, mirrorH; // the unflipped quarter's clipped rectangle
    bool mirrorUnderKnown;      // everything under the stack was one colour when it was started
    uintpixel_t mirrorUnder;
    SWMirrorLayer mirrorStack[SW_MIRROR_MAX_LAYERS + 1];
    
    // The whole main buffer holds one colour: set by a full clear or fill,
    // dropped by the next thing drawn. Lets a repeated fill be skipped and a
    // mirrored stack be worked out without reading the buffer back.
    bool uniformValid;
    uintpixel_t uniformColor;
    bool uniformKept;           // for the one swrFillRectangle call SWRenderer_drawRectangle is making
    bool tileRunEntering;       // SWRenderer_drawTileRun is flushing: leave held tile pictures held
    size_t textureCount;
    size_t surfaceCount;
    size_t totalTextureCount;
    size_t originalTPagCount;
    size_t originalSpriteCount;
    
    bool viewActive;
    int viewX, viewY, viewW, viewH;
    int portX, portY, portW, portH;
    int gameW, gameH, maxX, maxY;

    int offsetX, offsetY;
    float scaleX, scaleY;
    float defaultScaleX, defaultScaleY;
    
    int blendMode;
    bool usingAlphaBlendState;
    
    // only used for surfaces.  The application surface doesn't support these at the moment.
    int currentSurfaceIndex;
    int writeMask;
    
    SWVertex* vertexData;
    int vertexCount;
    int maxVertexCount;
    int primitiveType;
    bool primitiveBegun;
    bool primitiveOverflow;

    SWRotation rotationCache[SWR_ROTATION_CACHE_SIZE];
    int32_t floweyPipeSegmentItem;
}
SWRenderer;

// Partial clears. The platform hands out its display buffers in turn, and a
// buffer comes back holding the frame drawn in it a few frames before. On the
// Pocket a whole-screen clear is memory-bound (13 ms at 640x480), while a
// battle draws over a fifth of the screen. So each buffer remembers the colour
// it was last cleared to and which tiles have been written since; a clear to
// the same colour then rewrites only those tiles. Every write into the main
// buffer marks what it touches (swrDirtyRow, swrDirtyRect). Anything else that
// writes into a platform buffer calls swrFramebufferTouched, which forgets what
// was known, so the next clear is a whole one.
#define SWR_DIRTY_TILE_W_SHIFT 5            // 32 px tiles across: one bit each, 32 to a row
#define SWR_DIRTY_TILE_H_SHIFT 4            // 16 px tiles down
#define SWR_DIRTY_MAX_W 1024
#define SWR_DIRTY_MAX_ROWS 64               // 1024 px down
#define SWR_DIRTY_BUFFERS 4
// The first draw after a buffer is cleared, when it is an untinted 1:1 opaque
// sprite (a battle backdrop): everything that decides what it writes. When
// the same draw comes first again the next time that buffer is drawn into,
// the tiles nothing else has touched since still hold exactly what it would
// write there, so they are neither cleared nor redrawn (swrClearSettleBase).
typedef struct SWBaseKey {
    const void* texture;
    uint32_t serial;        // of the texture's runs (SWColorSpans), so a texture made again at the same address does not match
    int dx, dy, dw, dh, sx, sy, sw, sh;
    int32_t ystep;
    int flipY;
    int portX, portY, maxX, maxY;
} SWBaseKey;

typedef struct SWDirtyBuffer {
    const uintpixel_t* fb;  // the buffer this describes (NULL: unused)
    int width, height;
    uint32_t generation;    // swrDirtyGeneration when this was last known
    uint32_t lastUsed;
    bool known;             // every pixel outside the marked tiles is `color`
    uintpixel_t color;
    uint32_t rows[SWR_DIRTY_MAX_ROWS]; // bit c of rows[r]: tile (c, r) may hold something else
    // A recorded first draw (see SWBaseKey): baseRows are the tiles that hold
    // only it on the colour (not in rows, which is everything else); keep is,
    // while it is being drawn again, the tiles that already hold it.
    bool baseValid;
    SWBaseKey baseKey;
    uint32_t baseRows[SWR_DIRTY_MAX_ROWS];
    uint32_t keep[SWR_DIRTY_MAX_ROWS];
} SWDirtyBuffer;

// Bit counts for the tile masks; the builtins are missing from TinyCC, MSVC and old GCC.
#if (defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 4)) && !defined(__TINYC__)
#define swrBitCount(v) __builtin_popcount(v)
#define swrLowestBit(v) __builtin_ctz(v)
#else
FORCE_INLINE int swrBitCount(uint32_t v)
{
    int n = 0;
    while (v != 0) { v &= v - 1; n++; }
    return n;
}

// v must not be 0.
FORCE_INLINE int swrLowestBit(uint32_t v)
{
    int n = 0;
    while (!(v & 1u)) { v >>= 1; n++; }
    return n;
}
#endif

FORCE_INLINE uint32_t swrDirtyMask(int x0, int x1)
{
    int c0 = x0 >> SWR_DIRTY_TILE_W_SHIFT, c1 = (x1 - 1) >> SWR_DIRTY_TILE_W_SHIFT;
    return (0xFFFFFFFFu >> (31 - c1)) & ~((1u << c0) - 1);
}

// Marks pixels [x0, x1) of row y of the target, if the target is the main buffer.
FORCE_INLINE void swrDirtyRowIn(SWRenderer* swr, const uintpixel_t* target, int y, int x0, int x1)
{
    SWDirtyBuffer* d = swr->dirtyMark;
    if (d == NULL || target != swr->mainFb || y < 0 || y >= d->height) return;
    if (x0 < 0) x0 = 0;
    if (x1 > d->width) x1 = d->width;
    if (x0 >= x1) return;
    d->rows[y >> SWR_DIRTY_TILE_H_SHIFT] |= swrDirtyMask(x0, x1);
}
#define swrDirtyRow(swr, y, x0, x1) swrDirtyRowIn((swr), (swr)->fb, (y), (x0), (x1))

// Marks the rectangle [x0, x1) x [y0, y1) of the target, if it is the main buffer.
FORCE_INLINE void swrDirtyRectIn(SWRenderer* swr, const uintpixel_t* target, int x0, int y0, int x1, int y1)
{
    SWDirtyBuffer* d = swr->dirtyMark;
    if (d == NULL || target != swr->mainFb) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > d->width) x1 = d->width;
    if (y1 > d->height) y1 = d->height;
    if (x0 >= x1 || y0 >= y1) return;
    // The whole buffer: nothing more to mark until it is cleared again. A
    // backdrop over the whole screen (as every Deltarune room has) so stops
    // the sprites over it paying for marks.
    if (x0 == 0 && y0 == 0 && x1 == d->width && y1 == d->height) swr->dirtyMark = NULL;
    uint32_t mask = swrDirtyMask(x0, x1);
    for (int r = y0 >> SWR_DIRTY_TILE_H_SHIFT, last = (y1 - 1) >> SWR_DIRTY_TILE_H_SHIFT; r <= last; r++) d->rows[r] |= mask;
}
#define swrDirtyRect(swr, x0, y0, x1, y1) swrDirtyRectIn((swr), (swr)->fb, (x0), (y0), (x1), (y1))
#define swrDirtyAll(swr) swrDirtyRectIn((swr), (swr)->mainFb, 0, 0, SWR_DIRTY_MAX_W, SWR_DIRTY_MAX_ROWS << SWR_DIRTY_TILE_H_SHIFT)

// Fills the whole main buffer with one colour: only what needs it (see above).
void swrClearMain(SWRenderer* swr, uintpixel_t color);
int swrClearSettleBase(SWRenderer* swr, const SWBaseKey* key);
void swrBaseFinish(SWRenderer* swr, bool drawn);

// Inlined function definitions included below
#include "sw_pixel_calc.h"
#include "sw_inlined.h"
#include "sw_transform.h"

// Fills `count` pixels with one colour, two at a time where it can.
FORCE_INLINE void swrFillPixels(uintpixel_t* dst, size_t count, uintpixel_t color)
{
#if PIXEL_SIZE == 16
    if (count > 0 && ((uintptr_t) dst & 2)) {
        *dst++ = color;
        count--;
    }
    uint32_t pair = ((uint32_t) color << 16) | color;
    uint32_t* dst32 = (uint32_t*) (void*) dst;
    size_t pairs = count / 2;
    for (size_t i = 0; i < pairs; i++)
        dst32[i] = pair;
    if (count & 1)
        dst[count - 1] = color;
#else
    for (size_t i = 0; i < count; i++)
        dst[i] = color;
#endif
}

#include "sw_texture.h"
#include "sw_surface.h"
#include "sw_drawing.h"
#include "sw_texture_lru.h"
#include "sw_text.h"

#endif//_SW_RENDERER_PRIVATE_H
