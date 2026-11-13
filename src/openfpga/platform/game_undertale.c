/*
 * Undertale: what the port does differently for it. See of_game.h.
 */

#include "of_game.h"
#include "of_buttons.h"
#include "sw_renderer.h"

#include <string.h>

#include "binary_utils.h"
#include "collision.h"
#include "gettime.h"
#include "of_perf.h"
#include "log.h"
#include "of_state_requests.h"
#include "op.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm_builtins.h"

extern bool swrSmoothMinify; /* The replacement pipe art requires half-size Speed rendering. */

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

/* Each native's ids are looked up once per VM; these say whether they have
 * been, and forgetNativeVm clears them when the VM changes. The Flowey ones
 * belong to game_undertale_flowey.h, the snow patch's to
 * game_undertale_snowfloor.h. */
static bool particleVarsResolved;
static bool floweyStepVarsResolved;
static bool pipeVarsResolved;
static bool backdropVarsResolved;
static bool snowVarsResolved;
#ifndef ENABLE_VM_TRACING
static bool flipeyeSkippedReady;
static bool mouthSkippedReady;
static bool previewSkippedReady;
static bool leftEyeSkippedReady;
#endif

/* Variable ids the Speed decisions below look up (speedHalfBattle, speedSkipRun). */
static bool speedVarsResolved;

static void forgetNativeVm(void) {
    speedVarsResolved = false;
    particleVarsResolved = false;
    floweyStepVarsResolved = false;
    pipeVarsResolved = false;
    backdropVarsResolved = false;
    snowVarsResolved = false;
#ifndef ENABLE_VM_TRACING
    flipeyeSkippedReady = false;
    mouthSkippedReady = false;
    previewSkippedReady = false;
    leftEyeSkippedReady = false;
#endif
}

static void setNativeReal(Instance *self, int32_t id, GMLReal number) {
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
        setNativeReal(self, particleVars[PARTICLE_SIZE], size);
    }
    if (goal - (size - (GMLReal)0.1) < -epsilon) {
        size -= (GMLReal)0.03;
        setNativeReal(self, particleVars[PARTICLE_SIZE], size);
    }
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_XSCALE, "image_xscale", RValue_makeReal(size), -1);
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_YSCALE, "image_yscale", RValue_makeReal(size), -1);
    self->imageAlpha = (float)alpha;
    setNativeReal(self, particleVars[PARTICLE_PHASE], phase);
    GMLReal dx = GMLReal_sin(angle) * values[PARTICLE_X_WAVE];
    GMLReal dy = GMLReal_cos(angle) * values[PARTICLE_Y_WAVE];
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_X, "x", RValue_makeReal((GMLReal)self->x + dx), -1);
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_Y, "y", RValue_makeReal((GMLReal)self->y + dy), -1);
    return true;
}

static bool readNativeNumber(Instance *instance, int32_t id, GMLReal *out) {
    RValue value = Instance_getSelfVar(instance, id);
    if (value.type != RVALUE_REAL && value.type != RVALUE_INT32) return false;
    *out = RValue_toReal(value);
    return isfinite(*out);
}

#include "game_undertale_flowey.h"

#ifndef ENABLE_VM_TRACING
static const OfNativeScript g_nativeScripts[] = {
    { "gml_Object_obj_floweyx_lefteye_Draw_0", FLOWEY_LEFT_EYE_DRAW_LENGTH, 0x1cd174a60c76f9a2ull, floweyLeftEyeDraw },
    { "gml_Object_obj_gigavine_preview_Draw_0", FLOWEY_PREVIEW_DRAW_LENGTH, 0xc4b29d651d8f182full, floweyVinePreviewDraw },
    { "gml_Object_obj_floweyx_flipeye_Draw_0", FLOWEY_FLIPEYE_DRAW_LENGTH, 0x2ffb1453038ba855ull, floweyFlipeyeDraw },
    { "gml_Object_obj_floweyx_mouth_Draw_0", FLOWEY_MOUTH_DRAW_LENGTH, 0xee4dc253c9466abbull, floweyMouthDraw },
};
#endif

#include "game_undertale_snowfloor.h"

