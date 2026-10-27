#include "runner_snapshot.h"
#include "gettime.h"
#include "ini.h"
#include "spatial_grid.h"
#include "stb_ds.h"
#include "miniz.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#ifdef PLATFORM_BUSY_TICK
#include "log.h"
#endif
#ifdef ENABLE_SW_RENDERER
#include "sw/sw_renderer.h"
#endif
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* This is a same-build, WAD16/17 wire format. Never write live pointers. */
#define SNAP_MAGIC 0x31504e53u /* SNP1 */
#define SNAP_VERSION 10u
#define SNAP_RENDERER_VERSION 10u
#define SNAP_EPSILON_VERSION 9u
#define SNAP_TILEMAP_VERSION 8u
#define SNAP_GMS2_VERSION 7u
#define SNAP_SAVED_ROOM_VERSION 6u
#define SNAP_STRING_REF_VERSION 5u
#define SNAP_ZSTD_LEGACY_VERSION 4u
#define SNAP_LEGACY_VERSION 3u
#define SNAP_OUTPUT_LIMIT (1024u * 1024u)
#define SNAP_LIMIT (4u * 1024u * 1024u)
#define SNAP_MAX_ITEMS 65536u
#define SNAP_OWNER_NONE UINT32_MAX
#define SNAP_OWNER_ORPHAN (UINT32_MAX - 1u)
#define SNAP_DORMANT_POOL_COUNT 11u
#define SNAP_DORMANT_SLOT_LIMIT 4096u
#define SNAP_INI_MAGIC 0x31494e49u /* INI1 */
#define SNAP_STATIC_STRING_LIMIT 131072u
#define SNAP_STRING_REF 0x80000000u

