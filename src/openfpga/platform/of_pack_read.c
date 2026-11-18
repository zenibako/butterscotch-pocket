/*
 * Exact-size reads of the texture pack.
 *
 * fread on openfpgaOS is served from the OS's file cache, which fetches a
 * whole 32 KB block from the Pocket's host for any miss: about 28 ms however
 * little was asked for. A texture tile is a few KB, so most of each block is
 * wasted, and a battle's first frames read dozens of tiles. of_file_read_async
 * fetches exactly what is asked, at about 3.5 ms plus 0.6 ms per KB.
 *
 * As in the music reader (readDirect in of_audio_system.c), the read is
 * started and then waited for: while one is in flight the OS fails every
 * ordinary read with "busy", so it must finish before anything else reads.
 */

#include "of.h"

#include "gettime.h"
#include "log.h"
#include "of_hooks.h"
#include "of_bench.h"
#include "of_state_requests.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef OF_PC

/* Staging memory for direct reads; smaller sizes are tried if the OS cannot
 * give this much. */
#define OF_PACK_STAGE_BYTES (64u * 1024u)
#define OF_PACK_STAGE_MIN (16u * 1024u)
/* As for music: longer than the 2 s after which the OS drops a stalled read
 * (a screenshot being saved), after which direct reads are not used again. */
#define OF_PACK_TIMEOUT_NANOS 3000000000ull
#define OF_PACK_SETTLE_NANOS 3000000000ull

static bool g_packProbed;
static bool g_packDirect;
static uint32_t g_packSlot;
static uint8_t *g_packStage;
static uint32_t g_packPiece;
static volatile int g_packDone;
static volatile int g_packResult;
static uint64_t g_packRefusedNanos;

uint64_t platformPackRefusedNanos(void) {
    return g_packRefusedNanos;
}

/* Runs from the read-completion interrupt. */
static void packCallback(int token, int result) {
    (void) token;
    g_packResult = result;
    g_packDone = 1;
}

static void packProbe(const char *path) {
    g_packProbed = true;
    uint32_t slot = 0;
    uint32_t maxRead = of_file_async_max_read();
    if (maxRead < OF_PACK_STAGE_MIN || of_file_slot_find(path, &slot) != 0) return;
    uint32_t size = OF_PACK_STAGE_BYTES < maxRead ? OF_PACK_STAGE_BYTES : maxRead;
    for (; size >= OF_PACK_STAGE_MIN && g_packStage == NULL; size /= 2) {
        g_packStage = of_file_dma_stage_alloc(size, 4096);
        if (g_packStage != NULL) g_packPiece = size;
    }
    if (g_packStage == NULL) return;
    g_packSlot = slot;
    g_packDirect = true;
    logInfo("SWR: %s is read directly, up to %u KB at a time.\n", path, (unsigned) (g_packPiece / 1024u));
}

/* The direct-read log (of_hooks.h). Reads never overlap: each reader waits for its
 * read to finish, so "what came before" is simply the last read to finish. */
static int g_readLast = -1;
static uint64_t g_readLastEnd;
static bool g_readVerbose;

void ofReadLogSetVerbose(bool verbose) {
    g_readVerbose = verbose;
}

/* Music refills are frequent: logging each would fill the log's 160 KB in a
 * run. The last few are kept and written out just before the next texture read,
 * so the ones that come right before a texture read are in the log in full. */
#define READLOG_MUSIC_KEPT 4
static struct { int frame; uint32_t size; uint64_t refused, wait, end; } g_readMusic[READLOG_MUSIC_KEPT];
static unsigned g_readMusicCount;

