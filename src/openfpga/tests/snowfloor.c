/* Real-asset oracle for the Undertale obj_snowfloor Draw native (the snow
 * patch in room 57, room_tundra_snowpuzz). Each scenario runs Create once and
 * then the Draw event a few times, in the interpreter and as the native, from
 * the same start, and compares every array cell (value, type and stack type),
 * xx and yy, global.flag[64], the random state and the opcodes run. The
 * native runs twice: with the script profiler on, which counts its opcodes
 * and makes it call the random/floor built-ins, and with it off, as in play,
 * where it works random numbers out itself (snowRoll). The
 * renderer is absent, so draw_circle draws nothing here; frame comparisons
 * cover the drawing. Pass data.win to the fixture. */
#include "of_game.h"
#include "of_native.h"
#include "profiler.h"
#include "random.h"
#include "spatial_grid.h"
#include "stb_ds.h"

#include <stdio.h>
#include <string.h>

enum { FAR, PUSHED, OTHER_ROOM, NO_PLAYER, SHARED_ARRAY, SCENARIO_COUNT };
enum { ARRAYS = 4, GRID = 5, CELLS = ARRAYS * GRID * GRID, DRAWS = 4 };
static const char *const drawEvent = "gml_Object_obj_snowfloor_Draw_0";
static const char *const createEvent = "gml_Object_obj_snowfloor_Create_0";
static const char *const arrayNames[ARRAYS] = { "snowx", "snowy", "moveme", "dodraw" };

typedef struct {
    GMLReal values[CELLS + 3];
    uint8_t types[CELLS + 3], stackTypes[CELLS + 3];
    Random before, random;
    uint64_t ops;
} Outcome;

/* The id the event's instructions use for a variable of this scope. */
static int32_t varId(DataWin *dw, const char *name, int32_t scope) {
    for (uint32_t v = 0; v < dw->vari.variableCount; v++)
        if (dw->vari.variables[v].instanceType == scope && dw->vari.variables[v].varID >= 0 &&
            !strcmp(dw->vari.variables[v].name, name)) return dw->vari.variables[v].varID;
    require(false);
    return -1;
}

static int find(DataWin *dw, const char *name, bool code) {
    uint32_t count = code ? dw->code.count : dw->objt.count;
    for (uint32_t n = 0; n < count; n++) {
        const char *entry = code ? dw->code.entries[n].name : dw->objt.objects[n].name;
        if (entry != NULL && !strcmp(entry, name)) return (int) n;
    }
    require(false);
    return -1;
}

static void add(Runner *r, Instance *inst) {
    arrput(r->instances, inst);
    hmput(r->instancesById, inst->instanceId, inst);
    Runner_addInstanceToObjectLists(r, inst);
}

static void record(Outcome *out, int at, RValue value) {
    out->values[at] = RValue_toReal(value);
    out->types[at] = value.type;
    out->stackTypes[at] = value.gmlStackType;
}

