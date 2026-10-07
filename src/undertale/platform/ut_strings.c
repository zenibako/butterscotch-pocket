/*
 * Rewrites the handful of game strings that name keyboard keys so they name
 * the Pocket's buttons instead (see the mapping in of_platform.c).
 *
 * The user's data.win is never modified: the strings are patched in memory
 * after loading. Every replacement is no longer than the text it replaces,
 * so it is written over the original in the string table, which covers every
 * way the runner reaches a string.
 */

#include "ut_strings.h"

#include "log.h"

#include <string.h>

typedef struct {
    const char *from;
    const char *to;
} UtStringPatch;

static const UtStringPatch g_patches[] = {
    /* Title screen and the instruction screen. */
    { "[PRESS Z OR ENTER]", "[PRESS A OR START]" },
    { "[Z or ENTER]", "[A or START]" },
    { "[X or SHIFT]", "[B]" },
    { "[C or CTRL]", "[X or Y]" },
    /* F4 toggled fullscreen; here L toggles smoothed rendering of the
     * 640x480 screens. Esc-to-quit becomes the Pocket's own menu button. */
    { "[F4]", "[L]" },
    { "Fullscreen", "Smoothing" },
    { "[Hold ESC]", "[Analogue]" },
    /* Settings / joystick screens. */
    { "[Z / ENTER]", "[A / START]" },
    { "[X / SHIFT]", "[B]" },
    { "[C / CTRL]", "[X / Y]" },
    /* Key names substituted into dialogue. */
    { "[Z]", "[A]" },
    { "[X]", "[B]" },
    { "[C]", "[X]" },
    /* Papyrus's date. */
    { "\\XSTEP ONE..^1. PRESS&THE [ C ] KEY ON&YOUR KEYBOARD FOR&\"\\RDATING HUD\\X.\"/",
      "\\XSTEP ONE..^1. PRESS&THE [ X ] KEY ON&YOUR POCKET FOR&\"\\RDATING HUD\\X.\"/" },
    { "\\XSTEP ONE..^1. PRESS&THE [ C ] KEY ON&YOUR KEYBOARD FOR&\"\\RFRIENDSHIP HUD\\X.\"/",
      "\\XSTEP ONE..^1. PRESS&THE [ X ] KEY ON&YOUR POCKET FOR&\"\\RFRIENDSHIP HUD\\X.\"/" },
};
#define UT_PATCH_COUNT (sizeof(g_patches) / sizeof(g_patches[0]))

void utPatchStrings(DataWin *dw) {
    if (dw == NULL || dw->strg.strings == NULL) return;

    unsigned patched = 0;
    for (uint32_t i = 0; i < dw->strg.count; i++) {
        char *text = (char *) dw->strg.strings[i];
        if (text == NULL || (text[0] != '[' && text[0] != 'F' && text[0] != '\\')) continue;

        for (size_t p = 0; p < UT_PATCH_COUNT; p++) {
            if (strcmp(text, g_patches[p].from) != 0) continue;
            if (strlen(g_patches[p].to) <= strlen(g_patches[p].from)) {
                strcpy(text, g_patches[p].to);
                patched++;
            }
            break;
        }
    }
    logInfo("Strings: %u key prompts renamed for the Pocket's buttons\n", patched);
}
