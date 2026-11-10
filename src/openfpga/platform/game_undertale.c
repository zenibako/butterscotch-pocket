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
#include "utils.h"
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

enum {
    PIPE_FROZEN, PIPE_PHASE, PIPE_FLASH, PIPE_TINT, PIPE_I, PIPE_MIRROR_OFFSET,
    PIPE_X, PIPE_Y, PIPE_SWELL, PIPE_ANGLE, PIPE_X2, PIPE_Y2, PIPE_SWELL2, PIPE_ANGLE2,
    PIPE_ANGLE_OFFSET, PIPE_SOUL_RESCUE, PIPE_DEBUG, PIPE_FIELD_COUNT
};
static int32_t pipeVars[PIPE_FIELD_COUNT];
static bool pipeVarsResolved;

enum {
    BACKDROP_PHASE, BACKDROP_FROZEN, BACKDROP_I, BACKDROP_LAYER_PHASE, BACKDROP_BLEND,
    BACKDROP_GROWTH, BACKDROP_GROWTH2, BACKDROP_GROWTH3,
    BACKDROP_MSIN, BACKDROP_MSIN2, BACKDROP_MSIN3, BACKDROP_YSIN, BACKDROP_YSIN2, BACKDROP_YSIN3,
    BACKDROP_SSX, BACKDROP_SSX2, BACKDROP_SSX3, BACKDROP_FIELD_COUNT
};
static int32_t backdropVars[BACKDROP_FIELD_COUNT];
static bool backdropVarsResolved;