static const OfNativeCode g_natives[] = {
    { "gml_Object_obj_gigavine_Step_0", 540, 0x4097e114074c9429ull, floweyVineStep },
    { "gml_Object_obj_eyeshot_a_Step_0", 108, 0x18f12b40727b60b2ull, floweyEyeShotStep },
    { "gml_Object_obj_orangeparticle_Step_0", 392, 0x85cd9b4f052c0694ull, orangeParticleStep },
    { "gml_Object_obj_floweypipetest_Draw_0", 2292, 0xd060939f37f20db1ull, floweyPipeDraw1 },
    { "gml_Object_obj_floweypipetest_2_Draw_0", 2496, 0xc784a157fab0292eull, floweyPipeDraw2 },
    { "gml_Object_obj_floweypipetest_3_Draw_0", 2148, 0x543e2f97612cf23cull, floweyPipeDraw3 },
    { "gml_Object_obj_flowey_bgdraw_Draw_0", 272, 0x746a6e5ff0305b36ull, floweyBackgroundMotion },
    { "gml_Object_obj_floweyarm_Draw_0", 1824, 0x92efb78232475f92ull, floweyIdleArm },
    { "gml_Object_obj_fleshface_Draw_0", 676, 0x8f63731ba96dfb2bull, floweyFleshFace },
    { "gml_Object_obj_sidestalk_Draw_0", 376, 0xe8f07e7abe5af547ull, floweySideStalk },
    { "gml_Object_obj_bgpipe_Draw_0", 964, 0x12bf9b0542ba714cull, floweyBackdropPipes },
    { "gml_Object_obj_vines_flowey_Draw_0", 704, 0xec70756e7a99f41aull, floweyBackdropVines },
    { "gml_Object_obj_snowfloor_Draw_0", 2204, 0x659c1fd61851691eull, snowfloorDraw },
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

#include "game_undertale_bench.h"

static void battleUiInstall(Runner *runner) {
    SWRenderer_setFloweySpeedAssets(runner->renderer);
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

/* The battles Speed mode keeps at half size (speedHalfBattle): those estimated
 * from the battle-tour measurements of 2026-11-13 to need more than a frame's
 * 33 ms on the Pocket at full size, even drawing every other frame. They are
 * global.battlegroup values. Battles in the other battle rooms (Flowey's) keep
 * half size, as they had. */
static const int16_t g_speedHalfBattles[] = {
    2, 5, 6, 25, 31, 36, 41, 48, 49, 50, 51, 52, 55, 56, 57, 58, 59, 60, 64, 65, 67, 68, 76, 77,
    78, 79, 80, 81, 83, 85, 87, 88, 89, 90, 92, 93, 94, 101, 120, 121, 123, 124, 128, 129, 132, 133, 134, 135, 256,
};

/* The ids those decisions read, found again for each VM (forgetNativeVm) and data file. */
static int32_t g_speedGroupVar, g_speedIntroVar, g_speedHpVar, g_speedMasterObject, g_encounterObject;
static const void *g_speedObjectsOf;

static void resolveSpeedVars(Runner *runner) {
    if (!speedVarsResolved) {
        VMContext *ctx = runner->vmContext;
        g_speedGroupVar = VM_getOrAllocateVarID(ctx, "battlegroup");
        g_speedIntroVar = VM_getOrAllocateVarID(ctx, "introcon");
        g_speedHpVar = VM_getOrAllocateVarID(ctx, "hp");
        speedVarsResolved = true;
    }
    if (g_speedObjectsOf != runner->dataWin) {
        g_speedMasterObject = g_encounterObject = -1;
        for (uint32_t i = 0; i < runner->dataWin->objt.count; i++) {
            const char *name = runner->dataWin->objt.objects[i].name;
            if (name != NULL && strcmp(name, "obj_flowey_master") == 0) g_speedMasterObject = (int32_t) i;
            if (name != NULL && strcmp(name, "obj_battleblcon") == 0) g_encounterObject = (int32_t) i;
        }
        g_speedObjectsOf = runner->dataWin;
    }
}

static bool speedHalfBattle(Runner *runner) {
    if (runner->currentRoom == NULL || runner->currentRoom->name == NULL ||
        strcmp(runner->currentRoom->name, "room_battle") != 0 || runner->dataWin == NULL)
        return true;
    VMContext *ctx = runner->vmContext;
    if (ctx == NULL || ctx->globalScopeInstance == NULL) return true;
    resolveSpeedVars(runner);
    RValue group = Instance_getSelfVar(ctx->globalScopeInstance, g_speedGroupVar);
    int id = (int) RValue_toReal(group);
    for (unsigned i = 0; i < sizeof(g_speedHalfBattles) / sizeof(g_speedHalfBattles[0]); ++i)
        if (g_speedHalfBattles[i] == id) return true;
    return false;
}

/* Speed's skipping (speedSkipRun): in Flowey's fight (room_floweyx) once its
 * intro is over, two updates in a row may go undrawn. Its draws take 52-57 ms on
 * the Pocket, longer than one skipped update can make up. The intro's own
 * marker says when it is over: obj_flowey_master's introcon becomes -1 at the
 * end of its authored intro. That, and the player still having HP, is checked
 * every update; anything else, or anything missing, keeps the usual one. */
static bool numeric(RValue value) {
    return value.type == RVALUE_REAL || value.type == RVALUE_INT32 || value.type == RVALUE_INT64;
}

static int speedSkipRun(Runner *runner) {
    if (runner->currentRoom == NULL || runner->currentRoom->name == NULL ||
        strcmp(runner->currentRoom->name, "room_floweyx") != 0 || runner->dataWin == NULL)
        return 1;
    VMContext *ctx = runner->vmContext;
    if (ctx == NULL || ctx->globalScopeInstance == NULL) return 1;
    resolveSpeedVars(runner);
    if (g_speedMasterObject < 0) return 1;
    RValue hp = Instance_getSelfVar(ctx->globalScopeInstance, g_speedHpVar);
    if (!numeric(hp) || RValue_toReal(hp) <= 0) return 1;
    for (ptrdiff_t i = 0; i < arrlen(runner->instances); ++i) {
        Instance *instance = runner->instances[i];
        if (instance == NULL || !instance->active || instance->destroyed || instance->objectIndex != g_speedMasterObject) continue;
        RValue intro = Instance_getSelfVar(instance, g_speedIntroVar);
        return numeric(intro) && RValue_toReal(intro) == -1 ? 2 : 1;
    }
    return 1;
}

/* battlePrefetch: an overworld encounter makes obj_battleblcon (the "!", the
 * heart and the flash) with global.battlegroup set, and about 47 updates later
 * the game is in room_battle. While it is there, outside a battle room, the
 * battle's items are the ones the table gives: those nearly every battle draws,
 * then that battlegroup's own. The table holds TPAG indices of the data file it
 * was recorded from; where three of them are not the same items, nothing is
 * loaded ahead. */
#include "game_undertale_prefetch.h"

static bool prefetchTableFits(const DataWin *dw) {
    static const DataWin *checked;
    static bool fits;
    if (checked != dw) {
        checked = dw;
        fits = true;
        for (size_t i = 0; i < sizeof(g_prefetchCheck) / sizeof(g_prefetchCheck[0]); ++i) {
            const int16_t *c = g_prefetchCheck[i];
            const TexturePageItem *item = (uint32_t) c[0] < dw->tpag.count ? &dw->tpag.items[c[0]] : NULL;
            fits &= item != NULL && item->texturePageId == c[1] && item->sourceWidth == c[2] && item->sourceHeight == c[3];
        }
        if (!fits) logInfo("Prefetch: this data file's textures are not the ones its table was made for; none loaded ahead\n");
    }
    return fits;
}

static int battlePrefetch(Runner *runner, int16_t *items, int max, int *battle) {
    if (runner->dataWin == NULL || battleUiActive(runner) || !prefetchTableFits(runner->dataWin)) return 0;
    VMContext *ctx = runner->vmContext;
    if (ctx == NULL || ctx->globalScopeInstance == NULL) return 0;
    resolveSpeedVars(runner);
    if (g_encounterObject < 0) return 0;
    bool encounter = false;
    for (ptrdiff_t i = 0; i < arrlen(runner->instances) && !encounter; ++i) {
        const Instance *instance = runner->instances[i];
        encounter = instance != NULL && instance->active && !instance->destroyed && instance->objectIndex == g_encounterObject;
    }
    if (!encounter) return 0;
    RValue group = Instance_getSelfVar(ctx->globalScopeInstance, g_speedGroupVar);
    if (!numeric(group)) return 0;
    int id = (int) RValue_toReal(group);
    *battle = id;
    int count = 0;
    for (size_t i = 0; i < sizeof(g_prefetchCommon) / sizeof(g_prefetchCommon[0]) && count < max; ++i)
        items[count++] = g_prefetchCommon[i];
    for (size_t g = 0; g < sizeof(g_prefetchGroups) / sizeof(g_prefetchGroups[0]); ++g) {
        if (g_prefetchGroups[g].group != id) continue;
        for (unsigned i = 0; i < g_prefetchGroups[g].count && count < max; ++i)
            items[count++] = g_prefetchItems[g_prefetchGroups[g].start + i];
    }
    return count;
}

/* The battle tour over other battles (--bench-battles-list): each visit as the
 * tour makes its own (g_tourScript, g_tourJumps), 600 frames from the Ruins
 * room, so that a list of the tour's battles replays the tour exactly. */
enum { TOUR_LIST_MAX = OF_BENCH_MAX_SECTIONS - 1, TOUR_VISIT = 600, TOUR_FIRST_VISIT = 1000 };
static char g_listScript[4096];
static OfJump g_listJumps[TOUR_LIST_MAX * 6];
static OfBenchSection g_listSections[TOUR_LIST_MAX + 1];
static char g_listNames[TOUR_LIST_MAX][32], g_listTags[TOUR_LIST_MAX][8], g_listSets[TOUR_LIST_MAX][80];

static bool tourForList(const char *list, const char **script, const OfJump **jumps, int *jumpCount,
                        const OfBenchSection **sections, int *sectionCount) {
    /* The tour's walk to the first room; then each visit's moves, menu presses and confirms. */
    static const char prefix[] =
        "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z,700:R*200";
    static const struct { int at; const char *key; } visit[] = {
        {30, "L*20"}, {60, "R*20"}, {160, "Z"}, {168, "U*20"}, {200, "Z"}, {208, "D*20"}, {240, "Z"}, {248, "L*20"},
        {280, "Z"}, {288, "R*20"}, {320, "Z"}, {328, "U*20"}, {360, "Z"}, {368, "D*20"}, {400, "Z"}, {408, "L*20"},
        {440, "Z"}, {448, "R*20"}, {480, "Z"}, {488, "U*20"}, {530, "Z"}, {545, "Z"}, {560, "Z"},
    };
    int ids[TOUR_LIST_MAX], count = 0;
    for (const char *p = list; *p != '\0';) {
        char *end;
        long id = strtol(p, &end, 10);
        if (end == p || id < 1 || id > 9999 || count == TOUR_LIST_MAX) return false;
        ids[count++] = (int) id;
        p = end;
        if (*p == ',') p++;
        else if (*p != '\0') return false;
    }
    if (count == 0) return false;
    size_t used = (size_t) snprintf(g_listScript, sizeof(g_listScript), "%s", prefix);
    int nj = 0;
    g_listSections[0] = (OfBenchSection) { "boot to the first room", TOUR_FIRST_VISIT, "boot", NULL, NULL, NULL };
    for (int v = 0; v < count; v++) {
        int base = TOUR_FIRST_VISIT + v * TOUR_VISIT;
        for (size_t k = 0; k < sizeof(visit) / sizeof(visit[0]); k++) {
            used += (size_t) snprintf(g_listScript + used, sizeof(g_listScript) - used, ",%d:%s", base + visit[k].at, visit[k].key);
            if (used >= sizeof(g_listScript)) return false;
        }
        snprintf(g_listSets[v], sizeof(g_listSets[v]), "battlegroup=%d,border=0,mercy=0,hp=99,maxhp=99,encounter=0", ids[v]);
        g_listJumps[nj++] = (OfJump) { base, 12, "encounter=0", NULL }; /* the Ruins room */
        g_listJumps[nj++] = (OfJump) { base + 100, -1, g_listSets[v], "obj_battleblcon" };
        g_listJumps[nj++] = (OfJump) { base + 250, -1, "hp=99", NULL };
        g_listJumps[nj++] = (OfJump) { base + 350, -1, "hp=99", NULL };
        g_listJumps[nj++] = (OfJump) { base + 450, -1, "hp=99", NULL };
        g_listJumps[nj++] = (OfJump) { base + 500, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }; /* won */
        snprintf(g_listNames[v], sizeof(g_listNames[v]), "battlegroup %d", ids[v]);
        snprintf(g_listTags[v], sizeof(g_listTags[v]), "b%d", ids[v]);
        g_listSections[v + 1] = (OfBenchSection) { g_listNames[v], base + TOUR_VISIT - 1, g_listTags[v], NULL, NULL, NULL };
    }
    *script = g_listScript;
    *jumps = g_listJumps;
    *jumpCount = nj;
    *sections = g_listSections;
    *sectionCount = count + 1;
    return true;
}

const OfGame ofGame = {
    .title = "Undertale",
    .dataFiles = "data.win, textures.bin and music.bin",
    .installPrompts = installPrompts,
    .natives = g_natives,
    .nativeCount = sizeof(g_natives) / sizeof(g_natives[0]),
#ifndef ENABLE_VM_TRACING
    .nativeScripts = g_nativeScripts,
    .nativeScriptCount = sizeof(g_nativeScripts) / sizeof(g_nativeScripts[0]),
#endif
    .nativesForgetVm = forgetNativeVm,
    .installCollisionFilter = installVineCollisionFilter,
    .battleUiInstall = battleUiInstall,
    .battleUiActive = battleUiActive,
    .speedFullResBattles = true,
    .speedHalfBattle = speedHalfBattle,
    .speedSkipRun = speedSkipRun,
    .battlePrefetch = battlePrefetch,
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
    .tourForList = tourForList,
    .bulletTitle = "Undertale bullet-hell tour",
    .bulletScript = g_bulletScript,
    .bulletJumps = g_bulletJumps,
    .bulletJumpCount = (int) (sizeof(g_bulletJumps) / sizeof(g_bulletJumps[0])),
    .bulletSections = g_bulletSections,
    .bulletSectionCount = (int) (sizeof(g_bulletSections) / sizeof(g_bulletSections[0])),
    .saberTitle = "Undertale Chaos Saber",
    .saberScript = g_saberScript,
    .saberJumps = g_saberJumps,
    .saberJumpCount = (int) (sizeof(g_saberJumps) / sizeof(g_saberJumps[0])),
    .saberSections = g_saberSections,
    .saberSectionCount = (int) (sizeof(g_saberSections) / sizeof(g_saberSections[0])),
    .floweyTitle = "Undertale Photoshop Flowey",
    .floweyScript = g_floweyScript,
    .floweyJumps = g_floweyJumps,
    .floweyJumpCount = (int) (sizeof(g_floweyJumps) / sizeof(g_floweyJumps[0])),
    .floweySections = g_floweySections,
    .floweySectionCount = (int) (sizeof(g_floweySections) / sizeof(g_floweySections[0])),
    .floweyOpening = g_floweyOpening,
    .floweyOpeningCount = (int) (sizeof(g_floweyOpening) / sizeof(g_floweyOpening[0])),
    .snowTitle = "Undertale room 57 snow patch",
    .snowScript = g_snowScript,
    .snowJumps = g_snowJumps,
    .snowJumpCount = (int) (sizeof(g_snowJumps) / sizeof(g_snowJumps[0])),
    .snowSections = g_snowSections,
    .snowSectionCount = (int) (sizeof(g_snowSections) / sizeof(g_snowSections[0])),
    .ioDivisor = 1u,
    .ioAsyncBase = 110u,
    .ioAsyncStep = 2u,
    .savestateReason = savestateReason,
};
