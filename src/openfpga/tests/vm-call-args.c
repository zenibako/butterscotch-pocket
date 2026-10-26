/* Asset-free resolved bytecode, using the real CALL/CALLV interpreter and
 * Runner event re-entry. No VM source is included or replaced. */
#include "runner.h"
#include "vm.h"
#include "op.h"
#include "gml_array.h"
#include "gml_method.h"
#include <assert.h>
#include <stdio.h>

#if defined(ENABLE_WAD16) && !defined(ENABLE_WAD17)
#define FIXTURE_WAD 16
#elif defined(ENABLE_WAD17) && !defined(ENABLE_WAD16)
#define FIXTURE_WAD 17
#else
#error Build this fixture with exactly one WAD version
#endif

enum { ROOT, CHILD, EVENT, CODES };
enum { ARG, METHOD, TARGET, PROBE, EVENT_PROBE, FUNCTIONS };
enum { BUILTIN_TARGET, SCRIPT_TARGET, UNKNOWN_TARGET };
enum { MAX_ARGS = 33 };
typedef struct {
    Runner runner; /* Callbacks recover the fixture from the real Runner. */
    VMContext vm;
    DataWin data;
    CodeEntry codes[CODES];
    Function functions[FUNCTIONS];
    FuncCallCache cache[FUNCTIONS];
    IntIntHashMap locals[CODES];
    uint32_t words[1024];
    int used;
    RValue originals[MAX_ARGS];
    Instance *self, *eventSelf;
    int count, target, returnIndex;
    unsigned checks, events, nativeCalls;
    int nativeMode;
    VMNativeScript nativeScripts[CODES];
    VMNativeCode nativeCodes[CODES];
    bool inEvent;
} Fixture;

static void equalValue(RValue value, RValue expected) {
    assert(value.type == expected.type);
    switch (value.type) {
    case RVALUE_STRING: assert(!strcmp(value.string, expected.string)); break;
    case RVALUE_ARRAY: assert(value.array == expected.array); break;
    case RVALUE_STRUCT: assert(value.structInst == expected.structInst); break;
#if IS_WAD17_OR_HIGHER_ENABLED
    case RVALUE_METHOD: assert(value.method == expected.method); break;
#endif
    default: assert(RValue_toInt32(value) == RValue_toInt32(expected)); break;
    }
}

static RValue argument(VMContext *ctx, RValue *args, int32_t count) {
    Fixture *f = (Fixture *)ctx->runner;
    assert(count == 1 && args != NULL);
    int index = RValue_toInt32(args[0]);
    assert(index >= 0 && index < MAX_ARGS);
    return RValue_makeIndependent(f->originals[index]);
}

static RValue eventProbe(VMContext *ctx, RValue *args, int32_t count) {
    Fixture *f = (Fixture *)ctx->runner;
    assert(f->inEvent && args == NULL && count == 0);
    assert(ctx->currentCallFrame->self == f->eventSelf);
    assert(ctx->currentCallFrame->eventType == EVENT_CREATE);
    assert(ctx->currentCallFrame->eventObjectIndex == 0 && ctx->currentCallFrame->eventSubtype == 0);
    ++f->events;
    return RValue_makeOwnedString(safeStrdup("event result"));
}

static RValue checkArguments(VMContext *ctx, RValue *args, int32_t count) {
    Fixture *f = (Fixture *)ctx->runner;
    assert(count == f->count && (args == NULL) == (count == 0));
    for (int i = 0; i < count; ++i) equalValue(args[i], f->originals[i]);
    assert(ctx->currentCallFrame->self == f->self);
    CallFrame *savedFrame = ctx->currentCallFrame;
    RValue *savedArgs = savedFrame->scriptArgs;
    int savedCount = ctx->currentCallFrame->scriptArgsCount;
    int savedTop = ctx->stack.top;
    assert(savedTop == 1 && RValue_toInt32(ctx->stack.slots[0]) == 1234);
    f->inEvent = true;
    Runner_executeEvent(&f->runner, f->eventSelf, EVENT_CREATE, 0);
    f->inEvent = false;
    assert(ctx->currentCallFrame == savedFrame);
    assert(ctx->currentCallFrame->scriptArgs == savedArgs && ctx->currentCallFrame->scriptArgsCount == savedCount);
    assert(ctx->stack.top == savedTop && RValue_toInt32(ctx->stack.slots[0]) == 1234);
    assert(ctx->currentCallFrame->self == f->self && ctx->currentCallFrame->other == f->eventSelf);
    assert(ctx->currentCallFrame->eventType == -12 && ctx->currentCallFrame->eventSubtype == -13 &&
           ctx->currentCallFrame->eventObjectIndex == -14);
    for (int j = 0; j < count; ++j) equalValue(args[j], f->originals[j]);
    ++f->checks;
    if (count == 0) return RValue_makeInt32(71);
    return RValue_makeIndependent(args[f->returnIndex]);
}

