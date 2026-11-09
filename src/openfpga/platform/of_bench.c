#include "of_bench.h"

#include "of.h"

#include "gettime.h"
#include "of_diag.h"
#include "of_game.h"
#include "of_bench_scene.h"
#include "of_perf.h"
#include "sw_call_notes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>


static bool g_running = false;
static int g_frame = 0;
static int g_section = 0;
static uint64_t g_startNanos = 0;
static uint64_t g_sectionStart = 0;
static int g_sectionFirstUpdate;
static int g_lastLiveUpdate = -1;
static uint64_t g_liveLastPaceEnd;
static bool g_liveFramePending, g_liveSkipped, g_liveSubmitted;
enum { LIVE_DRAWN, LIVE_SKIPPED, LIVE_KINDS };
typedef struct {
    unsigned count;
    uint64_t workNanos, waitNanos;
} LiveUpdateTimes;
static struct {
    unsigned updates, submitted, micros, rate;
    LiveUpdateTimes times[LIVE_KINDS];
} g_liveSections[OF_BENCH_MAX_SECTIONS];
static unsigned g_sectionMs[OF_BENCH_MAX_SECTIONS];
static bool g_sectionSeen[OF_BENCH_MAX_SECTIONS];
static uint64_t g_flipNanos = 0; /* time inside the display flip this section */
static unsigned g_sectionFlipMs[OF_BENCH_MAX_SECTIONS];
static OfPerfTotals g_sectionTotals[OF_BENCH_MAX_SECTIONS];
/* The draw calls each section spent most on (see sw_call_notes.h). */
#define OF_BENCH_NOTES 10
static SWCallNote g_sectionNotes[OF_BENCH_MAX_SECTIONS][OF_BENCH_NOTES];
static int g_sectionNoteCount[OF_BENCH_MAX_SECTIONS];
static uint32_t g_sectionNotesMissed[OF_BENCH_MAX_SECTIONS];
static SWPipeRasterNotes g_sectionPipeRaster[OF_BENCH_MAX_SECTIONS];
static unsigned g_firstFrameMs = 0;

/* Draw calls and mixing are timed on one frame in this many: timing them
 * costs 3 to 5 ms of a battle frame on the device, which the game does not
 * pay, so the frame times come out about half a millisecond high. The tables
 * of where the time went are averages over the timed frames. */
#define OF_BENCH_DETAIL_EVERY 8

/* The run being measured: the game's benchmark, or its battle tour. */
static struct {
    const char *title;
    const OfBenchSection *sections;
    int sectionCount;
    bool tour;
    bool runProbes;
    bool livePacing;
} g_def;

static void benchStart(const char *title, const char *script, const OfJump *jumps, int jumpCount,
                       const OfBenchSection *sections, int sectionCount, bool tour, bool livePacing) {
    for (int i = 0; i < OF_BENCH_MAX_SECTIONS; ++i) g_sectionSeen[i] = false;
    g_def.title = title;
    g_def.sections = sections;
    g_def.sectionCount = sectionCount;
    g_def.tour = tour;
    g_def.runProbes = !livePacing;
    g_def.livePacing = livePacing;
    if (g_def.sectionCount > OF_BENCH_MAX_SECTIONS) ofDiagHalt("benchmark has more sections than OF_BENCH_MAX_SECTIONS");
    g_running = true;
    g_startNanos = nowNanos();
    ofPerfDetailEvery(livePacing ? 0 : OF_BENCH_DETAIL_EVERY);
    ofPlatformSetInputScript(script);
    if (jumpCount > 0) ofPlatformSetJumps(jumps, jumpCount);
    ofPlatformSetUncapped(!livePacing);
    ofSaveFsSetVolatile(true);
}

void ofBenchStart(void) {
    benchStart(ofGame.benchTitle, ofGame.benchScript, ofGame.benchJumps, ofGame.benchJumpCount,
               ofGame.benchSections, ofGame.benchSectionCount, false, false);
}

bool ofBenchStartTour(void) {
    if (ofGame.tourScript == NULL) return false;
    benchStart(ofGame.tourTitle, ofGame.tourScript, ofGame.tourJumps, ofGame.tourJumpCount,
               ofGame.tourSections, ofGame.tourSectionCount, true, false);
    return true;
}

