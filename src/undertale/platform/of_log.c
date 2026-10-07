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

/* Load breakdown: DATAWIN_LOG_CHUNKS makes the loader log "DataWin: NAME, ..."
 * as it starts each chunk. Time between consecutive lines is the time spent
 * on the earlier chunk; the slowest few are kept so the benchmark report can
 * show where loading time goes after the boot log has scrolled away. */
#define UT_LOAD_TOP 4
static char g_chunkName[5];
static uint64_t g_chunkStart = 0;
static struct { char name[5]; unsigned ms; } g_slowest[UT_LOAD_TOP];
static char g_loadSummary[UT_LOG_LINE_LEN];
static char g_loadPhases[UT_LOG_LINE_LEN];

static void finishChunk(uint64_t now) {
    if (g_chunkStart == 0) return;
    unsigned ms = (unsigned) ((now - g_chunkStart) / 1000000u);
    for (int i = 0; i < UT_LOAD_TOP; i++) {
        if (ms <= g_slowest[i].ms) continue;
        for (int j = UT_LOAD_TOP - 1; j > i; j--) g_slowest[j] = g_slowest[j - 1];
        memcpy(g_slowest[i].name, g_chunkName, sizeof(g_chunkName));
        g_slowest[i].ms = ms;
        break;
    }
    g_chunkStart = 0;
}

static void trackLoad(const char *format, const char *text, uint64_t now) {
    if (strncmp(format, "DataWin: phases:", 16) == 0) {
        /* Keep the loader's own phase totals, minus the "DataWin: " prefix. */
        snprintf(g_loadPhases, sizeof(g_loadPhases), "%s", text + 9);
        size_t len = strlen(g_loadPhases);
        if (len > 0 && g_loadPhases[len - 1] == '\n') g_loadPhases[len - 1] = '\0';
    } else if (strncmp(format, "DataWin: %.4s", 13) == 0) {
        finishChunk(now);
        memcpy(g_chunkName, text + 9, 4);
        g_chunkName[4] = '\0';
        g_chunkStart = now;
    } else if (strncmp(format, "Loaded \"", 8) == 0 || strncmp(format, "Unknown chunk", 13) == 0) {
        finishChunk(now);
        int n = snprintf(g_loadSummary, sizeof(g_loadSummary), "slowest chunks:");
        for (int i = 0; i < UT_LOAD_TOP && g_slowest[i].ms > 0; i++)
            n += snprintf(g_loadSummary + n, sizeof(g_loadSummary) - (size_t) n, " %s %u.%us", g_slowest[i].name,
                          g_slowest[i].ms / 1000, (g_slowest[i].ms % 1000) / 100);
    }
}

const char *utLogLoadSummary(void) {
    return g_loadSummary;
}

const char *utLogLoadPhases(void) {
    return g_loadPhases;
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
    trackLoad(format, text, now);

    size_t len = strlen(format);
    atLineStart = len > 0 && format[len - 1] == '\n';
}
