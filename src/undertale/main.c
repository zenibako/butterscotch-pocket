/*
 * undertale — Butterscotch (GameMaker: Studio runner) on openfpgaOS
 *
 * Build: make
 * Copy:  make copy
 * Test:  make test   (desktop; expects data.win in the current directory)
 */

#include "of.h"

#include "loop.h"
#include "stb_ds.h"

#include <stdio.h>

/* Data slots (see instance.json). Slot 4 holds the game's data.win. */
#define UT_SLOT_DATA_WIN 4
#define UT_DATA_WIN_NAME "data.win"

int main(void) {
#ifndef OF_PC
    of_file_slot_register(UT_SLOT_DATA_WIN, UT_DATA_WIN_NAME);
#endif

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
#ifdef UT_TRACE_FRAMES
    args.traceFrames = true;
#endif

    int ret = loop(args, "undertale");
    freeCommandLineArgs(&args);
    return ret;
}