bool ofBenchStartBulletTour(void) {
    if (ofGame.bulletScript == NULL) return false;
    benchStart(ofGame.bulletTitle, ofGame.bulletScript, ofGame.bulletJumps, ofGame.bulletJumpCount,
               ofGame.bulletSections, ofGame.bulletSectionCount, true, false);
    return true;
}

bool ofBenchStartChaosSaber(void) {
    if (ofGame.saberScript == NULL) return false;
    benchStart(ofGame.saberTitle, ofGame.saberScript, ofGame.saberJumps, ofGame.saberJumpCount,
               ofGame.saberSections, ofGame.saberSectionCount, true, false);
    g_def.runProbes = false;
    return true;
}

bool ofBenchStartFloweyTour(bool livePacing) {
    if (ofGame.floweyScript == NULL) return false;
    benchStart(ofGame.floweyTitle, ofGame.floweyScript, ofGame.floweyJumps, ofGame.floweyJumpCount,
               ofGame.floweySections, ofGame.floweySectionCount, true, livePacing);
    return true;
}

bool ofBenchLivePacing(void) {
    return g_def.livePacing;
}

/* SD read throughput, cold and warm.
 *
 * Earlier versions of this test read the same 2 MB of data.win several times
 * and reported 13 MB/s for everything but the first pass, while the loader,
 * reading the file for the first time, managed about 1.2 MB/s. That pattern
 * says repeat reads are served from a cache somewhere below the app. So each
 * test here reads its own, previously untouched megabyte of music.bin (far
 * larger than anything played during the benchmark), and one region is read
 * twice to show the warm figure. */
#define OF_IO_BYTES (1024u * 1024u)
#define OF_IO_MB(n) ((n) / ofGame.ioDivisor)
#define OF_IO_ASYNC_MB(i) (ofGame.ioAsyncBase + ofGame.ioAsyncStep * (i))
#define OF_IO_FILE "music.bin"

static unsigned ioTest(uint32_t megabyteOffset, uint32_t chunk) {
    uint8_t *buffer = malloc(OF_IO_BYTES);
    if (buffer == NULL) return 0;

    FILE *file = fopen(OF_IO_FILE, "rb");
    if (file == NULL) {
        free(buffer);
        return 0;
    }
    setvbuf(file, NULL, _IONBF, 0);
    fseek(file, (long) megabyteOffset * 1024 * 1024, SEEK_SET);

    uint64_t start = nowNanos();
    uint32_t done = 0;
    while (done < OF_IO_BYTES) {
        size_t got = fread(buffer, 1, chunk, file);
        if (got == 0) break;
        done += (uint32_t) got;
    }
    uint64_t micros = (nowNanos() - start) / 1000u;
    fclose(file);
    free(buffer);

    return micros > 0 ? (unsigned) ((uint64_t) done * 1000000u / 1024u / micros) : 0;
}

static void ioReport(void) {
    /* Offsets are spread through the second half of the pack. */
    ofLogPrint("SD read, KB/s, 1 MB each from " OF_IO_FILE ":\n");
    ofLogPrint("cold: 4K %u, 64K %u, 1M %u\n", ioTest(OF_IO_MB(70u), 4096), ioTest(OF_IO_MB(85u), 65536), ioTest(OF_IO_MB(100u), OF_IO_BYTES));
    ofLogPrint("same 64K region again: %u\n", ioTest(OF_IO_MB(85u), 65536));
}

/* The same pack read with the OS's non-blocking call instead of fread.
 * fread is served from the OS file cache, which fetches one 32 KB block per
 * command to the Pocket's host however much was asked for, and each command
 * has a round trip of its own. This times single commands of several sizes,
 * to show how much of a read is that round trip and how much is data, and
 * then a megabyte in the largest commands the OS accepts: into the OS's
 * staging memory (no copy) and into ordinary memory (one copy). */
#ifndef OF_PC
#define OF_ASYNC_TIMEOUT_NANOS 3000000000ull
#define OF_ASYNC_READS_PER_SIZE 8

static volatile int g_asyncDone;
static volatile int g_asyncResult;

/* Runs from the read-completion interrupt. */
static void asyncCallback(int token, int result) {
    (void) token;
    g_asyncResult = result;
    g_asyncDone = 1;
}

/* Reads `total` bytes from `offset` in commands of `length`. Returns the
 * time in microseconds, or 0 if a read failed or timed out. */
