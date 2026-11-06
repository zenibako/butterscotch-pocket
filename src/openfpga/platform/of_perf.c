/*
 * Frame-time overlay and log overlay (both switched on in the menu; R also
 * toggles the log with the debug buttons on), and the words in the top right
 * corner: which way L's speed/accuracy toggle is set, or a notice.
 *
 * Shows four numbers in the top-left corner over the last 30 frames, the
 * first three in milliseconds:
 *   average work time (frame period minus time spent sleeping for pacing)
 *   worst work time
 *   worst frame period (33 at full speed for a 30 fps game)
 *   frames skipped (speed mode's frame skipping; 0 in accuracy mode)
 */

#include "of_perf.h"

#include "debug_font.h"
#include "of_font.h"
#include "gettime.h"
#include "log.h"
#include "profiler.h"
#include "stb_ds.h"

#include <string.h>

#define OF_PERF_WINDOW 30
#define OF_PERF_SCALE  2

static bool g_enabled = false;
static bool g_logEnabled = false;
static uint64_t g_lastFrame = 0;
static uint64_t g_sleepNanos = 0;
static unsigned g_worstWork = 0, g_worstPeriod = 0, g_totalWork = 0;
static unsigned g_shownWork = 0, g_shownPeriod = 0, g_shownAverage = 0;
static int g_count = 0;
static unsigned g_skipped = 0, g_shownSkipped = 0;
static uint64_t g_loadNanos[OF_LOAD_KINDS];
static uint64_t g_phaseNanos[OF_PHASES];
static uint64_t g_drawNanos[OF_DRAW_KINDS];
static unsigned g_drawCalls[OF_DRAW_KINDS];
static OfPerfTotals g_totals;
static OfPhase g_phase = OF_PHASE_OTHER;
static uint64_t g_phaseStart = 0;

/* 3x5 digit glyphs, one row per 3 bits, top row first. */
static const uint16_t g_digits[10] = {
    075557, 022222, 071747, 071717, 055711,
    074717, 074757, 071111, 075757, 075717,
};

void ofPerfAddLoad(OfLoadKind kind, uint64_t nanos) {
    g_loadNanos[kind] += nanos;
}

bool ofPerfDetail = true;
static int g_detailEvery = 0;
static bool g_detailAlways = false;
static unsigned g_frameNumber = 0;
extern bool swrDrawTimed;

void ofPerfDetailEvery(int frames) {
    g_detailEvery = frames;
}

void ofPerfDetailAlways(bool always) {
    g_detailAlways = always;
}

/* A draw call that was not timed. The audio queue is topped up by count
 * here, there being no time to go by; asking how full it is costs half a
 * microsecond. */
void platformDrawCount(int kind) {
    g_drawCalls[kind]++;
    static unsigned sinceTick = 0;
    if (++sinceTick >= 8) {
        sinceTick = 0;
        platformBusyTick();
    }
}

void platformDrawProfile(int kind, uint64_t nanos) {
    g_drawNanos[kind] += nanos;
    g_drawCalls[kind]++;

    /* Top the audio queue up every few milliseconds of drawing. */
    static uint64_t sinceTick = 0;
    sinceTick += nanos;
    if (sinceTick >= OF_DRAW_TICK_NANOS) {
        sinceTick = 0;
        platformBusyTick();
    }
}

void ofPerfPhase(OfPhase phase) {
    uint64_t now = nowNanos();
    if (g_phaseStart != 0) g_phaseNanos[g_phase] += now - g_phaseStart;
    g_phase = phase;
    g_phaseStart = now;
}

