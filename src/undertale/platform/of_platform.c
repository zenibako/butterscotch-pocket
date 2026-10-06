/*
 * openfpgaOS platform backend for Butterscotch.
 *
 * Implements the platform* hooks declared in butterscotch/src/platformdefs.h
 * on top of the of_* API. The software renderer draws X1R5G5B5 pixels, which
 * is exactly OF_VIDEO_MODE_RGB555, at 320x240 or 640x480 depending on the
 * room, so presenting a frame is a mode check, one copy and a flip.
 */

#include "of.h"

#include "common.h"
#include "platformdefs.h"
#include "gettime.h"
#include "runner_keyboard.h"

#include "of_perf.h"
#include "ut_bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UT_SCREEN_W OF_SCREEN_W
#define UT_SCREEN_H OF_SCREEN_H
#define UT_HIRES_W 640
#define UT_HIRES_H 480

static Runner *g_runner = NULL;
static uint16_t *g_nextFb = NULL;
static int g_nextW = 0;
static int g_nextH = 0;
static bool g_showingFramebuffer = false;
static const char *g_inputScript = NULL;
static bool g_uncapped = false;
static bool g_hiresAvailable = true;
static int g_modeW = 0; /* 0 until the first frame sets a mode */
static int g_modeH = 0;
static int g_modeStride = 0; /* bytes per row of the display surface */

/* Pad button -> GML virtual key. Undertale reads Z/X/C with Enter/Shift/Ctrl
 * as aliases; the d-pad maps to the arrow keys. Select is not Esc (holding
 * Esc quits Undertale); Select and L toggle the frame-time overlay, R the log overlay. */
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

void utPlatformSetInputScript(const char *script) {
    g_inputScript = script;
}

void utPlatformSetUncapped(bool uncapped) {
    g_uncapped = uncapped;
}

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

/* Width of what the current room shows, in game pixels: the view if the room
 * uses one, the whole room otherwise. */
static int32_t visibleWidth(Runner *runner) {
    if (runner == NULL || runner->currentRoom == NULL) return UT_SCREEN_W;

    if (runner->viewsEnabled) {
        for (int i = 0; i < MAX_VIEWS; i++) {
            if (!runner->views[i].enabled) continue;
            GMLCamera *camera = Runner_getCameraForView(runner, i);
            return camera != NULL ? camera->viewWidth : UT_HIRES_W;
        }
    }
    return (int32_t) runner->currentRoom->width;
}

/* The renderer sizes its framebuffer from this. Undertale's window is always
 * 640x480, but overworld rooms show a 320x240 view scaled up 2x, so drawing
 * them at 320x240 loses nothing. Battles and menus use all 640x480 with small
 * fonts, and need the full resolution to stay legible. */
bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (!outW || !outH) return false;
    int32_t shown = visibleWidth(g_runner);
    static int32_t lastShown = 0;
    if (shown != lastShown) {
        logInfo("Video: room shows %d px across\n", (int) shown);
        lastShown = shown;
    }
    bool hires = g_hiresAvailable && shown > UT_SCREEN_W;
    *outW = hires ? UT_HIRES_W : UT_SCREEN_W;
    *outH = hires ? UT_HIRES_H : UT_SCREEN_H;
    return true;
}

/* Switches the display to match the frame about to be presented. */
static void matchVideoMode(int width, int height) {
    if (width == g_modeW && height == g_modeH) return;

    of_video_mode_t want = { (uint16_t) width, (uint16_t) height, 0, OF_VIDEO_MODE_RGB555, 0 };
    if (of_video_set_mode(&want) < 0) {
        if (width > UT_SCREEN_W) {
            logWarn("Video: %dx%d is not available, staying at %dx%d.\n", width, height, UT_SCREEN_W, UT_SCREEN_H);
            g_hiresAvailable = false;
            return;
        }
        /* An OS without mode setting still has the boot 320x240 mode. */
        g_modeStride = UT_SCREEN_W * (int) sizeof(uint16_t);
    } else {
        of_video_mode_t got;
        of_video_get_mode(&got);
        g_modeStride = got.stride;
        logInfo("Video: %ux%u, stride %u\n", (unsigned) got.width, (unsigned) got.height, (unsigned) got.stride);
    }
    g_modeW = width;
    g_modeH = height;
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
    /* UT_OVERLAY=1 turns both overlays on, to check them in frame dumps. */
    static bool overlaysChecked = false;
    if (!overlaysChecked) {
        overlaysChecked = true;
        if (getenv("UT_OVERLAY") != NULL) {
            utPerfToggle();
            utPerfToggleLog();
        }
    }
#endif
    utPerfFrame(g_nextFb, g_nextW, g_nextH);
    utBenchFrame();
#ifdef OF_PC
    dumpFrameIfRequested();
#endif

    if (!g_showingFramebuffer) {
        of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
        g_showingFramebuffer = true;
    }

    matchVideoMode(g_nextW, g_nextH);
    if (g_nextW != g_modeW || g_nextH != g_modeH) return; /* mode refused; the next frame is drawn at 320x240 */

    uint8_t *dst = of_video_surface();
    size_t rowBytes = (size_t) g_nextW * sizeof(uint16_t);
    if ((size_t) g_modeStride == rowBytes) {
        memcpy(dst, g_nextFb, rowBytes * (size_t) g_nextH);
    } else {
        for (int y = 0; y < g_nextH; y++)
            memcpy(dst + (size_t) y * g_modeStride, g_nextFb + (size_t) y * g_nextW, rowBytes);
    }
    uint64_t flipStart = nowNanos();
    of_video_flip();
    utBenchAddFlipTime(nowNanos() - flipStart);
}

void *platformGetProcAddress(const char *name) {
    (void) name;
    return NULL;
}

/* Scripted input for repeatable runs (the benchmark, and UT_SCRIPT on
 * desktop):
 *   "300:Z,340:D,341:Z,400:R*90"
 * presses a key on the given frame and releases it two frames later, or
 * after N frames with "*N".
 * Keys: U D L R (arrows), Z X C, E (Enter). */
static void runInputScript(void) {
    static int frame = 0;
    const char *script = g_inputScript;
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
        int hold = colon[2] == '*' ? atoi(colon + 3) : 2;
        if (frame == at) RunnerKeyboard_onKeyDown(g_runner->keyboard, key);
        if (frame == at + hold) RunnerKeyboard_onKeyUp(g_runner->keyboard, key);
        const char *comma = strchr(colon, ',');
        if (comma == NULL) break;
        p = comma + 1;
    }
}

/* Returns true when the app should quit; a Pocket core never does. */
bool platformHandleEvents(void) {
    of_input_poll();
    if (of_btn_pressed(OF_BTN_SELECT | OF_BTN_L1)) utPerfToggle();
    if (of_btn_pressed(OF_BTN_R1)) utPerfToggleLog();
    runInputScript();
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
    if (g_uncapped) return;
    uint64_t start = nowNanos();
    int64_t remaining = (int64_t) time - (int64_t) start;
    if (remaining > 2000000)
        usleep((useconds_t) ((remaining - 1000000) / 1000));
    while (nowNanos() < time) {
    }
    utPerfAddSleep(nowNanos() - start);
}