static void forgetNativeVm(void) {
    particleVarsResolved = false;
    pipeVarsResolved = false;
    backdropVarsResolved = false;
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

typedef struct {
    int last, xOrigin, xRadius, yOrigin, yRadius;
    GMLReal yDivisor;
} FloweyPipeWave;

static bool readNativeNumber(Instance *instance, int32_t id, GMLReal *out) {
    RValue value = Instance_getSelfVar(instance, id);
    if (value.type != RVALUE_REAL && value.type != RVALUE_INT32) return false;
    *out = RValue_toReal(value);
    return isfinite(*out);
}

static bool skipDormantVineCollision(Runner *runner, Instance *self) {
    GMLReal active, epsilon = runner->mathEpsilon;
    if (!ofPlatformSpeedMode() || !(epsilon >= 0 && epsilon < (GMLReal)0.5) ||
        !readNativeNumber(self, runner->skipCollisionVar, &active)) return false;

    // The stock handler only calls event_inherited when active == 1 and
    // image_alpha > 0.3. Match the VM's epsilon comparisons.
    if (GMLReal_fabs(active - (GMLReal)1) > epsilon) return true;
    GMLReal opacity = (GMLReal)self->imageAlpha;
    return isfinite(opacity) && opacity - (GMLReal)0.3 <= epsilon;
}

static void installVineCollisionFilter(Runner *runner) {
    static const char name[] = "gml_Object_obj_gigavine_Collision_1587";
    DataWin *dw = runner->dataWin;
    if (dw->gen8.wadVersion != 16 || dw->gen8.gameID != 864738521u) return;
    uint32_t length = 0;
    uint64_t hash = VM_codeHash(runner->vmContext, name, &length);
    if (length != 68 || hash != 0xbb9e9d11ca6b3d04ull) return;
    for (uint32_t i = 0; i < dw->code.count; ++i) {
        const char *entry = dw->code.entries[i].name;
        if (entry == NULL || strcmp(entry, name) != 0) continue;
        runner->skipCollisionCode = (int32_t)i;
        runner->skipCollisionVar = VM_getOrAllocateVarID(runner->vmContext, "active");
        runner->skipCollisionEvent = skipDormantVineCollision;
        return;
    }
}

static bool pipeNumber(Instance *instance, int field, GMLReal *out) {
    return readNativeNumber(instance, pipeVars[field], out);
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool floweyPipeDraw(VMContext *ctx, int pipe) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    static const FloweyPipeWave waves[] = {
        { 9, 45, 60, 45, 50, (GMLReal)3 },
        { 12, 49, 85, 3, 40, (GMLReal)2.2 },
        { 15, 34, 45, 34, 40, (GMLReal)3 },
    };
    if (!ofPlatformSpeedMode()) return false;
    Runner *runner = ctx->runner;
    Instance *self = ctx->currentCallFrame->self;
    if (runner == NULL || self == NULL || runner->currentRoom == NULL || ctx->globalScopeInstance == NULL ||
        runner->currentRoom->name == NULL || strcmp(runner->currentRoom->name, "room_floweyx") != 0 ||
        self->spriteIndex < 0 || (uint32_t) self->spriteIndex >= ctx->dataWin->sprt.count) return false;
    const char *sprite = ctx->dataWin->sprt.sprites[self->spriteIndex].name;
    if (sprite == NULL || strcmp(sprite, "spr_pipepart") != 0) return false;

    GMLReal epsilon = runner->mathEpsilon;
    if (!(epsilon >= 0 && epsilon < (GMLReal)0.5) || !isfinite(self->x) || !isfinite(self->y)) return false;
    if (!pipeVarsResolved) {
        static const char *const names[PIPE_FIELD_COUNT] = {
            "frozen", "siner", "flash", "blend2", "i", "op", "offx", "offy", "offs", "offv",
            "offx2", "offy2", "offs2", "offv2", "magicfactor3", "soul_rescue", "debug"
        };
        for (int i = 0; i < PIPE_FIELD_COUNT; i++) pipeVars[i] = VM_getOrAllocateVarID(ctx, names[i]);
        pipeVarsResolved = true;
    }

    GMLReal frozen, flash, mirrorOffset;
    RValue phaseValue = Instance_getSelfVar(self, pipeVars[PIPE_PHASE]);
    if (phaseValue.type != RVALUE_REAL || !isfinite(phaseValue.real) ||
        !pipeNumber(self, PIPE_FROZEN, &frozen) || !pipeNumber(self, PIPE_FLASH, &flash) ||
        !pipeNumber(self, PIPE_MIRROR_OFFSET, &mirrorOffset)) return false;
    /* Flash and debug paths have renderer or keyboard effects. Run their
     * original event, including the pipe art, when either is active. */
    if (GMLReal_fabs(flash - (GMLReal)1) <= epsilon || GMLReal_fabs(flash - (GMLReal)2) <= epsilon)
        return false;
    GMLReal rescue = 0, angleOffset = 0, debug;
    if (pipe == 0 && !pipeNumber(ctx->globalScopeInstance, PIPE_SOUL_RESCUE, &rescue)) return false;
    if (pipe != 0 && (!pipeNumber(ctx->globalScopeInstance, PIPE_DEBUG, &debug) ||
                      GMLReal_fabs(debug - (GMLReal)1) <= epsilon)) return false;
    if (pipe == 1 && !pipeNumber(self, PIPE_ANGLE_OFFSET, &angleOffset)) return false;

    /* Every iteration replaces these fields; only the last value on each
     * side can survive the event. Keep that state and omit the pipe draws. */
    const FloweyPipeWave *wave = &waves[pipe];
    int last = wave->last;
    bool advances = GMLReal_fabs(frozen) <= epsilon;
    GMLReal phase = advances ? phaseValue.real + (GMLReal)1 : phaseValue.real;
    /* Use the target's math library for the fixed angles too, as the VM does. */
    volatile GMLReal xAngle = (GMLReal)(last - wave->xOrigin) / (GMLReal)3;
    volatile GMLReal yAngle = (GMLReal)(last - wave->yOrigin) / wave->yDivisor;
    GMLReal dx = GMLReal_sin(xAngle) * (GMLReal)wave->xRadius;
    GMLReal y = (GMLReal)self->y + GMLReal_cos(yAngle) * (GMLReal)wave->yRadius;
    GMLReal bob = GMLReal_cos(((GMLReal)last + phase) / (GMLReal)6);
    GMLReal swell = (GMLReal)2 * GMLReal_sin((phase + (GMLReal)(4 * last)) / (GMLReal)6) - (GMLReal)1;
    bool leftRescued = pipe == 0 && rescue - (GMLReal)1 >= -epsilon;
    bool rightRescued = pipe == 0 && rescue - (GMLReal)2 >= -epsilon;
    GMLReal x1 = (GMLReal)self->x + dx, x2 = (GMLReal)self->x - dx;
    if (pipe == 0) {
        x1 -= (GMLReal)(2 * last);
        x2 += (GMLReal)(2 * last);
    }
    x2 += mirrorOffset;
    GMLReal y1 = y + bob * (GMLReal)(leftRescued ? 4 : 8);
    GMLReal y2 = y + bob * (GMLReal)(rightRescued ? 4 : 8);
    if (!isfinite(x1) || !isfinite(x2) || !isfinite(y1) || !isfinite(y2) || !isfinite(swell)) return false;

    if (advances) setNativeReal(self, pipeVars[PIPE_PHASE], phase);
    if (GMLReal_fabs(flash) <= epsilon) {
        RValue white = RValue_makeReal((GMLReal)16777215);
        VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_BLEND, "image_blend", white, -1);
        Instance_setSelfVar(self, pipeVars[PIPE_TINT], white);
    }
    setNativeReal(self, pipeVars[PIPE_I], (GMLReal)(last + 1));
    setNativeReal(self, pipeVars[PIPE_X], x1);
    setNativeReal(self, pipeVars[PIPE_Y], y1);
    if (leftRescued || swell <= epsilon) Instance_setSelfVar(self, pipeVars[PIPE_SWELL], RValue_makeReal(0));
    else setNativeReal(self, pipeVars[PIPE_SWELL], swell / (GMLReal)3);
    if (pipe == 1) setNativeReal(self, pipeVars[PIPE_ANGLE], (GMLReal)(-22 * last - 310));
    setNativeReal(self, pipeVars[PIPE_X2], x2);
    setNativeReal(self, pipeVars[PIPE_Y2], y2);
    if (rightRescued || swell <= epsilon) Instance_setSelfVar(self, pipeVars[PIPE_SWELL2], RValue_makeReal(0));
    else setNativeReal(self, pipeVars[PIPE_SWELL2], swell / (GMLReal)3);
    if (pipe == 1) setNativeReal(self, pipeVars[PIPE_ANGLE2], (GMLReal)(20 * last) + angleOffset);
    return true;
}