static RValue scriptProbe(VMContext *ctx, RValue *args, int32_t count) {
    Fixture *f = (Fixture *)ctx->runner;
    assert(args == NULL && count == 0 && ctx->currentCallFrame->parent != NULL && ctx->currentCallFrame->parent->parent == NULL);
    RValue checked = checkArguments(ctx, ctx->currentCallFrame->scriptArgs, ctx->currentCallFrame->scriptArgsCount);
    RValue_free(&checked);
    if (f->count == 0) return RValue_makeInt32(71);
    /* A real script returns a weak view of its own argument storage. The VM
     * must strengthen it before scriptArgs and caller arguments are released. */
    RValue result = ctx->currentCallFrame->scriptArgs[f->returnIndex];
    result.ownsReference = false;
    return result;
}

/* A native callee must see its own frame before it can re-enter the runner.
 * Returning a weak view of its copied storage exercises ownership cleanup. */
static void nativeFrame(VMContext *ctx, RValue *args, int32_t count) {
    Fixture *f = (Fixture *)ctx->runner;
    CallFrame *frame = ctx->currentCallFrame;
    assert(frame && frame->codeIndex == CHILD && frame->parent && frame->parent->codeIndex == ROOT);
    assert(frame->self == f->self && frame->initialSelf == f->self);
    assert(frame->other == f->eventSelf && frame->initialOther == f->eventSelf);
    assert(frame->eventObjectIndex == -14 && frame->eventType == -12 && frame->eventSubtype == -13);
    assert(frame->localVarsCount == 1 && frame->localVars != NULL);
    assert(frame->localVars[0].type == RVALUE_UNDEFINED);
    assert(frame->scriptArgsCount == count && (frame->scriptArgs == NULL) == (count == 0));
    if (count && f->nativeMode < 3) assert(frame->scriptArgs != args);
    for (int i = 0; i < count; ++i) {
        equalValue(frame->scriptArgs[i], f->originals[i]);
        if (f->nativeMode < 3 && args[i].type == RVALUE_STRING) assert(frame->scriptArgs[i].string != args[i].string);
    }
    ++f->nativeCalls;
}

static bool nativeScript(VMContext *ctx, RValue *args, int32_t count, RValue *result) {
    Fixture *f = (Fixture *)ctx->runner;
    CallFrame *frame = ctx->currentCallFrame;
    nativeFrame(ctx, args, count);
    if (f->nativeMode == 2) return false;
    RValue checked = checkArguments(ctx, args, count);
    RValue_free(&checked);
    if (count == 0) *result = RValue_makeInt32(71);
    else {
        /* Local storage is also cleaned up on return. */
        frame->localVars[0] = RValue_makeIndependent(frame->scriptArgs[f->returnIndex]);
        *result = frame->localVars[0];
        result->ownsReference = false;
    }
    return true;
}

static bool nativeCode(VMContext *ctx) {
    Fixture *f = (Fixture *)ctx->runner;
    CallFrame *frame = ctx->currentCallFrame;
    nativeFrame(ctx, frame->scriptArgs, frame->scriptArgsCount);
    if (f->nativeMode == 4) return false;
    RValue checked = checkArguments(ctx, frame->scriptArgs, frame->scriptArgsCount);
    RValue_free(&checked);
    if (frame->scriptArgsCount)
        frame->localVars[0] = RValue_makeIndependent(frame->scriptArgs[f->returnIndex]);
    return true;
}

#if IS_WAD17_OR_HIGHER_ENABLED
static RValue method(VMContext *ctx, RValue *args, int32_t count) {
    Fixture *f = (Fixture *)ctx->runner;
    assert(args == NULL && count == 0);
    if (f->target == SCRIPT_TARGET)
        return RValue_makeMethod(GMLMethod_create(CHILD, INSTANCE_SELF));
    if (f->target == BUILTIN_TARGET)
        return RValue_makeMethod(GMLMethod_createBuiltin(checkArguments, INSTANCE_SELF));
    return RValue_makeMethod(GMLMethod_createUnresolved("fixture_missing", INSTANCE_SELF));
}
#endif

