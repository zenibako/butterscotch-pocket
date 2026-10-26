#include "vm.h"
#include "op.h"
#include "stb_ds.h"
#include <assert.h>
#include <stdio.h>

// Resolved WAD16/17 bytecode: parent calls child, then a builtin and returns it.
// Use an asset-free context so exact exclusive counts do not depend on a game.
static RValue builtin(VMContext* ctx, RValue* args, int32_t count) {
    (void) ctx; (void) args;
    assert(count == 0);
    return RValue_makeInt32(17);
}

static bool nativeDone;
static bool nativeCode(VMContext* ctx) {
    (void) ctx;
    return nativeDone;
}

static bool nativeScript(VMContext* ctx, RValue* args, int32_t count, RValue* result) {
    (void) ctx; (void) args;
    assert(count == 0);
    *result = RValue_makeInt32(9);
    return nativeDone;
}

static void stats(Profiler* p, const char* name, uint64_t ops, uint64_t calls) {
    ptrdiff_t index = shgeti(p->entries, name);
    assert(index >= 0);
    assert(p->entries[index].value.ops == ops);
    assert(p->entries[index].value.calls == calls);
}

static void run(VMContext* ctx) {
    RValue result = VM_executeCode(ctx, 0, NULL, NULL, CODE_NOT_RELATED_TO_EVENTS, CODE_NOT_RELATED_TO_EVENTS, CODE_NOT_RELATED_TO_EVENTS, NULL, 0);
    if (ctx->nativeCode != NULL && nativeDone) {
        assert(result.type == RVALUE_UNDEFINED);
    } else {
        assert(result.type == RVALUE_INT32 && result.int32 == 17);
    }
    RValue_free(&result);
    assert(ctx->stack.top == 0 && ctx->currentCallFrame == NULL);
    if (ctx->profiler != NULL) assert(ctx->profiler->frameDepth == 0);
}

int main(void) {
    uint32_t words[] = {
        (uint32_t) OP_CALL << 24 | GML_TYPE_VARIABLE << 16, 0,
        (uint32_t) OP_POPZ << 24,
        (uint32_t) OP_CALL << 24 | GML_TYPE_VARIABLE << 16, 1,
        (uint32_t) OP_RET << 24,
        (uint32_t) OP_PUSH_IMMEDIATE << 24 | GML_TYPE_INT16 << 16 | 7,
        (uint32_t) OP_RET << 24
    };
    CodeEntry codes[] = {
        { .name = "parent", .length = 24 },
        { .name = "child", .offset = 24, .length = sizeof(words) }
    };
    Function functions[] = { { .name = "child" }, { .name = "builtin" } };
    FuncCallCache cache[] = {
        { .scriptCodeIndex = 1 }, { .builtin = (void*) builtin, .scriptCodeIndex = -1 }
    };
    IntIntHashMap localMaps[2] = {0};
    VMNativeCode nativeCodes[] = { nativeCode, NULL };
    VMNativeScript nativeScripts[] = { NULL, nativeScript };
    DataWin data = {0};
    data.bytecodeBuffer = (uint8_t*) words;
    data.code.count = 2; data.code.entries = codes;
    data.func.functionCount = 2; data.func.functions = functions;
    VMContext* ctx = calloc(1, sizeof(*ctx));
    assert(ctx != NULL);
    ctx->dataWin = &data; ctx->funcCallCache = cache; ctx->codeLocalsSlotMaps = localMaps;

    // Both games' bytecode versions, with the same Pocket interpreter flags.
    for (int wad = 16; wad <= 17; wad++) {
        data.gen8.wadVersion = wad;
        Profiler_setEnabled(&ctx->profiler, false);
        Profiler_setEnabled(&ctx->profiler, false);
        run(ctx); assert(ctx->profiler == NULL);
        Profiler_setEnabled(&ctx->profiler, true);
        Profiler* enabled = ctx->profiler;
        Profiler_setEnabled(&ctx->profiler, true);
        assert(ctx->profiler == enabled);
        run(ctx); run(ctx);
        assert(enabled->instructionCount == 12);
        stats(enabled, "parent", 8, 2); stats(enabled, "child", 4, 2);
        stats(enabled, "builtin", 0, 2);
        char* report = Profiler_createReport(enabled, 10, 2);
        assert(report != NULL && strstr(report, "parent") && strstr(report, "child"));
        free(report);

        Profiler_reset(enabled);
        assert(shlen(enabled->entries) == 0);
        ctx->nativeScript = nativeScripts; nativeDone = true;
        run(ctx);
        assert(enabled->instructionCount == 16);
        stats(enabled, "parent", 4, 1); stats(enabled, "child", 0, 1);
        Profiler_reset(enabled); nativeDone = false;
        run(ctx);
        assert(enabled->instructionCount == 22);
        stats(enabled, "parent", 4, 1); stats(enabled, "child", 2, 1);
        ctx->nativeScript = NULL;

        Profiler_reset(enabled); ctx->nativeCode = nativeCodes; nativeDone = true;
        run(ctx);
        assert(enabled->instructionCount == 22); stats(enabled, "parent", 0, 1);
        Profiler_reset(enabled); nativeDone = false;
        run(ctx);
        assert(enabled->instructionCount == 28); stats(enabled, "parent", 4, 1);
        ctx->nativeCode = NULL;

        Profiler_setEnabled(&ctx->profiler, false);
        run(ctx); assert(ctx->profiler == NULL);
        Profiler_setEnabled(&ctx->profiler, true);
        run(ctx);
        assert(ctx->profiler->instructionCount == 6);
        stats(ctx->profiler, "parent", 4, 1); stats(ctx->profiler, "child", 2, 1);
        Profiler_setEnabled(&ctx->profiler, false);
    }
    free(ctx);
    puts("PASS: profiler nested/native counts, reports, reset and off/on/off toggles (WAD16/17)");
    return 0;
}