static bool floweyPipeDraw1(VMContext *ctx) { return floweyPipeDraw(ctx, 0); }
static bool floweyPipeDraw2(VMContext *ctx) { return floweyPipeDraw(ctx, 1); }
static bool floweyPipeDraw3(VMContext *ctx) { return floweyPipeDraw(ctx, 2); }

static Instance *floweyBackdropInstance(VMContext *ctx, bool skipOnly) {
    Runner *runner = ctx->runner;
    if (!ofPlatformSpeedMode() || runner == NULL || runner->currentRoom == NULL ||
        runner->currentRoom->name == NULL || strcmp(runner->currentRoom->name, "room_floweyx") != 0 ||
        (skipOnly && !SWRenderer_isSkippingScreen(runner->renderer))) return NULL;
    if (!backdropVarsResolved) {
        static const char *const names[BACKDROP_FIELD_COUNT] = {
            "siner", "frozen", "i", "siner2", "blend3", "growth", "growth2", "growth3",
            "msin", "msin2", "msin3", "ysin", "ysin2", "ysin3", "ssx", "ssx2", "ssx3"
        };
        for (int i = 0; i < BACKDROP_FIELD_COUNT; i++) backdropVars[i] = VM_getOrAllocateVarID(ctx, names[i]);
        backdropVarsResolved = true;
    }
    return ctx->currentCallFrame->self;
}

