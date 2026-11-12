/* Undertale's room 57 snow patch (obj_snowfloor's Draw event), written out in C.
 * Included by game_undertale.c, which declares snowVarsResolved (forgotten with
 * the other natives' state when the VM changes) and lists snowfloorDraw in its
 * table of natives. */

/* obj_snowfloor's Draw event. Room 57 (room_tundra_snowpuzz, the one with
 * the "Dog Marriage" sign) has 16 of these, a 5x5 grid of snow dots each,
 * and the event runs about 2,600 instructions an instance to visit all 25:
 * draw the dot (draw_circle, radius 2.8), test it against the player
 * (collision_circle, radius 2), and while it is being pushed, move it away
 * from the player's box with a random wobble. That is some 42,000
 * instructions a frame whether or not the patch is in view. This is the
 * same event in C: the same built-in calls in the same order (so the random
 * numbers and the drawing are the same), the same comparisons with the
 * runner's epsilon, the same float arithmetic, and the same values and
 * stack types left in the arrays and in xx and yy. Anything it does not
 * expect (no player, an array a write would have to copy, a cell that is
 * not a real) is checked before the first change, and the bytecode runs
 * instead. */
enum { SNOW_X, SNOW_Y, SNOW_MOVE, SNOW_DRAW, SNOW_ARRAY_COUNT };
enum { SNOW_XX = SNOW_ARRAY_COUNT, SNOW_YY, SNOW_MOVING, SNOW_FLAG, SNOW_VAR_COUNT };
enum { SNOW_PLAYER_OBJECT = 1576, SNOW_ROOM = 57, SNOW_FLAG_INDEX = 64, SNOW_GRID = 5 };
static int32_t snowVars[SNOW_VAR_COUNT];
static bool snowPlayerNamed; /* object 1576 is obj_mainchara in this data.win */
static BuiltinFunc snowSetColor, snowDrawCircle, snowCollide, snowRandom, snowFloor;

typedef struct {
    RValue *slot;
    GMLArray *array;
} SnowArray;

/* The variable's slot and array, if a write to an existing cell would go
 * straight into it: VM_arraySetWithCoW copies an array that is shared with
 * a slot other than its owner (WAD 16 rules); that is left to the bytecode. */
static bool snowArray(Instance *instance, int32_t id, SnowArray *out) {
    RValue *slot = IntRValueHashMap_findSlot(&instance->selfVars, id);
    if (slot == NULL || slot->type != RVALUE_ARRAY || slot->array == NULL || slot->array->type != GML_LEGACY_ARRAY) return false;
    if (slot->array->refCount > 1 && slot->array->owner != (void *) slot) return false;
    out->slot = slot;
    out->array = slot->array;
    return true;
}

/* The store VM_arraySetWithCoW makes into an existing cell of an array it
 * need not copy: claim the array for this slot if no slot owns it, then
 * replace the value. */
static void snowStore(SnowArray *array, RValue *cell, GMLReal number, uint8_t stackType) {
    if (array->array->owner == NULL) array->array->owner = array->slot;
    RValue value = RValue_makeReal(number);
    value.gmlStackType = stackType;
    RValue_writeIntoSlotStealingOwnershipOrCopying(cell, value);
}

static bool snowNumber(RValue value) {
    return value.type == RVALUE_REAL || value.type == RVALUE_INT32 || value.type == RVALUE_BOOL;
}

/* OP_CMP on two numbers: equal within epsilon, otherwise by the sign of the
 * difference (a NaN difference counts as greater, as in the VM). */
static int snowCompare(GMLReal a, GMLReal b, GMLReal epsilon) {
    GMLReal diff = a - b;
    return GMLReal_fabs(diff) <= epsilon ? 0 : (diff < 0 ? -1 : 1);
}

/* A built-in called as OP_CALL calls it, profiler entry included. */
static RValue snowCall(VMContext *ctx, BuiltinFunc builtin, const char *name, RValue *args, int32_t argCount) {
#ifdef ENABLE_VM_GML_PROFILER
    if (ctx->profiler != NULL) Profiler_enter(ctx->profiler, name);
#else
    (void) name;
#endif
    RValue result = builtin(ctx, args, argCount);
#ifdef ENABLE_VM_GML_PROFILER
    if (ctx->profiler != NULL) Profiler_exit(ctx->profiler);
#endif
    return result;
}

