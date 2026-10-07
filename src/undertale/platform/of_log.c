/*
 * Log sink for Butterscotch: prefixes every line with seconds since start,
 * so load stages can be timed from the on-device terminal.
 */

#include "log.h"
#include "gettime.h"
#include "of_perf.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The last few lines, kept for the on-screen log overlay. */
static char g_lines[UT_LOG_LINES][UT_LOG_LINE_LEN];
static int g_lineHead = 0; /* line currently being written */
static int g_linePos = 0;

static void keepText(const char *text) {
    for (; *text != '\0'; text++) {
        if (*text == '\n') {
            g_lineHead = (g_lineHead + 1) % UT_LOG_LINES;
            g_linePos = 0;
            g_lines[g_lineHead][0] = '\0';
        } else if (g_linePos < UT_LOG_LINE_LEN - 1) {
            g_lines[g_lineHead][g_linePos++] = *text;
            g_lines[g_lineHead][g_linePos] = '\0';
        }
    }
}

const char *utLogLine(int age) {
    if (age < 0 || age >= UT_LOG_LINES - 1) return "";
    /* The head line is still being written; age 0 is the last complete one. */
    return g_lines[(g_lineHead - 1 - age + 2 * UT_LOG_LINES) % UT_LOG_LINES];
}

void utLogPrint(const char *format, ...) {
    char text[256];
    va_list va;
    va_start(va, format);
    vsnprintf(text, sizeof(text), format, va);
    va_end(va);
    fputs(text, stdout);
    keepText(text);
}

void platformLog(const logType type, const char *format, va_list va) {
    static uint64_t start = 0;
    static bool atLineStart = true;
    char text[256];

    uint64_t now = nowNanos();
    if (start == 0) start = now;

    if (atLineStart) {
        unsigned ms = (unsigned) ((now - start) / 1000000u);
        snprintf(text, sizeof(text), "[%3u.%02u] ", ms / 1000, (ms % 1000) / 10);
        fputs(text, stdout);
        keepText(text);
    }

    const char *prefix = "";
    switch (type) {
        case LOG_TYPE_WARNING: prefix = "Warning: "; break;
        case LOG_TYPE_ERROR:   prefix = "Error: ";   break;
        case LOG_TYPE_DEBUG:   prefix = "Debug: ";   break;
        case LOG_TYPE_NORMAL:  break;
    }
    fputs(prefix, stdout);
    keepText(prefix);

    vsnprintf(text, sizeof(text), format, va);
    fputs(text, stdout);
    keepText(text);

    size_t len = strlen(format);
    atLineStart = len > 0 && format[len - 1] == '\n';
}