/* The layered backdrop's Draw event updates room background state. Each layer
 * uses the same sine for opacity and drift, so keep both writes with one sine. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool floweyBackgroundMotion(VMContext *ctx) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    Instance *self = floweyBackdropInstance(ctx, false);
    if (self == NULL) return false;
    RValue phase = Instance_getSelfVar(self, backdropVars[BACKDROP_PHASE]);
    if (phase.type != RVALUE_REAL || !isfinite(phase.real) ||
        !(ctx->runner->mathEpsilon >= 0 && ctx->runner->mathEpsilon < (GMLReal)0.5)) return false;

    for (int i = 0; i < 8; i++) {
        RuntimeBackground *background = &ctx->runner->backgrounds[i];
        GMLReal wave = GMLReal_sin((phase.real + (GMLReal)i) / (GMLReal)8);
        background->alpha = (float)((GMLReal)0.5 + wave * (GMLReal)0.4);
        background->x = (float)((GMLReal)background->x + wave);
    }
    setNativeReal(self, backdropVars[BACKDROP_I], (GMLReal)8);
    setNativeReal(self, backdropVars[BACKDROP_LAYER_PHASE], phase.real + (GMLReal)7);
    setNativeReal(self, backdropVars[BACKDROP_PHASE], phase.real + (GMLReal)1);
    return true;
}

static bool floweyBackdropPhase(VMContext *ctx, Instance *self, GMLReal *out) {
    GMLReal frozen;
    if (self == NULL || !readNativeNumber(self, backdropVars[BACKDROP_FROZEN], &frozen)) return false;
    GMLReal epsilon = ctx->runner->mathEpsilon;
    RValue phase = Instance_getSelfVar(self, backdropVars[BACKDROP_PHASE]);
    if (phase.type != RVALUE_REAL || !isfinite(phase.real) || !(epsilon >= 0 && epsilon < (GMLReal)0.5)) return false;
    *out = phase.real;
    if (GMLReal_fabs(frozen) <= epsilon) {
        *out += (GMLReal)1;
        setNativeReal(self, backdropVars[BACKDROP_PHASE], *out);
    }
    return true;
}

/* The VM calls sin and cos separately. A fused sincos can round differently. */
static inline GMLReal backdropSin(GMLReal angle) {
    volatile GMLReal separateAngle = angle;
    return GMLReal_sin(separateAngle);
}

/* These sprites use the original events on drawn updates and on surfaces.
 * On a skipped screen update, retain every animation field they write. */
static bool floweyBackdropPipes(VMContext *ctx) {
    Instance *self = floweyBackdropInstance(ctx, true);
    GMLReal phase;
    if (!floweyBackdropPhase(ctx, self, &phase)) return false;
    GMLReal wave = backdropSin(phase / (GMLReal)9);
    setNativeReal(self, backdropVars[BACKDROP_MSIN], wave * (GMLReal)2);
    setNativeReal(self, backdropVars[BACKDROP_YSIN], GMLReal_cos(phase / (GMLReal)8));
    setNativeReal(self, backdropVars[BACKDROP_GROWTH], GMLReal_sin(phase / (GMLReal)7) * (GMLReal)0.012);
    setNativeReal(self, backdropVars[BACKDROP_MSIN2], backdropSin(phase / (GMLReal)10) * (GMLReal)1.5);
    setNativeReal(self, backdropVars[BACKDROP_YSIN2], GMLReal_cos(phase / (GMLReal)9) * (GMLReal)0.8);
    setNativeReal(self, backdropVars[BACKDROP_GROWTH2], backdropSin(phase / (GMLReal)8) * (GMLReal)0.01);
    setNativeReal(self, backdropVars[BACKDROP_MSIN3], GMLReal_sin(phase / (GMLReal)11));
    setNativeReal(self, backdropVars[BACKDROP_YSIN3], GMLReal_cos(phase / (GMLReal)10) * (GMLReal)0.5);
    setNativeReal(self, backdropVars[BACKDROP_GROWTH3], wave * (GMLReal)0.005);
    uint32_t tint = RValue_toColour(RValue_makeReal((GMLReal)self->imageBlend));
    setNativeReal(self, backdropVars[BACKDROP_BLEND], (GMLReal)Color_lerp((int32_t)tint, 0, 0.4f));
    return true;
}

