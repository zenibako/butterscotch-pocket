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

#include <stdbool.h>
#include <stdint.h>
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

void ofReadLogNote(int kind, uint32_t size, uint64_t refusedNanos, uint64_t waitNanos) {
    uint64_t end = nowNanos();
    uint64_t start = end - refusedNanos - waitNanos;
    uint64_t since = g_readLastEnd != 0 && start > g_readLastEnd ? start - g_readLastEnd : 0;
    int after = g_readLast < 0 || since > 50000000ull ? OF_READ_AFTER_IDLE
              : g_readLast == OF_READLOG_TEX ? OF_READ_AFTER_TEX : OF_READ_AFTER_MUSIC;
    ofBenchNoteRead(kind, after, refusedNanos + waitNanos);
    if (g_readVerbose) {
        static const char *const afterNames[] = { "tex", "music", "idle" };
        unsigned refused = (unsigned) (refusedNanos / 100000u), wait = (unsigned) (waitNanos / 100000u);
        unsigned gap = (unsigned) (since / 100000u);
        logInfo("READ %s f%d %u B: refused %u.%u ms, wait %u.%u ms; after %s, %u.%u ms since\n",
                kind == OF_READLOG_TEX ? "tex" : "music", ofPlatformScriptFrame(), (unsigned) size,
                refused / 10, refused % 10, wait / 10, wait % 10, afterNames[after], gap / 10, gap % 10);
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

#endif
