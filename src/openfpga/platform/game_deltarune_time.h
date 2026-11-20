/* obj_time's Begin Step, written out in C. It runs in every room of the
 * game, some 750 operations a frame plus four calls of
 * scr_gamepad_axis_check: the game's input arrays, rebuilt each frame.
 * Included by game_deltarune.c after the scissor dancers, whose dancerReal,
 * dancerCompare and dancerArith it shares.
 *
 *   if (paused) exit                              (and the console pause)
 *   global.time += 1
 *   if (keyboard_check(vk_escape)) { ...quit timer... } else quit_timer -= 2
 *   if (keyboard_check_pressed(vk_f4)) ...        fullscreen_toggle, window_center_toggle
 *   for (i = 0; i < 10; i += 1) { global.input_released[i] = 0; global.input_pressed[i] = 0 }
 *   if (obj_gamecontroller.gamepad_active == 1) {
 *       for j 0..3: down = keyboard_check(input_k[j]) || gamepad_button_check(pad, input_g[j])
 *                          || scr_gamepad_axis_check(pad, j)
 *       for k 4..9: down = keyboard_check(input_k[k]) || gamepad_button_check(pad, input_g[k])
 *   } else for l 0..9: down = keyboard_check(input_k[l])
 *       each: if (down) { if (input_held == 0) input_pressed = 1; input_held = 1 }
 *             else      { if (input_held == 1) input_released = 1; input_held = 0 }
 *   ...border fades...
 *   if (scr_debug()) { ...room_speed keys... }
 *
 * Only the usual frame is written out: paused, the console, Escape held, F4,
 * a pending window centring, a border fade and debug mode are all left to the
 * bytecode, decided before anything is changed, as is any variable that does
 * not hold what the game puts there. The array writes are the VM's own
 * (VM_arraySet, under the setowner token the bytecode gives each), in the
 * bytecode's order, and every read of an array goes through its live slot, so
 * copy-on-write forks come out the same. The two scripts it calls are checked
 * against the bytecode they were written from. */

#define TIME_OWNER_RELEASED 231756
#define TIME_OWNER_PRESSED 231697
#define TIME_OWNER_HELD 231700

enum {
    TM_PAUSED, TM_TIME, TM_CONSOLE, TM_QUIT, TM_FULLSCREEN, TM_CENTER, TM_I, TM_FADE_OUT, TM_FADE_IN,
    TM_DEBUG, TM_AXIS_VALUE, TM_RETURN, TM_ACTIVE, TM_PAD, TM_RELEASED, TM_PRESSED, TM_HELD, TM_KEYS,
    TM_BUTTONS, TM_COUNT
};

static int timeResolved; /* 0 not yet, 1 ready, -1 not this game's bytecode */
static int32_t timeVars[TM_COUNT];
static int32_t timeObject, timeController;
static BuiltinFunc timeKeyboard, timeKeyboardPressed, timeButton, timeAxis;

static void timeStepResolve(VMContext *ctx) {
    if (timeResolved != 0) return;
    timeResolved = -1;
    static const char *const scripts[] = { "gml_Script_scr_gamepad_axis_check", "gml_Script_scr_debug" };
    static const uint32_t lengths[] = { 452, 68 };
    static const uint64_t hashes[] = { 0xd96c74c25570b529ull, 0x847afa0367d28889ull };
    for (int i = 0; i < 2; ++i) {
        uint32_t length = 0;
        if (VM_codeHash(ctx, scripts[i], &length) != hashes[i] || length != lengths[i]) return;
    }
    static const char *const names[TM_COUNT] = {
        "paused", "time", "is_console", "quit_timer", "fullscreen_toggle", "window_center_toggle", "i",
        "border_fade_out", "border_fade_in", "debug", "axis_value", "__returnvalue", "gamepad_active",
        "gamepad_id", "input_released", "input_pressed", "input_held", "input_k", "input_g"
    };
    for (int i = 0; i < TM_COUNT; ++i) timeVars[i] = VM_getOrAllocateVarID(ctx, names[i]);
    timeKeyboard = VM_findBuiltin(ctx, "keyboard_check");
    timeKeyboardPressed = VM_findBuiltin(ctx, "keyboard_check_pressed");
    timeButton = VM_findBuiltin(ctx, "gamepad_button_check");
    timeAxis = VM_findBuiltin(ctx, "gamepad_axis_value");
    if (timeKeyboard == NULL || timeKeyboardPressed == NULL || timeButton == NULL || timeAxis == NULL) return;
    /* scr_gamepad_axis_check asks instance_exists(328) for obj_time. */
    DataWin *dw = ctx->runner->dataWin;
    timeObject = timeController = -1;
    for (uint32_t i = 0; i < dw->objt.count; ++i) {
        const char *name = dw->objt.objects[i].name;
        if (name == NULL) continue;
        if (strcmp(name, "obj_time") == 0) timeObject = (int32_t) i;
        else if (strcmp(name, "obj_gamecontroller") == 0) timeController = (int32_t) i;
    }
    if (timeObject != 328 || timeController < 0) return;
    timeResolved = 1;
}