static void setBackdropVineGrowth(Instance *self, int field, GMLReal number) {
    RValue value = RValue_makeReal(number);
    value.gmlStackType = GML_TYPE_INT32; /* ADD/SUB.v.i result stored by pop.v.v */
    Instance_setSelfVar(self, backdropVars[field], value);
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool floweyBackdropVines(VMContext *ctx) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    Instance *self = floweyBackdropInstance(ctx, true);
    GMLReal phase;
    if (!floweyBackdropPhase(ctx, self, &phase)) return false;
    GMLReal cosine = GMLReal_cos(phase / (GMLReal)6);
    GMLReal scale = (GMLReal)self->imageXscale;
    setBackdropVineGrowth(self, BACKDROP_GROWTH, (GMLReal)1 + backdropSin(phase / (GMLReal)6) * (GMLReal)0.05);
    setBackdropVineGrowth(self, BACKDROP_GROWTH2, (GMLReal)1 + cosine * (GMLReal)0.05);
    setBackdropVineGrowth(self, BACKDROP_GROWTH3, (GMLReal)1 - GMLReal_sin(phase / (GMLReal)7) * (GMLReal)0.05);
    setNativeReal(self, backdropVars[BACKDROP_SSX], GMLReal_sin(phase / (GMLReal)4) * (GMLReal)2 * scale);
    setNativeReal(self, backdropVars[BACKDROP_SSX2], GMLReal_sin(phase / (GMLReal)5) * scale);
    setNativeReal(self, backdropVars[BACKDROP_SSX3], cosine * (GMLReal)0.5 * scale);
    return true;
}

static const OfNativeCode g_natives[] = {
    { "gml_Object_obj_orangeparticle_Step_0", 392, 0x85cd9b4f052c0694ull, orangeParticleStep },
    { "gml_Object_obj_floweypipetest_Draw_0", 2292, 0xd060939f37f20db1ull, floweyPipeDraw1 },
    { "gml_Object_obj_floweypipetest_2_Draw_0", 2496, 0xc784a157fab0292eull, floweyPipeDraw2 },
    { "gml_Object_obj_floweypipetest_3_Draw_0", 2148, 0x543e2f97612cf23cull, floweyPipeDraw3 },
    { "gml_Object_obj_flowey_bgdraw_Draw_0", 272, 0x746a6e5ff0305b36ull, floweyBackgroundMotion },
    { "gml_Object_obj_bgpipe_Draw_0", 964, 0x12bf9b0542ba714cull, floweyBackdropPipes },
    { "gml_Object_obj_vines_flowey_Draw_0", 704, 0xec70756e7a99f41aull, floweyBackdropVines },
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
    { "intro, menu, naming", 650, "intro", NULL, NULL, NULL },
    { "first room 320x240", 1220, "room", NULL, NULL, NULL },
    { "Flowey talk 320x240", 2060, "talk", NULL, NULL, NULL },
    { "Flowey battle (640x480)", 2600, "battl", NULL, NULL, NULL },
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
    { "boot to the first room", 1000, "boot", NULL, NULL, NULL },
    { "Froggit (640x480)", 1599, "frog", NULL, NULL, NULL },
    { "Toriel (640x480)", 2199, "tori", NULL, NULL, NULL },
    { "Papyrus (640x480)", 2799, "papy", NULL, NULL, NULL },
    { "Undyne (640x480)", 3399, "undy", NULL, NULL, NULL },
    { "Mettaton EX (640x480)", 3999, "mtex", NULL, NULL, NULL },
    { "Asgore (640x480)", 4599, "asgr", NULL, NULL, NULL },
};