static uint32_t asyncTime(uint32_t slot, uint32_t offset, uint8_t *dest, bool advanceDest, uint32_t length, uint32_t total) {
    uint64_t start = nowNanos();
    for (uint32_t done = 0; done < total; done += length) {
        g_asyncDone = 0;
        g_asyncResult = 0;
        uint64_t issued = nowNanos();
        if (of_file_read_async((int) slot, offset + done, advanceDest ? dest + done : dest, length, asyncCallback) < 0) return 0;
        /* The poll is the OS's fallback for a lost interrupt. */
        while (!g_asyncDone && of_file_async_poll() != 1) {
            if (nowNanos() - issued > OF_ASYNC_TIMEOUT_NANOS) return 0;
        }
        if (g_asyncResult < 0) return 0;
    }
    return (uint32_t) ((nowNanos() - start) / 1000u);
}

static void asyncReport(void) {
    static const uint32_t sizes[] = { 4096, 16384, 32768, 65536 };
    char line[64];
    int at;

    uint32_t slot = 0;
    uint32_t maxRead = of_file_async_max_read();
    uint8_t *stage = maxRead > 0 ? of_file_dma_stage_alloc(maxRead, 4096) : NULL;
    if (stage == NULL || of_file_slot_find(OF_IO_FILE, &slot) != 0) {
        ofLogPrint("async read: not available (max %u)\n", (unsigned) maxRead);
        return;
    }

    at = snprintf(line, sizeof(line), "async max %uK ms:", (unsigned) (maxRead / 1024u));
    for (int i = 0; i < 4; i++) {
        if (sizes[i] > maxRead) continue;
        /* Each size gets its own untouched stretch of the pack. */
        uint32_t micros = asyncTime(slot, OF_IO_ASYNC_MB((uint32_t) i) * 1024u * 1024u, stage, false, sizes[i],
                                    sizes[i] * OF_ASYNC_READS_PER_SIZE);
        unsigned tenths = micros / (100u * OF_ASYNC_READS_PER_SIZE);
        at += snprintf(line + at, sizeof(line) - (size_t) at, " %uK %u.%u", (unsigned) (sizes[i] / 1024u), tenths / 10, tenths % 10);
    }
    ofLogPrint("%s\n", line);

    uint32_t total = OF_IO_BYTES - OF_IO_BYTES % maxRead;
    uint32_t staged = asyncTime(slot, OF_IO_MB(118u) * 1024u * 1024u, stage, false, maxRead, total);
    uint8_t *heapBuffer = malloc(OF_IO_BYTES);
    uint32_t copied = heapBuffer != NULL ? asyncTime(slot, OF_IO_MB(120u) * 1024u * 1024u, heapBuffer, true, maxRead, total) : 0;
    free(heapBuffer);
    ofLogPrint("async 1M KB/s: staged %u, copied %u\n",
               staged > 0 ? (unsigned) ((uint64_t) total * 1000000u / 1024u / staged) : 0,
               copied > 0 ? (unsigned) ((uint64_t) total * 1000000u / 1024u / copied) : 0);
}
#else
static void asyncReport(void) {}
#endif

void ofBenchAddFlipTime(uint64_t nanos, bool submitted) {
    if (!g_running) return;
    if (g_def.livePacing) g_liveSubmitted = submitted;
    else g_flipNanos += nanos;
}

static unsigned pipeMicros(const SWPipeRasterNotes *notes, SWRasterPath path) {
    unsigned calls = notes->calls[path];
    return calls ? (unsigned) (notes->nanos[path] / ((uint64_t) calls * 1000u)) : 0;
}

static void reportWork(void);

