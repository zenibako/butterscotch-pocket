#ifndef _SW_TEXTURE_PACK_H
#define _SW_TEXTURE_PACK_H

#include "sw_renderer_private.h"

// Optional pack of texture pages already converted to the renderer's 16-bit
// pixel format, so low-end targets can skip PNG decoding and the RGBA
// intermediate buffer that comes with it. Built offline from data.win.
//
// Each page is stored as a grid of square tiles that are compressed one by
// one, so that the part of a page a sprite occupies can be read without the
// rest: a 2048x2048 page is 8 MB decoded, and a battle needs a few monsters
// off it.
//
// File layout (all little-endian):
//   char     magic[4]   "UTX2"
//   uint32_t pageCount
//   uint32_t tileSize   side of a tile in pixels, a power of two from 16
//                       to 128; tiles on the right and bottom edges of a
//                       page are cut to fit it
//   uint32_t tileCount  across all pages
//   pageCount x { uint16_t width, height; uint32_t firstTile; }
//                       a page's tiles are numbered row by row from firstTile;
//                       width 0 means the pack does not hold the page
//   tileCount x { uint32_t offset, size; }
//                       where the tile's data is in the file, in bytes;
//                       size 0 is a tile that is transparent throughout
//   tile data, run-length encoded as 16-bit words:
//     control word with bit 15 set:   repeat the next word (control & 0x7FFF) + 1 times
//     control word with bit 15 clear: copy the next (control + 1) words verbatim
#define SW_TEXTURE_PACK_MAGIC "UTX2"
#define SW_TEXTURE_PACK_RUN_FLAG 0x8000
#define SW_TEXTURE_PACK_MAX_COUNT 0x8000
// The tile size the builder uses. The reader takes the size from the header,
// so older packs with larger tiles still load.
#define SW_TEXTURE_PACK_TILE 64

#ifndef SW_TEXTURE_PACK_PATH
#define SW_TEXTURE_PACK_PATH "textures.bin"
#endif

// A platform can carry the pack at the end of another file it reads all the
// time (SW_TEXTURE_PACK_HOST), so the two are read without switching files,
// which on the Pocket costs about 20 ms each way. The host file then ends
// with 16 bytes: uint32_t offset, size (where the pack is in it, the pack
// running right up to these bytes), uint32_t 0, char[4] "UTXT". Without that
// ending, or without the host file, the pack is read from its own file.
#define SW_TEXTURE_PACK_TAIL_MAGIC "UTXT"

// Returns false if there is no pack, or it has no entry for this page.
bool swrTexturePackGetSize(uint32_t pageId, int* outW, int* outH);

// The optional Flowey page follows the game's native texture pages.
bool swrTexturePackHasFloweyBackdrop(uint32_t nativePages);

bool swrTexturePackHasFloweyStalk(unsigned pose, int width, int height);
bool swrTexturePackDecodeFloweyStalk(unsigned pose, int width, int height, uintpixel_t* buffer);
bool swrTexturePackDecodeFloweyOpeningItem(int32_t item, uint32_t page, int x, int y, int w, int h, uintpixel_t* buffer);

// Decodes a page into `buffer`, which must hold width * height pixels.
bool swrTexturePackDecode(uint32_t pageId, uintpixel_t* buffer);

// Decodes the w x h rectangle of a page whose top left corner is (x, y) into
// `buffer`. The rectangle must lie within the page.
bool swrTexturePackDecodeRect(uint32_t pageId, int x, int y, int w, int h, uintpixel_t* buffer);

// Where the time of the decodes since the last call went, for the log line
// of a slow load: the reads, how long they waited for the platform to accept
// them, the platform's busy work between reads (audio) and the decoding.
typedef struct
{
    uint32_t reads, bytes;
    uint64_t readNanos, refusedNanos, tickNanos, decodeNanos;
}
SwrTexturePackStats;
void swrTexturePackTakeStats(SwrTexturePackStats* out);

// Cumulative window-read totals, independent of the per-load stats. A
// declined direct read includes a backend that is unavailable. Stdio time
// includes every fseek/fread attempt, whether or not it succeeds.
typedef struct
{
    uint32_t directReads, directDeclines, stdioReads, stdioErrors;
    uint64_t directNanos, declinedNanos, stdioNanos;
}
SwrTexturePackReadTotals;
void swrTexturePackGetReadTotals(SwrTexturePackReadTotals* out);

// Where in its file the pack starts: 0 unless it rides in the host file.
uint32_t swrTexturePackOffset(void);

// Why the last decode failed: "read" (with errno left as the C library set
// it) or "data" for a tile that does not decode. Empty after a success.
const char* swrTexturePackLastError(void);

#endif//_SW_TEXTURE_PACK_H
