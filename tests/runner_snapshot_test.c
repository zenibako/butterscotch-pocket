/* Core-only synthetic fixture for the Undertale (WAD16) snapshot contract. */
#include "runner_snapshot.h"
#include "stb_ds.h"
#include "log.h"
#include "spatial_grid.h"
#include "miniz.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Host adapter for the core test, with no Pocket platform dependency. */
void platformLog(logType type, const char *format, va_list args) {
    if (type >= LOG_TYPE_WARNING) vfprintf(stderr, format, args);
}

static bool noSurface(Renderer *renderer, int32_t id) {
    (void)renderer; (void)id;
    return false;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t hashBytes(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static uint8_t *decodeRaw(const uint8_t *bytes, size_t size, uint32_t *rawLenOut) {
    uint32_t rawLen = le32(bytes + 24);
    uint8_t *raw = malloc(rawLen);
    assert(raw);
    if (le32(bytes + 4) >= 4) {
        assert(ZSTD_decompress(raw, rawLen, bytes + 32, size - 32) == rawLen);
    } else {
        mz_ulong n = rawLen;
        assert(le32(bytes + 4) == 3);
        assert(mz_uncompress(raw, &n, bytes + 32, size - 32) == MZ_OK && n == rawLen);
    }
    *rawLenOut = rawLen;
    return raw;
}
static void encodeRaw(uint8_t **bytes, size_t *size, const uint8_t *raw,
                      uint32_t rawLen, uint32_t version) {
    size_t cap = version >= 4 ? ZSTD_compressBound(rawLen) : mz_compressBound(rawLen);
    uint8_t *out = malloc(cap + 32);
    assert(out);
    if (version >= 4) {
        cap = ZSTD_compress(out + 32, cap, raw, rawLen, 1);
        assert(!ZSTD_isError(cap));
    } else {
        mz_ulong n = cap;
        assert(mz_compress2(out + 32, &n, raw, rawLen, MZ_DEFAULT_COMPRESSION) == MZ_OK);
        cap = n;
    }
    memcpy(out, *bytes, 32);
    write32(out + 4, version);
    write32(out + 16, (uint32_t)cap + 8);
    write32(out + 24, rawLen);
    write32(out + 28, hashBytes(raw, rawLen));
    write32(out + 20, hashBytes(out + 24, cap + 8));
    free(*bytes); *bytes = out; *size = cap + 32;
}
static size_t roomBlock(const uint8_t *p, size_t n, uint32_t rooms) {
    for (size_t i = 0; i + 14 < n; i++)
        if (le32(p + i) == rooms && le32(p + i + 4 + rooms) == 320 &&
            le32(p + i + 8 + rooms) == 240) return i;
    assert(false); return 0;
}
static void repack(uint8_t **bytes, size_t *size,
                   void (*change)(uint8_t *, size_t)) {
    uint32_t rawLen;
    uint8_t *raw = decodeRaw(*bytes, *size, &rawLen);
    change(raw, rawLen);
    encodeRaw(bytes, size, raw, rawLen, le32(*bytes + 4));
    free(raw);
}
static void invalidRoomFlag(uint8_t *raw, size_t n) {
    size_t at = roomBlock(raw, n, 1);
    raw[at + 4] = 2;
}
static void hugeRoom(uint8_t *raw, size_t n) {
    size_t at = roomBlock(raw, n, 1);
    write32(raw + at + 5, 4194240);
    write32(raw + at + 9, 4194240);
}
static void invalidArrayCountAfterRooms(uint8_t *raw, size_t n) {
    size_t at = roomBlock(raw, n, 2);
    write32(raw + at + 4 + 2 + 2 * 18, 65537);
}
static void oversizedArrayShape(uint8_t *raw, size_t n) {
    for (size_t i = 0; i + 20 < n; i++) {
        if (le32(raw + i + 4) == 3 && le32(raw + i + 8) == 32000 &&
            le32(raw + i + 12) == 32000 && le32(raw + i + 16) == 1) {
            write32(raw + i + 16, 32000);
            return;
        }
    }
    assert(false);
}
static void invalidIniMagic(uint8_t *raw, size_t n) {
    for (size_t i = n - 8; i > 0; i--)
        if (le32(raw + i) == 0x31494e49u) {
            write32(raw + i, 0);
            return;
        }
    assert(false);
}
/* Revision 9 adds epsilon immediately after the unchanged RunnerPlain POD. */
static void toRevision8(uint8_t *raw, uint32_t *rawLen, uint32_t rooms) {
    size_t at = roomBlock(raw, *rawLen, rooms) - sizeof(GMLReal);
    memmove(raw + at, raw + at + sizeof(GMLReal), *rawLen - at - sizeof(GMLReal));
    *rawLen -= sizeof(GMLReal);
}
/* A zero-tilemap fixture has one revision-8 count before the vertex slots. */
static void toRevision7(uint8_t *raw, uint32_t *rawLen, uint32_t rooms) {
    toRevision8(raw, rawLen, rooms);
    size_t ini = *rawLen - 8;
    while (ini > 0 && le32(raw + ini) != 0x31494e49u) ini--;
    assert(ini >= 68 && le32(raw + ini - 56) == 0);
    memmove(raw + ini - 56, raw + ini - 52, *rawLen - (ini - 52));
    *rawLen -= 4;
}
/* Revision 7 added a layout byte (and, for GMS 2.3 games, an owner token) to
 * each array, the method and struct tables after the arrays, and the static
 * block flags, runtime layers, legacy tile layers and vertex resource slots
 * before the dormant pools. Strip them from a WAD 16 fixture's
 * body that has no methods, structs or static flags. */
static void toRevision6(uint8_t *raw, uint32_t *rawLen, uint32_t rooms) {
    toRevision7(raw, rawLen, rooms);
    size_t at = roomBlock(raw, *rawLen, rooms) + 4 + rooms;
    for (uint32_t i = 0; i < rooms; i++)
        at += 12 + 2 + 4 + (size_t)le32(raw + at + 14) * sizeof(RoomTile);
    uint32_t arrays = le32(raw + at); at += 4;
    for (uint32_t i = 0; i < arrays; i++) {
        assert(raw[at] == 0); /* legacy layout */
        memmove(raw + at, raw + at + 1, *rawLen - at - 1); *rawLen -= 1;
        at += 8 + 4 * (size_t)le32(raw + at + 4);
    }
    /* at: tile layer count, preceded by the method and struct counts */
    assert(le32(raw + at) == 0 && le32(raw + at + 4) == 0);
    memmove(raw + at, raw + at + 8, *rawLen - at - 8); *rawLen -= 8;
    size_t ini = *rawLen - 8;
    while (ini > 0 && le32(raw + ini) != 0x31494e49u) ini--;
    /* static flags, runtime layers, legacy tile layers, vertex format and
     * buffer slots: five zero counts before the dormant pools */
    assert(ini >= 64);
    for (size_t k = 64; k > 44; k -= 4) assert(le32(raw + ini - k) == 0);
    memmove(raw + ini - 64, raw + ini - 44, *rawLen - (ini - 44)); *rawLen -= 20;
}
/* Revision 6 added the saved persistent rooms after the live instances.
 * Older revisions lack that count; drop it from a body with no instances. */
static void dropSavedRooms(uint8_t *raw, uint32_t *rawLen, uint32_t rooms) {
    size_t at = roomBlock(raw, *rawLen, rooms) + 4 + rooms;
    for (uint32_t i = 0; i < rooms; i++)
        at += 12 + 2 + 4 + (size_t)le32(raw + at + 14) * sizeof(RoomTile);
    uint32_t arrays = le32(raw + at); at += 4;
    for (uint32_t i = 0; i < arrays; i++) at += 8 + 4 * (size_t)le32(raw + at + 4);
    at += 4 + (size_t)le32(raw + at) * sizeof(TileLayerMapEntry);
    assert(le32(raw + at) == 0); at += 4;   /* live instances */
    assert(le32(raw + at) == 0);            /* saved rooms */
    memmove(raw + at, raw + at + 4, *rawLen - at - 4);
    *rawLen -= 4;
}
static void removeIniAndMaybeDormantTail(uint8_t **bytes, size_t *size,
                                         size_t drop) {
    uint32_t rawLen;
    uint8_t *raw = decodeRaw(*bytes, *size, &rawLen);
    assert(drop <= rawLen);
    rawLen -= (uint32_t)drop;
    encodeRaw(bytes, size, raw, rawLen, le32(*bytes + 4));
    free(raw);
}

static void fixture(Runner *r, DataWin *dw, VMContext *vm, Renderer *renderer,
                    RendererVtable *vtable, Room *room, GameObject *object) {
    memset(r, 0, sizeof(*r));
    memset(dw, 0, sizeof(*dw));
    memset(vm, 0, sizeof(*vm));
    memset(renderer, 0, sizeof(*renderer));
    memset(vtable, 0, sizeof(*vtable));
    memset(room, 0, sizeof(*room));
    memset(object, 0, sizeof(*object));
    dw->gen8.wadVersion = 16;
    dw->room.rooms = room;
    dw->room.count = 1;
    dw->objt.objects = object;
    dw->objt.count = 1;
    room->width = 320;
    room->height = 240;
    room->payloadLoaded = true;
    r->dataWin = dw;
    r->vmContext = vm;
    r->renderer = renderer;
    r->currentRoom = room;
    r->currentRoomIndex = 0;
    r->currentRoomOrderPosition = 0;
    r->pendingRoom = -1;
    r->nextInstanceId = 100003;
    r->frameCount = 42;
    r->gameStartFired = true;
    r->mathEpsilon = GML_MATH_EPSILON;
    r->spatialGrid = SpatialGrid_create(room->width, room->height);
    r->instancesByObject = calloc(1, sizeof(Instance **));
    r->instancesByExactObject = calloc(1, sizeof(Instance **));
    vm->runner = r;
    vm->dataWin = dw;
    vm->globalScopeInstance = Instance_create(0, STRUCT_OBJECT_INDEX, 0, 0);
    vtable->surfaceExists = noSurface;
    renderer->vtable = vtable;
    renderer->runner = r;
}

static void destroyFixture(Runner *r) {
    Instance_free(r->vmContext->globalScopeInstance);
    for (ptrdiff_t i = 0; i < arrlen(r->instances); i++) Instance_free(r->instances[i]);
    arrfree(r->instances);
    for (ptrdiff_t i = 0; i < arrlen(r->dsMapPool); i++) {
        for (ptrdiff_t j = 0; j < shlen(r->dsMapPool[i]); j++) {
            free(r->dsMapPool[i][j].key);
            RValue_free(&r->dsMapPool[i][j].value);
        }
        shfree(r->dsMapPool[i]);
    }
    arrfree(r->dsMapPool);
    for (ptrdiff_t i = 0; i < arrlen(r->dsListPool); i++) {
        for (ptrdiff_t j = 0; j < arrlen(r->dsListPool[i].items); j++)
            RValue_free(&r->dsListPool[i].items[j]);
        arrfree(r->dsListPool[i].items);
    }
    arrfree(r->dsListPool);
    arrfree(r->dsQueuePool);
    arrfree(r->dsStackPool); arrfree(r->dsPriorityPool); arrfree(r->dsGridPool);
    arrfree(r->gmlBufferPool); arrfree(r->mpGridPool);
    arrfree(r->particleSystemPool); arrfree(r->particleTypePool);
    arrfree(r->audioEmitters); arrfree(r->vertexFormats); arrfree(r->vertexBuffers);
    for (ptrdiff_t i = 0; i < shlen(r->vmContext->varNameMap); i++)
        free(r->vmContext->varNameMap[i].key);
    shfree(r->vmContext->varNameMap);
    free(r->currentRoom->tiles);
    hmfree(r->tileLayerMap);
    hmfree(r->instancesById);
    for (uint32_t i = 0; i < r->dataWin->objt.count; i++) {
        arrfree(r->instancesByObject[i]);
        arrfree(r->instancesByExactObject[i]);
    }
    free(r->instancesByObject);
    free(r->instancesByExactObject);
    SpatialGrid_free(r->spatialGrid);
    Ini_free(r->currentIni); free(r->currentIniPath);
    Ini_free(r->cachedIni); free(r->cachedIniPath);
    /* Registry-owned structs can form cycles through arrays. Release all
     * contents before their shells, as in the runner's teardown. */
    for (ptrdiff_t i = 0; i < arrlen(r->structInstances); i++) {
        r->structInstances[i]->structRegistryIndex = -1;
        Instance_freeContents(r->structInstances[i]);
    }
    for (ptrdiff_t i = 0; i < arrlen(r->structInstances); i++) free(r->structInstances[i]);
    arrfree(r->structInstances);
    arrfree(r->eventDispatchInstances);
    arrfree(r->cachedDrawables);
}

static void invalidDestroyedActive(uint8_t *raw, size_t size) {
    size_t active = 12 + offsetof(Instance, active) - offsetof(Instance, x);
    size_t destroyed = 12 + offsetof(Instance, destroyed) - offsetof(Instance, x);
    for (size_t i = 0; i + destroyed < size; i++) {
        if (le32(raw + i) != 100002) continue;
        assert(raw[i + destroyed] == 1 && raw[i + active] == 0);
        raw[i + active] = 1;
        return;
    }
    assert(false);
}

static void testDestroyedInstanceAwaitingCleanup(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    object.parentId = -1;
    Instance *first = Instance_create(100001, 0, 11, 22);
    Instance *dead = Instance_create(100002, 0, 33, 44);
    Instance *last = Instance_create(100003, 0, 55, 66);
    dead->destroyed = true; dead->active = false; dead->cleanupEventFired = true;
    dead->layer = 55;
    arrput(r.instances, first); arrput(r.instances, dead); arrput(r.instances, last);
    for (ptrdiff_t i = 0; i < arrlen(r.instances); i++) {
        hmput(r.instancesById, r.instances[i]->instanceId, r.instances[i]);
        Runner_addInstanceToObjectLists(&r, r.instances[i]);
    }
    GMLArray *shared = GMLArray_create(&dw, 1);
    GMLArray_set(shared, 0, RValue_makeReal(777));
    Instance_setSelfVar(first, 3, RValue_makeArray(shared));
    Instance_setSelfVar(dead, 3, RValue_makeArray(shared));
    shared->owner = IntRValueHashMap_findSlot(&dead->selfVars, 3);
    GMLArray_decRef(shared);
    RuntimeLayer layer = {0}; layer.id = 55;
    RuntimeLayerElement element = {0};
    element.id = 56; element.type = RuntimeLayerElementType_Instance; element.instanceId = dead->instanceId;
    arrput(layer.elements, element); arrput(r.runtimeLayers, layer); r.nextLayerId = 57;
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCheck(&r) == NULL);
    assert(Runner_snapshotCapture(&r, 95, &bytes, &size, &reason));
    size_t invalidSize = size;
    uint8_t *invalid = malloc(size); assert(invalid); memcpy(invalid, bytes, size);
    repack(&invalid, &invalidSize, invalidDestroyedActive);
    assert(Runner_snapshotPrepare(&r, 95, invalid, invalidSize, &reason) == NULL);
    free(invalid);
    /* Capture and preparation preserve the pending cleanup and its owner. */
    assert(arrlen(r.instances) == 3 && r.instances[1] == dead);
    RunnerSnapshot *prepared = Runner_snapshotPrepare(&r, 95, bytes, size, &reason);
    assert(prepared && r.instances[1] == dead);
    Runner_snapshotApply(&r, prepared); Runner_snapshotFree(prepared);
    dead = r.instances[1];
    assert(arrlen(r.instances) == 3 && dead->instanceId == 100002);
    assert(dead->destroyed && !dead->active && dead->cleanupEventFired);
    int32_t deadId = 100002;
    assert(hmget(r.instancesById, deadId) == dead);
    assert(arrlen(r.instancesByObject[0]) == 3 && r.instancesByObject[0][1] == dead);
    assert(arrlen(r.instancesByExactObject[0]) == 3 && r.instancesByExactObject[0][1] == dead);
    RValue liveArray = Instance_getSelfVar(r.instances[0], 3);
    RValue deadArray = Instance_getSelfVar(dead, 3);
    assert(liveArray.array == deadArray.array);
    assert(liveArray.array->owner == IntRValueHashMap_findSlot(&dead->selfVars, 3));
    dead->active = true; assert(Runner_snapshotCheck(&r) != NULL); dead->active = false;
    dead->cleanupEventFired = false; assert(Runner_snapshotCheck(&r) != NULL); dead->cleanupEventFired = true;
    Runner_cleanupDestroyedInstances(&r);
    assert(arrlen(r.instances) == 2 && r.instances[0]->instanceId == 100001 && r.instances[1]->instanceId == 100003);
    assert(hmgeti(r.instancesById, deadId) < 0);
    assert(arrlen(r.instancesByObject[0]) == 2 && arrlen(r.instancesByExactObject[0]) == 2);
    assert(arrlen(r.runtimeLayers[0].elements) == 0);
    assert(GMLArray_get(liveArray.array, 0).real == 777);
    Runner_cleanupDestroyedInstances(&r); assert(arrlen(r.instances) == 2);
    Runner_freeRuntimeLayer(&r.runtimeLayers[0]); arrfree(r.runtimeLayers);
    free(bytes); destroyFixture(&r);
}

static void testRoundTrip(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    Instance *first = Instance_create(100001, 0, 11, 22);
    Instance *second = Instance_create(100002, 0, 33, 44);
    first->alarm[3] = 9;
    second->alarm[5] = 17;
    arrput(r.instances, first);
    arrput(r.instances, second);
    GMLArray *shared = GMLArray_create(&dw, 2);
    GMLArray_set(shared, 0, RValue_makeReal(73));
    Instance_setSelfVar(first, 10, RValue_makeArray(shared));
    Instance_setSelfVar(second, 11, RValue_makeArray(shared));
    Instance_setSelfVar(vm.globalScopeInstance, 12, RValue_makeReal(91));
    shput(vm.varNameMap, strdup("custom_state"), 12);
    vm.nextDynamicVarID = 130;
    DsMapEntry *map = NULL;
    shput(map, strdup("flag"), RValue_makeBool(true));
    arrput(r.dsMapPool, map);
    DsList list = {0};
    arrput(list.items, RValue_makeReal(17));
    arrput(r.dsListPool, list);
    room.tileCount = 1;
    room.tiles = calloc(1, sizeof(RoomTile));
    room.tiles[0].instanceID = 400;
    room.tiles[0].x = 19;
    RValue *owner = IntRValueHashMap_findSlot(&first->selfVars, 10);
    shared->owner = owner;
    GMLArray_decRef(shared);
    r.random = Random_create(123);
    uint32_t expectedRandom = r.random.state[0];
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCheck(&r) == NULL);
    assert(Runner_snapshotCapture(&r, 0x1234, &bytes, &size, &reason));
    assert(bytes && size > 0);
    RunnerSnapshot *prepared = Runner_snapshotPrepare(&r, 0x1234, bytes, size, &reason);
    assert(prepared);
    first->x = 700;
    first->alarm[3] = -1;
    r.frameCount = 100;
    r.random.state[0] = 0;
    vm.nextDynamicVarID = 900;
    room.tiles[0].x = 900;
    r.dsMapPool[0][0].value.int32 = 0;
    r.dsListPool[0].items[0].real = 900;
    Runner_snapshotApply(&r, prepared);
    Runner_snapshotFree(prepared);
    assert(r.frameCount == 42 && r.nextInstanceId == 100003);
    assert(r.instances[0]->instanceId == 100001 && r.instances[1]->instanceId == 100002);
    assert(r.instances[0]->x == 11 && r.instances[0]->alarm[3] == 9);
    assert(r.instances[1]->alarm[5] == 17 && r.random.state[0] == expectedRandom);
    RValue a = Instance_getSelfVar(r.instances[0], 10);
    RValue b = Instance_getSelfVar(r.instances[1], 11);
    assert(a.type == RVALUE_ARRAY && b.type == RVALUE_ARRAY && a.array == b.array);
    assert(a.array->owner == IntRValueHashMap_findSlot(&r.instances[0]->selfVars, 10));
    assert(GMLArray_get(a.array, 0).real == 73);
    assert(Instance_getSelfVar(vm.globalScopeInstance, 12).real == 91);
    assert(vm.nextDynamicVarID == 130);
    assert(shget(vm.varNameMap, "custom_state") == 12);
    assert(r.dsMapPool[0][0].value.int32 == 1);
    assert(r.dsListPool[0].items[0].real == 17);
    assert(room.tiles[0].x == 19);
    free(bytes);
    destroyFixture(&r);
}

