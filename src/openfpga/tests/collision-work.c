/* Asset-free characterization of the real dispatcher. Include runner.c so no
 * production test hook is needed; the target excludes the usual runner.o. */
#include "../../runner.c"
#include <assert.h>
#include <stdio.h>

#if defined(ENABLE_WAD16) && !defined(ENABLE_WAD17)
#define FIXTURE_WAD 16
#elif defined(ENABLE_WAD17) && !defined(ENABLE_WAD16)
#define FIXTURE_WAD 17
#else
#error Build this fixture with exactly one WAD version
#endif

enum { SELF, PARENT, CHILD, UNRELATED, OBJECTS };
enum { NONE, CREATE, DESTROY, DEACTIVATE, CHANGE_OUT, CHANGE_IN,
       MOVE, RESIZE, MOVE_SELF, ROOM, QUERY, NESTED };
typedef struct { unsigned self, other; int target; } Pair;
typedef struct Fixture Fixture;
struct Fixture {
    Runner runner;
    VMContext vm;
    DataWin data;
    GameObject objects[OBJECTS];
    Sprite sprite;
    CodeEntry code;
    VMNativeCode native[1];
    CallFrame caller;
    IntIntHashMap codeLocals;
    Instance** byObject[OBJECTS];
    Instance** byExact[OBJECTS];
    int32_t* eventObjects[OBJT_EVENT_TYPE_COUNT];
    FlattenedCollisionEvent events[2];
    FlattenedCollisionEventList lists[OBJECTS];
    Instance pool[48];
    int used, action;
    bool acted;
    Instance* victim;
    Fixture* nested;
    Pair pairs[256];
    int pairCount;
};

static Instance* add(Fixture* f, int object, unsigned id) {
    assert(f->used < 48);
    Instance* inst = &f->pool[f->used++];
    inst->instanceId = 100000 + id;
    inst->objectIndex = object;
    inst->active = true;
    inst->maskIndex = -1;
    inst->imageXscale = inst->imageYscale = 1;
    inst->pathIndex = -1;
    arrput(f->runner.instances, inst);
    hmput(f->runner.instancesById, inst->instanceId, inst);
    Runner_addInstanceToObjectLists(&f->runner, inst);
    return inst;
}

static bool record(VMContext* ctx) {
    Fixture* f = (Fixture*) ctx->runner; /* Runner is the first member. */
    Instance* self = ctx->currentCallFrame->self;
    assert(ctx->currentCallFrame->eventType == EVENT_COLLISION);
    assert(ctx->currentCallFrame->eventObjectIndex == SELF);
    assert(f->pairCount < 256);
    f->pairs[f->pairCount++] = (Pair) {
        self->instanceId - 100000, ctx->currentCallFrame->other->instanceId - 100000,
        ctx->currentCallFrame->eventSubtype
    };
    if (f->acted) return true;
    f->acted = true;
    switch (f->action) {
    case CREATE:
        add(f, CHILD, 4);
        break;
    case DESTROY:
        Runner_destroyInstance(&f->runner, f->victim, false);
        assert(f->victim->destroyed && !f->victim->active);
        break;
    case DEACTIVATE:
        Runner_setActiveState(&f->runner, f->victim, false);
        assert(!f->victim->active && !f->victim->destroyed);
        break;
    case CHANGE_OUT:
    case CHANGE_IN:
        Runner_removeInstanceFromObjectLists(&f->runner, f->victim);
        f->victim->objectIndex = f->action == CHANGE_OUT ? UNRELATED : CHILD;
        Runner_addInstanceToObjectLists(&f->runner, f->victim);
        break;
    case MOVE:
        f->victim->x = 50;
        break;
    case RESIZE:
        f->victim->imageXscale = 2;
        break;
    case MOVE_SELF:
        self->x = 50;
        break;
    case ROOM:
        f->runner.pendingRoom = 1;
        break;
    case QUERY: {
        int32_t base = Runner_pushInstancesForTarget(&f->runner, PARENT);
        assert(arrlen(f->runner.instanceSnapshots) == base + 2);
        int32_t inner = Runner_pushInstancesForTarget(&f->runner, INSTANCE_ALL);
        assert(arrlen(f->runner.instanceSnapshots) == inner + 4);
        /* Force arena relocation while the outer dispatch owns live offsets. */
        arrsetcap(f->runner.instanceSnapshots, arrcap(f->runner.instanceSnapshots) + 1024);
        int32_t one = Runner_pushInstancesForTarget(&f->runner, 100002);
        assert(arrlen(f->runner.instanceSnapshots) == one + 1);
        assert(f->runner.instanceSnapshots[one]->instanceId == 100002);
        Runner_popInstanceSnapshot(&f->runner, one);
        Runner_popInstanceSnapshot(&f->runner, inner);
        Runner_popInstanceSnapshot(&f->runner, base);
        assert(arrlen(f->runner.instanceSnapshots) == base);
        break;
    }
    case NESTED:
        dispatchCollisionEvents(&f->nested->runner);
        assert(arrlen(f->nested->runner.instanceSnapshots) == 0);
        break;
    default: break;
    }
    return true;
}

