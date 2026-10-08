/*
 * Frame-time overlay (Select or L) and log overlay (R).
 *
 * Shows three numbers in the top-left corner, in milliseconds, over the last
 * 30 frames:
 *   average work time (frame period minus time spent sleeping for pacing)
 *   worst work time
 *   worst frame period (33 at full speed for a 30 fps game)
 */

#include "of_perf.h"

#include "debug_font.h"
#include "gettime.h"
#include "log.h"

#define UT_PERF_WINDOW 30
#define UT_PERF_SCALE  2

static bool g_enabled = false;
static bool g_logEnabled = false;
static uint64_t g_lastFrame = 0;
static uint64_t g_sleepNanos = 0;
static unsigned g_worstWork = 0, g_worstPeriod = 0, g_totalWork = 0;
static unsigned g_shownWork = 0, g_shownPeriod = 0, g_shownAverage = 0;
static int g_count = 0;
static uint64_t g_loadNanos[UT_LOAD_KINDS];
static uint64_t g_phaseNanos[UT_PHASES];
static uint64_t g_drawNanos[UT_DRAW_KINDS];
static unsigned g_drawCalls[UT_DRAW_KINDS];
static UtPhase g_phase = UT_PHASE_OTHER;
static uint64_t g_phaseStart = 0;

/* 3x5 digit glyphs, one row per 3 bits, top row first. */
static const uint16_t g_digits[10] = {
    075557, 022222, 071747, 071717, 055711,
    074717, 074757, 071111, 075757, 075717,
};

void utPerfAddLoad(UtLoadKind kind, uint64_t nanos) {
    g_loadNanos[kind] += nanos;
}

void platformDrawProfile(int kind, uint64_t nanos) {
    g_drawNanos[kind] += nanos;
    g_drawCalls[kind]++;

    /* Top the audio queue up every few milliseconds of drawing. */
    static uint64_t sinceTick = 0;
    sinceTick += nanos;
    if (sinceTick >= UT_DRAW_TICK_NANOS) {
        sinceTick = 0;
        platformBusyTick();
    }
}

void utPerfPhase(UtPhase phase) {
    uint64_t now = nowNanos();
    if (g_phaseStart != 0) g_phaseNanos[g_phase] += now - g_phaseStart;
    g_phase = phase;
    g_phaseStart = now;
}

static void reportSlowFrame(unsigned workMs) {
    if (workMs >= UT_PERF_SLOW_FRAME_MS) {
        unsigned load[UT_LOAD_KINDS], phase[UT_PHASES];
        for (int i = 0; i < UT_LOAD_KINDS; i++) load[i] = (unsigned) (g_loadNanos[i] / 1000000u);
        for (int i = 0; i < UT_PHASES; i++) phase[i] = (unsigned) (g_phaseNanos[i] / 1000000u);
        logInfo("slow %u: step %u draw %u out %u snd %u\n", workMs, phase[UT_PHASE_STEP], phase[UT_PHASE_DRAW],
                phase[UT_PHASE_OUT], phase[UT_PHASE_AUDIO]);
        logInfo("  load: rm %u tx %u fx %u mix %u mus %u\n", load[UT_LOAD_ROOM], load[UT_LOAD_TEXTURE],
                load[UT_LOAD_SOUND], load[UT_LOAD_MIX], load[UT_LOAD_MUSIC]);
        unsigned draw[UT_DRAW_KINDS];
        for (int i = 0; i < UT_DRAW_KINDS; i++) draw[i] = (unsigned) (g_drawNanos[i] / 1000000u);
        logInfo("  draw: s%u/%u p%u/%u t%u/%u b%u/%u r%u/%u\n", g_drawCalls[0], draw[0], g_drawCalls[1], draw[1],
                g_drawCalls[2], draw[2], g_drawCalls[3], draw[3], g_drawCalls[4], draw[4]);
    }
    for (int i = 0; i < UT_LOAD_KINDS; i++) g_loadNanos[i] = 0;
    for (int i = 0; i < UT_PHASES; i++) g_phaseNanos[i] = 0;
    for (int i = 0; i < UT_DRAW_KINDS; i++) {
        g_drawNanos[i] = 0;
        g_drawCalls[i] = 0;
    }
}

void utPerfToggle(void) {
    g_enabled = !g_enabled;
}

void utPerfToggleLog(void) {
    g_logEnabled = !g_logEnabled;
}

/* Log overlay text: Butterscotch's debug font atlas shrunk 3:1 by averaging
 * coverage, which gives a 6x12 cell that still reads at 320x240. */
#define UT_LOG_SHRINK 3
#define UT_LOG_CELL_W 6
#define UT_LOG_CELL_H 12