static void testComparisonToleranceRoundTrip(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    r.mathEpsilon = (GMLReal)0.125;
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 96, &bytes, &size, &reason));
    assert(le32(bytes + 4) == 9);
    r.mathEpsilon = (GMLReal)0.5;
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 96, bytes, size, &reason);
    assert(s && r.mathEpsilon == (GMLReal)0.5);
    Runner_snapshotFree(s);
    assert(r.mathEpsilon == (GMLReal)0.5);
    s = Runner_snapshotPrepare(&r, 96, bytes, size, &reason);
    assert(s);
    Runner_snapshotApply(&r, s); Runner_snapshotFree(s);
    assert(r.mathEpsilon == (GMLReal)0.125);
    free(bytes); destroyFixture(&r);
}

static void testInvalidComparisonTolerance(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    r.mathEpsilon = (GMLReal)0.125;
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 97, &bytes, &size, &reason));
    uint32_t rawLen;
    uint8_t *raw = decodeRaw(bytes, size, &rawLen);
    size_t at = roomBlock(raw, rawLen, 1) - sizeof(GMLReal);
    GMLReal invalid[] = {(GMLReal)NAN, (GMLReal)INFINITY, (GMLReal)-1, (GMLReal)1};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        memcpy(raw + at, &invalid[i], sizeof(invalid[i]));
        /* Repair checksums so rejection exercises the scalar validation. */
        encodeRaw(&bytes, &size, raw, rawLen, 9);
        assert(Runner_snapshotPrepare(&r, 97, bytes, size, &reason) == NULL);
        assert(r.mathEpsilon == (GMLReal)0.125 && r.frameCount == 42);
    }
    free(raw); free(bytes); destroyFixture(&r);
}

