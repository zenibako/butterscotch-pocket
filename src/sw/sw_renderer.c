#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <float.h>
#include <assert.h>
#include "text_utils.h"
#include "image/image_decoder.h"

#include "sw_renderer_private.h"
#include "sw_texture_pack.h"
#include "gettime.h"
#include "vm_builtins.h"

#define MAX_TRIS 4096
#define VERTICES_PER_TRIANGLE 3
#define VERTICES_PER_QUAD 4

void platformSetNextFramebuffer(uintpixel_t* framebuffer, int width, int height, int bpp);

static void swrFlushPendingClear(SWRenderer* swr);
static int swrPendingCount = 0; // tile pictures being held (see the tile run cache)
static SWRenderer* swrPendingOwner = NULL;
static unsigned swrTileRunReaders; // grid replay can request texture-memory eviction
// A translucent fill of the whole screen that the held pictures are drawn
// through as they come out (see SWRenderer_drawRectangle); 0 if none.
static uint32_t swrPendingTint = 0;

// ===[ Call notes ]===
// See sw_call_notes.h.
#include "sw_call_notes.h"
bool swrCallNotes = false;
bool swrDrawTimed = true;
int swrProbeStop = 0, swrProbePath = 0;
SWRasterPath swrLastPipeRasterPath;
static SWPipeRasterNotes swrPipeRasterNotes;

void swrPipeRasterTake(SWPipeRasterNotes* out)
{
    if (out != NULL) *out = swrPipeRasterNotes;
    memset(&swrPipeRasterNotes, 0, sizeof(swrPipeRasterNotes));
}

#ifdef SW_DRAW_PROFILE
static bool swrProbing = false;   // no counting of calls, and so no audio top-ups, while the probe times them
static int32_t swrProbeGridSprite = -1;
#include "gettime.h"
#define SWR_NOTE_MAX 192
// A call is noted as numbers and only put into words when the notes are
// taken: formatting a description per call cost more than most calls did
// (about 0.15 ms each on the device), which the benchmark then measured.
enum { SWR_NOTE_NAMED, SWR_NOTE_SPRITE, SWR_NOTE_TILED, SWR_NOTE_RECT };
enum { SWR_NOTE_TURNED = 1, SWR_NOTE_TINTED = 2, SWR_NOTE_OUTLINE = 4 };
typedef struct {
    uint8_t kind, flags;
    int16_t alpha;      // in hundredths
    int32_t a, b, c, d; // sprite: width, height, tenths of x scale. tiled: its tpag. rect: x, y, width, height
    uint32_t color;
    const char* name;   // SWR_NOTE_NAMED: a string that outlives the notes
} SWNoteKey;
static struct { SWNoteKey key; uint32_t hash, calls; uint64_t nanos; } swrNotes[SWR_NOTE_MAX];
static int swrNoteCount = 0;
static uint32_t swrNoteMissed = 0;

static void swrNoteKeyed(uint64_t nanos, const SWNoteKey* key)
{
    uint32_t hash = 2166136261u;
    hash = (hash ^ ((uint32_t) key->kind | (uint32_t) key->flags << 8 | (uint32_t) (uint16_t) key->alpha << 16)) * 16777619u;
    hash = (hash ^ (uint32_t) key->a) * 16777619u;
    hash = (hash ^ (uint32_t) key->b) * 16777619u;
    hash = (hash ^ (uint32_t) key->c) * 16777619u;
    hash = (hash ^ (uint32_t) key->d) * 16777619u;
    hash = (hash ^ key->color) * 16777619u;
    hash = (hash ^ (uint32_t) (uintptr_t) key->name) * 16777619u;
    for (int i = 0; i < swrNoteCount; i++) {
        const SWNoteKey* have = &swrNotes[i].key;
        if (swrNotes[i].hash != hash || have->kind != key->kind || have->flags != key->flags || have->alpha != key->alpha || have->a != key->a ||
            have->b != key->b || have->c != key->c || have->d != key->d || have->color != key->color || have->name != key->name) continue;
        swrNotes[i].calls++;
        swrNotes[i].nanos += nanos;
        return;
    }
    int slot = swrNoteCount;
    if (swrNoteCount == SWR_NOTE_MAX) {
        // Full: the description with the least time makes way, so that ones
        // seen once (a bar a pixel longer every frame) do not crowd out the rest.
        slot = 0;
        for (int i = 1; i < SWR_NOTE_MAX; i++) if (swrNotes[i].nanos < swrNotes[slot].nanos) slot = i;
        swrNoteMissed += swrNotes[slot].calls;
    } else {
        swrNoteCount++;
    }
    swrNotes[slot].key = *key;
    swrNotes[slot].hash = hash;
    swrNotes[slot].calls = 1;
    swrNotes[slot].nanos = nanos;
}

static void swrNote(uint64_t nanos, const char* name)
{
    SWNoteKey key = { .kind = SWR_NOTE_NAMED, .name = name };
    swrNoteKeyed(nanos, &key);
}

static void swrNoteWords(const SWNoteKey* key, char* what, size_t size)
{
    switch (key->kind) {
        case SWR_NOTE_SPRITE:
            snprintf(what, size, "sprite %dx%d x%d.%d a%d%s%s", (int) key->a, (int) key->b, (int) key->c / 10, abs((int) key->c) % 10, (int) key->alpha,
                     (key->flags & SWR_NOTE_TURNED) ? " turned" : "", (key->flags & SWR_NOTE_TINTED) ? " tinted" : "");
            break;
        case SWR_NOTE_TILED:
            snprintf(what, size, "tiled tpag %d a%d", (int) key->a, (int) key->alpha);
            break;
        case SWR_NOTE_RECT:
            snprintf(what, size, "rect %d,%d %dx%d a%d c%06x%s", (int) key->a, (int) key->b, (int) key->c, (int) key->d, (int) key->alpha,
                     (unsigned) key->color, (key->flags & SWR_NOTE_OUTLINE) ? " outline" : "");
            break;
        default:
            snprintf(what, size, "%s", key->name != nullptr ? key->name : "?");
            break;
    }
}

int swrCallNotesTake(SWCallNote* out, int max, uint32_t* missed)
{
    int taken = 0;
    while (taken < max) {
        int heaviest = -1;
        for (int i = 0; i < swrNoteCount; i++) {
            if (swrNotes[i].calls != 0 && (heaviest < 0 || swrNotes[i].nanos > swrNotes[heaviest].nanos)) heaviest = i;
        }
        if (heaviest < 0) break;
        swrNoteWords(&swrNotes[heaviest].key, out[taken].what, sizeof(out[taken].what));
        out[taken].calls = swrNotes[heaviest].calls;
        out[taken].nanos = swrNotes[heaviest].nanos;
        taken++;
        swrNotes[heaviest].calls = 0;
    }
    if (missed != NULL) *missed = swrNoteMissed;
    swrNoteCount = 0;
    swrNoteMissed = 0;
    return taken;
}
// Times a statement that is part of some call, under a name of its own.
#define SWR_NOTED(what, statement) do { \
        bool noted_ = swrCallNotes && swrDrawTimed; \
        uint64_t noteStart_ = noted_ ? nowNanos() : 0; \
        statement; \
        if (noted_) swrNote(nowNanos() - noteStart_, what); \
    } while (0)
#else
int swrCallNotesTake(SWCallNote* out, int max, uint32_t* missed) { (void) out; (void) max; if (missed != NULL) *missed = 0; return 0; }
#define SWR_NOTED(what, statement) do { statement; } while (0)
#endif

// The same, for a stretch of code that a macro argument cannot hold: start
// it with SWR_NOTE_START(name) and end it with SWR_NOTE_END(name, "what").
#ifdef SW_DRAW_PROFILE
#define SWR_NOTE_START(var) uint64_t var = swrCallNotes && swrDrawTimed ? nowNanos() : 0
#define SWR_NOTE_END(var, what) do { if (var != 0) swrNote(nowNanos() - var, what); } while (0)
#else
#define SWR_NOTE_START(var) do { } while (0)
#define SWR_NOTE_END(var, what) do { } while (0)
#endif

// Partial clears (see SWDirtyBuffer). Bumped by swrFramebufferTouched: a
// buffer last known under an older generation is not known any more.
static uint32_t swrDirtyGeneration;
static uint32_t swrDirtyUses;
uint32_t swrDirtyLastTiles;

void swrFramebufferTouched(void)
{
    swrDirtyGeneration++;
}

int swrFillExtendRows = 0;

// swrFillExtendRows for a fill: when the rectangle covers the whole width of the main buffer and the rows from the
// top band's edge to the bottom band's, without covering the whole height, moves its top and bottom edges out to
// the buffer's. The edges are given in room coordinates, so the buffer's rows are mapped back the same way.
static void swrExtendFill(SWRenderer* swr, float x1, float* y1, float x2, float* y2)
{
    int band = swrFillExtendRows;
    if (band <= 0 || swr->fb != swr->mainFb || swr->scaleY == 0.0f || swr->portY != 0) return;
    float sx1 = x1, sy1 = *y1, sx2 = x2, sy2 = *y2;
    swrTransformPosIfNeeded(swr, &sx1, &sy1);
    swrTransformPosIfNeeded(swr, &sx2, &sy2);
    float left = sx1 < sx2 ? sx1 : sx2, right = sx1 < sx2 ? sx2 : sx1;
    float top = sy1 < sy2 ? sy1 : sy2, bottom = sy1 < sy2 ? sy2 : sy1;
    int width = swr->mainWidth, height = swr->mainHeight;
    // A fill covers columns floor(left) up to, not including, ceil(right) (swrFillRectangle), so only one that
    // reaches column width - 1 covers the whole width.
    if (left >= 1.0f || right <= (float) width - 1.0f) return;
    if (top > (float) band + 0.5f || bottom < (float) (height - band) - 1.5f) return;
    if (top <= 0.5f && bottom >= (float) height - 1.5f) return;
    float roomTop = (0.0f - (float) swr->portY) / swr->scaleY + swr->viewY;
    float roomBottom = ((float) height - (float) swr->portY) / swr->scaleY + swr->viewY;
    if (*y1 <= *y2) { *y1 = roomTop; *y2 = roomBottom; }
    else { *y1 = roomBottom; *y2 = roomTop; }
}

// Picks up what is known of the platform buffer about to be drawn in.
MAYBE_UNUSED static void swrDirtyAcquire(SWRenderer* swr, const uintpixel_t* fb, int width, int height)
{
    swrBaseFinish(swr, false);
    swr->dirty = swr->dirtyMark = NULL;
    static int fullClear = -1;
    if (fullClear < 0) fullClear = getenv("BS_FULL_CLEAR") != NULL;
    if (fullClear) return;
    if (width > SWR_DIRTY_MAX_W || height > (SWR_DIRTY_MAX_ROWS << SWR_DIRTY_TILE_H_SHIFT)) return;
    if (swr->dirtyBuffers == NULL) {
        swr->dirtyBuffers = (SWDirtyBuffer*) calloc(SWR_DIRTY_BUFFERS, sizeof(SWDirtyBuffer));
        if (swr->dirtyBuffers == NULL) return;
    }
    SWDirtyBuffer* found = NULL;
    SWDirtyBuffer* oldest = &swr->dirtyBuffers[0];
    for (int i = 0; i < SWR_DIRTY_BUFFERS; i++) {
        SWDirtyBuffer* d = &swr->dirtyBuffers[i];
        // A buffer of another size was drawn in with another layout: what
        // was known of every buffer is gone.
        if (d->fb != NULL && (d->width != width || d->height != height)) {
            d->known = false;
            d->tilePicture.count = 0;
        }
        if (d->fb == fb && d->width == width && d->height == height) found = d;
        if (d->lastUsed < oldest->lastUsed) oldest = d;
    }
    if (found == NULL) {
        found = oldest;
        found->fb = fb;
        found->known = false;
        found->tilePicture.count = 0;
    }
    if (found->generation != swrDirtyGeneration) {
        found->known = false;
        found->tilePicture.count = 0;
    }
    found->width = width;
    found->height = height;
    found->generation = swrDirtyGeneration;
    found->lastUsed = ++swrDirtyUses;
    swr->dirty = swr->dirtyMark = found;
}

// BS_CHECK_CLEAR=1 (a desktop check, slow): before each partial clear, every
// pixel outside the marked tiles must already be the colour, or some write
// went unmarked; that is logged with where it is, and the run stops.
static void swrDirtyCheck(const SWDirtyBuffer* d, const uintpixel_t* fb, uintpixel_t color);
static void swrDirtyCheckKept(const SWDirtyBuffer* d, const uintpixel_t* fb, uintpixel_t color) { swrDirtyCheck(d, fb, color); }

static void swrDirtyCheck(const SWDirtyBuffer* d, const uintpixel_t* fb, uintpixel_t color)
{
    for (int y = 0; y < d->height; y++) {
        uint32_t marked = d->rows[y >> SWR_DIRTY_TILE_H_SHIFT] | d->baseRows[y >> SWR_DIRTY_TILE_H_SHIFT];
        for (int x = 0; x < d->width; x++) {
            if (marked & (1u << (x >> SWR_DIRTY_TILE_W_SHIFT))) continue;
            if (fb[y * d->width + x] != color) {
                logError("swr: unmarked write at %d,%d in a %dx%d buffer: %04x, cleared to %04x\n", x, y, d->width,
                         d->height, (unsigned) fb[y * d->width + x], (unsigned) color);
                abort();
            }
        }
    }
}

static uint64_t swrFillTiles(SWRenderer* swr, const SWDirtyBuffer* d, const uint32_t* mask, const uint32_t* also, uintpixel_t color);

void swrClearMain(SWRenderer* swr, uintpixel_t color)
{
    SWDirtyBuffer* d = swr->dirty;
    if (d == NULL || d->fb != swr->mainFb || d->width != swr->mainWidth || d->height != swr->mainHeight) {
        swrFillPixels(swr->mainFb, (size_t) swr->mainWidth * swr->mainHeight, color);
        return;
    }
    static int check = -1;
    if (check < 0) check = getenv("BS_CHECK_CLEAR") != NULL;
    static uint32_t partial, whole;
    static uint64_t written;
    if (check && (partial + whole) % 600 == 599)
        logInfo("swr: clears %u partial, %u whole; partial ones wrote %u%% of a screen on average\n", partial, whole,
                partial ? (unsigned) (written * 100 / ((uint64_t) partial * d->width * d->height)) : 0);
    if (d->known && d->color == color) {
        if (check) swrDirtyCheck(d, swr->mainFb, color);
        partial++;
        written += swrFillTiles(swr, d, d->rows, d->baseRows, color);
    } else {
        whole++;
        swrFillPixels(swr->mainFb, (size_t) swr->mainWidth * swr->mainHeight, color);
    }
    d->known = true;
    d->color = color;
    uint32_t tiles = 0;
    for (int r = 0; r < SWR_DIRTY_MAX_ROWS; r++) tiles += (uint32_t) swrBitCount(d->rows[r] | d->baseRows[r]);
    swrDirtyLastTiles = tiles;
    memset(d->rows, 0, sizeof(d->rows));
    memset(d->baseRows, 0, sizeof(d->baseRows));
    d->baseValid = false;
    d->tilePicture.count = 0;
    swr->dirtyMark = d;
}

// Fills the tiles of the main buffer marked in either mask with the colour; returns the pixels written.
static uint64_t swrFillTiles(SWRenderer* swr, const SWDirtyBuffer* d, const uint32_t* mask, const uint32_t* also, uintpixel_t color)
{
    uint64_t written = 0;
    {
        int tileRows = (d->height + (1 << SWR_DIRTY_TILE_H_SHIFT) - 1) >> SWR_DIRTY_TILE_H_SHIFT;
        for (int r = 0; r < tileRows; r++) {
            uint32_t bits = mask[r] | (also != NULL ? also[r] : 0);
            int y0 = r << SWR_DIRTY_TILE_H_SHIFT, y1 = y0 + (1 << SWR_DIRTY_TILE_H_SHIFT);
            if (y1 > d->height) y1 = d->height;
            while (bits != 0) {
                // One run of marked tiles at a time.
                int c0 = swrLowestBit(bits);
                int c1 = c0;
                while (c1 < 32 && (bits & (1u << c1))) c1++;
                bits &= c1 >= 32 ? 0 : ~((1u << c1) - 1);
                int x0 = c0 << SWR_DIRTY_TILE_W_SHIFT, x1 = c1 << SWR_DIRTY_TILE_W_SHIFT;
                if (x1 > d->width) x1 = d->width;
                for (int y = y0; y < y1; y++)
                    swrFillPixels(&swr->mainFb[(size_t) y * d->width + x0], (size_t) (x1 - x0), color);
                written += (uint64_t) (y1 - y0) * (uint64_t) (x1 - x0);
            }
        }
    }
    return written;
}

#ifdef SW_PLATFORM_FRAMEBUFFER
// Optional: lets the platform hand out the buffer each frame is drawn into
// (typically the display's back buffer) so a finished frame does not have to
// be copied to the screen. Must return width * height pixels with no row
// padding, or NULL to make the renderer use its own buffer. It is called at
// the start of every frame and may return a different buffer each time; the
// contents are undefined, the renderer clears them.
uintpixel_t* platformAcquireFramebuffer(int width, int height);

static bool swrAcquirePlatformFramebuffer(SWRenderer* swr, int width, int height)
{
    uintpixel_t* fb = platformAcquireFramebuffer(width, height);
    if (!fb) return false;
    
    if (!swr->fbIsPlatform) free(swr->mainFb);
    swr->fbIsPlatform = true;
    swr->fb = swr->mainFb = fb;
    swr->fbPitch = width;
    swr->width = swr->mainWidth = width;
    swr->height = swr->mainHeight = height;
    swrDirtyAcquire(swr, fb, width, height);
    return true;
}
#endif

static void SWRenderer_gpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha);

static void SWRenderer_init(Renderer* renderer, DataWin* dataWin)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    renderer->dataWin = dataWin;
    
    //allocate texture buffer
    swr->textureCount = dataWin->txtr.count;
    swr->surfaceCount = SURFACE_MAX_COUNT;
    swr->totalTextureCount = swr->textureCount + swr->surfaceCount;
    swr->textures = (SWTexture**) safeCalloc(swr->totalTextureCount, sizeof(SWTexture*));
    swr->surfaces = (SWSurface**) safeCalloc(swr->surfaceCount, sizeof(SWSurface*));
    
    //HACK: this isn't good, really.  This should seriously be refactored.
    //expand datawin's tpag items list to include our surface count.
    swr->originalTPagCount = dataWin->tpag.count;
    dataWin->tpag.items = (TexturePageItem*) safeRealloc(dataWin->tpag.items, sizeof(TexturePageItem) * (dataWin->tpag.count + swr->surfaceCount));
    dataWin->tpag.count += swr->surfaceCount;
    
    swr->originalSpriteCount = dataWin->sprt.count;
    
    for (size_t i = swr->originalTPagCount; i < dataWin->tpag.count; i++)
    {
        memset(&dataWin->tpag.items[i], 0, sizeof(TexturePageItem));
        dataWin->tpag.items[i].texturePageId = -1;
    }
    
    // Allocate vertex data for primitive support
    swr->maxVertexCount = MAX_TRIS * VERTICES_PER_QUAD;
    swr->vertexData = (SWVertex*) safeCalloc(swr->maxVertexCount, sizeof(SWVertex));
    swr->vertexCount = 0;
    swr->primitiveType = 0;
    swr->primitiveBegun = false;
    
    logInfo("SWRenderer initialized.\n");
}

static void SWRenderer_destroy(Renderer* renderer)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (swr->pipeCount != 0) swrOverlayFlush(swr);
    VMBuiltins_resetD3D(renderer);

    swr->primitiveBegun = false;
    swr->primitiveOverflow = false;
    swr->primitiveType = -1;
    swr->vertexCount = 0;
    
    for (size_t i = 0; i < swr->surfaceCount; i++)
    {
        swrFreeSurface(swr->surfaces[i]);
    }
    free(swr->surfaces);
    swr->surfaceCount = 0;
    {
    for (size_t i = 0; i < swr->totalTextureCount; i++)
    {
        swrFreeTexture(swr->textures[i]);
    }
    }
    swrFreeItemTextures(swr);
    swrFreeTexture(swr->floweyPipeBackdrop);
    swrFreeTexture(swr->floweyStalkEyePipes);
    for (unsigned pose = 0; pose < SWR_FLOWEY_STALK_POSES; ++pose)
        swrFreeTexture(swr->floweyStalkPoses[pose]);
    free(swr->textures);
    swr->textureCount = 0;
    swr->totalTextureCount = 0;
    free(swr->vertexData);
    free(swr->hintedGlyphs);
    free(swr->contrastItems);
    free(swr->contrastFonts);
    free(swr->dirtyBuffers);
    swr->dirtyBuffers = swr->dirty = swr->dirtyMark = NULL;
    
    if (!swr->fbIsPlatform) free(swr->mainFb);
    swr->fb = swr->mainFb = NULL;
    
    free(swr);
    logInfo("SWRenderer destroyed.\n");
}

static void SWRenderer_beginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->frameCounter++;
    swr->gameW = gameW;
    swr->gameH = gameH;
    swr->drawingToSurface = false;
    swr->blendMode = bm_normal;

#ifdef SW_PLATFORM_FRAMEBUFFER
    {
        bool resized = swr->width != windowW || swr->height != windowH;
        if (swrAcquirePlatformFramebuffer(swr, windowW, windowH)) {
            // clearFrameBuffer ran before this frame's size was known.
            if (resized) swrClearMain(swr, swrConvertPixel(0));
            return;
        }
        if (swr->fbIsPlatform) {
            // The platform stopped providing a buffer: go back to our own.
            swr->fbIsPlatform = false;
            swr->fb = swr->mainFb = NULL;
            swr->width = swr->height = 0;
            swr->dirty = swr->dirtyMark = NULL;
        }
    }
#endif

    if (swr->width != windowW || swr->height != windowH)
    {
        //allocate frame buffer
        free(swr->fb);
        swr->fb = (uintpixel_t*) safeMalloc(windowW * windowH * sizeof(uintpixel_t));
        swr->fbPitch = windowW;
        swr->width = windowW;
        swr->height = windowH;
        
        swr->mainFb = swr->fb;
        swr->mainWidth = swr->width;
        swr->mainHeight = swr->height;
    }
    
#ifdef SW_DEBUG_FRAME_DRAW_BOUNDS
    logDebug("swr: begin drawing frame\n");
#endif
}

// This used to be just one, "endFrame". Not sure what the difference is.
static void SWRenderer_endFrameInit(Renderer* renderer)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void) renderer;
    
    //this is kinda useless to do twice isn't it?
}

static void SWRenderer_endFrameEnd(Renderer* renderer)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    assert(!swr->drawingToSurface);
    
#ifdef SW_DEBUG_FRAME_DRAW_BOUNDS
    logDebug("swr: end drawing frame\n");
#endif
    
    // Nothing cleared the frame itself: do it now rather than present stale pixels.
    if (swrSkipFrame) swr->pendingClear = false;
    swrFlushPendingClear(swr);
    
    platformSetNextFramebuffer(swr->fb, swr->width, swr->height, PIXEL_SIZE);

    swr->primitiveOverflow = false;
}

static void SWRenderer_beginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
                                 int32_t portX, int32_t portY, int32_t portW, int32_t portH, float viewAngle)
{
    swrOverlayFlushForState((SWRenderer*) renderer);
    (void)renderer; (void)viewX; (void)viewY; (void)viewW; (void)viewH;
    (void)portX; (void)portY; (void)portW; (void)portH; (void)viewAngle;
    UNIMP2();
    
    SWRenderer* swr = (SWRenderer*) renderer;
    
    float xratio, yratio;
    
    float portviewX = (float) portW / viewW;
    float portviewY = (float) portH / viewH;
    
    int offsetX = 0, offsetY = 0;
    
    if (swr->drawingToSurface) {
        UNIMP();
        xratio = 1.0f;
        yratio = 1.0f;
        portX = (int)(portX * xratio);
        portY = (int)(portY * yratio);
    }
    else {
        float scaleX = (float) swr->width  / swr->gameW;
        float scaleY = (float) swr->height / swr->gameH;
        float scale  = (scaleX < scaleY) ? scaleX : scaleY;

        int32_t scaledW = (int32_t)(swr->gameW * scale);
        int32_t scaledH = (int32_t)(swr->gameH * scale);
        
        offsetX = (swr->width  - scaledW) / 2;
        offsetY = (swr->height - scaledH) / 2;

        xratio = scale;
        yratio = scale;

        portX = (int)(portX * xratio) + offsetX;
        portY = (int)(portY * yratio) + offsetY;
    }
    
    swr->scaleX = xratio * portviewX;
    swr->scaleY = yratio * portviewY;
    swr->offsetX = offsetX;
    swr->offsetY = offsetY;
    swr->defaultScaleX = xratio;
    swr->defaultScaleY = yratio;
    
    portW = (int)(portW * xratio);
    portH = (int)(portH * yratio);
    
    swr->viewActive = true;
    swr->viewX = viewX;
    swr->viewY = viewY;
    swr->viewW = viewW;
    swr->viewH = viewH;
    swr->portX = portX;
    swr->portY = portY;
    swr->portW = portW;
    swr->portH = portH;
    swr->maxX = portX + portW;
    swr->maxY = portY + portH;
}

static void SWRenderer_endView(Renderer* renderer)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void)renderer;
    UNIMP2();
    
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->viewActive = false;
    
    swr->viewX = 0;
    swr->viewY = 0;
    swr->portX = 0;
    swr->portY = 0;
    swr->portW = swr->viewW = swr->width;
    swr->portH = swr->viewH = swr->height;
    swr->maxX = swr->portX + swr->portW;
    swr->maxY = swr->portY + swr->portH;
    swr->scaleX = swr->defaultScaleX;
    swr->scaleY = swr->defaultScaleY;
}

static void SWRenderer_beginGUI(Renderer* renderer, int32_t guiW, int32_t guiH,
                                int32_t portX, int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId)
{
    swrOverlayFlush((SWRenderer*) renderer);
    swrSwitchToSurface(renderer, targetSurfaceId, false);
    
    (void)guiW; (void)guiH;
    (void)portX; (void)portY; (void)portW; (void)portH;
    (void)targetSurfaceId;
    UNIMP2();
}

static void SWRenderer_setGuiProjection(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portW, int32_t portH, bool renderingToUserSurface)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void) renderer;
    (void) guiW; (void) guiH;
    (void) portW; (void) portH;
    (void) renderingToUserSurface;
    UNIMP();
}

static void SWRenderer_endGUI(Renderer* renderer)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void)renderer;
    UNIMP2();
}

static bool swrDrawFloweySideStalk(SWRenderer* swr, int32_t item, float x, float y,
                                  float originX, float originY, float xscale, uint32_t color)
{
    DataWin* dw = swr->base.dataWin;
    const Sprite* sprite = &dw->sprt.sprites[swr->floweyStalkSprite];
    for (uint32_t frame = 0; frame < sprite->textureCount; ++frame) {
        if (sprite->tpagIndices[frame] != item) continue;
        /* Hold one visual pose for six animation frames, while the draw event
         * continues to advance the authored animation and its position. */
        unsigned pose = frame / SWR_FLOWEY_STALK_HOLD;
        int32_t held = sprite->tpagIndices[pose * SWR_FLOWEY_STALK_HOLD];
        const TexturePageItem* tpag = &dw->tpag.items[held];
        float dx = x + (tpag->targetX - originX) * xscale;
        float dy = y + (tpag->targetY - originY);
        if (!(swr->floweyStalkLoadTried & (1u << pose))) {
            swr->floweyStalkLoadTried |= (uint16_t) (1u << pose);
            swr->floweyStalkPoses[pose] = swrLoadPackedFloweyStalk(swr, pose, tpag->sourceWidth / 2, tpag->sourceHeight / 2);
        }
        SWTexture* packed = swr->floweyStalkPoses[pose];
        if (packed != NULL && swrDrawPackedFloweyStalk(&swr->base, packed, dx, dy, xscale < 0.0f, color))
            return true;
        SWTexture* texture = swrTextureForItem(swr, held);
        if (texture == NULL) return false;
        return swrDrawFloweyStalkHalf(&swr->base, texture, dx, dy,
            tpag->sourceX - texture->originX, tpag->sourceY - texture->originY,
            tpag->sourceWidth, tpag->sourceHeight, xscale < 0.0f, color, pose);
    }
    return false;
}

static SWTexture* swrBuildFloweyPipeBackdrop(SWRenderer* swr, int32_t item, const TexturePageItem* tpag)
{
    SWTexture* source = swrTextureForItem(swr, item);
    if (source == NULL) return NULL;
    SWTexture* art = swrMakeFloweyPipeBackdrop(source, tpag->sourceX - source->originX, tpag->sourceY - source->originY);
    if (art == NULL) return NULL;
#if defined(OF_PC) && PIXEL_SIZE == 16 && !defined IS_BIG_ENDIAN
    unsigned layers = 0;
#endif
    for (unsigned layer = 0; layer < SWR_FLOWEY_BACKDROP_VINE_LAYERS; ++layer) {
        int32_t vineItem = swr->floweyBackdropItems[layer];
        if (vineItem < 0) continue;
        const TexturePageItem* vinePage = &swr->base.dataWin->tpag.items[vineItem];
        SWTexture* vine = swrTextureForItem(swr, vineItem);
        if (vine == NULL) {
            swrFreeTexture(art);
            return NULL;
        }
        swrAddFloweyBackdropVines(art, vine, vinePage->sourceX - vine->originX,
                                 vinePage->sourceY - vine->originY, vinePage, layer);
#if defined(OF_PC) && PIXEL_SIZE == 16 && !defined IS_BIG_ENDIAN
        ++layers;
#endif
    }
    int32_t segmentItem = swr->floweyPipeSegmentItem;
    if (segmentItem >= 0) {
        const TexturePageItem* segmentPage = &swr->base.dataWin->tpag.items[segmentItem];
        SWTexture* segment = swrTextureForItem(swr, segmentItem);
        if (segment != NULL) {
            int sx = segmentPage->sourceX - segment->originX;
            int sy = segmentPage->sourceY - segment->originY;
            swrAddFloweyBackdropPipeSegments(art, segment, sx, sy);
            swrAddFloweyEyePipes(art, segment, sx, sy);
#if defined(OF_PC) && PIXEL_SIZE == 16 && !defined IS_BIG_ENDIAN
            const char* path = getenv("BS_FLOWEY_BACKDROP_OUT");
            if (path != NULL && layers == SWR_FLOWEY_BACKDROP_VINE_LAYERS) {
                FILE* out = fopen(path, "wbx");
                if (out != NULL) {
                    size_t count = (size_t) art->width * art->height;
                    size_t written = fwrite(art->buffer, sizeof(*art->buffer), count, out);
                    if (fclose(out) != 0 || written != count) {
                        remove(path);
                        logError("SWR: Could not write Flowey backdrop to %s\n", path);
                    }
                } else {
                    logError("SWR: Could not create Flowey backdrop at %s\n", path);
                }
            }
#endif
        } else {
            swrFreeTexture(art);
            return NULL;
        }
    }
    return art;
}

