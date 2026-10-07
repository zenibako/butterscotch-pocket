/*
 * undertale — Butterscotch (GameMaker: Studio runner) on openfpgaOS
 *
 * Build: make
 * Copy:  make copy
 * Test:  make test   (desktop; expects data.win in the current directory)
 */

#include "of.h"

#include "loop.h"
#include "platform/of_diag.h"
#include "platform/ut_bench.h"
#include "stb_ds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Data slots (see the instance JSON under dist/). Slot 4 holds the game's
 * data.win, slot 5 the optional texture pack built from it, slot 6 the
 * optional music pack. */
#define UT_SLOT_DATA_WIN 4
#define UT_DATA_WIN_NAME "data.win"
#define UT_SLOT_TEXTURES 5
#define UT_TEXTURES_NAME "textures.bin"
#define UT_SLOT_MUSIC 6
#define UT_MUSIC_NAME "music.bin"

int main(int argc, char **argv) {
#ifndef OF_PC
    of_file_slot_register(UT_SLOT_DATA_WIN, UT_DATA_WIN_NAME);
    of_file_slot_register(UT_SLOT_TEXTURES, UT_TEXTURES_NAME);
    of_file_slot_register(UT_SLOT_MUSIC, UT_MUSIC_NAME);
#endif
    utDiagInstall();
    utDiagCheckFile(UT_DATA_WIN_NAME);

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
    args.dataWinPath = UT_DATA_WIN_NAME;
    for (int i = 0; i < argc; i++) {
        /* --bench-lowres keeps every room at 320x240 with plain point
         * sampling, for bitstreams whose 640x480 mode misbehaves;
         * --bench-smooth does the same with 2x2 averaging. */
        bool lowres = strcmp(argv[i], "--bench-lowres") == 0;
        bool smooth = strcmp(argv[i], "--bench-smooth") == 0;
        if (lowres || smooth || strcmp(argv[i], "--bench") == 0) {
            args.seed = 7;
            args.hasSeed = true;
            if (lowres) utPlatformSetHiresAllowed(false);
            if (smooth) utPlatformSetSmoothLowres(true);
            utBenchStart();
        }
    }

#ifdef OF_PC
    if (getenv("UT_SCRIPT") != NULL) utPlatformSetInputScript(getenv("UT_SCRIPT"));
    if (getenv("UT_UNCAPPED") != NULL) utPlatformSetUncapped(true);
    if (getenv("UT_SMOOTH") != NULL) utPlatformSetSmoothLowres(true);
    /* UT_SEED=<n> fixes the game's RNG so desktop runs are repeatable. */
    const char *seed = getenv("UT_SEED");
    if (seed != NULL) {
        args.seed = atoi(seed);
        args.hasSeed = true;
    }
#endif
#ifdef UT_TRACE_FRAMES
    args.traceFrames = true;
#endif

    int ret = loop(args, "undertale");
    freeCommandLineArgs(&args);
    return ret;
}