static void testRejectsBadInputAndLiveResources(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    arrsetcap(r.dsQueuePool, 4); /* Allocated capacity without a live queue. */
    arrput(r.eventDispatchInstances, NULL); /* Scratch survives completed dispatch. */
    assert(Runner_snapshotCheck(&r) == NULL);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 7, &bytes, &size, &reason));
    assert(Runner_snapshotPrepare(&r, 8, bytes, size, &reason) == NULL);
    assert(Runner_snapshotPrepare(&r, 7, bytes, size - 1, &reason) == NULL);
    bytes[size / 2] ^= 0x80;
    assert(Runner_snapshotPrepare(&r, 7, bytes, size, &reason) == NULL);
    assert(r.frameCount == 42);
    free(bytes);
    r.openTextFiles[0].isOpen = true;
    assert(Runner_snapshotCheck(&r) != NULL);
    assert(!Runner_snapshotCapture(&r, 7, &bytes, &size, &reason));
    assert(reason != NULL);
    destroyFixture(&r);
}

static bool listed(const char **list, int count, const char *text) {
    for (int i = 0; i < count; i++) if (list[i] && !strcmp(list[i], text)) return true;
    return false;
}

static void testBlockersListsEveryRefusal(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    const char *why[8] = {0};
    assert(Runner_snapshotBlockers(&r, why, 8) == 0);
    assert(why[0] == NULL);

    r.openTextFiles[0].isOpen = true;
    r.paused = true;
    /* The preflight stops at its first refusal; the list has both. */
    const char *first = Runner_snapshotCheck(&r);
    assert(first != NULL);
    assert(Runner_snapshotBlockers(&r, why, 8) == 2);
    assert(listed(why, 2, "Open text file"));
    assert(listed(why, 2, "Paused runner is unsupported for capture"));
    assert(listed(why, 2, first));
    assert(why[2] == NULL);

    /* A repeated refusal is listed once, and the count is the total even
     * when the caller's array is shorter. */
    r.openTextFiles[1].isOpen = true;
    const char *one[1] = {0};
    assert(Runner_snapshotBlockers(&r, one, 1) == 2);
    assert(one[0] != NULL);
    assert(Runner_snapshotBlockers(&r, NULL, 0) == 2);

    /* Listing changes nothing: the runner captures again once it is clear. */
    r.openTextFiles[0].isOpen = false;
    r.openTextFiles[1].isOpen = false;
    r.paused = false;
    assert(Runner_snapshotBlockers(&r, why, 8) == 0);
    assert(Runner_snapshotCheck(&r) == NULL);

    /* The vertex buffer table is a plain array sized by vertexBufferCount,
     * not an stb_ds array: the preflight must bound it by that count. */
    /* Revision 7 keeps the game's own vertex resources across a load, so an
     * empty pool and an unused buffer are fine; a written one is refused. */
    r.vertexBufferCount = 4;
    r.vertexBuffers = calloc(4, sizeof(*r.vertexBuffers));
    assert(r.vertexBuffers);
    assert(Runner_snapshotBlockers(&r, why, 8) == 0);
    Buffer_Vertex buffer; memset(&buffer, 0, sizeof(buffer));
    r.vertexBuffers[3] = &buffer;
    assert(Runner_snapshotBlockers(&r, why, 8) == 0);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 69, &bytes, &size, &reason));
    buffer.vertexCount = 3;
    assert(Runner_snapshotBlockers(&r, why, 8) == 1);
    assert(listed(why, 1, "Vertex buffer contents are unsupported"));
    assert(Runner_snapshotCheck(&r) != NULL);
    buffer.vertexCount = 0;
    buffer.currentMask = 1; /* A partial vertex has not advanced the stream. */
    assert(Runner_snapshotBlockers(&r, why, 8) == 1);
    assert(listed(why, 1, "Vertex buffer contents are unsupported"));
    assert(Runner_snapshotPrepare(&r, 69, bytes, size, &reason) == NULL);
    assert(buffer.currentMask == 1);
    buffer.currentMask = 0;
    VmVertexFormat activeFormat = {0};
    buffer.pCurrentFormatVFRelease = &activeFormat; /* vertex_begin alone */
    assert(Runner_snapshotCheck(&r) != NULL);
    assert(Runner_snapshotPrepare(&r, 69, bytes, size, &reason) == NULL);
    assert(buffer.pCurrentFormatVFRelease == &activeFormat);
    buffer.pCurrentFormatVFRelease = NULL;
    assert(Runner_snapshotCheck(&r) == NULL);
    RunnerSnapshot *prepared = Runner_snapshotPrepare(&r, 69, bytes, size, &reason);
    assert(prepared); Runner_snapshotFree(prepared); free(bytes);
    free(r.vertexBuffers);
    r.vertexBuffers = NULL;
    r.vertexBufferCount = 0;
    assert(Runner_snapshotBlockers(&r, why, 8) == 0);

    const char *none[2] = {0};
    assert(Runner_snapshotBlockers(NULL, none, 2) == 1);
    assert(none[0] && !strcmp(none[0], "Runner is not ready"));
    destroyFixture(&r);
}

static void testBlockersWithInvalidSpriteRegistry(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    const char *why[8] = {0};
    r.paused = true;
    dw.sprt.count = 1;
    assert(dw.sprt.sprites == NULL);
    assert(!strcmp(Runner_snapshotCheck(&r), "Invalid sprite registry"));
    assert(Runner_snapshotBlockers(&r, why, 8) == 2);
    assert(listed(why, 2, "Invalid sprite registry"));
    assert(listed(why, 2, "Paused runner is unsupported for capture"));
    assert(Runner_snapshotBlockers(&r, NULL, 0) == 2);

    dw.sprt.count = 0;
    dw.sprt.parsedCount = 1;
    assert(Runner_snapshotBlockers(&r, why, 8) == 2);
    assert(listed(why, 2, "Invalid sprite registry"));
    dw.sprt.parsedCount = 0;
    r.paused = false;
    assert(Runner_snapshotBlockers(&r, why, 8) == 0);
    destroyFixture(&r);
}

static void testDynamicSpriteCaptureEligibility(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    dw.sprt.sprites = calloc(1, sizeof(Sprite));
    assert(dw.sprt.sprites);
    dw.sprt.count = 1;
    dw.sprt.parsedCount = 1;
    dw.sprt.sprites[0].name = "__newsprite0"; /* A loaded asset's name is irrelevant. */
    dw.sprt.sprites[0].textureCount = 1;
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCheck(&r) == NULL);
    assert(Runner_snapshotCapture(&r, 7, &bytes, &size, &reason));
    uint32_t dynamic = DataWin_allocSpriteSlot(&dw, 1);
    assert(dynamic == 1);
    dw.sprt.sprites[dynamic].textureCount = 1;
    assert(Runner_snapshotCheck(&r) != NULL);
    const char *allocatedName = dw.sprt.sprites[dynamic].name;
    dw.sprt.sprites[dynamic].name = "renamed";
    assert(Runner_snapshotCheck(&r) != NULL);
    dw.sprt.sprites[dynamic].name = NULL;
    assert(Runner_snapshotCheck(&r) != NULL);
    dw.sprt.sprites[dynamic].name = allocatedName;
    uint8_t *rejected = NULL; size_t rejectedSize = 0;
    assert(!Runner_snapshotCapture(&r, 7, &rejected, &rejectedSize, &reason));
    assert(reason && rejected == NULL && rejectedSize == 0);
    assert(Runner_snapshotPrepare(&r, 7, bytes, size, &reason) == NULL);
    dw.sprt.sprites[dynamic].textureCount = 0; /* Deleted sprite leaves a reusable slot. */
    assert(Runner_snapshotCheck(&r) == NULL);
    dw.sprt.parsedCount = dw.sprt.count + 1;
    assert(Runner_snapshotCheck(&r) != NULL);
    dw.sprt.parsedCount = 1;
    RunnerSnapshot *prepared = Runner_snapshotPrepare(&r, 7, bytes, size, &reason);
    assert(prepared);
    Runner_snapshotFree(prepared);
    free((void *)dw.sprt.sprites[dynamic].name);
    free(dw.sprt.sprites);
    free(bytes);
    destroyFixture(&r);
}

/* A callee can have no locals or arguments and still be executing. Capturing
 * it would omit the continuation, so frame presence alone must refuse. */
static void testActiveCallFrameCaptureEligibility(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    CallFrame active = {0};
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    vm.currentCallFrame = &active;
    assert(Runner_snapshotCheck(&r) != NULL);
    assert(!Runner_snapshotCapture(&r, 7, &bytes, &size, &reason));
    assert(reason && bytes == NULL && size == 0);
    assert(vm.currentCallFrame == &active);
    vm.currentCallFrame = NULL;
    assert(Runner_snapshotCapture(&r, 7, &bytes, &size, &reason));
    RunnerSnapshot *prepared = Runner_snapshotPrepare(&r, 7, bytes, size, &reason);
    assert(prepared);
    Runner_snapshotApply(&r, prepared);
    assert(vm.currentCallFrame == NULL && vm.envStack == NULL && vm.stack.top == 0);
    Runner_snapshotFree(prepared);
    free(bytes);
    destroyFixture(&r);
}

static void testPausedCaptureEligibility(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 7, &bytes, &size, &reason));
    r.paused = true;
    assert(Runner_snapshotCheck(&r) != NULL);
    uint8_t *rejected = NULL; size_t rejectedSize = 0;
    assert(!Runner_snapshotCapture(&r, 7, &rejected, &rejectedSize, &reason));
    assert(reason && rejected == NULL && rejectedSize == 0);
    assert(Runner_snapshotPrepare(&r, 7, bytes, size - 1, &reason) == NULL);
    assert(r.paused); /* A rejected prepare leaves the target unchanged. */
    RunnerSnapshot *prepared = Runner_snapshotPrepare(&r, 7, bytes, size, &reason);
    assert(prepared);
    assert(r.paused); /* Prepare does not change the live runner. */
    Runner_snapshotApply(&r, prepared);
    Runner_snapshotFree(prepared);
    assert(!r.paused); /* Supported snapshots were captured while unpaused. */
    free(bytes);
    destroyFixture(&r);
}