static void reportLive(void) {
    ofLogPrint("Full speed: 30 updates/s\n");
    ofLogPrint("phase upd/s updates submit skips other   time-us\n");
    for (int i = 0; i < g_def.sectionCount; ++i) {
        unsigned updates = g_liveSections[i].updates;
        unsigned skipped = g_liveSections[i].times[LIVE_SKIPPED].count;
        unsigned submitted = g_liveSections[i].submitted;
        unsigned rate = g_liveSections[i].rate;
        ofLogPrint("%-5s %3u.%u %7u %6u %5u %5u %9u\n", g_def.sections[i].tag,
                   rate / 10, rate % 10, updates, submitted, skipped,
                   updates - submitted - skipped, g_liveSections[i].micros);
    }
    ofLogPrint("submit = frames sent to display\n");
    ofLogPrint("skips = adaptive; other includes room changes\n");
    ofLogPrint("ms/update    count   work   wait  total\n");
    for (int i = 0; i < g_def.sectionCount; ++i) {
        for (int kind = 0; kind < LIVE_KINDS; ++kind) {
            const LiveUpdateTimes *t = &g_liveSections[i].times[kind];
            const char *tag = g_def.sections[i].tag;
            const char *label = kind == LIVE_SKIPPED ? "skip" : "drawn";
            if (t->count == 0) {
                ofLogPrint("%-5s %-5s %5u %6s %6s %6s\n", tag, label, t->count, "--", "--", "--");
                continue;
            }
            uint64_t unit = (uint64_t) t->count * 100000u;
            unsigned work = (unsigned) ((t->workNanos + unit / 2) / unit);
            unsigned wait = (unsigned) ((t->waitNanos + unit / 2) / unit);
            unsigned total = (unsigned) ((t->workNanos + t->waitNanos + unit / 2) / unit);
            ofLogPrint("%-5s %-5s %5u %4u.%u %4u.%u %4u.%u\n", tag, label, t->count,
                       work / 10, work % 10, wait / 10, wait % 10, total / 10, total % 10);
        }
    }
    ofLogPrint("wait = time in the frame limiter\n");
}

static void advanceBenchFrame(Runner *runner, uint64_t now) {
    const OfBenchSection *section = &g_def.sections[g_section];
    if (!g_sectionSeen[g_section] && ofBenchSceneMatches(runner, section)) g_sectionSeen[g_section] = true;
    if (g_frame == 0) {
        g_firstFrameMs = (unsigned) ((now - g_startNanos) / 1000000u);
        g_sectionStart = now;
        g_sectionFirstUpdate = runner->frameCount;
        if (!g_def.livePacing) {
            ofPerfTakeTotals(NULL); /* loading is not part of the first section */
            swrCallNotes = true;
            swrCallNotesTake(NULL, 0, NULL);
            swrPipeRasterTake(NULL);
        }
    }
    g_frame++;
    if (g_frame < section->lastFrame) return;

    bool seen = (section->roomName == NULL && section->requiredObject == NULL) || g_sectionSeen[g_section];
    bool endValid = section->endValid == NULL ||
                    (ofBenchSceneMatches(runner, section) && section->endValid(runner));
    if (!seen || !endValid) {
        ofLogPrint("BENCH FAIL %s: %s\n", section->name,
                   seen ? "required scene not active at end" : "required scene not observed");
#ifdef OF_PC
        ofLogDump();
        exit(EXIT_FAILURE);
#else
        ofDiagHalt("benchmark scene check failed");
#endif
    }
    uint64_t elapsed = now - g_sectionStart;
    if (g_def.livePacing) {
        // The swap-indexed attack windows stay aligned with the uncapped tour.
        // Steps on room transitions still count as updates in the wall interval.
        unsigned updates = (unsigned) (runner->frameCount - g_sectionFirstUpdate);
        g_liveSections[g_section].updates = updates;
        g_liveSections[g_section].micros = (unsigned) (elapsed / 1000u);
        g_liveSections[g_section].rate = elapsed > 0 ?
            (unsigned) ((updates * 10000000000ull + elapsed / 2u) / elapsed) : 0;
        g_sectionFirstUpdate = runner->frameCount;
    } else {
        g_sectionMs[g_section] = (unsigned) (elapsed / 1000000u);
        g_sectionFlipMs[g_section] = (unsigned) (g_flipNanos / 1000000u);
        ofPerfTakeTotals(&g_sectionTotals[g_section]);
        g_sectionNoteCount[g_section] = swrCallNotesTake(g_sectionNotes[g_section], OF_BENCH_NOTES, &g_sectionNotesMissed[g_section]);
        swrPipeRasterTake(&g_sectionPipeRaster[g_section]);
    }
    g_sectionStart = now;
    g_flipNanos = 0;
    if (++g_section < g_def.sectionCount) return;

    g_running = false;
    swrCallNotes = false;
    ofPerfDetailEvery(0);
    /* The title can identify the build or its optimisation flags. */
    const char *note = "";
#ifdef OF_BUILD_NOTE
    note = OF_BUILD_NOTE;
#endif
    char drawn[40] = "";
    if (ofPlatformForcedSkip() > 0) snprintf(drawn, sizeof(drawn), ", drawing 1 frame in %d", ofPlatformForcedSkip());
    ofLogPrint("=== %s benchmark, %s%s%s%s%s ===\n", g_def.title, ofPlatformSpeedMode() ? "speed" : "accuracy",
               g_def.livePacing ? ", live pacing" : "", note[0] != '\0' ? ", " : "", note, drawn);
#ifndef OF_PC
    {
        /* Say which OS and bitstream this ran on; tables look alike otherwise. */
        const struct of_capabilities *caps = of_get_caps();
        ofLogPrint("OS %u.%u.%u, core variant %u, CPU %u MHz\n", (unsigned) ((caps->os_version >> 16) & 0xFF),
                   (unsigned) ((caps->os_version >> 8) & 0xFF), (unsigned) (caps->os_version & 0xFF),
                   (unsigned) caps->core_variant, (unsigned) (caps->cpu_freq_hz / 1000000u));
    }
#endif
    ofLogPrint("load to first frame: %u.%u s\n", g_firstFrameMs / 1000, (g_firstFrameMs % 1000) / 100);
    ofLogPrint("%s\n", ofLogLoadSummary());
    ofLogPrint("%s\n", ofLogLoadPhases());
    if (g_def.livePacing) reportLive();
    else reportWork();
    ofLogPrint("--- end of report ---\n"); /* ofLogDump keeps a report from its title to this line */
    ofLogDump();
    ofDiagHalt("benchmark finished");
}

