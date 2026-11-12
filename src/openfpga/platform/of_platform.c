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
#include "of_buttons.h"
#include "of_gamepad.h"

#include "of_hooks.h"
#include "of_perf.h"
#include "profiler.h"
#include "runner.h"
#include "of_bench.h"
#include "of_focus.h"
#include "of_font.h"
#include "of_loading.h"
#include "of_menu.h"
#include "of_qr.h"
#include "of_strings.h"
#include "of_native.h"
#include "of_state_requests.h"
#include "of_game.h"
#include "sw_renderer.h"
#include "sw_call_notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(OF_NATIVE_SAVESTATES) && !defined(OF_PC)
#include "of_heap.h"

int platformHeapCanReserve(size_t bytes)
{
    /* Large musl mallocs need metadata/alignment in addition to the payload.
     * Refuse overflow rather than querying a smaller reserve. */
    if (!bytes || bytes > UINT32_MAX - 65536u) return 0;
    uint32_t request = (uint32_t)bytes + 65536u;
    static bool measured;
    if (!measured) {
        measured = true;
        uint64_t start = nowNanos();
        void *probe = malloc(bytes);
        int allocated = probe != NULL;
        if (probe) free(probe);
        uint64_t probeUs = (nowNanos() - start) / 1000u;
        start = nowNanos();
        int confirmed = of_heap_can_reserve(request) > 0;
        uint64_t queryUs = (nowNanos() - start) / 1000u;
        /* Separate short lines fit the on-device log. No ongoing timing. */
        logInfo("heap %uK malloc/free %uus ok=%d\n",
                (unsigned)(bytes / 1024u), (unsigned)probeUs, allocated);
        logInfo("heap query +64K %uus ok=%d\n", (unsigned)queryUs, confirmed);
        /* This first check retains the original allocation policy even if
         * the query disagrees. A failed probe still reaches the fallback. */
        return allocated;
    }
    return of_heap_can_reserve(request) > 0;
}
#endif

#define OF_HIRES_W 640
#define OF_HIRES_H 480

static Runner *g_runner = NULL;
static uint16_t *g_nextFb = NULL;
static int g_nextW = 0;
static int g_nextH = 0;
static bool g_showingFramebuffer = false;
static bool g_loadingVideo = false; /* the loading screen set the video mode up */
static bool g_loadingShown = false; /* and has put a frame on screen */
static void blankDisplayBuffers(void);
static const char *g_inputScript = NULL;
static bool g_uncapped = false;
static bool g_hiresAvailable = true;
/* L's setting: true is speed, false accuracy (see the list of what speed
 * gives up further down). Speed is what the player starts in. Accuracy is the
 * target: wherever an optimisation brings accuracy up to speed's frame rate,
 * the shortcut speed takes there should go, so that the two differ only
 * where they still have to. The desktop build starts in accuracy instead,
 * because frame comparisons are made against what the game asks for;
 * BS_SMOOTH=1 gives it speed. */
#ifdef OF_PC
static bool g_smoothLowres = false;
#else
static bool g_smoothLowres = true;
#endif
#define OF_MODE_SHOWN_NANOS 2000000000ull
static uint64_t g_modeShownUntil = 0;
extern bool swrSmoothMinify; /* butterscotch/src/sw/sw_drawing.c */
extern bool swrSkipFrame;
extern int swrMirrorFaintAlpha;
extern bool swrFavorSpeed;
static int g_modeW = 0; /* 0 until the first frame sets a mode */
static int g_modeH = 0;
static int g_modeStride = 0; /* bytes per row of the display surface */

/* Gameplay buttons also dismiss snapshot notices. */
static const uint32_t g_gameButtons[] = {
    OF_BTN_UP, OF_BTN_DOWN, OF_BTN_LEFT, OF_BTN_RIGHT,
    OF_BTN_A, OF_BTN_B, OF_BTN_X, OF_BTN_Y, OF_BTN_START,
};
static uint32_t g_blockedGameButtons = 0;
#define OF_GAME_BUTTON_COUNT (sizeof(g_gameButtons) / sizeof(g_gameButtons[0]))

/* Debug buttons ("debug mode"): the buttons that are for looking into the
 * port and not for playing. Off at start-up unless the game was started with
 * --debug; the menu switches them on or off. With them on:
 *   - R toggles the log overlay;
 *   - 640x480 rooms keep showing which way L's speed/accuracy toggle is set;
 *   - Butterscotch's own debug hotkeys (see "Debug Features" in its README)
 *     are reached by holding Select and pressing another button, since a
 *     Pocket has no keyboard. A button pressed with Select held does not
 *     also reach the game, and that Select does not open the menu.
 * Each action that would otherwise leave the screen as it was says so in
 * the top right corner: changing room, clearing global.interact, pausing
 * ("Paused, frame N", which a step advances) and resuming. */
/* `notice` is what the top right corner says for two seconds when the hotkey
 * is used. Pause and step have none: a paused game says "Paused, frame N"
 * there for as long as it is paused, and a step changes N. */
static const struct {
    uint32_t button;
    int32_t key;
    const char *what;
    const char *notice;
} g_debugChords[] = {
    { OF_BTN_RIGHT, VK_PAGEUP,   "next room",                     "Next room" },
    { OF_BTN_LEFT,  VK_PAGEDOWN, "previous room",                 "Previous room" },
    { OF_BTN_START, VK_F8,       "pause on/off",                  NULL },
    { OF_BTN_A,     'O',         "step one frame (while paused)", NULL },
    { OF_BTN_B,     VK_F10,      "clear global.interact",         "interact = 0" },
};
#define OF_DEBUG_CHORD_COUNT (sizeof(g_debugChords) / sizeof(g_debugChords[0]))
static bool g_debugRequested = false;
static bool g_debugMode = false;
/* A key a menu row asked for, pressed on the frame the menu closes. */
static int32_t g_menuKey = 0;
/* The menu was up: the frame that follows it is not late. */
static bool g_menuClosed = false;

/* A few words in the top right corner for two seconds: what a button that
 * changes nothing else on screen just did. */
#define OF_NOTICE_NANOS 2000000000ull
static char g_notice[128];
static uint64_t g_noticeUntil = 0;
static bool g_snapshotNotice;
static bool g_sleepTest;
static const char *g_sleepTestLabel;

void ofPlatformStartSleepTest(void) {
    g_sleepTest = ofGame.sleepTest != NULL;
    if (g_sleepTest) ofSaveFsSetVolatile(true);
}

static void showNotice(const char *text) {
    g_snapshotNotice = false;
    snprintf(g_notice, sizeof(g_notice), "%s", text);
    g_noticeUntil = nowNanos() + OF_NOTICE_NANOS;
}

static void drawSnapshotNotice(uint16_t *fb, int width, int height) {
    int scale = ofFontScale(width), pad = 2 * scale;
    int lineHeight = ofFontLineHeight(scale), available = width - 2 * pad;
    if (available <= 0 || lineHeight <= 0) return;
    const char *text = g_notice;
    char line[128];
    for (int y = pad; *text && y + lineHeight + pad <= height; y += lineHeight + 2 * pad) {
        size_t count = 0, space = 0;
        while (text[count] && count < sizeof(line) - 1) {
            line[count] = text[count]; line[count + 1] = 0;
            if (ofFontWidth(line, scale) > available) break;
            if (text[count] == ' ') space = count;
            count++;
        }
        if (!count) return;
        /* Prefer whole words, but split an overlong word to keep progressing. */
        if (text[count] && space) count = space;
        memcpy(line, text, count); line[count] = 0;
        for (int row = y - pad; row < y + lineHeight + pad; row++)
            memset(fb + (size_t)row * (size_t)width, 0, (size_t)width * sizeof(*fb));
        ofFontDraw(fb, width, height, pad, y, line, 0x7FFF, scale);
        text += count;
        while (*text == ' ') text++;
    }
}