static Fixture* fixture(void) {
    Fixture* f = calloc(1, sizeof(*f));
    assert(f);
    f->runner.dataWin = &f->data;
    f->runner.vmContext = &f->vm;
    f->runner.pendingRoom = -1;
    /* No Create/Destroy/Cleanup handlers in these synthetic objects. */
    for (int i = 0; i < OBJT_EVENT_TYPE_COUNT; ++i)
        f->runner.eventSlotMap.maxSubtypeByType[i] = -1;
    f->runner.instancesByObject = f->byObject;
    f->runner.instancesByExactObject = f->byExact;
    f->runner.objectsWithAnyEventOfType = f->eventObjects;
    f->runner.flattenedCollisionEvents = f->lists;
    f->runner.spatialGrid = SpatialGrid_create(128, 128);
    f->data.gen8.wadVersion = FIXTURE_WAD;
    f->data.objt.count = OBJECTS;
    f->data.objt.objects = f->objects;
    for (int i = 0; i < OBJECTS; ++i) f->objects[i].parentId = -1;
    f->objects[CHILD].parentId = PARENT;
    f->sprite.width = f->sprite.height = 10;
    f->sprite.bboxMode = 1;
    f->data.sprt.count = 1;
    f->data.sprt.sprites = &f->sprite;
    f->code.name = "collision fixture";
    f->data.code.count = 1;
    f->data.code.entries = &f->code;
    f->native[0] = record;
    f->vm.nativeCode = f->native;
    f->vm.codeLocalsSlotMaps = &f->codeLocals;
    f->vm.dataWin = &f->data;
    f->vm.runner = &f->runner;
    f->vm.currentCallFrame = &f->caller;
    f->vm.currentCallFrame->eventType = -12;
    f->vm.currentCallFrame->eventSubtype = -13;
    f->vm.currentCallFrame->eventObjectIndex = -14;
    f->events[0] = (FlattenedCollisionEvent) { PARENT, 0, SELF };
    f->events[1] = (FlattenedCollisionEvent) { CHILD, 0, SELF };
    f->lists[SELF].events = f->events;
    f->lists[SELF].eventCount = 1;
    arrput(f->eventObjects[EVENT_COLLISION], SELF);
    return f;
}

static void finish(Fixture* f) {
    for (int i = 0; i < OBJECTS; ++i) {
        arrfree(f->byObject[i]);
        arrfree(f->byExact[i]);
    }
    arrfree(f->eventObjects[EVENT_COLLISION]);
    arrfree(f->runner.instances);
    arrfree(f->runner.instanceSnapshots);
    hmfree(f->runner.instancesById);
    SpatialGrid_free(f->runner.spatialGrid);
    free(f);
}