void ofBenchFrame(Runner *runner, bool skipped) {
    if (!g_running) return;
    if (g_def.livePacing) {
        g_liveFramePending = true;
        g_liveSkipped = skipped;
        g_liveSubmitted = false;
    } else {
        advanceBenchFrame(runner, nowNanos());
    }
}

void ofBenchPacedFrame(Runner *runner, uint64_t waitStart, uint64_t waitEnd) {
    if (!g_running || !g_def.livePacing) return;
    uint64_t updateStart = g_liveLastPaceEnd;
    // Room changes can run an update without reaching the swap path. Move
    // the boundary anyway so the following sample excludes that work.
    g_liveLastPaceEnd = waitEnd;
    if (!g_liveFramePending) return;
    g_liveFramePending = false;
    if (!ofPlatformSpeedMode()) ofDiagHalt("Flowey live pacing requires Speed mode");
    if (runner->frameCount <= g_lastLiveUpdate) return;
    g_lastLiveUpdate = runner->frameCount;
    if (g_frame != 0) {
        LiveUpdateTimes *t = &g_liveSections[g_section].times[g_liveSkipped ? LIVE_SKIPPED : LIVE_DRAWN];
        ++t->count;
        t->workNanos += waitStart - updateStart;
        t->waitNanos += waitEnd - waitStart;
        g_liveSections[g_section].submitted += g_liveSubmitted;
    }
    advanceBenchFrame(runner, waitEnd);
}

