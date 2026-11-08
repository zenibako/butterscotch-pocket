/*
 * Butterscotch (GameMaker: Studio runner) on openfpgaOS, one program per
 * game (GAME=undertale or GAME=deltarune; see the Makefile)
 *
 * Build: make
 * Copy:  make copy
 * Test:  make test   (desktop; expects data.win in the current directory)
 */

#include "of.h"

#include "loop.h"
#include "platform/of_diag.h"
#include "platform/of_bench.h"
#include "platform/of_focus.h"
#include "platform/of_hooks.h"
#include "platform/of_loading.h"
#include "platform/of_state_requests.h"
#include "stb_ds.h"

#ifdef OF_PC
extern bool swrTileLayerFast;
extern bool swrMirrorMerge;      /* butterscotch/src/sw/sw_drawing.c */
extern int swrMirrorFaintAlpha;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Data slots (see the instance JSON under dist/). Slot 4 holds the game's
 * data.win, slot 5 the optional texture pack built from it, slot 6 the
 * optional music pack. */
/* The Makefile names the game being built (GAME=...). */
#ifndef OF_GAME_NAME
#error "OF_GAME_NAME is not set; build with the Makefile (GAME=undertale or GAME=deltarune)"
#endif

#define OF_SLOT_DATA_WIN 4
#define OF_DATA_WIN_NAME "data.win"
#define OF_SLOT_TEXTURES 5
#define OF_TEXTURES_NAME "textures.bin"
#define OF_SLOT_MUSIC 6
#define OF_MUSIC_NAME "music.bin"

#ifdef OF_PC
/* In loop.c, which has no header for it: writes the input recording out. */
void saveInputRecording(void);

extern char **environ;

/* The desktop settings were UT_* until 2026-10-13 and are BS_* now. An old
 * name still works, unless the new one is set too, and is pointed out.
 * (rename-ut: keep) */
static void adoptOldSettings(void) {
    /* Gathered first: setenv may move environ while it is being walked. */
    const char *old[64];
    int count = 0;
    for (char **env = environ; env != NULL && *env != NULL && count < 64; env++)
        if (strncmp(*env, "UT_", 3) == 0) old[count++] = *env;

    char name[64];
    for (int i = 0; i < count; i++) {
        const char *equals = strchr(old[i], '=');
        size_t length = equals != NULL ? (size_t) (equals - old[i]) : 0;
        if (length < 4 || length >= sizeof(name)) continue;
        snprintf(name, sizeof(name), "BS_%.*s", (int) (length - 3), old[i] + 3);
        if (getenv(name) != NULL) continue;
        printf("[butterscotch] %.*s is now %s\n", (int) length, old[i], name);
        setenv(name, equals + 1, 0);
    }
}
#endif

