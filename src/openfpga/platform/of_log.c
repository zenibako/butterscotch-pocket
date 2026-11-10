/*
 * Log sink for Butterscotch: prefixes every line with seconds since start,
 * so load stages can be timed from the on-device terminal.
 */

#include "log.h"
#include "gettime.h"
#include "of_hooks.h"
#include "of_perf.h"
#ifndef OF_HOST_TOOL
#include "of_loading.h"
#endif
#include "of_save_format.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The last few lines, kept for the on-screen log overlay. */
static char g_lines[OF_LOG_LINES][OF_LOG_LINE_LEN];
static int g_lineHead = 0; /* line currently being written */
static int g_linePos = 0;

/* Writing a line to the OS console costs about 20 ms on the Pocket, and the
 * console is not visible once the game has its own video mode, so the
 * platform turns it off after the first frame. The overlay buffer is kept. */
static bool g_console = true;

void ofLogSetConsole(bool enabled) {
    g_console = enabled;
}

/* The Makefile names these after the game being built. */
#ifndef OF_GAME_NAME
#error "OF_GAME_NAME is not set; build with the Makefile (GAME=undertale or GAME=deltarune)"
#endif
#ifndef OF_LOG_SLOT_FILE
#error "OF_LOG_SLOT_FILE is not set; build with the Makefile"
#endif

/* Everything logged since start, for ofLogDump. When it fills up, the older
 * half goes and a line says so. */
#define OF_LOG_HISTORY (160u * 1024u)
static char g_history[OF_LOG_HISTORY];
static uint32_t g_historyLen = 0;
static bool g_historyCut = false;

static void keepHistory(const char *text) {
    size_t len = strlen(text);
    if (len >= OF_LOG_HISTORY / 2) return;
    if (g_historyLen + len > OF_LOG_HISTORY) {
        /* Drop the older half, from the start of a line. */
        uint32_t from = OF_LOG_HISTORY / 2;
        while (from < g_historyLen && g_history[from - 1] != '\n') from++;
        memmove(g_history, g_history + from, g_historyLen - from);
        g_historyLen -= from;
        g_historyCut = true;
    }
    memcpy(g_history + g_historyLen, text, len);
    g_historyLen += (uint32_t) len;
}

/* Writes the whole log as text to the game's second save slot file
 * (<game>_1.sav), which the core definition already names and no game uses.
 * A core can only write to its save slots, and the Pocket copies those back
 * to the card (Saves/butterscotch/common/) when the core is left through the
 * Analogue menu: this is how a log gets off the device without a screenshot,
 * which holds 21 short lines.
 *
 * What the file held from earlier runs is kept in front of this run's log:
 * every benchmark report whole (they are what a run is made for, and small),
 * then as much of the file's end as fits. Each run's own log starts with a
 * "Butterscotch log" line.
 * Returns false if the file could not be written. */
#define OF_LOG_REPORTS (24u * 1024u)
#define OF_LOG_EARLIER (48u * 1024u)
#define OF_LOG_HEADER "Butterscotch log, " OF_GAME_NAME "\n"

