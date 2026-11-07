#ifndef OF_BENCH_H
#define OF_BENCH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Built-in benchmark: a fixed, scripted run through a few scenes of the game
 * the core was built for, with a fixed seed and no frame pacing. Undertale's
 * plays the opening (intro skip, naming, the first room, Flowey's dialogue
 * and the first battle); Deltarune's plays its opening, then jumps to the
 * town, a Dark World field and a battle there. It reports the average time per
 * frame for each section, so two builds, OS versions or CPU variants can be
 * compared on the device. Started with "--bench" in the app arguments.
 */
void ofBenchStart(void);
/* "--bench-battles": the game's battle tour instead, if it has one (false if not). Each section is one battle. */
bool ofBenchStartTour(void);
/* "--bench-bullet-hell": the game's bullet tour instead, if it has one (false if not). */
bool ofBenchStartBulletTour(void);
/* Interactive sleep checkpoints, with ordinary progress kept in memory. */
void ofPlatformStartSleepTest(void);
/* Call once per presented frame. Prints the report and halts at the end. */
typedef struct Runner Runner;
void ofBenchFrame(Runner *runner);

/* Time spent inside the display flip, reported separately from the rest. */
void ofBenchAddFlipTime(uint64_t nanos);

/* Platform hooks the benchmark drives. */
bool ofPlatformShowLogAndHalt(void);
void ofPlatformSetHiresAllowed(bool allowed);
void ofPlatformSetSmoothLowres(bool enabled);
/* Speed mode keeps battles at full size where the game allows it (speedFullResBattles); off by default. */
void ofPlatformSetSpeedFullBattles(bool full);
/* Which way L's speed/accuracy setting is. */
bool ofPlatformSpeedMode(void);
void ofSaveFsSetVolatile(bool enabled);
void ofPlatformSetInputScript(const char *script);
/* On `frame` of a scripted run: set the globals in `set` ("name=1,other[2]=3", or NULL), create an
 * instance of the object named `create` at (0, 0) (or NULL), and go to `room` (an index, or -1 to stay).
 * The table must outlive the run. */
typedef struct {
    int frame;
    int room;
    const char *set;
    const char *create;
} OfJump;
void ofPlatformSetJumps(const OfJump *jumps, int count);
/* Turns debug mode (overlays and Select-chord hotkeys) on from start-up; see of_platform.c. */
void ofPlatformSetDebugMode(bool enabled);
/* Turns the script profiler on from the start, reporting every `frames` frames (0: the default). */
void ofPlatformSetScriptProfile(int frames);
void ofPlatformSetUncapped(bool uncapped);
void ofPlatformSetMirrorFaint(int alpha);
void ofPlatformSetForcedSkip(int every);
int ofPlatformForcedSkip(void);
/* What a 4 KB direct read costs, in tenths of a millisecond, by what came
 * just before it: a texture read after a texture read, a texture read after a
 * music read, a music read after a texture read, a music read after a music
 * read, a texture read after 100 ms without reads, and a texture read after a
 * read of data.win. Where the textures ride in the music file, the second and
 * third are far jumps within one file and the last is a change of file. All
 * zero where reads are not direct. */
enum { OF_READ_TEX_TEX, OF_READ_MUSIC_TEX, OF_READ_TEX_MUSIC, OF_READ_MUSIC_MUSIC, OF_READ_IDLE_TEX, OF_READ_FILE_TEX, OF_READ_COSTS };
void ofPlatformReadCosts(unsigned tenths[OF_READ_COSTS]);
/* What a call costs on this machine, in nanoseconds, each averaged over a few
 * thousand: reading the clock; a sprite draw on a skipped frame, which goes
 * through everything a draw does up to the first pixel and stops there; the
 * same draw without the renderer's own timing around it; asking how full the
 * audio queue is. */
/* The renderer's sprite cost probe (swrSpriteCostProbe) on the game's renderer. */
void ofPlatformSpriteCosts(int *width, int *height, unsigned nanos[7], unsigned *outside, int *path);
void ofPlatformCallCosts(unsigned *clock, unsigned *skippedDraw, unsigned *skippedDrawUntimed, unsigned *audioFree);

#endif /* OF_BENCH_H */
