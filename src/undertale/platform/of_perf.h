#ifndef UT_OF_PERF_H
#define UT_OF_PERF_H

#include <stdbool.h>
#include <stdint.h>

/* Frame-time and log overlays. Call utPerfFrame once per presented frame, before the
 * copy to the display, and utPerfAddSleep with time spent pacing. */
void utPerfToggle(void);
void utPerfToggleLog(void);

/* Recent log lines for the overlay; age 0 is the newest complete line. */
#define UT_LOG_LINES 21
#define UT_LOG_LINE_LEN 106
const char *utLogLine(int age);
/* Whether log lines also go to stdout (the OS console). */
void utLogSetConsole(bool enabled);
/* printf to the console and the overlay buffer, without a timestamp. */
void utLogPrint(const char *format, ...);
/* "slowest chunks: ..." once data.win has loaded, empty before that. */
const char *utLogLoadSummary(void);
/* The loader's "phases: alloc ..., read ..., parse ..., free ..." totals. */
const char *utLogLoadPhases(void);

/* Fills the frame with the most recent log lines on black. */
void utPerfDrawLogScreen(uint16_t *fb, int width, int height);

void utPerfAddSleep(uint64_t nanos);

/* A frame that takes longer than UT_PERF_SLOW_FRAME_MS is logged as two
 * lines, short enough to fit the log overlay, so a hitch seen on the device
 * can be attributed without a benchmark run:
 *
 *   slow 555: step 300 draw 200 out 20 snd 10
 *     load: room 0 tex 0 sfx 0 mix 12 music 0
 *     draw: s5/380 p437/20 t12/40 b1/3 r0/0
 *
 * The first line splits the frame by phase: game code, drawing, presenting
 * (overlays, copy, flip) and the audio update. The second gives time spent
 * inside those phases on particular jobs: loading the room, texture pages
 * and sound effects, mixing audio, and reading streamed music. The third
 * gives calls/ms for each kind of draw call: sprites, sprite parts (tiles),
 * text, tiled backgrounds and rectangles. All times in ms. */
#define UT_DRAW_KINDS 5
void platformDrawProfile(int kind, uint64_t nanos);
typedef enum { UT_LOAD_ROOM, UT_LOAD_TEXTURE, UT_LOAD_SOUND, UT_LOAD_MIX, UT_LOAD_MUSIC, UT_LOAD_KINDS } UtLoadKind;
typedef enum { UT_PHASE_OTHER, UT_PHASE_STEP, UT_PHASE_AUDIO, UT_PHASE_DRAW, UT_PHASE_OUT, UT_PHASES } UtPhase;
#define UT_PERF_SLOW_FRAME_MS 150
void utPerfAddLoad(UtLoadKind kind, uint64_t nanos);
/* Marks the start of a phase; time since the previous mark goes to the
 * phase that was running. */
void utPerfPhase(UtPhase phase);
void utPerfFrame(uint16_t *fb, int width, int height);

#endif /* UT_OF_PERF_H */