/* The bullet-hell tour retains the battle tour's 600-frame Asgore
 * encounter/win control, then enters Asriel through the same overworld
 * encounter object. Only HP and menu selection are replenished/controlled;
 * Asriel's ordinary turns, objects and timers remain in control. Named setup
 * sections cover the encounter and later turns, keeping the two 150-frame
 * attack sections entirely inside their natural projectile sequences. */
static const char g_bulletScript[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z,"
    "700:R*200,1030:L*20,1060:R*20,1160:Z,1168:U*20,1200:Z,1208:D*20,1240:Z,1248:L*20,1280:Z,1288:R*20,"
    "1320:Z,1328:U*20,1360:Z,1368:D*20,1400:Z,1408:L*20,1440:Z,1448:R*20,1480:Z,1488:U*20,1530:Z,1545:Z,1560:Z,"
    "1760:Z,1800:Z,1840:Z,1880:Z,1920:Z,1960:Z,2000:Z,2040:Z,2080:Z,2120:Z,2160:Z,2200:Z,2240:Z,2280:Z,"
    "2320:Z,2360:Z,2400:Z,2440:Z,2480:Z,2520:Z,2560:Z,2600:Z,2640:Z,2680:Z,2720:Z,2760:Z,2800:Z,2840:Z,"
    "2880:Z,2920:Z,2960:Z,3000:Z,3040:Z,3080:Z,3120:Z,3160:Z,3200:Z,3240:Z,3280:Z,3320:Z,3360:Z,3400:Z,"
    "3440:Z,3480:Z,3520:Z,3560:Z,3600:Z,3640:Z,3680:Z,3720:Z,3760:Z,3800:Z,3840:Z,3880:Z,3920:Z,3960:Z,"
    "4000:Z,4040:Z,4080:Z,4120:Z,4160:Z,4200:Z,4240:Z,4280:Z,4320:Z,4360:Z,4400:Z,4440:Z,4480:Z,4520:Z,4560:Z,"
    "4600:Z,4640:Z,4680:Z,4720:Z,4760:Z,4800:Z,4840:Z,4880:Z,4920:Z,4960:Z,5000:Z,5040:Z,5080:Z,5120:Z,"
    "5160:Z,5200:Z,5240:Z,5280:Z,5320:Z,5360:Z,5400:Z,5440:Z,5480:Z,5520:Z,5560:Z,5600:Z";

#define ASRIEL_ENCOUNTER_GLOBALS \
    "battlegroup=256,border=0,mercy=0,hp=99,maxhp=99,encounter=0," \
    "flag[500]=1,flag[501]=0,flag[505]=0,flag[506]=0,flag[507]=0,flag[508]=0"
#define BULLET_HP_AT(f) { (f), -1, "hp=99,maxhp=99,bmenucoord[0]=1", NULL },
#define BULLET_HP_1000(f) \
    BULLET_HP_AT((f)) BULLET_HP_AT((f) + 100) BULLET_HP_AT((f) + 200) BULLET_HP_AT((f) + 300) \
    BULLET_HP_AT((f) + 400) BULLET_HP_AT((f) + 500) BULLET_HP_AT((f) + 600) BULLET_HP_AT((f) + 700) \
    BULLET_HP_AT((f) + 800) BULLET_HP_AT((f) + 900)

static const OfJump g_bulletJumps[] = {
    { 1000, 12, "encounter=0", NULL },
    { 1100, -1, "battlegroup=101,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" },
    { 1250, -1, "hp=99,maxhp=99", NULL },
    { 1350, -1, "hp=99,maxhp=99", NULL },
    { 1450, -1, "hp=99,maxhp=99", NULL },
    { 1500, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99,maxhp=99", NULL },
    { 1600, 12, "encounter=0", NULL },
    { 1700, -1, ASRIEL_ENCOUNTER_GLOBALS, "obj_battleblcon" },
    BULLET_HP_1000(1800)
    BULLET_HP_1000(2800)
    BULLET_HP_1000(3800)
    BULLET_HP_AT(4800)
    BULLET_HP_AT(4900)
    BULLET_HP_AT(5000)
    BULLET_HP_AT(5100)
    BULLET_HP_AT(5200)
    BULLET_HP_AT(5300)
    BULLET_HP_AT(5400)
    BULLET_HP_AT(5500)
    BULLET_HP_AT(5600)
};
#undef BULLET_HP_1000
#undef BULLET_HP_AT

static const OfBenchSection g_bulletSections[] = {
    { "boot to the first room", 1000, "boot", NULL, NULL, NULL },
    { "Asgore control (640x480)", 1600, "asgr", "room_battle", "obj_asgoreb", NULL },
    { "Asriel encounter setup", 1920, "aset", "room_battle", NULL, NULL },
    { "Asriel Chaos Saber", 2070, "saber", "room_battle", "obj_ultimagen", NULL },
    { "Lost Soul transition", 5480, "trans", "room_battle", NULL, NULL },
    { "Lost Soul green spears", 5630, "spear", "room_battle", "obj_greenspeargen", NULL },
    /* Not an attack: keeps the final report overlay out of the 5629 probe. */
    { "benchmark capture tail", 5631, "tail", "room_battle", NULL, NULL },
};

/* Undertale v1.08: enter room_battle after the initial room has initialized.
 * Confirm through the opening menus, then let the natural turn play out. */
static const char g_saberScript[] =
    "4:Z,8:Z,12:Z,16:Z,20:Z,24:Z,28:Z,32:Z,36:Z,40:Z,44:Z,"
    "48:Z,52:Z,56:Z,60:Z,64:Z,68:Z,72:Z,76:Z,80:Z,84:Z";

static const OfJump g_saberJumps[] = {
    { 2, 306, ASRIEL_ENCOUNTER_GLOBALS, NULL },
};
#undef ASRIEL_ENCOUNTER_GLOBALS

static GMLReal benchGlobal(const Runner *runner, const char *name) {
    VMContext *vm = runner->vmContext;
    int32_t id = VM_getOrAllocateVarID(vm, name);
    return RValue_toReal(Instance_getSelfVar(vm->globalScopeInstance, id));
}

static bool saberTurnEnds(const Runner *runner) {
    /* The final attack frame enters Draw with one tick left. */
    return benchGlobal(runner, "turntimer") == 0 && benchGlobal(runner, "hp") > 0;
}

static const OfBenchSection g_saberSections[] = {
    { "Asriel encounter setup", 82, "aset", "room_battle", "obj_asrielfinal", NULL },
    { "Asriel Chaos Saber", 232, "saber", "room_battle", "obj_ultimagen", saberTurnEnds },
};

/* Undertale v1.08: room_floweyx's controller loads phase fplot - 1.
 * Let each checkpoint finish its setup, then measure the native attacks
 * for equal windows that end before the stationary player dies. */
static const char g_floweyScript[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z";

static const OfJump g_floweyJumps[] = {
    { 900, 294, "fplot=3", NULL },
    { 1430, 294, "fplot=7", NULL },
};

static bool floweyPhaseActive(const Runner *runner, GMLReal phase) {
    return benchGlobal(runner, "battlephase") == phase && benchGlobal(runner, "my_hp") > 0;
}

static bool floweyVinesActive(const Runner *runner) {
    return floweyPhaseActive(runner, 2);
}

static bool floweyFinaleActive(const Runner *runner) {
    return floweyPhaseActive(runner, 6);
}

static const OfBenchSection g_floweySections[] = {
    { "Boot and vine setup", 1260, "vset", "room_floweyx", "obj_flowey_master", NULL },
    { "Flowey vine pressure", 1420, "vines", "room_floweyx", "obj_gigavine", floweyVinesActive },
    { "Finale checkpoint setup", 1830, "fset", "room_floweyx", "obj_flowey_master", NULL },
    { "Flowey finale eye shots", 1990, "eyes", "room_floweyx", "obj_eyeshot_a", floweyFinaleActive },
};

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
    .ioDivisor = 1u,
    .ioAsyncBase = 110u,
    .ioAsyncStep = 2u,
    .savestateReason = savestateReason,
};
