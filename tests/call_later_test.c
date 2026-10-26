/* Exercise the real deferred-callback dispatcher and VM while no script runs.
 * Including runner.c exposes its private dispatcher without adding a public API. */
#include "../src/runner.c"
#include <assert.h>
#include "op.h"

#ifndef OF_PC
void platformLog(const logType type, const char* format, va_list args) {
    (void)type; (void)format; (void)args;
}
#endif

static Instance globalSelf, boundSelf;
static Instance* expectedSelf;
static int expectedObject, expectedEvent, expectedSubtype;
static unsigned calls;
static GMLMethod* returnedMethod;

static RValue probe(VMContext* ctx, RValue* args, int32_t count) {
    assert(args == NULL && count == 0);
    CallFrame* frame = ctx->currentCallFrame;
    assert(frame != NULL && frame->self == expectedSelf && frame->initialSelf == expectedSelf);
    assert(frame->other == NULL && frame->initialOther == NULL);
    assert(frame->eventObjectIndex == expectedObject && frame->eventType == expectedEvent);
    assert(frame->eventSubtype == expectedSubtype);
    assert(strcmp(VM_getCurrentCallFrameCodeName(ctx),
                  frame->codeIndex == UINT32_MAX ? "<unknown>" : "deferred_script") == 0);
    ++calls;
    GMLMethod_incRef(returnedMethod);
    return RValue_makeMethod(returnedMethod);
}

static void invoke(VMContext* ctx, RValue callback, Instance* target, bool active) {
    CallFrame caller = {0};
    caller.self = &boundSelf;
    caller.other = &globalSelf;
    caller.eventObjectIndex = 12;
    caller.eventType = EVENT_ALARM;
    caller.eventSubtype = 4;
    ctx->currentCallFrame = active ? &caller : NULL;
    CallFrame saved = caller;
    expectedSelf = target;
    expectedObject = active ? 12 : CODE_NOT_RELATED_TO_EVENTS;
    expectedEvent = active ? EVENT_ALARM : CODE_NOT_RELATED_TO_EVENTS;
    expectedSubtype = active ? 4 : CODE_NOT_RELATED_TO_EVENTS;
    unsigned before = calls;
    Runner_executeCallLaterCallback(ctx, callback);
    assert(calls == before + 1);
    assert(returnedMethod->refCount == 1); /* Ignored results must be released. */
    assert(ctx->currentCallFrame == (active ? &caller : NULL));
    assert(memcmp(&saved, &caller, sizeof(caller)) == 0);
    assert(ctx->stack.top == 0);
    ctx->currentCallFrame = NULL;
}

int main(void) {
    VMContext* ctx = calloc(1, sizeof(*ctx));
    Runner* runner = calloc(1, sizeof(*runner));
    assert(ctx != NULL && runner != NULL);
    DataWin data = {0};
    uint32_t words[] = {(uint32_t)OP_CALL << 24 | GML_TYPE_VARIABLE << 16, 0,
                        (uint32_t)OP_RET << 24};
    CodeEntry code = {.name = "deferred_script", .length = sizeof(words)};
    Function functions[] = {{.name = "probe"}, {.name = "deferred_script"}, {.name = "numeric_builtin"}};
    Script script = {.codeId = 0};
    FuncCallCache cache[] = {{.builtin = (void*)probe, .scriptCodeIndex = -1}};
    IntIntHashMap locals = {0};
    data.gen8.wadVersion = 17;
    data.bytecodeBuffer = (uint8_t*)words;
    data.code.entries = &code; data.code.count = 1;
    data.func.functions = functions; data.func.functionCount = 3;
    data.scpt.scripts = &script; data.scpt.count = 1;
    ctx->dataWin = &data; ctx->runner = runner; ctx->globalScopeInstance = &globalSelf;
    ctx->funcCallCache = cache; ctx->codeLocalsSlotMaps = &locals;
    runner->vmContext = ctx;
    int32_t boundId = 100001;
    hmput(runner->instancesById, boundId, &boundSelf);
    shput(ctx->codeIndexByName, "deferred_script", 0);
    shput(ctx->builtinMap, "numeric_builtin", probe);
    returnedMethod = GMLMethod_create(0, -1);
    RValue boundScript = RValue_makeMethod(GMLMethod_create(0, 100001));
    RValue missingScript = RValue_makeMethod(GMLMethod_create(0, 100099));
    RValue boundBuiltin = RValue_makeMethod(GMLMethod_createBuiltin(probe, 100001));
    RValue missingBuiltin = RValue_makeMethod(GMLMethod_createBuiltin(probe, 100099));
    for (int active = 0; active < 2; ++active) {
        invoke(ctx, RValue_makeInt32(1), &globalSelf, active != 0);
        invoke(ctx, RValue_makeInt32(0), &globalSelf, active != 0); /* SCPT fallback. */
        invoke(ctx, boundScript, &boundSelf, active != 0);
        invoke(ctx, missingScript, &globalSelf, active != 0);
        invoke(ctx, boundBuiltin, &boundSelf, active != 0);
        invoke(ctx, missingBuiltin, &globalSelf, active != 0);
        invoke(ctx, RValue_makeInt32(2), &globalSelf, active != 0);
    }
    // A builtin-only game has no code entry to borrow for its diagnostic name.
    data.code.count = 0;
    assert(strcmp(VM_getCurrentCallFrameCodeName(ctx), "<unknown>") == 0);
    for (int active = 0; active < 2; ++active) {
        invoke(ctx, boundBuiltin, &boundSelf, active != 0);
        invoke(ctx, missingBuiltin, &globalSelf, active != 0);
        invoke(ctx, RValue_makeInt32(2), &globalSelf, active != 0);
    }
    RValue_free(&boundScript); RValue_free(&missingScript);
    RValue_free(&boundBuiltin); RValue_free(&missingBuiltin);
    GMLMethod_decRef(returnedMethod);
    hmfree(runner->instancesById); shfree(ctx->codeIndexByName); shfree(ctx->builtinMap);
    free(runner); free(ctx);
    puts("PASS: call_later idle/active script, method, builtin, fallback and owned returns (20 callbacks)");
    return 0;
}
