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
#include "stb_ds.h"

#include <stdio.h>
#include <stdlib.h>

/* Data slots (see the instance JSON under dist/). Slot 4 holds the game's
 * data.win, slot 5 the optional texture pack built from it, slot 6 the
 * optional music pack. */
#define UT_SLOT_DATA_WIN 4
#define UT_DATA_WIN_NAME "data.win"
#define UT_SLOT_TEXTURES 5
#define UT_TEXTURES_NAME "textures.bin"
#define UT_SLOT_MUSIC 6
#define UT_MUSIC_NAME "music.bin"

int main(void) {
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
#ifdef OF_PC
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