static void run(Fixture* f, const Pair* want, int count) {
    /* A nonempty caller-owned prefix must survive every dispatch path. */
    arrput(f->runner.instanceSnapshots, &f->pool[0]);
    dispatchCollisionEvents(&f->runner);
    assert(arrlen(f->runner.instanceSnapshots) == 1);
    assert(f->runner.instanceSnapshots[0] == &f->pool[0]);
    Runner_popInstanceSnapshot(&f->runner, 0);
    assert(arrlen(f->runner.instanceSnapshots) == 0);
    assert(f->pairCount == count);
    for (int i = 0; i < count; ++i) {
        assert(f->pairs[i].self == want[i].self);
        assert(f->pairs[i].other == want[i].other);
        assert(f->pairs[i].target == want[i].target);
    }
    assert(f->vm.currentCallFrame == &f->caller);
    assert(f->vm.currentCallFrame->self == NULL && f->vm.currentCallFrame->other == NULL);
    assert(f->vm.currentCallFrame->eventType == -12 && f->vm.currentCallFrame->eventSubtype == -13);
    assert(f->vm.currentCallFrame->eventObjectIndex == -14);
}

static void ordering(void) {
    const Pair want[] = {
        {20, 8, PARENT}, {20, 9, PARENT}, {20, 2, PARENT}, {20, 3, PARENT},
        {10, 8, PARENT}, {10, 9, PARENT}, {10, 2, PARENT}, {10, 3, PARENT}
    };
    Fixture* f = fixture();
    add(f, SELF, 20); add(f, SELF, 10);
    add(f, CHILD, 3); add(f, PARENT, 9); add(f, CHILD, 2); add(f, PARENT, 8);
    run(f, want, 8);
    finish(f);
    /* A child-specific event wins over the ancestor target, without doubles. */
    const Pair specific[] = { {10, 8, PARENT}, {10, 2, CHILD} };
    f = fixture(); f->lists[SELF].eventCount = 2;
    add(f, SELF, 10); add(f, CHILD, 2); add(f, PARENT, 8);
    run(f, specific, 2);
    finish(f);
}

static void mutations(void) {
    const Pair unchanged[] = {{10, 1, PARENT}, {10, 2, PARENT}, {20, 1, PARENT}, {20, 2, PARENT}};
    const Pair removed[] = {{10, 1, PARENT}, {20, 1, PARENT}};
    const Pair created[] = {{10, 1, PARENT}, {10, 2, PARENT}, {20, 1, PARENT}, {20, 2, PARENT}, {20, 4, PARENT}};
    const Pair changedIn[] = {{10, 1, PARENT}, {20, 1, PARENT}, {20, 2, PARENT}};
    const Pair movedSelf[] = {{10, 1, PARENT}, {20, 1, PARENT}, {20, 2, PARENT}};
    const Pair room[] = {{10, 1, PARENT}};
    for (int action = CREATE; action <= QUERY; ++action) {
        Fixture* f = fixture(); f->action = action;
        add(f, SELF, 10); add(f, SELF, 20); add(f, PARENT, 1);
        f->victim = add(f, action == CHANGE_IN ? UNRELATED : CHILD, 2);
        if (action == RESIZE) f->victim->x = -15; /* 10 wide misses; 20 wide hits. */
        const Pair* want = unchanged; int count = 4;
        if (action == CREATE) { want = created; count = 5; }
        if (action == DESTROY || action == DEACTIVATE || action == CHANGE_OUT || action == MOVE) {
            want = removed; count = 2;
        }
        if (action == CHANGE_IN) { want = changedIn; count = 3; }
        if (action == MOVE_SELF) { want = movedSelf; count = 3; }
        if (action == ROOM) { want = room; count = 1; }
        run(f, want, count);
        finish(f);
    }
}

