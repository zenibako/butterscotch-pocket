#ifndef OF_LOADING_GUARD_H
#define OF_LOADING_GUARD_H

/*
 * The loading screen of the game cores: a black screen with a progress bar
 * along the bottom, as most Pocket cores show, in place of the OS text
 * terminal and its scrolling load log. The benchmark entries keep the log
 * (main.c decides). If loading fails, the bar turns red and the screen says
 * what went wrong; see ofLoadingFail.
 *
 * Progress is measured in roughly milliseconds of loading on the Pocket: the
 * data file's chunks weigh what reading them costs (about 1 ms a kilobyte),
 * and what follows the data file (VM, renderer, audio, the first room) a
 * quarter of that again. Load stages are recognised from the log lines that
 * already time them (of_log.c), and the 64 KB pieces of a long read move the
 * bar along within a stage (ofLoadingTick).
 */

#include <stdbool.h>
#include <stdint.h>

/* Shows the loading screen with an empty bar. Until ofLoadingEnd the OS
 * console is off (writing to it costs about 20 ms a line). */
void ofLoadingBegin(void);
/* Reads the data file's chunk table to weigh the stages; without it the bar
 * still moves, by a guess at the data file's weight. */
void ofLoadingPlan(const char *dataWinPath);
/* A log line (its format and its text) from of_log.c. */
void ofLoadingNote(const char *format, const char *text);
/* One piece of a long read is done (platformBusyTick). */
void ofLoadingTick(void);
/* The game's first frame is on screen. */
void ofLoadingEnd(void);
/* True between ofLoadingBegin and ofLoadingEnd. */
bool ofLoadingActive(void);
/* Progress so far, 0 to 1000. */
int ofLoadingPermille(void);

/* If the loading screen is up, shows why loading failed on it and never
 * returns; `why` is what stopped the app and `logSaved` whether the log made
 * it to the save slot. Returns if there is no loading screen to use. */
void ofLoadingFail(const char *why, bool logSaved);

/* Drawing, shared with the desktop preview (tools/loading-preview.c). The
 * frame is RGB555 with rows of `width` pixels. */
void ofLoadingDrawBar(uint16_t *fb, int width, int height, int permille, uint16_t color);
typedef struct {
    const char *title;    /* "Couldn't load Undertale" */
    const char *reason;   /* one sentence */
    const char *hint;     /* what to do, or NULL */
    const char *detail;   /* the error as logged, or NULL */
    const char *footer;   /* where the log is, or NULL */
} OfLoadError;
void ofLoadingDrawError(uint16_t *fb, int width, int height, const OfLoadError *error, int permille);
/* Fills in `error` for a failure, from the last error logged (`lastError`,
 * may be empty), what stopped the app, and whether the data file had loaded. */
void ofLoadingDescribe(OfLoadError *error, const char *title, const char *dataFiles, const char *lastError,
                       const char *why, bool dataLoaded, const char *footer);

/* Presents one loading frame (of_platform.c): returns the surface to draw on
 * and whether it is new to this screen (and so needs clearing), or NULL if
 * the display can't show the loading screen. */
uint16_t *ofPlatformLoadingSurface(bool *fresh);
void ofPlatformLoadingFlip(void);

#define OF_LOADING_WIDTH 320
#define OF_LOADING_HEIGHT 240

#endif /* OF_LOADING_GUARD_H */