static bool swrDrawFloweyPipeBackdrop(SWRenderer* swr, int32_t item, const TexturePageItem* tpag, uint32_t color)
{
    // The rear layer's normal tint is 60% white. During the intro it fades
    // up from black. Apply that fade to the whole neutral-pose composite.
    const uint32_t normalShade = 0x99;
    uint32_t shade = color & 0xff;
    if (shade > normalShade || color != shade * 0x010101) return false;
    if (swr->floweyPipeBackdrop == NULL) {
        swr->floweyPipeBackdrop = swrLoadPackedFloweyBackdrop(swr);
        if (swr->floweyPipeBackdrop == NULL)
            swr->floweyPipeBackdrop = swrBuildFloweyPipeBackdrop(swr, item, tpag);
        if (swr->floweyPipeBackdrop == NULL) return false;
    }
    SWTexture* art = swr->floweyPipeBackdrop;
    uint32_t fade = (shade * 255 + normalShade / 2) / normalShade;
    // Keep the static backdrop in screen space through the TV's camera shake.
    swrDrawSprite(&swr->base, swr->viewX, swr->viewY, art->width * 2, art->height * 2, art,
                  0, 0, art->width, art->height, fade * 0x010101, 1.0f);
    swr->floweyPipeFrame = swr->frameCounter;
    return true;
}

static bool swrFloweyScreenItem(const SWRenderer* swr, int32_t item)
{
    if (swr->floweyTvScreenSprite < 0) return false;
    const Sprite* screen = &swr->base.dataWin->sprt.sprites[swr->floweyTvScreenSprite];
    for (uint32_t frame = 0; frame < screen->textureCount; ++frame)
        if (screen->tpagIndices[frame] == item) return true;
    return false;
}

enum {
    FLOWEY_MOUTH_EDGE, FLOWEY_MOUTH_FLAP, FLOWEY_EYE_HOLDER, FLOWEY_EYE_BG, FLOWEY_EYE_OVERLAY
};

static void SWRenderer_drawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y,
                                  float originX, float originY, float xscale, float yscale,
                                  float angleDeg, uint32_t color, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    SWR_PROBE_STOP(1);
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dwin->tpag.count) {
        logError("%s: tpagIndex of %d is invalid\n", __func__, tpagIndex);
        return;
    }

    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) {
        logError("%s: tpagIndex of %d is invalid, as pageId of %d is invalid\n", __func__, tpagIndex, pageId);
        return;
    }
    bool floweySpeed = swr->floweySpeedRoom && swrSmoothMinify && swr->fb == swr->mainFb && !swr->drawingToSurface;
    if (floweySpeed) {
        if (tpagIndex == swr->floweyPipeItem) {
            // Submit at the first ordinary back-pipe call, in its original
            // place behind the boss and projectiles. Later layers are baked in.
            if (swr->floweyPipeFrame == swr->frameCounter) return;
            if (swr->blendMode == bm_normal &&
                fabsf(x - 400.0f) <= 1.01f && fabsf(y + 20.0f) <= 0.51f &&
                fabsf(xscale - 1.0f) <= 0.0051f && yscale == 1.0f && angleDeg == 0.0f &&
                alpha == 1.0f && originX == 0.0f && originY == 0.0f &&
                tpag->sourceWidth == 190 && tpag->sourceHeight == 181 &&
                tpag->targetX == 0 && tpag->targetY == 0 && tpag->targetWidth == 190 && tpag->targetHeight == 181 &&
                swr->portX == 0 && swr->portY == 0 && swr->maxX == 320 && swr->maxY == 240 &&
                swr->scaleX == 0.5f && swr->scaleY == 0.5f && swrDrawFloweyPipeBackdrop(swr, tpagIndex, tpag, color))
                return;
        }
        if (swr->floweyPipeBackdrop != NULL && swr->floweyPipeFrame == swr->frameCounter) {
            for (unsigned i = 0; i < swr->floweyBackdropCount; ++i)
                if (swr->floweyBackdropItems[i] == tpagIndex) return;
        }
        if (swr->blendMode == bm_normal) {
            if (tpagIndex == swr->floweyOuterEyeShadow && color == 0 && alpha == 1.0f) return;
            if (angleDeg != 0.0f && fabsf(angleDeg) <= 2.0f) {
                for (unsigned i = FLOWEY_MOUTH_EDGE; i <= FLOWEY_EYE_BG; ++i)
                    if (swr->floweyTrimItems[i] == tpagIndex) angleDeg = 0.0f;
            }
            if (swr->floweyStalkSprite >= 0 && tpagIndex >= swr->floweyStalkFirst &&
                tpagIndex <= swr->floweyStalkLast && swr->scaleX == 0.5f && swr->scaleY == 0.5f &&
                (xscale == 1.0f || xscale == -1.0f) && yscale >= 0.98f && yscale <= 1.02f &&
                // Keep using the held pose through the grayscale intro fade.
                angleDeg == 0.0f && color == (color & 0xff) * 0x010101 && alpha == 1.0f &&
                swrDrawFloweySideStalk(swr, tpagIndex, x, y, originX, originY, xscale, color)) return;
        }
        if (tpagIndex == swr->floweyVineItem && swr->blendMode == bm_normal &&
            alpha >= 0.0f && alpha < 1.0f) {
            // Keep the original art and reach. The vine can deal damage
            // while its alpha exceeds 0.3, so draw it solid until that ends.
            if (alpha <= 0.3f) return;
            alpha = 1.0f;
        }
    }
    SWTexture* texture = swrTextureForItem(swr, tpagIndex);
    if (!texture) {
        logError("%s: could not ensure texture is loaded, tpagIndex: %d, pageId: %d\n", __func__, tpagIndex, pageId);
        return;
    }
    SWR_PROBE_STOP(2);
    
    int sx = tpag->sourceX - texture->originX;
    int sy = tpag->sourceY - texture->originY;
    int sw = tpag->sourceWidth;
    int sh = tpag->sourceHeight;
    
    float dx = (float)(tpag->targetX - originX);
    float dy = (float)(tpag->targetY - originY);
    int dw = (int)(xscale * tpag->targetWidth);
    int dh = (int)(yscale * tpag->targetHeight);
    dx *= xscale;
    dy *= yscale;
    dx += x;
    dy += y;

    // Reuse the original rims and mouth trim upright through small rotations.
    if (floweySpeed && color == (color & 0xff) * 0x010101 && alpha == 1.0f &&
        ((tpagIndex == swr->floweyTrimItems[FLOWEY_EYE_OVERLAY] && fabsf(angleDeg) <= 4.0f) ||
         ((tpagIndex == swr->floweyTrimItems[FLOWEY_MOUTH_EDGE] ||
           tpagIndex == swr->floweyTrimItems[FLOWEY_MOUTH_FLAP] ||
           tpagIndex == swr->floweyTrimItems[FLOWEY_EYE_HOLDER]) && fabsf(angleDeg) <= 2.0f)) &&
        sw == tpag->targetWidth && sh == tpag->targetHeight &&
        swrDrawFloweyTrim(renderer, texture, dx, dy, dw, dh, sx, sy, sw, sh, color)) return;

    // The idle arm is a separate item from the attack's stem and strike.
    // The TV's warnings and flashes also use separate items or fills.
    if (floweySpeed && angleDeg == 0.0f && color == (color & 0xff) * 0x010101 && alpha == 1.0f &&
        sw == tpag->targetWidth && sh == tpag->targetHeight) {
        int kind = SWR_FLOWEY_ART_NONE;
        if (tpagIndex == swr->floweyArmItem && sw == 171 && sh == 275 &&
            (xscale == 1.0f || xscale == -1.0f) && yscale >= 0.96f && yscale <= 1.04f)
            kind = SWR_FLOWEY_ART_ARM;
        else if (tpagIndex == swr->floweyTvCaseItem && sw == 155 && sh == 167 && xscale == 1.0f && yscale == 1.0f)
            kind = SWR_FLOWEY_ART_CASE;
        else if (sw == 90 && sh == 105 && xscale >= 1.39f && xscale <= 1.51f &&
                 yscale >= 0.89f && yscale <= 1.01f && swrFloweyScreenItem(swr, tpagIndex))
            kind = SWR_FLOWEY_ART_SCREEN;
        if (kind != SWR_FLOWEY_ART_NONE &&
            swrDrawFloweyArt(renderer, texture, dx, dy, dw, dh, sx, sy, sw, sh, kind, color)) return;
    }
    
    if (UNLIKELY(swrMustRotate(angleDeg)))
    {
        float pivotX = (x - dx) * swrSgn(xscale);
        float pivotY = (y - dy) * swrSgn(yscale);
        
        if (tpag->targetWidth != tpag->sourceWidth)
            pivotX *= (float)tpag->targetWidth / tpag->sourceWidth;
        if (tpag->targetHeight != tpag->sourceHeight)
            pivotY *= (float)tpag->targetHeight/ tpag->sourceHeight;
        
        swrOverlayFlush(swr);
        swrDrawSpriteRotated(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha, angleDeg, pivotX, pivotY,
                             floweySpeed && tpagIndex == swr->floweyVineItem);
    }
    else
    {
        // Only a plain sprite draw may be held back for merging; see swrOverlayFlush.
        swr->overlayMergeAllowed = true;
        bool hinted = false;
        for (size_t i = 0; i < swr->hintedGlyphCount; ++i) {
            if (swr->hintedGlyphs[i].item == tpagIndex) {
                hinted = swrDrawHintedGlyph(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh,
                                             color, alpha, swr->hintedGlyphs[i].asset);
                break;
            }
        }
        if (hinted) { swr->overlayMergeAllowed = false; return; }
        if ((uint32_t) tpagIndex < swr->contrastItemCount && swr->contrastItems[tpagIndex])
            swrDrawContrastSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha, swr->contrastItems[tpagIndex]);
        else
            swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha);
        swr->overlayMergeAllowed = false;
    }
}

static void SWRenderer_drawSpritePart(Renderer* renderer, int32_t tpagIndex,
                                      float srcOffXf, float srcOffYf, float srcWf, float srcHf,
                                      float x, float y, float xscale, float yscale, float angleDeg,
                                      float pivotX, float pivotY, uint32_t color, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    // The interface allows fractional source rectangles; this renderer samples whole texels.
    int32_t srcOffX = (int32_t) srcOffXf, srcOffY = (int32_t) srcOffYf, srcW = (int32_t) srcWf, srcH = (int32_t) srcHf;
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;
    
    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dwin->tpag.count) return;
    
    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) return;
    
    // An item stored at another size than it is drawn has its page position
    // rescaled below, which only means something on the whole page.
    bool rescaled = tpag->sourceWidth != tpag->targetWidth || tpag->sourceHeight != tpag->targetHeight;
    SWTexture* texture = NULL;
    if (!rescaled)
        texture = swrTextureForItem(swr, tpagIndex);
    else if (swrEnsureTextureIsLoaded(swr, (uint32_t) pageId))
        texture = swr->textures[pageId];
    if (!texture) return;
    
    int sx = tpag->sourceX + srcOffX;
    int sy = tpag->sourceY + srcOffY;
    int sw = srcW;
    int sh = srcH;
    
    float dx = x;
    float dy = y;
    int dw = swrCeiling(xscale * sw);
    int dh = swrCeiling(yscale * sh);
    
    if (tpag->sourceWidth != tpag->targetWidth) {
        sx = sx * tpag->sourceWidth / tpag->targetWidth;
        sw = sw * tpag->sourceWidth / tpag->targetWidth;
    }
    if (tpag->sourceHeight != tpag->targetHeight) {
        sy = sy * tpag->sourceHeight / tpag->targetHeight;
        sh = sh * tpag->sourceHeight / tpag->targetHeight;
    }
    sx -= texture->originX;
    sy -= texture->originY;
    
    if (UNLIKELY(swrMustRotate(angleDeg)))
    {
        swrDrawSpriteRotated(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha, angleDeg, pivotX * dw, pivotY * dh, false);
    }
    else
    {
        swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, color, alpha);
    }
}

static void SWRenderer_drawSpritePos(Renderer* renderer, int32_t tpagIndex,
                                     float x1, float y1, float x2, float y2,
                                     float x3, float y3, float x4, float y4, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    // TODO: Implement this properly.  (I won't in this PR)
    //
    // You basically have to implement full texture UV mapping which I won't be
    // bothering with.  Somebody else can get on it.
    
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;

    if (tpagIndex < 0 || (uint32_t) tpagIndex >= dwin->tpag.count) {
        logError("%s: tpagIndex of %d is invalid\n", __func__, tpagIndex);
        return;
    }

    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) {
        logError("%s: tpagIndex of %d is invalid, as pageId of %d is invalid\n", __func__, tpagIndex, pageId);
        return;
    }
    int tw = tpag->targetWidth;
    int th = tpag->targetHeight;

    float ascalex = (x2 - x1) / tw;
    float ascaley = (y4 - y1) / th;
    
    (void) x3; (void) y3;
    (void) y2; (void) x4;

    UNIMP();
    
    SWRenderer_drawSprite(renderer, tpagIndex, x1, y1, 0.0f, 0.0f, ascalex, ascaley, 0.0f, renderer->drawColor, alpha);
}

// A held fill can be worked into the tile pictures only where the premultiplied blend exists.
MAYBE_UNUSED static bool swrTintFoldable(void)
{
#ifdef SW_HAS_PREMUL_BLEND
    return true;
#else
    return false;
#endif
}

MAYBE_UNUSED static bool swrPendingTintOff(void)
{
    static int off = -1;
    if (off < 0) off = getenv("BS_NO_TINT_FOLD") != NULL;
    return off != 0;
}

static void SWRenderer_drawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
                                     uint32_t color, float alpha, bool outline)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    SWRenderer* swr = (SWRenderer*) renderer;
    if (!outline && swr->blendMode == bm_normal) swrExtendFill(swr, x1, &y1, x2, &y2);
#if PIXEL_SIZE == 16
    // Nothing comes of a rectangle this faint (alphaBlend skips it), so it is no reason to let out what is held.
    if (swr->blendMode == bm_normal && swrIntAlpha(alpha) < 4) return;
#endif
    uintpixel_t pxcolor = swrConvertPixel(color);
#ifdef SW_HAS_PREMUL_BLEND
    // An opaque fill of the whole screen: the grid and tile pictures still held would not show under it.
    if (!outline && swrFillCoversMain(swr, x1, y1, x2, y2, alpha)) {
        swrHeldUnderDiscard(swr);
        // Holding those had marked the buffer as no longer one colour, though
        // nothing was written. With them gone and nothing else held over a
        // held clear, it is the clear's colour again, so the fill below takes
        // the clear instead of writing it out first (a battle's backdrop over
        // the room's floor grid).
        if (swr->clearHeld && swr->mirrorLayers == 0 && swr->mirrorStage == 0 && swr->overlayCount == 0 && swr->pipeCount == 0) {
            swr->uniformValid = true;
            swr->uniformColor = swr->clearHeldColor;
        }
    }
    // A fill over a held stack of mirrored layers may join it instead of letting it out.
    if (!outline && swrMirrorHoldFill(renderer, x1, y1, x2, y2, pxcolor, alpha)) return;
#endif
    
    // Still one colour if the flush has nothing to draw; a fill with that colour is then redundant.
    bool nothingHeld = swr->mirrorLayers == 0 && swr->mirrorStage == 0 && swr->overlayCount == 0 && swr->pipeCount == 0;
#if PIXEL_SIZE == 16
    // A translucent fill of the whole buffer over a held clear and its tiled
    // passes, with nothing else held: it joins them (swrFillHold).
    if (!outline && nothingHeld && swr->clearHeld && swr->tiledHeldCount > 0 && swrPendingCount == 0 && !swrGridHeld() &&
        swr->fb == swr->mainFb && swr->fbPitch == swr->mainWidth && swr->blendMode == bm_normal &&
        swrIntAlpha(alpha) <= 253 && swrFillCoversWhole(swr, x1, y1, x2, y2) && swrFillHold(swr, pxcolor, swrIntAlpha(alpha))) {
        swr->uniformValid = false;
        return;
    }
    // The same over a held clear and tile pictures (Deltarune's darkened
    // forest): the pictures come out through it, so the screen is gone over
    // once. Each pixel is blended as the fill would blend it.
    if (!outline && nothingHeld && swr->clearHeld && swr->tiledHeldCount == 0 && swrPendingCount > 0 && !swrGridHeld() &&
        swr->fb == swr->mainFb && swr->fbPitch == swr->mainWidth && swr->blendMode == bm_normal &&
        swrIsPartialAlpha(swrIntAlpha(alpha)) && swrTintFoldable() && !swrPendingTintOff() &&
        swr->portX == 0 && swr->portY == 0 && swr->maxX == swr->mainWidth && swr->maxY == swr->mainHeight &&
        swrFillCoversWhole(swr, x1, y1, x2, y2)) {
        swrPendingTint = (uint32_t) pxcolor | (uint32_t) swrIntAlpha(alpha) << 16;
        swrTileRunsFlush(swr);
        swrPendingTint = 0;
        swr->uniformValid = false;
        return;
    }
#endif
    bool kept = swr->uniformValid && nothingHeld;
    // A clear still held is handed to the fill, which replaces it if it covers as much.
    bool clearTaken = kept && !outline && swr->clearHeld;
    if (clearTaken) swr->clearHeld = false;
    swrOverlayFlush(swr);
    
    if (outline)
        swrDrawRectangle(renderer, x1, y1, x2, y2, pxcolor, alpha);
    else {
        swr->clearHeldForFill = clearTaken;
        swr->uniformKept = kept;
        swrFillRectangle(renderer, x1, y1, x2, y2, pxcolor, alpha);
        swr->uniformKept = false;
    }
}

static void SWRenderer_drawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                          uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4,
                                          float alpha, bool outline)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
#if PIXEL_SIZE == 16
    // As in SWRenderer_drawRectangle: too faint to draw, so nothing held is let out for it.
    if (((SWRenderer*) renderer)->blendMode == bm_normal && swrIntAlpha(alpha) < 4) return;
#endif
    swrOverlayFlush((SWRenderer*) renderer);
    uintpixel_t pxcolor1 = swrConvertPixel(color1);
    uintpixel_t pxcolor2 = swrConvertPixel(color2);
    uintpixel_t pxcolor3 = swrConvertPixel(color3);
    uintpixel_t pxcolor4 = swrConvertPixel(color4);
    
    if (outline)
        swrDrawRectangleColor(renderer, x1, y1, x2, y2, pxcolor1, pxcolor2, pxcolor3, pxcolor4, alpha);
    else
        swrFillRectangleColor(renderer, x1, y1, x2, y2, pxcolor1, pxcolor2, pxcolor3, pxcolor4, alpha);
}

static void SWRenderer_drawLine(Renderer* renderer, float x1, float y1, float x2, float y2,
                                float width, uint32_t color, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    (void)renderer; (void)x1; (void)y1; (void)x2; (void)y2;
    (void)width; (void)color; (void)alpha;
    
    uintpixel_t colorCvt = swrConvertPixel(color);
#ifdef TRANSPARENT_MASK
    colorCvt |= TRANSPARENT_MASK;
#endif
    swrDrawLine(renderer, x1, y1, x2, y2, width, colorCvt, colorCvt, alpha, SWR_LINE_ALIGN_CENTER);
}

static void SWRenderer_drawTriangle(Renderer* renderer,
                                    float x1, float y1, float x2, float y2, float x3, float y3,
                                    uint32_t color1, uint32_t color2, uint32_t color3,
                                    float alpha, bool outline)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    if (outline)
    {
        uintpixel_t color1cvt = swrConvertPixel(color1);
        uintpixel_t color2cvt = swrConvertPixel(color2);
        uintpixel_t color3cvt = swrConvertPixel(color3);
        swrDrawLine(renderer, x1, y1, x2, y2, 1, color1cvt, color2cvt, alpha, SWR_LINE_ALIGN_CENTER);
        swrDrawLine(renderer, x1, y1, x3, y3, 1, color1cvt, color3cvt, alpha, SWR_LINE_ALIGN_CENTER);
        swrDrawLine(renderer, x2, y2, x3, y3, 1, color3cvt, color3cvt, alpha, SWR_LINE_ALIGN_CENTER);
    }
    else
    {
        swrDrawTriangle(renderer, x1, y1, x2, y2, x3, y3, color1, color2, color3, alpha);
    }
}

// draw_circle and draw_ellipse, filled. The corners are those of Renderer_drawEllipseColor's polygon, worked out
// from a cos/sin table kept for each precision (the same cosf and sinf of the same float angles) rather than for
// every shape. A shape in one colour is filled as one convex polygon by pixel centres (swrFillConvexPolygon), which
// leaves no gaps; the triangle fan the other renderers use left out the row through a centre on an exact pixel row,
// so rows of small dots showed stripes. A shape shaded from centre to rim, or with no width or height, is still drawn
// as that triangle fan. A shape wholly outside the target draws nothing (nothing either way can land beyond a
// pixel's margin round its box), but still settles what is held (swrOverlayFlush), as a first triangle would have.
// Whether a filled ellipse centred on (cx, cy) with these radii lies wholly outside the target, a pixel's margin
// to spare: then neither the polygon nor the fan can put a pixel down (see SWRenderer_drawFilledEllipse).
static bool swrFilledEllipseOutside(SWRenderer* swr, float cx, float cy, float rx, float ry)
{
    if (!(swr->scaleX > 0.0f && swr->scaleY > 0.0f)) return false;
    float halfW = rx < 0.0f ? -rx : rx, halfH = ry < 0.0f ? -ry : ry;
    float left = cx - halfW, top = cy - halfH, right = cx + halfW, bottom = cy + halfH;
    swrTransformPosIfNeeded(swr, &left, &top);
    swrTransformPosIfNeeded(swr, &right, &bottom);
    return bottom < -1.0f || top > (float) swr->height + 1.0f || right < (float) swr->portX - 1.0f || left > (float) swr->maxX + 1.0f;
}

static void SWRenderer_drawFilledEllipse(Renderer* renderer, float cx, float cy, float rx, float ry, uint32_t col1, uint32_t col2, int32_t segments);

bool SWRenderer_filledEllipseOutside(Renderer* renderer, float cx, float cy, float rx, float ry)
{
    if (renderer == NULL || renderer->vtable->drawFilledEllipse != SWRenderer_drawFilledEllipse) return false;
    return swrFilledEllipseOutside((SWRenderer*) renderer, cx, cy, rx, ry);
}

// The shape itself, once what is held has been settled and the shape is known to reach the target.
static void swrFilledEllipseShape(Renderer* renderer, float cx, float cy, float rx, float ry, uint32_t col1, uint32_t col2, int32_t segments)
{
    static float unitCos[65], unitSin[65];
    static int32_t unitSegments = 0;
    float xs[65], ys[65];
    int32_t i;
    if (segments < 4 || segments > 64) return;
    if (unitSegments != segments) {
        float step = 6.2831853f / (float) segments;
        for (i = 1; segments >= i; i++) {
            float angle = step * (float) i;
            unitCos[i] = cosf(angle);
            unitSin[i] = sinf(angle);
        }
        unitSegments = segments;
    }
    if (col1 == col2 && rx != 0.0f && ry != 0.0f &&
        swrFillSmallDot(renderer, cx, cy, rx, ry, segments, unitCos, unitSin, col1, renderer->drawAlpha)) return;
    if (col1 == col2 && rx != 0.0f && ry != 0.0f) {
        // swrFillConvexPolygon's first pass done here in one go, on each corner as it is worked out: the same
        // corners (the same expressions, so the same rounding), moved into the buffer by the same statements.
        SWRenderer* swr = (SWRenderer*) renderer;
        float px[65], py[65];
        int first = 0, last = 0;
        for (i = 0; segments >= i; i++) {
            float x = i == 0 ? cx + rx : cx + rx * unitCos[i];
            float y = i == 0 ? cy : cy + ry * unitSin[i];
            swrTransformPosIfNeeded(swr, &x, &y);
            if (!(x == x) || !(y == y)) return; // NaN
            px[i] = x;
            py[i] = y;
            if (py[first] > y) first = i;
            if (y > py[last]) last = i;
        }
        swrFillTransformedConvex(renderer, px, py, segments + 1, first, last, col1, renderer->drawAlpha);
        return;
    }
    xs[0] = cx + rx;
    ys[0] = cy;
    for (i = 1; segments >= i; i++) {
        xs[i] = cx + rx * unitCos[i];
        ys[i] = cy + ry * unitSin[i];
    }
    for (i = 1; segments >= i; i++)
        swrDrawTriangle(renderer, cx, cy, xs[i - 1], ys[i - 1], xs[i], ys[i], col1, col2, col2, renderer->drawAlpha);
}

static void SWRenderer_drawFilledEllipse(Renderer* renderer, float cx, float cy, float rx, float ry, uint32_t col1, uint32_t col2, int32_t segments)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (SWR_SKIPPED(swr)) return;
    swrOverlayFlush(swr);
    if (swrFilledEllipseOutside(swr, cx, cy, rx, ry)) return;
    swrFilledEllipseShape(renderer, cx, cy, rx, ry, col1, col2, segments);
}

bool SWRenderer_drawFilledCircleSettled(Renderer* renderer, float cx, float cy, float radius)
{
    if (renderer == NULL || renderer->vtable->drawFilledEllipse != SWRenderer_drawFilledEllipse) return false;
    SWRenderer* swr = (SWRenderer*) renderer;
    if (SWR_SKIPPED(swr)) return true;
    if (swrFilledEllipseOutside(swr, cx, cy, radius, radius)) return true;
    // What Renderer_drawCircle hands the hook: the draw colour twice, the normalized precision, at least 4.
    int32_t segments = Renderer_normalizeCirclePrecision(renderer->circlePrecision);
    if (4 > segments) segments = 4;
    swrFilledEllipseShape(renderer, cx, cy, radius, radius, renderer->drawColor, renderer->drawColor, segments);
    return true;
}

static void SWRenderer_drawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
                                     float width, uint32_t color1, uint32_t color2, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    swrDrawLine(renderer, x1, y1, x2, y2, width, swrConvertPixel(color1), swrConvertPixel(color2), alpha, SWR_LINE_ALIGN_CENTER);
}

static void SWRenderer_drawText(Renderer* renderer, const char* text, float x, float y,
                                float xscale, float yscale, float angleDeg, float lineSeparation)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    swrDrawText(swr, text, x, y, xscale, yscale, angleDeg, renderer->drawColor, renderer->drawAlpha, lineSeparation);
}

static void SWRenderer_drawTextColor(Renderer* renderer, const char* text, float x, float y,
                                     float xscale, float yscale, float angleDeg,
                                     int32_t c1, int32_t c2, int32_t c3, int32_t c4, MAYBE_UNUSED float alpha,
                                     float lineSeparation)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    // TODO: allow c2, c3, c4
    (void) c2;
    (void) c3;
    (void) c4;
    
    swrDrawText(swr, text, x, y, xscale, yscale, angleDeg, c1, renderer->drawAlpha, lineSeparation);
}

static void SWRenderer_drawTextUI(Renderer* renderer, const char* text, float x, float y,
                                  float xscale, float yscale, float angleDeg,
                                  int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha,
                                  float lineSeparation)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    (void) xscale;
    (void) yscale;
    (void) angleDeg;
    (void) c2;
    (void) c3;
    (void) c4;
    (void) lineSeparation;

    swrDrawDebugText(swr, text, (int) x, (int) y, c1, alpha);
}