void ofReadLogNote(int kind, uint32_t size, uint64_t refusedNanos, uint64_t waitNanos) {
    uint64_t end = nowNanos();
    uint64_t start = end - refusedNanos - waitNanos;
    uint64_t since = g_readLastEnd != 0 && start > g_readLastEnd ? start - g_readLastEnd : 0;
    int after = g_readLast < 0 || since > 50000000ull ? OF_READ_AFTER_IDLE
              : g_readLast == OF_READLOG_TEX ? OF_READ_AFTER_TEX : OF_READ_AFTER_MUSIC;
    ofBenchNoteRead(kind, after, refusedNanos + waitNanos);
    if (g_readVerbose && kind == OF_READLOG_MUSIC) {
        unsigned slot = g_readMusicCount++ % READLOG_MUSIC_KEPT;
        g_readMusic[slot].frame = ofPlatformScriptFrame();
        g_readMusic[slot].size = size;
        g_readMusic[slot].refused = refusedNanos;
        g_readMusic[slot].wait = waitNanos;
        g_readMusic[slot].end = end;
    } else if (g_readVerbose) {
        unsigned kept = g_readMusicCount < READLOG_MUSIC_KEPT ? g_readMusicCount : READLOG_MUSIC_KEPT;
        for (unsigned i = g_readMusicCount - kept; i < g_readMusicCount; i++) {
            unsigned slot = i % READLOG_MUSIC_KEPT;
            unsigned wait = (unsigned) (g_readMusic[slot].wait / 100000u);
            unsigned ago = (unsigned) ((start - g_readMusic[slot].end) / 100000u);
            logInfo("READ music f%d %u B: wait %u.%u ms, ended %u.%u ms before the next texture read\n",
                    g_readMusic[slot].frame, (unsigned) g_readMusic[slot].size, wait / 10, wait % 10, ago / 10, ago % 10);
        }
        g_readMusicCount = 0;
        static const char *const afterNames[] = { "tex", "music", "idle" };
        unsigned refused = (unsigned) (refusedNanos / 100000u), wait = (unsigned) (waitNanos / 100000u);
        unsigned gap = (unsigned) (since / 100000u);
        logInfo("READ tex f%d %u B: refused %u.%u ms, wait %u.%u ms; after %s, %u.%u ms since\n",
                ofPlatformScriptFrame(), (unsigned) size, refused / 10, refused % 10, wait / 10, wait % 10,
                afterNames[after], gap / 10, gap % 10);
    }
    g_readLast = kind;
    g_readLastEnd = end;
}

static bool packReadPiece(uint32_t offset, uint32_t size) {
    g_packDone = 0;
    g_packResult = 0;
    uint64_t start = nowNanos();
    /* Refused while the host is busy with something else; that passes quickly. */
    while (of_file_read_async((int) g_packSlot, offset, g_packStage, size, packCallback) < 0) {
        if (nowNanos() - start > 50000000ull) return false;
    }
    uint64_t accepted = nowNanos();
    g_packRefusedNanos += accepted - start;
    /* The poll is the OS's fallback for a lost interrupt. */
    while (!g_packDone && of_file_async_poll() != 1) {
        if (nowNanos() - start > OF_PACK_TIMEOUT_NANOS) {
            /* Do not go back to ordinary reads while the OS still counts
             * this one as in flight. */
            uint64_t gaveUp = nowNanos();
            while (of_file_async_busy() == 1 && nowNanos() - gaveUp < OF_PACK_SETTLE_NANOS) of_file_async_poll();
            logWarn("SWR: a direct texture read timed out; using fread from here on.\n");
            g_packDirect = false;
            return false;
        }
    }
    ofReadLogNote(OF_READLOG_TEX, size, accepted - start, nowNanos() - accepted);
    return g_packResult >= 0;
}

bool platformPackRead(const char *path, uint32_t offset, uint32_t size, void *dest) {
    ofBgReadSettle();
    if (!g_packProbed) packProbe(path);
    if (!g_packDirect) return false;
    uint8_t *out = dest;
    for (uint32_t done = 0; done < size;) {
        uint32_t want = size - done < g_packPiece ? size - done : g_packPiece;
        if (!packReadPiece(offset + done, want)) return false;
        memcpy(out + done, g_packStage, want);
        done += want;
    }
    return true;
}

/* Reads `size` bytes at `offset` of the file in `slot` into the staging
 * memory; returns how long it took, or 0 if it failed. */
