/*
 * On-device diagnostics for the openfpgaOS build.
 *
 * When an app returns or aborts, openfpgaOS reboots the core, which on the
 * Pocket looks like an endless boot loop with no clue why. These hooks stop
 * instead: they bring the text terminal back and park, so the last log
 * lines stay on screen.
 */

#ifndef OF_PC

#include "of.h"

#include "of_diag.h"
#include "of_perf.h"
#include "of_bench.h"
#include "of_loading.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void ofDiagHalt(const char *why) {
    ofLogPrint("[butterscotch] %s -- halted.\n", why);
    /* Keep the log: on the screen there is room for its last lines only. */
    bool saved = ofLogDump();
    if (saved) ofLogPrint("Log saved; quit from the Analogue menu to keep it.\n");
    /* Still loading, behind the progress bar: say on it what went wrong. */
    ofLoadingFail(why, saved);
    ofPlatformShowLogAndHalt();

    /* Still on the OS terminal (nothing drawn yet): park there. */
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    for (;;) {
        of_input_poll();
        usleep(100000);
    }
}

static void onExit(void) {
    ofDiagHalt("app exited");
}

/* Replaces libc's abort(): Butterscotch's safeMalloc and friends call it on
 * allocation failure, right after logging what went wrong. */
void abort(void) {
    ofDiagHalt("abort() called (see message above)");
}

void ofDiagInstall(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    atexit(onExit);

    const struct of_capabilities *caps = of_get_caps();
    printf("[butterscotch] heap %u KB at %08x\n",
           (unsigned) (caps->heap_size / 1024), (unsigned) caps->heap_base);
}

void ofDiagCheckFile(const char *name) {
    FILE *f = fopen(name, "rb");
    if (f == NULL) {
        logError("Cannot open %s\n", name);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    printf("[butterscotch] %s: %ld bytes\n", name, size);
}

#endif /* !OF_PC */
