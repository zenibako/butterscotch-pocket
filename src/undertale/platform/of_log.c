/*
 * Log sink for Butterscotch: prefixes every line with seconds since start,
 * so load stages can be timed from the on-device terminal.
 */

#include "log.h"
#include "gettime.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void platformLog(const logType type, const char *format, va_list va) {
    static uint64_t start = 0;
    static bool atLineStart = true;

    uint64_t now = nowNanos();
    if (start == 0) start = now;

    if (atLineStart) {
        unsigned ms = (unsigned) ((now - start) / 1000000u);
        printf("[%3u.%02u] ", ms / 1000, (ms % 1000) / 10);
    }

    switch (type) {
        case LOG_TYPE_WARNING: fputs("Warning: ", stdout); break;
        case LOG_TYPE_ERROR:   fputs("Error: ", stdout);   break;
        case LOG_TYPE_DEBUG:   fputs("Debug: ", stdout);   break;
        case LOG_TYPE_NORMAL:  break;
    }
    vprintf(format, va);

    size_t len = strlen(format);
    atLineStart = len > 0 && format[len - 1] == '\n';
}
