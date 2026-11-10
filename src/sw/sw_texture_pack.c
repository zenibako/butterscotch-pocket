#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "sw_texture_pack.h"
#include "gettime.h"

#if PIXEL_SIZE == 16 && !defined IS_BIG_ENDIAN

typedef struct
{
    uint16_t width, height;
    uint32_t firstTile;
}
PackPage;

typedef struct
{
    uint32_t offset, size;
}
PackTile;

// The optional index precedes the UTXT footer. Its offsets are relative to
// the start of UTX2, so appending poses leaves the native tiles in place.
typedef struct {
    uint16_t width, height;
    PackTile tile;
} PackStalk;

typedef struct {
    PackStalk stalks[SWR_FLOWEY_STALK_POSES];
    char magic[4];
    uint32_t tail[4];
} PackHostFooter;

// Any pack's tiles fit these: the builder's tile size can change, and the
// reader takes the one the pack's header gives.
#define TILE_MAX 128
// A tile that does not compress at all is its pixels plus one control word.
#define TILE_MAX_WORDS (TILE_MAX * TILE_MAX + 16)

static FILE* packFile = NULL;
static const char* packPath = SW_TEXTURE_PACK_PATH;
static uint32_t packBase = 0;
static PackPage* packPages = NULL;
static PackTile* packTiles = NULL;
static uint32_t packCount = 0;
static uint32_t packTileCount = 0;
static int packTile = 0;
static bool packProbed = false;
static PackStalk packStalks[SWR_FLOWEY_STALK_POSES];
static uint32_t packStalkLimit;

static void packOpen(void);

// Opens the pack where it is and leaves the file at its start.
static FILE* packFind(void)
{
#ifdef SW_TEXTURE_PACK_HOST
    FILE* host = fopen(SW_TEXTURE_PACK_HOST, "rb");
    if (host) {
        PackHostFooter footer = {0};
        if (fseek(host, 0, SEEK_END) == 0) {
            long end = ftell(host);
            long size = end < (long) sizeof(footer) ? end : (long) sizeof(footer);
            if (size >= (long) sizeof(footer.tail) && fseek(host, end - size, SEEK_SET) == 0 &&
                fread((char*) &footer + sizeof(footer) - size, 1, (size_t) size, host) == (size_t) size) {
                const uint32_t* tail = footer.tail; // offset, size, reserved, magic
                if (memcmp(&tail[3], SW_TEXTURE_PACK_TAIL_MAGIC, 4) == 0 && tail[2] == 0 && tail[1] > 0 &&
                    (uint64_t) tail[0] + tail[1] == (uint64_t) end - sizeof(footer.tail) &&
                    fseek(host, (long) tail[0], SEEK_SET) == 0) {
                    packPath = SW_TEXTURE_PACK_HOST;
                    packBase = tail[0];
                    size_t indexSize = sizeof(footer) - sizeof(footer.tail);
                    if (tail[1] > indexSize && memcmp(footer.magic, "UST1", 4) == 0) {
                        memcpy(packStalks, footer.stalks, sizeof(packStalks));
                        packStalkLimit = tail[1] - indexSize;
                    }
                    return host;
                }
            }
        }
        fclose(host);
    }
#endif
    packPath = SW_TEXTURE_PACK_PATH;
    packBase = 0;
    return fopen(SW_TEXTURE_PACK_PATH, "rb");
}

uint32_t swrTexturePackOffset(void)
{
    if (!packProbed) packOpen();
    return packBase;
}