#ifdef OF_COLLISION_WORK_PROFILE
static RunnerCollisionWork take(Fixture* f) {
    RunnerCollisionWork got, cleared;
    const RunnerCollisionWork zero = {0};
    Runner_takeCollisionWork(&f->runner, &got);
    Runner_takeCollisionWork(&f->runner, &cleared);
    assert(memcmp(&cleared, &zero, sizeof(zero)) == 0);
    return got;
}

static void counts(void) {
    Fixture* empty = fixture();
    run(empty, NULL, 0);
    RunnerCollisionWork noWork = take(empty);
    const RunnerCollisionWork zero = {0};
    assert(memcmp(&noWork, &zero, sizeof(zero)) == 0);
    finish(empty);
    const int lengths[] = {0, 1, 2, 7, 8, 31, 32};
    const int bins[] = {0, 1, 2, 2, 3, 3, 4};
    for (int c = 0; c < 7; ++c) {
        Fixture* f = fixture();
        add(f, SELF, 10);
        /* Misses deliberately reuse the self bbox; target bboxes never cache. */
        for (int i = lengths[c]; i > 0; --i) add(f, PARENT, (unsigned)i)->x = 50;
        run(f, NULL, 0);
        RunnerCollisionWork got = take(f);
        assert(got.sorts == 1 && got.targets == (uint64_t)lengths[c]);
        assert(got.selfBoxes == (uint64_t)(lengths[c] != 0));
        assert(got.otherBoxes == (uint64_t)lengths[c]);
        if (lengths[c] < 2) assert(got.comparisons == 0);
        else assert(got.comparisons > 0); /* libc controls the exact qsort count. */
        for (int b = 0; b < 5; ++b) assert(got.targetLengths[b] == (uint64_t)(b == bins[c]));
        finish(f);
    }
    Fixture* f = fixture();
    add(f, SELF, 10)->solid = true;
    add(f, PARENT, 1);
    f->runner.collisionCompatibilityMode = true;
    const Pair want[] = {{10, 1, PARENT}};
    run(f, want, 1);
    RunnerCollisionWork got = take(f);
    assert(got.selfBoxes == 2 && got.otherBoxes == 2);
    finish(f);
}
#endif

static void nested(void) {
    Fixture* outer = fixture(); Fixture* inner = fixture();
    outer->action = NESTED; outer->nested = inner;
    add(outer, SELF, 10); add(outer, SELF, 20);
    add(outer, CHILD, 2); add(outer, PARENT, 1);
    add(inner, SELF, 30); add(inner, PARENT, 3);
    const Pair want[] = {{10, 1, PARENT}, {10, 2, PARENT}, {20, 1, PARENT}, {20, 2, PARENT}};
#ifdef OF_COLLISION_WORK_PROFILE
    /* Even a preexisting comparator context must survive both dispatches. */
    collisionSortRunner = &inner->runner;
#endif
    run(outer, want, 4);
#ifdef OF_COLLISION_WORK_PROFILE
    assert(collisionSortRunner == &inner->runner);
    collisionSortRunner = NULL;
#endif
    assert(inner->pairCount == 1 && inner->pairs[0].self == 30 && inner->pairs[0].other == 3);
#ifdef OF_COLLISION_WORK_PROFILE
    RunnerCollisionWork a = take(outer), b = take(inner);
    assert(a.sorts == 2 && a.targets == 4 && a.comparisons > 0);
    assert(a.selfBoxes == 4 && a.otherBoxes == 4 && a.targetLengths[2] == 2);
    assert(b.sorts == 1 && b.targets == 1 && b.comparisons == 0);
    assert(b.selfBoxes == 1 && b.otherBoxes == 1 && b.targetLengths[1] == 1);
#endif
    finish(inner); finish(outer);
}

int main(void) {
    ordering(); mutations(); nested();
#ifdef OF_COLLISION_WORK_PROFILE
    counts();
#endif
    printf("PASS: collision ordering, mutation visibility, nested arenas (WAD%d)\n", FIXTURE_WAD);
    return 0;
}