static void word(Fixture *f, uint32_t value) {
    assert(f->used < (int)(sizeof(f->words) / sizeof(f->words[0])));
    f->words[f->used++] = value;
}
static void push(Fixture *f, int value) {
    word(f, (uint32_t)OP_PUSH_IMMEDIATE << 24 | GML_TYPE_INT16 << 16 | (uint16_t)value);
}
static void call(Fixture *f, int function, int count) {
    word(f, (uint32_t)OP_CALL << 24 | GML_TYPE_VARIABLE << 16 | count);
    word(f, function);
}

static void program(Fixture *f, bool dynamic) {
    f->used = 0;
    push(f, 1234); /* Live caller stack survives script/event re-entry. */
    for (int i = f->count - 1; i >= 0; --i) { push(f, i); call(f, ARG, 1); }
    if (dynamic) {
        push(f, INSTANCE_SELF);
        call(f, METHOD, 0);
        word(f, (uint32_t)OP_CALLV << 24 | GML_TYPE_VARIABLE << 16 | f->count);
    } else {
        call(f, TARGET, f->count);
    }
    word(f, (uint32_t)OP_RET << 24);
    f->codes[ROOT].length = f->used * 4;
    f->codes[CHILD].offset = f->used * 4;
    call(f, PROBE, 0);
    word(f, (uint32_t)OP_RET << 24);
    f->codes[CHILD].length = f->used * 4;
    f->codes[EVENT].offset = f->used * 4;
    call(f, EVENT_PROBE, 0);
    word(f, (uint32_t)OP_RET << 24);
    f->codes[EVENT].length = f->used * 4;
    f->cache[TARGET].builtin = f->target == BUILTIN_TARGET ? (void *)checkArguments : NULL;
    f->cache[TARGET].scriptCodeIndex = f->target == SCRIPT_TARGET ? CHILD : -1;
}

static void referencesReleased(Fixture *f) {
    for (int i = 0; i < MAX_ARGS; ++i) {
        RValue value = f->originals[i];
        if (value.type == RVALUE_ARRAY) assert(value.array->refCount == 1);
        if (value.type == RVALUE_STRUCT) assert(value.structInst->refCount == 1);
#if IS_WAD17_OR_HIGHER_ENABLED
        if (value.type == RVALUE_METHOD) assert(value.method->refCount == 1);
#endif
    }
}