bool ofLogDump(void) {
    /* Read the earlier runs once; later dumps in this run replace only this run's part. */
    static char *earlier = NULL;
    static uint32_t earlierLen = 0;
    static bool earlierRead = false;
    if (!earlierRead) {
        earlierRead = true;
        FILE *old = fopen(OF_LOG_SLOT_FILE, "rb");
        char *text = old != NULL ? malloc(OF_SAVE_SLOT_BYTES) : NULL;
        if (text != NULL) {
            /* In pieces: one read for more than the slot holds came back empty on the device. */
            size_t got = 0;
            while (got < OF_SAVE_SLOT_BYTES - 1) {
                size_t want = OF_SAVE_SLOT_BYTES - 1 - got;
                size_t piece = fread(text + got, 1, want < 4096 ? want : 4096, old);
                if (piece == 0) break;
                got += piece;
            }
            text[got] = '\0';
            size_t len = strlen(text); /* the text ends at the first NUL */
            /* A slot never written, or holding something else, reads as anything at all. */
            if (len > 0 && strncmp(text, "Butterscotch log, ", 18) == 0) {
                /* Kept: every benchmark report in the file, whole (a run's log is half the slot, so only
                 * the last one or two would survive otherwise), then as much of the file's end as fits. */
                char *kept = malloc(OF_LOG_REPORTS + OF_LOG_EARLIER + 64);
                size_t keptLen = 0;
                if (kept != NULL) {
                    for (char *at = text; (at = strstr(at, "=== ")) != NULL; at += 4) {
                        if (at != text && at[-1] != '\n') continue;
                        char *end = strstr(at, "--- end of report ---");
                        char *next = strstr(at + 4, "\n=== ");
                        if (end == NULL || (next != NULL && next < end)) continue; /* not a whole report */
                        end = strchr(end, '\n');
                        size_t blockLen = end != NULL ? (size_t) (end - at) + 1 : strlen(at);
                        /* The same report may be in the file twice: kept by an earlier dump, and in the log it came from. */
                        bool seen = false;
                        for (size_t k = 0; k + blockLen <= keptLen && !seen; k++)
                            seen = memcmp(kept + k, at, blockLen) == 0;
                        if (seen || blockLen > OF_LOG_REPORTS) continue;
                        if (keptLen + blockLen > OF_LOG_REPORTS) { /* drop the oldest half to make room */
                            size_t drop = keptLen / 2;
                            while (drop < keptLen && kept[drop - 1] != '\n') drop++;
                            memmove(kept, kept + drop, keptLen - drop);
                            keptLen -= drop;
                            if (keptLen + blockLen > OF_LOG_REPORTS) keptLen = 0; /* still too big: keep only this one */
                        }
                        memcpy(kept + keptLen, at, blockLen);
                        keptLen += blockLen;
                    }
                    if (keptLen > 0) kept[keptLen++] = '\n';
                    size_t from = len > OF_LOG_EARLIER ? len - OF_LOG_EARLIER : 0;
                    while (from > 0 && from < len && text[from - 1] != '\n') from++;
                    memcpy(kept + keptLen, text + from, len - from);
                    keptLen += len - from;
                    earlier = kept;
                    earlierLen = (uint32_t) keptLen;
                }
            }
            free(text);
        }
        if (old != NULL) fclose(old);
    }

    FILE *file = fopen(OF_LOG_SLOT_FILE, "wb");
    if (file == NULL) return false;
    static const char cut[] = "(the start of this run's log no longer fitted and was dropped)\n";
    /* The file always starts with this line: it is how the next run knows the slot holds a log. */
    bool ok = fputs("Butterscotch log, file for " OF_GAME_NAME ": reports from earlier runs, the end of the last log, then this run\n\n", file) >= 0;
    if (earlier != NULL) {
        ok = fwrite(earlier, 1, earlierLen, file) == earlierLen;
        if (earlierLen > 0 && earlier[earlierLen - 1] != '\n') ok = ok && fputc('\n', file) != EOF;
        ok = ok && fputs("\n", file) >= 0;
    }
    ok = ok && fputs(OF_LOG_HEADER, file) >= 0;
    if (g_historyCut) ok = ok && fputs(cut, file) >= 0;
    ok = ok && fwrite(g_history, 1, g_historyLen, file) == g_historyLen;
    /* Text ends here; whatever the slot held beyond this is cut off by the NUL for a reader that stops at one. */
    ok = ok && fputc('\0', file) != EOF;
    return fclose(file) == 0 && ok;
}

static void keepText(const char *text) {
    keepHistory(text);
    for (; *text != '\0'; text++) {
        if (*text == '\n') {
            g_lineHead = (g_lineHead + 1) % OF_LOG_LINES;
            g_linePos = 0;
            g_lines[g_lineHead][0] = '\0';
        } else if (g_linePos < OF_LOG_LINE_LEN - 1) {
            g_lines[g_lineHead][g_linePos++] = *text;
            g_lines[g_lineHead][g_linePos] = '\0';
        }
    }
}

const char *ofLogLine(int age) {
    if (age < 0 || age >= OF_LOG_LINES - 1) return "";
    /* The head line is still being written; age 0 is the last complete one. */
    return g_lines[(g_lineHead - 1 - age + 2 * OF_LOG_LINES) % OF_LOG_LINES];
}

/* Load breakdown: DATAWIN_LOG_CHUNKS makes the loader log "DataWin: NAME, ..."
 * as it starts each chunk. Time between consecutive lines is the time spent
 * on the earlier chunk; the slowest few are kept so the benchmark report can
 * show where loading time goes after the boot log has scrolled away. */