int main(int argc, char **argv) {
#ifdef OF_PC
    adoptOldSettings();
#else
    of_file_slot_register(OF_SLOT_DATA_WIN, OF_DATA_WIN_NAME);
    of_file_slot_register(OF_SLOT_TEXTURES, OF_TEXTURES_NAME);
    of_file_slot_register(OF_SLOT_MUSIC, OF_MUSIC_NAME);
#endif
    ofDiagInstall();
    ofSavestateStartup();

    CommandLineArgs args = {0};
    args.exitAtFrame = -1;
    args.speedMultiplier = 1.0;
    args.fastForwardSpeed = 0.0;
    args.osType = OS_WINDOWS;
    args.loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
    args.lazyRooms = true;
    args.lazyTextures = true;
    args.lazyAudio = true;
    args.renderer = SOFTWARE;
    args.dataWinPath = OF_DATA_WIN_NAME;
    /* The game cores show a progress bar while loading; the benchmark entries
     * (and --debug, or --verbose-load) keep the OS terminal with the load log. */
    bool verboseLoad = false;
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "--bench", 7) == 0 || strncmp(argv[i], "--focus-", 8) == 0 ||
            strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "--verbose-load") == 0)
            verboseLoad = true;
        if (ofFocusStart(argv[i])) { args.seed=7; args.hasSeed=true; }
        if (strcmp(argv[i], "--bench-sleep") == 0) {
            args.seed = 7;
            args.hasSeed = true;
            ofPlatformStartSleepTest();
        }
        /* --bench measures accuracy mode and --bench-smooth speed mode,
         * whichever the player would start in. --bench-lowres keeps every
         * room at 320x240 with plain point sampling, for bitstreams whose
         * 640x480 mode misbehaves. */
        bool lowres = strcmp(argv[i], "--bench-lowres") == 0;
        bool smooth = strcmp(argv[i], "--bench-smooth") == 0;
        if (lowres || smooth || strcmp(argv[i], "--bench") == 0) {
            args.seed = 7;
            args.hasSeed = true;
            ofPlatformSetSmoothLowres(smooth);
            if (lowres) ofPlatformSetHiresAllowed(false);
            ofBenchStart();
        }
        /* --bench-battles (accuracy) and --bench-battles-smooth (speed): the battle tour. */
        bool tourSmooth = strcmp(argv[i], "--bench-battles-smooth") == 0;
        if (tourSmooth || strcmp(argv[i], "--bench-battles") == 0) {
            args.seed = 7;
            args.hasSeed = true;
            ofPlatformSetSmoothLowres(tourSmooth);
            if (!ofBenchStartTour()) logWarn("Bench: this game has no battle tour\n");
        }
        /* --bench-bullet-hell (accuracy) and --bench-bullet-hell-smooth (speed): the bullet tour. */
        bool bulletSmooth = strcmp(argv[i], "--bench-bullet-hell-smooth") == 0;
        if (bulletSmooth || strcmp(argv[i], "--bench-bullet-hell") == 0) {
            args.seed = 7;
            args.hasSeed = true;
            ofPlatformSetSmoothLowres(bulletSmooth);
            if (!ofBenchStartBulletTour()) logWarn("Bench: this game has no bullet-hell tour\n");
        }
        bool saberSmooth = strcmp(argv[i], "--bench-chaos-saber-smooth") == 0;
        if (saberSmooth || strcmp(argv[i], "--bench-chaos-saber") == 0) {
            args.seed = 7;
            args.hasSeed = true;
            ofPlatformSetSmoothLowres(saberSmooth);
            if (!ofBenchStartChaosSaber()) logWarn("Bench: this game has no Chaos Saber benchmark\n");
        }
        bool floweySmooth = strcmp(argv[i], "--bench-flowey-smooth") == 0;
        if (floweySmooth || strcmp(argv[i], "--bench-flowey") == 0) {
            args.seed = 7;
            args.hasSeed = true;
            ofPlatformSetSmoothLowres(floweySmooth);
            if (!ofBenchStartFloweyTour()) logWarn("Bench: this game has no Flowey tour\n");
        }
        if (strcmp(argv[i], "--speed-full-battles") == 0) ofPlatformSetSpeedFullBattles(true);
        if (strcmp(argv[i], "--debug") == 0) ofPlatformSetDebugMode(true);
        /* --sw-mixer: the software mix instead of the hardware mixer, to compare the two. */
        if (strcmp(argv[i], "--sw-mixer") == 0) ofAudioUseSoftwareMixer(true);
        if (strcmp(argv[i], "--mute") == 0) ofAudioSetMode(OF_AUDIO_DISABLED); /* as the menu's audio mode, or Select + Down in debug mode */
        /* Game-script times in the log every 150 frames (see ofPerfScriptReport): which scripts a section's
         * "step" and unexplained "draw" time are. Timing every script call slows the run, so a benchmark
         * with this on is for the names, not for its totals. */
        if (strcmp(argv[i], "--scripts") == 0) ofPlatformSetScriptProfile(150);
        /* --draw-every <n>: only every nth frame is drawn, the rest skipped as speed mode skips them. With a
         * benchmark and a large n this times everything but the drawing: what a skipped frame costs. */
        if (strcmp(argv[i], "--draw-every") == 0 && i + 1 < argc) ofPlatformSetForcedSkip(atoi(argv[++i]));
    }