/* Something CONV.v.b reads as false. */
static bool timeFalse(RValue value) {
    return (isNumber(value) || value.type == RVALUE_BOOL) && !RValue_toBool(value);
}

static bool timeCall(VMContext *ctx, BuiltinFunc fn, RValue *args, int32_t count) {
    RValue result = fn(ctx, args, count);
    bool truth = RValue_toBool(result);
    RValue_free(&result);
    return truth;
}

static RValue timeElement(Instance *globals, int field, int32_t index) {
    RValue array = Instance_getSelfVar(globals, timeVars[field]);
    return GMLArray_getOnArrayRef(&array, index);
}

/* An array of at least ten, and when asked, ten numbers. */
static bool timeArrayReady(Instance *globals, int field, bool numbers) {
    RValue array = Instance_getSelfVar(globals, timeVars[field]);
    if (array.type != RVALUE_ARRAY || array.array == NULL || GMLArray_length1D(array.array) < 10) return false;
    if (numbers)
        for (int32_t i = 0; i < 10; ++i)
            if (!isNumber(GMLArray_get(array.array, i))) return false;
    return true;
}

/* setowner <token>; global.<field>[index] = value */
static void timeArraySet(VMContext *ctx, Instance *globals, int field, int32_t token, int32_t index, int32_t value) {
    ctx->currentArrayOwner = (void *) (intptr_t) token;
    RValue *slot = IntRValueHashMap_getOrInsertUndefined(&globals->selfVars, timeVars[field]);
    RValue element = RValue_makeInt32(value);
    element.gmlStackType = GML_TYPE_VARIABLE; /* pushed, then CONV.i.v */
    VM_arraySet(ctx, slot, index, element);
}

static void timeInput(VMContext *ctx, Instance *globals, int32_t index, bool down) {
    GMLReal held = dancerReal(timeElement(globals, TM_HELD, index));
    if (down) {
        if (dancerCompare(ctx, held, 0) == 0) timeArraySet(ctx, globals, TM_PRESSED, TIME_OWNER_PRESSED, index, 1);
        timeArraySet(ctx, globals, TM_HELD, TIME_OWNER_HELD, index, 1);
    } else {
        if (dancerCompare(ctx, held, 1) == 0) timeArraySet(ctx, globals, TM_RELEASED, TIME_OWNER_RELEASED, index, 1);
        timeArraySet(ctx, globals, TM_HELD, TIME_OWNER_HELD, index, 0);
    }
}

/* scr_gamepad_axis_check(pad, direction) with self = obj_time, which is
 * also the first obj_time (checked before), so its axis_value = obj_time.axis_value
 * stores the same value back, retagged as a pushed variable. */
static bool timeAxisCheck(VMContext *ctx, Instance *self, RValue pad, int32_t direction) {
    RValue axisValue = Instance_getSelfVar(self, timeVars[TM_AXIS_VALUE]);
    axisValue.gmlStackType = GML_TYPE_VARIABLE;
    Instance_setSelfVar(self, timeVars[TM_AXIS_VALUE], axisValue);
    Instance_setSelfVar(self, timeVars[TM_RETURN], RValue_makeReal((GMLReal) 0));
    GMLReal threshold = dancerReal(axisValue);
    RValue args[2] = { pad, RValue_makeInt32(direction == 0 || direction == 2 ? 32786 : 32785) };
    RValue axis = timeAxis(ctx, args, 2);
    GMLReal value = RValue_toReal(axis);
    RValue_free(&axis);
    bool hit = direction < 2 ? dancerCompare(ctx, value, threshold) >= 0 : dancerCompare(ctx, value, -threshold) <= 0;
    if (hit) Instance_setSelfVar(self, timeVars[TM_RETURN], RValue_makeReal((GMLReal) 1));
    return hit;
}

