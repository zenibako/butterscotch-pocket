/*
 * Undertale: what the port does differently for it. See of_game.h.
 */

#include "of_game.h"
#include "of_buttons.h"
#include "sw_renderer.h"

#include <string.h>

#include "binary_utils.h"
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
static bool particleVarsResolved;
static int32_t floweyVineActive, floweyVineCon, floweyEyeShotSize;
static bool floweyStepVarsResolved;

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
    BACKDROP_SSX, BACKDROP_SSX2, BACKDROP_SSX3, BACKDROP_SSY, BACKDROP_DESPERATE, BACKDROP_ARM_STATE, BACKDROP_ARM_MIRROR, BACKDROP_FIELD_COUNT
};
static int32_t backdropVars[BACKDROP_FIELD_COUNT];
static bool backdropVarsResolved;

#ifndef ENABLE_VM_TRACING
enum { FLOWEY_FLIPEYE_DRAW_LENGTH = 7832, FLOWEY_MOUTH_DRAW_LENGTH = 4992,
       FLOWEY_PREVIEW_DRAW_LENGTH = 672, FLOWEY_LEFT_EYE_DRAW_LENGTH = 6356 };
static uint32_t flipeyeSkippedWords[FLOWEY_FLIPEYE_DRAW_LENGTH / 4];
static uint32_t mouthSkippedWords[FLOWEY_MOUTH_DRAW_LENGTH / 4];
static bool flipeyeSkippedReady;
static bool mouthSkippedReady;
static uint32_t previewSkippedWords[FLOWEY_PREVIEW_DRAW_LENGTH / 4];
static bool previewSkippedReady;
static uint32_t leftEyeSkippedWords[FLOWEY_LEFT_EYE_DRAW_LENGTH / 4];
static bool leftEyeSkippedReady;
#endif

static void forgetNativeVm(void) {
    particleVarsResolved = false;
    floweyStepVarsResolved = false;
    pipeVarsResolved = false;
    backdropVarsResolved = false;
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

static bool floweyStepContext(VMContext *ctx) {
    if (!ofPlatformSpeedMode() || ctx->runner == NULL ||
        ctx->currentCallFrame->eventType != EVENT_STEP || ctx->currentCallFrame->eventSubtype != STEP_NORMAL ||
        ctx->currentCallFrame->self == NULL) return false;
    GMLReal epsilon = ctx->runner->mathEpsilon;
    if (!(epsilon >= 0 && epsilon < (GMLReal)0.5)) return false;
    if (!floweyStepVarsResolved) {
        floweyVineActive = VM_getOrAllocateVarID(ctx, "active");
        floweyVineCon = VM_getOrAllocateVarID(ctx, "con");
        floweyEyeShotSize = VM_getOrAllocateVarID(ctx, "size");
        floweyStepVarsResolved = true;
    }
    return true;
}

static bool floweyVineStep(VMContext *ctx) {
    if (!floweyStepContext(ctx)) return false;
    Instance *self = ctx->currentCallFrame->self;
    GMLReal epsilon = ctx->runner->mathEpsilon, active, con;
    if (!readNativeNumber(self, floweyVineActive, &active)) return false;
    if (GMLReal_fabs(active - (GMLReal)1) > epsilon) return true;
    if (!readNativeNumber(self, floweyVineCon, &con) || !isfinite(self->imageXscale)) return false;
    bool held = GMLReal_fabs(con - (GMLReal)2) <= epsilon;
    if (!held && GMLReal_fabs(con) > epsilon) return false;
    if ((GMLReal)self->imageXscale - (GMLReal)1 >= -epsilon) return held;

    // The stock Step event also tests the stored float scale before making
    // the growing strike visible. Leave the alarm and fade branches in GML.
    GMLReal scale = (GMLReal)self->imageXscale + (GMLReal)0.08;
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_XSCALE, "image_xscale", RValue_makeReal(scale), -1);
    if ((GMLReal)self->imageXscale - (GMLReal)0.2 > epsilon) self->visible = true;
    return true;
}

