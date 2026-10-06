/*
 * openfpgaOS platform backend for Butterscotch.
 *
 * Implements the platform* hooks declared in butterscotch/src/platformdefs.h
 * on top of the of_* API. The software renderer draws straight into a
 * 320x240 X1R5G5B5 buffer, which is exactly OF_VIDEO_MODE_RGB555, so
 * presenting a frame is a single copy into the back buffer plus a flip.
 */

#include "of.h"

#include "common.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_keyboard.h"

#include "of_perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UT_SCREEN_W OF_SCREEN_W
#define UT_SCREEN_H OF_SCREEN_H

static Runner *g_runner = NULL;
static uint16_t *g_nextFb = NULL;
static int g_nextW = 0;
static int g_nextH = 0;

/* Pad button -> GML virtual key. Undertale reads Z/X/C with Enter/Shift/Ctrl
 * as aliases; the d-pad maps to the arrow keys. Select is not Esc (holding
 * Esc quits Undertale); Select, L and R toggle the frame-time overlay. */
static const struct {
    uint32_t button;
    int32_t key;
} g_keymap[] = {
    { OF_BTN_UP,     VK_UP },
    { OF_BTN_DOWN,   VK_DOWN },
    { OF_BTN_LEFT,   VK_LEFT },
    { OF_BTN_RIGHT,  VK_RIGHT },
    { OF_BTN_A,      'Z' },
    { OF_BTN_B,      'X' },
    { OF_BTN_X,      'C' },
    { OF_BTN_Y,      'C' },
    { OF_BTN_START,  VK_ENTER },
};
#define UT_KEYMAP_COUNT (sizeof(g_keymap) / sizeof(g_keymap[0]))

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    (void) reqW;
    (void) reqH;
    (void) title;
    (void) headless;

    /* Stay on the text terminal until the first frame is ready, so load-time
     * log lines (and any early failure) are visible on the device. */
    of_video_init();
    of_video_set_color_mode(OF_VIDEO_MODE_RGB555);
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    return true;
}

void platformExit(void) {
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    runner->setCursor = NULL;
    runner->currentCursor = GML_CR_DEFAULT;
}

/* The renderer sizes its framebuffer from this, so reporting the panel size
 * makes it scale the game's 640x480 window down to 320x240 while drawing. */
bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (!outW || !outH) return false;
    *outW = UT_SCREEN_W;
    *outH = UT_SCREEN_H;
    return true;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    return platformGetWindowSize(outW, outH);
}

void platformSetWindowSize(int32_t width, int32_t height) {
    (void) width;
    (void) height;
}

void platformSetWindowTitle(const char *title) {
    (void) title;
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (xPos) *xPos = 0.0;
    if (yPos) *yPos = 0.0;
}

void platformSetNextFramebuffer(uint16_t *framebuffer, int width, int height, int bpp) {
    if (bpp != 16) {
        g_nextFb = NULL;
        return;
    }
    g_nextFb = framebuffer;
    g_nextW = width;
    g_nextH = height;
}

#ifdef OF_PC
/* Desktop-only verification aid: UT_DUMP_FRAME=<n> writes frame n of the
 * RGB555 output to UT_DUMP_PATH (default frame.ppm) and exits. */