static void testAllLoadedRoomsAndDefinitions(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room rooms[2]; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &rooms[0], &object);
    memset(&rooms[1], 0, sizeof(rooms[1]));
    dw.room.count = 2;
    rooms[1].payloadLoaded = true;
    rooms[1].width = 640;
    rooms[1].height = 480;
    rooms[1].speed = 30;
    rooms[1].tileCount = 1;
    rooms[1].tiles = calloc(1, sizeof(RoomTile));
    rooms[1].tiles[0].x = 17;
    dw.gen8.gms2FPS = 30;
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 45, &bytes, &size, &reason));
    rooms[0].width = 999;
    rooms[0].persistent = true; /* A room_set_persistent after capture. */
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 45, bytes, size, &reason);
    assert(s);
    rooms[1].tiles[0].x = 999;
    rooms[1].width = 123;
    rooms[1].height = 124;
    rooms[1].speed = 60;
    rooms[1].persistent = true;
    dw.gen8.gms2FPS = 60;
    Runner_snapshotApply(&r, s);
    Runner_snapshotFree(s);
    assert(rooms[1].tiles[0].x == 17);
    assert(rooms[0].width == 320 && !rooms[0].persistent);
    assert(rooms[1].width == 640 && rooms[1].height == 480);
    assert(rooms[1].speed == 30 && !rooms[1].persistent);
    assert(dw.gen8.gms2FPS == 30);
    rooms[1].width = rooms[1].height = 4194240;
    assert(Runner_snapshotCheck(&r) != NULL);
    free(bytes);
    free(rooms[1].tiles);
    destroyFixture(&r);
}

static void testRestoreDiscardsLiveSavedRoomResources(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room rooms[3]; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &rooms[0], &object);
    memset(&rooms[1], 0, sizeof(rooms[1]));
    memset(&rooms[2], 0, sizeof(rooms[2]));
    dw.room.count = 3;
    rooms[1].width = 320; rooms[1].height = 240;
    rooms[1].payloadLoaded = true; rooms[1].persistent = true;
    rooms[2].width = 320; rooms[2].height = 240;
    rooms[2].payloadLoaded = true;
    r.savedRoomStates = calloc(3, sizeof(SavedRoomState));
    assert(r.savedRoomStates);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 94, &bytes, &size, &reason));

    r.currentRoom = &rooms[2]; r.currentRoomIndex = 2;
    Instance *battle = Instance_create(100005, 0, 2, 3);
    arrput(r.instances, battle);
    hmput(r.instancesById, battle->instanceId, battle);

    SavedRoomState *state = &r.savedRoomStates[1];
    state->initialized = true;
    Instance *saved = Instance_create(100004, 0, 7, 8);
    arrput(state->instances, saved);
    hmput(r.instancesById, saved->instanceId, saved);
    TileLayerState tileLayer = {0};
    int tileDepth = 100;
    hmput(state->tileLayerMap, tileDepth, tileLayer);
    RuntimeLayer layer = {0}; layer.dynamicName = strdup("saved layer");
    arrput(state->runtimeLayers, layer);
    assert(Runner_snapshotCheck(&r) != NULL);
    uint8_t *refused = NULL; size_t refusedSize = 0;
    assert(!Runner_snapshotCapture(&r, 95, &refused, &refusedSize, &reason));
    assert(Runner_snapshotPrepare(&r, 94, bytes, size - 1, &reason) == NULL);
    assert(state->instances[0] == saved && hmlen(state->tileLayerMap) == 1);
    RunnerSnapshot *prepared = Runner_snapshotPrepare(&r, 94, bytes, size, &reason);
    assert(prepared);
    assert(state->instances[0] == saved && state->runtimeLayers[0].dynamicName);
    r.frameCount = 99;
    Runner_snapshotApply(&r, prepared); Runner_snapshotFree(prepared);
    assert(r.frameCount == 42 && r.currentRoom == &rooms[0] && !arrlen(r.instances));
    assert(!state->initialized && !state->instances && !state->tileLayerMap &&
           !state->runtimeLayers);
    int savedId = 100004;
    assert(hmgeti(r.instancesById, savedId) < 0);
    int battleId = 100005;
    assert(hmgeti(r.instancesById, battleId) < 0);
    /* A persistent room's saved state is captured since revision 6. */
    state->initialized = true;
    assert(Runner_snapshotCheck(&r) == NULL);
    state->initialized = false;
    free(bytes); free(r.savedRoomStates);
    destroyFixture(&r);
}

/* A battle leaves the overworld room persistent: its instances, tile layers
 * and views wait in savedRoomStates until the player returns. */
static void testPersistentSavedRoomRoundTrip(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room rooms[3]; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &rooms[0], &object);
    memset(&rooms[1], 0, sizeof(rooms[1]));
    memset(&rooms[2], 0, sizeof(rooms[2]));
    dw.room.count = 3;
    rooms[1].width = 640; rooms[1].height = 480;
    rooms[1].payloadLoaded = true; rooms[1].persistent = true;
    rooms[2].width = 320; rooms[2].height = 240; rooms[2].payloadLoaded = true;
    r.savedRoomStates = calloc(3, sizeof(SavedRoomState));
    assert(r.savedRoomStates);
    r.currentRoom = &rooms[2]; r.currentRoomIndex = 2;
    SpatialGrid_free(r.spatialGrid);
    r.spatialGrid = SpatialGrid_create(rooms[2].width, rooms[2].height);

    Instance *battle = Instance_create(100005, 0, 2, 3);
    arrput(r.instances, battle);
    hmput(r.instancesById, battle->instanceId, battle);
    SavedRoomState *state = &r.savedRoomStates[1];
    state->initialized = true;
    state->backgroundColor = 0xabcdefu;
    state->viewsEnabled = true;
    state->views[0].portX = 12;
    state->defaultCameras[0].viewWidth = 320;
    Instance *saved = Instance_create(100004, 0, 7, 8);
    saved->alarm[1] = 6;
    arrput(state->instances, saved);
    hmput(r.instancesById, saved->instanceId, saved);
    TileLayerState tileLayer = {0};
    tileLayer.visible = true; tileLayer.offsetX = 4;
    int tileDepth = 100;
    hmput(state->tileLayerMap, tileDepth, tileLayer);
    GMLArray *shared = GMLArray_create(&dw, 1);
    GMLArray_set(shared, 0, RValue_makeReal(31));
    Instance_setSelfVar(saved, 20, RValue_makeArray(shared));
    Instance_setSelfVar(battle, 22, RValue_makeArray(shared));
    Instance_setSelfVar(saved, 21, RValue_makeReal(5));
    shared->owner = IntRValueHashMap_findSlot(&saved->selfVars, 20);
    GMLArray_decRef(shared);

    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCheck(&r) == NULL);
    assert(Runner_snapshotCapture(&r, 97, &bytes, &size, &reason));
    RunnerSnapshot *prepared = Runner_snapshotPrepare(&r, 97, bytes, size, &reason);
    assert(prepared);
    saved->x = 900; state->backgroundColor = 0; state->views[0].portX = 0;
    Runner_snapshotApply(&r, prepared); Runner_snapshotFree(prepared);

    assert(r.currentRoom == &rooms[2] && arrlen(r.instances) == 1);
    assert(state->initialized && arrlen(state->instances) == 1);
    Instance *restored = state->instances[0];
    assert(restored != saved && restored->instanceId == 100004);
    assert(restored->x == 7 && restored->y == 8 && restored->alarm[1] == 6);
    assert(Instance_getSelfVar(restored, 21).real == 5);
    RValue a = Instance_getSelfVar(restored, 20);
    RValue b = Instance_getSelfVar(r.instances[0], 22);
    assert(a.type == RVALUE_ARRAY && b.type == RVALUE_ARRAY && a.array == b.array);
    assert(a.array->owner == IntRValueHashMap_findSlot(&restored->selfVars, 20));
    assert(GMLArray_get(a.array, 0).real == 31);
    assert(state->backgroundColor == 0xabcdefu && state->viewsEnabled);
    assert(state->views[0].portX == 12 && state->defaultCameras[0].viewWidth == 320);
    assert(hmlen(state->tileLayerMap) == 1);
    assert(hmget(state->tileLayerMap, tileDepth).visible);
    assert(hmget(state->tileLayerMap, tileDepth).offsetX == 4);
    int savedId = 100004;
    assert(hmget(r.instancesById, savedId) == restored);
    /* Not live in the battle room: no object list or grid entry. */
    assert(arrlen(r.instancesByExactObject[0]) == 1 && r.instancesByExactObject[0][0] == r.instances[0]);

    /* A snapshot with a duplicate id across live and saved rooms is refused. */
    restored->instanceId = 100005;
    uint8_t *dup = NULL; size_t dupSize = 0;
    assert(Runner_snapshotCapture(&r, 98, &dup, &dupSize, &reason));
    assert(Runner_snapshotPrepare(&r, 98, dup, dupSize, &reason) == NULL);
    restored->instanceId = 100004;
    free(dup);

    hmdel(r.instancesById, savedId);
    Runner_freeSavedRoomStateResources(state);
    free(bytes); free(r.savedRoomStates);
    destroyFixture(&r);
}

static void testDormantPoolHoleRefuses(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    DsQueue freed = {.freed = true};
    arrput(r.dsQueuePool, freed);
    DsStack stack = {.freed = true}; arrput(r.dsStackPool, stack);
    DsPriority priority = {.freed = true}; arrput(r.dsPriorityPool, priority);
    DsGrid grid = {.freed = true}; arrput(r.dsGridPool, grid);
    GmlBuffer buffer = {0}; arrput(r.gmlBufferPool, buffer);
    MpGrid mp = {0}; arrput(r.mpGridPool, mp);
    ParticleSystem ps = {0}; arrput(r.particleSystemPool, ps);
    ParticleType pt = {0}; arrput(r.particleTypePool, pt);
    AudioEmitter emitter = {0}; arrput(r.audioEmitters, emitter);
    VmVertexFormat *format = NULL; arrput(r.vertexFormats, format);
    Buffer_Vertex *vertex = NULL; arrput(r.vertexBuffers, vertex);
    assert(Runner_snapshotCheck(&r) == NULL);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 53, &bytes, &size, &reason));
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 53, bytes, size, &reason);
    assert(s);
    arrput(r.dsQueuePool, freed); /* New timeline consumed id 1. */
    Runner_snapshotApply(&r, s); Runner_snapshotFree(s);
    assert(arrlen(r.dsQueuePool) == 1 && r.dsQueuePool[0].freed);
    assert(arrlen(r.dsStackPool) == 1 && r.dsStackPool[0].freed);
    assert(arrlen(r.dsPriorityPool) == 1 && r.dsPriorityPool[0].freed);
    assert(arrlen(r.dsGridPool) == 1 && r.dsGridPool[0].freed);
    assert(arrlen(r.gmlBufferPool) == 1 && !r.gmlBufferPool[0].isValid);
    assert(arrlen(r.mpGridPool) == 1 && !r.mpGridPool[0].inUse);
    assert(arrlen(r.particleSystemPool) == 1 && !r.particleSystemPool[0].used);
    assert(arrlen(r.particleTypePool) == 1 && !r.particleTypePool[0].used);
    assert(arrlen(r.audioEmitters) == 1 && !r.audioEmitters[0].active);
    assert(arrlen(r.vertexFormats) == 1 && !r.vertexFormats[0]);
    assert(arrlen(r.vertexBuffers) == 1 && !r.vertexBuffers[0]);
    size_t nextQueueId = 0;
    while (nextQueueId < (size_t)arrlen(r.dsQueuePool) && !r.dsQueuePool[nextQueueId].freed)
        nextQueueId++;
    assert(nextQueueId == 0);
    r.dsQueuePool[0].freed = false;
    assert(Runner_snapshotCheck(&r) != NULL);
    free(bytes);
    destroyFixture(&r);
}

