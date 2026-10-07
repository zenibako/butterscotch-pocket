#ifndef UT_OF_DIAG_H
#define UT_OF_DIAG_H

#ifndef OF_PC
/* Unbuffer logs, report the heap, and halt (rather than reboot) on exit. */
void utDiagInstall(void);
/* Print the size of a registered data file, or halt if it cannot be opened. */
void utDiagCheckFile(const char *name);
/* Shows the text terminal with a final message and parks forever. */
void utDiagHalt(const char *why);
#else
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
bool utPlatformShowLogAndHalt(void);
static inline void utDiagHalt(const char *why) {
    printf("[undertale] %s\n", why);
    utPlatformShowLogAndHalt();
    exit(0);
}
static inline void utDiagInstall(void) {}
static inline void utDiagCheckFile(const char *name) { (void) name; }
#endif

#endif /* UT_OF_DIAG_H */
