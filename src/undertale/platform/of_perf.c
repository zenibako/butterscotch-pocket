/*
 * Frame-time overlay, toggled with Select.
 *
 * Shows two numbers in the top-left corner, both in milliseconds and both the
 * worst case over the last 30 frames:
 *   left  = work time (frame period minus time spent sleeping for pacing)
 *   right = frame period (33 at full speed for a 30 fps game)
 */

#include "of_perf.h"

#include "gettime.h"

#define UT_PERF_WINDOW 30
#define UT_PERF_SCALE  2

static bool g_enabled = false;
static uint64_t g_lastFrame = 0;
static uint64_t g_sleepNanos = 0;
static unsigned g_worstWork = 0, g_worstPeriod = 0;
static unsigned g_shownWork = 0, g_shownPeriod = 0;
static int g_count = 0;

/* 3x5 digit glyphs, one row per 3 bits, top row first. */
static const uint16_t g_digits[10] = {
    075557, 022222, 071747, 071717, 055711,
    074717, 074757, 071111, 075757, 075717,
};

void utPerfToggle(void) {
    g_enabled = !g_enabled;
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
    uint64_t now = nowNanos();
    if (g_lastFrame != 0) {
        uint64_t period = now - g_lastFrame;
        uint64_t work = period > g_sleepNanos ? period - g_sleepNanos : 0;
        unsigned periodMs = (unsigned) (period / 1000000u);
        unsigned workMs = (unsigned) (work / 1000000u);
        if (periodMs > g_worstPeriod) g_worstPeriod = periodMs;
        if (workMs > g_worstWork) g_worstWork = workMs;
        if (++g_count >= UT_PERF_WINDOW) {
            g_shownWork = g_worstWork;
            g_shownPeriod = g_worstPeriod;
            g_worstWork = g_worstPeriod = 0;
            g_count = 0;
        }
    }
    g_lastFrame = now;
    g_sleepNanos = 0;

    if (!g_enabled || width < 64 || height < 16) return;
    int x = drawNumber(fb, width, 2, 2, g_shownWork);
    drawNumber(fb, width, x + 2 * UT_PERF_SCALE, 2, g_shownPeriod);
}