static void testMalformedRoomCleanupAndDimensions(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 50, &bytes, &size, &reason));
    repack(&bytes, &size, invalidRoomFlag);
    assert(Runner_snapshotPrepare(&r, 50, bytes, size, &reason) == NULL);
    free(bytes); bytes = NULL;
    assert(Runner_snapshotCapture(&r, 50, &bytes, &size, &reason));
    repack(&bytes, &size, hugeRoom);
    assert(Runner_snapshotPrepare(&r, 50, bytes, size, &reason) == NULL);
    free(bytes);
    room.width = room.height = 4194240;
    assert(Runner_snapshotCheck(&r) != NULL);
    destroyFixture(&r);
}

static void testRejectedPrepareKeepsLazyRoomUnloaded(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room rooms[2]; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &rooms[0], &object);
    memset(&rooms[1], 0, sizeof(rooms[1]));
    dw.room.count = 2;
    dw.lazyLoadRooms = true;
    rooms[1].width = 320; rooms[1].height = 240;
    rooms[1].payloadLoaded = true;
    rooms[1].backgroundsFileOffset = 4;
    rooms[1].viewsFileOffset = 8;
    rooms[1].gameObjectsFileOffset = 12;
    rooms[1].tilesFileOffset = 16;
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 51, &bytes, &size, &reason));
    size_t validSize = size;
    uint8_t *validBytes = malloc(size);
    memcpy(validBytes, bytes, size);
    DataWin_freeRoomPayload(&rooms[1]);
    dw.lazyLoadFile = tmpfile();
    uint8_t emptyTables[20] = {0};
    assert(dw.lazyLoadFile && fwrite(emptyTables, 1, sizeof(emptyTables), dw.lazyLoadFile) == sizeof(emptyTables));
    fflush(dw.lazyLoadFile); dw.fileSize = sizeof(emptyTables);
    repack(&bytes, &size, invalidArrayCountAfterRooms);
    assert(Runner_snapshotPrepare(&r, 51, bytes, size, &reason) == NULL);
    assert(!rooms[1].payloadLoaded && !rooms[1].backgrounds && !rooms[1].views);
    assert(!rooms[1].tiles && !rooms[1].gameObjects);
    RunnerSnapshot *prepared = Runner_snapshotPrepare(&r, 51, validBytes, validSize, &reason);
    assert(prepared && !rooms[1].payloadLoaded);
    Runner_snapshotFree(prepared);
    assert(!rooms[1].payloadLoaded && !rooms[1].backgrounds);
    prepared = Runner_snapshotPrepare(&r, 51, validBytes, validSize, &reason);
    assert(prepared && !rooms[1].payloadLoaded);
    Runner_snapshotApply(&r, prepared); Runner_snapshotFree(prepared);
    assert(rooms[1].payloadLoaded && rooms[1].backgrounds && rooms[1].views);
    DataWin_freeRoomPayload(&rooms[1]);
    dw.roomChunkBuffer = malloc(sizeof(emptyTables));
    assert(dw.roomChunkBuffer);
    memcpy(dw.roomChunkBuffer, emptyTables, sizeof(emptyTables));
    dw.roomChunkBase = 0;
    dw.roomChunkSize = sizeof(emptyTables);
    fclose(dw.lazyLoadFile);
    dw.lazyLoadFile = NULL;
    prepared = Runner_snapshotPrepare(&r, 51, validBytes, validSize, &reason);
    assert(prepared && !rooms[1].payloadLoaded);
    Runner_snapshotApply(&r, prepared); Runner_snapshotFree(prepared);
    assert(rooms[1].payloadLoaded && rooms[1].backgrounds && rooms[1].views);
    DataWin_freeRoomPayload(&rooms[1]);
    free(dw.roomChunkBuffer);
    free(validBytes); free(bytes);
    destroyFixture(&r);
}

static void testCumulativeArrayBudget(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    GMLArray *array = GMLArray_create(&dw, 64001);
    GMLArray_set(array, 31999, RValue_makeReal(0));
    GMLArray_set(array, 63999, RValue_makeReal(0));
    Instance_setSelfVar(vm.globalScopeInstance, 10, RValue_makeArray(array));
    GMLArray_decRef(array);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 52, &bytes, &size, &reason));
    repack(&bytes, &size, oversizedArrayShape);
    assert(Runner_snapshotPrepare(&r, 52, bytes, size, &reason) == NULL);
    assert(reason && strcmp(reason, "Array element budget exceeded") == 0);
    free(bytes); destroyFixture(&r);
}

static void testOpenIniRoundTrip(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    r.currentIni = Ini_parse("[route]\nname=old\n[empty]\n");
    r.currentIniPath = strdup("snowdin.ini");
    Ini_setString(r.currentIni, "route", "name", "Snowdin\"line\nnext");
    r.currentIniDirty = true;
    r.cachedIni = Ini_parse("[cache]\nmarker=warm\n");
    r.cachedIniPath = strdup("prior.ini");
    IniFile *liveCurrent = r.currentIni, *liveCache = r.cachedIni;
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCheck(&r) == NULL);
    assert(Runner_snapshotCapture(&r, 54, &bytes, &size, &reason));
    uint8_t *invalid = malloc(size);
    memcpy(invalid, bytes, size);
    size_t invalidSize = size;
    repack(&invalid, &invalidSize, invalidIniMagic);
    assert(Runner_snapshotPrepare(&r, 54, invalid, invalidSize, &reason) == NULL);
    assert(r.currentIni == liveCurrent && r.cachedIni == liveCache && r.currentIniDirty);
    free(invalid);
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 54, bytes, size, &reason);
    assert(s && r.currentIni == liveCurrent && r.cachedIni == liveCache);
    Runner_snapshotFree(s);
    assert(r.currentIni == liveCurrent && r.cachedIni == liveCache);
    s = Runner_snapshotPrepare(&r, 54, bytes, size, &reason);
    assert(s);
    Ini_setString(r.currentIni, "route", "name", "later");
    r.currentIniDirty = false;
    Runner_snapshotApply(&r, s); Runner_snapshotFree(s);
    assert(r.currentIni && r.cachedIni && r.currentIni != r.cachedIni);
    assert(strcmp(r.currentIniPath, "snowdin.ini") == 0);
    assert(strcmp(r.cachedIniPath, "prior.ini") == 0);
    assert(r.currentIniDirty);
    assert(strcmp(Ini_getString(r.currentIni, "route", "name"), "Snowdin\"line\nnext") == 0);
    assert(Ini_hasSection(r.currentIni, "empty"));
    assert(strcmp(Ini_getString(r.cachedIni, "cache", "marker"), "warm") == 0);
    free(bytes);
    Ini_free(r.currentIni); free(r.currentIniPath);
    Ini_free(r.cachedIni); free(r.cachedIniPath);
    r.currentIni = Ini_parse("[string]\nkind=memory\n");
    r.currentIniPath = NULL; r.currentIniDirty = true;
    r.cachedIni = Ini_parse("[old]\nkind=string-cache\n");
    r.cachedIniPath = NULL;
    bytes = NULL; size = 0;
    assert(Runner_snapshotCapture(&r, 55, &bytes, &size, &reason));
    s = Runner_snapshotPrepare(&r, 55, bytes, size, &reason);
    assert(s); Runner_snapshotApply(&r, s); Runner_snapshotFree(s);
    assert(r.currentIni && r.currentIniPath == NULL && r.currentIniDirty);
    assert(r.cachedIni && r.cachedIniPath == NULL);
    assert(strcmp(Ini_getString(r.currentIni, "string", "kind"), "memory") == 0);
    assert(strcmp(Ini_getString(r.cachedIni, "old", "kind"), "string-cache") == 0);
    free(bytes); destroyFixture(&r);
}

static void testSharedIniEntryBudget(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    r.currentIni = calloc(1, sizeof(*r.currentIni));
    r.currentIni->sections = calloc(65536, sizeof(IniSection));
    r.currentIni->capacity = 65536;
    for (int i = 0; i < 65536; i++)
        r.currentIni->sections[i].name = strdup("s");
    r.currentIni->count = 65535;
    r.cachedIni = Ini_parse("[cached]\n");
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 57, &bytes, &size, &reason));
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 57, bytes, size, &reason);
    assert(s); Runner_snapshotFree(s); free(bytes);
    r.currentIni->count = 65536;
    bytes = NULL; size = 0; reason = NULL;
    assert(!Runner_snapshotCapture(&r, 57, &bytes, &size, &reason));
    assert(reason && strcmp(reason, "INI entry budget exceeded") == 0);
    destroyFixture(&r);
}

static void testOlderCoreV3Trailers(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 56, &bytes, &size, &reason));
    uint32_t rawLen;
    uint8_t *raw = decodeRaw(bytes, size, &rawLen);
    toRevision6(raw, &rawLen, 1);
    dropSavedRooms(raw, &rawLen, 1);
    encodeRaw(&bytes, &size, raw, rawLen, 3);
    free(raw);
    removeIniAndMaybeDormantTail(&bytes, &size, 8);
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 56, bytes, size, &reason);
    assert(s); Runner_snapshotFree(s);
    removeIniAndMaybeDormantTail(&bytes, &size, 44);
    s = Runner_snapshotPrepare(&r, 56, bytes, size, &reason);
    assert(s); Runner_snapshotFree(s);
    free(bytes); destroyFixture(&r);
}

static void testRevision5WithoutSavedRooms(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 59, &bytes, &size, &reason));
    uint32_t rawLen;
    uint8_t *raw = decodeRaw(bytes, size, &rawLen);
    toRevision6(raw, &rawLen, 1);
    dropSavedRooms(raw, &rawLen, 1);
    encodeRaw(&bytes, &size, raw, rawLen, 5);
    free(raw);
    assert(le32(bytes + 4) == 5);
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 59, bytes, size, &reason);
    assert(s);
    Runner_snapshotApply(&r, s); Runner_snapshotFree(s);
    free(bytes); destroyFixture(&r);
}

#if IS_WAD17_OR_HIGHER_ENABLED
/* Revision 7: a GMS 2.3 struct whose GMS2 array holds the struct itself and a
 * method that two globals share; the array's owner is a scope token, and the
 * VM's static-block flags come back with the state. */
