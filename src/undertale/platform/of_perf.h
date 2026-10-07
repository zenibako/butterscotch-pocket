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
/* printf to the console and the overlay buffer, without a timestamp. */
void utLogPrint(const char *format, ...);
/* "slowest chunks: ..." once data.win has loaded, empty before that. */
const char *utLogLoadSummary(void);

/* Fills the frame with the most recent log lines on black. */
void utPerfDrawLogScreen(uint16_t *fb, int width, int height);

void utPerfAddSleep(uint64_t nanos);
void utPerfFrame(uint16_t *fb, int width, int height);

#endif /* UT_OF_PERF_H */
