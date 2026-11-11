/* Asset-free differential checks of the real legacy drawable builder.
 * The rank oracle shares neither the grouping algorithm nor its comparator. */
#include "../../runner.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned qsortCalls, insertionCases, fallbackCases, guardCases;
static void observedQsort(void *base, size_t count, size_t size,
                          int (*compare)(const void *, const void *)) {
    ++qsortCalls;
    qsort(base, count, size, compare);
}
#define qsort observedQsort
#include "../../runner.c"
#undef qsort

enum { CAPACITY = 257 };
typedef struct {
    Runner runner;
    DataWin data;
    Room room;
    Instance pool[CAPACITY];
} DrawableFixture;

static uint32_t randomState = 0xc91d3e75u;
static uint32_t nextRandom(void) {
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;
    return randomState;
}

static int64_t signedId(const Instance *inst) {
    uint32_t id = inst->instanceId;
    return id <= INT32_MAX ? (int64_t)id : (int64_t)id - 4294967296LL;
}

static void resetFixture(DrawableFixture *f, int count) {
    arrsetlen(f->runner.instances, 0);
    arrsetlen(f->runner.particleSystemPool, 0);
    arrsetlen(f->runner.cachedDrawables, CAPACITY);
    memset(f->runner.cachedDrawables, 0xa5, CAPACITY * sizeof(Drawable));
    f->runner.dataWin = &f->data;
    f->runner.currentRoom = &f->room;
    f->data.detectedFormat.major = 1;
    f->data.detectedFormat.minor = 4;
    f->room.tileCount = 0;
    f->runner.drawableListStructureDirty = true;
    f->runner.drawableListSortDirty = true;
    for (int i = 0; i < count; i++) {
        memset(&f->pool[i], 0, sizeof(f->pool[i]));
        f->pool[i].instanceId = (uint32_t)i;
        f->pool[i].layer = -1;
        arrput(f->runner.instances, &f->pool[i]);
    }
    qsortCalls = 0;
}

static unsigned inversions(const Runner *runner) {
    unsigned count = 0;
    for (int i = 0; i < arrlen(runner->instances); i++)
        for (int j = 0; j < i; j++)
            if (runner->instances[i]->depth == runner->instances[j]->depth &&
                signedId(runner->instances[j]) > signedId(runner->instances[i])) ++count;
    return count;
}

static void checkOrder(const Runner *runner) {
    Instance *expected[CAPACITY] = {0};
    int count = (int)arrlen(runner->instances);
    require(arrlen(runner->cachedDrawables) == count);
    for (int i = 0; i < count; i++) {
        Instance *inst = runner->instances[i];
        int rank = 0;
        for (int j = 0; j < count; j++) {
            Instance *other = runner->instances[j];
            if (other->depth > inst->depth ||
                (other->depth == inst->depth && signedId(other) < signedId(inst))) ++rank;
        }
        require(rank < count && expected[rank] == NULL);
        expected[rank] = inst;
    }
    for (int i = 0; i < count; i++) {
        const Drawable *d = &runner->cachedDrawables[i];
        require(d->type == DRAWABLE_INSTANCE && d->instance == expected[i]);
        require(d->depth == expected[i]->depth);
        require(d->layerOrder == 0 && d->elementOrder == 0);
    }
}

static void checkFast(DrawableFixture *f) {
    unsigned shifts = inversions(&f->runner);
    require(rebuildLegacyInstanceDrawables(&f->runner));
    require(qsortCalls == (unsigned)(shifts >= 256));
    if (shifts >= 256) ++fallbackCases;
    else ++insertionCases;
    checkOrder(&f->runner);
}

static void checkGuard(DrawableFixture *f) {
    Drawable before[CAPACITY];
    memcpy(before, f->runner.cachedDrawables, sizeof(before));
    require(!rebuildLegacyInstanceDrawables(&f->runner));
    require(qsortCalls == 0 && arrlen(f->runner.cachedDrawables) == CAPACITY);
    require(!memcmp(before, f->runner.cachedDrawables, sizeof(before)));
    ++guardCases;
}