static void SWRenderer_drawSpriteTiled(Renderer* renderer, int32_t tpagIndex,
                                       float originX, float originY, float x, float y,
                                       float xscale, float yscale, bool tileX, bool tileY,
                                       float roomW, float roomH, uint32_t color, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dwin = renderer->dataWin;

    if (0 > tpagIndex || dwin->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || swr->totalTextureCount <= (uint32_t) pageId) return;
    SWTexture* texture = swrTextureForItem(swr, tpagIndex);
    if (!texture) return;
    // A clear of the whole main buffer still held, with nothing over it but
    // tiled passes held already: this one can be held with them. Worked out
    // after the texture is in, because loading it can evict another and
    // settle the clear.
    bool canHold = swr->clearHeld && swr->fb == swr->mainFb && swrPendingCount == 0 && !swrGridHeld() &&
                   swr->mirrorLayers == 0 && swr->mirrorStage == 0 && swr->overlayCount == 0 && swr->pipeCount == 0;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) tpag->boundingWidth * axScale;
    float tileH = (float) tpag->boundingHeight * ayScale;
    if (0 >= tileW || 0 >= tileH) return;

    float startX, endX, startY, endY;
    if (tileX) {
        startX = fmodf(x - originX * axScale, tileW);
        if (startX > 0) startX -= tileW;
        endX = roomW;
    } else {
        startX = x - originX * axScale;
        endX = startX + tileW;
    }
    if (tileY) {
        startY = fmodf(y - originY * ayScale, tileH);
        if (startY > 0) startY -= tileH;
        endY = roomH;
    } else {
        startY = y - originY * ayScale;
        endY = startY + tileH;
    }
    
    int sx = tpag->sourceX - texture->originX;
    int sy = tpag->sourceY - texture->originY;
    int sw = tpag->sourceWidth;
    int sh = tpag->sourceHeight;

    int localX0 = tpag->targetX - originX;
    int localY0 = tpag->targetY - originY;
    int localX1 = localX0 + tpag->sourceWidth;
    int localY1 = localY0 + tpag->sourceHeight;
    int sx0 = xscale * localX0;
    int sy0 = yscale * localY0;
    int sx1 = xscale * localX1;
    int sy1 = yscale * localY1;

    // The room rectangle the port shows, with a pixel to spare. A small sprite
    // tiled over a big room is hundreds of copies, nearly all of them
    // somewhere else; those are stepped over below without a draw call.
    bool cull = swr->scaleX > 0.0f && swr->scaleY > 0.0f;
    int viewLeft = swr->viewX - 1, viewTop = swr->viewY - 1;
    int viewRight = cull ? swr->viewX + (int) ((float) (swr->maxX - swr->portX) / swr->scaleX) + 2 : 0;
    int viewBottom = cull ? swr->viewY + (int) ((float) (swr->maxY - swr->portY) / swr->scaleY) + 2 : 0;
    
    // Unscaled copies a whole number of pixels apart go down in one pass over the screen's rows.
    if (xscale == 1.0f && yscale == 1.0f && tileW == (float) (int) tileW && tileH == (float) (int) tileH)
    {
        int countX = 0, countY = 0;
        for (int dx = startX; endX > dx; dx += tileW) countX++;
        for (int dy = startY; endY > dy; dy += tileH) countY++;
        int firstX = (int) startX + (int) originX + sx0, firstY = (int) startY + (int) originY + sy0;
        if (canHold && swrTiledHold(swr, texture, sx, sy, sw, sh, firstX, firstY, (int) tileW, (int) tileH, countX, countY, color, alpha)) {
            swr->uniformValid = false; // one colour no longer, though nothing is written yet
            return;
        }
        swrOverlayFlush(swr);
        bool drawn;
        SWR_NOTED("  of which: tiled rows pass", drawn = swrDrawSpriteTiledRows(swr, texture, sx, sy, sw, sh, firstX, firstY, (int) tileW, (int) tileH, countX, countY, color, alpha));
        if (drawn) return;
    } else {
        swrOverlayFlush(swr);
    }
    
    for (int dy = startY; endY > dy; dy += tileH) {
        int cy = dy + (int)(originY * ayScale);
        int vy0 = cy + sy0;
        int vy1 = cy + sy1;
        int dh = vy1 - vy0;
        if (cull && ((vy0 < viewTop && vy1 < viewTop) || (vy0 > viewBottom && vy1 > viewBottom))) continue;

        for (int dx = startX; endX > dx; dx += tileW) {
            int cx = dx + (int)(originX * axScale);
            int vx0 = cx + sx0;
            int vx1 = cx + sx1;
            int dw = vx1 - vx0;
            if (cull && ((vx0 < viewLeft && vx1 < viewLeft) || (vx0 > viewRight && vx1 > viewRight))) continue;

            swrDrawSprite(renderer, vx0, vy0, dw, dh, texture, sx, sy, sw, sh, color, alpha);
        }
    }
}

static void SWRenderer_drawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y, float xscale, float yscale, float roomW, float roomH, uint32_t color, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;

    if (0 > surfaceID || swr->surfaceCount <= (size_t) surfaceID) return;

    SWSurface* surfaceP = swr->surfaces[surfaceID];
    if (!surfaceP) return;

    swrCommitShadowWritesToSurfaceIfNeeded(swr, surfaceP);
    SWTexture* surface = surfaceP->texture;

    float axScale = fabsf(xscale);
    float ayScale = fabsf(yscale);
    float tileW = (float) surface->width * axScale;
    float tileH = (float) surface->height * ayScale;
    if (0 >= tileW || 0 >= tileH) return;
    
    float originX = 0, originY = 0;

    float startX, endX, startY, endY;
    startX = fmodf(x - originX * axScale, tileW);
    if (startX > 0) startX -= tileW;
    endX = roomW;
    startY = fmodf(y - originY * ayScale, tileH);
    if (startY > 0) startY -= tileH;
    endY = roomH;
    
    int sx = 0, sy = 0;
    int sw = surface->width;
    int sh = surface->height;

    int localX0 = -originX;
    int localY0 = -originY;
    int localX1 = localX0 + surface->width;
    int localY1 = localY0 + surface->height;
    int sx0 = xscale * localX0;
    int sy0 = yscale * localY0;
    int sx1 = xscale * localX1;
    int sy1 = yscale * localY1;

    for (int dy = startY; endY > dy; dy += tileH) {
        int cy = dy + (int)(originY * ayScale);
        int vy0 = cy + sy0;
        int vy1 = cy + sy1;
        int dh = vy1 - vy0;

        for (int dx = startX; endX > dx; dx += tileW) {
            int cx = dx + (int)(originX * axScale);
            int vx0 = cx + sx0;
            int vx1 = cx + sx1;
            int dw = vx1 - vx0;

            swrDrawSprite(renderer, vx0, vy0, dw, dh, surface, sx, sy, sw, sh, color, alpha);
        }
    }
}

// The main loop clears the window (clearFrameBuffer) before every frame, and
// the runner then clears the whole target again with the room's background
// colour before drawing views. On slow memory two full-screen fills per frame
// are expensive, so the first one is only recorded and is skipped when the
// second one arrives. Anything else that could touch the main buffer first
// performs it.
static void swrFlushPendingClear(SWRenderer* swr)
{
    if (!swr->pendingClear) return;
    swr->pendingClear = false;
    if (!swr->mainFb) return;
    swrClearMain(swr, swr->pendingClearColor);
}

// A clear of the whole main buffer is held back as well: a room's tiles
// usually come next and cover most of it, and held tile pictures take the
// clear with them (swrTileRunsFlush), writing the colour only where they
// leave a gap. Anything else that draws performs it first (swrOverlayFlush).
void swrClearSettle(SWRenderer* swr)
{
    if (!swr->clearHeld) return;
    swr->clearHeld = false;
    if (!swr->mainFb) { swr->tiledHeldCount = 0; swrFillHeldDrop(); return; }
    if (swr->tiledHeldCount > 0) {
        SWR_NOTED("  of which: held clear and tiled written", swrTiledHeldWrite(swr, swr->clearHeldColor));
        return;
    }
    SWR_NOTED("  of which: held clear written", swrClearMain(swr, swr->clearHeldColor));
}

// swrClearSettle for a draw that may be its buffer's first draw (see
// SWBaseKey; key NULL when it cannot be one). When the clear is still held,
// nothing else is, and the buffer's recorded first draw is this same one,
// only the tiles something else touched are cleared, and the draw leaves out
// the tiles that still hold it (keep). Returns the draw's baseMode; the draw
// ends it with swrBaseFinish.
int swrClearSettleBase(SWRenderer* swr, const SWBaseKey* key)
{
    swrBaseFinish(swr, false); // one that did not end as expected
    static int off = -1;
    if (off < 0) off = getenv("BS_NO_BASE_KEEP") != NULL;
    SWDirtyBuffer* d = swr->dirty;
    if (off || key == NULL || !swr->clearHeld || !swr->mainFb || swr->fb != swr->mainFb || swr->tiledHeldCount > 0 ||
        swrPendingCount > 0 || swrGridHeld() || swr->mirrorLayers != 0 || swr->mirrorStage != 0 || swr->overlayCount != 0 ||
        d == NULL || d->fb != swr->mainFb || d->width != swr->mainWidth || d->height != swr->mainHeight) {
        // Settle in swrOverlayFlush's order, which the draw is about to call:
        // held tile pictures first, so that a clear under them is written
        // only in their gaps rather than in full and then copied over.
        static int fold = -1;
        if (fold < 0) fold = getenv("BS_NO_CLEAR_FOLD") == NULL;
        if (fold && swr->clearHeld && swrPendingCount > 0 && swrTileRunReaders == 0) {
            swrTileRunsFlush(swr);
            swrGridFlush(swr);
        }
        swrClearSettle(swr);
        return 0;
    }
    if (d->known && d->color == swr->clearHeldColor && d->baseValid && memcmp(&d->baseKey, key, sizeof(*key)) == 0) {
        swr->clearHeld = false;
        static int check = -1;
        if (check < 0) check = getenv("BS_CHECK_CLEAR") != NULL;
        static uint32_t kept;
        if (check) {
            swrDirtyCheckKept(d, swr->mainFb, d->color);
            if (++kept % 300 == 1) logInfo("swr: first draw kept %u times\n", kept);
        }
        SWR_NOTED("  of which: held clear written", swrFillTiles(swr, d, d->rows, NULL, d->color));
        for (int r = 0; r < SWR_DIRTY_MAX_ROWS; r++) {
            d->keep[r] = d->baseRows[r] & ~d->rows[r];
            d->baseRows[r] = d->keep[r];
            d->rows[r] = 0;
        }
        swr->dirtyMark = d;
        swr->baseMode = 2;
    } else {
        swrClearSettle(swr);
        if (swr->dirty != d || !d->known) return 0;
        memset(d->keep, 0, sizeof(d->keep));
        d->baseKey = *key;
        swr->baseMode = 1;
    }
    return swr->baseMode;
}

// Ends the draw swrClearSettleBase began: drawn when it went over just as
// recorded (every row through the colour's runs), so that it can be kept the
// next time; otherwise what it wrote counts as anything else's.
void swrBaseFinish(SWRenderer* swr, bool drawn)
{
    if (swr->baseMode == 0) return;
    swr->baseMode = 0;
    SWDirtyBuffer* d = swr->dirty;
    if (d == NULL) return;
    memset(d->keep, 0, sizeof(d->keep));
    if (drawn) { d->baseValid = true; return; }
    for (int r = 0; r < SWR_DIRTY_MAX_ROWS; r++) {
        d->rows[r] |= d->baseRows[r];
        d->baseRows[r] = 0;
    }
    d->baseValid = false;
}

static void SWRenderer_clearScreen(Renderer* renderer, uint32_t color, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    SWRenderer* swr = (SWRenderer*) renderer;
    bool wholeMain = swr->fb == swr->mainFb && swr->fbPitch == swr->width &&
                     swr->width == swr->mainWidth && swr->height == swr->mainHeight;
    if (wholeMain) {
        // This one covers them.
        swrHeldUnderDiscard(swr);
        swr->clearHeld = false;
    }
    swrOverlayFlush(swr);
    
    // A clear of the whole main buffer makes the pending one redundant.
    if (swr->fb == swr->mainFb && swr->fbPitch == swr->width)
        swr->pendingClear = false;
    else
        swrFlushPendingClear(swr);
    
    color = swrConvertPixel(color);
#ifdef TRANSPARENT_MASK
    if (alpha >= 0.95f) {
        color |= TRANSPARENT_MASK;
    }
    else if (alpha < 0.01f) {
        color &= ~TRANSPARENT_MASK;
    }
    else {
    #if PIXEL_SIZE == 32
        int alphai = (int)(255.0f * alpha);
        if (alphai < 0) alphai = 0;
        if (alphai > 255) alphai = 255;
        color |= (alphai << 24);
    #elif PIXEL_SIZE == 16
        color |= (alpha > 0.5f);
    #endif
    }
#else
    if (alpha < 0.5f)
        color = PXL_TRANSPARENT;
#endif
    
    if (wholeMain) {
        swr->clearHeld = true;
        swr->clearHeldColor = (uintpixel_t) color;
        swr->uniformValid = true;
        swr->uniformColor = (uintpixel_t) color;
        return;
    }
    swrDirtyRect(swr, 0, 0, swr->width, swr->height);
    for (int y = 0; y < swr->height; y++) {
        swrFillPixels(&swr->fb[y * swr->fbPitch], (size_t) swr->width, (uintpixel_t) color);
    }
    if (swr->fb == swr->mainFb && swr->fbPitch == swr->width) {
        swr->uniformValid = true;
        swr->uniformColor = (uintpixel_t) color;
    }
}

static void SWRenderer_gpuSetBlendEnable(Renderer* renderer, bool enable)
{
    swrOverlayFlush((SWRenderer*) renderer);
    UNIMP();
    
    SWRenderer* swr = (SWRenderer*) renderer;
    if (!enable)
        swr->blendMode = bm_normal;
}

static void SWRenderer_gpuSetAlphaTestEnable(Renderer* renderer, bool enable)
{
    swrOverlayFlush((SWRenderer*) renderer);
    UNIMP();
    (void)renderer; (void)enable;
}

static void SWRenderer_gpuSetAlphaTestRef(Renderer* renderer, uint8_t ref)
{
    swrOverlayFlush((SWRenderer*) renderer);
    UNIMP();
    (void)renderer; (void)ref;
}

static void SWRenderer_gpuSetBlendMode(Renderer* renderer, int32_t mode)
{
    swrOverlayFlush((SWRenderer*) renderer);
    // these are the only ones we support right now
    if (mode != bm_normal && mode != bm_add && mode != bm_subtract) {
        logWarn("swr: unsupported blend mode %d\n", mode);
        mode = bm_normal;
    }

    SWRenderer* swr = (SWRenderer*) renderer;
    swr->blendMode = mode;
    
    //logDebug("swr: switching to blend mode %d\n", mode);
    
    if (swr->usingAlphaBlendState)
    {
        swr->usingAlphaBlendState = false;
        SWRenderer_gpuSetColorWriteEnable(renderer, true, true, true, true);
    }
}

static void SWRenderer_gpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue, bool alpha)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (!swr->drawingToSurface) {
        logWarn("swr: gpuSetColorWriteEnable not supported for main framebuffer\n");
        return;
    }
    
    logWarn("SWRenderer_gpuSetColorWriteEnable(%d, %d, %d, %d)\n", red, green, blue, alpha);
    SWSurface* currSurf = swr->surfaces[swr->currentSurfaceIndex];
    
    swrCommitShadowWritesToSurfaceIfNeeded(swr, currSurf);
    
    swr->writeMask =
        (red ? WRITE_MASK_RED : 0) |
        (green ? WRITE_MASK_GREEN : 0) |
        (blue ? WRITE_MASK_BLUE : 0) |
        (alpha ? WRITE_MASK_ALPHA : 0);
    
    // no need to change other properties, because the width and height are the same.
    // but we ALWAYS need to re-fetch the writable surface texture since the old one
    // may have been freed.
    swr->fb = swrWritableSurfaceTexture(swr, swr->currentSurfaceIndex)->buffer;
    
    if (currSurf->shadowTexture) {
        assert(currSurf->texture->width == currSurf->shadowTexture->width);
        assert(currSurf->texture->height == currSurf->shadowTexture->height);
        assert(currSurf->texture->buffer != currSurf->shadowTexture->buffer);
    }
}

static void SWRenderer_gpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue, bool* alpha)
{
    *red = false;
    *green = false;
    *blue = false;
    *alpha = false;
    
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (!swr->drawingToSurface) {
        logWarn("swr: gpuGetColorWriteEnable not supported for main framebuffer\n");
        return;
    }
    
    *red = (swr->writeMask & WRITE_MASK_RED) != 0;
    *green = (swr->writeMask & WRITE_MASK_GREEN) != 0;
    *blue = (swr->writeMask & WRITE_MASK_BLUE) != 0;
    *alpha = (swr->writeMask & WRITE_MASK_ALPHA) != 0;
}

static void SWRenderer_gpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha, int32_t dfactor_alpha)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    if (sfactor == bm_src_alpha && dfactor == bm_one) {
        swr->blendMode = bm_add;
    }
    else if (sfactor == bm_src_alpha && (dfactor == bm_dest_alpha || dfactor == bm_inv_src_alpha)) {
        swr->blendMode = bm_normal;
    }
    else {
        swr->blendMode = bm_normal;
        logWarn("swr: unsupported ext blend mode combo: sfactor=%d  dfactor=%d\n", sfactor, dfactor);
    }
    
    // alpha handling now
    bool unhandled_alpha = false;
    if (swr->drawingToSurface && (sfactor_alpha == bm_dest_alpha && dfactor_alpha == bm_zero)) {
        swr->usingAlphaBlendState = true;
        SWRenderer_gpuSetColorWriteEnable(renderer, true, true, true, false);
    }
    else {
        swr->usingAlphaBlendState = false;
        SWRenderer_gpuSetColorWriteEnable(renderer, true, true, true, true);
        unhandled_alpha = true;
    }
    
    if (unhandled_alpha) {
        logWarn("swr: unsupported ext blend mode combo: sfactoralpha=%d  dfactoralpha=%d\n", sfactor_alpha, dfactor_alpha);
    }
}

static void SWRenderer_gpuSetTexFilter(Renderer* renderer, bool enable)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void) renderer;
    (void) enable;
    UNIMP();
}

static void SWRenderer_flush(Renderer* renderer)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    UNIMP();
    
    if (swr->drawingToSurface) {
        bool red, green, blue, alpha;
        SWRenderer_gpuGetColorWriteEnable(renderer, &red, &green, &blue, &alpha);
        SWRenderer_gpuSetColorWriteEnable(renderer, red, green, blue, alpha);
    }
}

static bool SWRenderer_gpuGetBlendEnable(Renderer* renderer)
{
    UNIMP();
    (void)renderer;
    return false;
}

static void SWRenderer_gpuSetFog(Renderer* renderer, bool enable, uint32_t color, float start, float end)
{
    swrOverlayFlush((SWRenderer*) renderer);
    UNIMP();
    (void)renderer; (void)enable; (void)color; (void)start; (void)end;
}

static int32_t SWRenderer_createSurface(Renderer* renderer, int32_t width, int32_t height)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    int32_t slot = -1;
    for (size_t i = 0; i < swr->surfaceCount; i++)
    {
        if (swr->surfaces[i] == NULL) {
            slot = (int32_t) i;
            break;
        }
    }
    
    if (slot < 0) {
        logError("swr: Could not create surface, too many exist at once.\n");
        return slot;
    }
    
    swr->surfaces[slot] = swrCreateSurface(width, height);
    return slot;
}

static bool SWRenderer_surfaceExists(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount)
        return false;
    
    return swr->surfaces[surfaceID] != NULL;
}

static bool SWRenderer_setRenderTarget(Renderer* renderer, int32_t surfaceID, bool implicitApplicationSurface)
{
    swrOverlayFlush((SWRenderer*) renderer);
    return swrSwitchToSurface(renderer, surfaceID, implicitApplicationSurface);
}

static float SWRenderer_getSurfaceWidth(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID)
        return (float)(int)((swr->drawingToSurface ? swr->mainWidth : swr->width) / swr->scaleX);
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL)
        return 0.0f;
    
    return (float) swr->surfaces[surfaceID]->texture->width;
}

static float SWRenderer_getSurfaceHeight(Renderer* renderer, int32_t surfaceID)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (surfaceID == APPLICATION_SURFACE_ID) 
        return (float)(int)((swr->drawingToSurface ? swr->mainHeight : swr->height) / swr->scaleY);
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL)
        return 0.0f;
    
    return (float) swr->surfaces[surfaceID]->texture->height;
}

static void SWRenderer_drawSurface(Renderer* renderer, int32_t surfaceID,
                                   int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                                   float x, float y, float xscale, float yscale, float angleDeg,
                                   uint32_t color, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    SWTexture* surface, localSurface;
    localSurface.halfBuffer = NULL;
    localSurface.glyphHints = NULL;
    localSurface.buttonCache = NULL;
    localSurface.rowBounds = NULL;
    localSurface.boxRows = NULL;
    localSurface.floweyArtRows = NULL;
    localSurface.pipeSolidBounds = NULL;
    localSurface.halfRowBounds = NULL;
    localSurface.halfFullBounds = NULL;
    localSurface.spans = localSurface.halfSpans = NULL;
    localSurface.immutable = false;
    localSurface.halfCoverage = NULL;
    localSurface.solid = localSurface.halfSolid = 0;
    localSurface.halfPhaseX = localSurface.halfPhaseY = 0;
    if (surfaceID == APPLICATION_SURFACE_ID) {
        localSurface.buffer = swr->drawingToSurface ? swr->mainFb : swr->fb;
        localSurface.width = swr->drawingToSurface ? swr->mainWidth : swr->width;
        localSurface.height = swr->drawingToSurface ? swr->mainHeight : swr->height;
        surface = &localSurface;
    } else {
        if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
            logError("swr: Invalid surface id %d for drawSurface\n", surfaceID);
            return;
        }

        swrCommitShadowWritesToSurfaceIfNeeded(swr, swr->surfaces[surfaceID]);
        surface = swr->surfaces[surfaceID]->texture;
    }
    
    if (srcWidth < 0) {
        srcWidth = surface->width;
        swrReverseTransformSizeIfNeeded(swr, &xscale, NULL);
    }
    if (srcHeight < 0) {
        srcHeight = surface->height;
        swrReverseTransformSizeIfNeeded(swr, NULL, &yscale);
    }
    
    int sx = srcLeft;
    int sy = srcTop;
    int sw = srcWidth;
    int sh = srcHeight;
    
    int tw = (int)(srcWidth * swr->scaleX);
    int th = (int)(srcHeight * swr->scaleY);
    
    float dx = x;
    float dy = y;
    int dw = (int)(xscale * tw);
    int dh = (int)(yscale * th);

    if (UNLIKELY(swrMustRotate(angleDeg)))
    {
        float pivotX = (x - dx) * swrSgn(xscale);
        float pivotY = (y - dy) * swrSgn(yscale);
        
        if (tw != sw)
            pivotX *= (float)tw / sw;
        if (th != sh)
            pivotY *= (float)th / sh;
        
        swrDrawSpriteRotated(renderer, dx, dy, dw, dh, surface, sx, sy, sw, sh, color, alpha, angleDeg, pivotX, pivotY, false);
    }
    else
    {
        swrDrawSprite(renderer, dx, dy, dw, dh, surface, sx, sy, sw, sh, color, alpha);
    }
}

static void SWRenderer_surfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width, int32_t height)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (surfaceID == APPLICATION_SURFACE_ID) {
        logError("swr: Don't support resizing the application window with this.  There must be another way! (need to set to %dx%d)\n", width, height);
        return;
    }
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
        logError("swr: Cannot resize surface id %d, it's invalid\n", surfaceID);
        return;
    }
    
    swrFreeSurface(swr->surfaces[surfaceID]);
    swr->surfaces[surfaceID] = swrCreateSurface(width, height);
}

static void SWRenderer_surfaceFree(Renderer* renderer, int32_t surfaceID)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (surfaceID == APPLICATION_SURFACE_ID) {
        logError("swr: Don't support SWRenderer_surfaceCopy the application window with this.  There must be another way!\n");
        return;
    }
    
    if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL) {
        logError("swr: Cannot resize surface id %d, it's invalid\n", surfaceID);
        return;
    }
    
    swrFreeSurface(swr->surfaces[surfaceID]);
    swr->surfaces[surfaceID] = NULL;
}

static void SWRenderer_surfaceCopy(Renderer* renderer,
                                   int32_t DestSurfaceID, int32_t DestX, int32_t DestY,
                                   int32_t SrcSurfaceID, int32_t SrcX, int32_t SrcY,
                                   int32_t SrcW, int32_t SrcH, bool part)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    SWTexture temp1, temp2;
    SWTexture *dstSurf, *srcSurf;
    bool freeSrcSurf = false;
    
    if (DestSurfaceID == APPLICATION_SURFACE_ID) {
        dstSurf = &temp1;
        temp1.width = swr->mainWidth;
        temp1.height = swr->mainHeight;
        temp1.buffer = swr->mainFb;
        swrDirtyAll(swr);
    }
    else if (DestSurfaceID < 0 || (size_t) DestSurfaceID >= swr->surfaceCount || swr->surfaces[DestSurfaceID] == NULL) {
        logError("swr: Cannot resize surface id %d, it's invalid (dest in surfaceCopy)\n", DestSurfaceID);
        return;
    }
    else {
        dstSurf = swrWritableSurfaceTexture(swr, DestSurfaceID);
    }
    
    if (SrcSurfaceID == APPLICATION_SURFACE_ID) {
        // TODO: resizing might be expensive
        temp2.width = swr->mainWidth;
        temp2.height = swr->mainHeight;
        temp2.buffer = swr->mainFb;
        if (swr->mainWidth == swr->portW && swr->mainHeight == swr->portH) {
            srcSurf = &temp2;
        }
        else {
            srcSurf = swrCropSectionFromTexture(&temp2, swr->gameW, swr->gameH, swr->portX, swr->portY, swr->maxX, swr->maxY);
            freeSrcSurf = true;
        }
    }
    else if (SrcSurfaceID < 0 || (size_t) SrcSurfaceID >= swr->surfaceCount || swr->surfaces[SrcSurfaceID] == NULL) {
        logError("swr: Cannot resize surface id %d, it's invalid (src in surfaceCopy)\n", SrcSurfaceID);
        return;
    }
    else {
        swrCommitShadowWritesToSurfaceIfNeeded(swr, swr->surfaces[SrcSurfaceID]);
        srcSurf = swr->surfaces[SrcSurfaceID]->texture;
    }

    if (!part)
    {
        // Not "part" means copy the whole source texture.
        SrcX = SrcY = 0;
        SrcW = srcSurf->width;
        SrcH = srcSurf->height;
    }
    
    if (SrcX + SrcW < 0) return;
    if (SrcY + SrcH < 0) return;
    if (SrcX >= srcSurf->width) return;
    if (SrcY >= srcSurf->height) return;
    if (DestX + SrcW < 0) return;
    if (DestY + SrcH < 0) return;
    if (DestX >= dstSurf->width) return;
    if (DestY >= dstSurf->height) return;
    
    if (SrcY < 0) {
        SrcH += SrcY;
        DestY -= SrcY;
        SrcY = 0;
    }
    if (SrcX < 0) {
        SrcW += SrcX;
        DestX -= SrcX;
        SrcX = 0;
    }
    if (SrcX + SrcW >= srcSurf->width)
        SrcW = srcSurf->width - SrcX;
    if (SrcY + SrcH >= srcSurf->height)
        SrcH = srcSurf->height - SrcY;
    
    if (DestX + SrcW >= dstSurf->width)
        SrcW = dstSurf->width - DestX;
    if (DestY + SrcH >= dstSurf->height)
        SrcH = dstSurf->height - DestY;
    
    for (int dy = 0; dy < SrcH; dy++) {
        /***/ uintpixel_t* dstLine = &dstSurf->buffer[(dy + DestY) * dstSurf->width];
        const uintpixel_t* srcLine = &srcSurf->buffer[(dy + SrcY)  * srcSurf->width];
        for (int dx = 0; dx < SrcW; dx++) {
            dstLine[dx + DestX] = srcLine[dx + SrcX];
        }
    }
    
    if (freeSrcSurf)
        swrFreeTexture(srcSurf);
}

static bool SWRenderer_surfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA)
{
    swrOverlayFlush((SWRenderer*) renderer);
    UNIMP();
    (void)renderer; (void)surfaceID; (void)outRGBA;
    return false;
}

static int32_t SWRenderer_gpuGetBlendMode(Renderer* renderer)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    return swr->blendMode;
}

static int32_t SWRenderer_createSpriteFromSurface(Renderer* renderer, int32_t surfaceID,
                                                   int32_t x, int32_t y, int32_t w, int32_t h,
                                                   bool removeback, bool smooth,
                                                   int32_t xorig, int32_t yorig)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    (void) smooth;
    
    int cropLeft = x;
    int cropTop = y;
    int cropRight = x + w;
    int cropBottom = y + h;
    
    SWTexture *srcTex, temp1;
    if (surfaceID == APPLICATION_SURFACE_ID)
    {
        srcTex = &temp1;
        temp1.width = swr->width;
        temp1.height = swr->height;
        temp1.buffer = swr->fb;
        
        swrTransformPosIntIfNeeded(swr, &cropLeft, &cropTop);
        swrTransformPosIntIfNeeded(swr, &cropRight, &cropBottom);
    }
    else
    {
        if (surfaceID < 0 || (size_t) surfaceID >= swr->surfaceCount || swr->surfaces[surfaceID] == NULL){
            logError("%s: Invalid surface ID %d\n", __func__, surfaceID);
            return -1;
        }
        SWSurface* surf = swr->surfaces[surfaceID];
        swrCommitShadowWritesToSurfaceIfNeeded(swr, surf);
        srcTex = surf->texture;
    }
    
    int32_t texturePageId = swrFindSurfaceTextureSlot(swr);
    int32_t tpagIndex = swrFindSurfaceTPagSlot(swr);
    if (texturePageId == -1 || tpagIndex == -1) {
        logError("%s: Sprite overflow!!\n", __func__);
        return 0;
    }
    
    SWTexture* tex = swrCropSectionFromTexture(srcTex, w, h, cropLeft, cropTop, cropRight, cropBottom);
    if (removeback)
        swrRemoveBackgroundFromTexture(tex);
    
    swr->textures[texturePageId] = tex;

    // TODO[MrPowerGamerBR]: This is supposed to be refactored, not to modify data.win structs directly.
    DataWin* dw = swr->base.dataWin;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    tpag->sourceX = 0;
    tpag->sourceY = 0;
    tpag->sourceWidth = (uint16_t) w;
    tpag->sourceHeight = (uint16_t) h;
    tpag->targetX = 0;
    tpag->targetY = 0;
    tpag->targetWidth = (uint16_t) w;
    tpag->targetHeight = (uint16_t) h;
    tpag->boundingWidth = (uint16_t) w;
    tpag->boundingHeight = (uint16_t) h;
    tpag->texturePageId = texturePageId;
    
    uint32_t spriteIndex = DataWin_allocSpriteSlot(dw, swr->originalSpriteCount);
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    // name was set by DataWin_allocSpriteSlot ("__newsprite<N>"); don't overwrite it here
    sprite->width = (uint32_t) tpag->targetWidth;
    sprite->height = (uint32_t) tpag->targetHeight;
    sprite->originX = xorig;
    sprite->originY = yorig;
    sprite->textureCount = 1;
    sprite->tpagIndices = (int32_t*) safeMalloc(sizeof(int32_t));
    sprite->tpagIndices[0] = (int32_t) tpagIndex;
    sprite->maskCount = 0;
    sprite->masks = nullptr;

    logInfo("%s: Allocated surface sprite with ID %d\n", __func__, spriteIndex);
    return spriteIndex;
}