static uint64_t timedRead(uint32_t slot, uint32_t offset, uint32_t size) {
    g_packDone = 0;
    g_packResult = 0;
    uint64_t start = nowNanos();
    while (of_file_read_async((int) slot, offset, g_packStage, size, packCallback) < 0) {
        if (nowNanos() - start > 50000000ull) return 0;
    }
    while (!g_packDone && of_file_async_poll() != 1) {
        if (nowNanos() - start > OF_PACK_TIMEOUT_NANOS) {
            uint64_t gaveUp = nowNanos();
            while (of_file_async_busy() == 1 && nowNanos() - gaveUp < OF_PACK_SETTLE_NANOS) of_file_async_poll();
            return 0;
        }
    }
    return g_packResult >= 0 ? nowNanos() - start : 0;
}

/* Where in its file the texture pack starts (sw_texture_pack.c). */
uint32_t swrTexturePackOffset(void);

/* Each kind of read is timed a few times, every read at a part of its file
 * not read before in this probe, so nothing can come from a cache. */
void ofPlatformReadCosts(unsigned tenths[OF_READ_COSTS]) {
    enum { SIZE = 4096, ROUNDS = 4, STRIDE = 96 * 1024 };
    uint64_t total[OF_READ_COSTS] = { 0 };
    unsigned counted[OF_READ_COSTS] = { 0 };
    memset(tenths, 0, OF_READ_COSTS * sizeof(tenths[0]));
    uint32_t musicSlot = 0, dataSlot = 0;
    /* The texture pack is opened, and its first read made, by its first sprite. */
    uint32_t texAt = swrTexturePackOffset(), musicAt = 0, dataAt = 0;
    if (!g_packDirect || g_packPiece < SIZE || of_file_slot_find("music.bin", &musicSlot) != 0 ||
        of_file_slot_find("data.win", &dataSlot) != 0) return;
    #define TEX() timedRead(g_packSlot, (texAt += STRIDE), SIZE)
    #define MUSIC() timedRead(musicSlot, (musicAt += STRIDE), SIZE)
    #define COUNT(kind, nanos) do { uint64_t n_ = (nanos); if (n_ != 0) { total[kind] += n_; counted[kind]++; } } while (0)
    for (int round = 0; round < ROUNDS; round++) {
        TEX();
        COUNT(OF_READ_TEX_TEX, TEX());
        COUNT(OF_READ_TEX_MUSIC, MUSIC());
        COUNT(OF_READ_MUSIC_TEX, TEX());
        MUSIC();
        COUNT(OF_READ_MUSIC_MUSIC, MUSIC());
        TEX();
        uint64_t idle = nowNanos();
        while (nowNanos() - idle < 100000000ull) {}
        COUNT(OF_READ_IDLE_TEX, TEX());
        timedRead(dataSlot, (dataAt += STRIDE), SIZE);
        COUNT(OF_READ_FILE_TEX, TEX());
    }
    #undef TEX
    #undef MUSIC
    #undef COUNT
    for (int kind = 0; kind < OF_READ_COSTS; kind++)
        if (counted[kind] != 0) tenths[kind] = (unsigned) (total[kind] / counted[kind] / 100000u);
}


/* ===[ Background reads ]===
 *
 * A read the game does not wait for: it runs on the Pocket's host while the
 * CPU (running from SDRAM) goes on with the game. The OS finishes it from its
 * interrupt or its 1 kHz tick, or from of_file_async_poll; here it is noticed
 * by polling. The texture and music readers wait for it (ofBgReadSettle)
 * before reading.
 *
 * The read goes straight to where it was asked for (SDRAM), never to app
 * staging memory in CRAM0: the host fills the OS's own CRAM0 bounce window and
 * the OS copies it out before it reports the read finished. A finished read
 * cannot be copied out of CRAM0 by the app later, as rc.52 did: once the read
 * is over, the OS hands CRAM0 to the bridge at the next frame boundary
 * (of_savestate_poll parks it for savestates) or at any save-file close, and a
 * CPU load from CRAM0 then is a load access fault (rc.52's trap in bgFinish's
 * memcpy). The texture and music readers copy out of their staging at once,
 * inside the same call, so they are not exposed to this. */

#define OF_BG_PIECE_BYTES (32u * 1024u)
#define OF_BG_PIECE_MIN (8u * 1024u)