static void reportWork(void) {
    ofLogPrint("ms per frame:          work   total\n");
    int firstFrame = 0;
    unsigned totalMs = 0;
    for (int i = 0; i < g_def.sectionCount; i++) {
        unsigned frames = (unsigned) (g_def.sections[i].lastFrame - firstFrame);
        unsigned total = g_sectionMs[i] * 10u / frames;
        unsigned work = (g_sectionMs[i] - g_sectionFlipMs[i]) * 10u / frames;
        ofLogPrint("%-21s %3u.%u  %3u.%u\n", g_def.sections[i].name, work / 10, work % 10, total / 10, total % 10);
        firstFrame = g_def.sections[i].lastFrame;
        totalMs += g_sectionMs[i];
    }
    ofLogPrint("%d frames in %u.%u s; full speed is 33.3\n", firstFrame, totalMs / 1000, (totalMs % 1000) / 100);
    ofLogPrint("work = total minus display flip\n");
    if (g_def.runProbes) {
        ioReport();
        asyncReport();
        unsigned clock, draw, drawUntimed, audioFree;
        ofPlatformCallCosts(&clock, &draw, &drawUntimed, &audioFree);
        ofLogPrint("call costs, us: clock %u.%02u, skipped sprite draw %u.%02u (%u.%02u without its timing), audio queue %u.%02u\n",
                   clock / 1000, clock % 1000 / 10, draw / 1000, draw % 1000 / 10, drawUntimed / 1000, drawUntimed % 1000 / 10,
                   audioFree / 1000, audioFree % 1000 / 10);
    }
    /* Where each section's time went, in ms per frame. step: game code; draw:
     * all drawing, of which the next five are sprites, sprite parts and
     * tiles, text, tiled backgrounds and rectangles (what is left of draw is
     * none of those: clears, held layers let out by another kind of draw,
     * the runner's own work); snd: the audio update; out: presenting. The
     * second table is how many of each kind of draw call a frame makes.
     * The five kinds are averages over the frames timed in detail. */
    #define TENTHS(nanos, frames) (unsigned) ((nanos) / ((uint64_t) (frames) * 1000000u)), \
                                  (unsigned) ((nanos) / ((uint64_t) (frames) * 100000u) % 10u)
    ofLogPrint("ms/fr  step   draw =  spr   til   txt   bkg   rct   snd   out\n");
    firstFrame = 0;
    for (int i = 0; i < g_def.sectionCount; i++) {
        const OfPerfTotals *t = &g_sectionTotals[i];
        int frames = g_def.sections[i].lastFrame - firstFrame;
        int detail = t->detailFrames > 0 ? (int) t->detailFrames : 1;
        ofLogPrint("%-5s %3u.%u %4u.%u  %3u.%u %3u.%u %3u.%u %3u.%u %3u.%u %3u.%u %3u.%u\n", g_def.sections[i].tag,
                   TENTHS(t->phaseNanos[OF_PHASE_STEP], frames), TENTHS(t->phaseNanos[OF_PHASE_DRAW], frames),
                   TENTHS(t->drawNanos[0], detail), TENTHS(t->drawNanos[1], detail), TENTHS(t->drawNanos[2], detail),
                   TENTHS(t->drawNanos[3], detail), TENTHS(t->drawNanos[4], detail),
                   TENTHS(t->phaseNanos[OF_PHASE_AUDIO], frames), TENTHS(t->phaseNanos[OF_PHASE_OUT], frames));
        firstFrame = g_def.sections[i].lastFrame;
    }
    /* Sound, in ms per frame wherever in the frame it ran (the draw profiler
     * feeds the queue too, so this is not the snd column): mix is all of
     * mixing, of which voices is decoding and summing them and write is
     * handing the samples to the OS; music is reading it from the card; fx
     * is loading sound effects. Then output sample pairs per frame, and how
     * many voices were playing on average, in tenths. */
    ofLogPrint("sound   mix = voices write  music    fx  pairs voices\n");
    firstFrame = 0;
    for (int i = 0; i < g_def.sectionCount; i++) {
        const OfPerfTotals *t = &g_sectionTotals[i];
        int frames = g_def.sections[i].lastFrame - firstFrame;
        int detail = t->detailFrames > 0 ? (int) t->detailFrames : 1;
        uint64_t pairs = t->loadNanos[OF_LOAD_MIX_PAIRS];
        unsigned voices = pairs > 0 ? (unsigned) (t->loadNanos[OF_LOAD_MIX_VOICE_PAIRS] * 10u / pairs) : 0;
        ofLogPrint("%-5s %3u.%u   %3u.%u %3u.%u  %3u.%u %3u.%u  %5u  %2u.%u\n", g_def.sections[i].tag,
                   TENTHS(t->loadNanos[OF_LOAD_MIX], detail), TENTHS(t->loadNanos[OF_LOAD_MIX_VOICES], detail),
                   TENTHS(t->loadNanos[OF_LOAD_MIX_WRITE], detail), TENTHS(t->loadNanos[OF_LOAD_MUSIC], frames),
                   TENTHS(t->loadNanos[OF_LOAD_SOUND], frames), (unsigned) (pairs / (uint64_t) frames), voices / 10, voices % 10);
        firstFrame = g_def.sections[i].lastFrame;
    }
    #undef TENTHS
    ofLogPrint("calls/fr      spr   til   txt   bkg   rct\n");
    firstFrame = 0;
    for (int i = 0; i < g_def.sectionCount; i++) {
        const OfPerfTotals *t = &g_sectionTotals[i];
        uint64_t frames = (uint64_t) (g_def.sections[i].lastFrame - firstFrame);
        ofLogPrint("%-5s       %5u %5u %5u %5u %5u\n", g_def.sections[i].tag, (unsigned) (t->drawCalls[0] / frames),
                   (unsigned) (t->drawCalls[1] / frames), (unsigned) (t->drawCalls[2] / frames),
                   (unsigned) (t->drawCalls[3] / frames), (unsigned) (t->drawCalls[4] / frames));
        firstFrame = g_def.sections[i].lastFrame;
    }
    for (int i = 0; i < g_def.sectionCount; i++) {
        const SWPipeRasterNotes *notes = &g_sectionPipeRaster[i];
        if (!(notes->calls[SWR_RASTER_WARM] || notes->calls[SWR_RASTER_BUILT] ||
              notes->calls[SWR_RASTER_PIVOT] || notes->calls[SWR_RASTER_OTHER])) continue;
        ofLogPrint("%s 45x31 raster: warm %u/%uus build %u/%uus\n", g_def.sections[i].tag,
                   (unsigned) notes->calls[SWR_RASTER_WARM], pipeMicros(notes, SWR_RASTER_WARM),
                   (unsigned) notes->calls[SWR_RASTER_BUILT], pipeMicros(notes, SWR_RASTER_BUILT));
        ofLogPrint("  pivot %u/%uus other %u/%uus\n",
                   (unsigned) notes->calls[SWR_RASTER_PIVOT], pipeMicros(notes, SWR_RASTER_PIVOT),
                   (unsigned) notes->calls[SWR_RASTER_OTHER], pipeMicros(notes, SWR_RASTER_OTHER));
    }
    /* For the last few sections, the draw calls that cost the most, grouped
     * by what was asked for: ms per frame, calls per frame (in tenths), what.
     * "of which" lines are parts of other calls and are counted in those too. */
    firstFrame = 0;
    for (int i = 0; i < g_def.sectionCount; i++) {
        /* Notes are only taken on the frames timed in detail. */
        uint64_t frames = g_sectionTotals[i].detailFrames > 0 ? g_sectionTotals[i].detailFrames : 1;
        firstFrame = g_def.sections[i].lastFrame;
        if ((!g_def.tour && i < g_def.sectionCount - 4) || g_sectionNoteCount[i] == 0) continue;
        ofLogPrint("%s: costliest draws, ms/fr calls/fr (%u calls dropped from the list)\n", g_def.sections[i].tag, (unsigned) g_sectionNotesMissed[i]);
        for (int n = 0; n < g_sectionNoteCount[i]; n++) {
            const SWCallNote *draw = &g_sectionNotes[i][n];
            unsigned tenths = (unsigned) (draw->nanos / (frames * 100000u));
            unsigned callTenths = (unsigned) ((uint64_t) draw->calls * 10u / frames);
            ofLogPrint(" %3u.%u %3u.%u %s\n", tenths / 10, tenths % 10, callTenths / 10, callTenths % 10, draw->what);
        }
    }
    if (g_def.runProbes) {
        /* What a direct read costs by what read came just before it (ofPlatformReadCosts), ms. */
        unsigned tenths[OF_READ_COSTS];
        ofPlatformReadCosts(tenths);
        #define MS(n) tenths[n] / 10, tenths[n] % 10
        ofLogPrint("4 KB read, ms: tex>tex %u.%u music>tex %u.%u tex>music %u.%u music>music %u.%u idle>tex %u.%u data.win>tex %u.%u\n",
                   MS(OF_READ_TEX_TEX), MS(OF_READ_MUSIC_TEX), MS(OF_READ_TEX_MUSIC), MS(OF_READ_MUSIC_MUSIC), MS(OF_READ_IDLE_TEX),
                   MS(OF_READ_FILE_TEX));
        #undef MS
        /* Save the report so far before probing outside a frame. */
        ofLogDump();
        /* A sprite draw cut short at each stage in turn (sw_call_notes.h), microseconds a call:
         * what it has cost by the renderer's door, with its texture found, on entering the
         * drawing code, clipped, set up, with held layers let out, and whole. */
        int width, height, path;
        unsigned at[7], outside;
        ofPlatformSpriteCosts(&width, &height, at, &outside, &path);
        #define US(n) (n) / 1000, (n) % 1000 / 10
        ofLogPrint("sprite %dx%d draw, us, way %d: door %u.%02u texture %u.%02u entered %u.%02u\n", width, height, path,
                   US(at[1]), US(at[2]), US(at[3]));
        ofLogPrint("  clipped %u.%02u set up %u.%02u let out %u.%02u whole %u.%02u; out of view %u.%02u\n",
                   US(at[4]), US(at[5]), US(at[6]), US(at[0]), US(outside));
        #undef US
    }
}