static void SWRenderer_deleteSprite(Renderer* renderer, int32_t spriteIndex)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    DataWin* dw = renderer->dataWin;
    if (0 > spriteIndex || dw->sprt.count <= (uint32_t) spriteIndex) return;

    // Refuse to delete original data.win sprites
    if (swr->originalSpriteCount > (uint32_t) spriteIndex) {
        logError("%s: Cannot delete sprite with index %d, it's invalid.\n", __func__, spriteIndex);
        return;
    }

    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->textureCount == 0) return; // already deleted

    for (uint32_t i = 0; i < sprite->textureCount; i++)
    {
        int32_t tpagIdx = sprite->tpagIndices[i];
        if (tpagIdx >= 0 && (uint32_t) tpagIdx >= swr->originalTPagCount) {
            TexturePageItem* tpag = &dw->tpag.items[tpagIdx];
            int16_t pageId = tpag->texturePageId;
            if (pageId >= 0 && swr->totalTextureCount > (uint32_t) pageId) {
                swrFreeTexture(swr->textures[pageId]);
                swr->textures[pageId] = NULL;
            }
            // Mark TPAG slot as free for reuse
            tpag->texturePageId = -1;
        }
    }
    
    free(sprite->tpagIndices);
    sprite->tpagIndices = NULL;
    
    const char* keepName = sprite->name;
    memset(sprite, 0, sizeof(Sprite));
    sprite->name = keepName;

    logInfo("SWR: Deleted sprite %d\n", spriteIndex);
}

static void SWRenderer_drawTiledPart(Renderer* renderer, int32_t tpagIndex,
                                     int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                                     float dstX, float dstY, float dstW, float dstH,
                                     uint32_t color, float alpha)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    UNIMP();
    (void)renderer; (void)tpagIndex;
    (void)srcX; (void)srcY; (void)srcW; (void)srcH;
    (void)dstX; (void)dstY; (void)dstW; (void)dstH;
    (void)color; (void)alpha;
}

static int32_t SWRenderer_ensureApplicationSurface(Renderer* renderer, int32_t width, int32_t height)
{
    swrOverlayFlush((SWRenderer*) renderer);
    // We don't evict surfaces, and especially not the primary framebuffer,
    // but if we did, this is where we would restore it.
    (void) renderer;
    (void) width;
    (void) height;
    
    return APPLICATION_SURFACE_ID;
}

static RendererVtable swrVtable;

bool SWRenderer_snapshotGetState(Renderer* renderer, SWRendererSnapshotState* state)
{
    memset(state, 0, sizeof(*state));
    if (renderer->vtable != &swrVtable) return true;
    SWRenderer* swr = (SWRenderer*)renderer;
    if (swrPendingCount || swr->viewActive || swr->primitiveBegun || swr->vertexCount || swr->overlayCount ||
        swr->mirrorLayers || swr->mirrorStage || swr->tiledHeldCount ||
        swr->pendingClear || swr->clearHeld || swr->drawingToSurface)
        return false;
    state->blendMode = swr->blendMode;
    state->writeMask = swr->writeMask;
    state->usingAlphaBlendState = swr->usingAlphaBlendState;
    return true;
}

void SWRenderer_snapshotApplyState(Renderer* renderer, const SWRendererSnapshotState* state)
{
    if (renderer->vtable != &swrVtable) return;
    SWRenderer* swr = (SWRenderer*)renderer;
    swr->blendMode = state->blendMode;
    swr->writeMask = state->writeMask;
    swr->usingAlphaBlendState = state->usingAlphaBlendState;
}

void SWRenderer_clearFrameBuffer(Renderer* renderer, uint32_t color)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    uintpixel_t pxcolor = swrConvertPixel(color);
    
#ifdef SW_PLATFORM_FRAMEBUFFER
    // This is the first thing drawn each frame, so it has to go to the buffer
    // the coming frame will use, not the one that was just presented.
    if (swr->fbIsPlatform && !swrAcquirePlatformFramebuffer(swr, swr->width, swr->height)) return;
#endif
    swr->pendingClear = true;
    swr->pendingClearColor = pxcolor;
}

static uint32_t SWRenderer_spriteGetTexture(Renderer* renderer, int32_t tpagIndex)
{
    (void) renderer;

    return (uint32_t) tpagIndex + 1;
}

static uint32_t SWRenderer_surfaceGetTexture(Renderer* renderer, int32_t surfaceID)
{
    (void) renderer;
    (void) surfaceID;

    return (uint32_t) -1;
}

static float SWRenderer_textureGetTexelWidth(Renderer* renderer, uint32_t texID)
{
    (void) renderer;
    (void) texID;
    
    return 1.0f;
}

static float SWRenderer_textureGetTexelHeight(Renderer* renderer, uint32_t texID)
{
    (void) renderer;
    (void) texID;
    
    return 1.0f;
}

static bool SWRenderer_textureGetUVs(Renderer* renderer, uint32_t texID, float* outUVs)
{
    (void) renderer;
    (void) texID;
    (void) outUVs;
    
    return false;
}

static void SWRenderer_textureSetStage(Renderer* renderer, int32_t slot, uint32_t texID)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void) renderer;
    (void) slot;
    (void) texID;
}

static bool SWRenderer_shaderIsCompiled(Renderer* renderer, int32_t shader)
{
    (void) renderer;
    (void) shader;
    
    return false;
}

static bool SWRenderer_shadersSupported(void)
{
    return false;
}

static void SWRenderer_gpuSetShader(Renderer* renderer, int32_t shaderIndex)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void) renderer;
    (void) shaderIndex;
}

static void SWRenderer_gpuResetShader(Renderer* renderer)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void) renderer;
}

static int32_t SWRenderer_shaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform)
{
    (void) renderer;
    (void) shaderIndex;
    (void) uniform;
    
    return 0;
}

static int32_t SWRenderer_shaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform)
{
    (void) renderer;
    (void) shaderIndex;
    (void) uniform;
    
    return 0;
}

static void SWRenderer_shaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1, float value2, float value3, float value4)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void) renderer;
    (void) handle;
    (void) count;
    (void) value1;
    (void) value2;
    (void) value3;
    (void) value4;
}

static void SWRenderer_shaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1, int32_t value2, int32_t value3, int32_t value4)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void) renderer;
    (void) handle;
    (void) count;
    (void) value1;
    (void) value2;
    (void) value3;
    (void) value4;
}

static void SWRenderer_applyProjection(Renderer* renderer, const Matrix4f* worldToClip, const Matrix4f* idk)
{
    swrOverlayFlush((SWRenderer*) renderer);
    (void) renderer;
    (void) worldToClip;
    (void) idk;
    UNIMP();
}

static void SWRenderer_primitiveBegin(Renderer* renderer, int32_t primitiveType)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    swr->primitiveType = primitiveType;
    swr->vertexCount = 0;
    swr->primitiveBegun = true;
    //logDebug("swr: primitiveBegin(%d)\n", primitiveType);
}

static void SWRenderer_primitiveBeginTexture(Renderer* renderer, int32_t primitiveType, int32_t texture)
{
    swrOverlayFlush((SWRenderer*) renderer);
    static bool shownWarning = false;
    if (!shownWarning) {
        shownWarning = true;
        logError("SWR: Do not support primitiveBeginTexture.  Redirect to primitiveBegin.\n");
    }
    
    (void) texture;
    SWRenderer_primitiveBegin(renderer, primitiveType);
}

static void swrPrimitiveLine(Renderer* renderer, SWVertex* vtx0, SWVertex* vtx1)
{
    float alphaAvg = (swrGetAlpha(vtx0->color) + swrGetAlpha(vtx1->color)) / 2;
    SWRenderer_drawLineColor(renderer, vtx0->x, vtx0->y, vtx1->x, vtx1->y, 1.0f, vtx0->color, vtx1->color, alphaAvg);
}

static void swrPrimitiveTriangle(Renderer* renderer, SWVertex* vtx0, SWVertex* vtx1, SWVertex* vtx2)
{
    const bool bWireFrameMode = false; // for debugging
    
    float alphaAvg = (swrGetAlpha(vtx0->color) + swrGetAlpha(vtx1->color) + swrGetAlpha(vtx2->color)) / 3;
    SWRenderer_drawTriangle(renderer, vtx0->x, vtx0->y, vtx1->x, vtx1->y, vtx2->x, vtx2->y, vtx0->color, vtx1->color, vtx2->color, alphaAvg, bWireFrameMode);
}

static void SWRenderer_primitiveEnd(Renderer* renderer)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    //logWarn("SWR: Ending with %d vertices, primitive type %d\n", swr->vertexCount, swr->primitiveType);
    switch (swr->primitiveType)
    {
        case PRIMITIVE_POINTS:
        {
            for (int i = 0; i < swr->vertexCount; i++)
            {
                SWVertex *vtx = &swr->vertexData[i];
                swrPlotPixel(renderer, vtx->x, vtx->y, swrConvertPixel(vtx->color), swrGetAlpha(vtx->color));
            }
            break;
        }
        case PRIMITIVE_LINES:
        {
            for (int i = 0; i + 1 < swr->vertexCount; i += 2)
                swrPrimitiveLine(renderer, &swr->vertexData[i], &swr->vertexData[i + 1]);
            
            break;
        }
        case PRIMITIVE_LINE_STRIP:
        {
            for (int i = 0; i + 1 < swr->vertexCount; i++)
                swrPrimitiveLine(renderer, &swr->vertexData[i], &swr->vertexData[i + 1]);
            
            break;
        }
        case PRIMITIVE_TRIANGLES:
        {
            for (int i = 0; i + 2 < swr->vertexCount; i += 3)
                swrPrimitiveTriangle(renderer, &swr->vertexData[i], &swr->vertexData[i + 1], &swr->vertexData[i + 2]);
            
            break;
        }
        case PRIMITIVE_TRIANGLE_STRIP:
        {
            if (swr->vertexCount < 3)
                break;
            
            for (int i = 0; i < swr->vertexCount; i++)
                swrPrimitiveTriangle(renderer, &swr->vertexData[i], &swr->vertexData[i + 1], &swr->vertexData[i + 2]);
            
            break;
        }
        case PRIMITIVE_TRIANGLE_FAN:
        {
            for (int i = 1; i + 1 < swr->vertexCount; i += 2)
                swrPrimitiveTriangle(renderer, &swr->vertexData[0], &swr->vertexData[i], &swr->vertexData[i + 1]);
            
            break;
        }
        default:
        {
            logError("SWR: Unimplemented primitive type %d", swr->primitiveType);
            break;
        }
    }
    
    swr->primitiveBegun = false;
    swr->vertexCount = 0;
}

static void SWRenderer_drawVertex(Renderer* renderer, float x, float y, float z, uint32_t color, float alphaMod, float u, float v)
{
    swrOverlayFlush((SWRenderer*) renderer);
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (swr->vertexCount >= swr->maxVertexCount)
    {
        // TODO: we could just expand this?
        if (!swr->primitiveOverflow)
            logError("SWR: Vertex overflow.  Vertices will be ignored until the next primitiveEnd.");
        
        swr->primitiveOverflow = true;
    }
    
    SWVertex* pVertex = &swr->vertexData[swr->vertexCount++];
    
    Pixel32ABGR pixel;
    pixel.l = color;
    pixel.p.a = (uint8_t)(pixel.p.a * alphaMod);
    
    pVertex->color = swrConvertPixel(pixel.l);
    pVertex->x = x;
    pVertex->y = y;
    
    //logDebug("swr:      drawVertex(%f, %f)\n", x, y);
    
    // Texture mapping and the third dimension are not supported.
    (void) z;
    (void) u;
    (void) v;
}

static void SWRenderer_drawVertexBuffer(Renderer* renderer, VertexBuffer* buffer, int32_t primitive, int32_t texture, int32_t offset, int32_t count)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return;
    swrOverlayFlush((SWRenderer*) renderer);
    // TODO
    
    (void) renderer;
    (void) buffer;
    (void) primitive;
    (void) texture;
    (void) offset;
    (void) count;
    
    UNIMP();
}
// Draw-call timing (SW_DRAW_PROFILE): each kind of call is timed and the
// totals handed to the platform, which can then say what a frame was spent
// drawing. A timed call made inside another is taken off the outer one's
// time, so no time is counted twice: swrProfInner is the time already
// accounted for by calls that ended since some outer call began.
#ifdef SW_DRAW_PROFILE
#include "gettime.h"
void platformDrawProfile(int kind, uint64_t nanos);
void platformDrawCount(int kind);
enum { SWR_PROF_SPRITE, SWR_PROF_PART, SWR_PROF_TEXT, SWR_PROF_TILED, SWR_PROF_RECT };
static uint64_t swrProfInner = 0;
static uint64_t swrProfLast = 0; // what the SWR_PROFILED that ended last was charged
// Reading the clock is not free everywhere (17 us on openfpgaOS, a trap into
// the kernel, against 6 us for the rest of a sprite draw that draws nothing),
// so the platform says when calls are to be timed (swrDrawTimed); otherwise
// they are only counted.
#define SWR_PROFILED(kind, call) do { \
        if (!swrDrawTimed) { \
            call; \
            swrProfLast = 0; \
            if (!swrProbing) platformDrawCount(kind); \
            break; \
        } \
        uint64_t inner_ = swrProfInner, start_ = nowNanos(); \
        call; \
        uint64_t span_ = nowNanos() - start_; \
        swrProfLast = span_ - (swrProfInner - inner_); \
        platformDrawProfile(kind, swrProfLast); \
        swrProfInner = inner_ + span_; \
    } while (0)
#else
#define SWR_PROFILED(kind, call) do { call; } while (0)
#endif

// ===[ Held sprite grid ]===
// A game can floor a room with a grid of sprites and then, in a battle, paint
// the whole screen over it. A grid given in one call (drawSpriteGrid) is held
// back like a clear and the tile pictures are: it lies over a held clear and
// under held tile pictures, is drawn when anything else draws, and is dropped
// with them if an opaque fill of the whole screen comes first
// (swrHeldUnderDiscard). Drawing it is the same sprite draws, one per cell.
// While it is held nothing is held over it but tile pictures, so that those
// draws find nothing of their own kind half-built.
#define SWR_GRID_MAX_CELLS 4096
#define SWR_GRID_MAX 16

static struct {
    int count;              // grids held, in the order they were given; 0 for none
    bool above;             // the grids lie over the tile pictures being held, not under them
    int cells;              // of subimgs in use
    SWRenderer* owner;
    struct {
        int32_t sprite, cols, rows, first; // first: where its images start in subimgs
        float x, y, stepX, stepY, alpha;
    } grids[SWR_GRID_MAX];
    int32_t subimgs[SWR_GRID_MAX_CELLS];
} swrGrid;


bool swrGridHeld(void) { return swrGrid.count > 0; }

// Draws the held grids and leaves none held.
static void swrGridReplay(SWRenderer* swr)
{
    Renderer* renderer = (Renderer*) swr;
    float alpha = renderer->drawAlpha;
    int count = swrGrid.count;
    swrGrid.count = 0;
    swrGrid.cells = 0;
    for (int g = 0; g < count; g++) {
        renderer->drawAlpha = swrGrid.grids[g].alpha;
        const int32_t* subimgs = &swrGrid.subimgs[swrGrid.grids[g].first];
        int32_t cols = swrGrid.grids[g].cols, rows = swrGrid.grids[g].rows;
        // A cell whose sprite cannot reach the view is left out here: most of a
        // room's floor is somewhere else, and a sprite draw is a long way
        // round to find that out. The sprite's own box, with a pixel to spare.
        int32_t spriteIndex = swrGrid.grids[g].sprite;
        bool cull = spriteIndex >= 0 && (uint32_t) spriteIndex < renderer->dataWin->sprt.count && swr->scaleX > 0.0f && swr->scaleY > 0.0f;
        float boxLeft = 0, boxTop = 0, boxRight = 0, boxBottom = 0, viewLeft = 0, viewTop = 0, viewRight = 0, viewBottom = 0;
        if (cull) {
            const Sprite* sprite = &renderer->dataWin->sprt.sprites[spriteIndex];
            boxLeft = -(float) sprite->originX - 1.0f; boxRight = (float) sprite->width - (float) sprite->originX + 1.0f;
            boxTop = -(float) sprite->originY - 1.0f; boxBottom = (float) sprite->height - (float) sprite->originY + 1.0f;
            viewLeft = (float) swr->viewX; viewTop = (float) swr->viewY;
            viewRight = viewLeft + (float) (swr->maxX - swr->portX) / swr->scaleX;
            viewBottom = viewTop + (float) (swr->maxY - swr->portY) / swr->scaleY;
            if (sprite->nineSliceEnabled) cull = false;
        }
        for (int32_t i = 0; i < cols; i++) {
            float cellX = swrGrid.grids[g].x + swrGrid.grids[g].stepX * (float) i;
            if (cull && (cellX + boxRight < viewLeft || cellX + boxLeft > viewRight)) continue;
            for (int32_t j = 0; j < rows; j++) {
                float cellY = swrGrid.grids[g].y + swrGrid.grids[g].stepY * (float) j;
                if (cull && (cellY + boxBottom < viewTop || cellY + boxTop > viewBottom)) continue;
                Renderer_drawSprite(renderer, spriteIndex, subimgs[i * rows + j], cellX, cellY);
            }
        }
    }
    renderer->drawAlpha = alpha;
    swrOverlayFlush(swr); // a cell held back as an overlay goes out with the rest
}

void swrGridFlush(SWRenderer* swr)
{
    if (swrGrid.count == 0) return;
    if (swr == NULL) swr = swrGrid.owner;
    swrClearSettle(swr);
    swrGridReplay(swr);
}

static bool SWRenderer_drawSpriteGrid(Renderer* renderer, int32_t spriteIndex, const int32_t* subimgs, int32_t cols, int32_t rows,
                                      float x, float y, float stepX, float stepY)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return true;
    SWRenderer* swr = (SWRenderer*) renderer;
#if PIXEL_SIZE != 16 || !defined SW_HAS_PREMUL_BLEND
    (void) swr; (void) spriteIndex; (void) subimgs; (void) cols; (void) rows; (void) x; (void) y; (void) stepX; (void) stepY;
    return false;
#else
    if (cols <= 0 || rows <= 0) return true;
    if (cols > SWR_GRID_MAX_CELLS || rows > SWR_GRID_MAX_CELLS || cols * rows > SWR_GRID_MAX_CELLS) return false;
    if (swr->drawingToSurface) return false;
    
    // A grid joins the ones held if nothing has been drawn or held since:
    // only tile pictures can have been, over grids that lie under them.
    bool joins = swr->pipeCount == 0 && swrGrid.count > 0 && (swrGrid.above || swrPendingCount == 0) && swrGrid.count < SWR_GRID_MAX &&
                 swrGrid.cells + cols * rows <= SWR_GRID_MAX_CELLS;
    if (!joins) {
        // The first grid goes over the tile pictures being held, if any, and
        // a held clear; whatever else is held goes out.
        if (swrGrid.count > 0) swrOverlayFlush(swr);
        swr->tileRunEntering = true;
        swrOverlayFlush(swr);
        swr->tileRunEntering = false;
        if (swrPendingCount == 0) swrOverlayFlushForState(swr);
        swrGrid.above = swrPendingCount > 0;
    }
    swrGrid.owner = swr;
    int g = swrGrid.count++;
    swrGrid.grids[g].sprite = spriteIndex;
#ifdef SW_DRAW_PROFILE
    swrProbeGridSprite = spriteIndex;
#endif
    swrGrid.grids[g].cols = cols; swrGrid.grids[g].rows = rows;
    swrGrid.grids[g].first = swrGrid.cells;
    swrGrid.grids[g].x = x; swrGrid.grids[g].y = y;
    swrGrid.grids[g].stepX = stepX; swrGrid.grids[g].stepY = stepY;
    swrGrid.grids[g].alpha = renderer->drawAlpha;
    memcpy(&swrGrid.subimgs[swrGrid.cells], subimgs, (size_t) (cols * rows) * sizeof(int32_t));
    swrGrid.cells += cols * rows;
    return true;
#endif
}

// ===[ Tile run cache ]===
// A room's tiles are hundreds of small draws that come out the same every
// frame. A run of them that nothing else is drawn between is composed once
// into a picture the size of the run's bounding box, in room coordinates, and
// that picture is drawn in their place for as long as the run stays the same.
// The key describes tile appearance and relative positions. Moving a whole
// run reuses its picture; changing its contents or arrangement makes a new one.
#define SWR_TILE_RUN_MIN_TILES 16
// A shorter run over pictures being held joins them rather than letting them
// out, when an entry is free for it (it never pushes out another picture).
#define SWR_TILE_RUN_MIN_TILES_HELD 8
#define SWR_TILE_RUN_MAX_PIXELS (2 * 1024 * 1024)
// One run's picture may be bigger, up to a 1680x1568 layer at full size
// (Deltarune's dancers room), when the heap can spare it beside the
// texture cache's reserve.
#define SWR_TILE_RUN_MAX_PIXELS_BIG (3 * 1024 * 1024)
#define SWR_TILE_RUN_MAX_SIDE 4096
#define SWR_TILE_RUN_IDLE_FRAMES 300

// The picture is drawn block by block: a layer of decoration is mostly
// nothing, and a layer of ground mostly has no holes, so each block is
// marked empty (skipped), solid (copied a row at a time) or mixed.
#define SWR_TILE_RUN_BLOCK 16
enum { SWR_BLOCK_EMPTY, SWR_BLOCK_SOLID, SWR_BLOCK_MIXED };

// A picture is in the pixels the room is drawn in: room pixels, or every
// second one (shift 1) when the room is drawn at half size. Its place and
// size are in those units. Storage starts on a block boundary when built;
// a queued placement can translate that storage to any whole-pixel position.
typedef struct {
    uint64_t key;
    uint64_t serial;       // new for every allocation, even at the same address
    uintpixel_t* pixels;    // NULL when the entry is free
    uint8_t* blocks;        // one SWR_BLOCK_* per block, row by row; lives in the same allocation as pixels
    int x, y, width, height;
    int anchorX, anchorY;   // first tile's origin when built, in picture units
    int shift;
    uint32_t lastUsedFrame;
    uint32_t tint;          // a flattened picture's opaque pixels can have a fill worked in (swrPendingTint)
    // A wide picture is built a column of blocks at a time, as the view comes to it (swrRunEnsure):
    // one byte per column, nonzero once built, in the same allocation; NULL once all are (or for one
    // built whole). A column not yet built has its blocks marked empty, so nothing reads its pixels.
    uint8_t* built;
    // Of a run's picture built a stretch at a time: the run's tiles that reach each column of blocks,
    // in their order (colTiles[colStart[c]] up to colTiles[colStart[c + 1]]), in the same allocation.
    uint32_t* colStart;
    uint16_t* colTiles;
    // Where the view was when a picture built a stretch at a time was last drawn, on which frame, and
    // which way it last moved (-1 left, 1 right): while it stays put, columns are built ahead (swrRunAhead).
    int32_t viewLeft, viewTop;
    uint32_t viewFrame, bornFrame;
    int lastStep;
    // A column built ahead a few rows of blocks at a time (swrAheadTake): which (-1 none), and how
    // many of its rows of blocks are drawn. It is marked built only once all are.
    int partCol, partRows;
    // Of a flattened picture: swrColumnsBuiltCount when no column of it was found ready to build ahead.
    uint32_t noneReadyAt;
} SWTileRun;

static SWTileRun swrTileRuns[SWR_TILE_RUN_ENTRIES];
static uint64_t swrTileRunSerial;

// Pictures waiting to be drawn, bottom first. A room has several tile layers
// one over the other, and drawing each in turn fills most of the screen once
// per layer. Pictures that follow one another with nothing else drawn between
// are held and drawn together (swrTileRunsFlush): for each block of the
// screen, drawing starts at the topmost picture that is solid there, since
// nothing under it can show.
static SWTileRun* swrPendingRuns[SWR_TILE_RUN_ENTRIES]; // swrPendingCount, at the top of the file, counts them
MAYBE_UNUSED static SWTileRun swrPendingPlacements[SWR_TILE_RUN_ENTRIES]; // immutable placement per queued draw

// The same pictures held together frame after frame are flattened into one,
// so that the screen is gone over once and not once per layer. An entry with
// a key and no pixels is a set seen once, to be flattened if it comes again.
#define SWR_TILE_FLAT_ENTRIES 2
#define SWR_TILE_FLAT_RECENT 8 // frames; more than one, since a skipped frame draws nothing
static SWTileRun swrFlatRuns[SWR_TILE_FLAT_ENTRIES];

static int swrFloorToStep(int v, int step)
{
    int r = v % step;
    return r < 0 ? v - r - step : v - r;
}

static uint8_t swrRunBlockKind(const SWTileRun* run, int unitX, int unitY)
{
    int x0 = unitX - run->x, y0 = unitY - run->y;
    int x1 = x0 + SWR_TILE_RUN_BLOCK, y1 = y0 + SWR_TILE_RUN_BLOCK;
    if (x1 <= 0 || y1 <= 0 || x0 >= run->width || y0 >= run->height) return SWR_BLOCK_EMPTY;
    int columns = (run->width + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK;
    if (x0 >= 0 && y0 >= 0 && ((x0 | y0) & (SWR_TILE_RUN_BLOCK - 1)) == 0)
        return run->blocks[(y0 / SWR_TILE_RUN_BLOCK) * columns + x0 / SWR_TILE_RUN_BLOCK];

    // A translated destination block can touch four storage blocks. Only a
    // wholly covered rectangle of solid blocks may hide the pictures below.
    bool solid = x0 >= 0 && y0 >= 0 && x1 <= run->width && y1 <= run->height;
    bool empty = true;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > run->width) x1 = run->width;
    if (y1 > run->height) y1 = run->height;
    for (int by = y0 / SWR_TILE_RUN_BLOCK; by <= (y1 - 1) / SWR_TILE_RUN_BLOCK; by++) {
        for (int bx = x0 / SWR_TILE_RUN_BLOCK; bx <= (x1 - 1) / SWR_TILE_RUN_BLOCK; bx++) {
            uint8_t kind = run->blocks[by * columns + bx];
            if (kind != SWR_BLOCK_EMPTY) empty = false;
            if (kind != SWR_BLOCK_SOLID) solid = false;
        }
    }
    return empty ? SWR_BLOCK_EMPTY : solid ? SWR_BLOCK_SOLID : SWR_BLOCK_MIXED;
}

static uint64_t swrTileRunHash(uint64_t hash, const void* data, size_t bytes)
{
    const uint8_t* at = (const uint8_t*) data;
    for (size_t i = 0; i < bytes; i++) hash = (hash ^ at[i]) * BS_FNV64_PRIME;
    return hash;
}

