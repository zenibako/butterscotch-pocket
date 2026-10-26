/* Real asset bytecode is the oracle; no game bytecode is copied into the repo.
 * Instrument only this translation unit to count actual interpreter entries. */
#ifdef NDEBUG
#undef NDEBUG /* These executables must retain their behavioral checks. */
#endif
#include "../../vm.c"
#include "of_native.h"
#include "of_game.h"
#include <assert.h>

static unsigned interpreterEntries, workFailures, checks;
static bool observe;
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *caller) {
    (void)caller;
    if (observe && fn == (void *)executeBytecodeInstructions)
        ++interpreterEntries;
}
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *caller) {
    (void)fn;
    (void)caller;
}

static const char *names[] = {"layer_get_all",          "array_length_1d",        "layer_get_depth",
                              "layer_get_all_elements", "layer_get_element_type", "layer_tile_get_x",
                              "layer_tile_x",           "layer_tile_get_y",       "layer_tile_y"};
static BuiltinFunc originals[9];
static uint64_t trace;
static unsigned calls;
static void hashWord(uint32_t word) {
    trace = (trace ^ word) * 1099511628211ULL;
}
static void hashValue(RValue v) {
    hashWord(v.type);
    if (v.type == RVALUE_ARRAY) {
        int n = GMLArray_length1D(v.array);
        hashWord(n);
        for (int i = 0; i < n; i++)
            hashValue(GMLArray_get(v.array, i));
    } else if (v.type == RVALUE_REAL) {
        uint32_t bits;
        float x = (float)v.real;
        memcpy(&bits, &x, 4);
        hashWord(bits);
    } else if (v.type == RVALUE_STRING) {
        for (const char *p = v.string; *p; p++)
            hashWord((unsigned char)*p);
    } else
        hashWord(RValue_toInt32(v));
}
static RValue traced(int which, VMContext *vm, RValue *a, int n) {
    ++calls;
    hashWord(which);
    hashWord(n);
    for (int i = 0; i < n; i++)
        hashValue(a[i]);
    RValue result = originals[which](vm, a, n);
    hashValue(result);
    return result;
}
#define WRAPPER(n)                                                                                                     \
    static RValue trace##n(VMContext *v, RValue *a, int32_t c) {                                                       \
        return traced(n, v, a, c);                                                                                     \
    }
WRAPPER(0)
WRAPPER(1) WRAPPER(2) WRAPPER(3) WRAPPER(4) WRAPPER(5) WRAPPER(6) WRAPPER(7)
    WRAPPER(8) static BuiltinFunc wrappers[] = {trace0, trace1, trace2, trace3, trace4, trace5, trace6, trace7, trace8};