/* obj_mainchara.bbox_*, read as OP_PUSH.v reads a built-in on an object. */
static GMLReal snowPlayerBox(VMContext *ctx, Instance *player, int16_t id, const char *name) {
    RValue value = VMBuiltins_getVariable(ctx, player, id, name, 0);
    GMLReal number = RValue_toReal(value);
    RValue_free(&value);
    return number;
}

/* The instances collision_circle(x, y, 2, obj_mainchara, false, true) can
 * find from self: the active ones of obj_mainchara or a child, not self.
 * None of them moves while the event runs, so each one's box is worked out
 * once, widened by 4: the built-in finds an instance only if its box comes
 * within the circle's radius of the centre, and with compatibility mode's
 * rounding the centre moves by at most half a pixel each way and the
 * radius grows to at most 2.5, so a centre outside the widened box (a pixel
 * to spare for rounding) cannot hit. A shape that needs the rotated test, or
 * too many instances, leaves every dot to the built-in. */
enum { SNOW_MAX_PLAYERS = 4 };
typedef struct {
    GMLReal left, top, right, bottom; /* the box, widened by 4 */
    InstanceBBox box;                 /* the box itself */
} SnowReach;

static int snowPlayers(Runner *runner, Instance *self, SnowReach *out) {
    if (runner->instancesByObject == NULL) return -1;
    Instance **bucket = runner->instancesByObject[SNOW_PLAYER_OBJECT];
    int count = 0;
    for (ptrdiff_t i = 0; i < arrlen(bucket); i++) {
        Instance *inst = bucket[i];
        if (!inst->active || inst == self) continue;
        if (count == SNOW_MAX_PLAYERS) return -1;
        if (Collision_obbNeedsSAT(Collision_getSprite(runner->dataWin, inst), inst)) return -1;
        InstanceBBox box = Collision_computeBBox(runner, inst);
        if (!box.valid) continue; /* Collision_circleOverlapsInstance: never */
        if (!isfinite(box.left) || !isfinite(box.top) || !isfinite(box.right) || !isfinite(box.bottom)) return -1;
        out[count].box = box;
        out[count].left = box.left - (GMLReal) 4;
        out[count].top = box.top - (GMLReal) 4;
        out[count].right = box.right + (GMLReal) 4;
        out[count].bottom = box.bottom + (GMLReal) 4;
        count++;
    }
    return count;
}

static bool snowMayTouch(Runner *runner, const SnowReach *players, int count, GMLReal cx, GMLReal cy) {
    if (count < 0 || !isfinite(cx) || !isfinite(cy)) return true;
    for (int i = 0; i < count; i++) {
        if (!(cx >= players[i].left && cx <= players[i].right && cy >= players[i].top && cy <= players[i].bottom)) continue;
        /* Inside the widened box: Collision_circleOverlapsInstance's closest-point test with the
         * centre and radius the built-in works out, half a pixel to spare for rounding. */
        GMLReal x = cx, y = cy, radius = (GMLReal) 2;
        if (runner->collisionCompatibilityMode) {
            GMLReal qx1r = GMLReal_bankersRound(x - radius), qy1r = GMLReal_bankersRound(y - radius);
            GMLReal qx2r = GMLReal_bankersRound(x + radius), qy2r = GMLReal_bankersRound(y + radius);
            x = (GMLReal) ((double) (qx1r + qx2r) * 0.5);
            y = (GMLReal) ((double) (qy1r + qy2r) * 0.5);
            GMLReal rx = (GMLReal) ((double) (qx2r - qx1r) * 0.5), ry = (GMLReal) ((double) (qy2r - qy1r) * 0.5);
            radius = rx < ry ? rx : ry;
        }
        const InstanceBBox *box = &players[i].box;
        GMLReal reach = radius + (GMLReal) 0.5;
        GMLReal closestX = x, closestY = y;
        if (box->left > closestX) closestX = box->left;
        if (closestX > box->right) closestX = box->right;
        if (box->top > closestY) closestY = box->top;
        if (closestY > box->bottom) closestY = box->bottom;
        GMLReal dx = closestX - x, dy = closestY - y;
        if (!(dx * dx + dy * dy > reach * reach)) return true;
    }
    return false;
}