static bool floweyEyeShotStep(VMContext *ctx) {
    if (!floweyStepContext(ctx)) return false;
    Instance *self = ctx->currentCallFrame->self;
    GMLReal size;
    if (!isfinite(self->imageAngle) || !readNativeNumber(self, floweyEyeShotSize, &size)) return false;
    GMLReal epsilon = ctx->runner->mathEpsilon;
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_ANGLE, "image_angle",
                          RValue_makeReal(self->imageAngle + (GMLReal)10), -1);
    if (size - (GMLReal)1 < -epsilon) {
        size += (GMLReal)0.1;
        setNativeReal(self, floweyEyeShotSize, size);
    }
    RValue scale = RValue_makeReal(size);
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_XSCALE, "image_xscale", scale, -1);
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_YSCALE, "image_yscale", scale, -1);
    return true;
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
    if (!ofPlatformSpeedMode() || !swrSmoothMinify) return false;
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
    if (pipe == 1 && self->x == 230 && self->y == 132 && mirrorOffset == (GMLReal)180)
        SWRenderer_drawFloweyStalkEyePipes(runner->renderer);
    return true;
}

static bool floweyPipeDraw1(VMContext *ctx) { return floweyPipeDraw(ctx, 0); }
static bool floweyPipeDraw2(VMContext *ctx) { return floweyPipeDraw(ctx, 1); }
static bool floweyPipeDraw3(VMContext *ctx) { return floweyPipeDraw(ctx, 2); }

