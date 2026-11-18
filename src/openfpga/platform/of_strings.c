/*
 * Rewrites the handful of game strings that name keyboard keys so they name
 * the Pocket's buttons instead (see the mapping in of_platform.c).
 *
 * The user's data.win is never modified: the strings are patched in memory
 * after loading. A replacement no longer than the text it replaces is written
 * over the original in the string table, which covers every way the runner
 * reaches a string. A longer one cannot be, so the table's entry is pointed
 * at the new text instead; that reaches code that looks the string up when it
 * runs (all the instruction screen needs), not anything that kept the old
 * pointer from loading.
 *
 * The rows here are the prompts every game might show; a game adds its own
 * in its game_<name>.c, with any change to its code that a prompt needs
 * (Undertale's instruction screen loses a line that way).
 */

#include "of_strings.h"

#include "log.h"
#include "of_game.h"
#include "of_buttons.h"

#include <string.h>

static const OfStringPatch g_patches[] = {
    /* Title screen and the instruction screen. The instruction screen and
     * the Pocket's own controls screen (dist/.../input.json) describe the
     * same buttons: keep their wording the same. Debug mode's buttons are
     * named on the Pocket's screen only. */
    { "[PRESS Z OR ENTER]", "[PRESS " OF_ICON_A " OR " OF_ICON_START "]" },
    { "[Z or ENTER]", "[" OF_ICON_A " or " OF_ICON_START "]" },
    { "[X or SHIFT]", OF_ICON_B },
    { "[C or CTRL]", "[" OF_ICON_X " or " OF_ICON_Y "]" },
    /* Reuse the fullscreen row for the port menu. The Esc-to-quit row
     * has no counterpart; Undertale's game_undertale.c drops it. */
    { "[F4]", OF_ICON_SELECT },
    { "Fullscreen", "Butterscotch menu" },
    /* Settings / joystick screens. */
    { "[Z / ENTER]", "[" OF_ICON_A " / " OF_ICON_START "]" },
    { "[X / SHIFT]", OF_ICON_B },
    { "[C / CTRL]", "[" OF_ICON_X " / " OF_ICON_Y "]" },
    /* The dialogue writer advances fixed character cells, not glyph widths.
     * A blank second cell reserves 16 (or 32 doubled) pixels for a 13px icon. */
    { "[Z]", OF_ICON_A " " },
    { "[X]", OF_ICON_B " " },
    { "[C]", OF_ICON_X " " },
};
#define OF_PATCH_COUNT (sizeof(g_patches) / sizeof(g_patches[0]))

/* Replaces string i if a row matches it. */
static bool patchOne(DataWin *dw, uint32_t i, char *text, const OfStringPatch *patches, size_t count) {
    for (size_t p = 0; p < count; p++) {
        if (strcmp(text, patches[p].from) != 0) continue;
        if (strlen(patches[p].to) <= strlen(patches[p].from))
            strcpy(text, patches[p].to);
        else
            dw->strg.strings[i] = (char *) patches[p].to;
        return true;
    }
    return false;
}

void ofPatchStrings(DataWin *dw) {
    if (dw == NULL || dw->strg.strings == NULL) return;

    unsigned patched = 0;
    for (uint32_t i = 0; i < dw->strg.count; i++) {
        char *text = (char *) dw->strg.strings[i];
        if (text == NULL || (text[0] != '[' && text[0] != 'F' && text[0] != '\\')) continue;

        if (patchOne(dw, i, text, g_patches, OF_PATCH_COUNT) || patchOne(dw, i, text, ofGame.strings, ofGame.stringCount))
            patched++;
    }
    logInfo("Strings: %u key prompts renamed for the Pocket's buttons\n", patched);
    if (ofGame.patchCode != NULL) ofGame.patchCode(dw);
}