static void testGms2ValuesRoundTrip(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    dw.gen8.wadVersion = 17;
    dw.detectedFormat.major = 2; dw.detectedFormat.minor = 3;
    CodeEntry entries[2]; memset(entries, 0, sizeof(entries));
    entries[0].present = entries[1].present = true;
    dw.code.entries = entries; dw.code.count = 2;
    vm.staticInitialized = calloc(2, sizeof(bool));
    vm.staticInitialized[1] = true;

    Instance *st = Runner_createStruct(&r);
    uint32_t structId = st->instanceId;
    GMLArray *items = GMLArray_create(&dw, 2);
    assert(items->type == GML_MODERN_ARRAY);
    items->owner = (void *)(uintptr_t)1234;
    GMLMethod *method = GMLMethod_create(1, (int32_t)structId);
    /* The setters take their own reference; ours are dropped below. */
    GMLArray_set(items, 0, RValue_makeStructWeak(st));
    GMLArray_set(items, 1, RValue_makeMethodWeak(method));
    Instance_setSelfVar(st, 5, RValue_makeArrayWeak(items));
    Instance_setSelfVar(vm.globalScopeInstance, 1, RValue_makeMethodWeak(method));
    Instance_setSelfVar(vm.globalScopeInstance, 2, RValue_makeMethodWeak(method));
    Instance_setSelfVar(vm.globalScopeInstance, 3, RValue_makeStructWeak(st));
    GMLArray_decRef(items); GMLMethod_decRef(method);
    assert(st->refCount == 3 && method->refCount == 3);

    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 77, &bytes, &size, &reason));
    assert(le32(bytes + 4) == 9);

    /* Disturb the live state, then load. */
    Instance_setSelfVar(vm.globalScopeInstance, 2, RValue_makeReal(0));
    vm.staticInitialized[1] = false; vm.staticInitialized[0] = true;
    RunnerSnapshot *snap = Runner_snapshotPrepare(&r, 77, bytes, size, &reason);
    assert(snap);
    Runner_snapshotApply(&r, snap); Runner_snapshotFree(snap);

    assert(arrlen(r.structInstances) == 1);
    Instance *back = r.structInstances[0];
    assert(back != st && back->instanceId == structId && back->structRegistryIndex == 0);
    assert(back->refCount == 3); /* the registry, global 3, and items[0] */
    int key = (int)structId;
    assert(hmget(r.instancesById, key) == back);
    RValue m1 = Instance_getSelfVar(vm.globalScopeInstance, 1);
    RValue m2 = Instance_getSelfVar(vm.globalScopeInstance, 2);
    assert(m1.type == RVALUE_METHOD && m2.type == RVALUE_METHOD && m1.method == m2.method);
    assert(m1.method->codeIndex == 1 && m1.method->boundInstanceId == (int32_t)structId);
    RValue s3 = Instance_getSelfVar(vm.globalScopeInstance, 3);
    assert(s3.type == RVALUE_STRUCT && s3.structInst == back);
    RValue arr = Instance_getSelfVar(back, 5);
    assert(arr.type == RVALUE_ARRAY && arr.array->type == GML_MODERN_ARRAY);
    assert(arr.array->owner == (void *)(uintptr_t)1234 && arr.array->modern.length == 2);
    assert(arr.array->modern.data[0].type == RVALUE_STRUCT && arr.array->modern.data[0].structInst == back);
    assert(arr.array->modern.data[1].type == RVALUE_METHOD && arr.array->modern.data[1].method == m1.method);
    assert(m1.method->refCount == 3); /* globals 1 and 2, and items[1] */
    assert(!vm.staticInitialized[0] && vm.staticInitialized[1]);

    /* A method that points at no code is not saved. */
    free(bytes); bytes = NULL;
    RValue invalidMethod = RValue_makeMethod(GMLMethod_create(7, -1));
    Instance_setSelfVar(vm.globalScopeInstance, 4, invalidMethod);
    RValue_free(&invalidMethod);
    assert(!Runner_snapshotCapture(&r, 77, &bytes, &size, &reason));
    free(bytes); free(vm.staticInitialized);
    destroyFixture(&r);
}
#endif

/* Revision 7: the current room's runtime layers. Elements that borrow from
 * the parsed room (a tilemap, a legacy tile, a sprite's name) come back
 * pointing at the same room data; an owned tile and a dynamic layer's name
 * come back as copies; legacy tiles moved after the capture are put back;
 * the game's own vertex format is kept when it matches. */
static void testRuntimeLayersRoundTrip(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    uint32_t cells[4] = {1, 2, 3, 4};
    RoomLayerTilesData tilemap = {0, 2, 2, cells};
    RoomTile legacy[2]; memset(legacy, 0, sizeof(legacy));
    legacy[0].x = 10; legacy[1].x = 20;
    static const char spriteName[] = "graphic_lamp";
    SpriteInstance sprites[1]; memset(sprites, 0, sizeof(sprites));
    sprites[0].name = spriteName;
    RoomLayerAssetsData assets = {2, legacy, 1, sprites};
    RoomLayer layers[2]; memset(layers, 0, sizeof(layers));
    layers[0].type = RoomLayerType_Tiles; layers[0].tilesData = &tilemap;
    layers[1].type = RoomLayerType_Assets; layers[1].assetsData = &assets;
    room.layers = layers; room.layerCount = 2;

    RuntimeLayer parsed; memset(&parsed, 0, sizeof(parsed));
    parsed.id = 5; parsed.depth = 100; parsed.visible = true;
    parsed.beginScript = parsed.endScript = parsed.shaderIndex = -1;
    RuntimeLayerElement el; memset(&el, 0, sizeof(el));
    el.id = 6; el.type = RuntimeLayerElementType_Tilemap; el.visible = true; el.tilemapData = &tilemap;
    arrput(parsed.elements, el);
    memset(&el, 0, sizeof(el));
    el.id = 7; el.type = RuntimeLayerElementType_Tile; el.tileElement = &legacy[1]; el.alpha = 0.5f;
    arrput(parsed.elements, el);
    memset(&el, 0, sizeof(el));
    el.id = 8; el.type = RuntimeLayerElementType_Sprite;
    el.spriteElement = calloc(1, sizeof(RuntimeSpriteElement));
    el.spriteElement->name = spriteName; el.spriteElement->x = 33;
    arrput(parsed.elements, el);
    arrput(r.runtimeLayers, parsed);
    RuntimeLayer dynamic; memset(&dynamic, 0, sizeof(dynamic));
    dynamic.id = 9; dynamic.dynamic = true; dynamic.dynamicName = strdup("_layer_9");
    dynamic.beginScript = dynamic.endScript = dynamic.shaderIndex = -1;
    memset(&el, 0, sizeof(el));
    el.id = 10; el.type = RuntimeLayerElementType_Tile; el.tileElementOwned = true;
    el.tileElement = calloc(1, sizeof(RoomTile)); el.tileElement->y = 44;
    arrput(dynamic.elements, el);
    memset(&el, 0, sizeof(el));
    el.id = 11; el.type = RuntimeLayerElementType_Instance; el.instanceId = 100001;
    arrput(dynamic.elements, el);
    arrput(r.runtimeLayers, dynamic);
    VmVertexElement elements[1] = {{0, (enum yyVertexType)1, (enum yyVertexUsage)1, 1}};
    VmVertexFormat format; memset(&format, 0, sizeof(format));
    format.id = 1; format.count = 1; format.format = elements; format.size = 12;
    arrsetlen(r.vertexFormats, 2); r.vertexFormats[0] = NULL; r.vertexFormats[1] = &format;

    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 66, &bytes, &size, &reason));
    legacy[1].x = 99; /* layer_tile_x after the capture */
    cells[0] = 99; /* tilemap_set after the capture */
    RunnerSnapshot *snap = Runner_snapshotPrepare(&r, 66, bytes, size, &reason);
    assert(snap);
    assert(cells[0] == 99); /* Preparation must leave live cells alone. */
    Runner_snapshotApply(&r, snap); Runner_snapshotFree(snap);
    assert(cells[0] == 1 && cells[3] == 4);

    assert(arrlen(r.runtimeLayers) == 2);
    RuntimeLayer *a = &r.runtimeLayers[0], *b = &r.runtimeLayers[1];
    assert(a->id == 5 && a->depth == 100 && a->visible && !a->dynamicName && arrlen(a->elements) == 3);
    assert(a->elements[0].type == RuntimeLayerElementType_Tilemap && a->elements[0].tilemapData == &tilemap);
    assert(a->elements[1].tileElement == &legacy[1] && !a->elements[1].tileElementOwned &&
           a->elements[1].alpha == 0.5f);
    assert(legacy[1].x == 20 && legacy[0].x == 10); /* put back */
    assert(a->elements[2].spriteElement && a->elements[2].spriteElement->name == spriteName &&
           a->elements[2].spriteElement->x == 33);
    assert(b->dynamic && b->dynamicName && !strcmp(b->dynamicName, "_layer_9") && arrlen(b->elements) == 2);
    assert(b->elements[0].tileElementOwned && b->elements[0].tileElement->y == 44);
    assert(b->elements[1].type == RuntimeLayerElementType_Instance && b->elements[1].instanceId == 100001);
    assert(arrlen(r.vertexFormats) == 2 && r.vertexFormats[1] == &format); /* kept */

    /* A changed vertex format is not the game the state was taken from. */
    format.size = 16;
    assert(Runner_snapshotPrepare(&r, 66, bytes, size, &reason) == NULL);
    format.size = 12;
    /* An element pointing outside the room's data is not saved. */
    RoomTile stray; memset(&stray, 0, sizeof(stray));
    r.runtimeLayers[0].elements[1].tileElement = &stray;
    assert(Runner_snapshotCheck(&r) != NULL);
    r.runtimeLayers[0].elements[1].tileElement = &legacy[1];
    free(bytes);
    for (ptrdiff_t i = 0; i < arrlen(r.runtimeLayers); i++) Runner_freeRuntimeLayer(&r.runtimeLayers[i]);
    arrfree(r.runtimeLayers);
    arrfree(r.vertexFormats); r.vertexFormats = NULL;
    room.layers = NULL; room.layerCount = 0;
    destroyFixture(&r);
}

/* Parsed tilemaps without a live layer element still carry game mutations.
 * Restore into a separate runner with pristine room payloads as on cold load. */
