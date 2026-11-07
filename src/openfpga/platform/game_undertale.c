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
#include "vm_builtins.h"

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

enum { PARTICLE_GOAL, PARTICLE_SIZE, PARTICLE_PHASE, PARTICLE_DIVISOR, PARTICLE_X_WAVE, PARTICLE_Y_WAVE, PARTICLE_FIELD_COUNT };
static int32_t particleVars[PARTICLE_FIELD_COUNT];
static bool particleVarsResolved;

static void forgetNativeVm(void) {
    particleVarsResolved = false;
}

static void setParticleReal(Instance *self, int32_t id, GMLReal number) {
    RValue value = RValue_makeReal(number);
    value.gmlStackType = GML_TYPE_VARIABLE; /* arithmetic result stored by pop.v.v */
    Instance_setSelfVar(self, id, value);
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool orangeParticleStep(VMContext *ctx) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    Instance *self = ctx->currentCallFrame->self;
    if (self == NULL || self->objectIndex == STRUCT_OBJECT_INDEX || ctx->runner == NULL) return false;
    GMLReal epsilon = ctx->runner->mathEpsilon;
    if (!isfinite(epsilon) || epsilon < 0 || !isfinite(self->imageAlpha) ||
        !isfinite(self->x) || !isfinite(self->y)) return false;
    GMLReal alpha = (GMLReal)self->imageAlpha - (GMLReal)0.01;
    /* The interpreter handles the last step, including the destroy event
     * and the instructions that still run after it. */
    if ((GMLReal)(float)alpha - (GMLReal)0.01 < -epsilon) return false;

    if (!particleVarsResolved) {
        static const char *const names[PARTICLE_FIELD_COUNT] = { "goalsize", "size", "siner", "gg", "rr", "vv" };
        for (int i = 0; i < PARTICLE_FIELD_COUNT; i++) particleVars[i] = VM_getOrAllocateVarID(ctx, names[i]);
        particleVarsResolved = true;
    }
    GMLReal values[PARTICLE_FIELD_COUNT];
    for (int i = 0; i < PARTICLE_FIELD_COUNT; i++) {
        RValue value = Instance_getSelfVar(self, particleVars[i]);
        if (value.type != RVALUE_REAL || !isfinite(value.real)) return false;
        values[i] = value.real;
    }
    if (values[PARTICLE_DIVISOR] == 0) return false;
    GMLReal phase = values[PARTICLE_PHASE] + (GMLReal)1;
    GMLReal angle = phase / values[PARTICLE_DIVISOR];
    if (!isfinite(angle)) return false;

    /* Compare the rounded opcode results with the runner's GML epsilon.
     * The second test sees the size after the first update. */
    GMLReal goal = values[PARTICLE_GOAL], size = values[PARTICLE_SIZE];
    if (goal - (size + (GMLReal)0.1) > epsilon) {
        size += (GMLReal)0.03;
        setParticleReal(self, particleVars[PARTICLE_SIZE], size);
    }
    if (goal - (size - (GMLReal)0.1) < -epsilon) {
        size -= (GMLReal)0.03;
        setParticleReal(self, particleVars[PARTICLE_SIZE], size);
    }
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_XSCALE, "image_xscale", RValue_makeReal(size), -1);
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_YSCALE, "image_yscale", RValue_makeReal(size), -1);
    self->imageAlpha = (float)alpha;
    setParticleReal(self, particleVars[PARTICLE_PHASE], phase);
    GMLReal dx = GMLReal_sin(angle) * values[PARTICLE_X_WAVE];
    GMLReal dy = GMLReal_cos(angle) * values[PARTICLE_Y_WAVE];
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_X, "x", RValue_makeReal((GMLReal)self->x + dx), -1);
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_Y, "y", RValue_makeReal((GMLReal)self->y + dy), -1);
    return true;
}

static const OfNativeCode g_natives[] = {
    { "gml_Object_obj_orangeparticle_Step_0", 392, 0x85cd9b4f052c0694ull, orangeParticleStep },
};

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
 * battles from across the game, each entered and left the way the game does
 * it, so its music starts and stops as in play. From a Ruins room (12, with
 * its music) the player walks a few steps, then the tour does what an
 * overworld encounter does (global.battlegroup and border set, an
 * obj_battleblcon made): the "!", the heart and the flash into room_battle.
 * The battle runs for about 350 frames with Z every 40 frames and the heart
 * moving between, HP kept topped up so no battle ends in a game over. Then
 * global.monster[0..2] is cleared, which is the game's own win: "YOU WON",
 * the battle music stopped, and a confirm press back to the Ruins room with
 * its music resumed. Each section is one visit, overworld included. Each
 * visit starts with a jump to the Ruins room, which changes nothing when the
 * last battle ended in time, but keeps a battle that did not from making the
 * next encounter inside room_battle (the game would then loop on it). A win
 * this way never takes a boss's own ending (Toriel's kill path writes
 * undertale.ini), and global.mercy, which some bosses raise to stop fleeing,
 * is reset before each encounter. Late bosses are entered without their
 * story set up, so they show their opening turns, not a real playthrough. */