static Instance *floweyBackdropInstance(VMContext *ctx) {
    Runner *runner = ctx->runner;
    if (!ofPlatformSpeedMode() || runner == NULL || runner->currentRoom == NULL ||
        runner->currentRoom->name == NULL || strcmp(runner->currentRoom->name, "room_floweyx") != 0) return NULL;
    if (!backdropVarsResolved) {
        static const char *const names[BACKDROP_FIELD_COUNT] = {
            "siner", "frozen", "i", "siner2", "blend3", "growth", "growth2", "growth3",
            "msin", "msin2", "msin3", "ysin", "ysin2", "ysin3", "ssx", "ssx2", "ssx3", "ssy", "desperate", "acon", "xr"
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
    Instance *self = floweyBackdropInstance(ctx);
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

static bool floweySceneryPose(const Instance* self) {
    return self != NULL && isfinite(self->x) && isfinite(self->y) &&
           isfinite(self->imageXscale) && isfinite(self->imageIndex) &&
           self->imageIndex > INT32_MIN && self->imageIndex < (GMLReal) INT32_MAX;
}

static void floweyScenerySprite(Renderer* renderer, const Instance* self, int sprite,
                               GMLReal x, GMLReal y, GMLReal sx, GMLReal sy, uint32_t tint) {
    if (renderer == NULL || SWRenderer_isSkippingScreen(renderer)) return;
    int frame = RValue_toInt32(RValue_makeReal((GMLReal)self->imageIndex));
    if (frame < 0) frame = (int)self->imageIndex;
    Renderer_drawSpriteExt(renderer, sprite, frame, x, y, sx, sy, 0, tint, 1);
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool floweyBackdropPipes(VMContext *ctx) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    Instance *self = floweyBackdropInstance(ctx);
    GMLReal phase;
    if (!floweySceneryPose(self) || !floweyBackdropPhase(ctx, self, &phase)) return false;
    GMLReal wave = backdropSin(phase / (GMLReal)9);
    GMLReal xs[] = { wave * (GMLReal)2, backdropSin(phase / (GMLReal)10) * (GMLReal)1.5,
                     GMLReal_sin(phase / (GMLReal)11) };
    GMLReal ys[] = { GMLReal_cos(phase / (GMLReal)8), GMLReal_cos(phase / (GMLReal)9) * (GMLReal)0.8,
                     GMLReal_cos(phase / (GMLReal)10) * (GMLReal)0.5 };
    GMLReal growth[] = { GMLReal_sin(phase / (GMLReal)7) * (GMLReal)0.012,
                         backdropSin(phase / (GMLReal)8) * (GMLReal)0.01, wave * (GMLReal)0.005 };
    for (int i = 0; i < 3; ++i) {
        setNativeReal(self, backdropVars[BACKDROP_MSIN + i], xs[i]);
        setNativeReal(self, backdropVars[BACKDROP_YSIN + i], ys[i]);
        setNativeReal(self, backdropVars[BACKDROP_GROWTH + i], growth[i]);
    }
    uint32_t tint = RValue_toColour(RValue_makeReal((GMLReal)self->imageBlend));
    uint32_t shadow = Color_lerp((int32_t)tint, 0, 0.4f);
    setNativeReal(self, backdropVars[BACKDROP_BLEND], (GMLReal)shadow);
    Renderer* renderer = ctx->runner->renderer;
    GMLReal x = self->x, y = self->y, scale = self->imageXscale;
    floweyScenerySprite(renderer, self, self->spriteIndex, x + (xs[2] - (GMLReal)60) * scale,
                       (y + ys[2]) - (GMLReal)20, growth[2] + scale, 1, shadow);
    floweyScenerySprite(renderer, self, self->spriteIndex, x + (xs[1] - (GMLReal)40) * scale,
                       (y + ys[1]) - (GMLReal)10, growth[1] + scale, 1, shadow);
    floweyScenerySprite(renderer, self, self->spriteIndex, x + xs[0] * scale,
                       y + ys[0], growth[0] + scale, 1, tint);
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
    Instance *self = floweyBackdropInstance(ctx);
    GMLReal phase;
    if (!floweySceneryPose(self) || !floweyBackdropPhase(ctx, self, &phase)) return false;
    GMLReal cosine = GMLReal_cos(phase / (GMLReal)6);
    GMLReal scale = (GMLReal)self->imageXscale;
    GMLReal growth[] = { (GMLReal)1 + backdropSin(phase / (GMLReal)6) * (GMLReal)0.05,
                         (GMLReal)1 + cosine * (GMLReal)0.05,
                         (GMLReal)1 - GMLReal_sin(phase / (GMLReal)7) * (GMLReal)0.05 };
    GMLReal xs[] = { GMLReal_sin(phase / (GMLReal)4) * (GMLReal)2 * scale,
                     GMLReal_sin(phase / (GMLReal)5) * scale, cosine * (GMLReal)0.5 * scale };
    for (int i = 0; i < 3; ++i)
        setBackdropVineGrowth(self, BACKDROP_GROWTH + i, growth[i]);
    for (int i = 0; i < 3; ++i)
        setNativeReal(self, backdropVars[BACKDROP_SSX + i], xs[i]);
    Renderer* renderer = ctx->runner->renderer;
    uint32_t tint = RValue_toColour(RValue_makeReal((GMLReal)self->imageBlend));
    GMLReal x = self->x, y = self->y;
    floweyScenerySprite(renderer, self, 2298, (x - xs[2]) + (GMLReal)20, y, scale, growth[2], tint);
    floweyScenerySprite(renderer, self, 2297, (x - xs[1]) - (GMLReal)20, y, scale, growth[1], tint);
    floweyScenerySprite(renderer, self, 2296, x - xs[0], y, scale, growth[0], tint);
    return true;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool floweySideStalk(VMContext* ctx) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    Instance* self = floweyBackdropInstance(ctx);
    GMLReal frozen;
    GMLReal epsilon = ctx->runner->mathEpsilon;
    if (!floweySceneryPose(self) || !readNativeNumber(self, backdropVars[BACKDROP_FROZEN], &frozen) ||
        !(epsilon >= 0 && epsilon < (GMLReal)0.5)) return false;
    RValue state = Instance_getSelfVar(self, backdropVars[BACKDROP_PHASE]);
    bool advancing = GMLReal_fabs(frozen) <= epsilon;
    if (state.type == RVALUE_INT32) {
        if (advancing && state.int32 == INT32_MAX) return false;
        if (advancing) ++state.int32;
    } else if (state.type == RVALUE_REAL && isfinite(state.real)) {
        if (advancing) state.real += (GMLReal)1;
    } else {
        return false;
    }
    GMLReal phase = RValue_toReal(state);
    if (advancing) {
        state.gmlStackType = GML_TYPE_VARIABLE;
        Instance_setSelfVar(self, backdropVars[BACKDROP_PHASE], state);
    }
    GMLReal scale = self->imageXscale;
    GMLReal growth = (GMLReal)1 + GMLReal_cos(phase / (GMLReal)5) * (GMLReal)0.01;
    GMLReal ssx = backdropSin(phase / (GMLReal)3) * (GMLReal)2 * scale;
    GMLReal ssy = GMLReal_cos(phase / (GMLReal)3) * (GMLReal)2;
    setBackdropVineGrowth(self, BACKDROP_GROWTH, growth);
    setNativeReal(self, backdropVars[BACKDROP_SSX], ssx);
    setNativeReal(self, backdropVars[BACKDROP_SSY], ssy);
    self->imageSpeed = advancing ? 3.0f : 1.0f;
    uint32_t tint = RValue_toColour(RValue_makeReal((GMLReal)self->imageBlend));
    floweyScenerySprite(ctx->runner->renderer, self, self->spriteIndex, (GMLReal)self->x - ssx,
                       (GMLReal)self->y + ssy, scale, growth, tint);
    return true;
}

// Both variants of the idle arm have the same draw order and writes. Leave
// active arm reaches and their attack spawns in the original bytecode.
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool floweyIdleArm(VMContext* ctx) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    Instance* self = floweyBackdropInstance(ctx);
    if (!floweySceneryPose(self)) return false;
    GMLReal epsilon = ctx->runner->mathEpsilon;
    GMLReal state, frozen, phase, desperate;
    if (!(epsilon >= 0 && epsilon < (GMLReal)0.5) ||
        !readNativeNumber(self, backdropVars[BACKDROP_ARM_STATE], &state) || GMLReal_fabs(state) > epsilon ||
        !readNativeNumber(self, backdropVars[BACKDROP_FROZEN], &frozen) ||
        !readNativeNumber(self, backdropVars[BACKDROP_PHASE], &phase) ||
        !readNativeNumber(self, backdropVars[BACKDROP_DESPERATE], &desperate)) return false;
    bool faster = GMLReal_fabs(desperate - (GMLReal)1) <= epsilon;
    if (!faster && GMLReal_fabs(desperate) > epsilon) return false;

    if (GMLReal_fabs(frozen) <= epsilon) {
        phase += faster ? (GMLReal)1.2 : (GMLReal)0.8;
        setNativeReal(self, backdropVars[BACKDROP_PHASE], phase);
    }
    GMLReal scale = self->imageXscale;
    GMLReal growth = (GMLReal)1 + GMLReal_cos(phase / (GMLReal)6) * (GMLReal)0.03;
    GMLReal ssx = backdropSin(phase / (GMLReal)3) * (faster ? (GMLReal)5 : (GMLReal)4) * scale;
    GMLReal ssy = GMLReal_cos(phase / (GMLReal)3) * (faster ? (GMLReal)8 : (GMLReal)4);
    setBackdropVineGrowth(self, BACKDROP_GROWTH, growth);
    setNativeReal(self, backdropVars[BACKDROP_SSX], ssx);
    setNativeReal(self, backdropVars[BACKDROP_SSY], ssy);
    GMLReal y = (GMLReal)self->y + ssy;
    if (faster) y += (GMLReal)2;
    uint32_t tint = RValue_toColour(RValue_makeReal((GMLReal)self->imageBlend));
    floweyScenerySprite(ctx->runner->renderer, self, self->spriteIndex,
                       (GMLReal)self->x - ssx, y, scale, growth, tint);
    setNativeReal(self, backdropVars[BACKDROP_ARM_MIRROR], -scale);
    return true;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool floweyFleshFace(VMContext* ctx) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    Instance* self = floweyBackdropInstance(ctx);
    if (!floweySceneryPose(self)) return false;
    GMLReal epsilon = ctx->runner->mathEpsilon;
    GMLReal phase, frozen, desperate;
    if (!(epsilon >= 0 && epsilon < (GMLReal)0.5) ||
        !readNativeNumber(self, backdropVars[BACKDROP_FROZEN], &frozen) ||
        !readNativeNumber(self, backdropVars[BACKDROP_PHASE], &phase) ||
        !readNativeNumber(self, backdropVars[BACKDROP_DESPERATE], &desperate)) return false;
    bool faster = GMLReal_fabs(desperate - (GMLReal)1) <= epsilon;
    if (!faster && GMLReal_fabs(desperate) > epsilon) return false;
    if (phase < (GMLReal)INT32_MIN || phase > (GMLReal)INT32_MAX) return false;

    bool advancing = GMLReal_fabs(frozen) <= epsilon;
    if (advancing) phase += (GMLReal)1.2;
    if (!faster) phase -= (GMLReal)0.2;
    if (advancing || !faster) setNativeReal(self, backdropVars[BACKDROP_PHASE], phase);

    Renderer* renderer = ctx->runner->renderer;
    if (renderer == NULL || SWRenderer_isSkippingScreen(renderer)) return true;
    GMLReal sway = GMLReal_sin(phase / (GMLReal)2);
    GMLReal x = self->x, y = self->y;
    uint32_t tint = RValue_toColour(RValue_makeReal((GMLReal)self->imageBlend));
    floweyScenerySprite(renderer, self, self->spriteIndex, x, y + sway * (GMLReal)3, 1, 1, tint);
    int frame = (int)(phase / (GMLReal)8);
    if (frame < 0) frame = (int)self->imageIndex;
    GMLReal noseY = (y + (faster ? (GMLReal)14 : (GMLReal)12)) +
                    sway * (faster ? (GMLReal)7 : (GMLReal)6);
    Renderer_drawSpriteExt(renderer, 2260, frame, x + (GMLReal)32, noseY, 1, 1, 0, tint, 1);
    return true;
}

#ifndef ENABLE_VM_TRACING
static bool floweySkippingDraw(const VMContext *ctx) {
    const Runner *runner = ctx->runner;
    return runner != NULL && SWRenderer_isSkippingScreen(runner->renderer) && ofPlatformSpeedMode() &&
           runner->currentRoom != NULL && runner->currentRoom->name != NULL &&
           strcmp(runner->currentRoom->name, "room_floweyx") == 0 &&
           ctx->dataWin->gen8.wadVersion == 16 && ctx->dataWin->gen8.gameID == 864738521u &&
           ctx->currentCallFrame->eventType == EVENT_DRAW && ctx->currentCallFrame->eventSubtype == DRAW_NORMAL;
}

static void skipFloweyDrawSpan(uint8_t *bytecode, uint32_t from, uint32_t to) {
    BinaryUtils_writeUint32(bytecode + from, ((uint32_t)OP_B << 24) | ((to - from) / 4));
}

static bool floweyFlipeyeDraw(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
    (void)args;
    (void)argCount;
    if (!floweySkippingDraw(ctx)) return false;
    uint8_t *bytecode = (uint8_t *)flipeyeSkippedWords;
    if (!flipeyeSkippedReady) {
        if (ctx->ip != 0) return false;
        memcpy(bytecode, ctx->bytecodeBase, sizeof(flipeyeSkippedWords));
        /* Both blocks only compute arguments and draw eye sprites. Keep
         * the original update/spawn logic and the op = 126 write between
         * the blocks. Linked operands keep their original byte offsets. */
        skipFloweyDrawSpan(bytecode, 0x0c38, 0x151c);
        skipFloweyDrawSpan(bytecode, 0x1528, FLOWEY_FLIPEYE_DRAW_LENGTH);
        flipeyeSkippedReady = true;
    }
    *result = VM_executeCurrentBytecode(ctx, bytecode);
    return true;
}

static bool floweyMouthDraw(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
    (void)args;
    (void)argCount;
    if (!floweySkippingDraw(ctx)) return false;
    uint8_t *bytecode = (uint8_t *)mouthSkippedWords;
    if (!mouthSkippedReady) {
        if (ctx->ip != 0) return false;
        memcpy(bytecode, ctx->bytecodeBase, sizeof(mouthSkippedWords));
        /* Skip only the sprite arguments and draws. Keep animation, attacks,
         * and the debug/laugh logic following the last span. */
        skipFloweyDrawSpan(bytecode, 0x01ec, 0x0288);
        skipFloweyDrawSpan(bytecode, 0x03e4, 0x0440);
        skipFloweyDrawSpan(bytecode, 0x0644, 0x10e8);
        mouthSkippedReady = true;
    }
    *result = VM_executeCurrentBytecode(ctx, bytecode);
    return true;
}

static bool floweyVinePreviewDraw(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
    (void)args;
    (void)argCount;
    if (!floweySkippingDraw(ctx)) return false;
    uint8_t *bytecode = (uint8_t *)previewSkippedWords;
    if (!previewSkippedReady) {
        if (ctx->ip != 0) return false;
        memcpy(bytecode, ctx->bytecodeBase, sizeof(previewSkippedWords));
        // Keep the colour, direction, timer and spawn logic. Skip only the
        // two warning-line draws when their destination is held.
        skipFloweyDrawSpan(bytecode, 0x010c, 0x01c4);
        previewSkippedReady = true;
    }
    *result = VM_executeCurrentBytecode(ctx, bytecode);
    return true;
}

static bool floweyLeftEyeDraw(VMContext *ctx, RValue *args, int32_t argCount, RValue *result) {
    (void)args;
    (void)argCount;
    if (!floweySkippingDraw(ctx)) return false;
    uint8_t *bytecode = (uint8_t *)leftEyeSkippedWords;
    if (!leftEyeSkippedReady) {
        if (ctx->ip != 0) return false;
        memcpy(bytecode, ctx->bytecodeBase, sizeof(leftEyeSkippedWords));
        // The alternate path at 0x0e0c still consumes both random draws.
        // Preserve the attack logic and the op write at 0x11ec.
        skipFloweyDrawSpan(bytecode, 0x0b7c, 0x0ce8);
        skipFloweyDrawSpan(bytecode, 0x0d2c, 0x0f18);
        skipFloweyDrawSpan(bytecode, 0x0f18, 0x11e8);
        skipFloweyDrawSpan(bytecode, 0x1010, 0x11e8);
        skipFloweyDrawSpan(bytecode, 0x11f4, FLOWEY_LEFT_EYE_DRAW_LENGTH);
        leftEyeSkippedReady = true;
    }
    *result = VM_executeCurrentBytecode(ctx, bytecode);
    return true;
}

static const OfNativeScript g_nativeScripts[] = {
    { "gml_Object_obj_floweyx_lefteye_Draw_0", FLOWEY_LEFT_EYE_DRAW_LENGTH, 0x1cd174a60c76f9a2ull, floweyLeftEyeDraw },
    { "gml_Object_obj_gigavine_preview_Draw_0", FLOWEY_PREVIEW_DRAW_LENGTH, 0xc4b29d651d8f182full, floweyVinePreviewDraw },
    { "gml_Object_obj_floweyx_flipeye_Draw_0", FLOWEY_FLIPEYE_DRAW_LENGTH, 0x2ffb1453038ba855ull, floweyFlipeyeDraw },
    { "gml_Object_obj_floweyx_mouth_Draw_0", FLOWEY_MOUTH_DRAW_LENGTH, 0xee4dc253c9466abbull, floweyMouthDraw },
};
#endif

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

enum { FLOWEY_VINE_ENTRY = 900, FLOWEY_FINALE_ENTRY = 1430 };
static const OfJump g_floweyJumps[] = {
    { FLOWEY_VINE_ENTRY, 294, "fplot=3", NULL },
    { FLOWEY_FINALE_ENTRY, 294, "fplot=7", NULL },
};

/* The first composite lands at entry + 2. The rest of the fade ends
 * before +14, and the TV laugh begins at +33 and runs for 180 updates.
 * Both checkpoints use this same authored intro. */
static const OfBenchOpeningWindow g_floweyOpening[] = {
    { "cold",   FLOWEY_VINE_ENTRY,      FLOWEY_VINE_ENTRY + 3 },
    { "fade",   FLOWEY_VINE_ENTRY + 3,  FLOWEY_VINE_ENTRY + 14 },
    { "hold",   FLOWEY_VINE_ENTRY + 14, FLOWEY_VINE_ENTRY + 33 },
    { "laugh",  FLOWEY_VINE_ENTRY + 33, FLOWEY_VINE_ENTRY + 213 },
    { "return", FLOWEY_FINALE_ENTRY,      FLOWEY_FINALE_ENTRY + 3 },
    { "fade2",  FLOWEY_FINALE_ENTRY + 3,  FLOWEY_FINALE_ENTRY + 14 },
    { "hold2",  FLOWEY_FINALE_ENTRY + 14, FLOWEY_FINALE_ENTRY + 33 },
    { "laugh2", FLOWEY_FINALE_ENTRY + 33, FLOWEY_FINALE_ENTRY + 213 },
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
    .ioDivisor = 1u,
    .ioAsyncBase = 110u,
    .ioAsyncStep = 2u,
    .savestateReason = savestateReason,
};
