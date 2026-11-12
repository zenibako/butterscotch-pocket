/* Undertale's Photoshop Flowey fight (room_floweyx): the native stand-ins
 * for its vines, eye shots, pipes, backdrop, side stalks, arms and face, and
 * the draws whose decorative spans Speed mode skips. Included by
 * game_undertale.c, which keeps each native's "resolved" flag (cleared by
 * forgetNativeVm), the shared readNativeNumber and setNativeReal helpers,
 * and the tables that install these. */

static int32_t floweyVineActive, floweyVineCon, floweyEyeShotSize;

enum {
    PIPE_FROZEN, PIPE_PHASE, PIPE_FLASH, PIPE_TINT, PIPE_I, PIPE_MIRROR_OFFSET,
    PIPE_X, PIPE_Y, PIPE_SWELL, PIPE_ANGLE, PIPE_X2, PIPE_Y2, PIPE_SWELL2, PIPE_ANGLE2,
    PIPE_ANGLE_OFFSET, PIPE_SOUL_RESCUE, PIPE_DEBUG, PIPE_FIELD_COUNT
};
static int32_t pipeVars[PIPE_FIELD_COUNT];

enum {
    BACKDROP_PHASE, BACKDROP_FROZEN, BACKDROP_I, BACKDROP_LAYER_PHASE, BACKDROP_BLEND,
    BACKDROP_GROWTH, BACKDROP_GROWTH2, BACKDROP_GROWTH3,
    BACKDROP_MSIN, BACKDROP_MSIN2, BACKDROP_MSIN3, BACKDROP_YSIN, BACKDROP_YSIN2, BACKDROP_YSIN3,
    BACKDROP_SSX, BACKDROP_SSX2, BACKDROP_SSX3, BACKDROP_SSY, BACKDROP_DESPERATE, BACKDROP_ARM_STATE, BACKDROP_ARM_MIRROR, BACKDROP_FIELD_COUNT
};
static int32_t backdropVars[BACKDROP_FIELD_COUNT];

#ifndef ENABLE_VM_TRACING
enum { FLOWEY_FLIPEYE_DRAW_LENGTH = 7832, FLOWEY_MOUTH_DRAW_LENGTH = 4992,
       FLOWEY_PREVIEW_DRAW_LENGTH = 672, FLOWEY_LEFT_EYE_DRAW_LENGTH = 6356 };
static uint32_t flipeyeSkippedWords[FLOWEY_FLIPEYE_DRAW_LENGTH / 4];
static uint32_t mouthSkippedWords[FLOWEY_MOUTH_DRAW_LENGTH / 4];
static uint32_t previewSkippedWords[FLOWEY_PREVIEW_DRAW_LENGTH / 4];
static uint32_t leftEyeSkippedWords[FLOWEY_LEFT_EYE_DRAW_LENGTH / 4];
#endif

typedef struct {
    int last, xOrigin, xRadius, yOrigin, yRadius;
    GMLReal yDivisor;
} FloweyPipeWave;

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
#endif /* !ENABLE_VM_TRACING */