static void reportSlowFrame(unsigned workMs) {
    if (workMs >= OF_PERF_SLOW_FRAME_MS) {
        unsigned load[OF_LOAD_KINDS], phase[OF_PHASES];
        for (int i = 0; i < OF_LOAD_KINDS; i++) load[i] = (unsigned) (g_loadNanos[i] / 1000000u);
        for (int i = 0; i < OF_PHASES; i++) phase[i] = (unsigned) (g_phaseNanos[i] / 1000000u);
        logInfo("slow %u: step %u draw %u out %u snd %u\n", workMs, phase[OF_PHASE_STEP], phase[OF_PHASE_DRAW],
                phase[OF_PHASE_OUT], phase[OF_PHASE_AUDIO]);
        logInfo("  load: rm %u tx %u fx %u mix %u mus %u\n", load[OF_LOAD_ROOM], load[OF_LOAD_TEXTURE],
                load[OF_LOAD_SOUND], load[OF_LOAD_MIX], load[OF_LOAD_MUSIC]);
        unsigned draw[OF_DRAW_KINDS];
        for (int i = 0; i < OF_DRAW_KINDS; i++) draw[i] = (unsigned) (g_drawNanos[i] / 1000000u);
        /* Calls and ms per kind; the ms, like mix above, are zero on a frame taken without detail. */
        logInfo("  draw: s%u/%u p%u/%u t%u/%u b%u/%u r%u/%u%s\n", g_drawCalls[0], draw[0], g_drawCalls[1], draw[1],
                g_drawCalls[2], draw[2], g_drawCalls[3], draw[3], g_drawCalls[4], draw[4], ofPerfDetail ? "" : " (not timed)");
    }
    g_totals.frames++;
    if (ofPerfDetail) g_totals.detailFrames++;
    for (int i = 0; i < OF_LOAD_KINDS; i++) {
        g_totals.loadNanos[i] += g_loadNanos[i];
        g_loadNanos[i] = 0;
    }
    for (int i = 0; i < OF_PHASES; i++) {
        g_totals.phaseNanos[i] += g_phaseNanos[i];
        g_phaseNanos[i] = 0;
    }
    for (int i = 0; i < OF_DRAW_KINDS; i++) {
        g_totals.drawNanos[i] += g_drawNanos[i];
        g_totals.drawCalls[i] += g_drawCalls[i];
        g_drawNanos[i] = 0;
        g_drawCalls[i] = 0;
    }
}

void ofPerfPeekTotals(OfPerfTotals *out) {
    *out = g_totals;
}

void ofPerfTakeTotals(OfPerfTotals *out) {
    if (out != NULL) *out = g_totals;
    memset(&g_totals, 0, sizeof(g_totals));
}

void ofPerfToggle(void) {
    g_enabled = !g_enabled;
}

void ofPerfToggleLog(void) {
    g_logEnabled = !g_logEnabled;
}

void ofPerfShowLog(void) {
    g_logEnabled = true;
}

/* Butterscotch's own report is a line per script too wide for a 320-pixel
 * log, so this writes its own: ms per frame and instructions per frame for
 * the heaviest few, names without the "gml_Object_"/"gml_Script_" prefix. */
#define OF_SCRIPT_TOP 20 /* scripts and the built-in functions they call share the list */

void ofPerfScriptReport(const Profiler *profiler, int frames) {
    if (profiler == NULL || frames <= 0) return;
    int count = (int) shlen(profiler->entries);
    uint64_t totalNanos = 0, totalOps = 0;
    int top[OF_SCRIPT_TOP];
    int shown = 0;
    for (int i = 0; i < count; i++) {
        uint64_t nanos = profiler->entries[i].value.nanos;
        totalNanos += nanos;
        totalOps += profiler->entries[i].value.ops;
        int at = shown;
        while (at > 0 && profiler->entries[top[at - 1]].value.nanos < nanos) at--;
        if (at >= OF_SCRIPT_TOP) continue;
        if (shown < OF_SCRIPT_TOP) shown++;
        for (int j = shown - 1; j > at; j--) top[j] = top[j - 1];
        top[at] = i;
    }

    /* Tenths of a millisecond per frame, in integers: no float printf here. */
    unsigned perFrame = (unsigned) (totalNanos / 100000u / (unsigned) frames);
    logInfo("scripts %u.%u ms %u ops /frame (%d, %d fr)\n", perFrame / 10, perFrame % 10,
            (unsigned) (totalOps / (unsigned) frames), count, frames);
    for (int i = 0; i < shown; i++) {
        const ProfilerEntry *entry = &profiler->entries[top[i]];
        const char *name = entry->key;
        if (strncmp(name, "gml_Object_", 11) == 0 || strncmp(name, "gml_Script_", 11) == 0) name += 11;
        perFrame = (unsigned) (entry->value.nanos / 100000u / (unsigned) frames);
        /* ms, instructions and calls per frame (calls in tenths). A built-in function has no instructions. */
        unsigned callTenths = (unsigned) (entry->value.calls * 10u / (unsigned) frames);
        logInfo(" %2u.%u %5u %3u.%u %s\n", perFrame / 10, perFrame % 10, (unsigned) (entry->value.ops / (unsigned) frames),
                callTenths / 10, callTenths % 10, name);
    }
}