/* builtin_random(n), as it works it out: the next number of the runner's
 * generator over BS_RAND_MAX, times n. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static GMLReal snowRoll(Runner *runner, GMLReal n) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    return ((GMLReal) Random_nextUInt32(&runner->random) / (GMLReal) BS_RAND_MAX) * n;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static bool snowfloorDraw(VMContext *ctx) {
#ifdef __clang__
#pragma clang fp contract(off)
#endif
    Runner *runner = ctx->runner;
    Instance *self = ctx->currentCallFrame->self;
    Instance *globals = ctx->globalScopeInstance;
    if (runner == NULL || self == NULL || globals == NULL || self->objectIndex == STRUCT_OBJECT_INDEX) return false;
    if (!snowVarsResolved) {
        snowPlayerNamed = ctx->dataWin->objt.count > SNOW_PLAYER_OBJECT &&
                          strcmp(ctx->dataWin->objt.objects[SNOW_PLAYER_OBJECT].name, "obj_mainchara") == 0;
        snowSetColor = VM_findBuiltin(ctx, "draw_set_color");
        snowDrawCircle = VM_findBuiltin(ctx, "draw_circle");
        snowCollide = VM_findBuiltin(ctx, "collision_circle");
        snowRandom = VM_findBuiltin(ctx, "random");
        snowFloor = VM_findBuiltin(ctx, "floor");
        /* The ids the event's own instructions use: xx and yy are also the
         * names of many scripts' locals, so look them up by scope in VARI
         * rather than by name alone. */
        static const char *const names[SNOW_VAR_COUNT] = { "snowx", "snowy", "moveme", "dodraw", "xx", "yy", "moving", "flag" };
        for (int i = 0; i < SNOW_VAR_COUNT; i++) {
            snowVars[i] = -1;
            int32_t scope = i == SNOW_FLAG ? INSTANCE_GLOBAL : INSTANCE_SELF;
            for (uint32_t v = 0; v < ctx->dataWin->vari.variableCount; v++) {
                const Variable *variable = &ctx->dataWin->vari.variables[v];
                if (variable->instanceType == scope && variable->varID >= 0 && variable->name != NULL &&
                    strcmp(variable->name, names[i]) == 0) {
                    snowVars[i] = variable->varID;
                    break;
                }
            }
        }
        snowVarsResolved = true;
    }
    for (int i = 0; i < SNOW_VAR_COUNT; i++)
        if (snowVars[i] < 0) return false;
    BuiltinFunc setColor = snowSetColor, drawCircle = snowDrawCircle, collide = snowCollide, randomOf = snowRandom, floorOf = snowFloor;
    if (!snowPlayerNamed || setColor == NULL || drawCircle == NULL || collide == NULL || randomOf == NULL || floorOf == NULL) return false;

    /* Everything the event reads that could make it act differently from
     * this, checked before anything changes. The xx and yy slots must exist
     * already, so that writing them cannot move the arrays' slots. */
    Instance *player = VM_findInstanceByTarget(ctx, SNOW_PLAYER_OBJECT);
    if (player == NULL || !snowNumber(Instance_getSelfVar(player, snowVars[SNOW_MOVING])) ||
        !IntRValueHashMap_contains(&self->selfVars, snowVars[SNOW_XX]) ||
        !IntRValueHashMap_contains(&self->selfVars, snowVars[SNOW_YY])) return false;
    SnowArray arrays[SNOW_ARRAY_COUNT], flags;
    RValue *rows[SNOW_ARRAY_COUNT][SNOW_GRID]; /* snowx[xx, 0..4] and so on: row xx of each array */
    for (int a = 0; a < SNOW_ARRAY_COUNT; a++) {
        if (!snowArray(self, snowVars[a], &arrays[a])) return false;
        for (int32_t xx = 0; xx < SNOW_GRID; xx++) {
            for (int32_t yy = 0; yy < SNOW_GRID; yy++) {
                RValue *cell = GMLArray_slot(arrays[a].array, xx * GML_LEGACY_ARRAY_STRIDE + yy);
                if (cell == NULL || cell->type != RVALUE_REAL) return false;
            }
            /* The cells exist, and no write in the event adds one, so a row stays where it is. */
            rows[a][xx] = GMLArray_slot(arrays[a].array, xx * GML_LEGACY_ARRAY_STRIDE);
        }
        for (int b = 0; b < a; b++)
            if (arrays[b].array == arrays[a].array) return false;
    }
    if (!snowArray(globals, snowVars[SNOW_FLAG], &flags)) return false;
    RValue *flagCell = GMLArray_slot(flags.array, SNOW_FLAG_INDEX);
    if (flagCell == NULL || !snowNumber(*flagCell)) return false;
    for (int a = 0; a < SNOW_ARRAY_COUNT; a++)
        if (arrays[a].array == flags.array) return false;

    /* The benchmark's "natives" table: this event's time, on the frames it times in detail. */
    uint64_t started = ofPerfDetail ? nowNanos() : 0;
    uint32_t tests = 0, skipped = 0;
    uint64_t collideNanos = 0; /* in collision_circle, on the same frames */