void ofPlatformSetDebugMode(bool enabled) {
    g_debugRequested = enabled;
}

static void setDebugMode(bool enabled) {
    g_debugMode = enabled;
    logInfo("Debug buttons %s\n", enabled ? "on: R log; Select + Right/Left room, Start pause, A step, B unstick, X script times, Y save log" : "off");
}

/* Script times (the menu, or Select + X with the debug buttons on; BS_PROFILE on desktop): Butterscotch's
 * GML profiler, reported to the log every so many frames. Timing every script
 * call costs time itself, so it is off until asked for. */
#define OF_PROFILE_FRAMES 60
static int g_profileFrames = 0;
static bool g_profileRequested = false;

void ofPlatformSetScriptProfile(int frames) {
    g_profileFrames = frames > 0 ? frames : OF_PROFILE_FRAMES;
    g_profileRequested = true;
}

static bool scriptProfileOn(void) {
    return g_runner != NULL && g_runner->vmContext->profiler != NULL;
}

static void setScriptProfile(bool enabled) {
    if (g_runner == NULL) return;
    if (g_profileFrames <= 0) g_profileFrames = OF_PROFILE_FRAMES;
    Profiler_setEnabled(&g_runner->vmContext->profiler, enabled);
}

/* Once per presented frame. */
static void scriptProfileFrame(void) {
    static int frames = 0;
    if (!scriptProfileOn()) {
        frames = 0;
        return;
    }
    if (++frames < g_profileFrames) return;
    ofPerfScriptReport(g_runner->vmContext->profiler, frames);
    Profiler_reset(g_runner->vmContext->profiler);
    frames = 0;
}

void ofPlatformSetInputScript(const char *script) {
    g_inputScript = script;
}

bool ofPlatformSpeedMode(void) {
    return g_smoothLowres;
}

void ofPlatformSetSmoothLowres(bool enabled) {
    g_smoothLowres = enabled;
}

void ofPlatformSetHiresAllowed(bool allowed) {
    g_hiresAvailable = allowed;
}

static int32_t visibleWidth(Runner *runner);

/* L's two settings. Accuracy draws everything as the game asks. Speed gives
 * up five things for time:
 *   - 640x480 rooms are drawn at 320x240 with 2x2 averaging;
 *   - mirrored blend layers too faint to move a 16-bit channel by more than
 *     one step are left out (swrMirrorFaintAlpha);
 *   - a stack of mirrored layers is worked out for every second pixel each
 *     way, and sprite draws at 8/256 opacity or less are left out
 *     (swrFavorSpeed);
 *   - small filled circles (4 pixels across or less) are stamped from kept
 *     patterns, which can move an edge pixel (swrFillSmallDot, swrFavorSpeed);
 *   - frames are skipped when the game is running behind (below).
 *
 * Frame skipping. Without it a scene that cannot be drawn in a frame's time
 * runs the whole game slow. With it, time lost to slow frames is kept as a
 * debt; once that reaches half a frame, one frame is run without drawing to
 * the screen (swrSkipFrame) and the time it saves pays the debt, by shortening
 * the wait at the end of the frame. The game keeps its pace and loses
 * smoothness instead. The debt is capped at one frame so a long stall (a room
 * load) is not chased with a burst of fast frames, and no two frames in a row
 * are skipped. */
#define OF_SPEED_FAINT_ALPHA 8
#define OF_SKIP_DEBT_NANOS 16000000ull
#define OF_SKIP_DEBT_CAP_NANOS 33000000ull
static uint64_t g_debtNanos = 0;
static bool g_skipThisFrame = false;
static int g_faintOverride = -1;
static int g_forceSkip = 0;
#ifdef OF_PC
static int g_dumpFrame = 0;
static int g_dumpTarget = -2;
#endif

/* Desktop test aids: a fixed threshold whatever L says, and skipping all but
 * every nth frame whatever the clock says. */
void ofPlatformSetMirrorFaint(int alpha) {
    g_faintOverride = alpha;
}

void ofPlatformSetForcedSkip(int every) {
    g_forceSkip = every;
}

int ofPlatformForcedSkip(void) {
    return g_forceSkip;
}

void ofPlatformCallCosts(unsigned *clock, unsigned *skippedDraw, unsigned *skippedDrawUntimed, unsigned *audioFree) {
    enum { CALLS = 4000 };
    uint64_t start = nowNanos();
    for (int i = 0; i < CALLS; i++) (void) nowNanos();
    uint64_t afterClock = nowNanos();
    *clock = (unsigned) ((afterClock - start) / CALLS);

    *skippedDraw = *skippedDrawUntimed = 0;
    Renderer *renderer = g_runner != NULL ? g_runner->renderer : NULL;
    if (renderer != NULL && renderer->dataWin->sprt.count > 0) {
        bool skipped = swrSkipFrame;
        bool timed = swrDrawTimed; /* as the last frame left it; the renderer reads the clock only when set */
        swrSkipFrame = true;
        start = nowNanos();
        for (int i = 0; i < CALLS; i++) Renderer_drawSprite(renderer, 0, i, 0.0f, 0.0f);
        uint64_t afterDraw = nowNanos();
        *skippedDraw = (unsigned) ((afterDraw - start) / CALLS);
        /* Two clock reads are the renderer's timing of the call; what is left is the call itself. */
        *skippedDrawUntimed = !timed ? *skippedDraw : *skippedDraw > 2 * *clock ? *skippedDraw - 2 * *clock : 0;
        swrSkipFrame = skipped;
    }

    start = nowNanos();
    volatile int sink = 0;
    for (int i = 0; i < CALLS; i++) sink += of_audio_free();
    *audioFree = (unsigned) ((nowNanos() - start) / CALLS);
}

void ofPlatformSpriteCosts(int *width, int *height, unsigned nanos[7], unsigned *outside, int *path) {
    int32_t w = 0, h = 0;
    uint32_t stages[SWR_PROBE_STAGES] = { 0 }, out = 0;
    *path = 0;
    if (g_runner != NULL && g_runner->renderer != NULL) swrSpriteCostProbe(g_runner->renderer, 2000, &w, &h, stages, &out, path);
    *width = (int) w; *height = (int) h; *outside = (unsigned) out;
    for (int s = 0; s < SWR_PROBE_STAGES; s++) nanos[s] = (unsigned) stages[s];
}

static void decideFrameSkip(void) {
    static bool lastSkipped = false;
    static int32_t lastShown = 0;
    static int frame = 0;
    frame++;
    int32_t shown = visibleWidth(g_runner);
    bool skip = g_smoothLowres && !g_uncapped && !lastSkipped && g_debtNanos >= OF_SKIP_DEBT_NANOS;
    if (g_forceSkip > 0) skip = frame % g_forceSkip != 0;
    /* Not across a change of picture size (the display mode follows the frame
     * that is drawn), not while paused or about to be (that frame is the one
     * left on screen), and not before anything has been shown. */
    if (shown != lastShown || g_nextFb == NULL || g_runner == NULL || g_runner->paused || of_btn(OF_BTN_SELECT)) skip = false;
#ifdef OF_PC
    if (g_dumpFrame == g_dumpTarget) skip = false;
#endif
    lastShown = shown;
    lastSkipped = skip;
    g_skipThisFrame = skip;
    swrSkipFrame = skip;
}