bool ofPerfOverlayOn(void) {
    return g_enabled;
}

bool ofPerfLogOn(void) {
    return g_logEnabled;
}

void ofPerfShown(unsigned *average, unsigned *worstWork, unsigned *worstPeriod, unsigned *skipped) {
    *average = g_shownAverage;
    *worstWork = g_shownWork;
    *worstPeriod = g_shownPeriod;
    *skipped = g_shownSkipped;
}

void ofPerfRestartClock(void) {
    g_lastFrame = 0;
    g_sleepNanos = 0;
}

void ofPerfHideOverlays(void) {
    g_enabled = false;
    g_logEnabled = false;
}

/* Log overlay text: Butterscotch's debug font atlas shrunk 3:1 by averaging
 * coverage, which gives a 6x12 cell (OF_LOG_CELL_W, OF_LOG_CELL_H) that still
 * reads at 320x240. */
#define OF_LOG_SHRINK 3

static void drawLogChar(uint16_t *fb, int width, int x, int y, char c, uint16_t color) {
    if (c < DEBUGFONT_FIRST_CP || c > DEBUGFONT_LAST_CP) return;
    const DebugFontGlyphEntry *glyph = &debugFontGlyphs[c - DEBUGFONT_FIRST_CP];

    for (int gy = 0; gy < glyph->h; gy += OF_LOG_SHRINK) {
        for (int gx = 0; gx < glyph->w; gx += OF_LOG_SHRINK) {
            int sum = 0;
            for (int sy = 0; sy < OF_LOG_SHRINK && gy + sy < glyph->h; sy++)
                for (int sx = 0; sx < OF_LOG_SHRINK && gx + sx < glyph->w; sx++)
                    sum += debugFontPixels[(glyph->y + gy + sy) * DEBUGFONT_ATLAS_W + glyph->x + gx + sx];
            if (sum < 80 * OF_LOG_SHRINK * OF_LOG_SHRINK) continue;

            int px = x + (glyph->xoffset + gx) / OF_LOG_SHRINK;
            int py = y + (glyph->yoffset + gy) / OF_LOG_SHRINK;
            if (px >= 0 && px < width && py >= 0) fb[py * width + px] = color;
        }
    }
}

int ofPerfDrawLogText(uint16_t *fb, int width, int height, int x, int y, const char *text, uint16_t color) {
    if (y < 0 || y + OF_LOG_CELL_H > height) return x + (int) strlen(text) * OF_LOG_CELL_W;
    for (; *text != '\0'; text++, x += OF_LOG_CELL_W) drawLogChar(fb, width, x, y, *text, color);
    return x;
}

void ofPerfDrawLogScreen(uint16_t *fb, int width, int height) {
    int rows = height / OF_LOG_CELL_H;
    if (rows > OF_LOG_LINES - 1) rows = OF_LOG_LINES - 1;
    int columns = width / OF_LOG_CELL_W;

    for (int i = 0; i < width * height; i++) fb[i] = 0;
    for (int row = 0; row < rows; row++) {
        const char *line = ofLogLine(rows - 1 - row);
        for (int col = 0; col < columns && line[col] != '\0'; col++)
            drawLogChar(fb, width, col * OF_LOG_CELL_W, row * OF_LOG_CELL_H, line[col], 0x7FFF);
    }
}

/* The in-game overlay shows fewer lines so most of the scene stays visible. */
#define OF_LOG_OVERLAY_ROWS 11

static void drawLog(uint16_t *fb, int width, int height) {
    int rows = OF_LOG_OVERLAY_ROWS;
    int columns = width / OF_LOG_CELL_W;
    int top = height - rows * OF_LOG_CELL_H;
    if (top < 16) return;

    /* Darken the area behind the text so it reads over any scene. */
    for (int i = top * width; i < height * width; i++) fb[i] = (uint16_t) ((fb[i] >> 2) & 0x1CE7);

    for (int row = 0; row < rows; row++) {
        const char *line = ofLogLine(rows - 1 - row);
        int y = top + row * OF_LOG_CELL_H;
        for (int col = 0; col < columns && line[col] != '\0'; col++)
            drawLogChar(fb, width, col * OF_LOG_CELL_W, y, line[col], 0x7FFF);
    }
}

