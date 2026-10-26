/*
 * Undertale: what the port does differently for it. See of_game.h.
 */

#include "of_game.h"
#include "of_buttons.h"
#include "sw_renderer.h"

#include <string.h>

#include "log.h"
#include "of_state_requests.h"
#include "stb_ds.h"

/* Prompts beyond the ones of_strings.c renames for every game. */
static const OfStringPatch g_strings[] = {
    /* Papyrus's date. */
    { "\\XSTEP ONE..^1. PRESS&THE [ C ] KEY ON&YOUR KEYBOARD FOR&\"\\RDATING HUD\\X.\"/",
      "\\XSTEP ONE..^1. PRESS&THE " OF_ICON_X " BUTTON ON&YOUR POCKET FOR&\"\\RDATING HUD\\X.\"/" },
    { "\\XSTEP ONE..^1. PRESS&THE [ C ] KEY ON&YOUR KEYBOARD FOR&\"\\RFRIENDSHIP HUD\\X.\"/",
      "\\XSTEP ONE..^1. PRESS&THE " OF_ICON_X " BUTTON ON&YOUR POCKET FOR&\"\\RFRIENDSHIP HUD\\X.\"/" },
};

/* Undertale v1.08's instruction screen lists five keys from a table, the
 * last being "[Hold ESC] - Quit". A Pocket core is left from the Pocket's
 * own menu, which the game cannot describe, so the list is cut to four by
 * changing the count the screen's code loads. The patch is applied only if
 * that code is exactly the build it was written against. */
#define UNDERTALE_NAMING_SCRIPT "gml_Script_scr_namingscreen"
#define UNDERTALE_NAMING_LENGTH 12332u
#define UNDERTALE_NAMING_COUNT_AT 0x2A58u
#define UNDERTALE_PUSH_FIVE 0x840F0005u
#define UNDERTALE_PUSH_FOUR 0x840F0004u

static void patchInstructions(DataWin *dw) {
    if (dw->bytecodeBuffer == NULL) return;
    for (uint32_t i = 0; i < dw->code.count; i++) {
        CodeEntry *entry = &dw->code.entries[i];
        if (!entry->present || entry->name == NULL || strcmp(entry->name, UNDERTALE_NAMING_SCRIPT) != 0) continue;
        if (entry->length != UNDERTALE_NAMING_LENGTH) return;

        uint8_t *at = dw->bytecodeBuffer + (entry->bytecodeAbsoluteOffset - dw->bytecodeBufferBase) + UNDERTALE_NAMING_COUNT_AT;
        uint32_t word;
        memcpy(&word, at, sizeof(word));
        if (word != UNDERTALE_PUSH_FIVE) return;
        word = UNDERTALE_PUSH_FOUR;
        memcpy(at, &word, sizeof(word));
        logInfo("Strings: instruction screen shortened to four lines\n");
        return;
    }
}

/* The attack prompt's Z is artwork, not text. Keep its PRESS sprite and
 * replace the separate keycap with the positional confirm icon. */
static bool drawPressButton(VMContext *ctx) {
    Instance *self = ctx->currentCallFrame->self;
    Renderer *renderer = ctx->runner->renderer;
    if (self == NULL || renderer == NULL) return false;
    BuiltinFunc drawSelf = VM_findBuiltin(ctx, "draw_self");
    if (drawSelf == NULL) return false;
    drawSelf(ctx, NULL, 0);
    ofButtonDraw(renderer, OF_ICON_FIRST, (float)self->x - 10, (float)self->y - 2, 26, 0xffffff, 1);
    return true;
}

static void installPrompts(Runner *runner) {
    bool installed = VM_setNativeCode(runner->vmContext, "gml_Object_obj_pressZ_Draw_0",
                                     252, 0xb8d5cacb73e73f07ull, drawPressButton);
    logInfo("Buttons: attack prompt %s\n", installed ? "installed" : "left unchanged (different code)");
}

/* Save states (of_state_requests.c) are taken anywhere the runner can store
 * exactly (Runner_snapshotCheck refuses the rest): the overworld, the game's
 * own menus, dialogue, battles, cutscenes, and the intro, title and naming
 * screens. A battle keeps the room it left as persistent saved state, which
 * snapshots carry since revision 6. Cutscenes used to be refused; restores
 * inside Papyrus's first scene (obj_papyrus1, the earliest that sets
 * obj_mainchara.cutscene) were checked exactly, as Deltarune's were. */