// Marks blocks of rows r0 to r1 of columns c0 to c1 of a picture empty, solid or mixed.
static void swrRunClassifyBlocks(SWTileRun* run, int c0, int c1, int r0, int r1)
{
    int blocksX = (run->width + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK, blocksY = r1;
#if PIXEL_SIZE == 16 && defined SW_NO_SEMI_TRANSPARENT_TEXTURE_SUPPORT
    // A pixel is opaque by its top bit alone: a block's pixels ANDed together have it when all are,
    // ORed together when any is. A row of the picture at a time, every block of it, so that the
    // picture is read through once in order (building a room's took most of 150 ms simulated
    // counting pixel by pixel, block by block).
    if (blocksX <= SWR_TILE_RUN_MAX_SIDE / SWR_TILE_RUN_BLOCK) {
        uint16_t ands[SWR_TILE_RUN_MAX_SIDE / SWR_TILE_RUN_BLOCK], ors[SWR_TILE_RUN_MAX_SIDE / SWR_TILE_RUN_BLOCK];
        int fullX = run->width / SWR_TILE_RUN_BLOCK;
        int by, bx, y, x;
        for (by = r0; by < blocksY; by++) {
            int y0 = by * SWR_TILE_RUN_BLOCK, y1 = y0 + SWR_TILE_RUN_BLOCK;
            if (y1 > run->height) y1 = run->height;
            for (bx = c0; bx < c1; bx++) { ands[bx] = 0xFFFF; ors[bx] = 0; }
            int fullEnd = c1 < fullX ? c1 : fullX;
            for (y = y0; y < y1; y++) {
                const uint16_t* row = &run->pixels[y * run->width];
                for (bx = c0; bx < fullEnd; bx++) {
                    const uint16_t* at = &row[bx * SWR_TILE_RUN_BLOCK];
                    uint16_t a = ands[bx], o = ors[bx];
                    for (x = 0; x < SWR_TILE_RUN_BLOCK; x++) { a &= at[x]; o |= at[x]; }
                    ands[bx] = a; ors[bx] = o;
                }
                if (c1 > fullX)
                    for (x = fullX * SWR_TILE_RUN_BLOCK; x < run->width; x++) { ands[fullX] &= row[x]; ors[fullX] |= row[x]; }
            }
            // Solid means the whole block: one cut short by the picture's edge is not.
            bool wholeRow = y1 - y0 == SWR_TILE_RUN_BLOCK;
            for (bx = c0; bx < c1; bx++) {
                bool whole = wholeRow && bx < fullX;
                run->blocks[by * blocksX + bx] = (ors[bx] & TRANSPARENT_MASK) == 0 ? SWR_BLOCK_EMPTY :
                                                 ((ands[bx] & TRANSPARENT_MASK) != 0 && whole) ? SWR_BLOCK_SOLID : SWR_BLOCK_MIXED;
            }
        }
        return;
    }
#endif
    for (int by = r0; by < blocksY; by++) {
        for (int bx = c0; bx < c1; bx++) {
            int x1 = bx * SWR_TILE_RUN_BLOCK + SWR_TILE_RUN_BLOCK, y1 = by * SWR_TILE_RUN_BLOCK + SWR_TILE_RUN_BLOCK;
            if (x1 > run->width) x1 = run->width;
            if (y1 > run->height) y1 = run->height;
            int opaque = 0, total = 0;
            for (int y = by * SWR_TILE_RUN_BLOCK; y < y1; y++) {
                const uintpixel_t* row = &run->pixels[y * run->width];
                for (int x = bx * SWR_TILE_RUN_BLOCK; x < x1; x++, total++) opaque += swrIsOpaque(row[x]) ? 1 : 0;
            }
            // Solid means the whole block: one cut short by the picture's edge is not.
            bool whole = total == SWR_TILE_RUN_BLOCK * SWR_TILE_RUN_BLOCK;
            run->blocks[by * blocksX + bx] = opaque == 0 ? SWR_BLOCK_EMPTY : (opaque == total && whole) ? SWR_BLOCK_SOLID : SWR_BLOCK_MIXED;
        }
    }
}

// Marks each block of columns c0 to c1 of a picture empty, solid or mixed.
static void swrRunClassifyColumns(SWTileRun* run, int c0, int c1)
{
    swrRunClassifyBlocks(run, c0, c1, 0, (run->height + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK);
}

static void swrRunClassify(SWTileRun* run)
{
    swrRunClassifyColumns(run, 0, (run->width + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK);
}

#ifdef TEXTURE_CACHE_RESERVE_BYTES
extern int platformHeapCanReserve(size_t bytes);
#endif

// Whether a picture this big may be built: always up to SWR_TILE_RUN_MAX_PIXELS,
// beyond that only when the texture cache's reserve would still be free after it.
static bool swrRunFits(int width, int height)
{
    if (width * height <= SWR_TILE_RUN_MAX_PIXELS) return true;
    if (width * height > SWR_TILE_RUN_MAX_PIXELS_BIG) return false;
#ifdef TEXTURE_CACHE_RESERVE_BYTES
    size_t bytes = (size_t) width * height * sizeof(uintpixel_t) + TEXTURE_CACHE_RESERVE_BYTES;
#if defined(OF_NATIVE_SAVESTATES) && !defined(OF_PC)
    return platformHeapCanReserve(bytes) > 0;
#else
    void* probe = malloc(bytes);
    if (probe == NULL) return false;
    free(probe);
    return true;
#endif
#else
    return true;
#endif
}

// Allocates a picture's pixels (cleared) and block marks in one piece, and with `lazy`
// the marks of which columns are built (none yet).
static bool swrRunAllocateWith(SWTileRun* run, int width, int height, bool lazy, size_t indexEntries)
{
    int blocksX = (width + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK, blocksY = (height + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK;
    size_t pixelBytes = (size_t) width * height * sizeof(uintpixel_t);
    size_t builtBytes = lazy ? (size_t) blocksX : 0;
    size_t marksEnd = pixelBytes + (size_t) blocksX * blocksY + builtBytes;
    size_t indexAt = (marksEnd + 3) & ~(size_t) 3;
    size_t total = indexEntries == 0 ? marksEnd : indexAt + ((size_t) blocksX + 1) * sizeof(uint32_t) + indexEntries * sizeof(uint16_t);
    uintpixel_t* pixels = (uintpixel_t*) calloc(total, 1);
    if (pixels == NULL) return false;
    run->serial = ++swrTileRunSerial;
    run->pixels = pixels;
    run->blocks = (uint8_t*) pixels + pixelBytes;
    run->built = lazy ? run->blocks + (size_t) blocksX * blocksY : NULL;
    run->colStart = indexEntries == 0 ? NULL : (uint32_t*) ((uint8_t*) pixels + indexAt);
    run->colTiles = indexEntries == 0 ? NULL : (uint16_t*) (run->colStart + blocksX + 1);
    run->viewFrame = 0;
    run->lastStep = 1;
    run->partCol = -1;
    run->partRows = 0;
    run->noneReadyAt = UINT32_MAX;
    run->width = width; run->height = height;
    return true;
}

static bool swrRunAllocate(SWTileRun* run, int width, int height, bool lazy)
{
    return swrRunAllocateWith(run, width, height, lazy, 0);
}

// The columns of blocks a tile at x (in a picture's units), w wide, reaches in a picture blocksX
// columns wide: c0 to c1, none when c0 >= c1. A tile of no width reaches none (it draws nothing).
#if PIXEL_SIZE == 16 // only swrDrawTileRunCached's 16-bit path builds pictures lazily
static void swrTileColumns(int x, int w, int blocksX, int* c0, int* c1)
{
    if (w <= 0) { *c0 = *c1 = 0; return; }
    int from = swrFloorToStep(x, SWR_TILE_RUN_BLOCK) / SWR_TILE_RUN_BLOCK;
    int to = swrFloorToStep(x + w - 1, SWR_TILE_RUN_BLOCK) / SWR_TILE_RUN_BLOCK + 1;
    *c0 = from < 0 ? 0 : from;
    *c1 = to > blocksX ? blocksX : to;
}

// Allocates a run's picture to be built a stretch at a time, with the list of its tiles that reach
// each column, the run's tiles placed with the picture's left edge at originX.
static bool swrRunAllocateIndexed(SWTileRun* run, int width, int height, RoomTile** tiles, const float* offsets, int32_t count, int originX)
{
    int blocksX = (width + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK;
    size_t entries = 0;
    for (int32_t t = 0; t < count; t++) {
        int c0, c1;
        swrTileColumns(tiles[t]->x + (int32_t) offsets[t * 2] - originX, (int) tiles[t]->width, blocksX, &c0, &c1);
        if (c1 > c0) entries += (size_t) (c1 - c0);
    }
    if (!swrRunAllocateWith(run, width, height, true, entries + 1)) return false;
    uint32_t* start = run->colStart;
    for (int32_t t = 0; t < count; t++) {
        int c0, c1;
        swrTileColumns(tiles[t]->x + (int32_t) offsets[t * 2] - originX, (int) tiles[t]->width, blocksX, &c0, &c1);
        for (int c = c0; c < c1; c++) start[c + 1]++;
    }
    for (int c = 0; c < blocksX; c++) start[c + 1] += start[c];
    // Filled using start[c] as column c's cursor, which leaves it at column c + 1's start.
    for (int32_t t = 0; t < count; t++) {
        int c0, c1;
        swrTileColumns(tiles[t]->x + (int32_t) offsets[t * 2] - originX, (int) tiles[t]->width, blocksX, &c0, &c1);
        for (int c = c0; c < c1; c++) run->colTiles[start[c]++] = (uint16_t) t;
    }
    for (int c = blocksX; c > 0; c--) start[c] = start[c - 1];
    start[0] = 0;
    return true;
}
#endif

// The columns of blocks of a picture placed at placeX (in its units) that pixels left to right of
// the buffer's port reach, with `margin` pixels to spare, cut to the picture.
static void swrRunColumnsFor(const SWTileRun* run, int placeX, int left, int right, int margin, int* c0, int* c1)
{
    int blocksX = (run->width + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK;
    int from = swrFloorToStep(left - margin - placeX, SWR_TILE_RUN_BLOCK) / SWR_TILE_RUN_BLOCK;
    int to = (right + margin - placeX + SWR_TILE_RUN_BLOCK - 1);
    to = to < 0 ? 0 : to / SWR_TILE_RUN_BLOCK;
    *c0 = from < 0 ? 0 : from > blocksX ? blocksX : from;
    *c1 = to < *c0 ? *c0 : to > blocksX ? blocksX : to;
}

// Whether columns c0 to c1 are all built.
static bool swrRunColumnsBuilt(const SWTileRun* run, int c0, int c1)
{
    if (run->built == NULL) return true;
    for (int c = c0; c < c1; c++)
        if (!run->built[c]) return false;
    return true;
}

// Counts columns marked built (swrRunColumnsMark), so that a flattened picture waiting for the
// pictures under it looks again only once one is (swrFlatAheadColumn).
static uint32_t swrColumnsBuiltCount;

// Marks columns c0 to c1 built, and lets the marks go once every column is.
static void swrRunColumnsMark(SWTileRun* run, int c0, int c1)
{
    swrColumnsBuiltCount++;
    int blocksX = (run->width + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK;
    memset(run->built + c0, 1, (size_t) (c1 - c0));
    int c = 0;
    while (c < blocksX && run->built[c]) c++;
    if (c == blocksX) run->built = NULL;
}

// Draws pictures, bottom first, onto pixels whose top left is at (left, top)
// in the pictures' units and that are width by height, block by block. With
// a colour to go under them, the pixels are first cleared to it wherever no
// picture is solid.
static void swrRunDrawStripes(const SWTileRun* run, uintpixel_t* pixels, int pitch, int left, int top, int width, int height, const uintpixel_t* under, bool underOnly)
{
    int right = left + width, bottom = top + height;
    for (int by = swrFloorToStep(top, SWR_TILE_RUN_BLOCK); by < bottom; by += SWR_TILE_RUN_BLOCK) {
        int y0 = by < top ? top : by;
        int y1 = by + SWR_TILE_RUN_BLOCK > bottom ? bottom : by + SWR_TILE_RUN_BLOCK;
        for (int bx = swrFloorToStep(left, SWR_TILE_RUN_BLOCK); bx < right;) {
            uint8_t kind = swrRunBlockKind(run, bx, by);
            int end = bx + SWR_TILE_RUN_BLOCK;
            while (end < right && swrRunBlockKind(run, end, by) == kind) end += SWR_TILE_RUN_BLOCK;
            int x0 = bx < left ? left : bx, x1 = end > right ? right : end;
            bx = end;
            if (under != NULL && kind != SWR_BLOCK_SOLID)
                for (int y = y0; y < y1; ++y)
                    swrFillPixels(&pixels[(y - top) * pitch + x0 - left], (size_t)(x1 - x0), *under);
            if (underOnly || kind == SWR_BLOCK_EMPTY) continue;
            int sy0 = y0 < run->y ? run->y : y0;
            int sy1 = y1 > run->y + run->height ? run->y + run->height : y1;
            if (x0 < run->x) x0 = run->x;
            if (x1 > run->x + run->width) x1 = run->x + run->width;
            if (x0 >= x1) continue;
            for (int y = sy0; y < sy1; ++y) {
                const uintpixel_t* src = &run->pixels[(y - run->y) * run->width + x0 - run->x];
                uintpixel_t* dst = &pixels[(y - top) * pitch + x0 - left];
                if (kind == SWR_BLOCK_SOLID) memcpy(dst, src, (size_t)(x1 - x0) * sizeof(uintpixel_t));
                else for (int x = 0; x < x1 - x0; ++x) {
                    uintpixel_t pixel = src[x];
                    if (swrIsOpaque(pixel)) dst[x] = pixel;
                }
            }
        }
    }
}

// The pictures from `first` up, over block (bx, by), cut to (tx0, ty0)-(tx1, ty1).
static void swrRunsDrawBlock(SWTileRun* const* runs, int count, int first, uintpixel_t* pixels, int pitch, int left, int top,
                             int bx, int by, int tx0, int tx1, int ty0, int ty1)
{
    for (int r = first; r < count; r++)
    {
        const SWTileRun* run = runs[r];
        uint8_t kind = swrRunBlockKind(run, bx, by);
        if (kind == SWR_BLOCK_EMPTY) continue;
        
        // And to the picture.
        int x0 = tx0, x1 = tx1, y0 = ty0, y1 = ty1;
        if (x0 < run->x) x0 = run->x;
        if (y0 < run->y) y0 = run->y;
        if (x1 > run->x + run->width) x1 = run->x + run->width;
        if (y1 > run->y + run->height) y1 = run->y + run->height;
        if (x0 >= x1 || y0 >= y1) continue;
        
        for (int y = y0; y < y1; y++)
        {
            const uintpixel_t* src = &run->pixels[(y - run->y) * run->width + (x0 - run->x)];
            uintpixel_t* dst = &pixels[(y - top) * pitch + (x0 - left)];
            if (kind == SWR_BLOCK_SOLID)
                memcpy(dst, src, (size_t) (x1 - x0) * sizeof(uintpixel_t));
            else for (int x = 0; x < x1 - x0; x++) {
                uintpixel_t pixel = src[x];
                if (swrIsOpaque(pixel)) dst[x] = pixel;
            }
        }
    }
}

static void swrRunsDrawOnto(SWTileRun* const* runs, int count, uintpixel_t* pixels, int pitch, int left, int top, int width, int height, const uintpixel_t* under, bool underOnly)
{
    // One picture (including flattened layers) can copy adjacent solid blocks
    // in one row span, keeping classification and transparency semantics.
    if (count == 1) {
        swrRunDrawStripes(runs[0], pixels, pitch, left, top, width, height, under, underOnly);
        return;
    }
    int right = left + width, bottom = top + height;
    for (int by = swrFloorToStep(top, SWR_TILE_RUN_BLOCK); by < bottom; by += SWR_TILE_RUN_BLOCK)
    {
        for (int bx = swrFloorToStep(left, SWR_TILE_RUN_BLOCK); bx < right; bx += SWR_TILE_RUN_BLOCK)
        {
            int first = 0;
            bool covered = false;
            for (int i = count - 1; i >= 0; i--) {
                if (swrRunBlockKind(runs[i], bx, by) == SWR_BLOCK_SOLID) { first = i; covered = true; break; }
            }
            // The block, cut to the target.
            int tx0 = bx < left ? left : bx, tx1 = bx + SWR_TILE_RUN_BLOCK > right ? right : bx + SWR_TILE_RUN_BLOCK;
            int ty0 = by < top ? top : by, ty1 = by + SWR_TILE_RUN_BLOCK > bottom ? bottom : by + SWR_TILE_RUN_BLOCK;
            if (under != NULL && !covered) {
                for (int y = ty0; y < ty1; y++)
                    swrFillPixels(&pixels[(y - top) * pitch + (tx0 - left)], (size_t) (tx1 - tx0), *under);
            }
            if (underOnly) continue; // the pictures themselves come in a second pass
            swrRunsDrawBlock(runs, count, first, pixels, pitch, left, top, bx, by, tx0, tx1, ty0, ty1);
        }
    }
}

// Pictures over a held clear with tiled passes held over it (a room's backdrop,
// Snowdin Town's bg_tb): the clear and the passes are worked out only in the
// blocks no picture is solid over, a row's stretch of such blocks at a time,
// and the pictures go over them as swrRunsDrawOnto puts them over a colour.
// The rest of the backdrop would only have been painted over. The port is the
// whole main buffer (the caller has checked).
// The same for one picture (the usual case: the room's layers flattened into
// one), a row of the buffer at a time from left to right: each stretch of
// solid blocks is one copy, each stretch of the others the held clear and
// passes, then the picture's opaque pixels where it has any.
static void swrRunDrawOverHeldRows(SWRenderer* swr, const SWTileRun* run, int left, int top, int width, int height)
{
    enum { SWR_OVER_HELD_ROW_BLOCKS = 1024 / SWR_TILE_RUN_BLOCK + 2 };
    uint8_t kinds[SWR_OVER_HELD_ROW_BLOCKS];
    int pitch = swr->mainWidth;
    int right = left + width, bottom = top + height;
    int firstBx = swrFloorToStep(left, SWR_TILE_RUN_BLOCK);
    int runRight = run->x + run->width, runBottom = run->y + run->height;
    int by;
    for (by = swrFloorToStep(top, SWR_TILE_RUN_BLOCK); by < bottom; by += SWR_TILE_RUN_BLOCK)
    {
        int ty0 = by < top ? top : by, ty1 = by + SWR_TILE_RUN_BLOCK > bottom ? bottom : by + SWR_TILE_RUN_BLOCK;
        int blocks = 0, bx, y;
        for (bx = firstBx; bx < right; bx += SWR_TILE_RUN_BLOCK) kinds[blocks++] = swrRunBlockKind(run, bx, by);
        for (y = ty0; y < ty1; y++)
        {
            uintpixel_t* dst = &swr->mainFb[(y - top) * pitch];
            bool inRun = y >= run->y && y < runBottom;
            const uintpixel_t* src = inRun ? &run->pixels[(y - run->y) * run->width] : NULL;
            int b = 0;
            while (b < blocks) {
                bool solid = kinds[b] == SWR_BLOCK_SOLID;
                int end = b + 1;
                while (end < blocks && (kinds[end] == SWR_BLOCK_SOLID) == solid) end++;
                int x0 = firstBx + b * SWR_TILE_RUN_BLOCK, x1 = firstBx + end * SWR_TILE_RUN_BLOCK;
                if (x0 < left) x0 = left;
                if (x1 > right) x1 = right;
                if (solid) {
                    // A solid block lies wholly inside the picture.
                    memcpy(&dst[x0 - left], &src[x0 - run->x], (size_t) (x1 - x0) * sizeof(uintpixel_t));
                } else {
                    swrTiledHeldSpan(swr, y - top, x0 - left, x1 - left);
                    int m;
                    for (m = b; m < end && inRun; m++) {
                        if (kinds[m] != SWR_BLOCK_MIXED) continue;
                        int mx0 = firstBx + m * SWR_TILE_RUN_BLOCK, mx1 = mx0 + SWR_TILE_RUN_BLOCK;
                        if (mx0 < x0) mx0 = x0;
                        if (mx1 > x1) mx1 = x1;
                        if (mx0 < run->x) mx0 = run->x;
                        if (mx1 > runRight) mx1 = runRight;
                        int x;
                        for (x = mx0; x < mx1; x++) {
                            uintpixel_t pixel = src[x - run->x];
                            if (swrIsOpaque(pixel)) dst[x - left] = pixel;
                        }
                    }
                }
                b = end;
            }
        }
    }
}

static void swrRunsDrawOverHeld(SWRenderer* swr, SWTileRun* const* runs, int count, int left, int top, int width, int height)
{
    if (count == 1) {
        swrRunDrawOverHeldRows(swr, runs[0], left, top, width, height);
        swrTiledHeldSpanEnd(swr);
        return;
    }
    uintpixel_t* pixels = swr->mainFb;
    int pitch = swr->mainWidth;
    int right = left + width, bottom = top + height;
    int firstBx = swrFloorToStep(left, SWR_TILE_RUN_BLOCK);
    enum { SWR_OVER_HELD_BLOCKS = 1024 / SWR_TILE_RUN_BLOCK + 2 };
    int8_t firsts[SWR_OVER_HELD_BLOCKS];
    for (int by = swrFloorToStep(top, SWR_TILE_RUN_BLOCK); by < bottom; by += SWR_TILE_RUN_BLOCK)
    {
        int ty0 = by < top ? top : by, ty1 = by + SWR_TILE_RUN_BLOCK > bottom ? bottom : by + SWR_TILE_RUN_BLOCK;
        // Which blocks of this row a picture is solid over (the topmost such picture, or -1).
        int blocks = 0;
        for (int bx = firstBx; bx < right; bx += SWR_TILE_RUN_BLOCK, blocks++) {
            firsts[blocks] = -1;
            for (int i = count - 1; i >= 0; i--)
                if (swrRunBlockKind(runs[i], bx, by) == SWR_BLOCK_SOLID) { firsts[blocks] = (int8_t) i; break; }
        }
        // The held clear and passes under each stretch of blocks without one.
        int b; // one declaration for both loops (MSVC 4.0 scopes a for's int to the block)
        for (b = 0; b < blocks; ) {
            if (firsts[b] >= 0) { b++; continue; }
            int end = b;
            while (end < blocks && firsts[end] < 0) end++;
            int x0 = firstBx + b * SWR_TILE_RUN_BLOCK, x1 = firstBx + end * SWR_TILE_RUN_BLOCK;
            if (x0 < left) x0 = left;
            if (x1 > right) x1 = right;
            for (int y = ty0; y < ty1; y++) swrTiledHeldSpan(swr, y - top, x0 - left, x1 - left);
            b = end;
        }
        // Then the pictures.
        for (b = 0; b < blocks; b++) {
            int bx = firstBx + b * SWR_TILE_RUN_BLOCK;
            int tx0 = bx < left ? left : bx, tx1 = bx + SWR_TILE_RUN_BLOCK > right ? right : bx + SWR_TILE_RUN_BLOCK;
            swrRunsDrawBlock(runs, count, firsts[b] < 0 ? 0 : firsts[b], pixels, pitch, left, top, bx, by, tx0, tx1, ty0, ty1);
        }
    }
    swrTiledHeldSpanEnd(swr);
}

// The rows of blocks of a picture.
static int swrRunBlockRows(const SWTileRun* run)
{
    return (run->height + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK;
}

// After rows r0 to r1 of blocks of columns c0 to c1 are drawn, their blocks are marked (so that a
// tall column built ahead pays for that a few rows at a time too); once a column's last row is, it
// is built. Until then (one column, built ahead) it is the picture's part column: its rows still to
// be drawn are marked empty, and the pixels of the rows drawn are already the whole picture's.
static void swrRunRowsDrawn(SWTileRun* run, int c0, int c1, int r0, int r1)
{
    swrRunClassifyBlocks(run, c0, c1, r0, r1);
    if (r1 < swrRunBlockRows(run)) {
        run->partCol = c0;
        run->partRows = r1;
        return;
    }
    if (run->partCol >= c0 && run->partCol < c1) run->partCol = -1;
    swrRunColumnsMark(run, c0, c1);
}

// The pictures being held flattened into one, or NULL: when they have not
// been held together before, are too big together, or there is no room.
// Draws rows r0 to r1 of blocks of columns c0 to c1 of a flattened picture from the pictures being
// held (which the port's stretch of has been built of, swrRunEnsure), works in its tint, and marks
// their blocks once their last row is drawn (swrRunRowsDrawn).
static void swrFlatDrawColumns(SWTileRun* entry, int c0, int c1, int r0, int r1)
{
    int x0 = c0 * SWR_TILE_RUN_BLOCK, x1 = swrMin(c1 * SWR_TILE_RUN_BLOCK, entry->width);
    int y0 = r0 * SWR_TILE_RUN_BLOCK, y1 = swrMin(r1 * SWR_TILE_RUN_BLOCK, entry->height);
    swrRunsDrawOnto(swrPendingRuns, swrPendingCount, entry->pixels + (size_t) y0 * entry->width + x0, entry->width,
                    entry->x + x0, entry->y + y0, x1 - x0, y1 - y0, NULL, false);
#ifdef SW_HAS_PREMUL_BLEND
    if (entry->tint != 0) {
        uint32_t alpha = entry->tint >> 16;
        uintpixel_t color = (uintpixel_t) (entry->tint & 0xffff);
        uint32_t srcRedBlue = swrSpreadRedBlue(color) * alpha, srcGreen = swrGreen(color) * alpha, dstalpha = 256 - alpha;
        for (int y = y0; y < y1; y++) {
            uintpixel_t* row = &entry->pixels[(size_t) y * entry->width];
            for (int x = x0; x < x1; x++)
                if (swrIsOpaque(row[x])) row[x] = swrBlendPremultiplied(row[x], srcRedBlue, srcGreen, dstalpha);
        }
    }
#endif
    swrRunRowsDrawn(entry, c0, c1, r0, r1);
}

// The columns of a flattened picture built a stretch at a time that the port shows (left, in the
// pictures' units, and width), built if they are not yet.
static void swrFlatEnsure(SWTileRun* entry, int left, int width)
{
    if (entry->built == NULL) return;
    int c0, c1;
    swrRunColumnsFor(entry, entry->x, left, left + width, SWR_TILE_RUN_BLOCK, &c0, &c1);
    int c = c0;
    while (c < c1 && entry->built != NULL) {
        if (entry->built[c]) { c++; continue; }
        if (c == entry->partCol) { swrFlatDrawColumns(entry, c, c + 1, entry->partRows, swrRunBlockRows(entry)); c++; continue; }
        int end = c;
        while (end < c1 && !entry->built[end] && end != entry->partCol) end++;
        swrFlatDrawColumns(entry, c, end, 0, swrRunBlockRows(entry));
        c = end;
    }
}

// How much of the pictures built a stretch at a time is built ahead of the view in a frame, all
// pictures together: so that standing still, not walking, pays for most of a wide picture
// (a walk at 3 pixels a frame needs a new 16-pixel column of each every 5 or 6 frames), at no more
// than about a millisecond a frame on the device. Counted in units of estimated work
// (swrRunAhead, swrFlatAhead) of about 40 us simulated each (4000 cycles at rt-os20's 100 MHz); a
// column taller than this is built over several frames.
#ifndef SWR_AHEAD_UNITS
#define SWR_AHEAD_UNITS 18
#endif
// While the view moves, only the columns this near the port's edge are built ahead (the port needs
// them in a few frames, swrRunEnsure, and would build them in one frame, every picture at once):
// a walk into a stretch not built yet pays for it a few rows a frame, a walk over built ones nothing.
#ifndef SWR_AHEAD_NEAR_COLUMNS
#define SWR_AHEAD_NEAR_COLUMNS 6
#endif
// Nor in the first frames of a picture: a room's entry and the fade-in after it are slow already.
#define SWR_AHEAD_SETTLE_FRAMES 12

// Who builds ahead in a frame. The pictures that wanted to last frame take turns, in the order they
// were drawn: the frame's units are the next one's, or anyone's once it has said it cannot use
// them all (a column finished, or a flattened picture waiting for the pictures under it) or when
// none wanted to last frame.
#define SWR_AHEAD_SEEN 8
static struct {
    uint32_t frame;
    int left;
    bool open;
    uint64_t grant, lastServed;
    uint64_t seen[SWR_AHEAD_SEEN], previous[SWR_AHEAD_SEEN];
    int seenCount, previousCount;
} swrAhead;

// How many units of work the picture may build ahead with now (0: none).
static int swrAheadTake(const SWRenderer* swr, const SWTileRun* run)
{
    if (swrAhead.frame != swr->frameCounter) {
        bool followed = swrAhead.frame + 1 == swr->frameCounter;
        swrAhead.previousCount = followed ? swrAhead.seenCount : 0;
        memcpy(swrAhead.previous, swrAhead.seen, sizeof(swrAhead.seen));
        swrAhead.seenCount = 0;
        swrAhead.frame = swr->frameCounter;
        swrAhead.left = SWR_AHEAD_UNITS;
        swrAhead.open = swrAhead.previousCount == 0;
        if (!swrAhead.open) {
            int at = 0;
            for (int i = 0; i < swrAhead.previousCount; i++)
                if (swrAhead.previous[i] == swrAhead.lastServed) at = i + 1;
            swrAhead.grant = swrAhead.previous[at % swrAhead.previousCount];
        }
    }
    bool listed = false;
    for (int i = 0; i < swrAhead.seenCount; i++) listed |= swrAhead.seen[i] == run->serial;
    if (!listed && swrAhead.seenCount < SWR_AHEAD_SEEN) swrAhead.seen[swrAhead.seenCount++] = run->serial;
    if (swrAhead.left <= 0 || !(swrAhead.open || swrAhead.grant == run->serial)) return 0;
    return swrAhead.left;
}

// The picture given `offered` units by swrAheadTake used this many: fewer when it could not use
// them (a column finished, or none ready), and then anyone drawn after it may have the rest.
static void swrAheadSpent(const SWTileRun* run, int units, int offered)
{
    swrAhead.left -= units;
    if (units > 0) swrAhead.lastServed = run->serial;
    if (units < offered) swrAhead.open = true;
}

// The next `rows` rows of blocks of column c to build ahead (fewer at its end): from r0 to r1.
static void swrAheadRows(const SWTileRun* run, int c, int rows, int* r0, int* r1)
{
    *r0 = c == run->partCol ? run->partRows : 0;
    *r1 = swrMin(*r0 + rows, swrRunBlockRows(run));
}

// Whether column c of a flattened picture can be drawn: the pictures held under it have it built.
static bool swrFlatColumnReady(const SWTileRun* entry, int c)
{
    int x0 = entry->x + c * SWR_TILE_RUN_BLOCK, x1 = x0 + SWR_TILE_RUN_BLOCK;
    for (int held = 0; held < swrPendingCount; held++) {
        int h0, h1;
        swrRunColumnsFor(swrPendingRuns[held], swrPendingRuns[held]->x, x0, x1, 0, &h0, &h1);
        if (!swrRunColumnsBuilt(swrPendingRuns[held], h0, h1)) return false;
    }
    return true;
}

// The next column of a flattened picture to build ahead of a view showing columns c0 to c1: the one
// begun, else the nearest not built whose pictures under it are, on the side the view last moved to
// first (those pictures are built outward from the view, but may have been made after it); -1 if
// none is ready.
static int swrFlatAheadColumn(SWTileRun* entry, int c0, int c1)
{
    // The one begun first; it waits if the pictures under it went out and came back since (a room
    // left and entered again keeps a flattened picture of the same pictures).
    if (entry->partCol >= 0) return swrFlatColumnReady(entry, entry->partCol) ? entry->partCol : -1;
    if (entry->noneReadyAt == swrColumnsBuiltCount) return -1; // nothing under it built since it looked
    int blocksX = (entry->width + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK;
    for (int side = 0; side < 2; side++) {
        int step = (side == 0) == (entry->lastStep >= 0) ? 1 : -1;
        for (int c = step > 0 ? c1 : c0 - 1; c >= 0 && c < blocksX; c += step)
            if (!entry->built[c] && swrFlatColumnReady(entry, c)) return c;
    }
    entry->noneReadyAt = swrColumnsBuiltCount;
    return -1;
}

// While the view stays put (or moves, near the port's edge), builds a little of a flattened picture
// built a stretch at a time ahead of it, as swrRunAhead does for the pictures it is made of (swrFlatAheadColumn), with the units
// swrAheadTake gives it.
static void swrFlatAhead(SWRenderer* swr, SWTileRun* entry, int left, int width)
{
    int32_t top = 0;
    bool followed = entry->viewFrame + 1 == swr->frameCounter;
    bool still = followed && entry->viewLeft == left && entry->viewTop == top;
    if (followed && entry->viewLeft != left) entry->lastStep = left > entry->viewLeft ? 1 : -1;
    entry->viewFrame = swr->frameCounter;
    entry->viewLeft = left; entry->viewTop = top;
    if (!followed || entry->built == NULL || swr->frameCounter - entry->bornFrame <= SWR_AHEAD_SETTLE_FRAMES) return;
    int c0, c1;
    swrRunColumnsFor(entry, entry->x, left, left + width, 0, &c0, &c1);
    int c = swrFlatAheadColumn(entry, c0, c1);
    if (!still && (c < c0 - SWR_AHEAD_NEAR_COLUMNS || c >= c1 + SWR_AHEAD_NEAR_COLUMNS)) return;
    int units = swrAheadTake(swr, entry), cost = 0;
    if (units == 0) return;
    SWR_NOTE_START(aheadNote);
    if (c >= 0) {
        // A block of it reads each picture under it and writes its own, a row of pixels of each at a
        // time (rt-os20: about 6000 cycles with three pictures, 10000 with five or six).
        int perBlock = ((swrPendingCount + 1) * 3 + 7) / 8 + (entry->tint != 0 ? 1 : 0), r0, r1;
        swrAheadRows(entry, c, swrMax(1, units / perBlock), &r0, &r1);
        swrFlatDrawColumns(entry, c, c + 1, r0, r1);
        cost = swrMin(units, (r1 - r0) * perBlock);
        if (r1 < swrRunBlockRows(entry)) cost = units; // the rest waits for the next frame
    }
    swrAheadSpent(entry, cost, units);
    SWR_NOTE_END(aheadNote, "  of which: tile picture columns built ahead");
}

static SWTileRun* swrPendingFlattened(SWRenderer* swr, uint32_t tint, int viewLeft, int viewWidth)
{
    uint64_t key = BS_FNV64_OFFSET;
    key = swrTileRunHash(key, &tint, sizeof(tint));
    int left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
    for (int i = 0; i < swrPendingCount; i++) {
        const SWTileRun* run = swrPendingRuns[i];
        key = swrTileRunHash(key, &run->key, sizeof(run->key));
        int32_t placement[4] = { run->x, run->y, run->anchorX, run->anchorY };
        key = swrTileRunHash(key, placement, sizeof(placement));
        if (run->x < left) left = run->x;
        if (run->y < top) top = run->y;
        if (run->x + run->width > right) right = run->x + run->width;
        if (run->y + run->height > bottom) bottom = run->y + run->height;
    }
    int width = right - left, height = bottom - top;
    if (width > SWR_TILE_RUN_MAX_SIDE || height > SWR_TILE_RUN_MAX_SIDE || width * height > SWR_TILE_RUN_MAX_PIXELS_BIG) return NULL;
    
    SWTileRun* spare = NULL;
    for (int e = 0; e < SWR_TILE_FLAT_ENTRIES; e++)
    {
        SWTileRun* entry = &swrFlatRuns[e];
        bool recent = swr->frameCounter - entry->lastUsedFrame <= SWR_TILE_FLAT_RECENT;
        if (entry->key == key && (entry->pixels != NULL || recent))
        {
            entry->lastUsedFrame = swr->frameCounter;
            if (entry->pixels != NULL) {
                swrFlatEnsure(entry, viewLeft, viewWidth);
                swrFlatAhead(swr, entry, viewLeft, viewWidth);
                return entry;
            }
            // Seen a moment ago too: worth a picture of its own. Built a stretch at a time too
            // when one of the pictures is (each is the same picture wherever it is built).
            bool lazy = false;
            for (int held = 0; held < swrPendingCount; held++) if (swrPendingRuns[held]->built != NULL) lazy = true;
            if (!swrRunFits(width, height) || !swrRunAllocate(entry, width, height, lazy)) return NULL;
            entry->x = left; entry->y = top;
            entry->shift = swrPendingRuns[0]->shift;
            entry->bornFrame = swr->frameCounter;
            if (lazy) {
                entry->tint = tint;
                swrFlatEnsure(entry, viewLeft, viewWidth);
                return entry;
            }
            swrRunsDrawOnto(swrPendingRuns, swrPendingCount, entry->pixels, width, left, top, width, height, NULL, false);
            entry->tint = tint;
#ifdef SW_HAS_PREMUL_BLEND
            if (tint != 0) {
                // Only the pixels with something in them: the gaps still show what is under the picture.
                uint32_t alpha = tint >> 16;
                uintpixel_t color = (uintpixel_t) (tint & 0xffff);
                uint32_t srcRedBlue = swrSpreadRedBlue(color) * alpha, srcGreen = swrGreen(color) * alpha, dstalpha = 256 - alpha;
                for (size_t i = 0, n = (size_t) width * height; i < n; i++)
                    if (swrIsOpaque(entry->pixels[i]))
                        entry->pixels[i] = swrBlendPremultiplied(entry->pixels[i], srcRedBlue, srcGreen, dstalpha);
            }
#endif
            swrRunClassify(entry);
            return entry;
        }
        // One not used for a while can be given to this set.
        if (!recent && (spare == NULL || entry->lastUsedFrame < spare->lastUsedFrame)) spare = entry;
    }
    if (spare != NULL) {
        free(spare->pixels);
        spare->pixels = NULL;
        spare->key = key;
        spare->lastUsedFrame = swr->frameCounter;
    }
    return NULL;
}

// Works a held translucent fill (swrPendingTint) into rows just drawn, as swrDrawHLineInt would over them.
// A tint is only ever held where swrTintFoldable().
static void swrTintRows(uintpixel_t* pixels, int pitch, int width, int height, uint32_t tint)
{
#ifdef SW_HAS_PREMUL_BLEND
    uint32_t alpha = tint >> 16;
    uintpixel_t color = (uintpixel_t) (tint & 0xffff);
    uint32_t srcRedBlue = swrSpreadRedBlue(color) * alpha, srcGreen = swrGreen(color) * alpha, dstalpha = 256 - alpha;
    for (int y = 0; y < height; y++)
        swrBlendSolidRow(&pixels[y * pitch], (size_t) width, srcRedBlue, srcGreen, dstalpha);
#else
    (void) pixels; (void) pitch; (void) width; (void) height; (void) tint;
#endif
}

// swrRunsDrawOnto, then the tint if there is one, a band of rows at a time while the band is still in the cache.
static void swrRunsDrawTinted(SWTileRun* const* runs, int count, uintpixel_t* pixels, int pitch, int left, int top, int width, int height, const uintpixel_t* under, uint32_t tint)
{
    if (tint == 0) {
        swrRunsDrawOnto(runs, count, pixels, pitch, left, top, width, height, under, false);
        return;
    }
    for (int y = 0; y < height; y += SWR_TILE_RUN_BLOCK) {
        int h = swrMin(height - y, SWR_TILE_RUN_BLOCK);
        swrRunsDrawOnto(runs, count, pixels + y * pitch, pitch, left, top + y, width, h, under, false);
        swrTintRows(pixels + y * pitch, pitch, width, h, tint);
    }
}

static bool swrTilePictureCanKeep(SWRenderer* swr, int width, int height, int shift)
{
    static int off = -1;
    if (off < 0) off = getenv("BS_NO_TILE_PICTURE_KEEP") != NULL;
    const SWDirtyBuffer* d = swr->dirty;
    // Only a full main-buffer clear under the pictures guarantees that their
    // transparent pixels are determined too. A grid below the pictures can
    // vary independently, while a grid above them is replayed after the copy.
    bool eligible = !off && swr->clearHeld && d != NULL && d->fb == swr->mainFb &&
                    swr->fb == swr->mainFb && swr->fbPitch == width && swr->portX == 0 && swr->portY == 0 &&
                    width > 0 && height > 0 && width == swr->mainWidth && height == swr->mainHeight &&
                    d->width == width && d->height == height && (!swrGridHeld() || swrGrid.above) &&
                    swr->baseMode == 0 && !swr->mirrorReplaying && swr->mirrorLayers == 0 &&
                    swr->mirrorStage == 0 && swr->overlayCount == 0 && swr->pipeCount == 0;
    SWTilePictureView* previous = &swr->tilePictureView;
    if (!eligible) {
        // Pictures drawn later in the frame, over what the clear's pictures
        // left (the dancers room's trees), are only marked dirty over them.
        if (swr->clearHeld || off) previous->width = 0;
        return false;
    }

    // Tracking dirty tiles adds work to each draw over the picture. Wait for
    // a second consecutive frame at the same view before starting it.
    // The raw view and shift determine the effective picture coordinates.
    bool stationary = previous->width == width && previous->height == height &&
                      previous->generation == d->generation && swr->frameCounter - previous->frame == 1 &&
                      previous->x == swr->viewX && previous->y == swr->viewY && previous->shift == shift;
    previous->frame = swr->frameCounter;
    previous->generation = d->generation;
    previous->x = swr->viewX;
    previous->y = swr->viewY;
    previous->width = width;
    previous->height = height;
    previous->shift = shift;
    return stationary;
}

static bool swrTilePictureMatches(const SWTilePictureKey* key, const SWTilePictureKey* prior)
{
    if (key->count != prior->count || key->left != prior->left || key->top != prior->top || key->clear != prior->clear ||
        key->tint != prior->tint)
        return false;
    for (int i = 0; i < key->count; i++)
        if (key->serials[i] != prior->serials[i] || key->x[i] != prior->x[i] || key->y[i] != prior->y[i])
            return false;
    return true;
}

static void swrTilePictureDraw(SWRenderer* swr, SWTileRun* const* runs, int count, int left, int top, uintpixel_t clear, uint32_t tint, uint32_t keyTint)
{
    SWDirtyBuffer* d = swr->dirty;
    int width = d->width, height = d->height;
    SWTilePictureKey key;
    memset(&key, 0, sizeof(key));
    key.left = left;
    key.top = top;
    key.count = swrPendingCount;
    key.clear = clear;
    key.tint = keyTint;
    for (int i = 0; i < swrPendingCount; i++) {
        const SWTileRun* run = swrPendingRuns[i];
        key.serials[i] = run->serial;
        key.x[i] = run->x;
        key.y[i] = run->y;
    }
    const int tileHeight = 1 << SWR_DIRTY_TILE_H_SHIFT;
    int rows = (height + tileHeight - 1) >> SWR_DIRTY_TILE_H_SHIFT;
    uint32_t full = swrDirtyMask(0, width);
    bool keep = swrTilePictureMatches(&key, &d->tilePicture);
    if (keep) {
        // A full write after the previous picture can dirty every tile.
        int r = 0;
        while (r < rows && (d->rows[r] & full) == full) r++;
        keep = r < rows;
    }
    if (keep) {
        for (int r = 0; r < rows; r++) {
            int y = r << SWR_DIRTY_TILE_H_SHIFT, h = swrMin(height - y, tileHeight);
            uint32_t bits = d->rows[r] & full;
            while (bits != 0) {
                int c0 = swrLowestBit(bits), c1 = c0 + 1;
                while (c1 < 32 && (bits & (1u << c1))) c1++;
                bits &= c1 >= 32 ? 0 : ~((1u << c1) - 1);
                int x = c0 << SWR_DIRTY_TILE_W_SHIFT;
                int end = swrMin(width, c1 << SWR_DIRTY_TILE_W_SHIFT);
                swrRunsDrawTinted(runs, count, swr->mainFb + y * width + x, width,
                                  left + x, top + y, end - x, h, &clear, tint);
            }
        }
#ifdef OF_PC
        static int check = -1;
        if (check < 0) check = getenv("BS_CHECK_TILE_PICTURE_KEEP") != NULL;
        if (check) {
            size_t bytes = (size_t) width * height * sizeof(uintpixel_t);
            uintpixel_t* fresh = malloc(bytes);
            require(fresh != NULL);
            swrRunsDrawTinted(runs, count, fresh, width, left, top, width, height, &clear, tint);
            require(memcmp(swr->mainFb, fresh, bytes) == 0);
            free(fresh);
            logInfo("swr: tile picture kept and checked at frame %u, view %d,%d\n", swr->frameCounter, left, top);
        }
#endif
    } else {
        swrRunsDrawTinted(runs, count, swr->mainFb, width, left, top, width, height, &clear, tint);
    }

    // Later draws dirty this picture. The clear-colour and first-sprite
    // shortcuts continue to require their own known uniform base.
    d->tilePicture = key;
    d->known = false;
    d->baseValid = false;
    memset(d->rows, 0, sizeof(d->rows));
    memset(d->baseRows, 0, sizeof(d->baseRows));
    swr->dirtyMark = d;
}

// Marks the blocks of the port that the pictures have anything in.
static void swrRunsMarkDirty(SWRenderer* swr, SWTileRun* const* runs, int count, int left, int top, int width, int height)
{
    for (int i = 0; i < count; i++) {
        const SWTileRun* run = runs[i];
        int x0 = swrMax(left, run->x), y0 = swrMax(top, run->y);
        int x1 = swrMin(left + width, run->x + run->width), y1 = swrMin(top + height, run->y + run->height);
        if (x0 >= x1 || y0 >= y1) continue;
        // Blocks are SWR_TILE_RUN_BLOCK from the picture's corner.
        for (int by = run->y + swrFloorToStep(y0 - run->y, SWR_TILE_RUN_BLOCK); by < y1; by += SWR_TILE_RUN_BLOCK) {
            int bx = run->x + swrFloorToStep(x0 - run->x, SWR_TILE_RUN_BLOCK);
            while (bx < x1) {
                if (swrRunBlockKind(run, bx, by) == SWR_BLOCK_EMPTY) { bx += SWR_TILE_RUN_BLOCK; continue; }
                int start = bx;
                while (bx < x1 && swrRunBlockKind(run, bx, by) != SWR_BLOCK_EMPTY) bx += SWR_TILE_RUN_BLOCK;
                int sx0 = swrMax(start, x0), sx1 = swrMin(bx, x1);
                int sy0 = swrMax(by, y0), sy1 = swrMin(by + SWR_TILE_RUN_BLOCK, y1);
                swrDirtyRect(swr, swr->portX + sx0 - left, swr->portY + sy0 - top, swr->portX + sx1 - left, swr->portY + sy1 - top);
            }
        }
    }
}

static void swrTileRunsDrawPending(SWRenderer* swr)
{
    // The top left of the port in the pictures' units: the view's corner, or
    // at half size the unit that a room position of zero lands a whole number
    // of pixels from (positions are floored, and a picture starts on an even one).
    int shift = swrPendingRuns[0]->shift;
    int32_t viewX = (int32_t) swr->viewX, viewY = (int32_t) swr->viewY;
    int left = shift == 0 ? viewX : (viewX + 1) >> 1, top = shift == 0 ? viewY : (viewY + 1) >> 1;
    uintpixel_t* target = &swr->fb[swr->portY * swr->fbPitch + swr->portX];
    int width = swr->maxX - swr->portX, height = swr->maxY - swr->portY;
#ifdef OF_PC
    // What the port shows of a picture built a stretch at a time was built when the run came in
    // (swrRunEnsure), for the same view: views change only after held pictures are let out.
    for (int held = 0; held < swrPendingCount; held++) {
        int c0, c1;
        swrRunColumnsFor(swrPendingRuns[held], swrPendingRuns[held]->x, left, left + width, SWR_TILE_RUN_BLOCK, &c0, &c1);
        require(swrRunColumnsBuilt(swrPendingRuns[held], c0, c1));
    }
#endif
    // A clear with tiled passes over it, under pictures that cover the whole of it with nothing
    // else held: worked in below them where they are not solid (swrRunsDrawOverHeld).
    if (swr->tiledHeldCount > 0 && swrGrid.count == 0 && swrPendingTint == 0 && swr->fb == swr->mainFb &&
        swr->fbPitch == swr->mainWidth && swr->portX == 0 && swr->portY == 0 && width == swr->mainWidth &&
        height == swr->mainHeight && swr->mainWidth <= 1024 && swrTiledHeldSpanBegin(swr)) {
        swr->tilePictureView.width = 0; // as swrTilePictureCanKeep does for a frame it cannot keep
        SWTileRun* flat;
        SWR_NOTED("  of which: tile pictures flattened", flat = swrPendingCount > 1 ? swrPendingFlattened(swr, 0, left, width) : NULL);
        SWR_NOTE_START(overNote);
        SWTileRun* const one[1] = { flat };
        swrRunsDrawOverHeld(swr, flat != NULL ? one : swrPendingRuns, flat != NULL ? 1 : swrPendingCount, left, top, width, height);
        SWR_NOTE_END(overNote, "  of which: pictures over held backdrop");
        return;
    }
    if (swr->tiledHeldCount > 0) swrClearSettle(swr); // a clear with tiled passes over it is written as it is, not worked in below
    bool canKeep = swrTilePictureCanKeep(swr, width, height, shift);
    if (!canKeep) {
        // Pictures over what is there already write only where they have
        // something; a kept picture under them is then redrawn only there.
        if (!swr->clearHeld && swrGrid.count == 0 && swr->dirtyMark != NULL && swr->fb == swr->mainFb)
            swrRunsMarkDirty(swr, swrPendingRuns, swrPendingCount, left, top, width, height);
        else
            swrDirtyRect(swr, swr->portX, swr->portY, swr->maxX, swr->maxY);
    }

    // A clear still held goes under the pictures if they are drawn over the
    // whole of what it clears; they then write its colour only in their gaps.
    const uintpixel_t* under = NULL;
    if (swr->clearHeld) {
        if (swr->fb == swr->mainFb && swr->fbPitch == swr->mainWidth && swr->portX == 0 && swr->portY == 0 &&
            width == swr->mainWidth && height == swr->mainHeight) {
            under = &swr->clearHeldColor;
            swr->clearHeld = false;
        } else {
            swrClearSettle(swr);
        }
    }
    
    // A grid still held goes between the clear and the pictures: the clear's
    // colour first, where no picture is solid, then the grid's sprites.
    if (swrGrid.count > 0 && !swrGrid.above) {
        if (under != NULL) swrRunsDrawOnto(swrPendingRuns, swrPendingCount, target, swr->fbPitch, left, top, width, height, under, true);
        under = NULL;
        int count = swrPendingCount;
        swrPendingCount = 0; // the sprites flush what is held, and the pictures are not theirs to let out
        swrGridReplay(swr);
        swrPendingCount = count;
    }
    
    // A tint is only taken over a held clear with no grid (SWRenderer_drawRectangle), so it covers everything drawn here.
    uint32_t tint = swrPendingTint, keyTint = tint;
    swrPendingTint = 0;
    if (under == NULL) tint = 0; // (not reached: kept for safety)
    SWTileRun* flat;
    SWR_NOTED("  of which: tile pictures flattened", flat = swrPendingCount > 1 ? swrPendingFlattened(swr, tint, left, width) : NULL);
    SWR_NOTE_START(copyNote);
    SWTileRun* const one[1] = { flat };
    SWTileRun* const* runs = flat != NULL ? one : swrPendingRuns;
    int count = flat != NULL ? 1 : swrPendingCount;
    // A flattened picture has the tint worked in already; its gaps show the clear with the tint over it.
#ifdef SW_HAS_PREMUL_BLEND
    uintpixel_t tintedUnder;
    if (flat != NULL && tint != 0) {
        uint32_t alpha = tint >> 16;
        uintpixel_t color = (uintpixel_t) (tint & 0xffff);
        tintedUnder = swrBlendPremultiplied(*under, swrSpreadRedBlue(color) * alpha, swrGreen(color) * alpha, 256 - alpha);
        under = &tintedUnder;
        tint = 0;
    }
#endif
    if (canKeep)
        swrTilePictureDraw(swr, runs, count, left, top, *under, tint, keyTint);
    else
        swrRunsDrawTinted(runs, count, target, swr->fbPitch, left, top, width, height, under, tint);
    SWR_NOTE_END(copyNote, "  of which: tile pictures copied");
    
    // Grids held over the pictures come last.
    if (swrGrid.count > 0) {
        swrPendingCount = 0; // drawn; the grid's sprites must not let them out again
        swrGridReplay(swr);
    }
}

// Draws the pictures being held, if any. Everything that draws comes through
// swrOverlayFlush first, which calls this, so nothing is drawn under them
// that should have been drawn over.
void swrTileRunsFlush(SWRenderer* swr)
{
    if (swrPendingCount == 0) return;
    if (swr == NULL) swr = swrPendingOwner;
    ++swrTileRunReaders;
    SWR_PROFILED(SWR_PROF_PART, swrTileRunsDrawPending(swr));
    --swrTileRunReaders;
    swrPendingCount = 0;
}

// Forgets the grid and tile pictures still held: the caller is about to paint
// the whole screen. A held clear is the caller's to deal with, since what it
// paints may be worked out from the clear's colour.
void swrHeldUnderDiscard(SWRenderer* swr)
{
    // Tiled passes held over the clear go, and that clear with them: it is no longer one colour to work from.
    if (swr->tiledHeldCount > 0) {
        swr->tiledHeldCount = 0;
        swrFillHeldDrop();
        swr->clearHeld = false;
    }
    swrPendingCount = 0;
    swrGrid.count = 0;
    swrGrid.cells = 0;
}

bool swrTileRunsFree(void)
{
    // Below-grid replay temporarily hides the queue to avoid recursive draws.
    // Its sprite loads may request memory, but the hidden pictures are in use.
    if (swrTileRunReaders != 0) return false;
    swrTileRunsFlush(NULL); // a held picture must be drawn before it goes
    swrGridFlush(NULL);
    bool freed = false;
    for (int e = 0; e < SWR_TILE_RUN_ENTRIES + SWR_TILE_FLAT_ENTRIES; e++) {
        SWTileRun* entry = e < SWR_TILE_RUN_ENTRIES ? &swrTileRuns[e] : &swrFlatRuns[e - SWR_TILE_RUN_ENTRIES];
        if (entry->pixels == NULL) continue;
        free(entry->pixels);
        entry->pixels = NULL;
        freed = true;
    }
    return freed;
}

// Only swrDrawTileRunCached's 16-bit path asks for a run's shape.
#if PIXEL_SIZE == 16
// What a run's tiles say about its picture: the key, the bounding box and the
// first tile's origin, or that the run cannot be drawn from a picture.
typedef struct {
    bool usable;
    uint64_t key;
    int anchorX, anchorY, left, top, right, bottom;
} SWTileRunShape;

static void swrTileRunShapeOf(const int32_t mode[2], RoomTile* const* tiles, const float* offsets, int32_t count, SWTileRunShape* out)
{
    int shift = mode[0];
    memset(out, 0, sizeof(*out));
    uint64_t key = BS_FNV64_OFFSET;
    key = swrTileRunHash(key, mode, 2 * sizeof(int32_t));
    int anchorX = swrFloor((float) tiles[0]->x + offsets[0]);
    int anchorY = swrFloor((float) tiles[0]->y + offsets[1]);
    int left = INT32_MAX, top = INT32_MAX, right = INT32_MIN, bottom = INT32_MIN;
    for (int32_t t = 0; t < count; t++)
    {
        const RoomTile* tile = tiles[t];
        if (tile->scaleX != 1.0f || tile->scaleY != 1.0f || tile->alpha < 1.0f) return;
        
        float exactLeft = (float) tile->x + offsets[t * 2], exactTop = (float) tile->y + offsets[t * 2 + 1];
        int tileLeft = swrFloor(exactLeft);
        int tileTop = swrFloor(exactTop);
        // At half size a tile lands where it would on the screen only from an
        // even position: from an odd one it moves a pixel as the view scrolls.
        if (shift != 0 && ((float) tileLeft != exactLeft || (float) tileTop != exactTop || ((tileLeft | tileTop) & 1) != 0)) return;
        if (tileLeft < left) left = tileLeft;
        if (tileTop < top) top = tileTop;
        if (tileLeft + (int) tile->width > right) right = tileLeft + (int) tile->width;
        if (tileTop + (int) tile->height > bottom) bottom = tileTop + (int) tile->height;
        
        // Everything that decides what the tile looks like, a word at a time:
        // this runs for every tile of the layer on every frame.
        uint32_t words[9] = {
            0, 0, (uint32_t) tile->useSpriteDefinition, (uint32_t) tile->backgroundDefinition,
            (uint32_t) tile->sourceX, (uint32_t) tile->sourceY, (uint32_t) tile->color, tile->width, tile->height,
        };
        float relative[2] = { exactLeft - (float) anchorX, exactTop - (float) anchorY };
        memcpy(words, relative, sizeof(relative));
        for (int w = 0; w < 9; w++) key = (key ^ words[w]) * BS_FNV64_PRIME;
    }
    out->usable = true;
    out->key = key;
    out->anchorX = anchorX; out->anchorY = anchorY;
    out->left = left; out->top = top; out->right = right; out->bottom = bottom;
}

// The shapes of recent runs, for the runs the runner vouches for
// (Renderer.tileRunGeneration): while no room tile has been written, the same
// tiles with the same offsets have the same shape, and comparing the pointers
// and offsets costs much less than going over every tile.
#define SWR_TILE_SHAPE_MEMOS 8
typedef struct {
    uint32_t generation;    // 0: unused
    uint32_t stamp;         // the runner's stamp for these arrays (Renderer.tileRunStamp), or 0
    int32_t mode[2];
    int32_t count, capacity;
    RoomTile** tiles;
    float* offsets;
    uint32_t lastUsed;
    SWTileRunShape shape;
} SWTileShapeMemo;

static SWTileShapeMemo swrTileShapeMemos[SWR_TILE_SHAPE_MEMOS];
static uint32_t swrTileShapeClock;

static void swrTileRunShape(uint32_t generation, uint32_t stamp, const int32_t mode[2], RoomTile** tiles, const float* offsets, int32_t count, SWTileRunShape* out)
{
    static int off = -1;
    if (off < 0) off = getenv("BS_NO_TILE_MEMO") != NULL;
    if (off || generation == 0) { swrTileRunShapeOf(mode, tiles, offsets, count, out); return; }
    ++swrTileShapeClock;
    size_t pointerBytes = (size_t) count * sizeof(RoomTile*), offsetBytes = (size_t) count * 2 * sizeof(float);
    SWTileShapeMemo* spare = &swrTileShapeMemos[0];
    for (int m = 0; m < SWR_TILE_SHAPE_MEMOS; m++) {
        SWTileShapeMemo* memo = &swrTileShapeMemos[m];
        // The same stamp under the same generation is the runner's word that the arrays are the ones compared before.
        if (memo->generation == generation && memo->count == count && memo->mode[0] == mode[0] && memo->mode[1] == mode[1] &&
            ((stamp != 0 && memo->stamp == stamp) ||
             (memo->tiles[0] == tiles[0] && memcmp(memo->tiles, tiles, pointerBytes) == 0 &&
              memcmp(memo->offsets, offsets, offsetBytes) == 0))) {
            memo->stamp = stamp;
            memo->lastUsed = swrTileShapeClock;
            *out = memo->shape;
            static int check = -1;
            if (check < 0) check = getenv("BS_CHECK_TILE_MEMO") != NULL;
            if (check) {
                // Every reuse checked against the tiles themselves.
                static uint32_t hits;
                SWTileRunShape fresh;
                swrTileRunShapeOf(mode, tiles, offsets, count, &fresh);
                require(fresh.usable == out->usable && fresh.key == out->key && fresh.anchorX == out->anchorX &&
                        fresh.anchorY == out->anchorY && fresh.left == out->left && fresh.top == out->top &&
                        fresh.right == out->right && fresh.bottom == out->bottom);
                if (++hits % 300 == 1) logInfo("swr: tile run shape reused %u times, each checked\n", hits);
            }
            return;
        }
        if (memo->generation != generation) { if (spare->generation == generation || memo->lastUsed < spare->lastUsed) spare = memo; }
        else if (spare->generation == generation && memo->lastUsed < spare->lastUsed) spare = memo;
    }
    swrTileRunShapeOf(mode, tiles, offsets, count, out);
    if (spare->capacity < count) {
        RoomTile** grownTiles = (RoomTile**) realloc(spare->tiles, pointerBytes);
        if (grownTiles == NULL) { spare->generation = 0; return; }
        spare->tiles = grownTiles;
        float* grownOffsets = (float*) realloc(spare->offsets, offsetBytes);
        if (grownOffsets == NULL) { spare->generation = 0; spare->capacity = 0; return; }
        spare->offsets = grownOffsets;
        spare->capacity = count;
    }
    memcpy(spare->tiles, tiles, pointerBytes);
    memcpy(spare->offsets, offsets, offsetBytes);
    spare->generation = generation;
    spare->stamp = stamp;
    spare->mode[0] = mode[0]; spare->mode[1] = mode[1];
    spare->count = count;
    spare->lastUsed = swrTileShapeClock;
    spare->shape = *out;
}
#endif

MAYBE_UNUSED static bool swrSmallRunsOff(void)
{
    static int off = -1;
    if (off < 0) off = getenv("BS_NO_SMALL_TILE_RUNS") != NULL;
    return off != 0;
}

MAYBE_UNUSED static bool swrLazyRunsOff(void)
{
    static int off = -1;
    if (off < 0) off = getenv("BS_NO_LAZY_TILE_RUNS") != NULL;
    return off != 0;
}

// Draws the run's tiles into columns c0 to c1 of its picture, placed with its storage's corner at
// (originX, originY) in the room, and marks their blocks. Only the tiles reaching the columns are
// drawn, in their order (for a single column, those listed for it in run->colTiles; for more, all
// are looked at), and a tile crossing their edge only as far as it reaches into them: as a tile of
// the part of its source that lands there, drawn whole (at full size, each pixel is the same texel
// either way). Not by cutting the draw to the port instead: cut, a tile does not always come out as
// it does whole (one crossing the port's left edge lost its pixel there). So each pixel of the
// columns comes out as the whole picture's would.
#if PIXEL_SIZE == 16 // only swrDrawTileRunCached's 16-bit path builds pictures lazily
static void swrRunDrawColumns(Renderer* renderer, SWTileRun* run, RoomTile** tiles, const float* offsets, int32_t count,
                              int originX, int originY, int c0, int c1, int r0, int r1)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    uintpixel_t* savedFb = swr->fb;
    uint16_t savedPitch = swr->fbPitch;
    float savedViewX = swr->viewX, savedViewY = swr->viewY;
    int savedPortX = swr->portX, savedPortY = swr->portY, savedPortW = swr->portW, savedPortH = swr->portH;
    int savedMaxX = swr->maxX, savedMaxY = swr->maxY;
    swr->fb = run->pixels;
    swr->fbPitch = (uint16_t) run->width;
    int y0 = r0 * SWR_TILE_RUN_BLOCK, y1 = swrMin(r1 * SWR_TILE_RUN_BLOCK, run->height);
    swr->viewY = (float) (originY + y0);
    swr->portY = y0; swr->portH = y1 - y0; swr->maxY = y1;
    int x0 = c0 * SWR_TILE_RUN_BLOCK, x1 = swrMin(c1 * SWR_TILE_RUN_BLOCK, run->width);
    swr->viewX = (float) (originX + x0);
    swr->portX = x0; swr->portW = x1 - x0; swr->maxX = x1;
    bool listed = c1 - c0 == 1;
    uint32_t end = listed ? run->colStart[c0 + 1] : (uint32_t) count;
    for (uint32_t k = listed ? run->colStart[c0] : 0; k < end; k++) {
        int32_t t = listed ? run->colTiles[k] : (int32_t) k;
        int x = tiles[t]->x + (int32_t) offsets[t * 2] - originX, w = (int) tiles[t]->width;
        int y = tiles[t]->y + (int32_t) offsets[t * 2 + 1] - originY, h = (int) tiles[t]->height;
        if (x + w <= x0 || x >= x1 || y + h <= y0 || y >= y1) continue;
        if (x >= x0 && x + w <= x1 && y >= y0 && y + h <= y1) {
            Renderer_drawTile(renderer, tiles[t], offsets[t * 2], offsets[t * 2 + 1]);
            continue;
        }
        int from = x < x0 ? x0 - x : 0, to = x + w > x1 ? x1 - x : w;
        int fromY = y < y0 ? y0 - y : 0, toY = y + h > y1 ? y1 - y : h;
        RoomTile part = *tiles[t];
        part.x += from;
        part.sourceX += from;
        part.width = (uint32_t) (to - from);
        part.y += fromY;
        part.sourceY += fromY;
        part.height = (uint32_t) (toY - fromY);
        Renderer_drawTile(renderer, &part, offsets[t * 2], offsets[t * 2 + 1]);
    }
    swrOverlayFlush(swr);
    swr->fb = savedFb;
    swr->fbPitch = savedPitch;
    swr->viewX = savedViewX; swr->viewY = savedViewY;
    swr->portX = savedPortX; swr->portY = savedPortY;
    swr->portW = savedPortW; swr->portH = savedPortH;
    swr->maxX = savedMaxX; swr->maxY = savedMaxY;
    swrRunRowsDrawn(run, c0, c1, r0, r1);
}

// Whether a run can be built a stretch at a time (swrRunDrawColumns): every tile at full size,
// on a whole pixel, in a room drawn at full size, and few enough to be listed by column.
static bool swrRunCanBuildLazily(RoomTile** tiles, const float* offsets, int32_t count)
{
    if (count > 65535) return false;
    for (int32_t t = 0; t < count; t++) {
        if (tiles[t]->scaleX != 1.0f || tiles[t]->scaleY != 1.0f) return false;
        float ox = offsets[t * 2], oy = offsets[t * 2 + 1];
        if (ox != (float) (int32_t) ox || oy != (float) (int32_t) oy || ox > 65536.0f || ox < -65536.0f || oy > 65536.0f || oy < -65536.0f)
            return false;
    }
    return true;
}

// Builds the columns cols[0..n) of a picture built a stretch at a time (those built already are
// passed over), placed at placeX, placeY, from the run's tiles (the same tiles it was built from, by
// its key). Other pictures may be held meanwhile, and a clear with tiled passes: when every texture
// these tiles need is in (nothing is loaded, so nothing is given up), they are set aside while the
// tiles are drawn into the picture, and stay held. Otherwise everything held goes out first, as for
// a picture built whole; or, unless mayFlush, nothing is built and this returns false.
static bool swrRunBuild(Renderer* renderer, SWTileRun* run, RoomTile** tiles, const float* offsets, int32_t count,
                        int placeX, int placeY, const int* cols, int n, bool mayFlush, int aheadRows)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    bool asideOk = !swrGridHeld();
    int32_t lastTpag = -2;
    for (int i = 0; i < n && asideOk; i++) {
        for (uint32_t k = run->colStart[cols[i]]; k < run->colStart[cols[i] + 1] && asideOk; k++) {
            int32_t tpag = Renderer_resolveObjectTPAGIndex(renderer->dataWin, tiles[run->colTiles[k]]);
            if (tpag != lastTpag && !swrTextureForItemIsIn(swr, tpag)) asideOk = false;
            lastTpag = tpag;
        }
    }
    if (!asideOk && !mayFlush) return false;
    int pending = swrPendingCount;
    bool clearHeld = swr->clearHeld, entering = swr->tileRunEntering;
    if (asideOk) {
        swrPendingCount = 0;
        swr->clearHeld = false;
        swr->tileRunEntering = true; // what swrOverlayFlush would let out is set aside or not there
    } else {
        swrTileRunsFlush(swr);
        swrGridFlush(swr);
    }
    ++swrTileRunReaders; // making room for a texture must not give up the picture being drawn into
    int rows = swrRunBlockRows(run);
    for (int i = 0; i < n && run->built != NULL; i++) {
        int c = cols[i], end = c;
        if (run->built[c]) continue;
        if (aheadRows > 0 || c == run->partCol) {
            // A column built ahead a few rows at a time: the next rows, or the rest of them.
            int r0 = c == run->partCol ? run->partRows : 0;
            swrRunDrawColumns(renderer, run, tiles, offsets, count, placeX, placeY, c, c + 1, r0, aheadRows > 0 ? swrMin(r0 + aheadRows, rows) : rows);
            continue;
        }
        // Neighbouring columns go together (one save of the renderer's state).
        while (i + 1 < n && cols[i + 1] == end + 1 && !run->built[end + 1] && end + 1 != run->partCol) { end++; i++; }
        swrRunDrawColumns(renderer, run, tiles, offsets, count, placeX, placeY, c, end + 1, 0, rows);
    }
    --swrTileRunReaders;
    if (asideOk) {
        swr->tileRunEntering = entering;
        swr->clearHeld = clearHeld;
        swrPendingCount = pending;
    }
    return true;
}

// Builds the columns of a lazily built picture that the port is about to show at placeX, placeY
// (and a little more), from the run's tiles.
static void swrRunEnsure(Renderer* renderer, SWTileRun* run, RoomTile** tiles, const float* offsets, int32_t count, int placeX, int placeY)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    int c0, c1;
    int32_t viewLeft = (int32_t) swr->viewX;
    swrRunColumnsFor(run, placeX, viewLeft, viewLeft + (swr->maxX - swr->portX), 3 * SWR_TILE_RUN_BLOCK, &c0, &c1);
    if (swrRunColumnsBuilt(run, c0, c1)) return;
    SWR_NOTE_START(buildNote);
    int cols[SWR_TILE_RUN_MAX_SIDE / SWR_TILE_RUN_BLOCK], n = 0;
    for (int c = c0; c < c1; c++)
        if (!run->built[c]) cols[n++] = c;
    swrRunBuild(renderer, run, tiles, offsets, count, placeX, placeY, cols, n, true, 0);
    SWR_NOTE_END(buildNote, "  of which: tile picture columns built");
}

// The next column of a picture built a stretch at a time to build ahead of a view showing columns
// c0 to c1: the nearest not yet built on the side the view last moved to, else on the other; -1
// when every column is built.
static int swrRunAheadColumn(const SWTileRun* run, int c0, int c1)
{
    if (run->partCol >= 0) return run->partCol; // one begun is finished first
    int blocksX = (run->width + SWR_TILE_RUN_BLOCK - 1) / SWR_TILE_RUN_BLOCK;
    int right = c1, left = c0 - 1;
    while (right < blocksX && run->built[right]) right++;
    while (left >= 0 && run->built[left]) left--;
    if (right >= blocksX) right = -1;
    if (run->lastStep < 0) return left >= 0 ? left : right;
    return right >= 0 ? right : left;
}

// While the view stays put (or moves, near the port's edge: SWR_AHEAD_NEAR_COLUMNS), builds a little
// more of a picture built a stretch at a time ahead of it (swrRunAheadColumn), as many rows of a column as the units swrAheadTake gives it allow, so that a
// walk later finds them built. Only when nothing held has to go
// out for it, and with every texture the tiles need in already (they were all brought in when the
// picture was created, so this reads nothing from the card).
static void swrRunAhead(Renderer* renderer, SWTileRun* run, RoomTile** tiles, const float* offsets, int32_t count, int placeX, int placeY, bool still)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    int c0, c1;
    int32_t viewLeft = (int32_t) swr->viewX;
    swrRunColumnsFor(run, placeX, viewLeft, viewLeft + (swr->maxX - swr->portX), 0, &c0, &c1);
    int c = swrRunAheadColumn(run, c0, c1), cost = 0;
    if (!still && (c < c0 - SWR_AHEAD_NEAR_COLUMNS || c >= c1 + SWR_AHEAD_NEAR_COLUMNS)) return;
    int units = swrAheadTake(swr, run);
    if (units == 0) return;
    SWR_NOTE_START(aheadNote);
    if (c >= 0) {
        // As many rows as the units allow, at least one: a unit for each block, for each tile reaching
        // the rows, and for each 256 pixels those tiles draw there (rt-os20: about 3600, 4800 and
        // 4000 cycles).
        int r0 = c == run->partCol ? run->partRows : 0, r1 = r0, rows = swrRunBlockRows(run);
        int cx0 = c * SWR_TILE_RUN_BLOCK, cx1 = cx0 + SWR_TILE_RUN_BLOCK;
        while (r1 < rows) {
            int y0 = r0 * SWR_TILE_RUN_BLOCK, y1 = (r1 + 1) * SWR_TILE_RUN_BLOCK, reaching = 0, area = 0;
            for (uint32_t k = run->colStart[c]; k < run->colStart[c + 1]; k++) {
                int32_t t = run->colTiles[k];
                int y = tiles[t]->y + (int32_t) offsets[t * 2 + 1] - placeY, h = (int) tiles[t]->height;
                if (y >= y1 || y + h <= y0) continue;
                int x = tiles[t]->x + (int32_t) offsets[t * 2] - placeX, w = (int) tiles[t]->width;
                reaching++;
                area += (swrMin(x + w, cx1) - swrMax(x, cx0)) * (swrMin(y + h, y1) - swrMax(y, y0));
            }
            int next = r1 + 1 - r0 + reaching + (area + 255) / 256;
            if (r1 > r0 && next > units) { cost = units; break; } // the rest waits for the next frame
            cost = next;
            r1++;
        }
        if (!swrRunBuild(renderer, run, tiles, offsets, count, placeX, placeY, &c, 1, false, r1 - r0)) cost = 0;
    }
    swrAheadSpent(run, swrMin(cost, units), units);
    SWR_NOTE_END(aheadNote, "  of which: tile picture columns built ahead");
}
#endif

// Draws the run from its cached picture, building it first if need be. Returns false if the run cannot be drawn that way.
static bool swrDrawTileRunCached(Renderer* renderer, RoomTile** tiles, const float* offsets, int32_t count)
{
    SWRenderer* swr = (SWRenderer*) renderer;
#if PIXEL_SIZE != 16
    (void) swr; (void) tiles; (void) offsets; (void) count;
    return false;
#else
    // The picture holds no partial coverage, so it can only stand in for
    // tiles drawn opaque and with normal blending, and at the room's own
    // size or at half of it.
    bool small = count < SWR_TILE_RUN_MIN_TILES;
    if (small && (count < SWR_TILE_RUN_MIN_TILES_HELD || swrPendingCount == 0 || swrSmallRunsOff())) return false;
    if (swr->drawingToSurface || swr->blendMode != bm_normal) return false;
    int shift;
    if (swr->scaleX == 1.0f && swr->scaleY == 1.0f) shift = 0;
    else if (swr->scaleX == 0.5f && swr->scaleY == 0.5f) shift = 1;
    else return false;
    
    int32_t mode[2] = { shift, swrFavorSpeed ? 1 : 0 };
    SWTileRunShape shape;
    SWR_NOTE_START(keyNote);
    swrTileRunShape(renderer->tileRunGeneration, renderer->tileRunGeneration != 0 ? renderer->tileRunStamp : 0, mode, tiles, offsets, count, &shape);
    SWR_NOTE_END(keyNote, "  of which: tile run key");
    if (!shape.usable) return false;
    uint64_t key = shape.key;
    int anchorX = shape.anchorX, anchorY = shape.anchorY;
    int left = shape.left, top = shape.top, right = shape.right, bottom = shape.bottom;
    
    // The picture starts on a block boundary, so that the pictures of a room share one grid of blocks.
    left = swrFloorToStep(left, SWR_TILE_RUN_BLOCK << shift);
    top = swrFloorToStep(top, SWR_TILE_RUN_BLOCK << shift);
    int width = (right - left + shift) >> shift, height = (bottom - top + shift) >> shift;
    if (width <= 0 || height <= 0 || width > SWR_TILE_RUN_MAX_SIDE || height > SWR_TILE_RUN_MAX_SIDE) return false;
    if (width * height > SWR_TILE_RUN_MAX_PIXELS_BIG) return false;
    
    // Find the picture, or the entry to build it in: a free one, else the one unused for longest.
    SWTileRun* run = NULL;
    SWTileRun* spare = &swrTileRuns[0];
    for (int e = 0; e < SWR_TILE_RUN_ENTRIES; e++)
    {
        SWTileRun* entry = &swrTileRuns[e];
        if (entry->pixels != NULL && swr->frameCounter - entry->lastUsedFrame > SWR_TILE_RUN_IDLE_FRAMES) {
            free(entry->pixels);
            entry->pixels = NULL;
        }
        if (entry->pixels != NULL && entry->key == key) { run = entry; break; }
        if (entry->pixels == NULL) {
            if (spare->pixels != NULL) spare = entry;
        } else if (spare->pixels != NULL && entry->lastUsedFrame < spare->lastUsedFrame) {
            spare = entry;
        }
    }
    for (int f = 0; f < SWR_TILE_FLAT_ENTRIES; f++) {
        SWTileRun* entry = &swrFlatRuns[f];
        if (entry->pixels != NULL && swr->frameCounter - entry->lastUsedFrame > SWR_TILE_RUN_IDLE_FRAMES) {
            free(entry->pixels);
            entry->pixels = NULL;
        }
    }
    
    if (run == NULL)
    {
        // An entry used this frame belongs to another run of the room being drawn; leave it be.
        if (spare->pixels != NULL && spare->lastUsedFrame == swr->frameCounter) return false;
        if (small && spare->pixels != NULL) return false;
        
        // Pictures being held belong on the screen, and would come out into the
        // new one once the tiles below start drawing: let them out first. That
        // also has to happen before one of them is given up for it.
        swrTileRunsFlush(swr);
        swrGridFlush(swr);
        free(spare->pixels);
        spare->pixels = NULL;
        // A picture much wider than the port is built a stretch at a time as the view comes to it
        // (swrRunEnsure, below): Snowdin Town's two 2720-pixel pictures took 400 ms simulated whole.
        bool lazy = shift == 0 && width > 2 * (swr->maxX - swr->portX) + 4 * SWR_TILE_RUN_BLOCK && !swrLazyRunsOff() &&
                    swrRunCanBuildLazily(tiles, offsets, count);
        if (!swrRunFits(width, height) ||
            !(lazy ? swrRunAllocateIndexed(spare, width, height, tiles, offsets, count, left >> shift)
                   : swrRunAllocate(spare, width, height, false))) return false;
        run = spare;
        run->key = key;
        run->bornFrame = swr->frameCounter;
        run->x = left >> shift; run->y = top >> shift;
        run->anchorX = anchorX >> shift; run->anchorY = anchorY >> shift;
        run->shift = shift;
        if (lazy) {
            // Its textures are all brought in now, as building it whole would: only the drawing
            // waits for the view, and reading the card does not move into the walk.
            // (The picture is in use meanwhile: making room for a texture must not give it up.)
            int32_t lastTpag = -2;
            ++swrTileRunReaders;
            for (int32_t t = 0; t < count; t++) {
                int32_t tpag = Renderer_resolveObjectTPAGIndex(renderer->dataWin, tiles[t]);
                if (tpag != lastTpag && tpag >= 0 && (uint32_t) tpag < renderer->dataWin->tpag.count) {
                    const TexturePageItem* item = &renderer->dataWin->tpag.items[tpag];
                    if (item->sourceWidth != item->targetWidth || item->sourceHeight != item->targetHeight) {
                        if (item->texturePageId >= 0) swrEnsureTextureIsLoaded(swr, (uint32_t) item->texturePageId);
                    } else {
                        swrTextureForItem(swr, tpag);
                    }
                }
                lastTpag = tpag;
            }
            --swrTileRunReaders;
        } else {
            // Point the renderer at the picture, with a view that maps the run's
            // bounding box onto it, and draw the tiles the ordinary way.
            SWR_NOTE_START(buildNote);
            SWRenderer saved = *swr;
            swr->fb = run->pixels;
            swr->fbPitch = (uint16_t) width;
            swr->viewX = left; swr->viewY = top;
            swr->portX = 0; swr->portY = 0;
            swr->portW = width; swr->portH = height;
            swr->maxX = width; swr->maxY = height;
            for (int32_t t = 0; t < count; t++)
                Renderer_drawTile(renderer, tiles[t], offsets[t * 2], offsets[t * 2 + 1]);
            swrOverlayFlush(swr);
            swr->fb = saved.fb;
            swr->fbPitch = saved.fbPitch;
            swr->viewX = saved.viewX; swr->viewY = saved.viewY;
            swr->portX = saved.portX; swr->portY = saved.portY;
            swr->portW = saved.portW; swr->portH = saved.portH;
            swr->maxX = saved.maxX; swr->maxY = saved.maxY;
        
            swrRunClassify(run);
            SWR_NOTE_END(buildNote, "  of which: tile picture built");
        }
    }
    // What the port shows of a picture built a stretch at a time, built if it is not yet; and a
    // little more ahead of it (swrRunAhead).
    if (run->built != NULL) {
        int placeX = run->x + (anchorX >> shift) - run->anchorX, placeY = run->y + (anchorY >> shift) - run->anchorY;
        int32_t viewLeft = (int32_t) swr->viewX, viewTop = (int32_t) swr->viewY;
        bool followed = run->viewFrame + 1 == swr->frameCounter;
        bool still = followed && run->viewLeft == viewLeft && run->viewTop == viewTop;
        if (followed && run->viewLeft != viewLeft) run->lastStep = viewLeft > run->viewLeft ? 1 : -1;
        run->viewFrame = swr->frameCounter;
        run->viewLeft = viewLeft; run->viewTop = viewTop;
        swrRunEnsure(renderer, run, tiles, offsets, count, placeX, placeY);
        if (followed && run->built != NULL && swr->frameCounter - run->bornFrame > SWR_AHEAD_SETTLE_FRAMES)
            swrRunAhead(renderer, run, tiles, offsets, count, placeX, placeY, still);
    }
    run->lastUsedFrame = swr->frameCounter;
    
    // Held, to be drawn together with the pictures that follow it directly.
    if (swrPendingCount == SWR_TILE_RUN_ENTRIES || (swrPendingCount > 0 && swrPendingRuns[0]->shift != shift)) swrTileRunsFlush(swr);
    // One image may be queued more than once, at different positions. Its
    // cache entry and each earlier placement must remain unchanged.
    SWTileRun* placed = &swrPendingPlacements[swrPendingCount];
    *placed = *run;
    placed->x += (anchorX >> shift) - run->anchorX;
    placed->y += (anchorY >> shift) - run->anchorY;
    placed->anchorX = anchorX >> shift; placed->anchorY = anchorY >> shift;
    swrPendingRuns[swrPendingCount++] = placed;
    swrPendingOwner = swr;
    return true;
#endif
}

// A run that cannot be drawn from a picture (too big, scaled, translucent, a
// few tiles only) is drawn tile by tile, leaving out the tiles that cannot
// reach the port: a room's tiles are mostly somewhere else, and each one
// costs a whole sprite draw to find that out further down.
static bool SWRenderer_drawTileRun(Renderer* renderer, RoomTile** tiles, const float* offsets, int32_t count)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return true;
    SWRenderer* swr = (SWRenderer*) renderer;
    // Whatever else is held goes first; pictures already held stay, for this
    // run to join, unless a grid is held over them.
    if (swrGridHeld() && swrGrid.above) swrOverlayFlush(swr);
    swr->tileRunEntering = true;
    swrOverlayFlush(swr);
    swr->tileRunEntering = false;
    if (swrDrawTileRunCached(renderer, tiles, offsets, count)) return true;
    swrTileRunsFlush(swr);
    swrGridFlush(swr);
    swrClearSettle(swr);
    
    for (int32_t t = 0; t < count; t++)
    {
        const RoomTile* tile = tiles[t];
        // Where the tile lands in the buffer, whichever way its scale points, with a pixel to spare.
        float x = ((float) tile->x + offsets[t * 2] - (float) swr->viewX) * swr->scaleX + (float) swr->portX;
        float y = ((float) tile->y + offsets[t * 2 + 1] - (float) swr->viewY) * swr->scaleY + (float) swr->portY;
        float w = (float) tile->width * tile->scaleX * swr->scaleX, h = (float) tile->height * tile->scaleY * swr->scaleY;
        float left = w < 0.0f ? x + w : x, right = w < 0.0f ? x : x + w;
        float top = h < 0.0f ? y + h : y, bottom = h < 0.0f ? y : y + h;
        if (right < (float) swr->portX - 1.0f || left > (float) swr->maxX + 1.0f) continue;
        if (bottom < (float) swr->portY - 1.0f || top > (float) swr->maxY + 1.0f) continue;
        Renderer_drawTile(renderer, tiles[t], offsets[t * 2], offsets[t * 2 + 1]);
    }
    return true;
}

// ===[ Tile layers ]===
// A GMS2 room is built from tile layers: a grid of cells, each naming a tile
// of one tileset. Drawn cell by cell through drawSpritePart, a layer costs a
// sprite draw for every cell of the room on every frame, in view or not.
// Here only the cells in view are drawn, and a cell that is neither mirrored,
// flipped nor rotated is copied straight from the tileset: a row at a time
// when the tile has no transparent pixels, not at all when it has nothing
// but. What comes out is what the cell-by-cell draws would have produced.
#define SWR_TILESETS 8
enum { SWR_TILE_UNKNOWN, SWR_TILE_EMPTY, SWR_TILE_SOLID, SWR_TILE_MIXED };

typedef struct {
    int32_t tpagIndex;
    uint32_t count;
    uint8_t* kinds;     // one SWR_TILE_* per tile index; NULL when the entry is free
    uint32_t lastUsedFrame;
} SWTileset;

static SWTileset swrTilesets[SWR_TILESETS];
bool swrTileLayerFast = true;

MAYBE_UNUSED static uint8_t* swrTileKinds(SWRenderer* swr, int32_t tpagIndex, uint32_t count)
{
    SWTileset* spare = &swrTilesets[0];
    for (int e = 0; e < SWR_TILESETS; e++) {
        SWTileset* entry = &swrTilesets[e];
        if (entry->kinds != NULL && entry->tpagIndex == tpagIndex && entry->count == count) {
            entry->lastUsedFrame = swr->frameCounter;
            return entry->kinds;
        }
        if (entry->kinds == NULL) {
            if (spare->kinds != NULL) spare = entry;
        } else if (spare->kinds != NULL && entry->lastUsedFrame < spare->lastUsedFrame)
            spare = entry;
    }
    uint8_t* kinds = (uint8_t*) calloc((size_t) count + 1, 1);
    if (kinds == NULL) return NULL;
    free(spare->kinds);
    spare->kinds = kinds;
    spare->tpagIndex = tpagIndex;
    spare->count = count;
    spare->lastUsedFrame = swr->frameCounter;
    return kinds;
}

static bool SWRenderer_drawTileLayer(Renderer* renderer, Background* tileset, const uint32_t* cells,
                                     uint32_t tilesX, uint32_t tilesY, float offsetX, float offsetY)
{
    if (SWR_SKIPPED((SWRenderer*) renderer)) return true;
    SWRenderer* swr = (SWRenderer*) renderer;
#if PIXEL_SIZE != 16
    (void) swr; (void) tileset; (void) cells; (void) tilesX; (void) tilesY; (void) offsetX; (void) offsetY;
    return false;
#else
    DataWin* dwin = renderer->dataWin;
    int32_t tpagIndex = tileset->tpagIndex;
    if (!swrTileLayerFast || tpagIndex < 0 || (uint32_t) tpagIndex >= dwin->tpag.count) return false;
    if (swr->scaleX <= 0.0f || swr->scaleY <= 0.0f || tilesX == 0 || tilesY == 0) return false;
    swrOverlayFlush(swr);
    
    int tileW = (int) tileset->gms2TileWidth, tileH = (int) tileset->gms2TileHeight;
    int borderX = (int) tileset->gms2OutputBorderX, borderY = (int) tileset->gms2OutputBorderY;
    uint32_t columns = tileset->gms2TileColumns;
    
    // The cells that can reach the port, with one to spare each way for a rotated tile.
    float viewLeft = (float) swr->viewX - offsetX, viewTop = (float) swr->viewY - offsetY;
    float viewRight = viewLeft + (float) swr->portW / swr->scaleX, viewBottom = viewTop + (float) swr->portH / swr->scaleY;
    int firstX = swrFloor(viewLeft / (float) tileW) - 1, lastX = swrFloor(viewRight / (float) tileW) + 1;
    int firstY = swrFloor(viewTop / (float) tileH) - 1, lastY = swrFloor(viewBottom / (float) tileH) + 1;
    if (firstX < 0) firstX = 0;
    if (firstY < 0) firstY = 0;
    if (lastX > (int) tilesX - 1) lastX = (int) tilesX - 1;
    if (lastY > (int) tilesY - 1) lastY = (int) tilesY - 1;
    
    // A copy stands in for a draw only at full size, with normal blending, of an item stored at the size it is drawn.
    TexturePageItem* tpag = &dwin->tpag.items[tpagIndex];
    bool direct = swr->scaleX == 1.0f && swr->scaleY == 1.0f && swr->blendMode == bm_normal &&
                  tpag->sourceWidth == tpag->targetWidth && tpag->sourceHeight == tpag->targetHeight;
    uint8_t* kinds = direct ? swrTileKinds(swr, tpagIndex, tileset->gms2TileCount) : NULL;
    SWTexture* texture = kinds != NULL ? swrTextureForItem(swr, tpagIndex) : NULL;
    
    for (int ty = firstY; ty <= lastY; ty++)
    {
        for (int tx = firstX; tx <= lastX; tx++)
        {
            uint32_t cell = cells[(uint32_t) ty * tilesX + (uint32_t) tx];
            uint32_t tileIndex = cell & 0x0007FFFF;
            if (tileIndex == 0 || tileIndex > tileset->gms2TileCount) continue;
            
            int srcX = (int) (tileIndex % columns) * (tileW + 2 * borderX) + borderX;
            int srcY = (int) (tileIndex / columns) * (tileH + 2 * borderY) + borderY;
            bool mirror = (cell & 0x10000000) != 0, flip = (cell & 0x20000000) != 0, rotate = (cell & 0x40000000) != 0;
            
            if (texture != NULL && !mirror && !flip && !rotate)
            {
                int sx = tpag->sourceX + srcX - texture->originX;
                int sy = tpag->sourceY + srcY - texture->originY;
                if (sx >= 0 && sy >= 0 && sx + tileW <= texture->width && sy + tileH <= texture->height)
                {
                    uint8_t kind = kinds[tileIndex];
                    if (kind == SWR_TILE_UNKNOWN)
                    {
                        int opaque = 0;
                        for (int y = 0; y < tileH; y++) {
                            const uintpixel_t* src = &texture->buffer[(sy + y) * texture->width + sx];
                            for (int x = 0; x < tileW; x++) opaque += swrIsOpaque(src[x]) ? 1 : 0;
                        }
                        kind = opaque == 0 ? SWR_TILE_EMPTY : opaque == tileW * tileH ? SWR_TILE_SOLID : SWR_TILE_MIXED;
                        kinds[tileIndex] = kind;
                    }
                    if (kind == SWR_TILE_EMPTY) continue;
                    
                    float fx = (float) (tx * tileW) + offsetX, fy = (float) (ty * tileH) + offsetY;
                    swrTransformPosIfNeeded(swr, &fx, &fy);
                    int dx = swrFloor(fx), dy = swrFloor(fy);
                    int x0 = dx < swr->portX ? swr->portX : dx, x1 = dx + tileW > swr->maxX ? swr->maxX : dx + tileW;
                    int y0 = dy < swr->portY ? swr->portY : dy, y1 = dy + tileH > swr->maxY ? swr->maxY : dy + tileH;
                    if (x0 >= x1 || y0 >= y1) continue;
                    
                    int width = x1 - x0;
                    for (int y = y0; y < y1; y++)
                    {
                        const uintpixel_t* src = &texture->buffer[(sy + (y - dy)) * texture->width + sx + (x0 - dx)];
                        uintpixel_t* dst = &swr->fb[y * swr->fbPitch + x0];
                        swrDirtyRow(swr, y, x0, x1);
                        if (kind == SWR_TILE_SOLID)
                            memcpy(dst, src, (size_t) width * sizeof(uintpixel_t));
                        else for (int x = 0; x < width; x++) {
                            uintpixel_t pixel = src[x];
                            if (swrIsOpaque(pixel)) dst[x] = pixel;
                        }
                    }
                    continue;
                }
            }
            
            // Anything else is drawn the way the runner would have drawn it.
            float angleDeg = rotate ? 90.0f : 0.0f;
            float pivotX = (float) (tx * tileW) + offsetX + (float) tileW / 2.0f;
            float pivotY = (float) (ty * tileH) + offsetY + (float) tileH / 2.0f;
            float dstX = (float) (tx * tileW) + offsetX + (mirror ? (float) tileW : 0.0f);
            float dstY = (float) (ty * tileH) + offsetY + (flip ? (float) tileH : 0.0f);
            SWRenderer_drawSpritePart(renderer, tpagIndex, (float) srcX, (float) srcY, (float) tileW, (float) tileH, dstX, dstY,
                                      mirror ? -1.0f : 1.0f, flip ? -1.0f : 1.0f, angleDeg, pivotX, pivotY, 0xFFFFFF, 1.0f);
            if (texture != NULL) texture = swrTextureForItem(swr, tpagIndex); // the draw may have moved it
        }
    }
    return true;
#endif
}

// See sw_call_notes.h. Leaves the pixels it drew in the buffer.
void swrSpriteCostProbe(Renderer* renderer, int calls, int32_t* width, int32_t* height, uint32_t nanos[SWR_PROBE_STAGES], uint32_t* outside, int* path)
{
    *width = *height = 0;
    *outside = 0;
    *path = 0;
    for (int s = 0; s < SWR_PROBE_STAGES; s++) nanos[s] = 0;
#ifdef SW_DRAW_PROFILE
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (swr->fb == NULL || calls <= 0) return;
    int32_t sprite = -1;
    if (swrProbeGridSprite >= 0 && (uint32_t) swrProbeGridSprite < dw->sprt.count && Renderer_resolveTPAGIndex(dw, swrProbeGridSprite, 0) >= 0)
        sprite = swrProbeGridSprite;
    bool fromGrid = sprite >= 0;
    for (uint32_t s = 0; !fromGrid && s < dw->sprt.count; s++) {
        const Sprite* candidate = &dw->sprt.sprites[s];
        if (candidate->textureCount == 0 || Renderer_resolveTPAGIndex(dw, (int32_t) s, 0) < 0) continue;
        if (sprite < 0) sprite = (int32_t) s;
        if (candidate->width == 40 && candidate->height == 40) { sprite = (int32_t) s; break; }
    }
    if (sprite < 0) return;
    *width = (int32_t) dw->sprt.sprites[sprite].width;
    *height = (int32_t) dw->sprt.sprites[sprite].height;
    
    bool skipped = swrSkipFrame, timed = swrDrawTimed, noted = swrCallNotes;
    swrSkipFrame = false; swrDrawTimed = false; swrCallNotes = false;
    swrProbing = true;
    swrOverlayFlush(swr);
    float x = (float) swr->viewX + 60.0f, y = (float) swr->viewY + 60.0f;
    swrProbePath = 0;
    Renderer_drawSprite(renderer, sprite, 0, x, y); // its texture loaded, its copies made
    *path = swrProbePath;
    for (int stage = SWR_PROBE_STAGES - 1; stage >= 0; stage--) {
        swrProbeStop = stage;
        uint64_t start = nowNanos();
        for (int i = 0; i < calls; i++) Renderer_drawSprite(renderer, sprite, 0, x, y);
        nanos[stage] = (uint32_t) ((nowNanos() - start) / (uint64_t) calls);
    }
    swrProbeStop = 0;
    uint64_t start = nowNanos();
    for (int i = 0; i < calls; i++) Renderer_drawSprite(renderer, sprite, 0, (float) swr->viewX - 10000.0f, y);
    *outside = (uint32_t) ((nowNanos() - start) / (uint64_t) calls);
    swrOverlayFlush(swr);
    swrProbing = false;
    swrSkipFrame = skipped; swrDrawTimed = timed; swrCallNotes = noted;
#else
    (void) renderer; (void) calls;
#endif
}

#ifdef SW_DRAW_PROFILE
// Times each kind of draw call and hands the totals to the platform, which
// can then say what a slow frame was spent drawing.

static void SWRenderer_profDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha)
{
    const TexturePageItem* tpag = NULL;
    if (swrCallNotes && swrDrawTimed && tpagIndex >= 0 && (uint32_t) tpagIndex < renderer->dataWin->tpag.count)
        tpag = &renderer->dataWin->tpag.items[tpagIndex];
    bool pipe = tpag != NULL && tpag->sourceWidth == SWR_PIPE_RASTER_W &&
                tpag->sourceHeight == SWR_PIPE_RASTER_H && !SWR_SKIPPED((SWRenderer*) renderer) &&
                swrMustRotate(angleDeg);
    if (pipe) swrLastPipeRasterPath = SWR_RASTER_OTHER;
    SWR_PROFILED(SWR_PROF_SPRITE, SWRenderer_drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha));
    if (tpag != NULL) {
        if (pipe) {
            swrPipeRasterNotes.calls[swrLastPipeRasterPath]++;
            swrPipeRasterNotes.nanos[swrLastPipeRasterPath] += swrProfLast;
        }
        SWNoteKey key = { .kind = SWR_NOTE_SPRITE, .alpha = (int16_t) (alpha * 100.0f), .a = tpag->targetWidth, .b = tpag->targetHeight,
                          .c = (int32_t) (xscale * 10.0f),
                          .flags = (uint8_t) ((angleDeg != 0.0f ? SWR_NOTE_TURNED : 0) | ((color & 0xFFFFFF) != 0xFFFFFF ? SWR_NOTE_TINTED : 0)) };
        swrNoteKeyed(swrProfLast, &key);
    }
}

static void SWRenderer_profDrawSpritePart(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha)
{
    SWR_PROFILED(SWR_PROF_PART, SWRenderer_drawSpritePart(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, alpha));
}

static void SWRenderer_profDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation)
{
    SWR_PROFILED(SWR_PROF_TEXT, SWRenderer_drawText(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation));
}

static void SWRenderer_profDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation)
{
    SWR_PROFILED(SWR_PROF_TEXT, SWRenderer_drawTextColor(renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation));
}

static void SWRenderer_profDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha)
{
    SWR_PROFILED(SWR_PROF_TILED, SWRenderer_drawSpriteTiled(renderer, tpagIndex, originX, originY, x, y, xscale, yscale, tileX, tileY, roomW, roomH, color, alpha));
    if (swrCallNotes && swrDrawTimed) {
        SWNoteKey key = { .kind = SWR_NOTE_TILED, .alpha = (int16_t) (alpha * 100.0f), .a = tpagIndex };
        swrNoteKeyed(swrProfLast, &key);
    }
}

