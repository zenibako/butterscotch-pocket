/* The scissor dancers of room 85 (obj_scissordancer's Step), written out
 * in C. Included by game_deltarune.c, which lists scissorDancerStep in its
 * natives and forgets scissorDancerResolved with the other natives' state.
 *
 * Room 85 (room_forest_dancers1) has 23 of them. Their Step is some 65
 * operations each, and the scr_depth it calls another 11: close to half the
 * game code the room runs in a frame.
 *
 *   dancetimer += 1
 *   if (dancetimer >= 30 && dancetimer <= 75) image_index += 0.25
 *   if (dancetimer >= 112 && dancetimer <= 157) image_index -= 0.25
 *   if (dancetimer >= 166) dancetimer = 0
 *   if (specturn == 1) { ...turns at the corners of its square... }
 *   if (myinteract == 1 && sndcon == 0) { sndcon = 1; myinteract = 0 } else myinteract = 2
 *   if (sndcon >= 1 && sndcon <= 5) { ...the snipping sounds... }
 *   if (sndcon == 6) { ... }
 *   scr_depth()                      depth = 100000 - (y * 10 + sprite_height * 10)
 *   if (instance_exists(bullet)) { bullet.x = x + hspeed; bullet.y = y + vspeed }
 *
 * Only the dance is written out. A dancer turning corners (specturn, room
 * 120), being talked to (myinteract 1) or snipping (sndcon 1 to 6) is left
 * to the bytecode, which is decided before anything is changed, as is any
 * variable holding something other than a number. The arithmetic and the
 * comparisons are the VM's: one operation at a time in GMLReal, whole
 * numbers kept whole where the VM keeps them so (a whole-number constant
 * stored straight into a variable becomes a real, as the VM's pop makes it),
 * and comparisons within math_get_epsilon. */

static bool scissorDancerResolved;
static int32_t dancerTimer, dancerSpecturn, dancerInteract, dancerSndcon, dancerBullet;
static BuiltinFunc dancerInstanceExists;

static void scissorDancerResolve(VMContext *ctx) {
    if (scissorDancerResolved) return;
    dancerTimer = VM_getOrAllocateVarID(ctx, "dancetimer");
    dancerSpecturn = VM_getOrAllocateVarID(ctx, "specturn");
    dancerInteract = VM_getOrAllocateVarID(ctx, "myinteract");
    dancerSndcon = VM_getOrAllocateVarID(ctx, "sndcon");
    dancerBullet = VM_getOrAllocateVarID(ctx, "bullet");
    dancerInstanceExists = VM_findBuiltin(ctx, "instance_exists");
    scissorDancerResolved = true;
}

static GMLReal dancerReal(RValue value) {
    return value.type == RVALUE_INT32 ? (GMLReal) value.int32 : value.real;
}

/* OP_CMP on two numbers: equal within the epsilon, otherwise by sign. */
static int dancerCompare(VMContext *ctx, GMLReal a, GMLReal b) {
    volatile GMLReal diff = a - b;
    GMLReal epsilon = ctx->runner != NULL ? ctx->runner->mathEpsilon : GML_MATH_EPSILON;
    return GMLReal_fabs(diff) <= epsilon ? 0 : (diff < 0 ? -1 : 1);
}

/* OP_ADD, OP_SUB and OP_MUL on an INT32 or REAL and an INT32 or REAL:
 * whole numbers stay whole, anything else is one GMLReal operation. */
static RValue dancerArith(char op, RValue a, RValue b) {
    if (a.type == RVALUE_INT32 && b.type == RVALUE_INT32)
        return RValue_makeInt32(op == '+' ? a.int32 + b.int32 : op == '-' ? a.int32 - b.int32 : a.int32 * b.int32);
    GMLReal x = (a.type == RVALUE_INT32 || a.type == RVALUE_REAL) ? dancerReal(a) : RValue_toReal(a);
    GMLReal y = (b.type == RVALUE_INT32 || b.type == RVALUE_REAL) ? dancerReal(b) : RValue_toReal(b);
    volatile GMLReal result = op == '+' ? x + y : op == '-' ? x - y : x * y;
    return RValue_makeReal(result);
}

static RValue dancerGet(VMContext *ctx, Instance *inst, int16_t id, const char *name) {
    return VMBuiltins_getVariable(ctx, inst, id, name, -1);
}

static void dancerSet(VMContext *ctx, Instance *inst, int16_t id, const char *name, RValue value) {
    VMBuiltins_setVariable(ctx, inst, id, name, value, -1);
    RValue_free(&value);
}