static const char *savestateReason(Runner *r) {
    if (r->dataWin->gen8.wadVersion != 16 || strcmp(r->dataWin->gen8.name, "UNDERTALE"))
        return "Unsupported game version";
    return NULL;
}

/* Key presses that walk from boot to the first battle; see runInputScript()
 * in of_platform.c for the syntax. Frame numbers assume a fresh save state. */
static const char g_script[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z,700:R*400,1100:L*10,1120:U*400,1560:Z,1605:Z,1650:Z,1695:Z,1740:Z,1785:Z,1830:Z,1875:Z,1920:Z,1965:Z,2010:Z,2055:Z,2100:Z,2145:Z,2190:Z,2235:Z,2280:Z,2325:Z,2370:Z,2415:Z,2460:Z,2505:Z,2550:Z,2595:Z,2640:Z,2685:Z";

/* Sections end on these frames; they follow the script above. */
static const OfBenchSection g_sections[] = {
    { "intro, menu, naming", 650, "intro" },
    { "first room 320x240", 1220, "room" },
    { "Flowey talk 320x240", 2060, "talk" },
    { "Flowey battle (640x480)", 2600, "battl" },
};

static void battleUiInstall(Runner *runner) {
    static const char *const sprites[] = {
        "spr_fightbt", "spr_fightbt_ja", "spr_fightbt_hollow", "spr_fightbt_hollow_ja",
        "spr_itembt", "spr_itembt_ja", "spr_itembt_hollow", "spr_itembt_hollow_ja",
        "spr_sparebt", "spr_sparebt_ja", "spr_sparebt_bandage", "spr_sparebt_bandage_ja",
        "spr_talkbt", "spr_talkbt_ja", "spr_talkbt_hollow", "spr_talkbt_hollow_ja",
        "spr_actbt_center", "spr_actbt_center_ja", "spr_actbt_center_hole", "spr_actbt_center_hole_ja",
        "spr_fightbt_center", "spr_fightbt_center_ja"
    };
    static const char *const fonts[] = {
        "fnt_curs", "fnt_plain", "fnt_plainbig", "fnt_comicsans", "fnt_papyrus",
        "fnt_ja_curs", "fnt_ja_plain", "fnt_ja_plainbig", "fnt_ja_plainsmall",
        "fnt_ja_comicsans", "fnt_ja_comicsans_big", "fnt_ja_papyrus", "fnt_ja_papyrus_btl"
    };
    SWRenderer_setContrastAssets(runner->renderer, sprites, sizeof(sprites) / sizeof(sprites[0]),
                                fonts, sizeof(fonts) / sizeof(fonts[0]));
    // Battle stats use white fnt_curs; the HP label is a transparent-ink sprite.
    static const char *const glyphSprites[] = { "spr_hpname", "spr_krmeter" };
    static const char *const whiteFonts[] = { "fnt_curs", "fnt_ja_curs" };
    SWRenderer_setContrastGlyphAssets(runner->renderer, glyphSprites, sizeof(glyphSprites) / sizeof(glyphSprites[0]),
                                     whiteFonts, sizeof(whiteFonts) / sizeof(whiteFonts[0]));
}

static bool battleUiActive(Runner *runner) {
    bool enabled = false;
    static const char *const rooms[] = {
        "room_battle", "room_floweybattle", "room_fastbattle", "room_storybattle",
        "room_fire_core_freebattle", "room_floweyx"
    };
    if (runner->currentRoom != NULL && runner->currentRoom->name != NULL) {
        for (unsigned i = 0; i < sizeof(rooms) / sizeof(rooms[0]); ++i)
            enabled |= strcmp(runner->currentRoom->name, rooms[i]) == 0;
    }
    return enabled;
}

const OfGame ofGame = {
    .installPrompts = installPrompts,
    .battleUiInstall = battleUiInstall,
    .battleUiActive = battleUiActive,
    .strings = g_strings,
    .stringCount = sizeof(g_strings) / sizeof(g_strings[0]),
    .patchCode = patchInstructions,
    .benchTitle = "Undertale",
    .benchScript = g_script,
    .benchSections = g_sections,
    .benchSectionCount = (int) (sizeof(g_sections) / sizeof(g_sections[0])),
    .ioDivisor = 1u,
    .ioAsyncBase = 110u,
    .ioAsyncStep = 2u,
    .savestateReason = savestateReason,
};