static const char g_tourScript[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z,"
    "700:R*200,1030:L*20,1060:R*20,1160:Z,1168:U*20,1200:Z,1208:D*20,1240:Z,1248:L*20,1280:Z,1288:R*20,"
    "1320:Z,1328:U*20,1360:Z,1368:D*20,1400:Z,1408:L*20,1440:Z,1448:R*20,1480:Z,1488:U*20,1530:Z,1545:Z,"
    "1560:Z,1630:L*20,1660:R*20,1760:Z,1768:U*20,1800:Z,1808:D*20,1840:Z,1848:L*20,1880:Z,1888:R*20,1920:Z,"
    "1928:U*20,1960:Z,1968:D*20,2000:Z,2008:L*20,2040:Z,2048:R*20,2080:Z,2088:U*20,2130:Z,2145:Z,2160:Z,"
    "2230:L*20,2260:R*20,2360:Z,2368:U*20,2400:Z,2408:D*20,2440:Z,2448:L*20,2480:Z,2488:R*20,2520:Z,"
    "2528:U*20,2560:Z,2568:D*20,2600:Z,2608:L*20,2640:Z,2648:R*20,2680:Z,2688:U*20,2730:Z,2745:Z,2760:Z,"
    "2830:L*20,2860:R*20,2960:Z,2968:U*20,3000:Z,3008:D*20,3040:Z,3048:L*20,3080:Z,3088:R*20,3120:Z,"
    "3128:U*20,3160:Z,3168:D*20,3200:Z,3208:L*20,3240:Z,3248:R*20,3280:Z,3288:U*20,3330:Z,3345:Z,3360:Z,"
    "3430:L*20,3460:R*20,3560:Z,3568:U*20,3600:Z,3608:D*20,3640:Z,3648:L*20,3680:Z,3688:R*20,3720:Z,"
    "3728:U*20,3760:Z,3768:D*20,3800:Z,3808:L*20,3840:Z,3848:R*20,3880:Z,3888:U*20,3930:Z,3945:Z,3960:Z,"
    "4030:L*20,4060:R*20,4160:Z,4168:U*20,4200:Z,4208:D*20,4240:Z,4248:L*20,4280:Z,4288:R*20,4320:Z,"
    "4328:U*20,4360:Z,4368:D*20,4400:Z,4408:L*20,4440:Z,4448:R*20,4480:Z,4488:U*20,4530:Z,4545:Z,4560:Z";

static const OfJump g_tourJumps[] = {
    { 1000, 12, "encounter=0", NULL }, /* the Ruins room */
    { 1100, -1, "battlegroup=4,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Froggit */
    { 1250, -1, "hp=99", NULL },
    { 1350, -1, "hp=99", NULL },
    { 1450, -1, "hp=99", NULL },
    { 1500, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 1600, 12, "encounter=0", NULL },
    { 1700, -1, "battlegroup=22,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Toriel */
    { 1850, -1, "hp=99", NULL },
    { 1950, -1, "hp=99", NULL },
    { 2050, -1, "hp=99", NULL },
    { 2100, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 2200, 12, "encounter=0", NULL },
    { 2300, -1, "battlegroup=27,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Papyrus */
    { 2450, -1, "hp=99", NULL },
    { 2550, -1, "hp=99", NULL },
    { 2650, -1, "hp=99", NULL },
    { 2700, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 2800, 12, "encounter=0", NULL },
    { 2900, -1, "battlegroup=47,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Undyne */
    { 3050, -1, "hp=99", NULL },
    { 3150, -1, "hp=99", NULL },
    { 3250, -1, "hp=99", NULL },
    { 3300, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 3400, 12, "encounter=0", NULL },
    { 3500, -1, "battlegroup=81,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Mettaton EX */
    { 3650, -1, "hp=99", NULL },
    { 3750, -1, "hp=99", NULL },
    { 3850, -1, "hp=99", NULL },
    { 3900, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 4000, 12, "encounter=0", NULL },
    { 4100, -1, "battlegroup=101,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Asgore */
    { 4250, -1, "hp=99", NULL },
    { 4350, -1, "hp=99", NULL },
    { 4450, -1, "hp=99", NULL },
    { 4500, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
};

static const OfBenchSection g_tourSections[] = {
    { "boot to the first room", 1000, "boot" },
    { "Froggit (640x480)", 1599, "frog" },
    { "Toriel (640x480)", 2199, "tori" },
    { "Papyrus (640x480)", 2799, "papy" },
    { "Undyne (640x480)", 3399, "undy" },
    { "Mettaton EX (640x480)", 3999, "mtex" },
    { "Asgore (640x480)", 4599, "asgr" },
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
    .title = "Undertale",
    .dataFiles = "data.win, textures.bin and music.bin",
    .installPrompts = installPrompts,
    .natives = g_natives,
    .nativeCount = sizeof(g_natives) / sizeof(g_natives[0]),
    .nativesForgetVm = forgetNativeVm,
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