#define OF_LOAD_TOP 4
static char g_chunkName[5];
static uint64_t g_chunkStart = 0;
static struct { char name[5]; unsigned ms; } g_slowest[OF_LOAD_TOP];
static char g_loadSummary[OF_LOG_LINE_LEN];
static char g_loadPhases[OF_LOG_LINE_LEN];

static void finishChunk(uint64_t now) {
    if (g_chunkStart == 0) return;
    unsigned ms = (unsigned) ((now - g_chunkStart) / 1000000u);
    for (int i = 0; i < OF_LOAD_TOP; i++) {
        if (ms <= g_slowest[i].ms) continue;
        for (int j = OF_LOAD_TOP - 1; j > i; j--) g_slowest[j] = g_slowest[j - 1];
        memcpy(g_slowest[i].name, g_chunkName, sizeof(g_chunkName));
        g_slowest[i].ms = ms;
        break;
    }
    g_chunkStart = 0;
}

/* Room loads announce themselves in the log; their time goes into the
 * slow-frame report (see of_perf.h). The host tools share this file
 * but have no frames to report on. */
#ifdef OF_HOST_TOOL
static void trackFrameLoads(const char *format, const char *text, uint64_t now) {
    (void) format; (void) text; (void) now;
}
#else
static void trackFrameLoads(const char *format, const char *text, uint64_t now) {
    static uint64_t roomStart = 0;
    ofLoadingNote(format, text);
    if (strncmp(format, "Room changed:", 13) == 0) {
        roomStart = now;
        ofAudioRoomChange();
    } else if (strncmp(format, "Runner: Room loaded:", 20) == 0) {
        if (roomStart != 0) ofPerfAddLoad(OF_LOAD_ROOM, now - roomStart);
        roomStart = 0;
    }
}
#endif

static void trackLoad(const char *format, const char *text, uint64_t now) {
    trackFrameLoads(format, text, now);
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
        for (int i = 0; i < OF_LOAD_TOP && g_slowest[i].ms > 0; i++)
            n += snprintf(g_loadSummary + n, sizeof(g_loadSummary) - (size_t) n, " %s %u.%us", g_slowest[i].name,
                          g_slowest[i].ms / 1000, (g_slowest[i].ms % 1000) / 100);
    }
}

const char *ofLogLoadSummary(void) {
    return g_loadSummary;
}

const char *ofLogLoadPhases(void) {
    return g_loadPhases;
}

void ofLogPrint(const char *format, ...) {
    char text[256];
    va_list va;
    va_start(va, format);
    vsnprintf(text, sizeof(text), format, va);
    va_end(va);
    if (g_console) fputs(text, stdout);
    keepText(text);
}

/* The last error logged, for the loading screen's error (of_loading.c). An
 * error can be logged in pieces; a new one starts at the start of a line. */
static char g_lastError[OF_LOG_LINE_LEN];

const char *ofLogLastError(void) {
    return g_lastError;
}

static void keepError(const char *text, bool newError) {
    if (newError) g_lastError[0] = '\0';
    size_t len = strlen(g_lastError);
    size_t room = sizeof(g_lastError) - 1 - len, add = strlen(text);
    if (add > room) add = room;
    memcpy(g_lastError + len, text, add);
    len += add;
    g_lastError[len] = '\0';
    while (len > 0 && (g_lastError[len - 1] == '\n' || g_lastError[len - 1] == ' ')) g_lastError[--len] = '\0';
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
        if (g_console) fputs(text, stdout);
        keepText(text);
    }

    const char *prefix = "";
    switch (type) {
        case LOG_TYPE_WARNING: prefix = "Warning: "; break;
        case LOG_TYPE_ERROR:   prefix = "Error: ";   break;
        case LOG_TYPE_DEBUG:   prefix = "Debug: ";   break;
        case LOG_TYPE_NORMAL:  break;
    }
    if (g_console) fputs(prefix, stdout);
    keepText(prefix);

    vsnprintf(text, sizeof(text), format, va);
    if (g_console) fputs(text, stdout);
    keepText(text);
    if (type == LOG_TYPE_ERROR) keepError(text, atLineStart);
    trackLoad(format, text, now);

    size_t len = strlen(format);
    atLineStart = len > 0 && format[len - 1] == '\n';
}
