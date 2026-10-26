/* Exercise real contact routing through Runner events and VM native dispatch. */
#include "../src/physics/physics.c"
#include <assert.h>

#ifndef OF_PC
void platformLog(const logType type, const char* format, va_list args) {
    (void)type; (void)format; (void)args;
}
#endif

static Instance self, other;
static unsigned callbacks;
static bool collision(VMContext* ctx) {
    CallFrame* frame = ctx->currentCallFrame;
    assert(frame != NULL);
    assert(frame->self == &self && frame->initialSelf == &self);
    assert(frame->other == &other && frame->initialOther == &other);
    assert(frame->eventType == EVENT_COLLISION && frame->eventSubtype == 1);
    assert(frame->eventObjectIndex == 3); /* Handler inherited from self's parent. */
    ++callbacks;
    return true;
}

int main(void) {
    Runner* runner = calloc(1, sizeof(*runner));
    VMContext* ctx = calloc(1, sizeof(*ctx));
    assert(runner != NULL && ctx != NULL);
    DataWin data = {0};
    GameObject objects[4] = {0};
    for (int i = 0; i < 4; ++i) { objects[i].name = "contact_object"; objects[i].parentId = -1; }
    objects[0].parentId = 3;
    objects[2].parentId = 1;
    data.objt.count = 4; data.objt.objects = objects;
    data.gen8.wadVersion = 17;
    uint32_t word = 0;
    data.bytecodeBuffer = (uint8_t*)&word;
    CodeEntry code = {.name = "collision", .length = 0};
    data.code.count = 1; data.code.entries = &code;
    IntIntHashMap locals = {0};
    VMNativeCode native[] = {collision};
    ctx->dataWin = &data; ctx->runner = runner; ctx->codeLocalsSlotMaps = &locals; ctx->nativeCode = native;
    runner->dataWin = &data; runner->vmContext = ctx; runner->pendingRoom = -1;
    int16_t slots[] = {-1, 0, -1, -1};
    runner->eventSlotMap.denseLookup[EVENT_COLLISION] = slots;
    runner->eventSlotMap.maxSubtypeByType[EVENT_COLLISION] = 3;
    uint32_t starts[] = {0, 1, 1, 1, 1};
    ObjectEventEntry entry = {0, 3, 0};
    runner->eventTable.objectCount = 4;
    runner->eventTable.byObjectStart = starts; runner->eventTable.byObject = &entry;
    self.objectIndex = 0;
    for (int active = 0; active < 2; ++active) {
        CallFrame caller = {0};
        caller.other = &self; caller.eventType = EVENT_ALARM; caller.eventSubtype = 7;
        CallFrame saved = caller;
        ctx->currentCallFrame = active ? &caller : NULL;
        for (int inherited = 0; inherited < 2; ++inherited) {
            other.objectIndex = inherited ? 2 : 1;
            unsigned before = callbacks;
            dispatchContact(runner, &self, &other);
            assert(callbacks == before + 1);
            assert(ctx->currentCallFrame == (active ? &caller : NULL));
            assert(memcmp(&caller, &saved, sizeof(caller)) == 0);
        }
    }
    free(ctx); free(runner);
    puts("PASS: physics contact self/other, target and handler inheritance, idle/active caller restoration");
    return 0;
}