static void packOpen(void)
{
    packProbed = true;
    
    FILE* file = packFind();
    if (!file) {
        logInfo("SWR: No texture pack (%s), decoding PNG pages instead.\n", SW_TEXTURE_PACK_PATH);
        return;
    }
    
    char magic[4];
    uint32_t header[3] = { 0, 0, 0 }; // pageCount, tileSize, tileCount
    if (fread(magic, 1, 4, file) != 4 || memcmp(magic, SW_TEXTURE_PACK_MAGIC, 4) != 0 ||
        fread(header, sizeof(uint32_t), 3, file) != 3 || header[0] == 0 || header[0] > 4096 ||
        header[1] < 16 || header[1] > TILE_MAX || (header[1] & (header[1] - 1)) != 0 || header[2] == 0 || header[2] > 1024 * 1024) {
        logWarn("SWR: %s is not a texture pack this build can read (rebuild it), ignoring it.\n", packPath);
        fclose(file);
        return;
    }
    
    PackPage* pages = (PackPage*) malloc(header[0] * sizeof(PackPage));
    PackTile* tiles = (PackTile*) malloc(header[2] * sizeof(PackTile));
    if (!pages || !tiles || fread(pages, sizeof(PackPage), header[0], file) != header[0] ||
        fread(tiles, sizeof(PackTile), header[2], file) != header[2]) {
        logWarn("SWR: Texture pack index is truncated, ignoring the pack.\n");
        free(pages);
        free(tiles);
        fclose(file);
        return;
    }
    // Tiles are read into a window by their offsets, so one that runs past
    // the end of the 32-bit file space must not get that far.
    uint32_t lastTileEnd = 0;
    for (uint32_t i = 0; i < header[2]; i++)
    {
        if ((uint64_t) packBase + tiles[i].offset + tiles[i].size > UINT32_MAX) {
            logWarn("SWR: Texture pack tile %u lies outside the file, ignoring the pack.\n", (unsigned) i);
            free(pages);
            free(tiles);
            fclose(file);
            return;
        }
        if (tiles[i].size && tiles[i].offset + tiles[i].size > lastTileEnd)
            lastTileEnd = tiles[i].offset + tiles[i].size;
    }
    if (packStalkLimit != 0) {
        for (unsigned pose = 0; pose < SWR_FLOWEY_STALK_POSES; ++pose) {
            const PackStalk* stalk = &packStalks[pose];
            const PackTile* tile = &stalk->tile;
            uint64_t end = (uint64_t) tile->offset + tile->size;
            if (stalk->width != 180 || stalk->height < 75 || stalk->height > 95 ||
                tile->offset != lastTileEnd || (tile->offset & 1) || (tile->size & 1) ||
                tile->size == 0 || tile->size > TILE_MAX_WORDS * sizeof(uint16_t) ||
                end > packStalkLimit || packBase + end > UINT32_MAX) {
                packStalkLimit = 0;
                break;
            }
            lastTileEnd = (uint32_t) end;
        }
        if (lastTileEnd != packStalkLimit) packStalkLimit = 0;
    }
    
    setvbuf(file, NULL, _IONBF, 0);
    packFile = file;
    packPages = pages;
    packTiles = tiles;
    packCount = header[0];
    packTileCount = header[2];
    packTile = (int) header[1];
    logInfo("SWR: Texture pack with %u pages in %u tiles of %d, in %s at %u KB.\n", (unsigned) packCount, (unsigned) packTileCount, packTile,
            packPath, (unsigned) (packBase / 1024));
}

bool swrTexturePackGetSize(uint32_t pageId, int* outW, int* outH)
{
    if (!packProbed) packOpen();
    if (pageId >= packCount) return false;
    
    const PackPage* page = &packPages[pageId];
    if (page->width == 0 || page->height == 0) return false;
    
    *outW = page->width;
    *outH = page->height;
    return true;
}

bool swrTexturePackHasFloweyBackdrop(uint32_t nativePages)
{
    if (!packProbed) packOpen();
    if (nativePages >= packCount || packCount - nativePages != 1) return false;

    const PackPage* page = &packPages[nativePages];
    if (page->width != SWR_FLOWEY_BACKDROP_WIDTH || page->height != SWR_FLOWEY_BACKDROP_HEIGHT ||
        page->firstTile >= packTileCount) return false;
    uint32_t columns = (page->width + packTile - 1) / packTile;
    uint32_t rows = (page->height + packTile - 1) / packTile;
    return packTileCount - page->firstTile == columns * rows;
}

bool swrTexturePackHasFloweyStalk(unsigned pose, int width, int height)
{
    if (!packProbed) packOpen();
    return packFile && packStalkLimit && pose < SWR_FLOWEY_STALK_POSES &&
           width == packStalks[pose].width && height == packStalks[pose].height;
}

#ifdef PLATFORM_BUSY_TICK
// Called between the pieces of a long blocking read, so the platform can keep
// time-critical work going (feeding its audio queue, for one).
void platformBusyTick(void);
#endif

#ifdef PLATFORM_PACK_READ
// Reads `size` bytes at `offset` of the file at `path` into `dest`, fetching
// exactly that much. Returns false to have the caller read it with fread.
bool platformPackRead(const char* path, uint32_t offset, uint32_t size, void* dest);
// Total time the platform's reads have waited to be accepted.
uint64_t platformPackRefusedNanos(void);
#endif

static SwrTexturePackStats packStats;
static SwrTexturePackReadTotals packReadTotals;

void swrTexturePackTakeStats(SwrTexturePackStats* out)
{
    *out = packStats;
    memset(&packStats, 0, sizeof(packStats));
}

void swrTexturePackGetReadTotals(SwrTexturePackReadTotals* out)
{
    *out = packReadTotals;
}

