#include "ut_bench.h"

#include "of.h"

#include "gettime.h"
#include "of_diag.h"
#include "of_perf.h"

#include <stdint.h>
#include <stdio.h>

/* Key presses that walk from boot to the first battle; see runInputScript()
 * in of_platform.c for the syntax. Frame numbers assume a fresh save state. */
static const char g_script[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z,700:R*400,1100:L*10,1120:U*400,1560:Z,1605:Z,1650:Z,1695:Z,1740:Z,1785:Z,1830:Z,1875:Z,1920:Z,1965:Z,2010:Z,2055:Z,2100:Z,2145:Z,2190:Z,2235:Z,2280:Z,2325:Z,2370:Z,2415:Z,2460:Z,2505:Z,2550:Z,2595:Z,2640:Z,2685:Z";

typedef struct {
    const char *name;
    int lastFrame;
} UtBenchSection;

/* Sections end on these frames; they follow the script above. */
static const UtBenchSection g_sections[] = {
    { "intro, menu, naming", 650 },
    { "first room 320x240", 1220 },
    { "Flowey talk 320x240", 2060 },
    { "Flowey battle (640x480)", 2600 },
};
#define UT_BENCH_SECTIONS ((int) (sizeof(g_sections) / sizeof(g_sections[0])))

static bool g_running = false;
static int g_frame = 0;
static int g_section = 0;
static uint64_t g_startNanos = 0;
static uint64_t g_sectionStart = 0;
static unsigned g_sectionMs[UT_BENCH_SECTIONS];
static uint64_t g_flipNanos = 0; /* time inside the display flip this section */
static unsigned g_sectionFlipMs[UT_BENCH_SECTIONS];
static unsigned g_firstFrameMs = 0;

void utBenchStart(void) {
    g_running = true;
    g_startNanos = nowNanos();
    utPlatformSetInputScript(g_script);
    utPlatformSetUncapped(true);
}

/* SD read throughput. Loading is dominated by file reads on the device, and
 * how fast they go depends on request size and on whether the destination is
 * 512-byte aligned (unaligned reads go through a bounce buffer in the OS). */
#define UT_IO_TOTAL (2u * 1024u * 1024u)
#define UT_IO_MAX_CHUNK (1024u * 1024u)

static void ioTest(const char *label, uint32_t chunk, uint32_t misalign) {
    static uint8_t buffer[UT_IO_MAX_CHUNK + 512] __attribute__((aligned(512)));

    FILE *file = fopen("data.win", "rb");
    if (file == NULL) return;
    setvbuf(file, NULL, _IONBF, 0);
    fseek(file, 1024 * 1024, SEEK_SET);

    uint64_t start = nowNanos();
    uint32_t done = 0;
    while (done < UT_IO_TOTAL) {
        size_t got = fread(buffer + misalign, 1, chunk, file);
        if (got == 0) break;
        done += (uint32_t) got;
    }
    uint64_t micros = (nowNanos() - start) / 1000u;
    fclose(file);

    unsigned kbPerSecond = micros > 0 ? (unsigned) ((uint64_t) done * 1000000u / 1024u / micros) : 0;
    utLogPrint("%-21s %6u KB/s\n", label, kbPerSecond);
}

static void ioReport(void) {
    utLogPrint("SD read speed (2 MB of data.win):\n");
    ioTest("4 KB reads", 4096, 0);
    ioTest("64 KB reads", 65536, 0);
    ioTest("1 MB reads", UT_IO_MAX_CHUNK, 0);
    ioTest("64 KB, unaligned", 65536, 4);
}

void utBenchAddFlipTime(uint64_t nanos) {
    if (g_running) g_flipNanos += nanos;
}

void utBenchFrame(void) {
    if (!g_running) return;

    uint64_t now = nowNanos();
    if (g_frame == 0) {
        g_firstFrameMs = (unsigned) ((now - g_startNanos) / 1000000u);
        g_sectionStart = now;
    }
    g_frame++;
    if (g_frame < g_sections[g_section].lastFrame) return;

    g_sectionMs[g_section] = (unsigned) ((now - g_sectionStart) / 1000000u);
    g_sectionFlipMs[g_section] = (unsigned) (g_flipNanos / 1000000u);
    g_sectionStart = now;
    g_flipNanos = 0;
    if (++g_section < UT_BENCH_SECTIONS) return;

    g_running = false;
    utLogPrint("=== Undertale benchmark ===\n");
#ifndef OF_PC
    {
        /* Say which OS and bitstream this ran on; tables look alike otherwise. */
        const struct of_capabilities *caps = of_get_caps();
        utLogPrint("OS %u.%u.%u, core variant %u, CPU %u MHz\n", (unsigned) ((caps->os_version >> 16) & 0xFF),
                   (unsigned) ((caps->os_version >> 8) & 0xFF), (unsigned) (caps->os_version & 0xFF),
                   (unsigned) caps->core_variant, (unsigned) (caps->cpu_freq_hz / 1000000u));
    }
#endif
    utLogPrint("load to first frame: %u.%u s\n", g_firstFrameMs / 1000, (g_firstFrameMs % 1000) / 100);
    utLogPrint("%s\n", utLogLoadSummary());
    utLogPrint("ms per frame:          work   total\n");
    int firstFrame = 0;
    unsigned totalMs = 0;
    for (int i = 0; i < UT_BENCH_SECTIONS; i++) {
        unsigned frames = (unsigned) (g_sections[i].lastFrame - firstFrame);
        unsigned total = g_sectionMs[i] * 10u / frames;
        unsigned work = (g_sectionMs[i] - g_sectionFlipMs[i]) * 10u / frames;
        utLogPrint("%-21s %3u.%u  %3u.%u\n", g_sections[i].name, work / 10, work % 10, total / 10, total % 10);
        firstFrame = g_sections[i].lastFrame;
        totalMs += g_sectionMs[i];
    }
    utLogPrint("%d frames in %u.%u s; full speed is 33.3\n", firstFrame, totalMs / 1000, (totalMs % 1000) / 100);
    utLogPrint("work = total minus display flip\n");
    ioReport();
    utDiagHalt("benchmark finished");
}
