/* Active Forest sentries only. Creation, warm-up, unusual values and modified
 * bytecode stay interpreted. All guards precede the first script-visible write. */
enum {
    S_ROT, S_SPEED, S_EYE, S_OFFX, S_OFFY, S_BOSS, S_CON, S_MADE, S_MAX,
    S_SINER, S_XRANGE, S_YRANGE, S_FB, S_I, S_XX, S_YY, S_XA, S_YA, S_BATTLE, S_COUNT
};
static int32_t sentryVars[S_COUNT];
static bool sentryResolved;

static RValue sentryGet(Instance *inst, int field) {
    return Instance_getSelfVar(inst, sentryVars[field]);
}
static void sentrySet(Instance *inst, int field, RValue value) {
    Instance_setSelfVar(inst, sentryVars[field], value);
}
static bool sentryFinite(RValue value) {
    return isNumber(value) && isfinite(RValue_toReal(value)) &&
           GMLReal_fabs(RValue_toReal(value)) <= (GMLReal)1000000;
}
/* These guarded operands cannot overflow. Preserve the VM's integer result
 * when both operands are integers, including builtin argument value types. */
static RValue sentryAdd(RValue a, RValue b) {
    if (a.type == RVALUE_INT32 && b.type == RVALUE_INT32) return RValue_makeInt32(a.int32 + b.int32);
    return RValue_makeReal(RValue_toReal(a) + RValue_toReal(b));
}
static GMLReal sentryCall(VMContext *ctx, BuiltinFunc fn, RValue *args, int count) {
    RValue value = fn(ctx, args, count);
    GMLReal result = RValue_toReal(value);
    RValue_free(&value);
    return result;
}
static void sentryProperty(VMContext *ctx, Instance *inst, int16_t field, const char *name,
                           int32_t owner, GMLReal number) {
    /* SETOWNER is observable by later array writes even though this property
     * write itself does not touch an array. Tokens belong to the guarded code. */
    ctx->currentArrayOwner = (void *)(intptr_t)owner;
    VMBuiltins_setVariable(ctx, inst, field, name, RValue_makeReal(number), -1);
}
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool forestSentryStep(VMContext *ctx) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    /* Each VM arithmetic opcode rounds separately; a fused multiply/add would
     * change alpha and coordinates even when the expression looks equivalent. */
    Runner *runner = ctx->runner;
    Instance *self = ctx->currentCallFrame->self;
    if (runner == NULL || self == NULL || runner->dataWin == NULL || !isfinite(self->x) || !isfinite(self->y) ||
        runner->mathEpsilon != GML_MATH_EPSILON ||
        runner->dataWin->objt.count <= 185 || runner->instancesByObject == NULL) return false;
    if (!sentryResolved) {
        static const char *const names[S_COUNT] = {
            "rot", "rotspeed", "eyer", "offx", "offy", "boss", "con", "made", "bmax",
            "siner", "xrange", "yrange", "fb", "i", "xx", "yy", "xa", "ya", "battlemode"
        };
        for (int i = 0; i < S_COUNT; ++i) sentryVars[i] = VM_getOrAllocateVarID(ctx, names[i]);
        sentryResolved = true;
    }
    for (int i = S_ROT; i <= S_YRANGE; ++i)
        if (!sentryFinite(sentryGet(self, i))) return false;
    GMLReal epsilon = runner->mathEpsilon;
    if (GMLReal_fabs(RValue_toReal(sentryGet(self, S_CON))) <= epsilon ||
        GMLReal_fabs(RValue_toReal(sentryGet(self, S_MADE)) - 1) > epsilon) return false;
    RValue bullets = sentryGet(self, S_FB);
    GMLReal maximum = RValue_toReal(sentryGet(self, S_MAX));
    if (maximum < 0 || maximum > 16 || bullets.type != RVALUE_ARRAY || bullets.array == NULL ||
        bullets.array->type != GML_MODERN_ARRAY || GMLArray_length1D(bullets.array) < (int32_t)ceilf(maximum)) return false;
    for (int i = 0; lessThan((GMLReal)i, maximum, epsilon); ++i) {
        RValue id = GMLArray_get(bullets.array, i);
        if (!isNumber(id) || !isfinite(RValue_toReal(id)) || RValue_toReal(id) < INSTANCE_ID_BASE ||
            RValue_toReal(id) > (GMLReal)2000000000 ||
            RValue_toReal(id) != (GMLReal)RValue_toInt32(id)) return false;
        Instance *target = VM_findInstanceByTarget(ctx, RValue_toInt32(id));
        if (target != NULL && target->objectIndex != 187) return false;
    }
    bool boss = GMLReal_fabs(RValue_toReal(sentryGet(self, S_BOSS)) - 1) <= epsilon;
    Instance *player = boss ? VM_findInstanceByTarget(ctx, 30) : NULL;
    if (player != NULL) {
        if (!isfinite(player->x) || !isfinite(player->y)) return false;
        Instance **statues = runner->instancesByObject[185];
        for (int i = 0; i < arrlen(statues); ++i) {
            Instance *statue = statues[i];
            if (statue->active && (!isfinite(statue->x) || !isfinite(statue->y) ||
                !sentryFinite(sentryGet(statue, S_XRANGE)) || !sentryFinite(sentryGet(statue, S_YRANGE)))) return false;
        }
    }
    BuiltinFunc lengthX = VM_findBuiltin(ctx, "lengthdir_x"), lengthY = VM_findBuiltin(ctx, "lengthdir_y");
    BuiltinFunc sine = VM_findBuiltin(ctx, "sin"), exists = VM_findBuiltin(ctx, "instance_exists");
    if (lengthX == NULL || lengthY == NULL || sine == NULL || exists == NULL) return false;

    /* No fallback below: preserve the event's operation and builtin call order. */
    RValue rot = sentryAdd(sentryGet(self, S_ROT), sentryGet(self, S_SPEED));
    sentrySet(self, S_ROT, rot);
    RValue direction[2] = {sentryGet(self, S_EYE), rot};
    GMLReal component = sentryCall(ctx, lengthX, direction, 2);
    sentrySet(self, S_XX, RValue_makeReal(component + RValue_toReal(sentryGet(self, S_OFFX))));
    component = sentryCall(ctx, lengthY, direction, 2);
    sentrySet(self, S_YY, RValue_makeReal(component + RValue_toReal(sentryGet(self, S_OFFY))));
    if (boss) {
        RValue object = RValue_makeInt32(30);
        if (sentryCall(ctx, exists, &object, 1) != 0) {
            Instance **players = runner->instancesByObject[30];
            for (int i = 0; i < arrlen(players); ++i)
                if (players[i]->active) sentrySet(players[i], S_BATTLE, RValue_makeReal(0));
            Instance **statues = runner->instancesByObject[185];
            int active = 0, count = (int)arrlen(statues);
            for (int i = 0; i < count; ++i) active += statues[i]->active;
            /* Match with(object)'s forward order for one/two active instances
             * and reverse order for larger buckets. No callbacks mutate it. */
            int step = active > 2 ? -1 : 1;
            for (int i = step < 0 ? count - 1 : 0; i >= 0 && i < count; i += step) {
                Instance *statue = statues[i];
                if (!statue->active) continue;
                GMLReal x = (GMLReal)statue->x, y = (GMLReal)statue->y;
                GMLReal xr = RValue_toReal(sentryGet(statue, S_XRANGE));
                GMLReal yr = RValue_toReal(sentryGet(statue, S_YRANGE));
                if (lessThan(x - xr, (GMLReal)player->x, epsilon) &&
                    lessThan((GMLReal)player->x, x + xr, epsilon) &&
                    lessThan(y - yr, (GMLReal)player->y, epsilon) &&
                    lessThan((GMLReal)player->y, y + yr, epsilon)) {
                    for (int j = 0; j < arrlen(players); ++j)
                        if (players[j]->active) sentrySet(players[j], S_BATTLE, RValue_makeReal(1));
                }
            }
        }
    }
    GMLReal speed = RValue_toReal(sentryGet(self, S_SPEED));
    if (lessThan(0, speed, epsilon)) sentrySet(self, S_SINER, sentryAdd(sentryGet(self, S_SINER), RValue_makeInt32(1)));
    if (lessThan(speed, 0, epsilon)) sentrySet(self, S_SINER, sentryAdd(sentryGet(self, S_SINER), RValue_makeInt32(-1)));
    /* Pop.v.i normalizes the integer literal to REAL before the loop. */
    sentrySet(self, S_I, RValue_makeReal(0));
    for (int i = 0; lessThan((GMLReal)i, maximum, epsilon); ++i) {
        RValue id = GMLArray_get(bullets.array, i);
        if (sentryCall(ctx, exists, &id, 1) != 0) {
            Instance *bullet = VM_findInstanceByTarget(ctx, RValue_toInt32(id));
            RValue phase = RValue_makeReal(RValue_toReal(sentryGet(self, S_SINER)) / (GMLReal)10);
            GMLReal wave = sentryCall(ctx, sine, &phase, 1);
            direction[0] = RValue_makeReal(((GMLReal)2 + (GMLReal)i * (GMLReal)2) * RValue_toReal(sentryGet(self, S_EYE)));
            direction[1] = RValue_makeReal(RValue_toReal(rot) - wave * (GMLReal)(i * 2));
            component = sentryCall(ctx, lengthX, direction, 2);
            sentrySet(self, S_XA, RValue_makeReal(component + RValue_toReal(sentryGet(self, S_OFFX))));
            wave = sentryCall(ctx, sine, &phase, 1);
            direction[1] = RValue_makeReal(RValue_toReal(rot) - wave * (GMLReal)(i * 2));
            component = sentryCall(ctx, lengthY, direction, 2);
            sentrySet(self, S_YA, RValue_makeReal(component + RValue_toReal(sentryGet(self, S_OFFY))));
            sentryProperty(ctx, bullet, BUILTIN_VAR_X, "x", 133641, (GMLReal)self->x + RValue_toReal(sentryGet(self, S_XA)));
            sentryProperty(ctx, bullet, BUILTIN_VAR_Y, "y", 133642, (GMLReal)self->y + RValue_toReal(sentryGet(self, S_YA)));
            ctx->currentArrayOwner = (void *)(intptr_t)133678;
            phase = RValue_makeReal(((GMLReal)i * (GMLReal)4 + RValue_toReal(sentryGet(self, S_SINER))) / (GMLReal)5);
            wave = sentryCall(ctx, sine, &phase, 1);
            sentryProperty(ctx, bullet, BUILTIN_VAR_IMAGE_ALPHA, "image_alpha", 133678,
                           (GMLReal)0.8 + wave * (GMLReal)0.2);
            if (lessThan(RValue_toReal(sentryGet(self, S_CON)), 1, epsilon))
                VMBuiltins_setVariable(ctx, bullet, BUILTIN_VAR_IMAGE_ALPHA, "image_alpha",
                                      RValue_makeReal((GMLReal)0 + RValue_toReal(sentryGet(self, S_EYE)) / (GMLReal)10), -1);
            ctx->currentArrayOwner = (void *)(intptr_t)133677;
            phase = RValue_makeReal(RValue_toReal(sentryGet(self, S_SINER)) / (GMLReal)10);
            wave = sentryCall(ctx, sine, &phase, 1);
            sentryProperty(ctx, bullet, BUILTIN_VAR_IMAGE_ANGLE, "image_angle", 133677,
                           RValue_toReal(rot) - wave * (GMLReal)(i * 4));
        }
        sentrySet(self, S_I, RValue_makeReal((GMLReal)i + 1));
    }
    return true;
}