static void dumpFrameIfRequested(void) {
    static int frame = 0;
    static int target = -2;
    if (target == -2) {
        const char *env = getenv("UT_DUMP_FRAME");
        target = env != NULL ? atoi(env) : -1;
    }
    if (target < 0 || frame++ != target) return;

    const char *path = getenv("UT_DUMP_PATH");
    FILE *f = fopen(path != NULL ? path : "frame.ppm", "wb");
    if (f == NULL) exit(1);
    fprintf(f, "P6\n%d %d\n255\n", g_nextW, g_nextH);
    for (int i = 0; i < g_nextW * g_nextH; i++) {
        uint16_t p = g_nextFb[i];
        uint8_t rgb[3] = {
            (uint8_t) (((p >> 10) & 0x1F) << 3),
            (uint8_t) (((p >> 5) & 0x1F) << 3),
            (uint8_t) ((p & 0x1F) << 3),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    exit(0);
}
#endif

void platformSwapBuffers(void) {
    if (g_nextFb == NULL) return;
#ifdef OF_PC
    dumpFrameIfRequested();
#endif

    utPerfFrame(g_nextFb, g_nextW, g_nextH);

    static bool showingFramebuffer = false;
    if (!showingFramebuffer) {
        of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
        showingFramebuffer = true;
    }

    uint16_t *dst = (uint16_t *) of_video_surface();
    int w = g_nextW < UT_SCREEN_W ? g_nextW : UT_SCREEN_W;
    int h = g_nextH < UT_SCREEN_H ? g_nextH : UT_SCREEN_H;

    if (g_nextW == UT_SCREEN_W) {
        memcpy(dst, g_nextFb, (size_t) UT_SCREEN_W * h * sizeof(uint16_t));
    } else {
        for (int y = 0; y < h; y++)
            memcpy(dst + y * UT_SCREEN_W, g_nextFb + y * g_nextW, (size_t) w * sizeof(uint16_t));
    }
    of_video_flip();
}

void *platformGetProcAddress(const char *name) {
    (void) name;
    return NULL;
}

#ifdef OF_PC
/* Desktop-only scripted input for repeatable test runs:
 *   UT_SCRIPT="300:Z,340:D,341:Z"
 * presses a key on the given frame and releases it two frames later.
 * Keys: U D L R (arrows), Z X C, E (Enter). */
static void runInputScript(void) {
    static int frame = 0;
    static const char *script = NULL;
    static bool loaded = false;
    if (!loaded) {
        script = getenv("UT_SCRIPT");
        loaded = true;
    }
    frame++;
    if (script == NULL || g_runner == NULL) return;

    for (const char *p = script; *p != '\0';) {
        int at = atoi(p);
        const char *colon = strchr(p, ':');
        if (colon == NULL) break;
        int32_t key = 0;
        switch (colon[1]) {
            case 'U': key = VK_UP; break;
            case 'D': key = VK_DOWN; break;
            case 'L': key = VK_LEFT; break;
            case 'R': key = VK_RIGHT; break;
            case 'E': key = VK_ENTER; break;
            default:  key = colon[1]; break;
        }
        if (frame == at) RunnerKeyboard_onKeyDown(g_runner->keyboard, key);
        if (frame == at + 2) RunnerKeyboard_onKeyUp(g_runner->keyboard, key);
        const char *comma = strchr(colon, ',');
        if (comma == NULL) break;
        p = comma + 1;
    }
}
#endif

/* Returns true when the app should quit; a Pocket core never does. */
bool platformHandleEvents(void) {
    of_input_poll();
    if (of_btn_pressed(OF_BTN_SELECT | OF_BTN_L1 | OF_BTN_R1)) utPerfToggle();
#ifdef OF_PC
    runInputScript();
#endif
    if (g_runner == NULL) return false;

    for (size_t i = 0; i < UT_KEYMAP_COUNT; i++) {
        if (of_btn_pressed(g_keymap[i].button))
            RunnerKeyboard_onKeyDown(g_runner->keyboard, g_keymap[i].key);
        if (of_btn_released(g_keymap[i].button))
            RunnerKeyboard_onKeyUp(g_runner->keyboard, g_keymap[i].key);
    }
    return false;
}

void platformSleepUntil(uint64_t time) {
#ifdef OF_PC
    /* UT_UNCAPPED=1 runs flat out, so `time` reports pure work per frame. */
    static int uncapped = -1;
    if (uncapped < 0) uncapped = getenv("UT_UNCAPPED") != NULL;
    if (uncapped) return;
#endif
    uint64_t start = nowNanos();
    int64_t remaining = (int64_t) time - (int64_t) start;
    if (remaining > 2000000)
        usleep((useconds_t) ((remaining - 1000000) / 1000));
    while (nowNanos() < time) {
    }
    utPerfAddSleep(nowNanos() - start);
}
