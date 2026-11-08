#ifndef SW_CALL_NOTES_H
#define SW_CALL_NOTES_H

#include <stdbool.h>
#include <stdint.h>

// What individual draw calls cost, for a benchmark to print: the profile's
// totals per kind of call say that rectangles took 20 ms, these say which
// rectangles. Calls are grouped by a short description of what was asked for
// (kind, size, opacity), and time is summed per description. Only kept while
// swrCallNotes is set, and only in builds with SW_DRAW_PROFILE.
typedef struct {
    char what[56];
    uint32_t calls;
    uint64_t nanos;
} SWCallNote;

extern bool swrCallNotes;

// Whether draw calls are timed at all (SW_DRAW_PROFILE builds): the platform
// may turn it off for frames nobody will ask about. Notes are only taken of
// timed calls.
extern bool swrDrawTimed;

// Copies out the descriptions with the most time, heaviest first, and starts
// over. *missed is how many calls were dropped: when the table of descriptions is
// full, the one with the least time makes way for a new one.
int swrCallNotesTake(SWCallNote* out, int max, uint32_t* missed);

enum { SWR_PIPE_RASTER_W = 45, SWR_PIPE_RASTER_H = 31 };
typedef enum {
    SWR_RASTER_OTHER, SWR_RASTER_PIVOT, SWR_RASTER_WARM, SWR_RASTER_BUILT, SWR_RASTER_PATHS
} SWRasterPath;
typedef struct {
    uint64_t nanos[SWR_RASTER_PATHS];
    uint32_t calls[SWR_RASTER_PATHS];
} SWPipeRasterNotes;

// Breakdown of timed full-sprite 45x31 rotations, using the existing draw timer.
extern SWRasterPath swrLastPipeRasterPath;
void swrPipeRasterTake(SWPipeRasterNotes* out);

// The sprite cost probe (swrSpriteCostProbe in sw_renderer.c) finds out where
// a sprite draw's time goes by cutting the draw short at numbered stages and
// timing each: the draw returns at stage swrProbeStop (0: never), and notes
// which of its ways of putting pixels down it took in swrProbePath.
#define SWR_PROBE_STAGES 7 // 1 door, 2 texture found, 3 entered, 4 clipped, 5 set up, 6 held layers let out; 0 is the whole draw
extern int swrProbeStop, swrProbePath;
#ifdef SW_DRAW_PROFILE
#define SWR_PROBE_STOP(stage) do { if (__builtin_expect(swrProbeStop == (stage), 0)) return; } while (0)
#define SWR_PROBE_PATH(path) do { swrProbePath = (path); } while (0)
#else
#define SWR_PROBE_STOP(stage) do { } while (0)
#define SWR_PROBE_PATH(path) do { } while (0)
#endif
struct Renderer;
// Nanoseconds per call for `calls` draws of one small sprite (a grid's, if one was drawn; else a
// 40x40 one): nanos[0] the whole draw, nanos[1..6] the draw cut short at that stage, *outside the
// whole draw with the sprite out of view. *path is how the pixels went down: 1 half-size copy with
// coverage, 2 rows copied whole, 3 opaque texels one by one, 4 translucent, 5 averaged.
void swrSpriteCostProbe(struct Renderer* renderer, int calls, int32_t* width, int32_t* height, uint32_t nanos[SWR_PROBE_STAGES], uint32_t* outside, int* path);

#endif
