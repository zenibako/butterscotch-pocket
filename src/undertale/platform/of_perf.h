#ifndef UT_OF_PERF_H
#define UT_OF_PERF_H

#include <stdbool.h>
#include <stdint.h>

/* Frame-time overlay. Call utPerfFrame once per presented frame, before the
 * copy to the display, and utPerfAddSleep with time spent pacing. */
void utPerfToggle(void);
void utPerfAddSleep(uint64_t nanos);
void utPerfFrame(uint16_t *fb, int width, int height);

#endif /* UT_OF_PERF_H */