static void drawLogChar(uint16_t *fb, int width, int x, int y, char c) {
    if (c < DEBUGFONT_FIRST_CP || c > DEBUGFONT_LAST_CP) return;
    const DebugFontGlyphEntry *glyph = &debugFontGlyphs[c - DEBUGFONT_FIRST_CP];

    for (int gy = 0; gy < glyph->h; gy += UT_LOG_SHRINK) {
        for (int gx = 0; gx < glyph->w; gx += UT_LOG_SHRINK) {
            int sum = 0;
            for (int sy = 0; sy < UT_LOG_SHRINK && gy + sy < glyph->h; sy++)
                for (int sx = 0; sx < UT_LOG_SHRINK && gx + sx < glyph->w; sx++)
                    sum += debugFontPixels[(glyph->y + gy + sy) * DEBUGFONT_ATLAS_W + glyph->x + gx + sx];
            if (sum < 80 * UT_LOG_SHRINK * UT_LOG_SHRINK) continue;

            int px = x + (glyph->xoffset + gx) / UT_LOG_SHRINK;
            int py = y + (glyph->yoffset + gy) / UT_LOG_SHRINK;
            if (px >= 0 && px < width && py >= 0) fb[py * width + px] = 0x7FFF;
        }
    }
}

void utPerfDrawLogScreen(uint16_t *fb, int width, int height) {
    int rows = height / UT_LOG_CELL_H;
    if (rows > UT_LOG_LINES - 1) rows = UT_LOG_LINES - 1;
    int columns = width / UT_LOG_CELL_W;

    for (int i = 0; i < width * height; i++) fb[i] = 0;
    for (int row = 0; row < rows; row++) {
        const char *line = utLogLine(rows - 1 - row);
        for (int col = 0; col < columns && line[col] != '\0'; col++)
            drawLogChar(fb, width, col * UT_LOG_CELL_W, row * UT_LOG_CELL_H, line[col]);
    }
}

/* The in-game overlay shows fewer lines so most of the scene stays visible. */
#define UT_LOG_OVERLAY_ROWS 11

static void drawLog(uint16_t *fb, int width, int height) {
    int rows = UT_LOG_OVERLAY_ROWS;
    int columns = width / UT_LOG_CELL_W;
    int top = height - rows * UT_LOG_CELL_H;
    if (top < 16) return;

    /* Darken the area behind the text so it reads over any scene. */
    for (int i = top * width; i < height * width; i++) fb[i] = (uint16_t) ((fb[i] >> 2) & 0x1CE7);

    for (int row = 0; row < rows; row++) {
        const char *line = utLogLine(rows - 1 - row);
        int y = top + row * UT_LOG_CELL_H;
        for (int col = 0; col < columns && line[col] != '\0'; col++)
            drawLogChar(fb, width, col * UT_LOG_CELL_W, y, line[col]);
    }
}

void utPerfAddSleep(uint64_t nanos) {
    g_sleepNanos += nanos;
}

static void drawDigit(uint16_t *fb, int width, int x, int y, int digit) {
    uint16_t glyph = g_digits[digit];
    for (int row = 0; row < 5; row++) {
        int bits = (glyph >> ((4 - row) * 3)) & 7;
        for (int col = 0; col < 3; col++) {
            uint16_t color = (bits & (4 >> col)) ? 0x7FFF : 0x0000;
            for (int sy = 0; sy < UT_PERF_SCALE; sy++)
                for (int sx = 0; sx < UT_PERF_SCALE; sx++)
                    fb[(y + row * UT_PERF_SCALE + sy) * width + x + col * UT_PERF_SCALE + sx] = color;
        }
    }
}

static int drawNumber(uint16_t *fb, int width, int x, int y, unsigned value) {
    if (value > 999) value = 999;
    int divisors[3] = { 100, 10, 1 };
    for (int i = 0; i < 3; i++) {
        drawDigit(fb, width, x, y, (value / divisors[i]) % 10);
        x += 4 * UT_PERF_SCALE;
    }
    return x;
}

void utPerfFrame(uint16_t *fb, int width, int height) {
    /* The frame ends here: whatever follows (overlays, copy, flip) is "out"
     * and is reported with the next frame. */
    utPerfPhase(UT_PHASE_OUT);
    uint64_t now = nowNanos();
    if (g_lastFrame != 0) {
        uint64_t period = now - g_lastFrame;
        uint64_t work = period > g_sleepNanos ? period - g_sleepNanos : 0;
        unsigned periodMs = (unsigned) (period / 1000000u);
        unsigned workMs = (unsigned) (work / 1000000u);
        if (periodMs > g_worstPeriod) g_worstPeriod = periodMs;
        if (workMs > g_worstWork) g_worstWork = workMs;
        g_totalWork += workMs;
        reportSlowFrame(workMs);
        if (++g_count >= UT_PERF_WINDOW) {
            g_shownAverage = g_totalWork / UT_PERF_WINDOW;
            g_shownWork = g_worstWork;
            g_shownPeriod = g_worstPeriod;
            g_worstWork = g_worstPeriod = g_totalWork = 0;
            g_count = 0;
        }
    }
    g_lastFrame = now;
    g_sleepNanos = 0;

    if (g_logEnabled) drawLog(fb, width, height);
    if (!g_enabled || width < 96 || height < 16) return;
    int x = drawNumber(fb, width, 2, 2, g_shownAverage);
    x = drawNumber(fb, width, x + 2 * UT_PERF_SCALE, 2, g_shownWork);
    drawNumber(fb, width, x + 2 * UT_PERF_SCALE, 2, g_shownPeriod);
}