static bool scissorDancerStep(VMContext *ctx) {
    Instance *self = ctx->currentCallFrame->self;
    if (self == NULL || self->objectIndex == STRUCT_OBJECT_INDEX) return false;
    scissorDancerResolve(ctx);
    if (dancerInstanceExists == NULL) return false;

    RValue timer = Instance_getSelfVar(self, dancerTimer), specturn = Instance_getSelfVar(self, dancerSpecturn);
    RValue interact = Instance_getSelfVar(self, dancerInteract), sndcon = Instance_getSelfVar(self, dancerSndcon);
    RValue bullet = Instance_getSelfVar(self, dancerBullet);
    if (!isNumber(timer) || !isNumber(specturn) || !isNumber(interact) || !isNumber(sndcon) || !isNumber(bullet))
        return false;
    /* The turns, the talk and the snipping are left to the bytecode. */
    if (dancerCompare(ctx, dancerReal(specturn), 1) == 0 || dancerCompare(ctx, dancerReal(interact), 1) == 0) return false;
    GMLReal snd = dancerReal(sndcon);
    if ((dancerCompare(ctx, snd, 1) >= 0 && dancerCompare(ctx, snd, 5) <= 0) || dancerCompare(ctx, snd, 6) == 0) return false;
    /* bullet.x = ... through an object index would write every instance of that object,
     * and through self, other or the like to this one: only an instance or noone here. */
    int32_t bulletId = RValue_toInt32(bullet);
    if (bulletId < INSTANCE_ID_BASE && bulletId != INSTANCE_NOONE) return false;

    /* dancetimer += 1, and the dance's two halves. */
    timer = dancerArith('+', timer, RValue_makeInt32(1));
    Instance_setSelfVar(self, dancerTimer, timer);
    GMLReal t = dancerReal(timer);
    if (dancerCompare(ctx, t, 30) >= 0 && dancerCompare(ctx, t, 75) <= 0)
        dancerSet(ctx, self, BUILTIN_VAR_IMAGE_INDEX, "image_index",
                  dancerArith('+', dancerGet(ctx, self, BUILTIN_VAR_IMAGE_INDEX, "image_index"), RValue_makeReal((GMLReal) 0.25)));
    if (dancerCompare(ctx, t, 112) >= 0 && dancerCompare(ctx, t, 157) <= 0)
        dancerSet(ctx, self, BUILTIN_VAR_IMAGE_INDEX, "image_index",
                  dancerArith('-', dancerGet(ctx, self, BUILTIN_VAR_IMAGE_INDEX, "image_index"), RValue_makeReal((GMLReal) 0.25)));
    if (dancerCompare(ctx, t, 166) >= 0) Instance_setSelfVar(self, dancerTimer, RValue_makeReal((GMLReal) 0));

    /* Not talked to: myinteract = 2. */
    Instance_setSelfVar(self, dancerInteract, RValue_makeReal((GMLReal) 2));

    /* scr_depth(): depth = 100000 - (y * 10 + sprite_height * 10) */
    RValue yTen = dancerArith('*', dancerGet(ctx, self, BUILTIN_VAR_Y, "y"), RValue_makeInt32(10));
    RValue heightTen = dancerArith('*', dancerGet(ctx, self, BUILTIN_VAR_SPRITE_HEIGHT, "sprite_height"), RValue_makeInt32(10));
    dancerSet(ctx, self, BUILTIN_VAR_DEPTH, "depth", dancerArith('-', RValue_makeInt32(100000), dancerArith('+', yTen, heightTen)));

    /* The bullet (its hit box) follows the dancer a frame ahead. */
    RValue arg = bullet;
#ifdef ENABLE_VM_GML_PROFILER
    if (ctx->profiler != NULL) Profiler_enter(ctx->profiler, "instance_exists");
#endif
    RValue exists = dancerInstanceExists(ctx, &arg, 1);
#ifdef ENABLE_VM_GML_PROFILER
    if (ctx->profiler != NULL) Profiler_exit(ctx->profiler);
#endif
    bool follow = RValue_toBool(exists);
    RValue_free(&exists);
    if (follow) {
        RValue bx = dancerArith('+', dancerGet(ctx, self, BUILTIN_VAR_X, "x"), dancerGet(ctx, self, BUILTIN_VAR_HSPEED, "hspeed"));
        Instance *target = VM_findInstanceByTarget(ctx, bulletId);
        if (target != NULL) dancerSet(ctx, target, BUILTIN_VAR_X, "x", bx);
        RValue by = dancerArith('+', dancerGet(ctx, self, BUILTIN_VAR_Y, "y"), dancerGet(ctx, self, BUILTIN_VAR_VSPEED, "vspeed"));
        target = VM_findInstanceByTarget(ctx, bulletId);
        if (target != NULL) dancerSet(ctx, target, BUILTIN_VAR_Y, "y", by);
    }
    return true;
}