#ifdef ENABLE_VM_GML_PROFILER
    bool profiled = ctx->profiler != NULL;
#else
    bool profiled = false;
#endif
    SnowReach players[SNOW_MAX_PLAYERS];
    int playerCount = snowPlayers(runner, self, players);
    GMLReal box[4]; /* obj_mainchara.bbox_left, right, top, bottom: read once, when a dot first needs them */
    bool boxRead = false;
    bool drewOne = false; /* a circle has been drawn this event, settling what was held */
    GMLReal epsilon = runner->mathEpsilon;
    bool inRoom = snowCompare((GMLReal) runner->currentRoomIndex, (GMLReal) SNOW_ROOM, epsilon) == 0;
    RValue white = RValue_makeInt32(16777215);
    RValue ignored = snowCall(ctx, setColor, "draw_set_color", &white, 1);
    RValue_free(&ignored);

    /* The event visits the dots in turn, each drawn and then moved. A dot's
     * move touches only its own cells and draws nothing, and the draws use
     * no random numbers, so drawing every dot first and then moving every
     * dot does the same, in the same order for the random numbers. Two
     * passes let the benchmark time the drawing on its own. */
    uint64_t drawStart = started != 0 ? nowNanos() : 0;
    for (int32_t yy = 0; yy < SNOW_GRID; yy++) {
        for (int32_t xx = 0; xx < SNOW_GRID; xx++) {
            RValue *snowX = &rows[SNOW_X][xx][yy], *snowY = &rows[SNOW_Y][xx][yy];
            if (snowCompare(rows[SNOW_DRAW][xx][yy].real, (GMLReal) 1, epsilon) == 0) {
                if (profiled) {
                    RValue args[4] = { *snowX, *snowY, RValue_makeReal((GMLReal) 2.8), RValue_makeInt32(0) };
                    args[0].ownsReference = args[1].ownsReference = false;
                    RValue drawn = snowCall(ctx, drawCircle, "draw_circle", args, 4);
                    RValue_free(&drawn);
                } else if (runner->renderer != NULL) {
                    /* What builtin_draw_circle does with these arguments. */
                    float x = (float) snowX->real, y = (float) snowY->real, radius = (float) (GMLReal) 2.8;
                    if (runner->applyOffsetForPrimitives) {
                        x += 1.0f;
                        y += 1.0f;
                    }
                    /* The event's first circle settles held layers; nothing drawn since holds anything
                     * new, so the others can skip that (and need nothing at all off the target). */
                    if (!drewOne) {
                        Renderer_drawCircle(runner->renderer, x, y, radius, false);
                        drewOne = true;
                    } else if (!SWRenderer_drawFilledCircleSettled(runner->renderer, x, y, radius)) {
                        Renderer_drawCircle(runner->renderer, x, y, radius, false);
                    }
                }
            }
        }
    }
    if (drawStart != 0) ofPerfAddLoad(OF_LOAD_NATIVE_DRAW, nowNanos() - drawStart);

    for (int32_t yy = 0; yy < SNOW_GRID; yy++) {
        for (int32_t xx = 0; xx < SNOW_GRID; xx++) {
            RValue *snowX = &rows[SNOW_X][xx][yy], *snowY = &rows[SNOW_Y][xx][yy];
            RValue *move = &rows[SNOW_MOVE][xx][yy];
            /* collision_circle can only answer noone when no player instance
             * overlaps the circle, so it is called only when one might. */
            bool touched = false;
            tests++;
            if (snowMayTouch(runner, players, playerCount, snowX->real, snowY->real)) {
                RValue args[6] = { *snowX, *snowY, RValue_makeInt32(2), RValue_makeInt32(SNOW_PLAYER_OBJECT),
                                   RValue_makeInt32(0), RValue_makeInt32(1) };
                args[0].ownsReference = args[1].ownsReference = false;
                uint64_t calledAt = started != 0 ? nowNanos() : 0;
                RValue hit = snowCall(ctx, collide, "collision_circle", args, 6);
                if (calledAt != 0) collideNanos += nowNanos() - calledAt;
                /* hit != noone; an undefined result is unequal to any number. */
                touched = !snowNumber(hit) || snowCompare(RValue_toReal(hit), (GMLReal) -4, epsilon) != 0;
                RValue_free(&hit);
            } else {
                skipped++;
            }
            if (touched) {
                GMLReal whole;
                if (profiled) {
                    RValue four = RValue_makeInt32(4);
                    RValue roll = snowCall(ctx, randomOf, "random", &four, 1);
                    RValue floored = snowCall(ctx, floorOf, "floor", &roll, 1);
                    whole = RValue_toReal(floored);
                    RValue_free(&roll);
                    RValue_free(&floored);
                } else {
                    whole = GMLReal_floor(snowRoll(runner, (GMLReal) 4));
                }
                snowStore(&arrays[SNOW_MOVE], move, whole + (GMLReal) 2, GML_TYPE_VARIABLE);
            }
            if (snowCompare(move->real, (GMLReal) 1, epsilon) <= 0) continue;

            if (snowCompare(RValue_toReal(Instance_getSelfVar(player, snowVars[SNOW_MOVING])), (GMLReal) 1, epsilon) == 0) {
                if (inRoom && snowCompare(RValue_toReal(*flagCell), (GMLReal) 0, epsilon) == 0)
                    snowStore(&flags, flagCell, (GMLReal) -1, GML_TYPE_DOUBLE);
                if (!boxRead) {
                    /* Reading bbox_* only works the box out; the player does not move in this event. */
                    box[0] = snowPlayerBox(ctx, player, BUILTIN_VAR_BBOX_LEFT, "bbox_left");
                    box[1] = snowPlayerBox(ctx, player, BUILTIN_VAR_BBOX_RIGHT, "bbox_right");
                    box[2] = snowPlayerBox(ctx, player, BUILTIN_VAR_BBOX_TOP, "bbox_top");
                    box[3] = snowPlayerBox(ctx, player, BUILTIN_VAR_BBOX_BOTTOM, "bbox_bottom");
                    boxRead = true;
                }
                if (snowCompare(box[0], snowX->real, epsilon) > 0)
                    snowStore(&arrays[SNOW_X], snowX, snowX->real - move->real, GML_TYPE_VARIABLE);
                if (snowCompare(box[1], snowX->real, epsilon) < 0)
                    snowStore(&arrays[SNOW_X], snowX, snowX->real + move->real, GML_TYPE_VARIABLE);
                if (snowCompare(box[2], snowY->real, epsilon) > 0)
                    snowStore(&arrays[SNOW_Y], snowY, snowY->real - move->real, GML_TYPE_VARIABLE);
                if (snowCompare(box[3], snowY->real, epsilon) < 0)
                    snowStore(&arrays[SNOW_Y], snowY, snowY->real + move->real, GML_TYPE_VARIABLE);
                /* snowx += (random(moveme) - moveme / 2) / 2, then the same for snowy. */
                for (int axis = SNOW_X; axis <= SNOW_Y; axis++) {
                    RValue *cell = axis == SNOW_X ? snowX : snowY;
                    GMLReal start = cell->real;
                    GMLReal rolled;
                    if (profiled) {
                        RValue span = *move;
                        span.ownsReference = false;
                        RValue wobble = snowCall(ctx, randomOf, "random", &span, 1);
                        rolled = RValue_toReal(wobble);
                        RValue_free(&wobble);
                    } else {
                        rolled = snowRoll(runner, move->real);
                    }
                    GMLReal half = move->real / (GMLReal) 2;
                    GMLReal offset = (rolled - half) / (GMLReal) 2;
                    snowStore(&arrays[axis], cell, start + offset, GML_TYPE_VARIABLE);
                }
            }
            snowStore(&arrays[SNOW_MOVE], move, move->real - (GMLReal) 1, GML_TYPE_VARIABLE);
        }
    }

    ofPerfAddLoad(OF_LOAD_COLLIDE_TESTS, tests);
    ofPerfAddLoad(OF_LOAD_COLLIDE_SKIPPED, skipped);
    if (started != 0) {
        ofPerfAddLoad(OF_LOAD_NATIVE, nowNanos() - started);
        ofPerfAddLoad(OF_LOAD_NATIVE_COLLIDE, collideNanos);
    }
    /* The loop leaves xx = 0 and yy = 5, each last stored from an ADD.i.v. */
    RValue xxValue = RValue_makeReal((GMLReal) 0), yyValue = RValue_makeReal((GMLReal) SNOW_GRID);
    xxValue.gmlStackType = yyValue.gmlStackType = GML_TYPE_VARIABLE;
    Instance_setSelfVar(self, snowVars[SNOW_XX], xxValue);
    Instance_setSelfVar(self, snowVars[SNOW_YY], yyValue);
    return true;
}