static const char* packLastError = "";

const char* swrTexturePackLastError(void)
{
    return packLastError;
}

// Tiles are read a window at a time: one read covers the tiles a rectangle
// needs that lie close together in the file. Each read costs a fixed few
// milliseconds on the Pocket plus a little per KB, so tiles a short gap apart
// are cheaper to read together, gap included, than one by one. The last
// window is kept, so the next item often finds its tiles already in it.
#define WINDOW_BYTES (64 * 1024)
#define WINDOW_GAP_BYTES (6 * 1024)

static uint16_t window[WINDOW_BYTES / sizeof(uint16_t)];
static uint32_t windowStart = 0, windowEnd = 0;

static bool readWindow(uint32_t start, uint32_t end)
{
    windowStart = windowEnd = 0;
    uint32_t size = end - start;
    errno = 0;
    uint64_t readStart = nowNanos();
    uint64_t readEnd = readStart;
    bool done = false;
#ifdef PLATFORM_PACK_READ
    uint64_t refusedBefore = platformPackRefusedNanos();
    done = platformPackRead(packPath, packBase + start, size, window);
    packStats.refusedNanos += platformPackRefusedNanos() - refusedBefore;
    readEnd = nowNanos();
    if (done) {
        ++packReadTotals.directReads;
        packReadTotals.directNanos += readEnd - readStart;
    } else {
        ++packReadTotals.directDeclines;
        packReadTotals.declinedNanos += readEnd - readStart;
    }
#endif
    if (!done) {
        uint64_t stdioStart = readEnd;
        bool read = fseek(packFile, (long) (packBase + start), SEEK_SET) == 0 &&
                    fread(window, 1, size, packFile) == size;
        int readError = errno;
        readEnd = nowNanos();
        ++packReadTotals.stdioReads;
        packReadTotals.stdioNanos += readEnd - stdioStart;
        if (!read) {
            ++packReadTotals.stdioErrors;
            packLastError = "read";
            clearerr(packFile);
            errno = readError;
            return false;
        }
    }
    packStats.reads++;
    packStats.bytes += size;
    packStats.readNanos += readEnd - readStart;
#ifdef PLATFORM_BUSY_TICK
    platformBusyTick();
    packStats.tickNanos += nowNanos() - readEnd;
#endif
    windowStart = start;
    windowEnd = end;
    return true;
}

// Decodes one tile of tw x th pixels into `out`, row after row with no gaps.
// Its data must be in the window.
static bool decodeTile(const PackTile* tile, int tw, int th, uint16_t* out)
{
    packLastError = "data";
    size_t pixels = (size_t) tw * th;
    size_t count = tile->size / sizeof(uint16_t);
    if (count > TILE_MAX_WORDS || (tile->offset & 1) != 0) return false;
    const uint16_t* words = &window[(tile->offset - windowStart) / sizeof(uint16_t)];
    
    uint16_t* end = out + pixels;
    size_t i = 0;
    while (i < count)
    {
        uint16_t control = words[i++];
        if (control & SW_TEXTURE_PACK_RUN_FLAG) {
            size_t run = (size_t) (control & 0x7FFF) + 1;
            if (i >= count || run > (size_t) (end - out)) return false;
            uint16_t word = words[i++];
            for (size_t n = 0; n < run; n++) out[n] = word;
            out += run;
        } else {
            size_t literals = (size_t) control + 1;
            if (i + literals > count || literals > (size_t) (end - out)) return false;
            memcpy(out, &words[i], literals * sizeof(uint16_t));
            out += literals;
            i += literals;
        }
    }
    if (out != end) return false;
    packLastError = "";
    return true;
}

bool swrTexturePackDecodeFloweyStalk(unsigned pose, int width, int height, uintpixel_t* buffer)
{
    if (!swrTexturePackHasFloweyStalk(pose, width, height)) return false;
    const PackTile* tile = &packStalks[pose].tile;
    return readWindow(tile->offset, tile->offset + tile->size) && decodeTile(tile, width, height, buffer);
}

// Reads the window that starts with tile (tx, ty) of the rectangle's tiles
// tx0..tx1 x ty0..ty1, taking in the tiles after it while they are close
// enough and fit.
static bool loadWindowFrom(const PackPage* page, int columns, int tx0, int tx1, int ty1, int tx, int ty)
{
    const PackTile* first = &packTiles[page->firstTile + (uint32_t) (ty * columns + tx)];
    uint32_t start = first->offset;
    uint32_t end = start + first->size;
    if (first->size > WINDOW_BYTES) return false;
    
    for (int y = ty, x = tx + 1; y <= ty1; y++, x = tx0)
    {
        for (; x <= tx1; x++)
        {
            uint32_t index = page->firstTile + (uint32_t) (y * columns + x);
            if (index >= packTileCount) return readWindow(start, end);
            const PackTile* tile = &packTiles[index];
            if (tile->size == 0) continue;
            if (tile->offset < end || tile->offset - end > WINDOW_GAP_BYTES ||
                tile->offset + tile->size - start > WINDOW_BYTES)
                return readWindow(start, end);
            end = tile->offset + tile->size;
        }
    }
    return readWindow(start, end);
}