static bool SWRenderer_profDrawTileRun(Renderer* renderer, RoomTile** tiles, const float* offsets, int32_t count)
{
    bool drawn;
    SWR_PROFILED(SWR_PROF_PART, drawn = SWRenderer_drawTileRun(renderer, tiles, offsets, count));
    return drawn;
}

static bool SWRenderer_profDrawTileLayer(Renderer* renderer, Background* tileset, const uint32_t* cells, uint32_t tilesX, uint32_t tilesY, float offsetX, float offsetY)
{
    bool drawn;
    SWR_PROFILED(SWR_PROF_PART, drawn = SWRenderer_drawTileLayer(renderer, tileset, cells, tilesX, tilesY, offsetX, offsetY));
    return drawn;
}

static void SWRenderer_profDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline)
{
    SWR_PROFILED(SWR_PROF_RECT, SWRenderer_drawRectangle(renderer, x1, y1, x2, y2, color, alpha, outline));
    if (swrCallNotes && swrDrawTimed) {
        // Where it is on the screen, not in the room, so that a scrolling view does not make each frame's a new one.
        const SWRenderer* swr = (const SWRenderer*) renderer;
        SWNoteKey key = { .kind = SWR_NOTE_RECT, .alpha = (int16_t) (alpha * 100.0f), .a = (int) x1 - swr->viewX, .b = (int) y1 - swr->viewY,
                          .c = (int) (x2 - x1), .d = (int) (y2 - y1), .color = color & 0xFFFFFF, .flags = (uint8_t) (outline ? SWR_NOTE_OUTLINE : 0) };
        swrNoteKeyed(swrProfLast, &key);
    }
}
#endif


