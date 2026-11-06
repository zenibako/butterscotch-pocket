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

/* The battle tour ("--bench-battles"): a walk to the first room, then six
 * battles from across the game, each entered by setting the battle group and
 * going to room_battle (306) from room_area1 (4), and played for 450 frames
 * with Z every 40 frames and the heart moving between. HP is kept topped up so
 * no battle ends in a game over. Late bosses are entered without their story
 * set up, so they show their opening turns, not a real playthrough. */
static const char g_tourScript[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z,"
    "700:R*200,1061:Z,1069:U*20,1101:Z,1109:D*20,1141:Z,1149:L*20,1181:Z,1189:R*20,1221:Z,1229:U*20,1261:Z,"
    "1269:D*20,1301:Z,1309:L*20,1341:Z,1349:R*20,1381:Z,1389:U*20,1421:Z,1429:D*20,1461:Z,1469:L*20,1501:Z,"
    "1509:R*20,1541:Z,1549:U*20,1581:Z,1589:D*20,1621:Z,1629:L*20,1661:Z,1669:R*20,1701:Z,1709:U*20,1741:Z,"
    "1749:D*20,1781:Z,1789:L*20,1821:Z,1829:R*20,1861:Z,1869:U*20,1901:Z,1909:D*20,1941:Z,1949:L*20,1981:Z,"
    "1989:R*20,2021:Z,2029:U*20,2061:Z,2069:D*20,2101:Z,2109:L*20,2141:Z,2149:R*20,2181:Z,2189:U*20,2221:Z,"
    "2229:D*20,2261:Z,2269:L*20,2301:Z,2309:R*20,2381:Z,2389:D*20,2421:Z,2429:L*20,2461:Z,2469:R*20,2501:Z,"
    "2509:U*20,2541:Z,2549:D*20,2581:Z,2589:L*20,2621:Z,2629:R*20,2661:Z,2669:U*20,2701:Z,2709:D*20,2741:Z,"
    "2749:L*20,2781:Z,2789:R*20,2821:Z,2829:U*20,2861:Z,2869:D*20,2901:Z,2909:L*20,2941:Z,2949:R*20,2981:Z,"
    "2989:U*20,3021:Z,3029:D*20,3061:Z,3069:L*20,3101:Z,3109:R*20,3141:Z,3149:U*20,3181:Z,3189:D*20,3221:Z,"
    "3229:L*20,3261:Z,3269:R*20,3301:Z,3309:U*20,3341:Z,3349:D*20,3381:Z,3389:L*20,3421:Z,3429:R*20,3461:Z,"
    "3469:U*20,3501:Z,3509:D*20,3541:Z,3549:L*20,3581:Z,3589:R*20,3621:Z,3629:U*20,3661:Z,3669:D*20";

static const OfJump g_tourJumps[] = {
    { 1000, -1, "battlegroup=4,hp=99,maxhp=99" }, /* Froggit */
    { 1001, 306, NULL }, /* room_battle */
    { 1101, -1, "hp=99" },
    { 1201, -1, "hp=99" },
    { 1301, -1, "hp=99" },
    { 1401, -1, "hp=99" },
    { 1449, 4, NULL }, /* out of the last battle */
    { 1450, -1, "battlegroup=22,hp=99,maxhp=99" }, /* Toriel */
    { 1451, 306, NULL }, /* room_battle */
    { 1551, -1, "hp=99" },
    { 1651, -1, "hp=99" },
    { 1751, -1, "hp=99" },
    { 1851, -1, "hp=99" },
    { 1899, 4, NULL }, /* out of the last battle */
    { 1900, -1, "battlegroup=27,hp=99,maxhp=99" }, /* Papyrus */
    { 1901, 306, NULL }, /* room_battle */
    { 2001, -1, "hp=99" },
    { 2101, -1, "hp=99" },
    { 2201, -1, "hp=99" },
    { 2301, -1, "hp=99" },
    { 2349, 4, NULL }, /* out of the last battle */
    { 2350, -1, "battlegroup=47,hp=99,maxhp=99" }, /* Undyne */
    { 2351, 306, NULL }, /* room_battle */
    { 2451, -1, "hp=99" },
    { 2551, -1, "hp=99" },
    { 2651, -1, "hp=99" },
    { 2751, -1, "hp=99" },
    { 2799, 4, NULL }, /* out of the last battle */
    { 2800, -1, "battlegroup=81,hp=99,maxhp=99" }, /* Mettaton EX */
    { 2801, 306, NULL }, /* room_battle */
    { 2901, -1, "hp=99" },
    { 3001, -1, "hp=99" },
    { 3101, -1, "hp=99" },
    { 3201, -1, "hp=99" },
    { 3249, 4, NULL }, /* out of the last battle */
    { 3250, -1, "battlegroup=101,hp=99,maxhp=99" }, /* Asgore */
    { 3251, 306, NULL }, /* room_battle */
    { 3351, -1, "hp=99" },
    { 3451, -1, "hp=99" },
    { 3551, -1, "hp=99" },
    { 3651, -1, "hp=99" },
};

static const OfBenchSection g_tourSections[] = {
    { "boot to the first room", 1000, "boot" },
    { "Froggit (640x480)", 1448, "frog" },
    { "Toriel (640x480)", 1898, "tori" },
    { "Papyrus (640x480)", 2348, "papy" },
    { "Undyne (640x480)", 2798, "undy" },
    { "Mettaton EX (640x480)", 3248, "mtex" },
    { "Asgore (640x480)", 3698, "asgr" },
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
    .speedFullResBattles = true,
    .strings = g_strings,
    .stringCount = sizeof(g_strings) / sizeof(g_strings[0]),
    .patchCode = patchInstructions,
    .benchTitle = "Undertale",
    .benchScript = g_script,
    .benchSections = g_sections,
    .benchSectionCount = (int) (sizeof(g_sections) / sizeof(g_sections[0])),
    .tourTitle = "Undertale battle tour",
    .tourScript = g_tourScript,
    .tourJumps = g_tourJumps,
    .tourJumpCount = (int) (sizeof(g_tourJumps) / sizeof(g_tourJumps[0])),
    .tourSections = g_tourSections,
    .tourSectionCount = (int) (sizeof(g_tourSections) / sizeof(g_tourSections[0])),
    .ioDivisor = 1u,
    .ioAsyncBase = 110u,
    .ioAsyncStep = 2u,
    .savestateReason = savestateReason,
};