static void checkTilemapCellsColdRestore(uint32_t version) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room rooms[2]; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &rooms[0], &object);
    rooms[1] = rooms[0]; dw.room.count = 2;
    uint32_t cells[2][2] = {{0xf0000042u, 3}, {17, 0x40000004u}};
    RoomLayerTilesData maps[2] = {{0, 2, 1, cells[0]}, {0, 1, 2, cells[1]}};
    RoomLayer layers[2]; memset(layers, 0, sizeof(layers));
    for (int i = 0; i < 2; i++) {
        layers[i].type = RoomLayerType_Tiles; layers[i].tilesData = &maps[i];
        rooms[i].layers = &layers[i]; rooms[i].layerCount = 1;
    }
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    r.mathEpsilon = (GMLReal)0.125;
    assert(Runner_snapshotCapture(&r, 67, &bytes, &size, &reason));
    if (version == 8) {
        uint32_t rawLen;
        uint8_t *raw = decodeRaw(bytes, size, &rawLen);
        toRevision8(raw, &rawLen, 2);
        encodeRaw(&bytes, &size, raw, rawLen, 8);
        free(raw);
    }
    destroyFixture(&r);

    fixture(&r, &dw, &vm, &renderer, &vtable, &rooms[0], &object);
    rooms[1] = rooms[0]; dw.room.count = 2;
    r.mathEpsilon = (GMLReal)0.5;
    for (int i = 0; i < 2; i++) {
        cells[i][0] = cells[i][1] = 0;
        rooms[i].layers = &layers[i]; rooms[i].layerCount = 1;
    }
    uint32_t rawLen;
    uint8_t *raw = decodeRaw(bytes, size, &rawLen);
    size_t header = 0;
    for (size_t i = 20; i + 4 <= rawLen; i++)
        if (le32(raw + i) == 0xf0000042u) { header = i - 16; break; }
    assert(header && le32(raw + header - 4) == 2);
    for (int fault = 0; fault < 5; fault++) {
        uint8_t *changed = malloc(rawLen), *bad = malloc(size);
        assert(changed && bad);
        memcpy(changed, raw, rawLen); memcpy(bad, bytes, size);
        size_t badSize = size; uint32_t changedLen = rawLen;
        if (fault == 0) write32(changed + header + 8, UINT32_MAX); /* width */
        if (fault == 1) write32(changed + header, 2); /* missing room */
        if (fault == 2) write32(changed + header + 24, 0); /* duplicate map */
        if (fault == 3) write32(changed + header - 4, UINT32_MAX); /* map count */
        if (fault == 4) { /* Validly framed, but omits both loaded maps. */
            write32(changed + header - 4, 0);
            memmove(changed + header, changed + header + 48, rawLen - header - 48);
            changedLen -= 48;
        }
        encodeRaw(&bad, &badSize, changed, changedLen, version);
        assert(Runner_snapshotPrepare(&r, 67, bad, badSize, &reason) == NULL);
        assert(cells[0][0] == 0 && cells[1][0] == 0 && r.frameCount == 42);
        free(changed); free(bad);
    }
    free(raw);
    RunnerSnapshot *snap = Runner_snapshotPrepare(&r, 67, bytes, size, &reason);
    assert(snap && cells[0][0] == 0 && cells[1][0] == 0);
    Runner_snapshotFree(snap); /* Abandoning a prepared load changes no cells. */
    assert(cells[0][0] == 0 && cells[1][0] == 0);
    snap = Runner_snapshotPrepare(&r, 67, bytes, size, &reason);
    assert(snap);
    Runner_snapshotApply(&r, snap); Runner_snapshotFree(snap);
    assert(r.mathEpsilon == (version == 8 ? GML_MATH_EPSILON : (GMLReal)0.125));
    assert(cells[0][0] == 0xf0000042u && cells[0][1] == 3);
    assert(cells[1][0] == 17 && cells[1][1] == 0x40000004u);
    maps[1].tileData = NULL;
    assert(Runner_snapshotCheck(&r) != NULL);
    maps[1].tileData = cells[1]; maps[1].tilesY = UINT32_MAX;
    assert(Runner_snapshotCheck(&r) != NULL);
    maps[1].tilesY = 2;
    free(rooms[1].tiles); rooms[1].tiles = NULL;
    free(bytes); destroyFixture(&r);
}

static void testTilemapCellsColdRestore(void) {
    checkTilemapCellsColdRestore(8);
    checkTilemapCellsColdRestore(9);
}

/* Legacy formats describe no live vertex resources. Never displace a plain
 * buffer pool with their dormant stb arrays, or free it as an stb array. */
static void testLegacyLoadRefusesLiveVertexResources(void) {
    for (uint32_t version = 3; version <= 6; version++) {
        Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
        Room room; GameObject object;
        fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
        uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
        assert(Runner_snapshotCapture(&r, 70, &bytes, &size, &reason));
        uint32_t rawLen;
        uint8_t *raw = decodeRaw(bytes, size, &rawLen);
        toRevision6(raw, &rawLen, 1);
        if (version < 6) dropSavedRooms(raw, &rawLen, 1);
        encodeRaw(&bytes, &size, raw, rawLen, version); free(raw);
        r.mathEpsilon = (GMLReal)0.5;

        r.vertexBufferCount = 4;
        r.vertexBuffers = calloc(4, sizeof(*r.vertexBuffers));
        assert(r.vertexBuffers);
        Buffer_Vertex **buffers = r.vertexBuffers;
        assert(Runner_snapshotPrepare(&r, 70, bytes, size, &reason) == NULL);
        assert(reason && !strcmp(reason, "The game's vertex resources differ from the state's"));
        assert(r.vertexBuffers == buffers && r.vertexBufferCount == 4 && r.frameCount == 42);
        free(buffers); r.vertexBuffers = NULL; r.vertexBufferCount = 0;

        VmVertexFormat format = {0}; format.id = 7;
        arrput(r.vertexFormats, &format);
        assert(Runner_snapshotPrepare(&r, 70, bytes, size, &reason) == NULL);
        assert(r.vertexFormats[0] == &format && format.id == 7);
        r.vertexFormats[0] = NULL; /* Dormant slots remain compatible. */
        RunnerSnapshot *snap = Runner_snapshotPrepare(&r, 70, bytes, size, &reason);
        assert(snap); Runner_snapshotApply(&r, snap); Runner_snapshotFree(snap);
        assert(r.mathEpsilon == GML_MATH_EPSILON);
        free(bytes); destroyFixture(&r);
    }
}

static void testRevisions6Through8StillLoad(void) {
    for (uint32_t version = 6; version <= 8; version++) {
        Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
        Room room; GameObject object;
        fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
        r.score = 123;
        uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
        assert(Runner_snapshotCapture(&r, 68, &bytes, &size, &reason));
        uint32_t rawLen;
        uint8_t *raw = decodeRaw(bytes, size, &rawLen);
        if (version == 6) toRevision6(raw, &rawLen, 1);
        else if (version == 7) toRevision7(raw, &rawLen, 1);
        else toRevision8(raw, &rawLen, 1);
        encodeRaw(&bytes, &size, raw, rawLen, version);
        free(raw);
        r.score = 0;
        r.mathEpsilon = (GMLReal)0.5;
        RunnerSnapshot *snap = Runner_snapshotPrepare(&r, 68, bytes, size, &reason);
        assert(snap);
        Runner_snapshotApply(&r, snap); Runner_snapshotFree(snap);
        assert(r.score == 123);
        assert(r.mathEpsilon == GML_MATH_EPSILON);
        free(bytes); destroyFixture(&r);
    }
}

static void testZstdV4FrameValidation(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    assert(sizeof(r.score) == sizeof(uint32_t));
    const uint32_t scoreBits = 0x3eaaaaabu, livesBits = 0x80000000u;
    memcpy(&r.score, &scoreBits, sizeof(scoreBits));
    memcpy(&r.lives, &livesBits, sizeof(livesBits));
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 58, &bytes, &size, &reason));
    assert(le32(bytes + 4) == 9);
    uint32_t rawLen;
    uint8_t *raw = decodeRaw(bytes, size, &rawLen);
    toRevision6(raw, &rawLen, 1);
    dropSavedRooms(raw, &rawLen, 1);
    encodeRaw(&bytes, &size, raw, rawLen, 4);
    assert(le32(bytes + 4) == 4);
    assert(ZSTD_getFrameContentSize(bytes + 32, size - 32) == le32(bytes + 24));
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 58, bytes, size, &reason);
    assert(s);
    r.score = 0; r.lives = 1;
    Runner_snapshotApply(&r, s); Runner_snapshotFree(s);
    uint32_t actualScore, actualLives;
    memcpy(&actualScore, &r.score, sizeof(actualScore));
    memcpy(&actualLives, &r.lives, sizeof(actualLives));
    assert(actualScore == scoreBits && actualLives == livesBits);
    assert(Runner_snapshotPrepare(&r, 58, bytes, size - 1, &reason) == NULL);
    uint8_t *bad = malloc(size + 1);
    /* A truncated frame with a repaired outer checksum must reach the codec. */
    memcpy(bad, bytes, size - 1);
    write32(bad + 16, (uint32_t)(size - 1 - 24));
    write32(bad + 20, hashBytes(bad + 24, size - 1 - 24));
    assert(Runner_snapshotPrepare(&r, 58, bad, size - 1, &reason) == NULL);
    memcpy(bad, bytes, size);
    bad[size] = 0;
    write32(bad + 16, (uint32_t)(size + 1 - 24));
    write32(bad + 20, hashBytes(bad + 24, size + 1 - 24));
    assert(Runner_snapshotPrepare(&r, 58, bad, size + 1, &reason) == NULL);
    memcpy(bad, bytes, size);
    bad[32] ^= 0x80;
    write32(bad + 20, hashBytes(bad + 24, size - 24));
    assert(Runner_snapshotPrepare(&r, 58, bad, size, &reason) == NULL);
    ZSTD_CCtx *ctx = ZSTD_createCCtx();
    assert(ctx);
    assert(!ZSTD_isError(ZSTD_CCtx_setParameter(ctx, ZSTD_c_contentSizeFlag, 0)));
    size_t cap = ZSTD_compressBound(rawLen);
    uint8_t *unknown = malloc(cap + 32);
    memcpy(unknown, bytes, 32);
    size_t compressed = ZSTD_compress2(ctx, unknown + 32, cap, raw, rawLen);
    assert(!ZSTD_isError(compressed));
    assert(ZSTD_getFrameContentSize(unknown + 32, compressed) == ZSTD_CONTENTSIZE_UNKNOWN);
    write32(unknown + 16, (uint32_t)compressed + 8);
    write32(unknown + 20, hashBytes(unknown + 24, compressed + 8));
    assert(Runner_snapshotPrepare(&r, 58, unknown, compressed + 32, &reason) == NULL);
    free(unknown); ZSTD_freeCCtx(ctx); free(raw);
    free(bad); free(bytes);
    destroyFixture(&r);
}

static void testFreeCameraRefuses(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    r.freeCamZoom = 1.0f;
    r.freeCamPanX = 0.25f;
    assert(Runner_snapshotCheck(&r) != NULL);
    destroyFixture(&r);
}

static void testRoomLoadedAfterCaptureIsDiscarded(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room rooms[2]; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &rooms[0], &object);
    memset(&rooms[1], 0, sizeof(rooms[1]));
    dw.room.count = 2;
    dw.lazyLoadRooms = true;
    rooms[1].width = 600;
    rooms[1].height = 240;
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 46, &bytes, &size, &reason));
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 46, bytes, size, &reason);
    assert(s);
    rooms[1].payloadLoaded = true;
    rooms[1].tileCount = 1;
    rooms[1].tiles = calloc(1, sizeof(RoomTile));
    rooms[1].tiles[0].x = 999;
    rooms[1].width = 900;
    Runner_snapshotApply(&r, s);
    Runner_snapshotFree(s);
    assert(!rooms[1].payloadLoaded && rooms[1].tileCount == 0 && rooms[1].tiles == NULL);
    assert(rooms[1].width == 600);
    free(bytes);
    destroyFixture(&r);
}