static bool swrAssetNamed(const char* name, const char* const* names, size_t count)
{
    if (name == NULL) return false;
    for (size_t i = 0; i < count; ++i)
        if (strcmp(name, names[i]) == 0) return true;
    return false;
}

static bool swrFloweyStalkFrames(const DataWin* dw, const Sprite* sprite)
{
    if (sprite->textureCount != SWR_FLOWEY_STALK_FRAMES) return false;
    int32_t previous = -1;
    for (uint32_t frame = 0; frame < sprite->textureCount; ++frame) {
        int32_t item = sprite->tpagIndices[frame];
        if (item <= previous || (uint32_t) item >= dw->tpag.count) return false;
        const TexturePageItem* tpag = &dw->tpag.items[item];
        if (tpag->sourceWidth != 360 || tpag->targetWidth != 360 ||
            tpag->sourceHeight < 150 || tpag->sourceHeight > 190 || tpag->targetHeight != tpag->sourceHeight)
            return false;
        previous = item;
    }
    return true;
}

void SWRenderer_setFloweySpeedAssets(Renderer* renderer)
{
    static const char* const backdrop[] = {
        "spr_vines_flowey_3", "spr_vines_flowey_2", "spr_vines_flowey",
        "spr_floweyx_behind" /* SWR_FLOWEY_BACKDROP_SHADOW */
    };
    /* Keep the FLOWEY_* order above; old C++ compilers lack indexed initializers. */
    static const char* const trim[] = {
        "spr_floweyx_mouthedge",
        "spr_floweyx_mouthl",
        "spr_flipeye_holder",
        "spr_flipeye_bg",
        "spr_lefteye_overlay"
    };
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    swr->floweyVineItem = -1;
    swr->floweyPipeItem = -1;
    swr->floweyPipeSegmentItem = -1;
    swr->floweyStalkSprite = -1;
    swr->floweyArmItem = -1;
    swr->floweyTvCaseItem = -1;
    swr->floweyTvScreenSprite = -1;
    swr->floweyOuterEyeShadow = -1;
    for (unsigned trimSlot = 0; trimSlot < sizeof(trim) / sizeof(trim[0]); ++trimSlot)
        swr->floweyTrimItems[trimSlot] = -1;
    swr->floweyBackdropCount = 0;
    for (unsigned backdropSlot = 0; backdropSlot < sizeof(backdrop) / sizeof(backdrop[0]); ++backdropSlot)
        swr->floweyBackdropItems[backdropSlot] = -1;
    if (dw->gen8.wadVersion != 16 || dw->gen8.gameID != 864738521u) return;
    for (uint32_t spriteIndex = 0; spriteIndex < dw->sprt.count; ++spriteIndex) {
        const Sprite* sprite = &dw->sprt.sprites[spriteIndex];
        if (!sprite->name) continue;
        if (strcmp(sprite->name, "spr_sidestalk") == 0 && swrFloweyStalkFrames(dw, sprite)) {
            swr->floweyStalkSprite = (int32_t) spriteIndex;
            swr->floweyStalkFirst = sprite->tpagIndices[0];
            swr->floweyStalkLast = sprite->tpagIndices[sprite->textureCount - 1];
        }
        if (strcmp(sprite->name, "spr_tvinside_old") == 0 && sprite->textureCount == 37)
            swr->floweyTvScreenSprite = (int32_t) spriteIndex;
        if (sprite->textureCount != 1) continue;
        int32_t item = sprite->tpagIndices[0];
        if (item < 0 || (uint32_t) item >= dw->tpag.count) continue;
        if (strcmp(sprite->name, "spr_gigavine") == 0) {
            const TexturePageItem* page = &dw->tpag.items[item];
            if (sprite->width == 514 && sprite->height == 37 && sprite->originX == 15 && sprite->originY == 14 &&
                page->sourceWidth == 511 && page->sourceHeight == 33 && page->targetX == 2 && page->targetY == 3 &&
                page->targetWidth == 511 && page->targetHeight == 33)
                swr->floweyVineItem = item;
        } else if (strcmp(sprite->name, "spr_bgpipe") == 0) {
            swr->floweyPipeItem = item;
        } else if (strcmp(sprite->name, "spr_pipepart") == 0) {
            const TexturePageItem* page = &dw->tpag.items[item];
            if (sprite->originX == 21 && sprite->originY == 19 &&
                page->sourceWidth == 45 && page->sourceHeight == 31 && page->targetX == 0 && page->targetY == 3 &&
                page->targetWidth == 45 && page->targetHeight == 31)
                swr->floweyPipeSegmentItem = item;
        } else if (strcmp(sprite->name, "spr_floweyarm") == 0) {
            swr->floweyArmItem = item;
        } else if (strcmp(sprite->name, "spr_floweyx_tv") == 0) {
            swr->floweyTvCaseItem = item;
        } else if (strcmp(sprite->name, "spr_flipeye_holder_bg") == 0) {
            swr->floweyOuterEyeShadow = item;
        } else {
            for (unsigned trimIndex = 0; trimIndex < sizeof(trim) / sizeof(trim[0]); ++trimIndex)
                if (strcmp(sprite->name, trim[trimIndex]) == 0) swr->floweyTrimItems[trimIndex] = item;
            for (unsigned backdropIndex = 0; backdropIndex < sizeof(backdrop) / sizeof(backdrop[0]); ++backdropIndex) {
                if (strcmp(sprite->name, backdrop[backdropIndex]) != 0) continue;
                swr->floweyBackdropItems[backdropIndex] = item;
                if (swr->floweyBackdropCount <= backdropIndex) swr->floweyBackdropCount = backdropIndex + 1;
                break;
            }
        }
    }
}