typedef struct {
    uint8_t *p; size_t n, cap; bool ok; const char *failure;
    const Strg *strg; uint32_t *stringIndex; uint32_t stringMask;
    size_t stringBytes;
} Writer;
typedef struct {
    const uint8_t *p; size_t n, pos; bool ok;
    const Strg *strg; bool stringRefs;
    size_t stringBytes;
} Reader;
static void put(Writer *w, const void *p, size_t n) {
    if (!w->ok) return;
    if (n > SNAP_LIMIT - w->n) {
        w->ok = false; w->failure = "Runner snapshot exceeds the size limit"; return;
    }
    if (n == 0) return;
    if (w->n + n > w->cap) {
        size_t cap = w->cap ? w->cap : 1024;
        while (cap < w->n + n) cap *= 2;
        if (cap > SNAP_LIMIT) cap = SNAP_LIMIT;
        uint8_t *next = (uint8_t *)realloc(w->p, cap);
        if (!next) { w->ok = false; w->failure = "Out of memory"; return; }
        w->p = next; w->cap = cap;
    }
    memcpy(w->p + w->n, p, n); w->n += n;
}
static void get(Reader *r, void *p, size_t n) {
    if (!r->ok || n > r->n - r->pos) { r->ok = false; return; }
    if (n == 0) return;
    memcpy(p, r->p + r->pos, n); r->pos += n;
}
static void put32(Writer *w, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(w, b, 4);
}
static uint32_t get32(Reader *r) {
    uint8_t b[4] = {0}; get(r, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static void put64(Writer *w, uint64_t v) { put32(w, (uint32_t)v); put32(w, (uint32_t)(v >> 32)); }
static uint64_t get64(Reader *r) { uint64_t lo = get32(r), hi = get32(r); return lo | hi << 32; }
static uint32_t checksum(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
static bool countOK(uint32_t n) { return n <= SNAP_MAX_ITEMS; }

/* Pointer-free scalar portions of Instance. The omitted gaps contain the
 * static-parent pointer, collision-cell cache, self-vars and physics body. */
#define I_A offsetof(Instance, x)
#define I_B offsetof(Instance, collisionCells)
#define I_C offsetof(Instance, spriteIndex)
#define I_D offsetof(Instance, physicsBody)
#define I_E offsetof(Instance, physicsContact)
typedef struct {
    uint32_t id;
    int32_t object, room;
    uint8_t a[I_B - I_A], b[I_D - I_C], c[sizeof(Instance) - I_E];
} InstancePlain;
static InstancePlain instancePlain(const Instance *inst) {
    InstancePlain p = {0};
    p.id = inst->instanceId; p.object = inst->objectIndex; p.room = inst->roomIndex;
    memcpy(p.a, (const uint8_t *)inst + I_A, sizeof(p.a));
    memcpy(p.b, (const uint8_t *)inst + I_C, sizeof(p.b));
    memcpy(p.c, (const uint8_t *)inst + I_E, sizeof(p.c));
    return p;
}
static void setInstancePlain(Instance *inst, const InstancePlain *p) {
    inst->instanceId = p->id; inst->objectIndex = p->object; inst->roomIndex = p->room;
    memcpy((uint8_t *)inst + I_A, p->a, sizeof(p->a));
    memcpy((uint8_t *)inst + I_C, p->b, sizeof(p->b));
    memcpy((uint8_t *)inst + I_E, p->c, sizeof(p->c));
    inst->spatialGridDirty = false;
    inst->collisionCellsTrusted = 0;
    inst->lastCollisionQueryId = 0;
}

typedef struct {
    int32_t roomIndex, roomOrder, frame, pendingRoom;
    uint32_t nextInstanceId, collisionQueryCounter;
    bool gameStartFired, viewsEnabled, drawBackgroundColor, appSurfaceEnabled;
    bool appSurfaceAutoDraw, usingAppSurface, drawEnabled, dateTimeLocal;
    bool collisionCompatibilityMode, applyOffsetForPrimitives;
    int32_t applicationWidth, applicationHeight, oldApplicationWidth, oldApplicationHeight;
    int32_t guiWidth, guiHeight, cursorSprite, cursorSpriteSubimage, currentCursor;
    int32_t nextLayerId, nextLayerDrawOrder, nextCallLaterId;
    int32_t asyncBufferNextRequestId, xboxAsyncIdCounter;
    int32_t lastMusicInstance, viewCurrent;
    int32_t nextDynamicVarID, currentVertexFormatId, vertexBufferCount;
    uint32_t vertexFormatBit;
    int32_t surfaceStack[MAX_SURFACES];
    uint32_t backgroundColor;
    GMLReal score, lives, health, deltaTime, gameSpeedOverride;
    GMLReal mpPotMaxrot, mpPotStep, mpPotAhead;
    bool mpPotOnSpot;
    Random random;
    uint64_t elapsedGameNanos;
    float gms2FPS;
    RuntimeView views[MAX_VIEWS];
    GMLCamera defaultCameras[MAX_DEFAULT_ROOM_CAMERAS], userCameras[MAX_USER_CAMERAS];
    GMLCamera surfaceCamera, guiCamera;
    RuntimeBackground backgrounds[8];
    RunnerKeyboardState keyboard;
    RunnerGamepadState gamepads;
    RunnerMouseState mouse;
    uint8_t rendererBefore[offsetof(Renderer, runner) - offsetof(Renderer, drawColor)];
    /* Freeze the pre-D3D tail, including its historical pointer-alignment
     * padding. Growing Renderer must never grow the snapshot or copy pointers.
     * Five matrices, shader, filter+padding, blend factors, camera = 348 bytes. */
    uint8_t rendererAfter[sizeof(void *) == 8 ? 352 : 348];
#ifdef ENABLE_SW_RENDERER
    SWRendererSnapshotState swState;
#endif
} RunnerPlain;
typedef char SnapshotRendererLayoutCheck[
    offsetof(Renderer, cameraCurrent) + sizeof(int32_t) -
    offsetof(Renderer, gmlMatrices) == 348 ? 1 : -1];
typedef struct { uint8_t byte; RunnerPlain plain; } SnapshotPlainAlignment;

/* The upstream-sync candidate accidentally wrote a larger revision-9 POD.
 * Its D3D pointer is untrusted padding on input, never a restorable address. */
#define SNAP_EXPANDED_RENDERER_BYTES (sizeof(void *) == 8 ? 368u : 360u)
static size_t expandedPlainSize(void) {
    size_t size = offsetof(RunnerPlain, rendererAfter) + SNAP_EXPANDED_RENDERER_BYTES;
#ifdef ENABLE_SW_RENDERER
    size += sizeof(SWRendererSnapshotState);
#endif
    size_t alignment = offsetof(SnapshotPlainAlignment, plain);
    return (size + alignment - 1) / alignment * alignment;
}

static RunnerPlain makePlain(const Runner *r) {
    RunnerPlain p = {0};
    p.roomIndex = r->currentRoomIndex; p.roomOrder = r->currentRoomOrderPosition;
    p.frame = r->frameCount; p.pendingRoom = r->pendingRoom;
    p.nextInstanceId = r->nextInstanceId; p.collisionQueryCounter = r->collisionQueryCounter;
    p.gameStartFired = r->gameStartFired; p.viewsEnabled = r->viewsEnabled;
    p.drawBackgroundColor = r->drawBackgroundColor; p.backgroundColor = r->backgroundColor;
    p.appSurfaceEnabled = r->appSurfaceEnabled; p.appSurfaceAutoDraw = r->appSurfaceAutoDraw;
    p.usingAppSurface = r->usingAppSurface; p.drawEnabled = r->drawEnabled;
    p.dateTimeLocal = r->dateTimeLocal; p.collisionCompatibilityMode = r->collisionCompatibilityMode;
    p.applyOffsetForPrimitives = r->applyOffsetForPrimitives;
    p.applicationWidth = r->applicationWidth; p.applicationHeight = r->applicationHeight;
    p.oldApplicationWidth = r->oldApplicationWidth; p.oldApplicationHeight = r->oldApplicationHeight;
    p.guiWidth = r->guiWidth; p.guiHeight = r->guiHeight;
    p.cursorSprite = r->cursorSprite; p.cursorSpriteSubimage = r->cursorSpriteSubimage;
    p.currentCursor = r->currentCursor; p.nextLayerId = r->nextLayerId;
    p.nextLayerDrawOrder = r->nextLayerDrawOrder; p.nextCallLaterId = r->nextCallLaterId;
    p.asyncBufferNextRequestId = r->asyncBufferNextRequestId;
    p.xboxAsyncIdCounter = r->xboxAsyncIdCounter;
    p.lastMusicInstance = r->lastMusicInstance; p.viewCurrent = r->viewCurrent;
    p.nextDynamicVarID = r->vmContext->nextDynamicVarID;
    p.currentVertexFormatId = r->currentVertexFormatId;
    p.vertexBufferCount = r->vertexBufferCount;
    p.vertexFormatBit = r->vertexFormatBit;
    memcpy(p.surfaceStack, r->surfaceStack, sizeof(p.surfaceStack));
    p.score = r->score; p.lives = r->lives; p.health = r->health;
    p.deltaTime = r->deltaTime; p.gameSpeedOverride = r->gameSpeedOverride;
    p.mpPotMaxrot = r->mpPotMaxrot; p.mpPotStep = r->mpPotStep;
    p.mpPotAhead = r->mpPotAhead; p.mpPotOnSpot = r->mpPotOnSpot;
    p.random = r->random;
    uint64_t now = nowNanos();
    p.elapsedGameNanos = now >= r->gameStartTime ? now - r->gameStartTime : 0;
    p.gms2FPS = r->dataWin->gen8.gms2FPS;
    memcpy(p.views, r->views, sizeof(p.views));
    memcpy(p.defaultCameras, r->defaultCameras, sizeof(p.defaultCameras));
    memcpy(p.userCameras, r->userCameras, sizeof(p.userCameras));
    p.surfaceCamera = r->surfaceCamera; p.guiCamera = r->guiCamera;
    memcpy(p.backgrounds, r->backgrounds, sizeof(p.backgrounds));
    if (r->keyboard) p.keyboard = *r->keyboard;
    if (r->gamepads) p.gamepads = *r->gamepads;
    if (r->mouse) p.mouse = *r->mouse;
    memcpy(p.rendererBefore, (const uint8_t *)r->renderer + offsetof(Renderer, drawColor), sizeof(p.rendererBefore));
    memcpy(p.rendererAfter, (const uint8_t *)r->renderer + offsetof(Renderer, gmlMatrices), 348);
#ifdef ENABLE_SW_RENDERER
    SWRenderer_snapshotGetState(r->renderer, &p.swState);
#endif
    return p;
}

typedef enum { SLOT_GLOBAL, SLOT_INSTANCE, SLOT_MAP, SLOT_LIST, SLOT_ARRAY, SLOT_SAVED, SLOT_STRUCT } SlotKind;
typedef struct { SlotKind kind; uint32_t container, index; char *key; } SlotRef;
typedef struct { GMLArray *array; uint32_t ownerSlot; } ArrayRef;
typedef struct { char *key; int32_t value; } SnapshotVarName;
/* The pointer-free part of SavedRoomState. */
typedef struct {
    RuntimeBackground backgrounds[8];
    RuntimeView views[MAX_VIEWS];
    GMLCamera defaultCameras[MAX_DEFAULT_ROOM_CAMERAS];
    uint32_t backgroundColor;
    bool drawBackgroundColor, viewsEnabled;
} SavedRoomPlain;

typedef struct {
    uint32_t width, height, speed, tileCount;
    bool persistent, payloadLoaded;
    RoomTile *tiles, *displacedTiles;
    Room stagedPayload;
    bool stagedLoaded;
    /* What a persistent room left behind (SavedRoomState), revision 6. */
    bool hasSaved;
    SavedRoomPlain saved;
    TileLayerMapEntry *savedTileLayers;
    Instance **savedInstances;
} SnapshotRoom;

typedef struct SnapshotLegacyTiles { uint32_t room, layer, count; RoomTile *tiles; } SnapshotLegacyTiles;
typedef struct { uint32_t room, layer, count; uint32_t *cells; } SnapshotTilemap;
struct RunnerSnapshot {
    bool applied;
    RunnerPlain plain;
    GMLReal mathEpsilon;
    float drawDepth;
    bool legacyTextureCoordinates;
    Room *room;
    Instance *global;
    Instance **instances;
    Instance **savedInstances; /* every room's, in order; owned by roomStates */
    DsMapEntry **maps;
    DsList *lists;
    IniFile *currentIni, *cachedIni;
    char *currentIniPath, *cachedIniPath;
    bool currentIniDirty;
    DsQueue *queues;
    DsStack *stacks;
    DsPriority *priorities;
    DsGrid *grids;
    GmlBuffer *buffers;
    MpGrid *mpGrids;
    ParticleSystem *particleSystems;
    ParticleType *particleTypes;
    AudioEmitter *emitters;
    VmVertexFormat **vertexFormats;
    Buffer_Vertex **vertexBuffers;
    SnapshotVarName *varNames;
    GMLArray **arrays; /* one temporary strong ref per array */
    uint32_t *owners;
    /* Revision 7 (GMS2): methods hold one table ref each until freed; structs
     * are the registry, in order, each counting the registry's +1. */
    GMLMethod **methods;
    Instance **structs;
    bool *staticInitialized;
    /* GMS 2 rooms: the current room's runtime layers, ready to install, and
     * the legacy tiles of every loaded room (layer_tile_x/y move them). */
    RuntimeLayer *layers;
    bool hasLayers; /* revision 7: also, the live vertex resources are kept */
    struct SnapshotLegacyTiles *legacyTiles;
    SnapshotTilemap *tilemaps;
    SlotRef *slots;
    SnapshotRoom *roomStates;
    uint32_t roomCount;
    TileLayerMapEntry *tileLayers;
    bool *roomInitialized;
    InstanceIdEntry *byId;
    Instance ***byObject, ***byExact;
    SpatialGrid *grid;
    /* After apply these are the displaced live allocations, freed by Free. */
    Instance *oldGlobal;
    Instance **oldInstances;
    Instance **oldStructs;
    RuntimeLayer *oldLayers;
    DsMapEntry **oldMaps;
    DsList *oldLists;
    IniFile *oldCurrentIni;
    char *oldCurrentIniPath;
    DsQueue *oldQueues;
    DsStack *oldStacks;
    DsPriority *oldPriorities;
    DsGrid *oldGrids;
    GmlBuffer *oldBuffers;
    MpGrid *oldMpGrids;
    ParticleSystem *oldParticleSystems;
    ParticleType *oldParticleTypes;
    AudioEmitter *oldEmitters;
    VmVertexFormat **oldVertexFormats;
    Buffer_Vertex **oldVertexBuffers;
    SnapshotVarName *oldVarNames;
    TileLayerMapEntry *oldTileLayers;
    InstanceIdEntry *oldById;
    Instance ***oldByObject, ***oldByExact;
    SpatialGrid *oldGrid;
    IniFile *oldCachedIni;
    char *oldCachedIniPath;
    uint32_t objectCount;
};

static void freeValues(Instance *global, Instance **instances, DsMapEntry **maps, DsList *lists) {
    /* Arrays can refer across containers, so release every owner before the
     * registry's final array refs are dropped. */
    if (global) Instance_free(global);
    for (ptrdiff_t i = 0; i < arrlen(instances); i++) Instance_free(instances[i]);
    arrfree(instances);
    for (ptrdiff_t i = 0; i < arrlen(maps); i++) {
        DsMapEntry *map = maps[i];
        for (ptrdiff_t j = 0; j < shlen(map); j++) {
            free(map[j].key); RValue_free(&map[j].value);
        }
        shfree(map);
    }
    arrfree(maps);
    for (ptrdiff_t i = 0; i < arrlen(lists); i++) {
        for (ptrdiff_t j = 0; j < arrlen(lists[i].items); j++) RValue_free(&lists[i].items[j]);
        arrfree(lists[i].items);
    }
    arrfree(lists);
}
static void freeLayers(RuntimeLayer *layers) {
    for (ptrdiff_t i = 0; i < arrlen(layers); i++) Runner_freeRuntimeLayer(&layers[i]);
    arrfree(layers);
}
/* Structs refer to one another and to arrays; release every struct's values
 * before any shell goes, as the runner's own teardown does. */
static void freeStructs(Instance **structs) {
    for (ptrdiff_t i = 0; i < arrlen(structs); i++) {
        structs[i]->structRegistryIndex = -1;
        Instance_freeContents(structs[i]);
    }
    for (ptrdiff_t i = 0; i < arrlen(structs); i++) free(structs[i]);
    arrfree(structs);
}
static void freeDormantPools(DsQueue *queues, DsStack *stacks,
                             DsPriority *priorities, DsGrid *grids,
                             GmlBuffer *buffers, MpGrid *mpGrids,
                             ParticleSystem *systems, ParticleType *types,
                             AudioEmitter *emitters, VmVertexFormat **formats,
                             Buffer_Vertex **vertexBuffers) {
    for (ptrdiff_t i = 0; i < arrlen(queues); i++) arrfree(queues[i].items);
    for (ptrdiff_t i = 0; i < arrlen(stacks); i++) arrfree(stacks[i].items);
    for (ptrdiff_t i = 0; i < arrlen(priorities); i++) arrfree(priorities[i].items);
    for (ptrdiff_t i = 0; i < arrlen(grids); i++) free(grids[i].items);
    for (ptrdiff_t i = 0; i < arrlen(buffers); i++) free(buffers[i].data);
    for (ptrdiff_t i = 0; i < arrlen(mpGrids); i++) free(mpGrids[i].cells);
    for (ptrdiff_t i = 0; i < arrlen(systems); i++) {
        arrfree(systems[i].particles); arrfree(systems[i].emitters);
    }
    for (ptrdiff_t i = 0; i < arrlen(emitters); i++) arrfree(emitters[i].voices);
    arrfree(queues); arrfree(stacks); arrfree(priorities); arrfree(grids);
    arrfree(buffers); arrfree(mpGrids); arrfree(systems); arrfree(types);
    arrfree(emitters); arrfree(formats); arrfree(vertexBuffers);
}
static void freeBuckets(Instance ***buckets, uint32_t count) {
    if (!buckets) return;
    for (uint32_t i = 0; i < count; i++) arrfree(buckets[i]);
    free(buckets);
}
static void freeVarNames(SnapshotVarName *names) {
    for (ptrdiff_t i = 0; i < shlen(names); i++) free(names[i].key);
    shfree(names);
}
void Runner_snapshotFree(RunnerSnapshot *s) {
    if (!s) return;
    if (s->applied) {
        freeValues(s->oldGlobal, s->oldInstances, s->oldMaps, s->oldLists);
        freeDormantPools(s->oldQueues, s->oldStacks, s->oldPriorities,
                         s->oldGrids, s->oldBuffers, s->oldMpGrids,
                         s->oldParticleSystems, s->oldParticleTypes,
                         s->oldEmitters, s->oldVertexFormats, s->oldVertexBuffers);
        freeVarNames(s->oldVarNames);
        hmfree(s->oldTileLayers); hmfree(s->oldById);
        freeBuckets(s->oldByObject, s->objectCount);
        freeBuckets(s->oldByExact, s->objectCount);
        if (s->oldGrid) SpatialGrid_free(s->oldGrid);
        if (s->oldCurrentIni) Ini_free(s->oldCurrentIni);
        free(s->oldCurrentIniPath);
        if (s->oldCachedIni) Ini_free(s->oldCachedIni);
        free(s->oldCachedIniPath);
    } else {
        freeValues(s->global, s->instances, s->maps, s->lists);
        freeDormantPools(s->queues, s->stacks, s->priorities, s->grids,
                         s->buffers, s->mpGrids, s->particleSystems,
                         s->particleTypes, s->emitters, s->vertexFormats,
                         s->vertexBuffers);
        freeVarNames(s->varNames);
        hmfree(s->tileLayers); hmfree(s->byId);
        freeBuckets(s->byObject, s->objectCount);
        freeBuckets(s->byExact, s->objectCount);
        if (s->grid) SpatialGrid_free(s->grid);
        if (s->currentIni) Ini_free(s->currentIni);
        free(s->currentIniPath);
        if (s->cachedIni) Ini_free(s->cachedIni);
        free(s->cachedIniPath);
    }
    if (s->roomStates) for (uint32_t i = 0; i < s->roomCount; i++) {
        SnapshotRoom *state = &s->roomStates[i];
        for (ptrdiff_t j = 0; j < arrlen(state->savedInstances); j++)
            Instance_free(state->savedInstances[j]);
        arrfree(state->savedInstances); hmfree(state->savedTileLayers);
    }
    arrfree(s->savedInstances);
    for (ptrdiff_t i = 0; i < arrlen(s->arrays); i++) GMLArray_decRef(s->arrays[i]);
    arrfree(s->arrays); arrfree(s->owners); arrfree(s->slots);
    for (ptrdiff_t i = 0; i < arrlen(s->methods); i++) GMLMethod_decRef(s->methods[i]);
    arrfree(s->methods);
    /* Last: an array released above may still drop a struct reference. */
    freeStructs(s->applied ? s->oldStructs : s->structs);
    free(s->staticInitialized);
    if (s->applied) freeLayers(s->oldLayers); else freeLayers(s->layers);
    for (ptrdiff_t i = 0; i < arrlen(s->legacyTiles); i++) free(s->legacyTiles[i].tiles);
    arrfree(s->legacyTiles);
    for (ptrdiff_t i = 0; i < arrlen(s->tilemaps); i++) free(s->tilemaps[i].cells);
    arrfree(s->tilemaps);
    if (s->roomStates) for (uint32_t i = 0; i < s->roomCount; i++) {
        SnapshotRoom *state = &s->roomStates[i];
        if (state->stagedLoaded) DataWin_freeRoomPayload(&state->stagedPayload);
        free(s->applied ? state->displacedTiles : state->tiles);
    }
    free(s->roomStates);
    free(s->roomInitialized); free(s);
}

static bool hasLiveDynamicSprite(const DataWin *dw) {
    /* The SPRT parser records the original boundary; renderer-created slots
     * begin after it, even when their generated names are changed or absent. */
    for (uint32_t i = dw->sprt.parsedCount; i < dw->sprt.count; i++)
        if (dw->sprt.sprites[i].textureCount) return true;
    return false;
}

/* Every refusal the preflight finds, for the desktop coverage probe. The
 * count is the total; only the first max are stored for the caller. */
#define SNAP_MAX_BLOCKERS 64   /* more distinct refusals than the preflight has */
typedef struct {
    const char **out; int max; int count;
    const char *seen[SNAP_MAX_BLOCKERS];
} BlockerList;

static void noteBlocker(BlockerList *list, const char *msg) {
    for (int i = 0; i < list->count && i < SNAP_MAX_BLOCKERS; i++)
        if (list->seen[i] == msg) return;
    if (list->count < SNAP_MAX_BLOCKERS) list->seen[list->count] = msg;
    if (list->count < list->max) list->out[list->count] = msg;
    list->count++;
}

/* With a list, every check runs and each distinct refusal is noted; without
 * one, the first refusal is returned through reason, as capture needs. */
/* Vertex resources that games make once at start (GMS 2's __init_d3d makes
 * a format and an empty buffer) are not saved: a load keeps the live ones,
 * and only if they are the same as when the state was taken. A format is
 * summed by its definition; a buffer must not have been written to. */
static uint32_t vertexFormatSum(const VmVertexFormat *f) {
    uint32_t h = 2166136261u;
#define MIX(v) do { uint32_t v_ = (uint32_t)(v); for (int b_ = 0; b_ < 4; b_++) { h = (h ^ (v_ & 255u)) * 16777619u; v_ >>= 8; } } while (0)
    MIX(f->id); MIX(f->count); MIX(f->bitMask); MIX(f->size); MIX(f->yyFVF);
    for (uint32_t i = 0; f->format && i < f->count; i++) {
        MIX(f->format[i].offset); MIX(f->format[i].type); MIX(f->format[i].usage); MIX(f->format[i].bit);
    }
#undef MIX
    return h;
}
static bool vertexBufferUnused(const Buffer_Vertex *b) {
    return b->vertexCount == 0 && !b->frozen && b->streamCurrent == b->streamStart &&
        !b->pFrozenVB && b->currentMask == 0 && !b->pCurrentFormatVFRelease;
}
/* Bound the multiplication before using the parsed tilemap's dimensions. */
static bool tilemapCellsValid(const RoomLayerTilesData *map) {
    return map && (!map->tilesY || map->tilesX <= SNAP_LIMIT / sizeof(uint32_t) / map->tilesY) &&
        (!map->tilesX || !map->tilesY || map->tileData);
}
/* The parts of a runtime layer and its elements that are not pointers. */
typedef struct {
    uint32_t id, drawOrder; int32_t depth;
    float xOffset, yOffset, hSpeed, vSpeed;
    int32_t beginScript, endScript, shaderIndex;
    uint8_t automaticDepth, visible, dynamic, hasName;
} LayerPlain;
typedef struct { uint32_t id, type, blend; float alpha; uint8_t visible, owned; } ElementPlain;
/* An element that borrows from the room's parsed layers is saved as the
 * room layer and item it borrows: the tilemap of a tiles layer, a legacy tile
 * or a sprite instance (for its name) of an assets layer. */
static bool elementSource(const Room *room, const RuntimeLayerElement *el, int32_t *layer, int32_t *item) {
    *layer = -1; *item = -1;
    switch (el->type) {
        case RuntimeLayerElementType_Background: return el->backgroundElement != NULL;
        case RuntimeLayerElementType_Instance: return true;
        case RuntimeLayerElementType_Sprite:
            if (!el->spriteElement) return false;
            if (!el->spriteElement->name) return true;
            break;
        case RuntimeLayerElementType_Tilemap: if (!el->tilemapData) return false; break;
        case RuntimeLayerElementType_Tile:
            if (!el->tileElement) return false;
            if (el->tileElementOwned) return true;
            break;
        default: return false;
    }
    for (uint32_t l = 0; room->layers && l < room->layerCount; l++) {
        const RoomLayer *rl = &room->layers[l];
        if (el->type == RuntimeLayerElementType_Tilemap) {
            if (rl->tilesData == el->tilemapData) { *layer = (int32_t)l; return true; }
            continue;
        }
        const RoomLayerAssetsData *a = rl->assetsData;
        if (rl->type != RoomLayerType_Assets || !a) continue;
        if (el->type == RuntimeLayerElementType_Tile && a->legacyTiles &&
            el->tileElement >= a->legacyTiles && el->tileElement < a->legacyTiles + a->legacyTileCount) {
            *layer = (int32_t)l; *item = (int32_t)(el->tileElement - a->legacyTiles); return true;
        }
        if (el->type == RuntimeLayerElementType_Sprite)
            for (uint32_t j = 0; a->sprites && j < a->spriteCount; j++)
                if (a->sprites[j].name == el->spriteElement->name) {
                    *layer = (int32_t)l; *item = (int32_t)j; return true;
                }
    }
    return false;
}
static bool eligible(Runner *r, const char **reason, bool forCapture, BlockerList *all) {
#define REFUSE(msg) do { if (all) noteBlocker(all, msg); \
        else { if (reason) *reason = msg; return false; } } while (0)
    if (!r || !r->dataWin || !r->vmContext || !r->renderer || !r->currentRoom) {
        /* Nothing below can be examined without these. */
        if (all) noteBlocker(all, "Runner is not ready");
        else if (reason) *reason = "Runner is not ready";
        return false;
    }
    /* GMS 2.3 (WAD 17) values and current-room runtime layers are revision 7. */
    if (r->dataWin->gen8.wadVersion != 16 && r->dataWin->gen8.wadVersion != 17)
        REFUSE("Only WAD 16 and 17 state is supported");
    bool spritesValid = r->dataWin->sprt.parsedCount <= r->dataWin->sprt.count &&
        (!r->dataWin->sprt.count || r->dataWin->sprt.sprites);
    if (!spritesValid)
        REFUSE("Invalid sprite registry");
    if (r->currentRoomIndex < 0 || (uint32_t)r->currentRoomIndex >= r->dataWin->room.count)
        REFUSE("Invalid room");
    if (forCapture && !r->currentRoom->payloadLoaded) REFUSE("Current room payload is unavailable");
    if (forCapture && r->paused) REFUSE("Paused runner is unsupported for capture");
    if (forCapture && (isnan(r->renderer->drawDepth) || isinf(r->renderer->drawDepth)))
        REFUSE("Renderer depth is not finite");
    if (spritesValid && hasLiveDynamicSprite(r->dataWin))
        REFUSE("Dynamic sprite is unsupported");
    if (forCapture && !SpatialGrid_dimensionsValid(r->currentRoom->width,
                                                    r->currentRoom->height))
        REFUSE("Room dimensions exceed the collision grid limit");
    if (r->pendingRoom != -1) REFUSE("Room transition is pending");
    /* These visual-only offsets live outside RunnerPlain. Ordinary gameplay
     * uses the defaults; refuse a debug camera rather than restore it wrong. */
    if (r->freeCamPanX != 0.0f || r->freeCamPanY != 0.0f ||
        (r->freeCamZoom != 0.0f && r->freeCamZoom != 1.0f))
        REFUSE("Free camera offset is unsupported");
    if (r->vmContext->currentCallFrame || r->vmContext->envStack || r->vmContext->stack.top)
        REFUSE("Game script is still executing");
    if (!r->vmContext->globalScopeInstance) REFUSE("Global scope is missing");
    if (arrlen(r->instanceSnapshots))
        REFUSE("Instance dispatch is still active");
    if (r->physics) REFUSE("Active physics is unsupported");
    if (r->physicsRooms) {
        for (uint32_t i = 0; i < r->dataWin->room.count; i++)
            if (r->physicsRooms[i]) REFUSE("Physics room is unsupported");
    }
    if (r->vmContext->staticStructs) {
        for (uint32_t i = 0; i < r->dataWin->code.count; i++)
            if (r->vmContext->staticStructs[i]) REFUSE("Static structs are unsupported");
    }
    for (uint32_t i = 0; i < r->dataWin->room.count; i++) {
        SavedRoomState *st = r->savedRoomStates ? &r->savedRoomStates[i] : NULL;
        Room *room = &r->dataWin->room.rooms[i];
        if (forCapture && st && arrlen(st->runtimeLayers))
            REFUSE("Saved room layers are unsupported");
        for (ptrdiff_t j = 0; forCapture && st && j < arrlen(st->instances); j++) {
            Instance *inst = st->instances[j];
            if (!inst || inst->destroyed || inst->objectIndex < 0 ||
                (uint32_t)inst->objectIndex >= r->dataWin->objt.count || inst->physicsBody)
                REFUSE("Unsupported saved room instance");
        }
        if (forCapture && !SpatialGrid_dimensionsValid(room->width, room->height))
            REFUSE("Room dimensions exceed the collision grid limit");
        if (forCapture && room->payloadLoaded && room->tileCount && !room->tiles)
            REFUSE("Loaded room tiles are unavailable");
        for (uint32_t l = 0; forCapture && room->payloadLoaded && room->layers && l < room->layerCount; l++)
            if (room->layers[l].type == RoomLayerType_Tiles && !tilemapCellsValid(room->layers[l].tilesData))
                REFUSE("Loaded room tilemap cells are unavailable or too large");
    }
    /* The live layers are replaced by a load, so only a capture checks them. */
    for (ptrdiff_t i = 0; forCapture && i < arrlen(r->runtimeLayers); i++)
        for (ptrdiff_t j = 0; j < arrlen(r->runtimeLayers[i].elements); j++) {
            int32_t layer, item;
            if (!elementSource(r->currentRoom, &r->runtimeLayers[i].elements[j], &layer, &item)) {
                REFUSE("Unsupported layer element"); i = arrlen(r->runtimeLayers); break;
            }
        }
    if (r->currentIni && r->currentIni == r->cachedIni)
        REFUSE("Aliased INI cache is unsupported");
    for (int i = 0; i < MAX_OPEN_TEXT_FILES; i++) if (r->openTextFiles[i].isOpen) REFUSE("Open text file");
    for (int i = 0; i < MAX_OPEN_BINARY_FILES; i++) if (r->openBinaryFiles[i].isOpen) REFUSE("Open binary file");
    if (arrlen(r->fileFindResults)) REFUSE("File enumeration is active");
    if (arrlen(r->asyncBufferGroupOps) || arrlen(r->asyncSaveLoadQueue) ||
        r->asyncBufferGroupActive || r->asyncBufferGroupName)
        REFUSE("Async file operation is pending");
    if (r->xboxAccountPickerPendingId >= 0 && r->xboxAsyncIdCounter != 0)
        REFUSE("Async dialog is pending");
    if (arrlen(r->callLaterEntries)) REFUSE("Pending callbacks are unsupported");
    if (shlen(r->disabledObjects)) REFUSE("Object filtering is unsupported");
    size_t dormantSlots = (size_t)arrlen(r->dsQueuePool) + arrlen(r->dsStackPool) +
        arrlen(r->dsPriorityPool) + arrlen(r->dsGridPool) + arrlen(r->gmlBufferPool) +
        arrlen(r->mpGridPool) + arrlen(r->particleSystemPool) +
        arrlen(r->particleTypePool) + arrlen(r->audioEmitters) +
        arrlen(r->vertexFormats);
    /* vertexBuffers is a plain array of vertexBufferCount slots (see
     * allocBufferVertex), not an stb_ds array: arrlen() on it reads outside
     * the allocation. Capture refuses any nonzero count just below. */
    if (r->vertexBuffers && r->vertexBufferCount > 0) dormantSlots += (size_t)r->vertexBufferCount;
    if (dormantSlots > SNAP_DORMANT_SLOT_LIMIT) REFUSE("Dormant pool history is too large");
    for (ptrdiff_t i = 0; i < arrlen(r->dsQueuePool); i++)
        if (!r->dsQueuePool[i].freed || arrlen(r->dsQueuePool[i].items)) REFUSE("Live DS queue is unsupported");
    for (ptrdiff_t i = 0; i < arrlen(r->dsStackPool); i++)
        if (!r->dsStackPool[i].freed || arrlen(r->dsStackPool[i].items)) REFUSE("Live DS stack is unsupported");
    for (ptrdiff_t i = 0; i < arrlen(r->dsPriorityPool); i++)
        if (!r->dsPriorityPool[i].freed || arrlen(r->dsPriorityPool[i].items)) REFUSE("Live DS priority is unsupported");
    for (ptrdiff_t i = 0; i < arrlen(r->dsGridPool); i++)
        if (!r->dsGridPool[i].freed || r->dsGridPool[i].items) REFUSE("Live DS grid is unsupported");
    for (ptrdiff_t i = 0; i < arrlen(r->gmlBufferPool); i++)
        if (r->gmlBufferPool[i].isValid || r->gmlBufferPool[i].data) REFUSE("Live GML buffer is unsupported");
    for (ptrdiff_t i = 0; i < arrlen(r->mpGridPool); i++)
        if (r->mpGridPool[i].inUse || r->mpGridPool[i].cells) REFUSE("Live motion grid is unsupported");
    for (ptrdiff_t i = 0; i < arrlen(r->particleSystemPool); i++)
        if (r->particleSystemPool[i].used || arrlen(r->particleSystemPool[i].particles) ||
            arrlen(r->particleSystemPool[i].emitters)) REFUSE("Live particle system is unsupported");
    for (ptrdiff_t i = 0; i < arrlen(r->particleTypePool); i++)
        if (r->particleTypePool[i].used || r->particleTypePool[i].refCount > 0)
            REFUSE("Live particle type is unsupported");
    for (ptrdiff_t i = 0; i < arrlen(r->audioEmitters); i++)
        if (r->audioEmitters[i].active || arrlen(r->audioEmitters[i].voices))
            REFUSE("Live audio emitter is unsupported");
    for (ptrdiff_t i = 0; i < arrlen(r->vertexFormats); i++)
        if (r->vertexFormats[i] && forCapture && !r->vertexFormats[i]->format && r->vertexFormats[i]->count)
            REFUSE("Malformed vertex format");
    for (int32_t i = 0; r->vertexBuffers && i < r->vertexBufferCount; i++)
        if (r->vertexBuffers[i] && forCapture && !vertexBufferUnused(r->vertexBuffers[i]))
            REFUSE("Vertex buffer contents are unsupported");
    if (r->newVertexFormat) REFUSE("Vertex format construction is active");
#ifdef ENABLE_SW_RENDERER
    SWRendererSnapshotState swState;
    if (!SWRenderer_snapshotGetState(r->renderer, &swState))
        REFUSE("Software renderer has unfinished draw work");
#endif
    if (r->renderer->vtable && r->renderer->vtable->surfaceExists) {
        for (int32_t i = 0; i < 64; i++)
            if (i != r->applicationSurfaceId && r->renderer->vtable->surfaceExists(r->renderer, i))
                REFUSE("User surface is unsupported");
    }
    for (ptrdiff_t i = 0; i < arrlen(r->instances); i++) {
        Instance *inst = r->instances[i];
        /* Draw/GUI can destroy a dialogue object after Step's cleanup pass.
         * Keep this quiescent instance, its values and list membership until
         * the next ordinary cleanup; capture must not advance its lifetime. */
        if (!inst || (inst->destroyed && (inst->active || !inst->cleanupEventFired)) || inst->objectIndex < 0 ||
            (uint32_t)inst->objectIndex >= r->dataWin->objt.count || inst->physicsBody)
            REFUSE("Unsupported live instance");
    }
    return !all || all->count == 0;
#undef REFUSE
}
const char *Runner_snapshotCheck(Runner *r) {
    const char *reason = NULL;
    return eligible(r, &reason, true, NULL) ? NULL : reason;
}
int Runner_snapshotBlockers(Runner *r, const char **out, int max) {
    BlockerList all = { out, out && max > 0 ? max : 0, 0, {0} };
    for (int i = 0; i < all.max; i++) out[i] = NULL;
    (void)eligible(r, NULL, true, &all);
    return all.count;
}

typedef struct {
    RValue **slots; GMLArray **arrays;
    GMLMethod **methods; /* revision 7: shared by identity, as GML compares them */
    Runner *runner;
} Graph;
static int arrayIndex(const Graph *g, GMLArray *array) {
    for (ptrdiff_t i = 0; i < arrlen(g->arrays); i++) if (g->arrays[i] == array) return (int)i;
    return -1;
}
/* A method is saved as what made it: a code entry, or the function-table
 * entry behind a built-in or an unresolved name (handleBreakPushRef). */
enum { SNAP_METHOD_CODE, SNAP_METHOD_BUILTIN, SNAP_METHOD_UNRESOLVED };
#if IS_WAD17_OR_HIGHER_ENABLED
static int methodIndex(const Graph *g, GMLMethod *method) {
    for (ptrdiff_t i = 0; i < arrlen(g->methods); i++) if (g->methods[i] == method) return (int)i;
    return -1;
}
static bool methodSource(const Runner *r, const GMLMethod *m, uint8_t *kind, uint32_t *index) {
    const DataWin *dw = r->dataWin;
    if (m->builtin == NULL && m->unresolvedName == NULL) {
        if (m->codeIndex < 0 || (uint32_t)m->codeIndex >= dw->code.count ||
            !dw->code.entries[m->codeIndex].present) return false;
        *kind = SNAP_METHOD_CODE; *index = (uint32_t)m->codeIndex; return true;
    }
    if (m->codeIndex != -1 || !r->vmContext->funcCallCache) return false;
    for (uint32_t i = 0; i < dw->func.functionCount; i++) {
        if (m->builtin && !m->unresolvedName &&
            r->vmContext->funcCallCache[i].builtin == (void *)m->builtin) {
            *kind = SNAP_METHOD_BUILTIN; *index = i; return true;
        }
        if (m->unresolvedName && !m->builtin && dw->func.functions[i].name == m->unresolvedName) {
            *kind = SNAP_METHOD_UNRESOLVED; *index = i; return true;
        }
    }
    return false;
}
#endif
/* Structs are saved as the whole registry, in its order; a value names one
 * by its place there. Static structs, and structs that inherit statics, are
 * not saved. */
static bool structSaved(const Runner *r, const Instance *st) {
    return st && st->objectIndex == STRUCT_OBJECT_INDEX && st->structRegistryIndex >= 0 &&
        st->structRegistryIndex < (int32_t)arrlen(r->structInstances) &&
        r->structInstances[st->structRegistryIndex] == st && !st->staticParent && !st->destroyed;
}
static bool collect(Graph *g, RValue *slot) {
    if ((size_t)arrlen(g->slots) >= SNAP_MAX_ITEMS) return false;
    arrput(g->slots, slot);
    switch (slot->type) {
        case RVALUE_UNDEFINED: case RVALUE_REAL: case RVALUE_INT32:
        case RVALUE_BOOL: case RVALUE_ASSETREF: return true;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64: return true;
#endif
        case RVALUE_STRING: return slot->string != NULL && strlen(slot->string) < SNAP_LIMIT;
        case RVALUE_ARRAY:
            if (!slot->array || (slot->array->type != GML_LEGACY_ARRAY &&
                                 slot->array->type != GML_MODERN_ARRAY)) return false;
            if (arrayIndex(g, slot->array) < 0) {
                if ((size_t)arrlen(g->arrays) >= SNAP_MAX_ITEMS) return false;
                arrput(g->arrays, slot->array);
            }
            return true;
#if IS_WAD17_OR_HIGHER_ENABLED
        case RVALUE_METHOD: {
            uint8_t kind; uint32_t index;
            if (!slot->method || !methodSource(g->runner, slot->method, &kind, &index)) return false;
            if (methodIndex(g, slot->method) < 0) {
                if ((size_t)arrlen(g->methods) >= SNAP_MAX_ITEMS) return false;
                arrput(g->methods, slot->method);
            }
            return true;
        }
#endif
        case RVALUE_STRUCT: return structSaved(g->runner, slot->structInst);
        default: return false;
    }
}
static bool collectMap(Graph *g, IntRValueHashMap *map) {
    for (uint32_t i = 0; i < map->capacity; i++)
        if (map->entries[i].key != INT_RVALUE_HASHMAP_EMPTY_KEY &&
            !collect(g, &map->entries[i].value)) return false;
    return true;
}
static bool hasSavedRoom(const SavedRoomState *st) {
    return st->initialized || arrlen(st->instances) || hmlen(st->tileLayerMap);
}
static bool collectGraph(Runner *r, Graph *g) {
    for (ptrdiff_t i = 0; i < arrlen(r->instances); i++)
        if (!collectMap(g, &r->instances[i]->selfVars)) return false;
    for (uint32_t i = 0; r->savedRoomStates && i < r->dataWin->room.count; i++) {
        SavedRoomState *st = &r->savedRoomStates[i];
        for (ptrdiff_t j = 0; j < arrlen(st->instances); j++)
            if (!collectMap(g, &st->instances[j]->selfVars)) return false;
    }
    if (!collectMap(g, &r->vmContext->globalScopeInstance->selfVars)) return false;
    for (ptrdiff_t i = 0; i < arrlen(r->dsMapPool); i++)
        for (ptrdiff_t j = 0; j < shlen(r->dsMapPool[i]); j++)
            if (!collect(g, &r->dsMapPool[i][j].value)) return false;
    for (ptrdiff_t i = 0; i < arrlen(r->dsListPool); i++)
        for (ptrdiff_t j = 0; j < arrlen(r->dsListPool[i].items); j++)
            if (!collect(g, &r->dsListPool[i].items[j])) return false;
    /* The writer and reader visit values in this same order: array owners
     * are restored by the position of their slot. */
    if ((size_t)arrlen(r->structInstances) > SNAP_MAX_ITEMS) return false;
    for (ptrdiff_t i = 0; i < arrlen(r->structInstances); i++) {
        if (!structSaved(r, r->structInstances[i]) ||
            !collectMap(g, &r->structInstances[i]->selfVars)) return false;
    }
    for (ptrdiff_t a = 0; a < arrlen(g->arrays); a++) {
        GMLArray *array = g->arrays[a];
        if (array->type == GML_MODERN_ARRAY) {
            if (array->modern.length < 0 || array->modern.length > (int32_t)SNAP_MAX_ITEMS) return false;
            for (int32_t i = 0; i < array->modern.length; i++)
                if (!collect(g, &array->modern.data[i])) return false;
            continue;
        }
        if (array->legacy.rowCount < 0 || array->legacy.rowCount > 1024) return false;
        for (int32_t row = 0; row < array->legacy.rowCount; row++) {
            GMLArrayRow *rr = &array->legacy.rows[row];
            if (rr->length < 0 || rr->length > 32000) return false;
            for (int32_t col = 0; col < rr->length; col++)
                if (!collect(g, &rr->data[col])) return false;
        }
    }
    return true;
}
static uint32_t slotIndex(const Graph *g, const void *slot) {
    if (!slot) return SNAP_OWNER_NONE;
    for (ptrdiff_t i = 0; i < arrlen(g->slots); i++)
        if (g->slots[i] == slot) return (uint32_t)i;
    return SNAP_OWNER_NONE;
}
static bool buildStringIndex(Writer *w, const Strg *strg) {
    if (strg->count > SNAP_STATIC_STRING_LIMIT ||
        (strg->count && !strg->strings)) return false;
    if (!strg->count) return true;
    uint32_t capacity = 1;
    while (capacity < strg->count * 2u) capacity *= 2u;
    w->stringIndex = (uint32_t *)calloc(capacity, sizeof(uint32_t));
    if (!w->stringIndex) return false;
    w->strg = strg;
    w->stringMask = capacity - 1u;
    for (uint32_t i = 0; i < strg->count; i++) {
        const char *s = strg->strings[i];
        if (!s) continue;
        uint32_t bucket = checksum((const uint8_t *)s, strlen(s)) & w->stringMask;
        while (w->stringIndex[bucket]) {
            const char *other = strg->strings[w->stringIndex[bucket] - 1u];
            if (strcmp(s, other) == 0) break;
            bucket = (bucket + 1u) & w->stringMask;
        }
        if (!w->stringIndex[bucket]) w->stringIndex[bucket] = i + 1u;
    }
    return true;
}
static void putString(Writer *w, const char *s) {
    if (!w->ok) return;
    size_t n = s ? strlen(s) : 0;
    if (n >= SNAP_LIMIT - w->stringBytes) {
        w->ok = false; w->failure = "Runner snapshot strings exceed the size limit"; return;
    }
    w->stringBytes += n + 1;
    if (s && w->stringIndex) {
        uint32_t bucket = checksum((const uint8_t *)s, n) & w->stringMask;
        while (w->stringIndex[bucket]) {
            uint32_t index = w->stringIndex[bucket] - 1u;
            if (strcmp(s, w->strg->strings[index]) == 0) {
                put32(w, SNAP_STRING_REF | index); return;
            }
            bucket = (bucket + 1u) & w->stringMask;
        }
    }
    put32(w, (uint32_t)n); put(w, s, n);
}
static void putIni(Writer *w, const IniFile *ini, uint32_t *entryBudget) {
    if (!ini || ini->count < 0 || !countOK((uint32_t)ini->count) ||
        (ini->count && !ini->sections)) {
        w->ok = false; w->failure = "Invalid INI section count"; return;
    }
    if ((uint32_t)ini->count > *entryBudget) {
        w->ok = false; w->failure = "INI entry budget exceeded"; return;
    }
    *entryBudget -= (uint32_t)ini->count;
    put32(w, (uint32_t)ini->count);
    for (int i = 0; i < ini->count && w->ok; i++) {
        const IniSection *section = &ini->sections[i];
        if (!section->name || section->count < 0 ||
            !countOK((uint32_t)section->count) ||
            (section->count && (!section->keys || !section->values))) {
            w->ok = false; w->failure = "Invalid INI entry count"; return;
        }
        if ((uint32_t)section->count > *entryBudget) {
            w->ok = false; w->failure = "INI entry budget exceeded"; return;
        }
        *entryBudget -= (uint32_t)section->count;
        putString(w, section->name);
        put32(w, (uint32_t)section->count);
        for (int j = 0; j < section->count && w->ok; j++) {
            if (!section->keys[j] || !section->values[j]) {
                w->ok = false; w->failure = "Invalid INI entry"; return;
            }
            putString(w, section->keys[j]);
            putString(w, section->values[j]);
        }
    }
}
static void putValue(Writer *w, const Graph *g, RValue *v) {
    put32(w, (uint32_t)v->type | ((uint32_t)v->gmlStackType << 8) |
             ((uint32_t)v->assetRefType << 16));
    switch (v->type) {
        case RVALUE_UNDEFINED: break;
        case RVALUE_INT32: case RVALUE_BOOL: case RVALUE_ASSETREF: put32(w, (uint32_t)v->int32); break;
        case RVALUE_REAL: { double d = (double)v->real; put(w, &d, sizeof(d)); break; }
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64: put64(w, (uint64_t)v->int64); break;
#endif
        case RVALUE_STRING: putString(w, v->string); break;
        case RVALUE_ARRAY: put32(w, (uint32_t)arrayIndex(g, v->array)); break;
#if IS_WAD17_OR_HIGHER_ENABLED
        case RVALUE_METHOD: put32(w, (uint32_t)methodIndex(g, v->method)); break;
#endif
        case RVALUE_STRUCT: put32(w, (uint32_t)v->structInst->structRegistryIndex); break;
        default: w->ok = false; w->failure = "Unsupported snapshot value"; break;
    }
}
static void putMap(Writer *w, const Graph *g, IntRValueHashMap *map) {
    put32(w, map->count);
    for (uint32_t i = 0; i < map->capacity; i++) {
        if (map->entries[i].key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
        put32(w, (uint32_t)map->entries[i].key);
        putValue(w, g, &map->entries[i].value);
    }
}

bool Runner_snapshotCapture(Runner *r, uint64_t gameId, uint8_t **data,
                            size_t *size, const char **reason) {
    if (data) *data = NULL;
    if (size) *size = 0;
    if (reason) *reason = NULL;
    if (!data || !size) { if (reason) *reason = "Invalid output"; return false; }
#ifdef PLATFORM_BUSY_TICK
    uint64_t phase[6]; phase[0] = nowNanos();
#endif
    if (!eligible(r, reason, true, NULL)) return false;
    Graph g = {0};
    g.runner = r;
    if (!collectGraph(r, &g)) {
        if (reason) *reason = "Unsupported value graph";
        arrfree(g.slots); arrfree(g.arrays); arrfree(g.methods); return false;
    }
#ifdef PLATFORM_BUSY_TICK
    phase[1] = nowNanos();
#endif
    Writer w = {.ok = true};
    if (!buildStringIndex(&w, &r->dataWin->strg)) {
        if (reason) *reason = "Runner snapshot string dictionary unavailable";
        free(w.stringIndex);
        arrfree(g.slots); arrfree(g.arrays); arrfree(g.methods); return false;
    }
    put32(&w, SNAP_MAGIC); put32(&w, SNAP_VERSION); put64(&w, gameId);
    put32(&w, 0); put32(&w, 0); /* length and checksum filled below */
    RunnerPlain plain = makePlain(r); put(&w, &plain, sizeof(plain));
    /* Explicit extensions leave the historical POD layout frozen. */
    put(&w, &r->mathEpsilon, sizeof(r->mathEpsilon));
    put(&w, &r->renderer->drawDepth, sizeof(float));
    uint8_t legacyCoordinates = r->renderer->legacyTextureCoordinates;
    put(&w, &legacyCoordinates, 1);
    put32(&w, r->dataWin->room.count);
    for (uint32_t i = 0; i < r->dataWin->room.count; i++) {
        uint8_t initialized = r->savedRoomStates && r->savedRoomStates[i].initialized;
        put(&w, &initialized, 1);
    }
    for (uint32_t i = 0; i < r->dataWin->room.count; i++) {
        const Room *room = &r->dataWin->room.rooms[i];
        uint8_t persistent = room->persistent, loaded = room->payloadLoaded;
        put32(&w, room->width); put32(&w, room->height); put32(&w, room->speed);
        put(&w, &persistent, 1); put(&w, &loaded, 1);
        put32(&w, loaded ? room->tileCount : 0);
        if (loaded) put(&w, room->tiles, (size_t)room->tileCount * sizeof(RoomTile));
    }
    put32(&w, (uint32_t)arrlen(g.arrays));
    /* GMS 2.3 bytecode owns arrays by a scope token (BREAK_SETOWNER), not by
     * the slot holding them: revision 7 keeps that token as it is. */
    bool tokenOwners = IS_WAD17_OR_HIGHER(r->vmContext);
    for (ptrdiff_t i = 0; i < arrlen(g.arrays); i++) {
        GMLArray *a = g.arrays[i];
        uint8_t modern = a->type == GML_MODERN_ARRAY;
        put(&w, &modern, 1);
        if (tokenOwners) put64(&w, (uint64_t)(uintptr_t)a->owner);
        if (modern) { put32(&w, (uint32_t)a->modern.length); continue; }
        if (tokenOwners) {
            put32(&w, (uint32_t)a->legacy.rowCount);
            for (int32_t row = 0; row < a->legacy.rowCount; row++)
                put32(&w, (uint32_t)a->legacy.rows[row].length);
            continue;
        }
        uint32_t owner = slotIndex(&g, a->owner);
        /* A dead legacy owner slot must remain non-null and distinct from
         * every live slot, so a shared array still forks on the next write. */
        if (a->owner && (owner == SNAP_OWNER_NONE ||
            ((RValue *)a->owner)->type != RVALUE_ARRAY ||
            ((RValue *)a->owner)->array != a))
            owner = SNAP_OWNER_ORPHAN;
        put32(&w, owner);
        put32(&w, (uint32_t)a->legacy.rowCount);
        for (int32_t row = 0; row < a->legacy.rowCount; row++)
            put32(&w, (uint32_t)a->legacy.rows[row].length);
    }
    put32(&w, (uint32_t)arrlen(g.methods));
#if IS_WAD17_OR_HIGHER_ENABLED
    for (ptrdiff_t i = 0; i < arrlen(g.methods); i++) {
        uint8_t kind = 0; uint32_t index = 0;
        methodSource(r, g.methods[i], &kind, &index);
        put(&w, &kind, 1); put32(&w, index);
        put32(&w, (uint32_t)g.methods[i]->boundInstanceId);
    }
#endif
    put32(&w, (uint32_t)arrlen(r->structInstances));
    for (ptrdiff_t i = 0; i < arrlen(r->structInstances); i++) {
        const Instance *st = r->structInstances[i];
        uint8_t pinned = st->pinned;
        put32(&w, st->instanceId); put(&w, &pinned, 1);
        put32(&w, (uint32_t)st->constructorCodeIndex);
    }
    put32(&w, (uint32_t)hmlen(r->tileLayerMap));
    for (ptrdiff_t i = 0; i < hmlen(r->tileLayerMap); i++)
        put(&w, &r->tileLayerMap[i], sizeof(TileLayerMapEntry));
    put32(&w, (uint32_t)arrlen(r->instances));
    for (ptrdiff_t i = 0; i < arrlen(r->instances); i++) {
        InstancePlain ip = instancePlain(r->instances[i]); put(&w, &ip, sizeof(ip));
        putMap(&w, &g, &r->instances[i]->selfVars);
    }
    /* Revision 6: what each persistent room left behind when the player went
     * elsewhere (persistRoomState), such as the overworld during a battle. */
    uint32_t savedRooms = 0;
    for (uint32_t i = 0; r->savedRoomStates && i < r->dataWin->room.count; i++)
        if (hasSavedRoom(&r->savedRoomStates[i])) savedRooms++;
    put32(&w, savedRooms);
    for (uint32_t i = 0; savedRooms && i < r->dataWin->room.count; i++) {
        SavedRoomState *st = &r->savedRoomStates[i];
        if (!hasSavedRoom(st)) continue;
        SavedRoomPlain sp; memset(&sp, 0, sizeof(sp));
        memcpy(sp.backgrounds, st->backgrounds, sizeof(sp.backgrounds));
        memcpy(sp.views, st->views, sizeof(sp.views));
        memcpy(sp.defaultCameras, st->defaultCameras, sizeof(sp.defaultCameras));
        sp.backgroundColor = st->backgroundColor;
        sp.drawBackgroundColor = st->drawBackgroundColor;
        sp.viewsEnabled = st->viewsEnabled;
        put32(&w, i); put(&w, &sp, sizeof(sp));
        put32(&w, (uint32_t)hmlen(st->tileLayerMap));
        for (ptrdiff_t j = 0; j < hmlen(st->tileLayerMap); j++)
            put(&w, &st->tileLayerMap[j], sizeof(TileLayerMapEntry));
        put32(&w, (uint32_t)arrlen(st->instances));
        for (ptrdiff_t j = 0; j < arrlen(st->instances); j++) {
            InstancePlain ip = instancePlain(st->instances[j]); put(&w, &ip, sizeof(ip));
            putMap(&w, &g, &st->instances[j]->selfVars);
        }
    }
    putMap(&w, &g, &r->vmContext->globalScopeInstance->selfVars);
    put32(&w, (uint32_t)shlen(r->vmContext->varNameMap));
    for (ptrdiff_t i = 0; i < shlen(r->vmContext->varNameMap); i++) {
        putString(&w, r->vmContext->varNameMap[i].key);
        put32(&w, (uint32_t)r->vmContext->varNameMap[i].value);
    }
    put32(&w, (uint32_t)arrlen(r->dsMapPool));
    for (ptrdiff_t i = 0; i < arrlen(r->dsMapPool); i++) {
        DsMapEntry *m = r->dsMapPool[i]; put32(&w, (uint32_t)shlen(m));
        for (ptrdiff_t j = 0; j < shlen(m); j++) {
            putString(&w, m[j].key); putValue(&w, &g, &m[j].value);
        }
    }
    put32(&w, (uint32_t)arrlen(r->dsListPool));
    for (ptrdiff_t i = 0; i < arrlen(r->dsListPool); i++) {
        DsList *list = &r->dsListPool[i]; uint8_t freed = list->freed;
        put(&w, &freed, 1); put32(&w, (uint32_t)arrlen(list->items));
        for (ptrdiff_t j = 0; j < arrlen(list->items); j++) putValue(&w, &g, &list->items[j]);
    }
    for (ptrdiff_t i = 0; i < arrlen(r->structInstances); i++)
        putMap(&w, &g, &r->structInstances[i]->selfVars);
    for (ptrdiff_t a = 0; a < arrlen(g.arrays); a++) {
        GMLArray *array = g.arrays[a];
        if (array->type == GML_MODERN_ARRAY) {
            for (int32_t i = 0; i < array->modern.length; i++) putValue(&w, &g, &array->modern.data[i]);
            continue;
        }
        for (int32_t row = 0; row < array->legacy.rowCount; row++)
            for (int32_t col = 0; col < array->legacy.rows[row].length; col++)
                putValue(&w, &g, &array->legacy.rows[row].data[col]);
    }
    /* Which functions have run their static block (GMS 2.3). */
    uint32_t staticCount = r->vmContext->staticInitialized ? r->dataWin->code.count : 0;
    put32(&w, staticCount);
    for (uint32_t i = 0; i < staticCount; i += 8) {
        uint8_t bits = 0;
        for (uint32_t b = 0; b < 8 && i + b < staticCount; b++)
            if (r->vmContext->staticInitialized[i + b]) bits |= (uint8_t)(1u << b);
        put(&w, &bits, 1);
    }
    /* The current room's runtime layers (GMS 2 rooms always have them). */
    put32(&w, (uint32_t)arrlen(r->runtimeLayers));
    for (ptrdiff_t i = 0; i < arrlen(r->runtimeLayers); i++) {
        const RuntimeLayer *L = &r->runtimeLayers[i];
        LayerPlain lp; memset(&lp, 0, sizeof(lp));
        lp.id = L->id; lp.drawOrder = L->drawOrder; lp.depth = L->depth;
        lp.xOffset = L->xOffset; lp.yOffset = L->yOffset; lp.hSpeed = L->hSpeed; lp.vSpeed = L->vSpeed;
        lp.beginScript = L->beginScript; lp.endScript = L->endScript; lp.shaderIndex = L->shaderIndex;
        lp.automaticDepth = L->automaticDepth; lp.visible = L->visible; lp.dynamic = L->dynamic;
        lp.hasName = L->dynamicName != NULL;
        put(&w, &lp, sizeof(lp));
        if (L->dynamicName) putString(&w, L->dynamicName);
        put32(&w, (uint32_t)arrlen(L->elements));
        for (ptrdiff_t j = 0; j < arrlen(L->elements); j++) {
            const RuntimeLayerElement *el = &L->elements[j];
            int32_t layer, item;
            if (!elementSource(r->currentRoom, el, &layer, &item)) {
                w.ok = false; w.failure = "Unsupported layer element"; break;
            }
            ElementPlain ep; memset(&ep, 0, sizeof(ep));
            ep.id = el->id; ep.type = (uint32_t)el->type; ep.blend = el->blend; ep.alpha = el->alpha;
            ep.visible = el->visible; ep.owned = el->tileElementOwned;
            put(&w, &ep, sizeof(ep));
            put32(&w, (uint32_t)layer); put32(&w, (uint32_t)item);
            switch (el->type) {
                case RuntimeLayerElementType_Background:
                    put(&w, el->backgroundElement, sizeof(RuntimeBackgroundElement)); break;
                case RuntimeLayerElementType_Instance: put32(&w, (uint32_t)el->instanceId); break;
                case RuntimeLayerElementType_Sprite: {
                    RuntimeSpriteElement sp = *el->spriteElement; sp.name = NULL;
                    put(&w, &sp, sizeof(sp)); break;
                }
                case RuntimeLayerElementType_Tile:
                    if (el->tileElementOwned) put(&w, el->tileElement, sizeof(RoomTile));
                    break;
                default: break;
            }
        }
    }
    /* Legacy tiles of every loaded room's assets layers: layer_tile_x/y move
     * them in the parsed room data itself. */
    uint32_t legacyLayers = 0;
    for (uint32_t i = 0; i < r->dataWin->room.count; i++) {
        const Room *room = &r->dataWin->room.rooms[i];
        for (uint32_t l = 0; room->payloadLoaded && room->layers && l < room->layerCount; l++)
            if (room->layers[l].type == RoomLayerType_Assets && room->layers[l].assetsData &&
                room->layers[l].assetsData->legacyTileCount) legacyLayers++;
    }
    put32(&w, legacyLayers);
    for (uint32_t i = 0; i < r->dataWin->room.count; i++) {
        const Room *room = &r->dataWin->room.rooms[i];
        for (uint32_t l = 0; room->payloadLoaded && room->layers && l < room->layerCount; l++) {
            const RoomLayerAssetsData *a = room->layers[l].assetsData;
            if (room->layers[l].type != RoomLayerType_Assets || !a || !a->legacyTileCount) continue;
            put32(&w, i); put32(&w, l); put32(&w, a->legacyTileCount);
            put(&w, a->legacyTiles, (size_t)a->legacyTileCount * sizeof(RoomTile));
        }
    }
    /* Revision 8: tilemap_set[_at_pixel] writes directly into parsed room
     * cells, including layers no longer present in the live runtime list. */
    uint32_t tilemapCount = 0;
    for (uint32_t i = 0; i < r->dataWin->room.count; i++) {
        const Room *room = &r->dataWin->room.rooms[i];
        for (uint32_t l = 0; room->payloadLoaded && room->layers && l < room->layerCount; l++)
            if (room->layers[l].type == RoomLayerType_Tiles) tilemapCount++;
    }
    put32(&w, tilemapCount);
    for (uint32_t i = 0; i < r->dataWin->room.count; i++) {
        const Room *room = &r->dataWin->room.rooms[i];
        for (uint32_t l = 0; room->payloadLoaded && room->layers && l < room->layerCount; l++) {
            if (room->layers[l].type != RoomLayerType_Tiles) continue;
            const RoomLayerTilesData *map = room->layers[l].tilesData;
            put32(&w, i); put32(&w, l); put32(&w, map->tilesX); put32(&w, map->tilesY);
            put(&w, map->tileData, (size_t)map->tilesX * map->tilesY * sizeof(uint32_t));
        }
    }
    /* Vertex resources are described, not saved (vertexFormatSum). The buffer
     * pool is a plain array of vertexBufferCount slots, not an stb array. */
    put32(&w, (uint32_t)arrlen(r->vertexFormats));
    for (ptrdiff_t i = 0; i < arrlen(r->vertexFormats); i++)
        put32(&w, r->vertexFormats[i] ? vertexFormatSum(r->vertexFormats[i]) : 0);
    uint32_t bufferSlots = r->vertexBuffers ? (uint32_t)r->vertexBufferCount : 0;
    put32(&w, bufferSlots);
    for (uint32_t i = 0; i < bufferSlots; i++)
        put32(&w, r->vertexBuffers[i] ? r->vertexBuffers[i]->bufferSize + 1u : 0);
    put32(&w, (uint32_t)arrlen(r->dsQueuePool));
    put32(&w, (uint32_t)arrlen(r->dsStackPool));
    put32(&w, (uint32_t)arrlen(r->dsPriorityPool));
    put32(&w, (uint32_t)arrlen(r->dsGridPool));
    put32(&w, (uint32_t)arrlen(r->gmlBufferPool));
    put32(&w, (uint32_t)arrlen(r->mpGridPool));
    put32(&w, (uint32_t)arrlen(r->particleSystemPool));
    put32(&w, (uint32_t)arrlen(r->particleTypePool));
    put32(&w, (uint32_t)arrlen(r->audioEmitters));
    put32(&w, 0); /* vertex formats and buffers: described above */
    put32(&w, 0);
    put32(&w, SNAP_INI_MAGIC);
    uint32_t iniFlags = (r->currentIni ? 1u : 0u) |
        (r->cachedIni ? 2u : 0u) |
        (r->currentIniPath ? 4u : 0u) |
        (r->cachedIniPath ? 8u : 0u) |
        (r->currentIniDirty ? 16u : 0u);
    put32(&w, iniFlags);
    uint32_t iniEntryBudget = SNAP_MAX_ITEMS;
    if (r->currentIniPath) putString(&w, r->currentIniPath);
    if (r->currentIni) putIni(&w, r->currentIni, &iniEntryBudget);
    if (r->cachedIniPath) putString(&w, r->cachedIniPath);
    if (r->cachedIni) putIni(&w, r->cachedIni, &iniEntryBudget);
    free(w.stringIndex);
    arrfree(g.slots); arrfree(g.arrays); arrfree(g.methods);
    if (!w.ok || w.n > SNAP_LIMIT) {
        if (reason) *reason = w.failure ? w.failure : "Runner snapshot encoding failed";
        free(w.p); return false;
    }
    uint32_t rawLen = (uint32_t)(w.n - 24);
#ifdef PLATFORM_BUSY_TICK
    phase[2] = nowNanos();
#endif
    uint32_t rawCrc = checksum(w.p + 24, rawLen);
#ifdef PLATFORM_BUSY_TICK
    phase[3] = nowNanos();
#endif
    size_t compressedCap = ZSTD_compressBound(rawLen);
    uint8_t *compressed = (uint8_t *)malloc(compressedCap);
    ZSTD_CCtx *compressor = ZSTD_createCCtx();
    size_t compressedLen = 0;
    bool compressedOk = compressed && compressor;
    if (compressedOk) {
        compressedLen = ZSTD_CCtx_setParameter(compressor, ZSTD_c_compressionLevel, 1);
        compressedOk = !ZSTD_isError(compressedLen);
    }
    if (compressedOk) {
        compressedLen = ZSTD_CCtx_setParameter(compressor, ZSTD_c_minMatch, 4);
        compressedOk = !ZSTD_isError(compressedLen);
    }
    if (compressedOk) {
        compressedLen = ZSTD_compress2(compressor, compressed, compressedCap, w.p + 24, rawLen);
        compressedOk = !ZSTD_isError(compressedLen);
    }
    ZSTD_freeCCtx(compressor);
    if (!compressedOk) {
        if (reason) *reason = "Runner snapshot compression failed";
        free(compressed); free(w.p); return false;
    }
    compressedCap = compressedLen;
#ifdef PLATFORM_BUSY_TICK
    phase[4] = nowNanos();
#endif
    if (compressedCap > SNAP_OUTPUT_LIMIT - 32) {
        if (reason) *reason = "Compressed runner snapshot exceeds the size limit";
        free(compressed); free(w.p); return false;
    }
    Writer out = {.ok = true};
    put32(&out, SNAP_MAGIC); put32(&out, SNAP_VERSION); put64(&out, gameId);
    put32(&out, (uint32_t)compressedCap + 8); put32(&out, 0);
    put32(&out, rawLen); put32(&out, rawCrc);
    put(&out, compressed, compressedCap);
    free(compressed); free(w.p);
    if (!out.ok || out.n > SNAP_OUTPUT_LIMIT) {
        if (reason) *reason = out.failure ? out.failure : "Runner snapshot output failed";
        free(out.p); return false;
    }
    uint32_t crc = checksum(out.p + 24, out.n - 24);
#ifdef PLATFORM_BUSY_TICK
    phase[5] = nowNanos();
    logInfo("State core phases: graph %llu encode %llu crc %llu zstd %llu pack %llu ms; raw %u compressed %lu\n",
        (unsigned long long)((phase[1] - phase[0]) / 1000000),
        (unsigned long long)((phase[2] - phase[1]) / 1000000),
        (unsigned long long)((phase[3] - phase[2]) / 1000000),
        (unsigned long long)((phase[4] - phase[3]) / 1000000),
        (unsigned long long)((phase[5] - phase[4]) / 1000000), rawLen, (unsigned long)compressedCap);
#endif
    out.p[20] = (uint8_t)crc; out.p[21] = (uint8_t)(crc >> 8);
    out.p[22] = (uint8_t)(crc >> 16); out.p[23] = (uint8_t)(crc >> 24);
    *data = out.p; *size = out.n; return true;
}

static char *getString(Reader *rd) {
    uint32_t len = get32(rd);
    if (rd->stringRefs && (len & SNAP_STRING_REF)) {
        uint32_t index = len & ~SNAP_STRING_REF;
        if (!rd->ok || !rd->strg || index >= rd->strg->count ||
            !rd->strg->strings || !rd->strg->strings[index]) {
            rd->ok = false; return NULL;
        }
        const char *source = rd->strg->strings[index];
        size_t n = strlen(source);
        if (n >= SNAP_LIMIT - rd->stringBytes) { rd->ok = false; return NULL; }
        rd->stringBytes += n + 1;
        char *copy = (char *)malloc(n + 1);
        if (!copy) { rd->ok = false; return NULL; }
        memcpy(copy, source, n + 1);
        return copy;
    }
    if (!rd->ok || len >= SNAP_LIMIT - rd->stringBytes ||
        len > rd->n - rd->pos) { rd->ok = false; return NULL; }
    rd->stringBytes += (size_t)len + 1;
    char *out = (char *)malloc((size_t)len + 1);
    if (!out) { rd->ok = false; return NULL; }
    get(rd, out, len); out[len] = 0;
    if (strlen(out) != len) { rd->ok = false; free(out); return NULL; }
    return out;
}
static IniFile *getIni(Reader *rd, uint32_t *entryBudget) {
    uint32_t sections = get32(rd);
    if (!rd->ok || !countOK(sections) || sections > (rd->n - rd->pos) / 8 ||
        sections > *entryBudget) { rd->ok = false; return NULL; }
    *entryBudget -= sections;
    IniFile *ini = (IniFile *)calloc(1, sizeof(*ini));
    if (!ini) { rd->ok = false; return NULL; }
    ini->sections = (IniSection *)calloc(sections ? sections : 1, sizeof(IniSection));
    if (!ini->sections) { free(ini); rd->ok = false; return NULL; }
    ini->count = ini->capacity = (int)sections;
    for (uint32_t i = 0; i < sections && rd->ok; i++) {
        IniSection *section = &ini->sections[i];
        section->name = getString(rd);
        uint32_t count = get32(rd);
        if (!rd->ok || !section->name || !countOK(count) ||
            count > *entryBudget || count > (rd->n - rd->pos) / 8) {
            rd->ok = false; break;
        }
        *entryBudget -= count;
        section->keys = (char **)calloc(count ? count : 1, sizeof(char *));
        section->values = (char **)calloc(count ? count : 1, sizeof(char *));
        if (!section->keys || !section->values) {
            rd->ok = false; break;
        }
        section->count = section->capacity = (int)count;
        for (uint32_t j = 0; j < count && rd->ok; j++) {
            section->keys[j] = getString(rd);
            section->values[j] = getString(rd);
            if (!section->keys[j] || !section->values[j]) rd->ok = false;
        }
    }
    if (!rd->ok) { Ini_free(ini); return NULL; }
    return ini;
}
static RValue *slotPointer(RunnerSnapshot *s, SlotRef ref) {
    switch (ref.kind) {
        case SLOT_GLOBAL: return IntRValueHashMap_findSlot(&s->global->selfVars, (int32_t)ref.index);
        case SLOT_INSTANCE:
            if (ref.container >= (uint32_t)arrlen(s->instances)) return NULL;
            return IntRValueHashMap_findSlot(&s->instances[ref.container]->selfVars, (int32_t)ref.index);
        case SLOT_SAVED:
            if (ref.container >= (uint32_t)arrlen(s->savedInstances)) return NULL;
            return IntRValueHashMap_findSlot(&s->savedInstances[ref.container]->selfVars, (int32_t)ref.index);
        case SLOT_MAP: {
            if (ref.container >= (uint32_t)arrlen(s->maps)) return NULL;
            DsMapEntry *map = s->maps[ref.container];
            ptrdiff_t at = shgeti(map, ref.key);
            return at >= 0 ? &map[at].value : NULL;
        }
        case SLOT_LIST:
            if (ref.container >= (uint32_t)arrlen(s->lists) ||
                ref.index >= (uint32_t)arrlen(s->lists[ref.container].items)) return NULL;
            return &s->lists[ref.container].items[ref.index];
        case SLOT_ARRAY:
            if (ref.container >= (uint32_t)arrlen(s->arrays)) return NULL;
            return GMLArray_slot(s->arrays[ref.container], (int32_t)ref.index);
        case SLOT_STRUCT:
            if (ref.container >= (uint32_t)arrlen(s->structs)) return NULL;
            return IntRValueHashMap_findSlot(&s->structs[ref.container]->selfVars, (int32_t)ref.index);
    }
    return NULL;
}
static void getValue(Reader *rd, RunnerSnapshot *s, RValue *out, SlotRef ref) {
    if ((size_t)arrlen(s->slots) >= SNAP_MAX_ITEMS) { rd->ok = false; return; }
    uint32_t tag = get32(rd), type = tag & 255u;
    if (!rd->ok) return;
    RValue value = RValue_makeUndefined();
    value.type = (uint8_t)type;
    value.gmlStackType = (uint8_t)(tag >> 8);
    value.assetRefType = (uint8_t)(tag >> 16);
    switch (type) {
        case RVALUE_UNDEFINED: break;
        case RVALUE_INT32: case RVALUE_BOOL: case RVALUE_ASSETREF:
            value.int32 = (int32_t)get32(rd); break;
        case RVALUE_REAL: { double d = 0; get(rd, &d, sizeof(d)); value.real = (GMLReal)d; break; }
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64: value.int64 = (int64_t)get64(rd); break;
#endif
        case RVALUE_STRING:
            value.string = getString(rd); value.ownsReference = true; break;
        case RVALUE_ARRAY: {
            uint32_t id = get32(rd);
            if (id >= (uint32_t)arrlen(s->arrays)) { rd->ok = false; break; }
            value.array = s->arrays[id]; value.ownsReference = true;
            GMLArray_incRef(value.array);
            break;
        }
#if IS_WAD17_OR_HIGHER_ENABLED
        case RVALUE_METHOD: {
            uint32_t id = get32(rd);
            if (id >= (uint32_t)arrlen(s->methods)) { rd->ok = false; break; }
            value.method = s->methods[id]; value.ownsReference = true;
            GMLMethod_incRef(value.method);
            break;
        }
#endif
        case RVALUE_STRUCT: {
            uint32_t id = get32(rd);
            if (id >= (uint32_t)arrlen(s->structs)) { rd->ok = false; break; }
            value.structInst = s->structs[id]; value.ownsReference = true;
            Instance_structIncRef(value.structInst);
            break;
        }
        default: rd->ok = false; break;
    }
    if (!rd->ok) { if (value.type == RVALUE_STRING) free((void *)value.string); return; }
    *out = value;
    arrput(s->slots, ref);
}
static void getMap(Reader *rd, RunnerSnapshot *s, IntRValueHashMap *map,
                   SlotKind kind, uint32_t container) {
    uint32_t count = get32(rd);
    if (!rd->ok || !countOK(count)) { rd->ok = false; return; }
    for (uint32_t i = 0; i < count && rd->ok; i++) {
        int32_t key = (int32_t)get32(rd);
        if (key == INT_RVALUE_HASHMAP_EMPTY_KEY || IntRValueHashMap_contains(map, key)) {
            rd->ok = false; break;
        }
        RValue *slot = IntRValueHashMap_getOrInsertUndefined(map, key);
        SlotRef ref = {kind, container, (uint32_t)key, NULL};
        getValue(rd, s, slot, ref);
    }
}
/* Element i of a, for either layout; counts elements when out is NULL. */
static int32_t arrayElements(GMLArray *a, RValue ***out) {
    int32_t n = 0;
    if (a->type == GML_MODERN_ARRAY) {
        for (int32_t i = 0; i < a->modern.length; i++) if (out) (*out)[n++] = &a->modern.data[i]; else n++;
        return n;
    }
    for (int32_t row = 0; row < a->legacy.rowCount; row++)
        for (int32_t col = 0; col < a->legacy.rows[row].length; col++)
            if (out) (*out)[n++] = &a->legacy.rows[row].data[col]; else n++;
    return n;
}
static void breakArrayCycles(RunnerSnapshot *s) {
    for (ptrdiff_t i = 0; i < arrlen(s->arrays); i++) {
        GMLArray *a = s->arrays[i];
        int32_t n = arrayElements(a, NULL);
        RValue **cells = n ? (RValue **)malloc((size_t)n * sizeof(RValue *)) : NULL;
        if (n && !cells) continue;
        arrayElements(a, &cells);
        for (int32_t k = 0; k < n; k++)
            if (cells[k]->type == RVALUE_ARRAY) { RValue_free(cells[k]); *cells[k] = RValue_makeUndefined(); }
        free(cells);
    }
}
static bool visitArray(RunnerSnapshot *s, uint32_t id, uint8_t *colors, unsigned depth) {
    if (depth > 128 || colors[id] == 1) return false;
    if (colors[id] == 2) return true;
    colors[id] = 1;
    GMLArray *a = s->arrays[id];
    int32_t n = arrayElements(a, NULL);
    RValue **cells = n ? (RValue **)malloc((size_t)n * sizeof(RValue *)) : NULL;
    if (n && !cells) return false;
    arrayElements(a, &cells);
    for (int32_t k = 0; k < n; k++) {
        RValue *v = cells[k];
        if (v->type != RVALUE_ARRAY) continue;
        uint32_t child = UINT32_MAX;
        for (ptrdiff_t i = 0; i < arrlen(s->arrays); i++)
            if (s->arrays[i] == v->array) { child = (uint32_t)i; break; }
        if (child == UINT32_MAX || !visitArray(s, child, colors, depth + 1)) { free(cells); return false; }
    }
    free(cells);
    colors[id] = 2;
    return true;
}

static bool buildIndexes(RunnerSnapshot *s, Runner *r) {
    s->objectCount = r->dataWin->objt.count;
    s->byObject = (Instance ***)calloc(s->objectCount ? s->objectCount : 1, sizeof(Instance **));
    s->byExact = (Instance ***)calloc(s->objectCount ? s->objectCount : 1, sizeof(Instance **));
    if (!s->byObject || !s->byExact) return false;
    SnapshotRoom *targetRoom = &s->roomStates[s->plain.roomIndex];
    s->grid = SpatialGrid_tryCreate(targetRoom->width, targetRoom->height);
    if (!s->grid) return false;
    for (ptrdiff_t i = 0; i < arrlen(s->instances); i++) {
        Instance *inst = s->instances[i];
        int id = (int)inst->instanceId;
        if (hmgeti(s->byId, id) >= 0) return false;
        hmput(s->byId, id, inst);
        int32_t object = inst->objectIndex;
        unsigned depth = 0;
        while (object >= 0 && (uint32_t)object < s->objectCount && depth++ < 32) {
            arrput(s->byObject[object], inst);
            object = r->dataWin->objt.objects[object].parentId;
        }
        arrput(s->byExact[inst->objectIndex], inst);
        SpatialGrid_markInstanceAsDirty(s->grid, inst);
    }
    for (ptrdiff_t i = 0; i < arrlen(s->savedInstances); i++) {
        Instance *inst = s->savedInstances[i];
        int id = (int)inst->instanceId;
        if (hmgeti(s->byId, id) >= 0) return false;
        hmput(s->byId, id, inst);
    }
    for (ptrdiff_t i = 0; i < arrlen(s->structs); i++) {
        int id = (int)s->structs[i]->instanceId;
        if (hmgeti(s->byId, id) >= 0) return false;
        hmput(s->byId, id, s->structs[i]);
    }
    return true;
}

static bool plainLayoutMatches(const Reader *rd, size_t plainSize, uint32_t rooms) {
    Reader probe = *rd;
    if (plainSize > probe.n || sizeof(GMLReal) + 4 > probe.n - plainSize) return false;
    probe.pos = plainSize;
    GMLReal epsilon = 0; get(&probe, &epsilon, sizeof(epsilon));
    if (!probe.ok || !(epsilon >= 0 && epsilon < 1) || get32(&probe) != rooms ||
        !countOK(rooms)) return false;
    /* A candidate's SW blend mode/write mask can resemble the other ABI's
     * epsilon/room count. Validate the complete room block before selecting;
     * this probe never allocates or changes the live room definitions. */
    for (uint32_t i = 0; i < rooms; i++) {
        uint8_t initialized = 0; get(&probe, &initialized, 1);
        if (!probe.ok || initialized > 1) return false;
    }
    for (uint32_t i = 0; i < rooms; i++) {
        uint32_t width = get32(&probe), height = get32(&probe);
        get32(&probe); /* speed */
        uint8_t persistent = 0, loaded = 0;
        get(&probe, &persistent, 1); get(&probe, &loaded, 1);
        uint32_t tiles = get32(&probe);
        if (!probe.ok || persistent > 1 || loaded > 1 ||
            !SpatialGrid_dimensionsValid(width, height) || !countOK(tiles) ||
            (!loaded && tiles) || (size_t)tiles > (probe.n - probe.pos) / sizeof(RoomTile))
            return false;
        probe.pos += (size_t)tiles * sizeof(RoomTile);
    }
    return probe.ok;
}

RunnerSnapshot *Runner_snapshotPrepare(Runner *r, uint64_t gameId,
                                       const uint8_t *data, size_t size,
                                       const char **reason) {
#ifdef PLATFORM_BUSY_TICK
    uint64_t prep[6] = {nowNanos(), 0, 0, 0, 0, 0};
#endif
    if (reason) *reason = NULL;
    if (!r || !r->dataWin || !r->vmContext || !data || size < 32 || size > SNAP_OUTPUT_LIMIT) {
        if (reason) *reason = "Invalid runner snapshot";
        return NULL;
    }
    if (!eligible(r, reason, false, NULL)) return NULL;
    Reader hdr = {.p = data, .n = size, .pos = 0, .ok = true};
    uint32_t magic = get32(&hdr), version = get32(&hdr);
    uint64_t id = get64(&hdr);
    uint32_t bodyLen = get32(&hdr), crc = get32(&hdr);
    uint32_t rawLen = get32(&hdr), rawCrc = get32(&hdr);
    if (!hdr.ok || magic != SNAP_MAGIC ||
        (version != SNAP_VERSION && version != SNAP_EPSILON_VERSION &&
         version != SNAP_TILEMAP_VERSION &&
         version != SNAP_GMS2_VERSION && version != SNAP_SAVED_ROOM_VERSION &&
         version != SNAP_STRING_REF_VERSION &&
         version != SNAP_ZSTD_LEGACY_VERSION && version != SNAP_LEGACY_VERSION) ||
        id != gameId ||
        bodyLen != size - 24 || crc != checksum(data + 24, bodyLen) ||
        rawLen < sizeof(RunnerPlain) || rawLen > SNAP_LIMIT - 24) {
        if (reason) *reason = "Runner snapshot header or checksum mismatch";
        return NULL;
    }
    uint8_t *raw = (uint8_t *)malloc(rawLen);
    bool decoded = false;
    if (raw && version == SNAP_LEGACY_VERSION) {
        mz_ulong decompressedLen = rawLen;
        decoded = mz_uncompress(raw, &decompressedLen, data + 32, size - 32) == MZ_OK &&
                  decompressedLen == rawLen;
    } else if (raw) {
        ZSTD_frameHeader frame = {0};
        size_t headerResult = ZSTD_getFrameHeader(&frame, data + 32, size - 32);
        size_t frameLen = ZSTD_findFrameCompressedSize(data + 32, size - 32);
        if (headerResult == 0 && frame.frameType == ZSTD_frame &&
            frame.frameContentSize == rawLen && frame.windowSize <= SNAP_LIMIT &&
            !ZSTD_isError(frameLen) && frameLen == size - 32) {
            ZSTD_DCtx *ctx = ZSTD_createDCtx();
            if (ctx) {
                size_t bounded = ZSTD_DCtx_setParameter(ctx, ZSTD_d_windowLogMax, 22);
                if (!ZSTD_isError(bounded)) {
                    size_t decodedLen = ZSTD_decompressDCtx(ctx, raw, rawLen,
                                                               data + 32, size - 32);
                    decoded = !ZSTD_isError(decodedLen) && decodedLen == rawLen;
                }
                ZSTD_freeDCtx(ctx);
            }
        }
    }
    if (!decoded || checksum(raw, rawLen) != rawCrc) {
        if (reason) *reason = "Runner snapshot decompression failed";
        free(raw); return NULL;
    }
#ifdef PLATFORM_BUSY_TICK
    prep[1] = nowNanos();
#endif
    Reader rd = {.p = raw, .n = rawLen, .pos = 0, .ok = true};
    rd.strg = &r->dataWin->strg;
    rd.stringRefs = version >= SNAP_STRING_REF_VERSION;
    if (rd.stringRefs && rd.strg->count > SNAP_STATIC_STRING_LIMIT) {
        if (reason) *reason = "Runner snapshot string dictionary unavailable";
        free(raw); return NULL;
    }
    RunnerSnapshot *s = (RunnerSnapshot *)calloc(1, sizeof(*s));
    if (!s) { if (reason) *reason = "Out of memory"; free(raw); return NULL; }
    Room *shadowRooms = NULL;
    const char *failure = NULL;
    bool expanded = false;
    if (version == SNAP_EPSILON_VERSION) {
        bool original = plainLayoutMatches(&rd, sizeof(RunnerPlain), r->dataWin->room.count);
        expanded = plainLayoutMatches(&rd, expandedPlainSize(), r->dataWin->room.count);
        /* Never guess if corrupt input makes both revision-9 layouts plausible.
         * The selected layout still passes the complete body validation below. */
        if (original == expanded) {
            failure = "Unrecognized or ambiguous revision-9 renderer layout"; goto bad;
        }
    }
    if (expanded) {
        size_t after = offsetof(RunnerPlain, rendererAfter);
        get(&rd, &s->plain, after);
        get(&rd, s->plain.rendererAfter, 348);
        /* Skip only the serialized address, not the new scalar state. */
        rd.pos = after + (sizeof(void *) == 8 ? 352 : 348) + sizeof(void *);
        get(&rd, &s->drawDepth, sizeof(float));
        uint8_t coordinates = 0; get(&rd, &coordinates, 1);
        if (!rd.ok || coordinates > 1) goto bad;
        s->legacyTextureCoordinates = coordinates != 0;
#ifdef ENABLE_SW_RENDERER
        rd.pos = after + SNAP_EXPANDED_RENDERER_BYTES;
        get(&rd, &s->plain.swState, sizeof(s->plain.swState));
#endif
        rd.pos = expandedPlainSize();
    } else get(&rd, &s->plain, sizeof(s->plain));
    s->mathEpsilon = GML_MATH_EPSILON;
    if (version >= SNAP_EPSILON_VERSION)
        get(&rd, &s->mathEpsilon, sizeof(s->mathEpsilon));
    if (version >= SNAP_RENDERER_VERSION) {
        get(&rd, &s->drawDepth, sizeof(float));
        uint8_t coordinates = 0; get(&rd, &coordinates, 1);
        if (!rd.ok || coordinates > 1) goto bad;
        s->legacyTextureCoordinates = coordinates != 0;
    }
    if (isnan(s->drawDepth) || isinf(s->drawDepth)) goto bad;
    if (!(s->mathEpsilon >= 0 && s->mathEpsilon < 1)) goto bad;
    if (!rd.ok || s->plain.roomIndex < 0 ||
        (uint32_t)s->plain.roomIndex >= r->dataWin->room.count ||
        s->plain.pendingRoom != -1) goto bad;
    s->room = &r->dataWin->room.rooms[s->plain.roomIndex];
    uint32_t roomCount = get32(&rd);
    if (roomCount != r->dataWin->room.count || !countOK(roomCount)) goto bad;
    s->roomCount = roomCount;
    s->roomInitialized = (bool *)calloc(roomCount ? roomCount : 1, sizeof(bool));
    if (!s->roomInitialized) goto bad;
    for (uint32_t i = 0; i < roomCount; i++) {
        uint8_t b = 0; get(&rd, &b, 1);
        if (b > 1) goto bad;
        s->roomInitialized[i] = b != 0;
    }
    s->roomStates = (SnapshotRoom *)calloc(roomCount ? roomCount : 1, sizeof(SnapshotRoom));
    if (!s->roomStates) goto bad;
    for (uint32_t i = 0; i < roomCount && rd.ok; i++) {
        SnapshotRoom *state = &s->roomStates[i];
        uint8_t persistent = 0, loaded = 0;
        state->width = get32(&rd); state->height = get32(&rd); state->speed = get32(&rd);
        get(&rd, &persistent, 1); get(&rd, &loaded, 1);
        state->tileCount = get32(&rd);
        if (!rd.ok || persistent > 1 || loaded > 1 ||
            !countOK(state->tileCount) || (!loaded && state->tileCount) ||
            (size_t)state->tileCount > (rd.n - rd.pos) / sizeof(RoomTile)) goto bad;
        if (!SpatialGrid_dimensionsValid(state->width, state->height)) goto bad;
        state->persistent = persistent != 0;
        state->payloadLoaded = loaded != 0;
        if (!state->payloadLoaded) continue;
        Room *room = &r->dataWin->room.rooms[i];
        if (!room->payloadLoaded && r->dataWin->lazyLoadRooms &&
            !r->dataWin->roomChunkBuffer && !r->dataWin->lazyLoadFile)
            goto bad;
        if (!room->payloadLoaded && r->dataWin->lazyLoadRooms) {
            if (!shadowRooms) {
                shadowRooms = (Room *)malloc((size_t)roomCount * sizeof(Room));
                if (!shadowRooms) goto bad;
                memcpy(shadowRooms, r->dataWin->room.rooms,
                       (size_t)roomCount * sizeof(Room));
            }
            DataWin shadow = *r->dataWin;
            shadow.room.rooms = shadowRooms;
#ifdef PLATFORM_BUSY_TICK
            uint64_t roomStarted = nowNanos();
#endif
            DataWin_loadRoomPayload(&shadow, (int32_t)i);
#ifdef PLATFORM_BUSY_TICK
            logInfo("State room payload: room %u, %llu ms\n", i,
                (unsigned long long)((nowNanos() - roomStarted) / 1000000));
#endif
            state->stagedPayload = shadowRooms[i];
            state->stagedLoaded = true;
        } else if (!room->payloadLoaded) goto bad;
        state->tiles = (RoomTile *)malloc((state->tileCount ? state->tileCount : 1) * sizeof(RoomTile));
        if (!state->tiles) goto bad;
        get(&rd, state->tiles, (size_t)state->tileCount * sizeof(RoomTile));
    }
    free(shadowRooms); shadowRooms = NULL;
#ifdef PLATFORM_BUSY_TICK
    prep[2] = nowNanos();
#endif
    if (!s->roomStates[s->plain.roomIndex].payloadLoaded) goto bad;
    uint32_t arrayCount = get32(&rd);
    if (!rd.ok || !countOK(arrayCount)) goto bad;
    uint32_t arrayElements = 0;
    bool gms2 = version >= SNAP_GMS2_VERSION;
    bool tokenOwners = gms2 && IS_WAD17_OR_HIGHER(r->vmContext);
    for (uint32_t i = 0; i < arrayCount && rd.ok; i++) {
        uint8_t modern = 0;
        if (gms2) get(&rd, &modern, 1);
        uint64_t token = tokenOwners ? get64(&rd) : 0;
        if (!rd.ok || modern > 1) goto bad;
        if (modern) {
            uint32_t len = get32(&rd);
            if (!rd.ok || len > SNAP_MAX_ITEMS - arrayElements || (size_t)len > (rd.n - rd.pos) / 4) {
                failure = "Array element budget exceeded"; goto bad;
            }
            arrayElements += len;
            GMLArray *a = (GMLArray *)calloc(1, sizeof(GMLArray));
            if (!a) goto bad;
            a->type = GML_MODERN_ARRAY; a->refCount = 1;
            a->owner = (void *)(uintptr_t)token;
            a->modern.data = (RValue *)calloc(len ? len : 1, sizeof(RValue));
            if (!a->modern.data) { free(a); goto bad; }
            a->modern.length = a->modern.capacity = (int32_t)len;
            arrput(s->arrays, a); arrput(s->owners, SNAP_OWNER_NONE);
            continue;
        }
        uint32_t owner = tokenOwners ? SNAP_OWNER_NONE : get32(&rd), rowCount = get32(&rd);
        if (!rd.ok || !countOK(rowCount) || rowCount > 1024) goto bad;
        GMLArray *a = (GMLArray *)calloc(1, sizeof(GMLArray));
        if (!a) goto bad;
        a->type = GML_LEGACY_ARRAY; a->refCount = 1;
        if (tokenOwners) a->owner = (void *)(uintptr_t)token;
        a->legacy.rowCount = a->legacy.rowCapacity = (int32_t)rowCount;
        a->legacy.rows = (GMLArrayRow *)calloc(rowCount ? rowCount : 1, sizeof(GMLArrayRow));
        if (!a->legacy.rows) { free(a); goto bad; }
        arrput(s->arrays, a); arrput(s->owners, owner);
        for (uint32_t row = 0; row < rowCount; row++) {
            uint32_t len = get32(&rd);
            if (!rd.ok || len > 32000) goto bad;
            if (len > SNAP_MAX_ITEMS - arrayElements ||
                (size_t)len > (rd.n - rd.pos) / 4) {
                failure = "Array element budget exceeded"; goto bad;
            }
            arrayElements += len;
            GMLArrayRow *rr = &a->legacy.rows[row];
            rr->data = (RValue *)calloc(len ? len : 1, sizeof(RValue));
            if (!rr->data) goto bad;
            rr->length = rr->capacity = (int32_t)len;
        }
    }
    if (gms2) {
        uint32_t methodCount = get32(&rd);
        if (!rd.ok || !countOK(methodCount) || (methodCount && !IS_WAD17_OR_HIGHER_ENABLED)) goto bad;
        const DataWin *dw = r->dataWin;
        for (uint32_t i = 0; i < methodCount && rd.ok; i++) {
            uint8_t kind = 0; get(&rd, &kind, 1);
            uint32_t index = get32(&rd);
            int32_t bound = (int32_t)get32(&rd);
            if (!rd.ok) goto bad;
            GMLMethod *m = NULL;
            if (kind == SNAP_METHOD_CODE && index < dw->code.count && dw->code.entries[index].present)
                m = GMLMethod_create((int32_t)index, bound);
            else if (kind == SNAP_METHOD_BUILTIN && index < dw->func.functionCount &&
                     r->vmContext->funcCallCache && r->vmContext->funcCallCache[index].builtin)
                m = GMLMethod_createBuiltin((BuiltinFunc)r->vmContext->funcCallCache[index].builtin, bound);
            else if (kind == SNAP_METHOD_UNRESOLVED && index < dw->func.functionCount)
                m = GMLMethod_createUnresolved(dw->func.functions[index].name, bound);
            if (!m) goto bad;
            arrput(s->methods, m);
        }
        uint32_t structCount = get32(&rd);
        if (!rd.ok || !countOK(structCount)) goto bad;
        for (uint32_t i = 0; i < structCount && rd.ok; i++) {
            uint32_t id = get32(&rd); uint8_t pinned = 0; get(&rd, &pinned, 1);
            int32_t constructor = (int32_t)get32(&rd);
            if (!rd.ok || pinned > 1 || constructor < -1 ||
                (constructor >= 0 && (uint32_t)constructor >= dw->code.count)) goto bad;
            Instance *st = Instance_create(id, STRUCT_OBJECT_INDEX, 0, 0);
            st->refCount = 1; /* the registry's */
            st->pinned = pinned != 0;
            st->structRegistryIndex = (int32_t)i;
            st->constructorCodeIndex = constructor;
            arrput(s->structs, st);
        }
    }
    uint32_t tileLayerCount = get32(&rd);
    if (!rd.ok || !countOK(tileLayerCount)) goto bad;
    for (uint32_t i = 0; i < tileLayerCount && rd.ok; i++) {
        TileLayerMapEntry item; get(&rd, &item, sizeof(item));
        if (hmgeti(s->tileLayers, item.key) >= 0) goto bad;
        hmput(s->tileLayers, item.key, item.value);
    }
    uint32_t instanceCount = get32(&rd);
    if (!rd.ok || !countOK(instanceCount)) goto bad;
    for (uint32_t i = 0; i < instanceCount && rd.ok; i++) {
        InstancePlain p; get(&rd, &p, sizeof(p));
        if (!rd.ok || p.object < 0 || (uint32_t)p.object >= r->dataWin->objt.count ||
            p.room < -1 || (p.room >= 0 && (uint32_t)p.room >= r->dataWin->room.count)) goto bad;
        Instance *inst = Instance_create(p.id, p.object, 0, 0);
        setInstancePlain(inst, &p);
        arrput(s->instances, inst);
        if (inst->destroyed && (inst->active || !inst->cleanupEventFired)) goto bad;
        getMap(&rd, s, &inst->selfVars, SLOT_INSTANCE, i);
    }
    uint32_t savedRooms = version >= SNAP_SAVED_ROOM_VERSION ? get32(&rd) : 0;
    if (!rd.ok || savedRooms > roomCount || (savedRooms && !r->savedRoomStates)) goto bad;
    int64_t lastSaved = -1;
    for (uint32_t n = 0; n < savedRooms && rd.ok; n++) {
        uint32_t index = get32(&rd);
        if (!rd.ok || index >= roomCount || (int64_t)index <= lastSaved) goto bad;
        lastSaved = index;
        SnapshotRoom *state = &s->roomStates[index];
        state->hasSaved = true;
        get(&rd, &state->saved, sizeof(state->saved));
        uint32_t layers = get32(&rd);
        if (!rd.ok || !countOK(layers)) goto bad;
        for (uint32_t i = 0; i < layers && rd.ok; i++) {
            TileLayerMapEntry item; get(&rd, &item, sizeof(item));
            if (!rd.ok || hmgeti(state->savedTileLayers, item.key) >= 0) goto bad;
            hmput(state->savedTileLayers, item.key, item.value);
        }
        uint32_t count = get32(&rd);
        if (!rd.ok || !countOK(count) ||
            count > SNAP_MAX_ITEMS - (uint32_t)arrlen(s->savedInstances)) goto bad;
        for (uint32_t i = 0; i < count && rd.ok; i++) {
            InstancePlain p; get(&rd, &p, sizeof(p));
            if (!rd.ok || p.object < 0 || (uint32_t)p.object >= r->dataWin->objt.count ||
                p.room < -1 || (p.room >= 0 && (uint32_t)p.room >= r->dataWin->room.count)) goto bad;
            Instance *inst = Instance_create(p.id, p.object, 0, 0);
            setInstancePlain(inst, &p);
            arrput(state->savedInstances, inst);
            arrput(s->savedInstances, inst);
            getMap(&rd, s, &inst->selfVars, SLOT_SAVED, (uint32_t)arrlen(s->savedInstances) - 1);
        }
    }
    s->global = Instance_create(0, STRUCT_OBJECT_INDEX, 0, 0);
    getMap(&rd, s, &s->global->selfVars, SLOT_GLOBAL, 0);
    uint32_t varNameCount = get32(&rd);
    if (!rd.ok || !countOK(varNameCount)) goto bad;
    for (uint32_t i = 0; i < varNameCount && rd.ok; i++) {
        char *key = getString(&rd);
        if (!rd.ok || !key) goto bad;
        if (shgeti(s->varNames, key) >= 0) { free(key); goto bad; }
        int32_t value = (int32_t)get32(&rd);
        shput(s->varNames, key, value);
    }
    uint32_t mapPoolLen = get32(&rd);
    if (!rd.ok || !countOK(mapPoolLen)) goto bad;
    for (uint32_t i = 0; i < mapPoolLen && rd.ok; i++) {
        DsMapEntry *map = NULL; arrput(s->maps, map);
        uint32_t count = get32(&rd);
        if (!rd.ok || !countOK(count)) goto bad;
        for (uint32_t j = 0; j < count && rd.ok; j++) {
            char *key = getString(&rd);
            if (!rd.ok || !key) goto bad;
            if (shgeti(s->maps[i], key) >= 0) { free(key); goto bad; }
            RValue value = RValue_makeUndefined();
            shput(s->maps[i], key, value);
            ptrdiff_t at = shgeti(s->maps[i], key);
            SlotRef ref = {SLOT_MAP, i, 0, key};
            getValue(&rd, s, &s->maps[i][at].value, ref);
        }
    }
    uint32_t listPoolLen = get32(&rd);
    if (!rd.ok || !countOK(listPoolLen)) goto bad;
    for (uint32_t i = 0; i < listPoolLen && rd.ok; i++) {
        DsList list = {0}; arrput(s->lists, list);
        uint8_t freed = 0; get(&rd, &freed, 1);
        uint32_t count = get32(&rd);
        if (!rd.ok || freed > 1 || !countOK(count) || (freed && count)) goto bad;
        s->lists[i].freed = freed != 0;
        for (uint32_t j = 0; j < count && rd.ok; j++) {
            RValue value = RValue_makeUndefined(); arrput(s->lists[i].items, value);
            SlotRef ref = {SLOT_LIST, i, j, NULL};
            getValue(&rd, s, &s->lists[i].items[j], ref);
        }
    }
    for (ptrdiff_t i = 0; i < arrlen(s->structs) && rd.ok; i++)
        getMap(&rd, s, &s->structs[i]->selfVars, SLOT_STRUCT, (uint32_t)i);
    for (uint32_t a = 0; a < arrayCount && rd.ok; a++) {
        GMLArray *array = s->arrays[a];
        if (array->type == GML_MODERN_ARRAY) {
            for (int32_t i = 0; i < array->modern.length && rd.ok; i++) {
                SlotRef ref = {SLOT_ARRAY, a, (uint32_t)i, NULL};
                getValue(&rd, s, &array->modern.data[i], ref);
            }
            continue;
        }
        for (int32_t row = 0; row < array->legacy.rowCount; row++)
            for (int32_t col = 0; col < array->legacy.rows[row].length && rd.ok; col++) {
                SlotRef ref = {SLOT_ARRAY, a, (uint32_t)row * GML_LEGACY_ARRAY_STRIDE + (uint32_t)col, NULL};
                getValue(&rd, s, &array->legacy.rows[row].data[col], ref);
            }
    }
    if (gms2) {
        uint32_t staticCount = get32(&rd);
        if (!rd.ok || (staticCount && staticCount != r->dataWin->code.count) ||
            (staticCount && !r->vmContext->staticInitialized)) goto bad;
        if (staticCount) {
            s->staticInitialized = (bool *)calloc(staticCount, sizeof(bool));
            if (!s->staticInitialized) goto bad;
            for (uint32_t i = 0; i < staticCount && rd.ok; i += 8) {
                uint8_t bits = 0; get(&rd, &bits, 1);
                for (uint32_t b = 0; b < 8 && i + b < staticCount; b++)
                    s->staticInitialized[i + b] = (bits >> b) & 1u;
            }
        }
        /* Borrowed elements point into the payload the room will have once
         * applied: the live one, or the one staged above. */
        SnapshotRoom *current = &s->roomStates[s->plain.roomIndex];
        const Room *payload = current->stagedLoaded ? &current->stagedPayload : s->room;
        uint32_t layerCount = get32(&rd);
        if (!rd.ok || !countOK(layerCount)) goto bad;
        s->hasLayers = true;
        uint32_t elementBudget = SNAP_MAX_ITEMS;
        for (uint32_t i = 0; i < layerCount && rd.ok; i++) {
            LayerPlain lp; get(&rd, &lp, sizeof(lp));
            if (!rd.ok || lp.automaticDepth > 1 || lp.visible > 1 || lp.dynamic > 1 || lp.hasName > 1) goto bad;
            RuntimeLayer L; memset(&L, 0, sizeof(L));
            L.id = lp.id; L.drawOrder = lp.drawOrder; L.depth = lp.depth;
            L.xOffset = lp.xOffset; L.yOffset = lp.yOffset; L.hSpeed = lp.hSpeed; L.vSpeed = lp.vSpeed;
            L.beginScript = lp.beginScript; L.endScript = lp.endScript; L.shaderIndex = lp.shaderIndex;
            L.automaticDepth = lp.automaticDepth; L.visible = lp.visible; L.dynamic = lp.dynamic;
            arrput(s->layers, L);
            RuntimeLayer *dst = &arrlast(s->layers);
            if (lp.hasName && !(dst->dynamicName = getString(&rd))) goto bad;
            uint32_t elements = get32(&rd);
            if (!rd.ok || elements > elementBudget) goto bad;
            elementBudget -= elements;
            for (uint32_t j = 0; j < elements && rd.ok; j++) {
                ElementPlain ep; get(&rd, &ep, sizeof(ep));
                int32_t layer = (int32_t)get32(&rd), item = (int32_t)get32(&rd);
                if (!rd.ok || ep.visible > 1 || ep.owned > 1) goto bad;
                RuntimeLayerElement el; memset(&el, 0, sizeof(el));
                el.id = ep.id; el.type = (RuntimeLayerElementType)ep.type;
                el.visible = ep.visible; el.alpha = ep.alpha; el.blend = ep.blend;
                const RoomLayer *rl = layer >= 0 && payload->layers && (uint32_t)layer < payload->layerCount
                    ? &payload->layers[layer] : NULL;
                const RoomLayerAssetsData *a = rl && rl->type == RoomLayerType_Assets ? rl->assetsData : NULL;
                switch (ep.type) {
                    case RuntimeLayerElementType_Background:
                        el.backgroundElement = (RuntimeBackgroundElement *)malloc(sizeof(RuntimeBackgroundElement));
                        if (!el.backgroundElement) goto bad;
                        get(&rd, el.backgroundElement, sizeof(RuntimeBackgroundElement));
                        break;
                    case RuntimeLayerElementType_Instance: el.instanceId = (int32_t)get32(&rd); break;
                    case RuntimeLayerElementType_Sprite:
                        el.spriteElement = (RuntimeSpriteElement *)malloc(sizeof(RuntimeSpriteElement));
                        if (!el.spriteElement) goto bad;
                        get(&rd, el.spriteElement, sizeof(RuntimeSpriteElement));
                        el.spriteElement->name = NULL;
                        if (layer >= 0) {
                            if (!a || item < 0 || (uint32_t)item >= a->spriteCount) { free(el.spriteElement); goto bad; }
                            el.spriteElement->name = a->sprites[item].name;
                        }
                        break;
                    case RuntimeLayerElementType_Tilemap:
                        if (!rl || rl->type != RoomLayerType_Tiles || !rl->tilesData) goto bad;
                        el.tilemapData = rl->tilesData;
                        break;
                    case RuntimeLayerElementType_Tile:
                        if (ep.owned) {
                            el.tileElement = (RoomTile *)malloc(sizeof(RoomTile));
                            if (!el.tileElement) goto bad;
                            el.tileElementOwned = true;
                            get(&rd, el.tileElement, sizeof(RoomTile));
                        } else {
                            if (!a || item < 0 || (uint32_t)item >= a->legacyTileCount) goto bad;
                            el.tileElement = &a->legacyTiles[item];
                        }
                        break;
                    default: goto bad;
                }
                arrput(dst->elements, el);
            }
        }
        uint32_t legacyLayers = get32(&rd);
        if (!rd.ok || !countOK(legacyLayers)) goto bad;
        for (uint32_t i = 0; i < legacyLayers && rd.ok; i++) {
            uint32_t room = get32(&rd), layer = get32(&rd), count = get32(&rd);
            if (!rd.ok || room >= roomCount || !s->roomStates[room].payloadLoaded) goto bad;
            SnapshotRoom *state = &s->roomStates[room];
            const Room *target = state->stagedLoaded ? &state->stagedPayload : &r->dataWin->room.rooms[room];
            if (!target->layers || layer >= target->layerCount ||
                target->layers[layer].type != RoomLayerType_Assets || !target->layers[layer].assetsData ||
                target->layers[layer].assetsData->legacyTileCount != count ||
                (size_t)count > (rd.n - rd.pos) / sizeof(RoomTile)) goto bad;
            SnapshotLegacyTiles t = {room, layer, count, (RoomTile *)malloc((size_t)count * sizeof(RoomTile))};
            if (!t.tiles) goto bad;
            get(&rd, t.tiles, (size_t)count * sizeof(RoomTile));
            arrput(s->legacyTiles, t);
        }
        if (version >= SNAP_TILEMAP_VERSION) {
            uint32_t tilemapCount = get32(&rd);
            if (!rd.ok || !countOK(tilemapCount)) goto bad;
            uint32_t previousRoom = 0, previousLayer = 0;
            for (uint32_t i = 0; i < tilemapCount && rd.ok; i++) {
                uint32_t room = get32(&rd), layer = get32(&rd);
                uint32_t width = get32(&rd), height = get32(&rd);
                if (!rd.ok || room >= roomCount || !s->roomStates[room].payloadLoaded ||
                    (i && (room < previousRoom || (room == previousRoom && layer <= previousLayer)))) goto bad;
                previousRoom = room; previousLayer = layer;
                SnapshotRoom *state = &s->roomStates[room];
                const Room *target = state->stagedLoaded ? &state->stagedPayload : &r->dataWin->room.rooms[room];
                if (!target->layers || layer >= target->layerCount || target->layers[layer].type != RoomLayerType_Tiles)
                    goto bad;
                const RoomLayerTilesData *map = target->layers[layer].tilesData;
                if (!tilemapCellsValid(map) || map->tilesX != width || map->tilesY != height) goto bad;
                size_t count = (size_t)width * height;
                if (count > (rd.n - rd.pos) / sizeof(uint32_t)) goto bad;
                SnapshotTilemap t = {room, layer, (uint32_t)count, NULL};
                t.cells = (uint32_t *)malloc((count ? count : 1) * sizeof(uint32_t));
                if (!t.cells) goto bad;
                get(&rd, t.cells, count * sizeof(uint32_t));
                arrput(s->tilemaps, t);
            }
            /* Require every loaded tilemap exactly once, in capture order. */
            uint32_t expected = 0;
            for (uint32_t i = 0; i < roomCount; i++) {
                SnapshotRoom *state = &s->roomStates[i];
                const Room *target = state->stagedLoaded ? &state->stagedPayload : &r->dataWin->room.rooms[i];
                for (uint32_t l = 0; state->payloadLoaded && target->layers && l < target->layerCount; l++)
                    if (target->layers[l].type == RoomLayerType_Tiles) expected++;
            }
            if (tilemapCount != expected) goto bad;
        }
        uint32_t formatSlots = get32(&rd);
        if (!rd.ok || formatSlots != (uint32_t)arrlen(r->vertexFormats)) {
            failure = "The game's vertex resources differ from the state's"; goto bad;
        }
        for (uint32_t i = 0; i < formatSlots && rd.ok; i++) {
            uint32_t sum = get32(&rd);
            uint32_t live = r->vertexFormats[i] ? vertexFormatSum(r->vertexFormats[i]) : 0;
            if (rd.ok && sum != live) { failure = "The game's vertex resources differ from the state's"; goto bad; }
        }
        uint32_t bufferSlots = get32(&rd);
        uint32_t liveSlots = r->vertexBuffers ? (uint32_t)r->vertexBufferCount : 0;
        if (!rd.ok || bufferSlots != liveSlots) {
            failure = "The game's vertex resources differ from the state's"; goto bad;
        }
        for (uint32_t i = 0; i < bufferSlots && rd.ok; i++) {
            uint32_t size = get32(&rd);
            const Buffer_Vertex *b = r->vertexBuffers[i];
            if (rd.ok && (size != (b ? b->bufferSize + 1u : 0) || (b && !vertexBufferUnused(b)))) {
                failure = "The game's vertex resources differ from the state's"; goto bad;
            }
        }
    }
    /* Revisions 3-6 have no vertex-resource contract. Applying their dormant
     * arrays must never displace (or arrfree) a live plain buffer pool. */
    if (!gms2) {
        bool liveVertex = r->vertexBufferCount > 0;
        for (ptrdiff_t i = 0; !liveVertex && i < arrlen(r->vertexFormats); i++)
            liveVertex = r->vertexFormats[i] != NULL;
        if (liveVertex) {
            failure = "The game's vertex resources differ from the state's"; goto bad;
        }
    }
    uint32_t dormant[SNAP_DORMANT_POOL_COUNT] = {0};
    if (rd.ok && rd.pos != rd.n) {
        if (rd.n - rd.pos < sizeof(dormant)) goto bad;
        uint32_t total = 0;
        for (uint32_t i = 0; i < SNAP_DORMANT_POOL_COUNT; i++) {
            dormant[i] = get32(&rd);
            if (!rd.ok || dormant[i] > SNAP_DORMANT_SLOT_LIMIT - total) goto bad;
            total += dormant[i];
        }
    }
    /* All recorded slots are tombstones. Their lengths alone reproduce the
     * resource ID allocator's next reuse choice. */
#define MAKE_DORMANT(field, type, index) do { \
    if (dormant[index]) { \
        arrsetlen(s->field, dormant[index]); \
        memset(s->field, 0, (size_t)dormant[index] * sizeof(type)); \
    } \
} while (0)
    MAKE_DORMANT(queues, DsQueue, 0);
    MAKE_DORMANT(stacks, DsStack, 1);
    MAKE_DORMANT(priorities, DsPriority, 2);
    MAKE_DORMANT(grids, DsGrid, 3);
    MAKE_DORMANT(buffers, GmlBuffer, 4);
    MAKE_DORMANT(mpGrids, MpGrid, 5);
    MAKE_DORMANT(particleSystems, ParticleSystem, 6);
    MAKE_DORMANT(particleTypes, ParticleType, 7);
    MAKE_DORMANT(emitters, AudioEmitter, 8);
    MAKE_DORMANT(vertexFormats, VmVertexFormat *, 9);
    MAKE_DORMANT(vertexBuffers, Buffer_Vertex *, 10);
#undef MAKE_DORMANT
    for (ptrdiff_t i = 0; i < arrlen(s->queues); i++) s->queues[i].freed = true;
    for (ptrdiff_t i = 0; i < arrlen(s->stacks); i++) s->stacks[i].freed = true;
    for (ptrdiff_t i = 0; i < arrlen(s->priorities); i++) s->priorities[i].freed = true;
    for (ptrdiff_t i = 0; i < arrlen(s->grids); i++) s->grids[i].freed = true;
    if (rd.ok && rd.pos != rd.n) {
        uint32_t magic = get32(&rd), flags = get32(&rd);
        if (!rd.ok || magic != SNAP_INI_MAGIC || (flags & ~31u)) goto bad;
        uint32_t entries = SNAP_MAX_ITEMS;
        if (flags & 4u) s->currentIniPath = getString(&rd);
        if (flags & 1u) s->currentIni = getIni(&rd, &entries);
        if (flags & 8u) s->cachedIniPath = getString(&rd);
        if (flags & 2u) s->cachedIni = getIni(&rd, &entries);
        if (!rd.ok || ((flags & 4u) && !s->currentIniPath) ||
            ((flags & 1u) && !s->currentIni) ||
            ((flags & 8u) && !s->cachedIniPath) ||
            ((flags & 2u) && !s->cachedIni)) goto bad;
        s->currentIniDirty = (flags & 16u) != 0;
    }
    if (!rd.ok || rd.pos != rd.n) goto bad;
    for (uint32_t a = 0; a < arrayCount; a++) {
        uint32_t owner = s->owners[a];
        if (owner == SNAP_OWNER_NONE) continue;
        if (owner == SNAP_OWNER_ORPHAN) {
            s->arrays[a]->owner = s->arrays[a]; /* stable, non-slot identity */
            continue;
        }
        if (owner >= (uint32_t)arrlen(s->slots)) goto bad;
        RValue *slot = slotPointer(s, s->slots[owner]);
        /* Earlier captures could name a live slot whose address had been
         * recycled after the array lost its owner. It is still an orphan. */
        s->arrays[a]->owner = slot && slot->type == RVALUE_ARRAY &&
            slot->array == s->arrays[a] ? (void *)slot : (void *)s->arrays[a];
    }
#ifdef PLATFORM_BUSY_TICK
    prep[3] = nowNanos();
#endif
    uint8_t *colors = (uint8_t *)calloc(arrayCount ? arrayCount : 1, 1);
    if (!colors) goto bad;
    bool acyclic = true;
    for (uint32_t a = 0; a < arrayCount; a++)
        if (!visitArray(s, a, colors, 0)) { acyclic = false; break; }
    free(colors);
    if (!acyclic || !buildIndexes(s, r)) goto bad;
#ifdef PLATFORM_BUSY_TICK
    prep[4] = nowNanos();
#endif
    free(raw);
#ifdef PLATFORM_BUSY_TICK
    prep[5] = nowNanos();
    logInfo("State prepare phases: decode %llu rooms %llu values %llu indexes %llu free %llu ms\n",
        (unsigned long long)((prep[1] - prep[0]) / 1000000),
        (unsigned long long)((prep[2] - prep[1]) / 1000000),
        (unsigned long long)((prep[3] - prep[2]) / 1000000),
        (unsigned long long)((prep[4] - prep[3]) / 1000000),
        (unsigned long long)((prep[5] - prep[4]) / 1000000));
#endif
    return s;
bad:
    free(shadowRooms);
    if (reason) *reason = failure ? failure : "Malformed or unsupported runner snapshot";
    breakArrayCycles(s);
    Runner_snapshotFree(s);
    free(raw);
    return NULL;
}

static void applyPlain(Runner *r, const RunnerPlain *p) {
    r->currentRoomIndex = p->roomIndex; r->currentRoomOrderPosition = p->roomOrder;
    r->frameCount = p->frame; r->pendingRoom = p->pendingRoom;
    r->nextInstanceId = p->nextInstanceId;
    r->collisionQueryCounter = p->collisionQueryCounter;
    r->gameStartFired = p->gameStartFired; r->viewsEnabled = p->viewsEnabled;
    r->drawBackgroundColor = p->drawBackgroundColor; r->backgroundColor = p->backgroundColor;
    r->appSurfaceEnabled = p->appSurfaceEnabled; r->appSurfaceAutoDraw = p->appSurfaceAutoDraw;
    r->usingAppSurface = p->usingAppSurface; r->drawEnabled = p->drawEnabled;
    r->dateTimeLocal = p->dateTimeLocal;
    r->collisionCompatibilityMode = p->collisionCompatibilityMode;
    r->applyOffsetForPrimitives = p->applyOffsetForPrimitives;
    r->applicationWidth = p->applicationWidth; r->applicationHeight = p->applicationHeight;
    r->oldApplicationWidth = p->oldApplicationWidth;
    r->oldApplicationHeight = p->oldApplicationHeight;
    r->guiWidth = p->guiWidth; r->guiHeight = p->guiHeight;
    r->cursorSprite = p->cursorSprite; r->cursorSpriteSubimage = p->cursorSpriteSubimage;
    r->currentCursor = p->currentCursor; r->nextLayerId = p->nextLayerId;
    r->nextLayerDrawOrder = p->nextLayerDrawOrder; r->nextCallLaterId = p->nextCallLaterId;
    r->asyncBufferNextRequestId = p->asyncBufferNextRequestId;
    r->xboxAsyncIdCounter = p->xboxAsyncIdCounter;
    r->lastMusicInstance = p->lastMusicInstance; r->viewCurrent = p->viewCurrent;
    r->vmContext->nextDynamicVarID = p->nextDynamicVarID;
    r->currentVertexFormatId = p->currentVertexFormatId;
    r->vertexBufferCount = p->vertexBufferCount;
    r->vertexFormatBit = p->vertexFormatBit;
    memcpy(r->surfaceStack, p->surfaceStack, sizeof(r->surfaceStack));
    r->score = p->score; r->lives = p->lives; r->health = p->health;
    r->deltaTime = p->deltaTime; r->gameSpeedOverride = p->gameSpeedOverride;
    r->mpPotMaxrot = p->mpPotMaxrot; r->mpPotStep = p->mpPotStep;
    r->mpPotAhead = p->mpPotAhead; r->mpPotOnSpot = p->mpPotOnSpot;
    r->random = p->random;
    r->dataWin->gen8.gms2FPS = p->gms2FPS;
    uint64_t now = nowNanos();
    r->gameStartTime = now >= p->elapsedGameNanos ? now - p->elapsedGameNanos : 0;
    r->fpsWindowStartNanos = now;
    r->fpsWindowStartFrame = r->frameCount;
    memcpy(r->views, p->views, sizeof(r->views));
    memcpy(r->defaultCameras, p->defaultCameras, sizeof(r->defaultCameras));
    memcpy(r->userCameras, p->userCameras, sizeof(r->userCameras));
    r->surfaceCamera = p->surfaceCamera; r->guiCamera = p->guiCamera;
    memcpy(r->backgrounds, p->backgrounds, sizeof(r->backgrounds));
    if (r->keyboard) *r->keyboard = p->keyboard;
    if (r->gamepads) *r->gamepads = p->gamepads;
    if (r->mouse) *r->mouse = p->mouse;
    memcpy((uint8_t *)r->renderer + offsetof(Renderer, drawColor),
           p->rendererBefore, sizeof(p->rendererBefore));
    memcpy((uint8_t *)r->renderer + offsetof(Renderer, gmlMatrices),
           p->rendererAfter, 348);
#ifdef ENABLE_SW_RENDERER
    SWRenderer_snapshotApplyState(r->renderer, &p->swState);
#endif
}

void Runner_snapshotApply(Runner *r, RunnerSnapshot *s) {
    if (!r || !s || s->applied) return;
    s->oldGlobal = r->vmContext->globalScopeInstance;
    s->oldInstances = r->instances; s->oldMaps = r->dsMapPool;
    s->oldStructs = r->structInstances;
    s->oldLists = r->dsListPool;
    s->oldQueues = r->dsQueuePool; s->oldStacks = r->dsStackPool;
    s->oldPriorities = r->dsPriorityPool; s->oldGrids = r->dsGridPool;
    s->oldBuffers = r->gmlBufferPool; s->oldMpGrids = r->mpGridPool;
    s->oldParticleSystems = r->particleSystemPool;
    s->oldParticleTypes = r->particleTypePool;
    s->oldEmitters = r->audioEmitters;
    s->oldVertexFormats = r->vertexFormats;
    s->oldVertexBuffers = r->vertexBuffers;
    s->oldVarNames = (SnapshotVarName *)r->vmContext->varNameMap;
    s->oldTileLayers = r->tileLayerMap;
    s->oldById = r->instancesById;
    s->oldByObject = r->instancesByObject; s->oldByExact = r->instancesByExactObject;
    s->oldGrid = r->spatialGrid;
    s->oldCurrentIni = r->currentIni;
    s->oldCurrentIniPath = r->currentIniPath;
    s->oldCachedIni = r->cachedIni; s->oldCachedIniPath = r->cachedIniPath;

    r->vmContext->globalScopeInstance = s->global;
    r->vmContext->varNameMap = (void *)s->varNames;
    r->instances = s->instances; r->dsMapPool = s->maps; r->dsListPool = s->lists;
    r->structInstances = s->structs;
    if (s->staticInitialized)
        memcpy(r->vmContext->staticInitialized, s->staticInitialized,
               r->dataWin->code.count * sizeof(bool));
    else if (r->vmContext->staticInitialized)
        memset(r->vmContext->staticInitialized, 0, r->dataWin->code.count * sizeof(bool));
    r->dsQueuePool = s->queues; r->dsStackPool = s->stacks;
    r->dsPriorityPool = s->priorities; r->dsGridPool = s->grids;
    r->gmlBufferPool = s->buffers; r->mpGridPool = s->mpGrids;
    r->particleSystemPool = s->particleSystems;
    r->particleTypePool = s->particleTypes; r->audioEmitters = s->emitters;
    if (s->hasLayers) {
        /* Revision 7 checked the live vertex resources match; keep them, and
         * let Free release the empty dormant arrays read for them. */
        s->oldVertexFormats = s->vertexFormats; s->oldVertexBuffers = s->vertexBuffers;
    } else {
        r->vertexFormats = s->vertexFormats; r->vertexBuffers = s->vertexBuffers;
    }
    r->instancesById = s->byId;
    r->instancesByObject = s->byObject; r->instancesByExactObject = s->byExact;
    r->spatialGrid = s->grid; r->tileLayerMap = s->tileLayers;
    r->currentRoom = s->room;
    if (r->savedRoomStates) for (uint32_t i = 0; i < s->roomCount; i++)
        Runner_freeSavedRoomStateResources(&r->savedRoomStates[i]);
    for (uint32_t i = 0; i < s->roomCount; i++) {
        Room *room = &r->dataWin->room.rooms[i];
        SnapshotRoom *state = &s->roomStates[i];
        if (state->payloadLoaded) {
            if (state->stagedLoaded) {
                Room *staged = &state->stagedPayload;
                room->backgrounds = staged->backgrounds;
                room->views = staged->views;
                room->gameObjects = staged->gameObjects;
                room->gameObjectCount = staged->gameObjectCount;
                room->layers = staged->layers;
                room->layerCount = staged->layerCount;
                room->payloadLoaded = true;
                room->tiles = staged->tiles;
                room->tileCount = staged->tileCount;
                state->stagedLoaded = false;
            }
            state->displacedTiles = room->tiles;
            room->tiles = state->tiles;
            room->tileCount = state->tileCount;
        } else if (room->payloadLoaded && r->dataWin->lazyLoadRooms && !room->eagerlyLoaded) {
            DataWin_freeRoomPayload(room);
        }
        room->width = state->width; room->height = state->height;
        room->speed = state->speed; room->persistent = state->persistent;
    }
    if (s->hasLayers) {
        s->oldLayers = r->runtimeLayers;
        Runner_invalidateLayerElementLookup(r);
        r->runtimeLayers = s->layers;
        for (ptrdiff_t i = 0; i < arrlen(s->legacyTiles); i++) {
            SnapshotLegacyTiles *t = &s->legacyTiles[i];
            Room *room = &r->dataWin->room.rooms[t->room];
            memcpy(room->layers[t->layer].assetsData->legacyTiles, t->tiles, (size_t)t->count * sizeof(RoomTile));
        }
        for (ptrdiff_t i = 0; i < arrlen(s->tilemaps); i++) {
            SnapshotTilemap *t = &s->tilemaps[i];
            Room *room = &r->dataWin->room.rooms[t->room];
            if (t->count) memcpy(room->layers[t->layer].tilesData->tileData, t->cells,
                                 (size_t)t->count * sizeof(uint32_t));
        }
    } else {
        s->oldLayers = NULL;
    }
    r->currentIni = s->currentIni;
    r->currentIniPath = s->currentIniPath;
    r->currentIniDirty = s->currentIniDirty;
    r->cachedIni = s->cachedIni;
    r->cachedIniPath = s->cachedIniPath;
    for (uint32_t i = 0; r->savedRoomStates && i < s->roomCount; i++) {
        SnapshotRoom *state = &s->roomStates[i];
        SavedRoomState *st = &r->savedRoomStates[i];
        if (state->hasSaved) {
            memcpy(st->backgrounds, state->saved.backgrounds, sizeof(st->backgrounds));
            memcpy(st->views, state->saved.views, sizeof(st->views));
            memcpy(st->defaultCameras, state->saved.defaultCameras, sizeof(st->defaultCameras));
            st->backgroundColor = state->saved.backgroundColor;
            st->drawBackgroundColor = state->saved.drawBackgroundColor;
            st->viewsEnabled = state->saved.viewsEnabled;
            st->tileLayerMap = state->savedTileLayers; state->savedTileLayers = NULL;
            st->instances = state->savedInstances; state->savedInstances = NULL;
        }
        st->initialized = s->roomInitialized[i];
    }
    applyPlain(r, &s->plain);
    r->mathEpsilon = s->mathEpsilon;
    r->renderer->drawDepth = s->drawDepth;
    r->renderer->legacyTextureCoordinates = s->legacyTextureCoordinates;
    /* Capture refuses paused runners, so every supported snapshot resumes.
     * The caller restores audio after this core commit. */
    r->paused = false;
    if (r->eventDispatchInstances) arrsetlen(r->eventDispatchInstances, 0);
    if (r->cachedDrawables) arrsetlen(r->cachedDrawables, 0);
    r->vmContext->currentCallFrame = NULL;
    r->vmContext->currentArrayOwner = NULL;
    r->drawableListStructureDirty = true;
    r->drawableListSortDirty = false;
    s->applied = true;
}