static void testOrphanedSharedArrayOwner(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    Instance *first = Instance_create(100001, 0, 0, 0);
    Instance *second = Instance_create(100002, 0, 0, 0);
    arrput(r.instances, first); arrput(r.instances, second);
    GMLArray *shared = GMLArray_create(&dw, 1);
    GMLArray_set(shared, 0, RValue_makeReal(42));
    Instance_setSelfVar(first, 10, RValue_makeArray(shared));
    Instance_setSelfVar(second, 11, RValue_makeArray(shared));
    RValue deadOwnerSlot = RValue_makeUndefined();
    shared->owner = &deadOwnerSlot; /* A former owner outside every live root. */
    GMLArray_decRef(shared);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 47, &bytes, &size, &reason));
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 47, bytes, size, &reason);
    assert(s);
    Runner_snapshotApply(&r, s);
    Runner_snapshotFree(s);
    RValue *a = IntRValueHashMap_findSlot(&r.instances[0]->selfVars, 10);
    RValue *b = IntRValueHashMap_findSlot(&r.instances[1]->selfVars, 11);
    assert(a->array == b->array && a->array->refCount == 2);
    assert(a->array->owner && a->array->owner != a && a->array->owner != b);
    free(bytes);
    destroyFixture(&r);
}

static void testReusedSharedArrayOwnerSlot(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    Instance *first = Instance_create(100001, 0, 0, 0);
    Instance *second = Instance_create(100002, 0, 0, 0);
    arrput(r.instances, first); arrput(r.instances, second);
    GMLArray *shared = GMLArray_create(&dw, 1);
    GMLArray_set(shared, 0, RValue_makeReal(42));
    Instance_setSelfVar(first, 10, RValue_makeArray(shared));
    Instance_setSelfVar(first, 12, RValue_makeReal(7));
    Instance_setSelfVar(second, 11, RValue_makeArray(shared));
    /* The former owner address has been reused by a live, unrelated value. */
    RValue *reused = IntRValueHashMap_findSlot(&first->selfVars, 12);
    shared->owner = reused;
    GMLArray_decRef(shared);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 49, &bytes, &size, &reason));
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 49, bytes, size, &reason);
    assert(s);
    Runner_snapshotApply(&r, s); Runner_snapshotFree(s);
    RValue *a = IntRValueHashMap_findSlot(&r.instances[0]->selfVars, 10);
    RValue *b = IntRValueHashMap_findSlot(&r.instances[1]->selfVars, 11);
    RValue *reusedAfter = IntRValueHashMap_findSlot(&r.instances[0]->selfVars, 12);
    assert(a->array == b->array && a->array->refCount == 2);
    assert(a->array->owner && a->array->owner != a && a->array->owner != b);
    assert(a->array->owner != reusedAfter);
    free(bytes);
    destroyFixture(&r);
}

static void testLargeCompressibleMap(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    DsMapEntry *map = NULL;
    char textValue[1025]; memset(textValue, 'x', 1024); textValue[1024] = 0;
    for (int i = 0; i < 1200; i++) {
        char key[32]; snprintf(key, sizeof(key), "key%d", i);
        shput(map, strdup(key), RValue_makeOwnedString(strdup(textValue)));
    }
    arrput(r.dsMapPool, map);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 48, &bytes, &size, &reason));
    assert(size < 1024 * 1024);
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 48, bytes, size, &reason);
    assert(s);
    Runner_snapshotApply(&r, s); Runner_snapshotFree(s);
    assert(shlen(r.dsMapPool[0]) == 1200);
    assert(strlen(shget(r.dsMapPool[0], "key1199").string) == 1024);
    free(bytes);
    destroyFixture(&r);
}

static void testStaticStringReferences(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    char staticText[129]; memset(staticText, 'q', 128); staticText[128] = 0;
    const char *strings[] = {staticText, "other"};
    dw.strg.strings = strings; dw.strg.count = 2;
    DsList list = {0};
    for (int i = 0; i < 64; i++)
        arrput(list.items, RValue_makeOwnedString(strdup(staticText)));
    arrput(list.items, RValue_makeOwnedString(strdup("dynamic-value")));
    arrput(r.dsListPool, list);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 91, &bytes, &size, &reason));
    assert(le32(bytes + 4) == 9);
    uint32_t rawLen;
    uint8_t *raw = decodeRaw(bytes, size, &rawLen);
    dw.strg.count = 0;
    uint8_t *literal = NULL; size_t literalSize = 0;
    assert(Runner_snapshotCapture(&r, 91, &literal, &literalSize, &reason));
    assert(le32(literal + 24) - rawLen >= 64u * 128u);
    free(literal);
    dw.strg.count = 2;
    size_t ref = rawLen;
    for (size_t i = 4; i + 4 <= rawLen; i++)
        if ((le32(raw + i - 4) & 255u) == RVALUE_STRING &&
            le32(raw + i) == 0x80000000u) {
            ref = i; break;
        }
    assert(ref < rawLen);
    assert(Runner_snapshotPrepare(&r, 91, bytes, size - 1, &reason) == NULL);
    RunnerSnapshot *s = Runner_snapshotPrepare(&r, 91, bytes, size, &reason);
    assert(s);
    Runner_snapshotApply(&r, s); Runner_snapshotFree(s);
    assert(arrlen(r.dsListPool[0].items) == 65);
    for (int i = 0; i < 64; i++) {
        RValue *value = &r.dsListPool[0].items[i];
        assert(value->type == RVALUE_STRING && value->ownsReference);
        assert(strcmp(value->string, staticText) == 0 && value->string != staticText);
        if (i) assert(value->string != r.dsListPool[0].items[i - 1].string);
    }
    assert(strcmp(r.dsListPool[0].items[64].string, "dynamic-value") == 0);
    strings[0] = NULL;
    assert(Runner_snapshotPrepare(&r, 91, bytes, size, &reason) == NULL);
    strings[0] = staticText;
    char *oversized = malloc(65537); assert(oversized);
    memset(oversized, 'x', 65536); oversized[65536] = 0;
    strings[0] = oversized;
    assert(Runner_snapshotPrepare(&r, 91, bytes, size, &reason) == NULL);
    strings[0] = staticText;
    free(oversized);
    dw.strg.count = 131073;
    assert(Runner_snapshotPrepare(&r, 91, bytes, size, &reason) == NULL);
    uint8_t *tooMany = NULL; size_t tooManySize = 0;
    assert(!Runner_snapshotCapture(&r, 91, &tooMany, &tooManySize, &reason));
    dw.strg.count = 2;
    uint8_t *bad = malloc(size); assert(bad); memcpy(bad, bytes, size);
    write32(raw + ref, 0x80000002u);
    size_t badSize = size;
    encodeRaw(&bad, &badSize, raw, rawLen, le32(bytes + 4));
    assert(Runner_snapshotPrepare(&r, 91, bad, badSize, &reason) == NULL);
    free(bad); free(raw); free(bytes);
    destroyFixture(&r);
}

static void testStaticStringExpansionBudget(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    char *text = malloc(65537); assert(text);
    memset(text, 'x', 65536); text[65536] = 0;
    const char *strings[] = {text};
    dw.strg.strings = strings; dw.strg.count = 1;
    DsList list = {0};
    for (int i = 0; i < 64; i++)
        arrput(list.items, RValue_makeOwnedString(strdup(text)));
    arrput(r.dsListPool, list);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(!Runner_snapshotCapture(&r, 92, &bytes, &size, &reason));
    assert(bytes == NULL && size == 0);
    destroyFixture(&r);
    free(text);
}

static void testDecoderStaticStringExpansionBudget(void) {
    Runner r; DataWin dw; VMContext vm; Renderer renderer; RendererVtable vtable;
    Room room; GameObject object;
    fixture(&r, &dw, &vm, &renderer, &vtable, &room, &object);
    char *longText = malloc(65537); assert(longText);
    memset(longText, 'x', 65536); longText[65536] = 0;
    const char *strings[] = {"short", longText};
    dw.strg.strings = strings; dw.strg.count = 2;
    DsList list = {0};
    const char *liveStrings[64];
    for (int i = 0; i < 64; i++) {
        RValue value = RValue_makeOwnedString(strdup("short"));
        liveStrings[i] = value.string;
        arrput(list.items, value);
    }
    arrput(r.dsListPool, list);
    uint8_t *bytes = NULL; size_t size = 0; const char *reason = NULL;
    assert(Runner_snapshotCapture(&r, 93, &bytes, &size, &reason));
    uint32_t rawLen;
    uint8_t *raw = decodeRaw(bytes, size, &rawLen);
    size_t run = rawLen;
    uint32_t runs = 0;
    for (size_t at = 0; at + 64u * 8u <= rawLen; at++) {
        int j = 0;
        for (; j < 64; j++) {
            size_t value = at + (size_t)j * 8u;
            if ((le32(raw + value) & 255u) != RVALUE_STRING ||
                le32(raw + value + 4) != 0x80000000u) break;
        }
        if (j == 64) { run = at; runs++; }
    }
    assert(runs == 1); /* Exactly the 64 consecutive list values. */
    for (int j = 0; j < 64; j++)
        write32(raw + run + (size_t)j * 8u + 4u, 0x80000001u);
    encodeRaw(&bytes, &size, raw, rawLen, le32(bytes + 4));
    assert(size < 1024u * 1024u);
    assert(Runner_snapshotPrepare(&r, 93, bytes, size, &reason) == NULL);
    assert(r.frameCount == 42 && arrlen(r.dsListPool[0].items) == 64);
    for (int i = 0; i < 64; i++) {
        assert(r.dsListPool[0].items[i].string == liveStrings[i]);
        assert(strcmp(liveStrings[i], "short") == 0);
    }
    longText[65535] = 0; /* 64 * (65535 + NUL) is exactly 4 MiB. */
    RunnerSnapshot *atLimit = Runner_snapshotPrepare(&r, 93, bytes, size, &reason);
    assert(atLimit);
    Runner_snapshotFree(atLimit);
    free(raw); free(bytes);
    destroyFixture(&r);
    free(longText);
}

int main(void) {
    testComparisonToleranceRoundTrip();
    testInvalidComparisonTolerance();
    testDestroyedInstanceAwaitingCleanup();
    testBlockersListsEveryRefusal();
    testRestoreDiscardsLiveSavedRoomResources();
    testPersistentSavedRoomRoundTrip();
    testStaticStringReferences();
    testStaticStringExpansionBudget();
    testDecoderStaticStringExpansionBudget();
    testZstdV4FrameValidation();
    testRevision5WithoutSavedRooms();
    testLegacyLoadRefusesLiveVertexResources();
    testRevisions6Through8StillLoad();
    testTilemapCellsColdRestore();
    testRuntimeLayersRoundTrip();
#if IS_WAD17_OR_HIGHER_ENABLED
    testGms2ValuesRoundTrip();
#endif
    testRoundTrip();
    testRejectsBadInputAndLiveResources();
    testActiveCallFrameCaptureEligibility();
    testPausedCaptureEligibility();
    testDynamicSpriteCaptureEligibility();
    testBlockersWithInvalidSpriteRegistry();
    testAllLoadedRoomsAndDefinitions();
    testDormantPoolHoleRefuses();
    testMalformedRoomCleanupAndDimensions();
    testRejectedPrepareKeepsLazyRoomUnloaded();
    testCumulativeArrayBudget();
    testOpenIniRoundTrip();
    testSharedIniEntryBudget();
    testOlderCoreV3Trailers();
    testFreeCameraRefuses();
    testRoomLoadedAfterCaptureIsDiscarded();
    testOrphanedSharedArrayOwner();
    testReusedSharedArrayOwnerSlot();
    testLargeCompressibleMap();
    return 0;
}
