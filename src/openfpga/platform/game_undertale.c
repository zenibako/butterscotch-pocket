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

static void forgetNativeVm(void) {
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
