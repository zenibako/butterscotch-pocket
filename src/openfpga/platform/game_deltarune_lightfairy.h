/* Deltarune's light fairies (obj_lightfairy), written out in C. Included by
 * game_deltarune.c, which lists lightFairyStep and lightFairyDraw in its
 * natives and forgets lightFairyResolved with the other natives' state.
 *
 * Room 85 (room_forest_dancers1, past the Bake Sale) has about ninety of
 * them, and each runs two short events a frame, about sixty operations in
 * all: some 5,000 a frame for the room, or a third of its game code.
 *
 *   Step:  x += sin(siner / sinx) * period
 *          y += sin(siner / siny) * period
 *          siner += 1
 *   Draw:  image_alpha = sin(siner / 26) * factor
 *          draw_sprite_ext(sprite_index, siner / 10, x, y,
 *                          3 + sin(siner / 12), 3 + sin(siner / 12), 0, c_white, image_alpha)
 *
 * The arithmetic is the VM's, one operation at a time in GMLReal, with
 * whole-number operands kept whole where the VM keeps them so (siner + 1).
 * Anything but numbers in those variables is left to the bytecode. */

static bool lightFairyResolved;
static int32_t fairySiner, fairySinx, fairySiny, fairyPeriod, fairyFactor;
static BuiltinFunc fairyDrawSpriteExt;

static void lightFairyResolve(VMContext *ctx) {
    if (lightFairyResolved) return;
    fairySiner = VM_getOrAllocateVarID(ctx, "siner");
    fairySinx = VM_getOrAllocateVarID(ctx, "sinx");
    fairySiny = VM_getOrAllocateVarID(ctx, "siny");
    fairyPeriod = VM_getOrAllocateVarID(ctx, "period");
    fairyFactor = VM_getOrAllocateVarID(ctx, "factor");
    fairyDrawSpriteExt = VM_findBuiltin(ctx, "draw_sprite_ext");
    lightFairyResolved = true;
}

static GMLReal fairyReal(RValue value) {
    return value.type == RVALUE_INT32 ? (GMLReal) value.int32 : value.real;
}

/* One VM operation's rounding: the product is stored before it is added
 * to, as the VM stores each result on its stack, so no fused multiply-add
 * can stand in for the two. */
static GMLReal fairyMul(GMLReal a, GMLReal b) {
    volatile GMLReal product = a * b;
    return product;
}

static GMLReal fairyDiv(GMLReal a, GMLReal b) {
    volatile GMLReal quotient = a / b;
    return quotient;
}

static bool lightFairyStep(VMContext *ctx) {
    Instance *self = ctx->currentCallFrame->self;
    if (self == NULL) return false;
    lightFairyResolve(ctx);
    RValue siner = Instance_getSelfVar(self, fairySiner), sinx = Instance_getSelfVar(self, fairySinx);
    RValue siny = Instance_getSelfVar(self, fairySiny), period = Instance_getSelfVar(self, fairyPeriod);
    if (!isNumber(siner) || !isNumber(sinx) || !isNumber(siny) || !isNumber(period)) return false;

    GMLReal s = fairyReal(siner), p = fairyReal(period);
    GMLReal x = (GMLReal) self->x + fairyMul(GMLReal_sin(fairyDiv(s, fairyReal(sinx))), p);
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_X, "x", RValue_makeReal(x), -1);
    GMLReal y = (GMLReal) self->y + fairyMul(GMLReal_sin(fairyDiv(s, fairyReal(siny))), p);
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_Y, "y", RValue_makeReal(y), -1);
    Instance_setSelfVar(self, fairySiner, siner.type == RVALUE_INT32 ? RValue_makeInt32(siner.int32 + 1)
                                                                     : RValue_makeReal(siner.real + (GMLReal) 1));
    return true;
}

static bool lightFairyDraw(VMContext *ctx) {
    Instance *self = ctx->currentCallFrame->self;
    if (self == NULL || ctx->runner->renderer == NULL) return false;
    lightFairyResolve(ctx);
    if (fairyDrawSpriteExt == NULL) return false;
    RValue siner = Instance_getSelfVar(self, fairySiner), factor = Instance_getSelfVar(self, fairyFactor);
    if (!isNumber(siner) || !isNumber(factor)) return false;

    GMLReal s = fairyReal(siner);
    VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_ALPHA, "image_alpha",
                           RValue_makeReal(fairyMul(GMLReal_sin(fairyDiv(s, (GMLReal) 26)), fairyReal(factor))), -1);
    GMLReal scale = (GMLReal) 3 + GMLReal_sin(fairyDiv(s, (GMLReal) 12));
    RValue args[9] = {
        RValue_makeReal((GMLReal) self->spriteIndex),
        RValue_makeReal(fairyDiv(s, (GMLReal) 10)),
        RValue_makeReal((GMLReal) self->x),
        RValue_makeReal((GMLReal) self->y),
        RValue_makeReal(scale),
        RValue_makeReal(scale),
        RValue_makeInt32(0),
        RValue_makeInt32(16777215),
        RValue_makeReal((GMLReal) self->imageAlpha),
    };
#ifdef ENABLE_VM_GML_PROFILER
    if (ctx->profiler != NULL) Profiler_enter(ctx->profiler, "draw_sprite_ext");
#endif
    RValue result = fairyDrawSpriteExt(ctx, args, 9);
#ifdef ENABLE_VM_GML_PROFILER
    if (ctx->profiler != NULL) Profiler_exit(ctx->profiler);
#endif
    RValue_free(&result);
    return true;
}