void ofPlatformSetUncapped(bool uncapped) {
    g_uncapped = uncapped;
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless, bool fullscreen) {
    (void) reqW;
    (void) reqH;
    (void) title;
    (void) headless;
    (void) fullscreen;

    /* The loading screen already has the display (of_loading.c). */
    if (g_loadingVideo) return true;
    /* Stay on the text terminal until the first frame is ready, so load-time
     * log lines (and any early failure) are visible on the device. */
    blankDisplayBuffers();
    of_video_init();
    of_video_set_color_mode(OF_VIDEO_MODE_RGB555);
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    return true;
}

void platformExit(void) {
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    ofButtonsInstallFonts(runner->dataWin);
    ofFontSetRunner(runner);
    ofPatchStrings(runner->dataWin);
    ofNativeInstall(runner);
    if (ofGame.installPrompts != NULL) ofGame.installPrompts(runner);
    if (ofGame.battleUiInstall != NULL) ofGame.battleUiInstall(runner);
    ofSavestateInit(runner);
    runner->setCursor = NULL;
    runner->currentCursor = GML_CR_DEFAULT;
}

/* Width of what the current room shows, in game pixels: the view if the room
 * uses one, the whole room otherwise. */
static int32_t visibleWidth(Runner *runner) {
    if (runner == NULL || runner->currentRoom == NULL) return OF_SCREEN_W;

    if (runner->viewsEnabled) {
        for (int i = 0; i < MAX_VIEWS; i++) {
            if (!runner->views[i].enabled) continue;
            GMLCamera *camera = Runner_getCameraForView(runner, i);
            return camera != NULL ? camera->viewWidth : OF_HIRES_W;
        }
    }
    return (int32_t) runner->currentRoom->width;
}

/* The renderer sizes its framebuffer from this. Undertale's window is always
 * 640x480, but overworld rooms show a 320x240 view scaled up 2x, so drawing
 * them at 320x240 loses nothing. Battles and menus use all 640x480 with small
 * fonts, and need the full resolution to stay legible. */
static int g_speedFullBattles = -1; /* -1: not decided yet */
void ofPlatformSetSpeedFullBattles(bool full) {
    g_speedFullBattles = full ? 1 : 0;
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (!outW || !outH) return false;
    int32_t shown = visibleWidth(g_runner);
    static int32_t lastShown = 0;
    if (shown != lastShown) {
        logInfo("Video: room shows %d px across\n", (int) shown);
        lastShown = shown;
    }
    bool battle = g_runner != NULL && ofGame.battleUiActive != NULL && ofGame.battleUiActive(g_runner);
    /* Speed mode can keep a battle at full size where the game allows it
     * (speedFullResBattles), when asked with --speed-full-battles (or
     * BS_SPEED_FULL_BATTLES=1 on desktop); off by default, since most battles
     * are still too slow for it. The size then changes only as the battle
     * starts or ends, under the flash that covers the room change, never in
     * the middle of one; a battle too heavy for full speed skips frames. */
    if (g_speedFullBattles < 0) g_speedFullBattles = getenv("BS_SPEED_FULL_BATTLES") != NULL;
    bool lowres = g_smoothLowres && !(ofGame.speedFullResBattles && battle && g_speedFullBattles);
    bool hires = g_hiresAvailable && !lowres && shown > OF_SCREEN_W;
    swrSmoothMinify = lowres;
    swrMirrorFaintAlpha = g_faintOverride >= 0 ? g_faintOverride : g_smoothLowres ? OF_SPEED_FAINT_ALPHA : 0;
    swrFavorSpeed = g_smoothLowres;
    if (g_runner != NULL && g_runner->renderer != NULL && ofGame.battleUiActive != NULL)
        SWRenderer_setContrastEnabled(g_runner->renderer, lowres && battle);
#ifdef GAME_UNDERTALE
    if (g_runner != NULL && g_runner->renderer != NULL) {
        const Room *room = g_runner->currentRoom;
        SWRenderer_setFloweySpeedRoom(g_runner->renderer, lowres && room != NULL && room->name != NULL &&
                                                         strcmp(room->name, "room_floweyx") == 0);
    }
#endif
    *outW = hires ? OF_HIRES_W : OF_SCREEN_W;
    *outH = hires ? OF_HIRES_H : OF_SCREEN_H;
    return true;
}

/* Blacks out all three display buffers, as big as the largest mode.
 *
 * openfpgaOS (902a8ad) changes the scanout's size and colour mode first and
 * clears its buffers after, and of_video_init switches from the terminal to
 * the app's buffers without clearing them at all. Whatever the shown buffer
 * held is meanwhile read at the new size: an old frame from the last run, or
 * at 640x480 a 320x240 frame read four times too wide, which looks like
 * static. Clearing them beforehand leaves only black to show. Also the
 * renderer's: it draws in place into these buffers. */
static void blankDisplayBuffers(void) {
#ifndef OF_PC
    const uint32_t bytes = (uint32_t) OF_HIRES_W * OF_HIRES_H * sizeof(uint16_t);
    for (int i = 0; i < 3; i++) {
        uint8_t *buffer = of_video_buffer_addr(i);
        if (buffer == NULL) continue;
        memset(buffer, 0, bytes);
        of_cache_clean_range(buffer, bytes);
    }
    swrFramebufferTouched();
#endif
}

