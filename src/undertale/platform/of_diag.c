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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void haltWithTerminal(const char *why) {
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    printf("\n[undertale] %s -- halted.\n", why);
    for (;;) {
        of_input_poll();
        usleep(100000);
    }
}

static void onExit(void) {
    haltWithTerminal("app exited");
}

/* Replaces libc's abort(): Butterscotch's safeMalloc and friends call it on
 * allocation failure, right after logging what went wrong. */
void abort(void) {
    haltWithTerminal("abort() called (see message above)");
}

void utDiagInstall(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    atexit(onExit);

    const struct of_capabilities *caps = of_get_caps();
    printf("[undertale] heap %u KB at %08x\n",
           (unsigned) (caps->heap_size / 1024), (unsigned) caps->heap_base);
}

void utDiagCheckFile(const char *name) {
    FILE *f = fopen(name, "rb");
    if (f == NULL) {
        printf("[undertale] cannot open %s\n", name);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    printf("[undertale] %s: %ld bytes\n", name, size);
}

#endif /* !OF_PC */