void ofPerfAddSleep(uint64_t nanos) {
    g_sleepNanos += nanos;
}

static void drawDigit(uint16_t *fb, int width, int x, int y, int digit) {
    uint16_t glyph = g_digits[digit];
    for (int row = 0; row < 5; row++) {
        int bits = (glyph >> ((4 - row) * 3)) & 7;
        for (int col = 0; col < 3; col++) {
            uint16_t color = (bits & (4 >> col)) ? 0x7FFF : 0x0000;
            for (int sy = 0; sy < OF_PERF_SCALE; sy++)
                for (int sx = 0; sx < OF_PERF_SCALE; sx++)
                    fb[(y + row * OF_PERF_SCALE + sy) * width + x + col * OF_PERF_SCALE + sx] = color;
        }
    }
}

static int drawNumber(uint16_t *fb, int width, int x, int y, unsigned value) {
    if (value > 999) value = 999;
    int divisors[3] = { 100, 10, 1 };
    for (int i = 0; i < 3; i++) {
        drawDigit(fb, width, x, y, (value / divisors[i]) % 10);
        x += 4 * OF_PERF_SCALE;
    }
    return x;
}

void ofPerfDrawMode(uint16_t *fb, int width, int height, const char *label) {
    int scale = ofFontScale(width);
    int pad = 2 * scale;
    int boxW = ofFontWidth(label, scale) + 2 * pad;
    int boxH = ofFontLineHeight(scale) + 2 * pad;
    int left = width - boxW;
    if (left < 0 || height < boxH) return;

    /* A black box behind the words so they read over any scene. */
    for (int y = 0; y < boxH; y++)
        for (int x = left; x < width; x++) fb[y * width + x] = 0;
    ofFontDraw(fb, width, height, left + pad, pad, label, 0x7FFF, scale);
}

bool ofPerfFrame(uint16_t *fb, int width, int height) {
    /* The frame ends here: whatever follows (overlays, copy, flip) is "out"
     * and is reported with the next frame. */
    ofPerfPhase(OF_PHASE_OUT);
    uint64_t now = nowNanos();
    if (fb == NULL) g_skipped++;
    if (g_lastFrame != 0) {
        uint64_t period = now - g_lastFrame;
        uint64_t work = period > g_sleepNanos ? period - g_sleepNanos : 0;
        unsigned periodMs = (unsigned) (period / 1000000u);
        unsigned workMs = (unsigned) (work / 1000000u);
        if (periodMs > g_worstPeriod) g_worstPeriod = periodMs;
        if (workMs > g_worstWork) g_worstWork = workMs;
        g_totalWork += workMs;
        reportSlowFrame(workMs);
        if (++g_count >= OF_PERF_WINDOW) {
            g_shownAverage = g_totalWork / OF_PERF_WINDOW;
            g_shownWork = g_worstWork;
            g_shownPeriod = g_worstPeriod;
            g_shownSkipped = g_skipped;
            g_skipped = 0;
            g_worstWork = g_worstPeriod = g_totalWork = 0;
            g_count = 0;
        }
    }
    g_lastFrame = now;
    g_sleepNanos = 0;
    g_frameNumber++;
    ofPerfDetail = g_enabled || g_logEnabled || g_detailAlways || (g_detailEvery > 0 && g_frameNumber % (unsigned) g_detailEvery == 0);
    swrDrawTimed = ofPerfDetail;

    if (fb == NULL) return false; /* a skipped frame: counted, nothing to draw on */
    if (g_logEnabled) drawLog(fb, width, height);
    if (!g_enabled || width < 128 || height < 16) return g_logEnabled;
    int x = drawNumber(fb, width, 2, 2, g_shownAverage);
    x = drawNumber(fb, width, x + 2 * OF_PERF_SCALE, 2, g_shownWork);
    x = drawNumber(fb, width, x + 2 * OF_PERF_SCALE, 2, g_shownPeriod);
    drawNumber(fb, width, x + 2 * OF_PERF_SCALE, 2, g_shownSkipped);
    return true;
}