static const char *assetPath;
static DataWin *loadData(void) {
    DataWinParserOptions opts = {0};
    opts.parseGen8 = opts.parseScpt = opts.parseCode = opts.parseVari = opts.parseFunc = opts.parseStrg = true;
    return DataWin_parse(assetPath, opts);
}
typedef struct {
    uint64_t trace;
    unsigned calls;
    int x[6], y[6];
} Outcome;
static Outcome run(DataWin *dw, int code, int native, int scenario, GMLReal epsilon, RValue *args, int count,
                   bool expectedFast) {
    dw = loadData(); /* VM_create resolves operands in place: each VM needs fresh input. */
    Runner r = {0};
    r.dataWin = dw;
    r.pendingRoom = -1;
    r.mathEpsilon = epsilon;
    VMContext *vm = VM_create(dw);
    r.vmContext = vm;
    vm->runner = &r;
    Instance *self = Instance_create(50, -1, 0, 0);
    CallFrame caller = {0};
    vm->currentCallFrame = &caller;
    caller.self = self;
    vm->globalScopeInstance = Instance_create(0, STRUCT_OBJECT_INDEX, 0, 0);
    int sentinel = VM_getOrAllocateVarID(vm, "__tile_fixture_sentinel");
    Instance_setSelfVar(self, sentinel, RValue_makeInt32(123));
    /* Caller state must survive both the native and the interpreted script. */
    RValue locals[1] = {RValue_makeString("caller local")}, outerArgs[1] = {RValue_makeInt32(789)};
    vm->currentCallFrame->localVars = locals;
    vm->currentCallFrame->localVarsCount = 1;
    vm->currentCallFrame->scriptArgs = outerArgs;
    vm->currentCallFrame->scriptArgsCount = 1;
    vm->currentCallFrame->codeIndex = -1;
    vm->currentCallFrame->eventType = EVENT_STEP;
    stackPush(vm, RValue_makeInt32(456));
    RoomTile tiles[6] = {0};
    for (int i = 0; i < 6; i++) {
        tiles[i].x = 10 + i * 10;
        tiles[i].y = -10 - i * 10;
        tiles[i].scaleX = i % 2 ? -2 : 0.5f;
        tiles[i].scaleY = 3;
    }
    if (scenario != 1) {
        for (int i = 0; i < 4; i++) {
            RuntimeLayer l = {0};
            l.id = 10 + i;
            l.depth = i == 1 ? 99 : 20;
            arrput(r.runtimeLayers, l);
        }
        for (int i = 0; i < 6; i++) {
            RuntimeLayerElement el = {0};
            el.id = i == 2 ? 100 : 100 + i;
            el.type = i == 3 ? RuntimeLayerElementType_Background : RuntimeLayerElementType_Tile;
            el.tileElement = i == 4 ? NULL : &tiles[i];
            arrput(r.runtimeLayers[i < 2 ? 0 : i < 4 ? 1 : 2].elements, el);
        }
        /* Duplicate layer ID: get_all includes both; first-match get_depth/all_elements wins. */
        if (scenario == 2)
            r.runtimeLayers[2].id = 10;
        if (scenario == 3)
            r.runtimeLayers[0].id = 16777217; /* REAL rounds this to a missing ID. */
        if (scenario == 4) {
            r.runtimeLayers[0].id = 0;
            r.runtimeLayers[2].id = 2147483520u;
        }
        if (scenario == 5) {
            r.runtimeLayers[0].depth = INT32_MIN;
            r.runtimeLayers[2].depth = INT32_MAX;
        }
        if (scenario == 6) {
            r.runtimeLayers[0].id = (uint32_t)-1;
            r.runtimeLayers[2].id = (uint32_t)-7;
        }
        /* A dead instance element is omitted by the real all_elements builtin. */
        RuntimeLayerElement dead = {0};
        dead.id = 999;
        dead.type = RuntimeLayerElementType_Instance;
        dead.instanceId = 12345;
        arrput(r.runtimeLayers[0].elements, dead);
    }
    for (int i = 0; i < 9; i++) {
        originals[i] = VM_findBuiltin(vm, names[i]);
        assert(originals[i]);
        shput(vm->builtinMap, names[i], wrappers[i]);
        for (uint32_t j = 0; j < dw->func.functionCount; j++)
            if (!strcmp(dw->func.functions[j].name, names[i]))
                vm->funcCallCache[j].builtin = (void *)wrappers[i];
    }
    /* Removing only the registry entry leaves the interpreted call cache
     * intact. The native must decline before executing any builtin. */
    if (scenario == 7)
        shdel(vm->builtinMap, "layer_tile_y");
    if (native == 0)
        setenv("BS_NO_NATIVE", "1", 1);
    else
        unsetenv("BS_NO_NATIVE");
    if (native < 2)
        ofNativeInstall(&r);
    else {
        for (size_t i = 0; i < ofGame.nativeScriptCount; ++i) {
            const OfNativeScript *n = &ofGame.nativeScripts[i];
            if (strcmp(n->name, "gml_Script_tile_layer_shift"))
                continue;
            bool installed = VM_setNativeScript(vm, n->name, n->length + (native == 2 ? 4 : 0),
                                                n->hash ^ (native == 3 ? 1 : 0), n->native);
            assert(!installed);
        }
    }
    trace = 1469598103934665603ULL;
    calls = 0;
    interpreterEntries = 0;
    observe = true;
    RValue result = VM_callCodeIndex(vm, code, args, count);
    observe = false;
    assert(result.type == RVALUE_UNDEFINED);
    RValue_free(&result);
    printf("case %d native %d epsilon %.9g: %u interpreter entries, %u builtin calls\n", scenario, native,
           (double)epsilon, interpreterEntries, calls);
    if (native && expectedFast && interpreterEntries != 0)
        ++workFailures;
    if (!native || !expectedFast)
        assert(interpreterEntries == 1);
    assert(vm->currentCallFrame->self == self && vm->currentCallFrame == &caller && caller.codeIndex == UINT32_MAX);
    assert(vm->currentCallFrame->localVars == locals && vm->currentCallFrame->localVarsCount == 1 && !strcmp(locals[0].string, "caller local"));
    assert(vm->currentCallFrame->scriptArgs == outerArgs && vm->currentCallFrame->scriptArgsCount == 1 && outerArgs[0].int32 == 789);
    assert(vm->stack.top == 1 && vm->stack.slots[0].int32 == 456 && caller.parent == NULL);
    assert(vm->currentCallFrame->eventType == EVENT_STEP && IntRValueHashMap_count(&self->selfVars) == 1);
    assert(Instance_getSelfVar(self, sentinel).int32 == 123);
    Outcome out = {.trace = trace, .calls = calls};
    for (int i = 0; i < 6; i++) {
        out.x[i] = tiles[i].x;
        out.y[i] = tiles[i].y;
        assert(tiles[i].scaleX == (i % 2 ? -2 : 0.5f) && tiles[i].scaleY == 3);
    }
    if (scenario == 0 && epsilon == GML_MATH_EPSILON && count == 3 && args[0].type == RVALUE_REAL &&
        args[0].real == 20 && args[1].type == RVALUE_REAL && args[1].real == 2.75f) {
        assert(out.x[0] == 12 && out.y[0] == -13 && out.x[1] == 22 && out.y[1] == -23);
        assert(out.x[2] == 30 && out.y[2] == -30 && out.x[5] == 62 && out.y[5] == -63);
    }
    for (ptrdiff_t i = 0; i < arrlen(r.runtimeLayers); i++)
        arrfree(r.runtimeLayers[i].elements);
    arrfree(r.runtimeLayers);
    hmfree(r.instancesById);
    vm->currentCallFrame->localVars = NULL;
    vm->currentCallFrame->localVarsCount = 0;
    vm->currentCallFrame->scriptArgs = NULL;
    vm->currentCallFrame->scriptArgsCount = 0;
    RValue sentinelValue = stackPop(vm);
    assert(sentinelValue.type == RVALUE_INT32 && sentinelValue.int32 == 456);
    RValue_free(&sentinelValue);
    vm->currentCallFrame = NULL;
    Instance_free(self);
    VM_free(vm);
    DataWin_free(dw);
    ++checks;
    return out;
}
static void pair(DataWin *dw, int code, int scenario, GMLReal epsilon, RValue *args, int count, bool fast) {
    Outcome a = run(dw, code, false, scenario, epsilon, args, count, false);
    Outcome b = run(dw, code, true, scenario, epsilon, args, count, fast);
    assert(a.trace == b.trace && a.calls == b.calls && !memcmp(a.x, b.x, sizeof(a.x)) &&
           !memcmp(a.y, b.y, sizeof(a.y)));
}
int main(int argc, char **argv) {
    assert(argc >= 2);
    bool behaviorOnly = argc > 2 && !strcmp(argv[2], "--behavior-only");
    assetPath = argv[1];
    DataWin *dw = loadData();
    assert(dw);
    int code = -1;
    for (uint32_t i = 0; i < dw->code.count; i++)
        if (!strcmp(dw->code.entries[i].name, "gml_Script_tile_layer_shift"))
            code = i;
#ifdef GAME_DELTARUNE
    assert(code >= 0 && dw->code.entries[code].offset == 4);
    RValue args[] = {RValue_makeReal(20), RValue_makeReal(2.75f), RValue_makeReal(-3.25f)};
    if (argc > 2 && !strcmp(argv[2], "--legacy-unsigned-ids")) {
        /* Explicit opt-in characterization of pre-existing float-to-int UB.
         * Keep this outside the sanitizer-clean regression suite. */
        args[0] = RValue_makeReal(0);
        pair(dw, code, 6, GML_MATH_EPSILON, args, 3, true);
        DataWin_free(dw);
        return workFailures ? 1 : 0;
    }
    pair(dw, code, 0, GML_MATH_EPSILON, args, 3, true);
    pair(dw, code, 1, GML_MATH_EPSILON, args, 3, true);
    pair(dw, code, 2, GML_MATH_EPSILON, args, 3, true);
    pair(dw, code, 0, 0, args, 3, true);
    pair(dw, code, 0, 1.5f, args, 3, true);
    args[0] = RValue_makeReal(20.25f);
    pair(dw, code, 0, 0.5f, args, 3, true);
    args[0] = RValue_makeReal(999);
    pair(dw, code, 0, GML_MATH_EPSILON, args, 3, true);
    args[0] = RValue_makeString("20");
    pair(dw, code, 0, GML_MATH_EPSILON, args, 3, false);
    args[0] = RValue_makeReal(20);
    args[1] = RValue_makeInt32(-2);
    args[2] = RValue_makeInt32(3);
    pair(dw, code, 0, GML_MATH_EPSILON, args, 3, true);
    pair(dw, code, 0, GML_MATH_EPSILON, args, 1, false);
    args[0] = RValue_makeReal(NAN);
    pair(dw, code, 0, GML_MATH_EPSILON, args, 3, false);
    args[0] = RValue_makeReal(20);
    pair(dw, code, 0, NAN, args, 3, false);
    args[0] = RValue_makeInt32(20);
    pair(dw, code, 0, 0.5f, args, 3, true);
    pair(dw, code, 0, -0.25f, args, 3, false);
    args[0] = RValue_makeReal(0);
    pair(dw, code, 3, GML_MATH_EPSILON, args, 3, true);
    args[0] = RValue_makeReal(20);
    pair(dw, code, 4, GML_MATH_EPSILON, args, 3, true);
    args[0] = RValue_makeInt32(INT32_MIN);
    pair(dw, code, 5, GML_MATH_EPSILON, args, 3, true);
    args[0] = RValue_makeInt32(INT32_MAX);
    pair(dw, code, 5, GML_MATH_EPSILON, args, 3, true);
    args[0] = RValue_makeReal(20);
    pair(dw, code, 7, GML_MATH_EPSILON, args, 3, false);
    args[0] = RValue_makeReal(99);
    pair(dw, code, 0, GML_MATH_EPSILON, args, 3, true);
    /* Corrupt the expected registration identity, not proprietary bytecode. */
    VMContext *vm = VM_create(dw);
    for (size_t i = 0; i < ofGame.nativeScriptCount; i++)
        if (!strcmp(ofGame.nativeScripts[i].name, "gml_Script_tile_layer_shift")) {
            const OfNativeScript *n = &ofGame.nativeScripts[i];
            bool wrongLength = VM_setNativeScript(vm, n->name, n->length + 4, n->hash, n->native);
            bool wrongHash = VM_setNativeScript(vm, n->name, n->length, n->hash ^ 1, n->native);
            assert(!wrongLength && !wrongHash);
            assert(vm->nativeScript == NULL);
            ++checks;
        }
    VM_free(vm);
    args[0] = RValue_makeReal(20);
    Outcome oracle = run(dw, code, 0, 0, GML_MATH_EPSILON, args, 3, false);
    for (int mode = 2; mode <= 3; mode++) {
        Outcome fallback = run(dw, code, mode, 0, GML_MATH_EPSILON, args, 3, false);
        assert(oracle.trace == fallback.trace && oracle.calls == fallback.calls);
        assert(!memcmp(oracle.x, fallback.x, sizeof(oracle.x)) && !memcmp(oracle.y, fallback.y, sizeof(oracle.y)));
    }
#else
    assert(code < 0);
    Runner r = {0};
    r.dataWin = dw;
    r.vmContext = VM_create(dw);
    r.vmContext->runner = &r;
    ofNativeInstall(&r);
    for (size_t i = 0; i < ofGame.nativeScriptCount; i++)
        assert(strcmp(ofGame.nativeScripts[i].name, "gml_Script_tile_layer_shift"));
    VM_free(r.vmContext);
    ++checks;
#endif
    DataWin_free(dw);
    printf("%u behavior checks, %u work-bound failures\n", checks, workFailures);
    return !behaviorOnly && workFailures ? 1 : 0;
}