static bool timeStep(VMContext *ctx) {
    Instance *self = ctx->currentCallFrame->self;
    if (self == NULL || self->objectIndex == STRUCT_OBJECT_INDEX || ctx->runner == NULL) return false;
    timeStepResolve(ctx);
    Instance *globals = ctx->globalScopeInstance;
    if (timeResolved != 1 || globals == NULL || self->objectIndex != timeObject) return false;

    /* Everything decided before anything is changed. */
    if (!timeFalse(Instance_getSelfVar(self, timeVars[TM_PAUSED])) ||
        !timeFalse(Instance_getSelfVar(globals, timeVars[TM_CONSOLE])) ||
        !timeFalse(Instance_getSelfVar(self, timeVars[TM_FADE_OUT])) ||
        !timeFalse(Instance_getSelfVar(self, timeVars[TM_FADE_IN])) ||
        !timeFalse(Instance_getSelfVar(globals, timeVars[TM_DEBUG]))) return false;
    RValue time = Instance_getSelfVar(globals, timeVars[TM_TIME]), quit = Instance_getSelfVar(self, timeVars[TM_QUIT]);
    RValue full = Instance_getSelfVar(self, timeVars[TM_FULLSCREEN]), center = Instance_getSelfVar(self, timeVars[TM_CENTER]);
    if (!isNumber(time) || !isNumber(quit) || !isNumber(full) || !isNumber(center)) return false;
    if (dancerCompare(ctx, dancerReal(full), 1) == 0 || dancerCompare(ctx, dancerReal(center), 1) == 0 ||
        dancerCompare(ctx, dancerReal(center), 2) == 0) return false;
    Instance *controller = VM_findInstanceByTarget(ctx, timeController);
    if (controller == NULL || VM_findInstanceByTarget(ctx, timeObject) != self) return false;
    RValue active = Instance_getSelfVar(controller, timeVars[TM_ACTIVE]);
    if (!isNumber(active)) return false;
    bool gamepad = dancerCompare(ctx, dancerReal(active), 1) == 0;
    if (gamepad && (!isNumber(Instance_getSelfVar(controller, timeVars[TM_PAD])) ||
                    !isNumber(Instance_getSelfVar(self, timeVars[TM_AXIS_VALUE])) ||
                    !timeArrayReady(globals, TM_BUTTONS, false))) return false;
    if (!timeArrayReady(globals, TM_KEYS, false) || !timeArrayReady(globals, TM_HELD, true)) return false;
    RValue key = RValue_makeInt32(27);
    if (timeCall(ctx, timeKeyboard, &key, 1)) return false;
    key = RValue_makeInt32(115);
    if (timeCall(ctx, timeKeyboardPressed, &key, 1)) return false;

    /* global.time += 1; quit_timer -= 2 */
    Instance_setSelfVar(globals, timeVars[TM_TIME], dancerArith('+', time, RValue_makeInt32(1)));
    Instance_setSelfVar(self, timeVars[TM_QUIT], dancerArith('-', quit, RValue_makeInt32(2)));

    /* for (i = 0; i < 10; i += 1) released and pressed cleared; i ends a real 10. */
    for (int32_t i = 0; i < 10; ++i) {
        timeArraySet(ctx, globals, TM_RELEASED, TIME_OWNER_RELEASED, i, 0);
        timeArraySet(ctx, globals, TM_PRESSED, TIME_OWNER_PRESSED, i, 0);
    }
    RValue ten = RValue_makeReal((GMLReal) 10);
    ten.gmlStackType = GML_TYPE_VARIABLE; /* i += 1 was an ADD.i.v */
    Instance_setSelfVar(self, timeVars[TM_I], ten);

    for (int32_t n = 0; n < 10; ++n) {
        RValue arg = timeElement(globals, TM_KEYS, n);
        bool down = timeCall(ctx, timeKeyboard, &arg, 1);
        if (gamepad && !down) {
            RValue pad = Instance_getSelfVar(controller, timeVars[TM_PAD]);
            RValue args[2] = { pad, timeElement(globals, TM_BUTTONS, n) };
            down = timeCall(ctx, timeButton, args, 2);
            if (!down && n < 4) down = timeAxisCheck(ctx, self, pad, n);
        }
        timeInput(ctx, globals, n, down);
    }
    return true;
}