bool swrTexturePackDecodeRect(uint32_t pageId, int x, int y, int w, int h, uintpixel_t* buffer)
{
    int pageW, pageH;
    if (!swrTexturePackGetSize(pageId, &pageW, &pageH)) return false;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > pageW || y + h > pageH) return false;
    
    const int tileSize = packTile;
    const PackPage* page = &packPages[pageId];
    int columns = (pageW + tileSize - 1) / tileSize;
    static uint16_t pixels[TILE_MAX * TILE_MAX];
    int tx0 = x / tileSize, tx1 = (x + w - 1) / tileSize;
    int ty0 = y / tileSize, ty1 = (y + h - 1) / tileSize;
    
    for (int ty = ty0; ty <= ty1; ty++)
    {
        for (int tx = tx0; tx <= tx1; tx++)
        {
            uint32_t index = page->firstTile + (uint32_t) (ty * columns + tx);
            if (index >= packTileCount) return false;
            
            int tileX = tx * tileSize, tileY = ty * tileSize;
            int tw = pageW - tileX < tileSize ? pageW - tileX : tileSize;
            int th = pageH - tileY < tileSize ? pageH - tileY : tileSize;
            
            // The part of this tile that falls inside the rectangle.
            int left = x > tileX ? x : tileX;
            int top = y > tileY ? y : tileY;
            int right = x + w < tileX + tw ? x + w : tileX + tw;
            int bottom = y + h < tileY + th ? y + h : tileY + th;
            size_t rowBytes = (size_t) (right - left) * sizeof(uintpixel_t);
            
            const PackTile* tile = &packTiles[index];
            if (tile->size == 0) {
                packLastError = "";
                for (int row = top; row < bottom; row++)
                    memset(&buffer[(size_t) (row - y) * w + (left - x)], 0, rowBytes);
                continue;
            }
            if ((tile->offset < windowStart || tile->offset + tile->size > windowEnd) &&
                !loadWindowFrom(page, columns, tx0, tx1, ty1, tx, ty)) {
                if (packLastError[0] == '\0') packLastError = "data";
                return false;
            }
            uint64_t decodeStart = nowNanos();
            if (!decodeTile(tile, tw, th, pixels)) return false;
            packStats.decodeNanos += nowNanos() - decodeStart;
            for (int row = top; row < bottom; row++)
                memcpy(&buffer[(size_t) (row - y) * w + (left - x)], &pixels[(row - tileY) * tw + (left - tileX)], rowBytes);
        }
    }
    return true;
}

bool swrTexturePackDecode(uint32_t pageId, uintpixel_t* buffer)
{
    int pageW, pageH;
    if (!swrTexturePackGetSize(pageId, &pageW, &pageH)) return false;
    return swrTexturePackDecodeRect(pageId, 0, 0, pageW, pageH, buffer);
}

#else

bool swrTexturePackGetSize(UNUSED uint32_t pageId, UNUSED int* outW, UNUSED int* outH) { return false; }
bool swrTexturePackHasFloweyBackdrop(UNUSED uint32_t nativePages) { return false; }
bool swrTexturePackHasFloweyStalk(UNUSED unsigned pose, UNUSED int width, UNUSED int height) { return false; }
bool swrTexturePackDecodeFloweyStalk(UNUSED unsigned pose, UNUSED int width, UNUSED int height, UNUSED uintpixel_t* buffer) { return false; }
bool swrTexturePackDecode(UNUSED uint32_t pageId, UNUSED uintpixel_t* buffer) { return false; }
bool swrTexturePackDecodeRect(UNUSED uint32_t pageId, UNUSED int x, UNUSED int y, UNUSED int w, UNUSED int h, UNUSED uintpixel_t* buffer) { return false; }
const char* swrTexturePackLastError(void) { return ""; }
uint32_t swrTexturePackOffset(void) { return 0; }
void swrTexturePackTakeStats(SwrTexturePackStats* out) { memset(out, 0, sizeof(*out)); }
void swrTexturePackGetReadTotals(SwrTexturePackReadTotals* out) { memset(out, 0, sizeof(*out)); }

#endif
