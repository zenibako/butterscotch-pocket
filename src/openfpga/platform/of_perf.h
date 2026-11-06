#ifndef OF_OF_PERF_H
#define OF_OF_PERF_H

#include <stdbool.h>
#include <stdint.h>

/* Frame-time and log overlays. Call ofPerfFrame once per presented frame, before the
 * copy to the display, and ofPerfAddSleep with time spent pacing. */
void ofPerfToggle(void);
void ofPerfToggleLog(void);
void ofPerfHideOverlays(void);
void ofPerfShowLog(void);
bool ofPerfOverlayOn(void);
bool ofPerfLogOn(void);
/* The four numbers the frame-time overlay shows (see of_perf.c), whether or
 * not it is showing. */
void ofPerfShown(unsigned *average, unsigned *worstWork, unsigned *worstPeriod, unsigned *skipped);
/* Forgets when the last frame was, so time spent with the game stopped (the
 * menu) is not counted as one long frame. */
void ofPerfRestartClock(void);

/* Logs where game-script time went over the last `frames` frames, short
 * enough for the log overlay:
 *
 *   scripts 6.4 ms 5210 ops /frame (41, 60 fr)
 *     2.1  1830   1.0 obj_mainchara_Step_0
 *     ...
 *
 * First line: everything together, how many entries ran, and the window.
 * Then the heaviest twenty: ms per frame, VM instructions per frame, calls
 * per frame, name. Built-in functions are entries of their own, with no
 * instructions. Times are an entry's own, without what it calls. */
struct Profiler;
void ofPerfScriptReport(const struct Profiler *profiler, int frames);

/* Recent log lines for the overlay; age 0 is the newest complete line. */
#define OF_LOG_LINES 41 /* a 640x480 log screen shows about 34 */
#define OF_LOG_LINE_LEN 106
const char *ofLogLine(int age);
/* Whether log lines also go to stdout (the OS console). */
void ofLogSetConsole(bool enabled);
/* Writes everything logged so far to the game's spare save slot; see of_log.c. */
bool ofLogDump(void);
/* printf to the console and the overlay buffer, without a timestamp. */
void ofLogPrint(const char *format, ...) __attribute__((format(printf, 1, 2)));
/* "slowest chunks: ..." once data.win has loaded, empty before that. */
const char *ofLogLoadSummary(void);
/* The loader's "phases: alloc ..., read ..., parse ..., free ..." totals. */
const char *ofLogLoadPhases(void);

/* The fallback log font, before the game's font is ready: a 6x12 cell. */
#define OF_LOG_CELL_W 6
#define OF_LOG_CELL_H 12
/* One line in the log overlay's font; returns the x after it. */
int ofPerfDrawLogText(uint16_t *fb, int width, int height, int x, int y, const char *text, uint16_t color);

/* Fills the frame with the most recent log lines on black, in the game's font. */
void ofPerfDrawLogScreen(uint16_t *fb, int width, int height);

void ofPerfAddSleep(uint64_t nanos);

/* A frame that takes longer than OF_PERF_SLOW_FRAME_MS is logged as two
 * lines, short enough to fit the log overlay, so a hitch seen on the device
 * can be attributed without a benchmark run:
 *
 *   slow 555: step 300 draw 200 out 20 snd 10
 *     load: rm 0 tx 0 fx 0 mix 12 mus 0
 *     draw: s5/380 p437/20 t12/40 b1/3 r0/0
 *
 * The first line splits the frame by phase: game code, drawing, presenting
 * (overlays, copy, flip) and the audio update. The second gives time spent
 * inside those phases on particular jobs: loading the room, texture pages
 * and sound effects, mixing audio, and reading streamed music. The third
 * gives calls/ms for each kind of draw call: sprites, sprite parts (tiles),
 * text, tiled backgrounds and rectangles. All times in ms. */
#define OF_DRAW_KINDS 5
void platformDrawProfile(int kind, uint64_t nanos);
void platformDrawCount(int kind);
#define OF_DRAW_TICK_NANOS 5000000u
void platformBusyTick(void);
void ofAudioRoomChange(void);
/* OF_LOAD_MIX is all of mixing, wherever in the frame it ran; MIX_VOICES
 * (decoding and summing the voices) and MIX_WRITE (handing the samples to
 * the OS) are parts of it. The last three are counts, not times: output sample
 * pairs mixed, those times the voices playing, and voice pairs advanced through
 * proven silence. */
typedef enum { OF_LOAD_ROOM, OF_LOAD_TEXTURE, OF_LOAD_SOUND, OF_LOAD_MIX, OF_LOAD_MUSIC,
               OF_LOAD_MIX_VOICES, OF_LOAD_MIX_WRITE, OF_LOAD_MIX_PAIRS, OF_LOAD_MIX_VOICE_PAIRS, OF_LOAD_MIX_SILENT_PAIRS, OF_LOAD_KINDS } OfLoadKind;
typedef enum { OF_PHASE_OTHER, OF_PHASE_STEP, OF_PHASE_AUDIO, OF_PHASE_DRAW, OF_PHASE_OUT, OF_PHASES } OfPhase;
#define OF_PERF_SLOW_FRAME_MS 150
void ofPerfAddLoad(OfLoadKind kind, uint64_t nanos);
/* The same split, added up over every frame since the last call (or since
 * start), for the benchmark's per-section table. Taking it starts a new sum. */
typedef struct {
    uint64_t phaseNanos[OF_PHASES];
    uint64_t drawNanos[OF_DRAW_KINDS];
    uint64_t drawCalls[OF_DRAW_KINDS];
    uint64_t loadNanos[OF_LOAD_KINDS];
    uint32_t frames, detailFrames;
} OfPerfTotals;
/* Detail: the time of each draw call and of the parts of mixing. It takes
 * clock reads by the hundred, at 17 us each on the device (3 to 5 ms of a
 * battle frame), so it is only taken on frames someone will look at: all of
 * them while an overlay is up or the platform asks (debug mode), every nth
 * for a benchmark, none otherwise. drawNanos and the mix times in the totals
 * are of detailFrames frames; everything else of frames. */
extern bool ofPerfDetail;
void ofPerfDetailEvery(int frames); /* 0: only as above */
void ofPerfDetailAlways(bool always);
void ofPerfTakeTotals(OfPerfTotals *out);
void ofPerfPeekTotals(OfPerfTotals *out); /* the same sum, left running */
/* Marks the start of a phase; time since the previous mark goes to the
 * phase that was running. */
void ofPerfPhase(OfPhase phase);
bool ofPerfFrame(uint16_t *fb, int width, int height); /* true if it drew on fb */
/* Writes a few words in the top right corner, in the game's font (see
 * of_font.h): which way L's speed/accuracy toggle is set, or a notice. */
void ofPerfDrawMode(uint16_t *fb, int width, int height, const char *label);

#endif /* OF_OF_PERF_H */