static void run(Fixture *f, bool dynamic) {
    program(f, dynamic);
    unsigned before = f->checks, events = f->events, natives = f->nativeCalls;
    f->nativeScripts[CHILD] = f->nativeMode == 1 || f->nativeMode == 2 ? nativeScript : NULL;
    f->nativeCodes[CHILD] = f->nativeMode >= 3 ? nativeCode : NULL;
    RValue result = VM_executeCode(&f->vm, ROOT, f->self, f->eventSelf, -14, -12, -13, NULL, 0);
    if (f->target == UNKNOWN_TARGET) {
        assert(result.type == RVALUE_UNDEFINED && f->checks == before && f->events == events);
    } else {
        assert(f->checks == before + 1 && f->events == events + 1);
        if (f->nativeMode == 3) assert(result.type == RVALUE_UNDEFINED);
        else if (f->count == 0) assert(RValue_toInt32(result) == 71);
        else {
            equalValue(result, f->originals[f->returnIndex]);
            if (result.type == RVALUE_STRING || result.type == RVALUE_ARRAY ||
                result.type == RVALUE_STRUCT || result.type == RVALUE_METHOD)
                assert(result.ownsReference);
        }
    }
    assert(f->nativeCalls == natives + (f->target == SCRIPT_TARGET && f->nativeMode ? 1 : 0));
    RValue_free(&result);
    referencesReleased(f);
    assert(f->vm.stack.top == 0 && f->vm.currentCallFrame == NULL);
#if IS_WAD17_OR_HIGHER_ENABLED
    assert(f->vm.exception == NULL);
#endif
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    Fixture *f = calloc(1, sizeof(*f));
    assert(f != NULL);
    f->runner.dataWin = &f->data; f->runner.vmContext = &f->vm; f->runner.pendingRoom = -1;
    f->vm.runner = &f->runner; f->vm.dataWin = &f->data;
    f->vm.nativeCode = f->nativeCodes;
    f->vm.nativeScript = f->nativeScripts;
    f->vm.funcCallCache = f->cache; f->vm.codeLocalsSlotMaps = f->locals;
    f->data.gen8.wadVersion = FIXTURE_WAD;
    f->data.bytecodeBuffer = (uint8_t *)f->words;
    f->data.code.count = CODES; f->data.code.entries = f->codes;
    f->data.func.functionCount = FUNCTIONS; f->data.func.functions = f->functions;
    const char *names[] = {"argument", "method", "target", "script_probe", "event_probe"};
    for (int i = 0; i < FUNCTIONS; ++i) {
        f->functions[i].name = (char *)names[i]; f->cache[i].scriptCodeIndex = -1;
    }
    f->cache[ARG].builtin = (void *)argument;
    f->cache[PROBE].builtin = (void *)scriptProbe;
    f->cache[EVENT_PROBE].builtin = (void *)eventProbe;
#if IS_WAD17_OR_HIGHER_ENABLED
    f->cache[METHOD].builtin = (void *)method;
#endif
    f->codes[CHILD].localsCount = 1;
    IntIntHashMap_getOrInsertSequential(&f->locals[CHILD], 0);
    f->codes[ROOT].name = "caller"; f->codes[CHILD].name = "callee"; f->codes[EVENT].name = "event";
    f->self = Instance_create(100001, 0, 0, 0);
    f->eventSelf = Instance_create(100002, 0, 0, 0);
    /* Minimal resolved Create event table, as consumed by Runner_executeEvent. */
    int16_t slot = 0;
    uint32_t starts[] = {0, 1};
    ObjectEventEntry event = {0, 0, EVENT};
    f->runner.eventSlotMap.denseLookup[EVENT_CREATE] = &slot;
    f->runner.eventTable.objectCount = 1;
    f->runner.eventTable.byObject = &event; f->runner.eventTable.byObjectStart = starts;
    for (int j = 0; j < MAX_ARGS; ++j) {
        switch (j % 5) {
        case 1: f->originals[j] = RValue_makeOwnedString(safeStrdup("owned argument")); break;
        case 2: f->originals[j] = RValue_makeArray(GMLArray_create(&f->data, 1)); break;
        case 3: f->originals[j] = RValue_makeStructAndIncRef(Instance_create(0, STRUCT_OBJECT_INDEX, 0, 0)); break;
#if IS_WAD17_OR_HIGHER_ENABLED
        case 4: f->originals[j] = RValue_makeMethod(GMLMethod_create(CHILD, INSTANCE_SELF)); break;
#endif
        default: f->originals[j] = RValue_makeInt32(j + 500); break;
        }
    }
    int counts[] = {0, 1, 16, 17, MAX_ARGS};
    unsigned cases = 0;
    for (int dynamic = 0; dynamic <= (FIXTURE_WAD >= 17); ++dynamic) {
        for (int target = BUILTIN_TARGET; target <= UNKNOWN_TARGET; ++target) {
            f->target = target;
            for (int c = 0; c < (int)(sizeof(counts) / sizeof(counts[0])); ++c) {
                f->count = counts[c];
                int returns = f->count > 5 ? 5 : 1;
                for (int r = 0; r < returns; ++r) {
                    f->returnIndex = r;
                    for (int native = 0; native <= (target == SCRIPT_TARGET ? 4 : 0); ++native) {
                        f->nativeMode = native;
                        run(f, dynamic != 0); ++cases;
                    }
                }
            }
        }
    }
    for (int k = 0; k < MAX_ARGS; ++k) {
        Instance *structure = f->originals[k].type == RVALUE_STRUCT ? f->originals[k].structInst : NULL;
        RValue_free(&f->originals[k]);
        if (structure != NULL) { assert(structure->refCount == 0); Instance_free(structure); }
    }
    for (int i = 0; i < CODES; ++i) IntIntHashMap_free(&f->locals[i]);
    Instance_free(f->self); Instance_free(f->eventSelf);
    printf("PASS: VM call arguments WAD%d, %u cases, %u checked calls and nested events\n", FIXTURE_WAD, cases, f->checks);
    free(f);
    return 0;
}