static bool g_bgProbed, g_bgOff, g_bgActive;
static uint32_t g_bgPiece;
static volatile int g_bgDone;
static volatile int g_bgResult;
static uint64_t g_bgStart;
static int g_bgOutcome = OF_BG_IDLE;
static uint32_t g_bgReads;
static uint64_t g_bgReadNanos, g_bgSettleNanos;
static struct { const char *path; uint32_t slot; } g_bgFiles[2];

/* Runs from the read-completion interrupt. */
static void bgCallback(int token, int result) {
    (void) token;
    g_bgResult = result;
    g_bgDone = 1;
}

uint32_t ofBgReadMax(void) {
    if (!g_bgProbed) {
        g_bgProbed = true;
        uint32_t maxRead = of_file_async_max_read();
        uint32_t size = OF_BG_PIECE_BYTES < maxRead ? OF_BG_PIECE_BYTES : maxRead;
        if (size >= OF_BG_PIECE_MIN) g_bgPiece = size;
        if (g_bgPiece != 0) logInfo("Prefetch: reads in the background, up to %u KB at a time.\n", (unsigned) (g_bgPiece / 1024u));
    }
    return g_bgOff ? 0 : g_bgPiece;
}

static bool bgSlot(const char *path, uint32_t *slot) {
    for (int i = 0; i < 2; i++) {
        if (g_bgFiles[i].path != NULL && strcmp(g_bgFiles[i].path, path) == 0) {
            *slot = g_bgFiles[i].slot;
            return true;
        }
    }
    if (of_file_slot_find(path, slot) != 0) return false;
    for (int i = 0; i < 2; i++) {
        if (g_bgFiles[i].path == NULL) {
            g_bgFiles[i].path = path;
            g_bgFiles[i].slot = *slot;
            break;
        }
    }
    return true;
}

bool ofBgReadStart(const char *path, uint32_t offset, uint32_t size, void *dest) {
    uint32_t slot = 0;
    if (g_bgActive || g_bgOutcome != OF_BG_IDLE || size == 0 || size > ofBgReadMax() || ofSavestateDeferFileReads() ||
        !bgSlot(path, &slot)) return false;
    g_bgDone = 0;
    g_bgResult = 0;
    /* Refused while the host is busy with something else: tried again at the next poll.
     * `dest` is SDRAM: the OS copies the read there from its bounce window before
     * the callback, so the app never loads from CRAM0 (see above). */
    if (of_file_read_async((int) slot, offset, dest, size, bgCallback) < 0) return false;
    g_bgActive = true;
    g_bgStart = nowNanos();
    return true;
}

static void bgFinish(void) {
    bool ok = g_bgResult >= 0;
    g_bgActive = false;
    g_bgOutcome = ok ? OF_BG_DONE : OF_BG_FAILED;
    g_bgReads++;
    g_bgReadNanos += nowNanos() - g_bgStart;
}

/* As the texture and music readers do: a read that has not finished in 3 s
 * (the host saving a screenshot) is given up, and background reads with it. */
static bool bgTimedOut(void) {
    if (nowNanos() - g_bgStart <= OF_PACK_TIMEOUT_NANOS) return false;
    uint64_t gaveUp = nowNanos();
    while (of_file_async_busy() == 1 && nowNanos() - gaveUp < OF_PACK_SETTLE_NANOS) of_file_async_poll();
    logWarn("Prefetch: a background read timed out; none from here on.\n");
    g_bgOff = true;
    g_bgActive = false;
    g_bgOutcome = OF_BG_FAILED;
    return true;
}

int ofBgReadPoll(void) {
    if (g_bgActive) {
        if (!g_bgDone) of_file_async_poll(); /* the OS's fallback for a lost interrupt */
        if (g_bgDone) bgFinish();
        else if (!bgTimedOut()) return OF_BG_BUSY;
    }
    int outcome = g_bgOutcome;
    g_bgOutcome = OF_BG_IDLE;
    return outcome;
}

void ofBgReadSettle(void) {
    if (!g_bgActive) return;
    uint64_t start = nowNanos();
    while (!g_bgDone && of_file_async_poll() != 1) {
        if (bgTimedOut()) break;
    }
    if (g_bgActive && g_bgDone) bgFinish();
    g_bgSettleNanos += nowNanos() - start;
}