static Outcome run(DataWin *dw, int scenario, bool native, bool profiled) {
    int drawCode = find(dw, drawEvent, true), createCode = find(dw, createEvent, true);
    int snowObject = find(dw, "obj_snowfloor", false), playerObject = find(dw, "obj_mainchara", false);
    VMContext *vm = VM_create(dw);
    Runner r = {0};
    r.dataWin = dw;
    r.vmContext = vm;
    r.mathEpsilon = GML_MATH_EPSILON;
    vm->runner = &r;
    r.spatialGrid = SpatialGrid_create(640, 640);
    r.pendingRoom = -1;
    r.currentRoomIndex = scenario == OTHER_ROOM ? 56 : 57;
    r.instancesByObject = (Instance ***) safeCalloc(dw->objt.count, sizeof(Instance **));
    r.instancesByExactObject = (Instance ***) safeCalloc(dw->objt.count, sizeof(Instance **));
    Random_setSeed(&r.random, 7);
    EventSlotMap_build(&r.eventSlotMap, dw);
    ResolvedEventTable_build(&r.eventTable, dw, &r.eventSlotMap);

    if (vm->globalScopeInstance == NULL) vm->globalScopeInstance = Instance_create(0, STRUCT_OBJECT_INDEX, 0, 0); /* as VM_reset makes it */
    /* global.flag[64] = 0, owned by its slot as a bytecode write leaves it. */
    RValue *flagSlot = IntRValueHashMap_getOrInsertUndefined(&vm->globalScopeInstance->selfVars, varId(dw, "flag", INSTANCE_GLOBAL));
    GMLArray *flags = GMLArray_create(dw, 0);
    flags->owner = flagSlot;
    *flagSlot = RValue_makeArray(flags);
    GMLArray_set(flags, 64, RValue_makeReal(0));

    Instance *self = Instance_create(100001, snowObject, (GMLReal) 102.2f, 401);
    add(&r, self);
    Instance *player = NULL;
    if (scenario != NO_PLAYER) {
        /* Over the left half of the grid (dots at x 104.4 to 120.4, y 402 to
         * 418) for the pushed scenarios, well away otherwise. */
        bool near = scenario == PUSHED || scenario == OTHER_ROOM;
        player = Instance_create(100002, playerObject, near ? 104 : 300, near ? 392 : 100);
        player->spriteIndex = dw->objt.objects[playerObject].spriteId;
        add(&r, player);
        RValue moving = RValue_makeReal(near ? 1 : 0);
        Instance_setSelfVar(player, varId(dw, "moving", INSTANCE_SELF), moving);
    }
    RValue created = VM_executeCode(vm, createCode, self, NULL, snowObject, EVENT_CREATE, 0, NULL, 0);
    RValue_free(&created);
    if (scenario == SHARED_ARRAY) {
        /* Another variable owns snowx's array: a write must copy it first,
         * which the native leaves to the bytecode. */
        int32_t aliasId = varId(dw, "snowy", INSTANCE_SELF) + 100000;
        IntRValueHashMap_getOrInsertUndefined(&self->selfVars, aliasId);
        RValue *snowSlot = IntRValueHashMap_findSlot(&self->selfVars, varId(dw, "snowx", INSTANCE_SELF));
        RValue *aliasSlot = IntRValueHashMap_findSlot(&self->selfVars, aliasId);
        require(snowSlot != NULL && aliasSlot != NULL && snowSlot->type == RVALUE_ARRAY);
        *aliasSlot = RValue_makeIndependent(*snowSlot);
        snowSlot->array->owner = aliasSlot;
    }

    if (native) {
        const OfNativeCode *spec = NULL;
        for (size_t i = 0; i < ofGame.nativeCount; i++)
            if (!strcmp(ofGame.natives[i].name, drawEvent)) spec = &ofGame.natives[i];
        require(spec != NULL);
        require(!VM_setNativeCode(vm, drawEvent, spec->length, spec->hash ^ 1, spec->native));
        ofNativeInstall(&r);
        require(vm->nativeCode != NULL && vm->nativeCode[drawCode] != NULL);
    }
    if (profiled) Profiler_setEnabled(&vm->profiler, true);
    Random before = r.random;
    for (int n = 0; n < DRAWS; n++) {
        RValue result = VM_executeCode(vm, drawCode, self, NULL, snowObject, EVENT_DRAW, 0, NULL, 0);
        RValue_free(&result);
    }

    Outcome out;
    memset(&out, 0, sizeof(out));
    out.ops = profiled ? shget(vm->profiler->entries, (char *) drawEvent).ops : 0;
    out.before = before;
    out.random = r.random;
    for (int a = 0; a < ARRAYS; a++) {
        RValue *slot = IntRValueHashMap_findSlot(&self->selfVars, varId(dw, arrayNames[a], INSTANCE_SELF));
        require(slot != NULL && slot->type == RVALUE_ARRAY);
        for (int xx = 0; xx < GRID; xx++)
            for (int yy = 0; yy < GRID; yy++)
                record(&out, (a * GRID + xx) * GRID + yy, GMLArray_get(slot->array, xx * GML_LEGACY_ARRAY_STRIDE + yy));
    }
    record(&out, CELLS, Instance_getSelfVar(self, varId(dw, "xx", INSTANCE_SELF)));
    record(&out, CELLS + 1, Instance_getSelfVar(self, varId(dw, "yy", INSTANCE_SELF)));
    record(&out, CELLS + 2, GMLArray_get(flagSlot->array, 64));

    ResolvedEventTable_free(&r.eventTable);
    EventSlotMap_destroy(&r.eventSlotMap);
    SpatialGrid_free(r.spatialGrid);
    for (uint32_t n = 0; n < dw->objt.count; n++) {
        arrfree(r.instancesByObject[n]);
        arrfree(r.instancesByExactObject[n]);
    }
    free(r.instancesByObject);
    free(r.instancesByExactObject);
    arrfree(r.instances);
    hmfree(r.instancesById);
    Instance_free(self);
    if (player != NULL) Instance_free(player);
    VM_free(vm);
    return out;
}

int main(int argc, char **argv) {
    require(argc == 2);
    unsetenv("BS_NO_NATIVE");
    DataWinParserOptions opts = {0};
    opts.parseGen8 = opts.parseScpt = opts.parseCode = opts.parseVari = opts.parseFunc = opts.parseStrg =
        opts.parseObjt = opts.parseSprt = true;
    for (int i = 0; i < SCENARIO_COUNT; i++) {
        DataWin *dw = DataWin_parse(argv[1], opts);
        require(dw != NULL);
        Outcome a = run(dw, i, false, true);
        DataWin_free(dw);
        dw = DataWin_parse(argv[1], opts);
        require(dw != NULL);
        Outcome b = run(dw, i, true, true);
        DataWin_free(dw);
        dw = DataWin_parse(argv[1], opts);
        require(dw != NULL);
        Outcome c = run(dw, i, true, false);
        DataWin_free(dw);
        if (i == NO_PLAYER || i == SHARED_ARRAY) require(a.ops == b.ops && b.ops > 0);
        else require(a.ops > 0 && b.ops == 0);
        /* The scenarios do what they are named for. */
        bool rolled = memcmp(&a.random, &a.before, sizeof(a.random)) != 0;
        if (i == FAR) require(!rolled && a.values[CELLS + 2] == 0);
        if (i == PUSHED) require(rolled && a.values[CELLS + 2] == -1);
        if (i == OTHER_ROOM) require(rolled && a.values[CELLS + 2] == 0);
        require(!memcmp(a.values, b.values, sizeof(a.values)));
        require(!memcmp(a.types, b.types, sizeof(a.types)));
        require(!memcmp(a.stackTypes, b.stackTypes, sizeof(a.stackTypes)));
        require(!memcmp(&a.random, &b.random, sizeof(a.random)));
        /* The unprofiled native, as in play. */
        require(!memcmp(a.values, c.values, sizeof(a.values)));
        require(!memcmp(a.types, c.types, sizeof(a.types)));
        require(!memcmp(a.stackTypes, c.stackTypes, sizeof(a.stackTypes)));
        require(!memcmp(&a.random, &c.random, sizeof(a.random)));
        printf("snowfloor scenario %d: ops %llu interpreted, %llu native\n", i, (unsigned long long) a.ops, (unsigned long long) b.ops);
    }
    puts("PASS: snow dots, types, flag, random state, fallback and opcode counts, profiled and unprofiled");
    return 0;
}