#ifndef OF_PC
    if (!verboseLoad) ofLoadingBegin();
#else
    (void) verboseLoad;
#endif
    ofDiagCheckFile(OF_DATA_WIN_NAME);
#ifndef OF_PC
    ofLoadingPlan(OF_DATA_WIN_NAME);
#endif

#ifdef OF_PC
    if (getenv("BS_SCRIPT") != NULL) ofPlatformSetInputScript(getenv("BS_SCRIPT"));
    if (getenv("BS_UNCAPPED") != NULL) ofPlatformSetUncapped(true);
    if (getenv("BS_SMOOTH") != NULL) ofPlatformSetSmoothLowres(true);
    /* BS_NO_MIRROR=1 draws mirrored layers the ordinary way; BS_MIRROR_FAINT=<alpha of 256> sets how faint
     * a mirrored layer has to be to be left out (4 leaves none out). For comparing frames. */
    if (getenv("BS_NO_MIRROR") != NULL) swrMirrorMerge = false;
    if (getenv("BS_NO_TILELAYER") != NULL) swrTileLayerFast = false;
    if (getenv("BS_MIRROR_FAINT") != NULL) ofPlatformSetMirrorFaint(atoi(getenv("BS_MIRROR_FAINT")));
    /* BS_SKIP=<n> draws only every nth frame, to check that a skipped frame leaves the game where a drawn
     * one would. */
    if (getenv("BS_SKIP") != NULL) ofPlatformSetForcedSkip(atoi(getenv("BS_SKIP")));
    if (getenv("BS_DEBUG") != NULL) ofPlatformSetDebugMode(true);
    /* BS_PROFILE=<frames> logs the heaviest game scripts every that many frames (the report the menu's script times give
     * on the device). */
    if (getenv("BS_PROFILE") != NULL) ofPlatformSetScriptProfile(atoi(getenv("BS_PROFILE")));
#ifdef ENABLE_VM_OPCODE_PROFILER
    /* `make ops` builds with this: the ranking of bytecode instructions executed, printed on the way out. */
    args.opcodeProfiler = true;
#endif
    /* BS_RECORD=<file> writes every key press and release, by frame, when the run ends; BS_PLAYBACK=<file>
     * replays one. Both together replay and then go on recording, to extend a recording. */
    args.recordInputsPath = getenv("BS_RECORD");
    args.playbackInputsPath = getenv("BS_PLAYBACK");
    if (args.recordInputsPath != NULL) atexit(saveInputRecording);
    /* BS_EXIT_FRAME=<n> leaves the main loop at that frame, the ordinary way out: the opcode ranking of
     * `make ops` is only printed on that path (a frame dump exits on the spot). */
    if (getenv("BS_EXIT_FRAME") != NULL) args.exitAtFrame = atoi(getenv("BS_EXIT_FRAME"));
    /* BS_DUMP_STATE=<frame> prints every instance and its variables at that
     * frame; BS_DISASM=<code entry name, or *> prints its bytecode at start. */
    if (getenv("BS_DUMP_STATE") != NULL) {
        int frame = atoi(getenv("BS_DUMP_STATE"));
        hmput(args.dumpFrames, frame, true);
    }
    if (getenv("BS_DISASM") != NULL) shput(args.disassemble, getenv("BS_DISASM"), true);
    /* BS_SEED=<n> fixes the game's RNG so desktop runs are repeatable. */
    const char *seed = getenv("BS_SEED");
    if (seed != NULL) {
        args.seed = atoi(seed);
        args.hasSeed = true;
    }
#endif
#ifdef OF_TRACE_FRAMES
    args.traceFrames = true;
#endif

    int ret = loop(args, OF_GAME_NAME);
    freeCommandLineArgs(&args);
    return ret;
}