void ofBgReadReset(void) {
    g_bgActive = false;
    g_bgOutcome = OF_BG_IDLE;
}

void ofBgReadTotals(uint32_t *reads, uint64_t *readNanos, uint64_t *settleNanos) {
    *reads = g_bgReads;
    *readNanos = g_bgReadNanos;
    *settleNanos = g_bgSettleNanos;
}

#else

void ofPlatformReadCosts(unsigned tenths[OF_READ_COSTS]) {
    memset(tenths, 0, OF_READ_COSTS * sizeof(tenths[0]));
}

uint64_t platformPackRefusedNanos(void) {
    return 0;
}

void ofReadLogSetVerbose(bool verbose) {
    (void) verbose;
}

void ofReadLogNote(int kind, uint32_t size, uint64_t refusedNanos, uint64_t waitNanos) {
    (void) kind; (void) size; (void) refusedNanos; (void) waitNanos;
}

bool platformPackRead(const char *path, uint32_t offset, uint32_t size, void *dest) {
    (void) path;
    (void) offset;
    (void) size;
    (void) dest;
    return false;
}


/* Background reads on the desktop: the read is made at once, and reported
 * finished at the next poll. */
static bool g_bgActive;
static int g_bgOutcome = OF_BG_IDLE;
static uint64_t g_bgStart, g_bgReadyAt;
static uint32_t g_bgReads;
static uint64_t g_bgReadNanos, g_bgSettleNanos;
static struct { const char *path; FILE *file; } g_bgFiles[2];

#define OF_BG_PC_PIECE (32u * 1024u)

uint32_t ofBgReadMax(void) {
    return OF_BG_PC_PIECE;
}

static FILE *bgFile(const char *path) {
    for (int i = 0; i < 2; i++)
        if (g_bgFiles[i].path != NULL && strcmp(g_bgFiles[i].path, path) == 0) return g_bgFiles[i].file;
    for (int i = 0; i < 2; i++) {
        if (g_bgFiles[i].path == NULL) {
            FILE *file = fopen(path, "rb");
            if (file == NULL) return NULL;
            g_bgFiles[i].path = path;
            g_bgFiles[i].file = file;
            return file;
        }
    }
    return NULL;
}

/* How long a read takes to finish: nothing here. */
static uint64_t bgLatency(uint32_t size) {
    (void) size;
    return 0;
}

bool ofBgReadStart(const char *path, uint32_t offset, uint32_t size, void *dest) {
    if (g_bgActive || g_bgOutcome != OF_BG_IDLE || size == 0 || size > OF_BG_PC_PIECE) return false;
    FILE *file = bgFile(path);
    if (file == NULL) return false;
    bool ok = fseek(file, (long) offset, SEEK_SET) == 0 && fread(dest, 1, size, file) == size;
    g_bgStart = nowNanos();
    g_bgReadyAt = g_bgStart + (ok ? bgLatency(size) : 0);
    g_bgActive = true;
    g_bgOutcome = ok ? OF_BG_DONE : OF_BG_FAILED;
    return true;
}

int ofBgReadPoll(void) {
    if (g_bgActive) {
        if (nowNanos() < g_bgReadyAt) return OF_BG_BUSY;
        g_bgActive = false;
        g_bgReads++;
        g_bgReadNanos += nowNanos() - g_bgStart;
    }
    int outcome = g_bgOutcome;
    g_bgOutcome = OF_BG_IDLE;
    return outcome;
}

void ofBgReadSettle(void) {
    if (!g_bgActive) return;
    uint64_t start = nowNanos();
    while (nowNanos() < g_bgReadyAt) {
    }
    g_bgSettleNanos += nowNanos() - start;
}

void ofBgReadReset(void) {
    g_bgActive = false;
    g_bgOutcome = OF_BG_IDLE;
}

void ofBgReadTotals(uint32_t *reads, uint64_t *readNanos, uint64_t *settleNanos) {
    *reads = g_bgReads;
    *readNanos = g_bgReadNanos;
    *settleNanos = g_bgSettleNanos;
}

#endif

/* Common runner hook: lazy room and texture payloads share the OS file service with assets. */
void platformBeforeDataFileRead(void) {
    ofBgReadSettle();
}