static void literalCases(DrawableFixture *f) {
    const uint32_t ids[] = {0, UINT32_MAX, 17, 0x80000000u, INT32_MAX, 0x80000001u, 1, 0xfffffffeu};
    resetFixture(f, 8);
    for (int i = 0; i < 8; i++) f->pool[i].instanceId = ids[i];
    checkFast(f); /* Signed IDs cross zero in an insertion-only group. */

    for (int zeroSlot = 2; zeroSlot <= 3; zeroSlot++) {
        resetFixture(f, 24);
        for (int i = 0, value = 23; i < 24; i++)
            f->pool[i].instanceId = i == zeroSlot ? 0 : (uint32_t)value--;
        require(inversions(&f->runner) == (unsigned)(253 + zeroSlot));
        checkFast(f); /* 255 shifts stays in insertion; 256 invokes qsort. */
    }
    resetFixture(f, 256);
    for (int i = 0; i < 256; i++) f->pool[i].instanceId = (uint32_t)(255 - i);
    checkFast(f);
    resetFixture(f, 8);
    for (int i = 0; i < 8; i++) f->pool[i].depth = INT32_MIN + 64;
    f->pool[0].depth = INT32_MIN;
    f->pool[7].depth = INT32_MIN + 127;
    checkFast(f);
    resetFixture(f, 8);
    f->pool[0].depth = INT32_MAX;
    f->pool[7].depth = INT32_MAX - 127;
    for (int i = 1; i < 7; i++) f->pool[i].depth = INT32_MAX - 64;
    checkFast(f);
}

static void randomCases(DrawableFixture *f) {
    for (int scenario = 0; scenario < 2000; scenario++) {
        int count = 8 + (int)(nextRandom() % 249);
        int range = 1 + (int)(nextRandom() % 128);
        int32_t base = scenario % 3 == 0 ? INT32_MIN :
                       scenario % 3 == 1 ? INT32_MAX - range : -range / 2;
        uint32_t firstId = nextRandom();
        resetFixture(f, count);
        for (int i = 0; i < count; i++) {
            f->pool[i].depth = base + (int32_t)(nextRandom() % (unsigned)range);
            f->pool[i].instanceId = firstId + (uint32_t)i * 0x9e3779b9u;
        }
        for (int i = count - 1; i > 0; i--) {
            int j = (int)(nextRandom() % (unsigned)(i + 1));
            Instance *saved = f->runner.instances[i];
            f->runner.instances[i] = f->runner.instances[j];
            f->runner.instances[j] = saved;
        }
        checkFast(f);
    }
}

static void guardCasesAndCaller(DrawableFixture *f) {
    const int counts[] = {0, 7, 257};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        resetFixture(f, counts[i]);
        checkGuard(f);
    }
    resetFixture(f, 8);
    f->pool[0].depth = 128;
    checkGuard(f);
    resetFixture(f, 8);
    f->pool[0].depth = INT32_MIN;
    f->pool[7].depth = INT32_MAX;
    checkGuard(f);
    for (int flags = 0; flags < 4; flags++) {
        resetFixture(f, 8);
        ParticleSystem system = {0};
        system.used = (flags & 1) != 0;
        system.automaticDraw = (flags & 2) != 0;
        arrput(f->runner.particleSystemPool, system);
        if (flags == 3) checkGuard(f);
        else checkFast(f);
    }

    resetFixture(f, 8);
    rebuildDrawableCacheIfDirty(&f->runner);
    require(qsortCalls == 0);
    require(!f->runner.drawableListStructureDirty && !f->runner.drawableListSortDirty);
    checkOrder(&f->runner);

    resetFixture(f, 8);
    RoomTile tile = {0};
    tile.tileDepth = 100;
    f->room.tiles = &tile;
    f->room.tileCount = 1;
    rebuildDrawableCacheIfDirty(&f->runner);
    require(qsortCalls == 1 && arrlen(f->runner.cachedDrawables) == 9);
    require(f->runner.cachedDrawables[0].type == DRAWABLE_TILE);
    require(f->runner.cachedDrawables[0].tileIndex == 0);
    require(f->runner.cachedDrawables[0].depth == 100);

    resetFixture(f, 8);
    f->data.detectedFormat.major = 2;
    rebuildDrawableCacheIfDirty(&f->runner);
    require(qsortCalls == 1); /* GMS2 stays with its layer-aware general builder. */
    checkOrder(&f->runner);
}

int main(void) {
    DrawableFixture *f = (DrawableFixture *)calloc(1, sizeof(*f));
    require(f != NULL);
    literalCases(f);
    randomCases(f);
    guardCasesAndCaller(f);
    require(insertionCases > 0 && fallbackCases > 0 && guardCases == 6);
    arrfree(f->runner.instances);
    arrfree(f->runner.cachedDrawables);
    arrfree(f->runner.particleSystemPool);
    free(f);
    printf("PASS: drawable rank parity, zero fields, %u insertion cases, %u qsort fallbacks, %u guards\n",
           insertionCases, fallbackCases, guardCases);
    return 0;
}