/* Switches the display to match the frame about to be presented. */
static void matchVideoMode(int width, int height) {
    if (width == g_modeW && height == g_modeH) return;

    blankDisplayBuffers();
    of_video_mode_t want = { (uint16_t) width, (uint16_t) height, 0, OF_VIDEO_MODE_RGB555, 0 };
    if (of_video_set_mode(&want) < 0) {
        if (width > OF_SCREEN_W) {
            logWarn("Video: %dx%d is not available, staying at %dx%d.\n", width, height, OF_SCREEN_W, OF_SCREEN_H);
            g_hiresAvailable = false;
            return;
        }
        /* An OS without mode setting still has the boot 320x240 mode. */
        g_modeStride = OF_SCREEN_W * (int) sizeof(uint16_t);
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

/* Logs each distinct buffer the display hands out, the first few times. The
 * OS source puts app frame buffers at the uncached SDRAM alias (0x5xxxxxxx),
 * which would make every frame buffer read an AXI round trip; newer OS
 * versions move them to the cached alias. This line says which the card has. */
static void logFramebufferAddress(const void *surface) {
    static uintptr_t seen[4];
    static int count;
    uintptr_t address = (uintptr_t) surface;
    for (int i = 0; i < count; i++) {
        if (seen[i] == address) return;
    }
    if (count == (int) (sizeof(seen) / sizeof(seen[0]))) return;
    seen[count++] = address;
    logInfo("Video: frame buffer %d at 0x%08lx (%s alias)\n", count, (unsigned long) address,
            (address >> 28) == 0x5 ? "uncached" : "cached or other");
}

/* The renderer draws each frame straight into the display's back buffer, so
 * presenting is just a flip. Returns NULL (renderer keeps its own buffer and
 * the frame is copied) if the mode is unavailable or its rows are padded. */
uint16_t *platformAcquireFramebuffer(int width, int height) {
    matchVideoMode(width, height);
    if (width != g_modeW || height != g_modeH) return NULL;
    if (g_modeStride != width * (int) sizeof(uint16_t)) return NULL;
    uint16_t *surface = (uint16_t *) (void *) of_video_surface();
    logFramebufferAddress(surface);
    return surface;
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
/* Desktop-only verification aid: BS_DUMP_FRAME=<n> writes frame n of the
 * RGB555 output to BS_DUMP_PATH (default frame.ppm) and exits. */
static void writePictureTo(const char *path, const uint16_t *fb, int width, int height) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) exit(1);
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        uint16_t p = fb[i];
        uint8_t rgb[3] = {
            (uint8_t) (((p >> 10) & 0x1F) << 3),
            (uint8_t) (((p >> 5) & 0x1F) << 3),
            (uint8_t) ((p & 0x1F) << 3),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void writeFrameTo(const char *path, bool thenExit) {
    writePictureTo(path, g_nextFb, g_nextW, g_nextH);
    if (thenExit) exit(0);
}

static void writeFrameDump(void) {
    const char *path = getenv("BS_DUMP_PATH");
    writeFrameTo(path != NULL ? path : "frame.ppm", true);
}

/* BS_DUMP_EVERY=<n> also writes every nth frame on the way there, to
 * <BS_DUMP_DIR>/f<frame>.ppm, to follow a long scripted run. */
static void dumpFrameIfRequested(bool drawn) {
    static int every = 0;
    if (g_dumpTarget == -2) {
        const char *env = getenv("BS_DUMP_FRAME");
        g_dumpTarget = env != NULL ? atoi(env) : -1;
        env = getenv("BS_DUMP_EVERY");
        every = env != NULL ? atoi(env) : 0;
    }
    /* A skipped frame counts but has no picture. */
    if (!drawn) {
        g_dumpFrame++;
        return;
    }
    int frame = g_dumpFrame;
    int target = g_dumpTarget;
    if (every > 0 && frame > 0 && frame % every == 0) {
        const char *dir = getenv("BS_DUMP_DIR");
        char path[512];
        snprintf(path, sizeof(path), "%s/f%06d.ppm", dir != NULL ? dir : ".", frame);
        writeFrameTo(path, false);
    }
    g_dumpFrame++;
    if (target < 0 || frame != target) return;
    writeFrameDump();
}
#endif

static int g_scriptFrame = 0; /* frames of scripted input so far (runInputScript) */

#ifdef OF_PC
/* BS_FRAME_LOG=<file> (desktop) writes one line per drawn frame: frame,
 * room, width, step/draw/out/audio microseconds, calls per draw kind, and the
 * 32x16 tiles the renderer had marked as drawn in the buffer it cleared last
 * (swrDirtyLastTiles), a measure of how much of the screen a frame touches.
 * BS_OBJECTS=<frame> logs the objects that have instances on that frame. */
static void frameLog(void) {
    static FILE *out = NULL;
    static int state = 0;
    if (state == 0) {
        const char *path = getenv("BS_FRAME_LOG");
        out = path != NULL ? fopen(path, "w") : NULL;
        state = out != NULL ? 1 : 2;
    }
    const char *objects = getenv("BS_OBJECTS");
    if (objects != NULL && g_runner != NULL && g_scriptFrame == atoi(objects)) {
        char line[2048] = "";
        size_t used = 0;
        int count = (int) arrlen(g_runner->instances);
        for (int i = 0; i < count; i++) {
            Instance *inst = g_runner->instances[i];
            if (inst == NULL || inst->destroyed || inst->objectIndex < 0 || (uint32_t) inst->objectIndex >= g_runner->dataWin->objt.count) continue;
            const char *name = g_runner->dataWin->objt.objects[inst->objectIndex].name;
            if (name == NULL || strstr(line, name) != NULL) continue;
            int n = snprintf(line + used, sizeof(line) - used, "%s ", name);
            if (n < 0 || (size_t) n >= sizeof(line) - used) break;
            used += (size_t) n;
        }
        logInfo("Objects at %d in %s: %s\n", g_scriptFrame, g_runner->currentRoom ? g_runner->currentRoom->name : "?", line);
    }
    if (state != 1 || g_runner == NULL) return;
    /* Peeks, so a benchmark's sums stay whole; the line is this frame's
     * share. A benchmark taking the sums starts them over: then the whole
     * sum is the share. */
    static OfPerfTotals last;
    OfPerfTotals now, t;
    ofPerfPeekTotals(&now);
    if (now.frames < last.frames) memset(&last, 0, sizeof(last));
    t = now;
    for (int k = 0; k < OF_PHASES; k++) t.phaseNanos[k] -= last.phaseNanos[k];
    for (int k = 0; k < OF_DRAW_KINDS; k++) t.drawCalls[k] -= last.drawCalls[k];
    last = now;
    fprintf(out, "%d %s %d %llu %llu %llu %llu", g_scriptFrame, g_runner->currentRoom ? g_runner->currentRoom->name : "?", g_nextW,
            (unsigned long long) (t.phaseNanos[OF_PHASE_STEP] / 1000), (unsigned long long) (t.phaseNanos[OF_PHASE_DRAW] / 1000),
            (unsigned long long) (t.phaseNanos[OF_PHASE_OUT] / 1000), (unsigned long long) (t.phaseNanos[OF_PHASE_AUDIO] / 1000));
    for (int k = 0; k < OF_DRAW_KINDS; k++) fprintf(out, " %llu", (unsigned long long) t.drawCalls[k]);
    fprintf(out, " %u\n", swrDirtyLastTiles);
    fflush(out);
}
#endif

void platformSwapBuffers(void) {
    if (g_nextFb == NULL) return;
    ofPerfDetailAlways(g_debugMode);
    if (g_skipThisFrame) {
        /* Nothing was drawn: the frame is counted and the last picture stays up. */
        scriptProfileFrame();
        ofPerfFrame(NULL, 0, 0);
        ofFocusPresent(g_runner,true);
        ofBenchFrame(g_runner, true);
#ifdef OF_PC
        dumpFrameIfRequested(false);
#endif
        ofPerfPhase(OF_PHASE_OTHER);
        ofFocusEnd(0);
        return;
    }
#ifdef OF_PC
    /* BS_OVERLAY=1 turns both overlays on, to check them in frame dumps. */
    static bool overlaysChecked = false;
    if (!overlaysChecked) {
        overlaysChecked = true;
        if (getenv("BS_OVERLAY") != NULL) {
            ofPerfToggle();
            ofPerfToggleLog();
        }
    }
#endif
    scriptProfileFrame();
    /* Words drawn over the picture are not the renderer's: it then clears
     * the whole of this buffer next time it gets it. */
    bool overlaid = ofPerfFrame(g_nextFb, g_nextW, g_nextH);
    ofFocusPresent(g_runner,false);
    /* L's setting, by the name the instruction screen gives the toggle:
     * for a moment after L is pressed, and in debug mode for as long as a
     * 640x480 room (the only kind it changes) is showing. */
    static bool wasPaused = false;
    bool paused = g_runner != NULL && g_runner->paused;
    if (wasPaused && !paused) showNotice("Resumed");
    wasPaused = paused;
    bool drawnOver = true;
    if (g_snapshotNotice) {
        drawSnapshotNotice(g_nextFb, g_nextW, g_nextH);
    } else if (g_sleepTestLabel) {
        ofPerfDrawMode(g_nextFb, g_nextW, g_nextH, g_sleepTestLabel);
    } else if (paused) {
        /* A paused game presents this frame and then no more until it is
         * stepped or resumed, so the words stay up with it. That is also
         * why this comes before a notice: one caught here would stay up
         * for as long as the pause. */
        char text[32];
        snprintf(text, sizeof(text), "Paused, frame %d", g_runner->frameCount);
        ofPerfDrawMode(g_nextFb, g_nextW, g_nextH, text);
    } else if (nowNanos() < g_noticeUntil) {
        ofPerfDrawMode(g_nextFb, g_nextW, g_nextH, g_notice);
    } else if (nowNanos() < g_modeShownUntil || g_debugMode) {
        ofPerfDrawMode(g_nextFb, g_nextW, g_nextH, g_smoothLowres ? "Speed" : "Accuracy");
    } else {
        drawnOver = false;
    }
    if (overlaid || drawnOver) swrFramebufferTouched();
#ifdef OF_PC
    frameLog();
#endif
    ofBenchFrame(g_runner, false);
#ifdef OF_PC
    dumpFrameIfRequested(true);
#endif

    if (!g_showingFramebuffer) {
        of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
        g_showingFramebuffer = true;
        ofLoadingEnd();
    }

    matchVideoMode(g_nextW, g_nextH);
    if (g_nextW != g_modeW || g_nextH != g_modeH) return; /* mode refused; the next frame is drawn at 320x240 */

    uint8_t *dst = of_video_surface();
    size_t rowBytes = (size_t) g_nextW * sizeof(uint16_t);
    if ((uint8_t *) g_nextFb == dst) {
        /* Drawn in place (platformAcquireFramebuffer): nothing to copy. */
    } else if ((size_t) g_modeStride == rowBytes) {
        swrFramebufferTouched(); /* dst may be a buffer the renderer draws in another time */
        memcpy(dst, g_nextFb, rowBytes * (size_t) g_nextH);
    } else {
        swrFramebufferTouched();
        for (int y = 0; y < g_nextH; y++)
            memcpy(dst + (size_t) y * g_modeStride, g_nextFb + (size_t) y * g_nextW, rowBytes);
    }
    uint64_t flipStart = nowNanos();
    bool submitted = true;
#ifdef OF_PC
    /* BS_NOFLIP=1 never presents: the desktop window waits for vsync on every flip, which caps a long scripted
     * run at the display's refresh rate. */
    static int noFlip = -1;
    if (noFlip < 0) noFlip = getenv("BS_NOFLIP") != NULL;
    submitted = !noFlip;
#endif
    if (submitted) of_video_flip();
    uint64_t flipNanos=nowNanos() - flipStart;
    ofBenchAddFlipTime(flipNanos, submitted);
#ifndef OF_PC
    ofLogSetConsole(false);
#endif
    ofPerfPhase(OF_PHASE_OTHER);
    ofFocusEnd(flipNanos);
}

/* Switches to a mode the log screen can draw straight into. */
static bool useLogMode(int width, int height) {
    matchVideoMode(width, height);
    return g_modeW == width && g_modeH == height && g_modeStride == width * (int) sizeof(uint16_t);
}

/* Shows the recent log full-screen and never returns. Used for the benchmark
 * report and for fatal errors: the OS text terminal does not display once the
 * app has switched to its own 16-bit video modes, so the text is drawn into
 * the framebuffer instead. Returns false if no frame has been drawn yet, in
 * which case the OS terminal is still on screen and the caller can use it. */
bool ofPlatformShowLogAndHalt(void) {
    if (g_nextFb == NULL || !g_showingFramebuffer) return false;

    /* 640x480 where the core has it: the game's font at its own size fits
     * about twice the text per line that 320x240 does. */
    int width = OF_HIRES_W, height = OF_HIRES_H;
    if (!g_hiresAvailable || !useLogMode(width, height)) {
        width = OF_SCREEN_W;
        height = OF_SCREEN_H;
        if (!useLogMode(width, height)) return false;
    }
    g_nextW = width;
    g_nextH = height;

    /* A toggles a QR code of the report (or of the log's end, after a fatal
     * error): a phone photo of it gets the text off the Pocket without the
     * SD card. tools/pq1-decode.py reads it back. */
    static OfQr qr;
    bool showQr = false, qrMade = false;
#ifdef OF_PC
    showQr = getenv("BS_DUMP_QR") != NULL;
#endif
    for (;;) {
        if (showQr && !qrMade) {
            qrMade = true;
            size_t len = 0;
            bool keepEnd = false;
            const char *text = ofLogQrText(&len, &keepEnd);
            if (!ofQrMake(&qr, ofLogHeader(), text, len, keepEnd, g_nextW, g_nextH))
                ofLogPrint("QR code: could not be made\n");
        }
        g_nextFb = (uint16_t *) (void *) of_video_surface();
        swrFramebufferTouched();
        if (showQr && qr.size > 0) {
            ofQrDraw(&qr, g_nextFb, g_nextW, g_nextH, g_nextW, 0x0000, 0x7FFF);
        } else {
            ofPerfDrawLogScreen(g_nextFb, g_nextW, g_nextH);
            ofPerfDrawMode(g_nextFb, g_nextW, g_nextH, "A: QR code");
        }
#ifdef OF_PC
        if (getenv("BS_DUMP_PATH") != NULL) writeFrameDump();
        exit(0);
#endif
        of_video_flip();
        of_input_poll();
        if (of_btn_pressed(OF_BTN_A)) showQr = !showQr;
        usleep(100000);
    }
}

/* The loading screen (of_loading.c) draws at 320x240 straight into the
 * display's buffers until the game's first frame. Each buffer is cleared the
 * first time it comes round; after that only the bar is drawn over. */
uint16_t *ofPlatformLoadingSurface(bool *fresh) {
    if (g_showingFramebuffer) return NULL; /* the game has the screen */
    if (!g_loadingVideo) {
        blankDisplayBuffers();
        of_video_init();
        of_video_set_color_mode(OF_VIDEO_MODE_RGB555);
        g_loadingVideo = true;
    }
    if (!useLogMode(OF_LOADING_WIDTH, OF_LOADING_HEIGHT)) return NULL;
    uint16_t *surface = (uint16_t *) (void *) of_video_surface();
    static uint16_t *seen[4];
    static int count = 0;
    *fresh = true;
    for (int i = 0; i < count; i++)
        if (seen[i] == surface) *fresh = false;
    if (*fresh && count < (int) (sizeof(seen) / sizeof(seen[0]))) seen[count++] = surface;
    swrFramebufferTouched();
    return surface;
}

void ofPlatformLoadingFlip(void) {
    of_video_flip();
    /* Off the terminal once there is a frame to show instead. */
    if (!g_loadingShown) {
        of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
        g_loadingShown = true;
    }
}

void *platformGetProcAddress(const char *name) {
    (void) name;
    return NULL;
}

/* Room jumps for scripted runs: on a given frame, set some of the game's
 * globals and go to a room, to reach a scene without playing up to it. The
 * game's own state is otherwise whatever it was, so this only suits rooms
 * that set themselves up. */
static const OfJump *g_jumps = NULL;
static int g_jumpCount = 0;

void ofPlatformSetJumps(const OfJump *jumps, int count) {
    g_jumps = jumps;
    g_jumpCount = count;
}

/* "name=1,other[2]=3": numeric globals, or elements of existing global arrays. */
static void setGlobals(const char *spec) {
    VMContext *vm = g_runner->vmContext;
    for (const char *p = spec; *p != '\0';) {
        char name[64];
        size_t n = strcspn(p, "=[");
        if (n == 0 || n >= sizeof(name)) break;
        memcpy(name, p, n);
        name[n] = '\0';
        int index = p[n] == '[' ? atoi(p + n + 1) : -1;
        const char *eq = strchr(p, '=');
        if (eq == NULL) break;
        RValue value = RValue_makeReal((GMLReal) atof(eq + 1));
        int32_t id = VM_getOrAllocateVarID(vm, name);
        if (index < 0) {
            Instance_setSelfVar(vm->globalScopeInstance, id, value);
        } else {
            RValue array = Instance_getSelfVar(vm->globalScopeInstance, id);
            if (array.type == RVALUE_ARRAY) GMLArray_set(array.array, index, value);
            else logWarn("Jump: global.%s is not an array\n", name);
        }
        const char *comma = strchr(eq, ',');
        if (comma == NULL) break;
        p = comma + 1;
    }
}

/* An instance of the object with this name, at (0, 0), as the game's own instance_create would make it. */
static void createObject(const char *name) {
    for (uint32_t i = 0; i < g_runner->dataWin->objt.count; i++) {
        const char *objectName = g_runner->dataWin->objt.objects[i].name;
        if (objectName == NULL || strcmp(objectName, name) != 0) continue;
        Runner_createInstance(g_runner, 0, 0, (int32_t) i);
        return;
    }
    logWarn("Jump: no object %s\n", name);
}

/* Scripted input for repeatable runs (the benchmark, and BS_SCRIPT on
 * desktop):
 *   "300:Z,340:D,341:Z,400:R*90"
 * presses a key on the given frame and releases it two frames later, or
 * after N frames with "*N".
 * Keys: U D L R (arrows), Z X C, E (Enter). */
static void runInputScript(void) {
    int frame = ++g_scriptFrame;
    const char *script = g_inputScript;
    if (g_runner != NULL) {
        for (int i = 0; i < g_jumpCount; i++) {
            if (g_jumps[i].frame != frame) continue;
            if (g_jumps[i].set != NULL) setGlobals(g_jumps[i].set);
            if (g_jumps[i].create != NULL) createObject(g_jumps[i].create);
            if (g_jumps[i].room >= 0) g_runner->pendingRoom = g_jumps[i].room;
        }
    }
#ifdef OF_PC
    /* BS_GOTO="<frame>:<room index>" jumps to a room, to reach a scene without playing up to it. The game's own
     * state is whatever it was, so this only suits rooms that set themselves up. */
    const char *jump = getenv("BS_GOTO");
    if (jump != NULL && g_runner != NULL && frame == atoi(jump) && strchr(jump, ':') != NULL) {
        const char *target = strchr(jump, ':') + 1;
        int room = atoi(target);
        if (*target < '0' || *target > '9') { /* a room's name */
            room = -1;
            for (uint32_t i = 0; i < g_runner->dataWin->room.count; i++)
                if (g_runner->dataWin->room.rooms[i].name != NULL && strcmp(g_runner->dataWin->room.rooms[i].name, target) == 0) room = (int) i;
            if (room < 0) logWarn("BS_GOTO: no room %s\n", target);
        }
        if (room >= 0) g_runner->pendingRoom = room;
    }
    /* BS_SET="<frame>:name=1,other[2]=3" sets numeric globals (or elements of existing global arrays). */
    const char *set = getenv("BS_SET");
    if (set != NULL && g_runner != NULL && frame == atoi(set) && strchr(set, ':') != NULL)
        setGlobals(strchr(set, ':') + 1);
#endif
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

/* ===[ The menu ]===
 *
 * Select (pressed and let go on its own) opens it over the paused game; see
 * of_menu.c for how it looks and moves. It holds the debug and performance
 * settings that used to be button chords, so none of them has to be
 * remembered. (The Pocket's own core menu cannot hold them: the hardware
 * gives a core no menu variables of its own. interact.json entries can only
 * write the Analogizer registers and the app id that an instance file sets.)
 * Everything it sets lasts until the core is left, like the chords did. */

static void toggleSpeed(void) {
    g_smoothLowres = !g_smoothLowres;
    g_modeShownUntil = nowNanos() + OF_MODE_SHOWN_NANOS;
    logInfo("Video: %s\n", g_smoothLowres ? "speed (640x480 rooms at 320x240, faint layers left out, frame skipping)" : "accuracy");
}

static void toggleScriptTimes(void) {
    /* The report goes to the log, so bring that up with it. */
    bool enable = !scriptProfileOn();
    setScriptProfile(enable);
    if (enable) ofPerfShowLog();
    logInfo("Debug: script times %s\n", enable ? "on, every 2 s" : "off");
}

static const char *onOff(bool on) {
    return on ? "On" : "Off";
}

static bool menuResume(int direction) {
    (void) direction;
    return true;
}

static const char *menuVideoValue(void) {
    return g_smoothLowres ? "Speed" : "Accuracy";
}

static bool menuVideo(int direction) {
    (void) direction;
    toggleSpeed();
    return false;
}

static const char *menuFrameTimesValue(void) {
    return onOff(ofPerfOverlayOn());
}

static bool menuFrameTimes(int direction) {
    (void) direction;
    ofPerfToggle();
    return false;
}

static const char *menuLogValue(void) {
    return onOff(ofPerfLogOn());
}

static bool menuLog(int direction) {
    (void) direction;
    ofPerfToggleLog();
    return false;
}

static const char *menuScriptTimesValue(void) {
    return onOff(scriptProfileOn());
}

static bool menuScriptTimes(int direction) {
    (void) direction;
    toggleScriptTimes();
    return false;
}

static const char *menuDebugValue(void) {
    return onOff(g_debugMode);
}

static bool menuDebug(int direction) {
    (void) direction;
    setDebugMode(!g_debugMode);
    return false;
}

/* Rows that act on the game press one of Butterscotch's debug keys as the
 * menu closes, the way the chords do. */
static bool pressAfterMenu(int32_t key, const char *notice, const char *what) {
    g_menuKey = key;
    showNotice(notice);
    logInfo("Debug: %s\n", what);
    return true;
}

static bool menuNextRoom(int direction) {
    (void) direction;
    return pressAfterMenu(VK_PAGEUP, "Next room", "next room");
}

static bool menuPreviousRoom(int direction) {
    (void) direction;
    return pressAfterMenu(VK_PAGEDOWN, "Previous room", "previous room");
}

static bool menuUnstick(int direction) {
    (void) direction;
    return pressAfterMenu(VK_F10, "interact = 0", "clear global.interact");
}

static bool menuSaveLog(int direction) {
    (void) direction;
    bool saved = ofLogDump();
    showNotice(saved ? "Log saved" : "Log not saved");
    logInfo("Debug: log %s\n", saved ? "written to the spare save slot; quit from the Analogue menu to keep it" : "could not be written");
    return true;
}

static const char *const g_audioModeNames[OF_AUDIO_MODE_COUNT] = { "Normal", "Disabled", "Music only", "Sound only" };

static void setAudioMode(OfAudioMode mode) {
    ofAudioSetMode(mode);
    logInfo("Debug: audio mode %s%s\n", g_audioModeNames[mode],
            mode == OF_AUDIO_DISABLED ? "" : "; what was playing unheard is rough until it next starts");
}

static const char *menuAudioValue(void) {
    return g_audioModeNames[ofAudioMode()];
}

static bool menuAudio(int direction) {
    int step = direction < 0 ? OF_AUDIO_MODE_COUNT - 1 : 1;
    setAudioMode((OfAudioMode) (((int) ofAudioMode() + step) % OF_AUDIO_MODE_COUNT));
    return false;
}

static const char *menuSnapshotValue(void) {
    return ofSavestateReason(g_runner) ? "Unavailable" : "Ready in game";
}
static bool menuSnapshotReason(int direction) {
    (void)direction;
    const char *why = ofSavestateReason(g_runner);
    showNotice(why ? why : "Use Pocket controls to save");
    return true;
}

static const OfMenuRow g_debugRows[] = {
    { "Save log to SD card", NULL, menuSaveLog, NULL,
      "Writes the debug log. Quit through the Analogue menu to keep it." },
    { "Save-state status", menuSnapshotValue, menuSnapshotReason, NULL,
      "Checks this scene. Use Pocket controls to save or load a Memory." },
    { "Audio mode", menuAudioValue, menuAudio, NULL,
      "Isolates music or effects to diagnose audio problems." },
    { "Show frame times", menuFrameTimesValue, menuFrameTimes, NULL,
      "Shows frame times and skipped frames over the game." },
    { "Show log overlay", menuLogValue, menuLog, NULL,
      "Shows recent debug messages over the game." },
    { "Show script times in log", menuScriptTimesValue, menuScriptTimes, NULL,
      "Profiles slow scripts in the log. Adds some overhead." },
    { "Enable debug buttons", menuDebugValue, menuDebug, NULL,
      "Enables R for logs and Select chords for debug actions." },
    { "Next room", NULL, menuNextRoom, NULL,
      "Warps forward one room. May disrupt game progress." },
    { "Previous room", NULL, menuPreviousRoom, NULL,
      "Warps back one room. May disrupt game progress." },
    { "Clear interact", NULL, menuUnstick, NULL,
      "Resets the interaction flag to debug a stuck cutscene." },
};

static const OfMenuPage g_debugPage = { "DEBUG", g_debugRows, (int) (sizeof(g_debugRows) / sizeof(g_debugRows[0])) };

static const OfMenuRow g_menuRows[] = {
    { "Resume", NULL, menuResume, NULL,
      "Closes this menu and returns to the game." },
    { "Performance Mode", menuVideoValue, menuVideo, NULL,
      "Speed favors frame rate. Accuracy preserves visual detail." },
    { "Debug", NULL, NULL, &g_debugPage,
      "Opens logs, performance tools and troubleshooting actions." },
};

#ifdef OF_PC
static bool g_menuDump = false;
#endif
static uint8_t *g_menuShownIn = NULL; /* the display buffer the menu last showed a picture in */

static void menuPresent(const uint16_t *fb) {
    uint8_t *dst = of_video_surface();
    g_menuShownIn = dst;
    size_t rowBytes = (size_t) g_nextW * sizeof(uint16_t);
    for (int y = 0; y < g_nextH; y++) memcpy(dst + (size_t) y * (size_t) g_modeStride, fb + (size_t) y * (size_t) g_nextW, rowBytes);
#ifdef OF_PC
    if (g_menuDump) {
        const char *path = getenv("BS_DUMP_PATH");
        writePictureTo(path != NULL ? path : "frame.ppm", fb, g_nextW, g_nextH);
    }
    static int noFlip = -1;
    if (noFlip < 0) noFlip = getenv("BS_NOFLIP") != NULL;
    if (noFlip) return;
#endif
    of_video_flip();
}

static void menuIdle(void) {
    platformBusyTick();
    ofSavestateMenuPoll();
}
static void runMenu(void) {
    /* Over the picture last shown, at the size it was shown. */
    if (g_nextFb == NULL || !g_showingFramebuffer || g_nextW != g_modeW || g_nextH != g_modeH) return;
    static const OfMenu menu = {
        { "BUTTERSCOTCH", g_menuRows, (int) (sizeof(g_menuRows) / sizeof(g_menuRows[0])) }, menuPresent, menuIdle,
    };
    AudioSystem *audio = g_runner->audioSystem;
    bool wasPaused = g_runner->paused;
    if (!wasPaused) audio->vtable->pauseAll(audio);
    logInfo("Menu\n");
    if (!ofMenuRun(&menu, g_nextFb, g_nextW, g_nextH)) logWarn("Menu: no memory for it\n");
    swrFramebufferTouched(); /* the menu drew into the display's buffers */
    /* The menu drew into the display's buffers, which can include the one
     * the last frame was drawn in; the game's picture is now in the buffer it
     * showed last. A paused game shows that one again (the log screen too). */
    if (g_menuShownIn != NULL && g_modeStride == g_nextW * (int) sizeof(uint16_t)) g_nextFb = (uint16_t *) (void *) g_menuShownIn;
    if (!wasPaused) audio->vtable->resumeAll(audio);
    /* The time the menu was up is not a slow frame to report or catch up on. */
    ofPerfRestartClock();
    g_menuClosed = true;
}

/* Returns true when the app should quit; a Pocket core never does. */
bool platformHandleEvents(void) {
    ofPerfPhase(OF_PHASE_STEP);
    ofFocusInput(g_runner,g_scriptFrame+1);
    of_input_poll();

    /* A Pocket menu can hide the game longer than a timed toast lasts.
     * Keep rejection visible until a fresh game-button press. */
    if (g_snapshotNotice) for (size_t i = 0; i < OF_GAME_BUTTON_COUNT; i++) {
        if (of_btn_pressed(g_gameButtons[i])) {
            g_snapshotNotice = false; g_noticeUntil = 0; break;
        }
    }

    static bool chordUsed = false;
    static int32_t keyToRelease = 0;
    if (g_debugRequested) {
        g_debugRequested = false;
        setDebugMode(true);
    }
    if (of_btn_pressed(OF_BTN_SELECT)) chordUsed = false;
    /* Select on its own, let go: the menu. */
    bool openMenu = of_btn_released(OF_BTN_SELECT) && !chordUsed;
#ifdef OF_PC
    /* BS_MENU="<frame>[:<buttons>]" opens the menu on that frame and presses the buttons in it, one a frame
     * (U D L R, A B); with BS_DUMP_PATH the menu as the buttons leave it is written there and the run ends. */
    static int menuFrame = 0;
    const char *menuAt = getenv("BS_MENU");
    if (menuAt != NULL && ++menuFrame == atoi(menuAt)) {
        openMenu = true;
        const char *colon = strchr(menuAt, ':');
        ofMenuScript(colon != NULL ? colon + 1 : "");
        g_menuDump = getenv("BS_DUMP_PATH") != NULL;
    }
#endif
    /* Frame stepping is the one hotkey the runner itself gates on this. */
    if (g_runner != NULL) g_runner->debugMode = g_debugMode;
    if (g_runner != NULL && keyToRelease != 0) {
        RunnerKeyboard_onKeyUp(g_runner->keyboard, keyToRelease);
        keyToRelease = 0;
    }
    bool chording = g_debugMode && of_btn(OF_BTN_SELECT);
    if (g_debugMode && of_btn_pressed(OF_BTN_R1)) ofPerfToggleLog();
    if (of_btn_pressed(OF_BTN_L1)) toggleSpeed();
    runInputScript();
    decideFrameSkip();
    if (g_runner == NULL) return false;
    if (g_profileRequested) {
        g_profileRequested = false;
        setScriptProfile(true);
    }

    if (openMenu) {
        runMenu();
#ifdef OF_PC
        if (g_menuDump) exit(0);
#endif
        /* What was pressed in the menu is not for the game. */
        of_input_poll();
        chording = false;
        if (g_menuKey != 0) {
            RunnerKeyboard_onKeyDown(g_runner->keyboard, g_menuKey);
            keyToRelease = g_menuKey;
            g_menuKey = 0;
        }
        g_runner->debugMode = g_debugMode;
    }

    if (chording && of_btn_pressed(OF_BTN_X)) {
        toggleScriptTimes();
        chordUsed = true;
        showNotice(scriptProfileOn() ? "Script times on" : "Script times off");
    }
    if (chording && of_btn_pressed(OF_BTN_DOWN)) {
        /* Off, or back to everything from whichever mode the menu left. */
        bool mute = ofAudioMode() == OF_AUDIO_NORMAL;
        setAudioMode(mute ? OF_AUDIO_DISABLED : OF_AUDIO_NORMAL);
        chordUsed = true;
        showNotice(mute ? "Sound off" : "Sound on");
    }
    if (chording && of_btn_pressed(OF_BTN_Y)) {
        bool saved = ofLogDump();
        chordUsed = true;
        showNotice(saved ? "Log saved" : "Log not saved");
        logInfo("Debug: log %s\n", saved ? "written to the spare save slot; quit from the Analogue menu to keep it" : "could not be written");
    }
    if (chording) {
        /* One hotkey per frame; it is released on the next. */
        for (size_t i = 0; i < OF_DEBUG_CHORD_COUNT && keyToRelease == 0; i++) {
            if (!of_btn_pressed(g_debugChords[i].button)) continue;
            RunnerKeyboard_onKeyDown(g_runner->keyboard, g_debugChords[i].key);
            keyToRelease = g_debugChords[i].key;
            chordUsed = true;
            if (g_debugChords[i].notice != NULL) showNotice(g_debugChords[i].notice);
            if (g_debugChords[i].key == 'O' && !g_runner->paused) showNotice("Pause first: " OF_ICON_SELECT " + " OF_ICON_START);
            logInfo("Debug: %s\n", g_debugChords[i].what);
        }
    }

    uint32_t buttons = 0;
    for (size_t i = 0; i < OF_GAME_BUTTON_COUNT; i++)
        if (of_btn(g_gameButtons[i])) buttons |= g_gameButtons[i];
#ifdef OF_PC
    buttons |= ofGamepadScriptButtons(getenv("BS_PAD"), g_scriptFrame);
#endif
    /* A menu-closing button must be released before it reaches gameplay.
     * Undertale derives its own presses from held joystick state. */
    buttons = ofGamepadFilterButtons(&g_blockedGameButtons, buttons, chording || openMenu);
    ofGamepadPoll(g_runner->gamepads, buttons, openMenu);
    if (g_sleepTest) {
        int32_t pendingBefore = g_runner->pendingRoom;
        g_sleepTestLabel = ofGame.sleepTest(g_runner);
        if (g_runner->pendingRoom != pendingBefore) {
            /* Consume the checkpoint's input, including Start's confirm
             * alias, until release rather than carrying it into the scene. */
            g_blockedGameButtons |= buttons;
            ofGamepadPoll(g_runner->gamepads, 0, true);
        }
    }
    return false;
}

void platformSleepUntil(uint64_t time) {
    if (g_uncapped) return;
    uint64_t start = nowNanos();
    if (!g_smoothLowres || g_menuClosed) {
        g_menuClosed = false;
        g_debtNanos = 0;
    } else if (start > time) {
        g_debtNanos += start - time;
        if (g_debtNanos > OF_SKIP_DEBT_CAP_NANOS) g_debtNanos = OF_SKIP_DEBT_CAP_NANOS;
    } else {
        uint64_t paid = time - start < g_debtNanos ? time - start : g_debtNanos;
        time -= paid;
        g_debtNanos -= paid;
    }
    int64_t remaining = (int64_t) time - (int64_t) start;
    if (remaining > 2000000)
        usleep((useconds_t) ((remaining - 1000000) / 1000));
    while (nowNanos() < time) {
    }
    uint64_t end = nowNanos();
    ofPerfAddSleep(end - start);
    ofBenchPacedFrame(g_runner, g_scriptFrame, start, end);
}

void ofPlatformSnapshotNotice(const char *text) {
    showNotice(text);
    g_snapshotNotice = true; g_noticeUntil = UINT64_MAX;
    if (!g_nextFb || g_nextW <= 0 || g_nextH <= 0 || !g_showingFramebuffer ||
        g_nextW != g_modeW || g_nextH != g_modeH) return;
    drawSnapshotNotice(g_nextFb, g_nextW, g_nextH);
    swrFramebufferTouched();
    uint8_t *surface = of_video_surface();
    if ((uint8_t *)g_nextFb != surface) {
        for (int y = 0; y < g_nextH; y++)
            memcpy(surface + (size_t)y * (size_t)g_modeStride, g_nextFb + (size_t)y * (size_t)g_nextW,
                   (size_t)g_nextW * sizeof(uint16_t));
    }
    of_video_flip();
}

void ofPlatformSnapshotGet(uint32_t values[3]) {
    values[0] = (uint32_t)g_scriptFrame;
#ifdef OF_PC
    values[1] = (uint32_t)g_dumpFrame;
#else
    values[1] = 0;
#endif
    values[2] = g_smoothLowres ? 1u : 0u;
}

void ofPlatformSnapshotResumed(Runner *runner, const uint32_t values[3]) {
    g_snapshotNotice = false; g_noticeUntil = 0;
    g_scriptFrame = (int)values[0];
    g_smoothLowres = values[2] != 0;
#ifdef OF_PC
    g_dumpFrame = (int)values[1];
    (void)runner;
#else
    /* The device's input edge history is new on wake; a key held in the old
     * state must not remain stuck waiting for a release that will never come. */
    memset(runner->keyboard->keyDown, 0, sizeof(runner->keyboard->keyDown));
    memset(runner->keyboard->keyPressed, 0, sizeof(runner->keyboard->keyPressed));
    memset(runner->keyboard->keyReleased, 0, sizeof(runner->keyboard->keyReleased));
    of_input_poll();
    uint32_t buttons = 0;
    for (size_t i = 0; i < OF_GAME_BUTTON_COUNT; i++)
        if (of_btn(g_gameButtons[i])) buttons |= g_gameButtons[i];
    g_blockedGameButtons = 0;
    ofGamepadPoll(runner->gamepads, buttons, true);
#endif
    /* The display buffers need not hold what was drawn in them before. */
    swrFramebufferTouched();
    /* Old presentation timing must not cause frame skips after restore. */
    g_debtNanos = 0; g_skipThisFrame = false;
    ofPerfRestartClock();
}
