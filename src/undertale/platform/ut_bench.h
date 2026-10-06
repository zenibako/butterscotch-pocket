#ifndef UT_BENCH_H
#define UT_BENCH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Built-in benchmark: a fixed, scripted play-through of the opening (intro
 * skip, naming, the first room, Flowey's dialogue and the first battle) run
 * with a fixed seed and no frame pacing. It reports the average time per
 * frame for each section, so two builds, OS versions or CPU variants can be
 * compared on the device. Started with "--bench" in the app arguments.
 */
void utBenchStart(void);
/* Call once per presented frame. Prints the report and halts at the end. */
void utBenchFrame(void);

/* Time spent inside the display flip, reported separately from the rest. */
void utBenchAddFlipTime(uint64_t nanos);

/* Platform hooks the benchmark drives. */
void utPlatformSetInputScript(const char *script);
void utPlatformSetUncapped(bool uncapped);

#endif /* UT_BENCH_H */