int SWRenderer_prefetchItem(Renderer* renderer, int32_t tpagIndex)
{
    if (renderer == NULL || renderer->vtable != &swrVtable) return SWR_PREFETCH_SKIPPED;
    return (int) swrPrefetchItem((SWRenderer*) renderer, tpagIndex);
}

int SWRenderer_prefetchItemStep(Renderer* renderer, int32_t tpagIndex, void** state)
{
    if (renderer == NULL || renderer->vtable != &swrVtable) {
        swrPrefetchCancel(state);
        return SWR_PREFETCH_SKIPPED;
    }
    return (int) swrPrefetchItemStep((SWRenderer*) renderer, tpagIndex, state);
}

void SWRenderer_prefetchCancel(void** state)
{
    swrPrefetchCancel(state);
}

int SWRenderer_planItem(Renderer* renderer, int32_t tpagIndex, uint32_t (*spans)[2], int max)
{
    if (renderer == NULL || renderer->vtable != &swrVtable || max <= 0) return -1;
    SwrPackSpan planned[16];
    if (max > 16) max = 16;
    int count = swrPlanItem((SWRenderer*) renderer, tpagIndex, planned, max);
    for (int i = 0; i < count; ++i) {
        spans[i][0] = planned[i].start;
        spans[i][1] = planned[i].size;
    }
    return count;
}

unsigned SWRenderer_textureEvictions(void)
{
    extern unsigned swrTextureEvictions;
    return swrTextureEvictions;
}

void SWRenderer_takeItemLoads(unsigned* count, uint64_t* nanos)
{
    SwrItemLoads loads;
    swrTakeItemLoads(&loads);
    *count = loads.count;
    *nanos = loads.nanos;
}

void SWRenderer_setFloweySpeedRoom(Renderer* renderer, bool active)
{
    ((SWRenderer*) renderer)->floweySpeedRoom = active;
}

void SWRenderer_drawFloweyStalkEyePipes(Renderer* renderer)
{
    if (renderer == NULL || renderer->vtable != &swrVtable) return;
    SWRenderer* swr = (SWRenderer*) renderer;
    if (SWR_SKIPPED(swr) || !swr->floweySpeedRoom || !swrSmoothMinify ||
        swr->drawingToSurface || swr->fb != swr->mainFb || swr->blendMode != bm_normal ||
        swr->scaleX != 0.5f || swr->scaleY != 0.5f || swr->floweyPipeSegmentItem < 0) return;
    if (swr->floweyStalkEyePipes == NULL) {
        int32_t item = swr->floweyPipeSegmentItem;
        const TexturePageItem* page = &renderer->dataWin->tpag.items[item];
        SWTexture* pipe = swrTextureForItem(swr, item);
        if (pipe == NULL) return;
        SWTexture* art = swrMakeFloweyStalkEyePipes(pipe, page->sourceX - pipe->originX, page->sourceY - pipe->originY);
        if (art == NULL) return;
        int32_t backingItem = swr->floweyBackdropItems[SWR_FLOWEY_BACKDROP_SHADOW];
        if (backingItem >= 0) {
            SWTexture* backing = swrTextureForItem(swr, backingItem);
            if (backing == NULL) {
                swrFreeTexture(art);
                return;
            }
            swrAddFloweyStalkEyeBacking(art, backing, &renderer->dataWin->tpag.items[backingItem]);
        }
        swr->floweyStalkEyePipes = art;
    }
    // Draw at the native eye-pipe event's depth, over the green stalks and
    // under the eyes. Like the rear pipes, hold the pose through camera shake.
    SWTexture* art = swr->floweyStalkEyePipes;
    swrDrawSprite(renderer, swr->viewX + art->originX * 2, swr->viewY + art->originY * 2,
                  art->width * 2, art->height * 2, art, 0, 0, art->width, art->height, 0xffffff, 1.0f);
}

bool SWRenderer_isSkippingScreen(const Renderer* renderer)
{
    return renderer != NULL && renderer->vtable == &swrVtable && SWR_SKIPPED((const SWRenderer*)renderer);
}

void SWRenderer_setContrastAssets(Renderer* renderer, const char* const* sprites, size_t spriteCount,
                                 const char* const* fonts, size_t fontCount)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    uint32_t i;
    free(swr->hintedGlyphs); swr->hintedGlyphs = NULL; swr->hintedGlyphCount = 0;
    free(swr->contrastItems); free(swr->contrastFonts);
    swr->contrastItemCount = dw->tpag.count;
    swr->contrastFontCount = dw->font.count;
    swr->contrastItems = dw->tpag.count ? (uint8_t*) safeCalloc(dw->tpag.count, 1) : NULL;
    swr->contrastFonts = dw->font.count ? (uint8_t*) safeCalloc(dw->font.count, 1) : NULL;
    for (i = 0; i < dw->sprt.count; ++i) {
        const Sprite* sprite = &dw->sprt.sprites[i];
        if (!swrAssetNamed(sprite->name, sprites, spriteCount)) continue;
        for (uint32_t j = 0; j < sprite->textureCount; ++j) {
            int32_t item = sprite->tpagIndices[j];
            if (item >= 0 && (uint32_t) item < dw->tpag.count) swr->contrastItems[item] = SWR_CONTRAST_BUTTON;
        }
    }
    for (i = 0; i < dw->font.count; ++i)
        swr->contrastFonts[i] = swrAssetNamed(dw->font.fonts[i].name, fonts, fontCount) ? SWR_FONT_BUBBLE_CONTRAST : 0;
}

void SWRenderer_setContrastGlyphAssets(Renderer* renderer, const char* const* sprites, size_t spriteCount,
                                      const char* const* fonts, size_t fontCount)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    uint32_t i;
    for (i = 0; i < dw->sprt.count; ++i) {
        const Sprite* sprite = &dw->sprt.sprites[i];
        if (!swrAssetNamed(sprite->name, sprites, spriteCount)) continue;
        for (uint32_t j = 0; j < sprite->textureCount; ++j) {
            int32_t item = sprite->tpagIndices[j];
            if (item >= 0 && (uint32_t) item < swr->contrastItemCount) swr->contrastItems[item] = SWR_CONTRAST_UI_GLYPH;
        }
    }
    for (i = 0; i < swr->contrastFontCount; ++i)
        if (swrAssetNamed(dw->font.fonts[i].name, fonts, fontCount)) swr->contrastFonts[i] |= SWR_FONT_WHITE_CONTRAST;
}

void SWRenderer_setHintedGlyphAssets(Renderer* renderer, const SWHintedGlyphAsset* assets, size_t count)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    free(swr->hintedGlyphs); swr->hintedGlyphs = NULL; swr->hintedGlyphCount = 0;
    if (!assets || count == 0 || count > 16) return;
    swr->hintedGlyphs = (SWHintedGlyphBinding*) safeCalloc(count, sizeof(SWHintedGlyphBinding));
    for (size_t a = 0; a < count; ++a) {
        if (!assets[a].spriteName) continue;
        for (uint32_t i = 0; i < dw->sprt.count; ++i) {
            const Sprite* sprite = &dw->sprt.sprites[i];
            if (!sprite->name || strcmp(sprite->name, assets[a].spriteName) != 0 || sprite->textureCount != 1) continue;
            int32_t item = sprite->tpagIndices[0];
            if (item < 0 || (uint32_t) item >= dw->tpag.count) continue;
            const TexturePageItem* tpag = &dw->tpag.items[item];
            if (tpag->sourceWidth != assets[a].width || tpag->sourceHeight != assets[a].height ||
                tpag->targetWidth != tpag->sourceWidth || tpag->targetHeight != tpag->sourceHeight) continue;
            swr->hintedGlyphs[swr->hintedGlyphCount].item = item;
            swr->hintedGlyphs[swr->hintedGlyphCount].asset = &assets[a];
            ++swr->hintedGlyphCount;
            break;
        }
    }
}

void SWRenderer_setContrastEnabled(Renderer* renderer, bool enabled)
{
    ((SWRenderer*) renderer)->contrastEnabled = enabled;
}

Renderer* SWRenderer_create(void)
{
    SWRenderer* swr = (SWRenderer*) safeCalloc(1, sizeof(SWRenderer));
    swr->base.vtable = &swrVtable;
    swrVtable.init                     = SWRenderer_init;
    swrVtable.destroy                  = SWRenderer_destroy;
    swrVtable.beginFrame               = SWRenderer_beginFrame;
    swrVtable.endFrameInit             = SWRenderer_endFrameInit;
    swrVtable.endFrameEnd              = SWRenderer_endFrameEnd;
    swrVtable.beginView                = SWRenderer_beginView;
    swrVtable.endView                  = SWRenderer_endView;
    swrVtable.beginGUI                 = SWRenderer_beginGUI;
    swrVtable.setGuiProjection         = SWRenderer_setGuiProjection;
    swrVtable.endGUI                   = SWRenderer_endGUI;
    swrVtable.drawSprite               = SWRenderer_drawSprite;
    swrVtable.drawSpritePart           = SWRenderer_drawSpritePart;
    swrVtable.drawSpritePos            = SWRenderer_drawSpritePos;
    swrVtable.drawRectangle            = SWRenderer_drawRectangle;
    swrVtable.drawRectangleColor       = SWRenderer_drawRectangleColor;
    swrVtable.drawLine                 = SWRenderer_drawLine;
    swrVtable.drawTriangle             = SWRenderer_drawTriangle;
    swrVtable.drawFilledEllipse        = SWRenderer_drawFilledEllipse;
    swrVtable.drawLineColor            = SWRenderer_drawLineColor;
    swrVtable.drawText                 = SWRenderer_drawText;
    swrVtable.drawTextColor            = SWRenderer_drawTextColor;
    swrVtable.flush                    = SWRenderer_flush;
    swrVtable.clearScreen              = SWRenderer_clearScreen;
    swrVtable.createSpriteFromSurface  = SWRenderer_createSpriteFromSurface;
    swrVtable.deleteSprite             = SWRenderer_deleteSprite;
    swrVtable.gpuSetBlendMode          = SWRenderer_gpuSetBlendMode;
    swrVtable.gpuSetBlendModeExt       = SWRenderer_gpuSetBlendModeExt;
    swrVtable.gpuSetBlendEnable        = SWRenderer_gpuSetBlendEnable;
    swrVtable.gpuSetAlphaTestEnable    = SWRenderer_gpuSetAlphaTestEnable;
    swrVtable.gpuSetAlphaTestRef       = SWRenderer_gpuSetAlphaTestRef;
    swrVtable.gpuSetColorWriteEnable   = SWRenderer_gpuSetColorWriteEnable;
    swrVtable.gpuGetColorWriteEnable   = SWRenderer_gpuGetColorWriteEnable;
    swrVtable.gpuGetBlendEnable        = SWRenderer_gpuGetBlendEnable;
    swrVtable.gpuGetBlendMode          = SWRenderer_gpuGetBlendMode;
    swrVtable.gpuSetTexFilter          = SWRenderer_gpuSetTexFilter;
    swrVtable.gpuSetFog                = SWRenderer_gpuSetFog;
    swrVtable.drawSpriteTiled          = SWRenderer_drawSpriteTiled;
    swrVtable.drawSurfaceTiled         = SWRenderer_drawSurfaceTiled;
    swrVtable.createSurface            = SWRenderer_createSurface;
    swrVtable.surfaceExists            = SWRenderer_surfaceExists;
    swrVtable.setRenderTarget          = SWRenderer_setRenderTarget;
    swrVtable.ensureApplicationSurface = SWRenderer_ensureApplicationSurface;
    swrVtable.getSurfaceWidth          = SWRenderer_getSurfaceWidth;
    swrVtable.getSurfaceHeight         = SWRenderer_getSurfaceHeight;
    swrVtable.drawSurface              = SWRenderer_drawSurface;
    swrVtable.surfaceResize            = SWRenderer_surfaceResize;
    swrVtable.surfaceFree              = SWRenderer_surfaceFree;
    swrVtable.surfaceCopy              = SWRenderer_surfaceCopy;
    swrVtable.surfaceGetPixels         = SWRenderer_surfaceGetPixels;
    swrVtable.drawTiledPart            = SWRenderer_drawTiledPart;
    swrVtable.spriteGetTexture         = SWRenderer_spriteGetTexture;
    swrVtable.surfaceGetTexture        = SWRenderer_surfaceGetTexture;
    swrVtable.textureGetTexelWidth     = SWRenderer_textureGetTexelWidth;
    swrVtable.textureGetTexelHeight    = SWRenderer_textureGetTexelHeight;
    swrVtable.textureGetUVs            = SWRenderer_textureGetUVs;
    swrVtable.textureSetStage          = SWRenderer_textureSetStage;
    swrVtable.gpuSetShader             = SWRenderer_gpuSetShader;
    swrVtable.gpuResetShader           = SWRenderer_gpuResetShader;
    swrVtable.shaderGetUniform         = SWRenderer_shaderGetUniform;
    swrVtable.shaderGetSamplerIndex    = SWRenderer_shaderGetSamplerIndex;
    swrVtable.shaderSetUniformF        = SWRenderer_shaderSetUniformF;
    swrVtable.shaderSetUniformI        = SWRenderer_shaderSetUniformI;
    swrVtable.shaderIsCompiled         = SWRenderer_shaderIsCompiled;
    swrVtable.shadersSupported         = SWRenderer_shadersSupported;
    swrVtable.applyProjection          = SWRenderer_applyProjection;
    swrVtable.primitiveBegin           = SWRenderer_primitiveBegin;
    swrVtable.primitiveBeginTexture    = SWRenderer_primitiveBeginTexture;
    swrVtable.primitiveEnd             = SWRenderer_primitiveEnd;
    swrVtable.drawVertex               = SWRenderer_drawVertex;
    swrVtable.drawVertexBuffer         = SWRenderer_drawVertexBuffer;
    swrVtable.drawTextUI               = SWRenderer_drawTextUI;
#ifdef SW_DRAW_PROFILE
    swrVtable.drawSprite               = SWRenderer_profDrawSprite;
    swrVtable.drawSpritePart           = SWRenderer_profDrawSpritePart;
    swrVtable.drawText                 = SWRenderer_profDrawText;
    swrVtable.drawTextColor            = SWRenderer_profDrawTextColor;
    swrVtable.drawSpriteTiled          = SWRenderer_profDrawSpriteTiled;
    swrVtable.drawRectangle            = SWRenderer_profDrawRectangle;
#endif
    swrVtable.drawSpriteGrid           = SWRenderer_drawSpriteGrid;
    swrVtable.drawTileRun              = SWRenderer_drawTileRun;
#ifdef SW_DRAW_PROFILE
    swrVtable.drawTileRun              = SWRenderer_profDrawTileRun;
#endif
    swrVtable.drawTileLayer            = SWRenderer_drawTileLayer;
#ifdef SW_DRAW_PROFILE
    swrVtable.drawTileLayer            = SWRenderer_profDrawTileLayer;
#endif
    
    swrVtable.drawTile                 = NULL;
    
    swr->base.drawColor = 0xFFFFFF;
    swr->base.drawAlpha = 1.0f;
    swr->base.drawFont = -1;
    swr->base.drawHalign = 0;
    swr->base.drawValign = 0;
    swr->base.circlePrecision = 24;

    return (Renderer*) swr;
}
