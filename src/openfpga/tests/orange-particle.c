#include "of_game.h"
#include "of_native.h"
#include "profiler.h"
#include "stb_ds.h"

enum { GROW, SHRINK, NEAR_GOAL, CUSTOM_EPSILON, INTEGER_PHASE, EXPIRE, SCENARIO_COUNT };
static const char *const fields[] = {"goalsize", "size", "siner", "gg", "rr", "vv"};
static const char *const event = "gml_Object_obj_orangeparticle_Step_0";
enum { FIELD_COUNT = sizeof(fields) / sizeof(fields[0]) };

typedef struct {
    GMLReal fields[FIELD_COUNT];
    uint8_t types[FIELD_COUNT], stackTypes[FIELD_COUNT];
    float x, y, xscale, yscale, alpha;
    bool active, destroyed;
    uint64_t ops;
} Outcome;

static Outcome run(DataWin *dw, int code, int obj, int test, bool native) {
    VMContext *vm = VM_create(dw);
    Runner r = {0};
    r.dataWin = dw;
    r.vmContext = vm;
    r.mathEpsilon = test == CUSTOM_EPSILON ? (GMLReal)0.2 : GML_MATH_EPSILON;
    vm->runner = &r;
    r.spatialGrid = SpatialGrid_create(640, 480);
    r.pendingRoom = -1;
    EventSlotMap_build(&r.eventSlotMap, dw);
    ResolvedEventTable_build(&r.eventTable, dw, &r.eventSlotMap);
    Instance *self = Instance_create(100001, obj, 123.25f, 246.5f);
    self->imageAlpha = test == EXPIRE ? 0.011f : 0.99f;
    GMLReal values[FIELD_COUNT] = {1.5f, 0.75f, 12.25f, 3.25f, 0.125f, -0.2f};
    if (test == SHRINK) values[0] = 0.3f;
    if (test == NEAR_GOAL) values[1] = 1.49f;
    if (test == CUSTOM_EPSILON) values[1] = 1.25f;
    int32_t ids[FIELD_COUNT];
    for (int i = 0; i < FIELD_COUNT; i++) {
        ids[i] = VM_getOrAllocateVarID(vm, fields[i]);
        RValue value = test == INTEGER_PHASE && i == 2 ? RValue_makeInt32(12) : RValue_makeReal(values[i]);
        Instance_setSelfVar(self, ids[i], value);
    }

    if (native) {
        require(ofGame.nativeCount == 1);
        const OfNativeCode *spec = ofGame.natives;
        require(!VM_setNativeCode(vm, event, spec->length, spec->hash ^ 1, spec->native));
        ofNativeInstall(&r);
        require(vm->nativeCode != NULL && vm->nativeCode[code] != NULL);
    }
    Profiler_setEnabled(&vm->profiler, true);
    int steps = test == EXPIRE ? 1 : 3;
    for (int n = 0; n < steps; n++) {
        RValue result = VM_executeCode(vm, code, self, NULL, obj, EVENT_STEP, 0, NULL, 0);
        RValue_free(&result);
    }
    Outcome out = {0};
    out.ops = shget(vm->profiler->entries, (char*)event).ops;
    for (int i = 0; i < FIELD_COUNT; i++) {
        RValue value = Instance_getSelfVar(self, ids[i]);
        out.fields[i] = RValue_toReal(value);
        out.types[i] = value.type;
        out.stackTypes[i] = value.gmlStackType;
    }
    out.x = self->x; out.y = self->y; out.xscale = self->imageXscale;
    out.yscale = self->imageYscale; out.alpha = self->imageAlpha;
    out.active = self->active; out.destroyed = self->destroyed;
    ResolvedEventTable_free(&r.eventTable);
    EventSlotMap_destroy(&r.eventSlotMap);
    SpatialGrid_free(r.spatialGrid);
    Instance_free(self);
    VM_free(vm);
    return out;
}

int main(int argc, char **argv) {
    require(argc == 2);
    unsetenv("BS_NO_NATIVE");
    DataWinParserOptions opts = {0};
    opts.parseGen8 = opts.parseScpt = opts.parseCode = opts.parseVari = opts.parseFunc = opts.parseStrg = opts.parseObjt = true;
    int code = -1, obj = -1;
    for (int i = 0; i < SCENARIO_COUNT; i++) {
        DataWin *dw = DataWin_parse(argv[1], opts);
        require(dw != NULL);
        for (uint32_t n = 0; n < dw->code.count; n++)
            if (dw->code.entries[n].name != NULL && !strcmp(dw->code.entries[n].name, event)) code = (int)n;
        for (uint32_t n = 0; n < dw->objt.count; n++)
            if (!strcmp(dw->objt.objects[n].name, "obj_orangeparticle")) obj = (int)n;
        require(code >= 0 && obj >= 0);
        Outcome a = run(dw, code, obj, i, false);
        DataWin_free(dw);
        dw = DataWin_parse(argv[1], opts);
        require(dw != NULL);
        Outcome b = run(dw, code, obj, i, true);
        DataWin_free(dw);
        if (i == INTEGER_PHASE || i == EXPIRE) require(a.ops == b.ops && b.ops > 0);
        else require(a.ops > 0 && b.ops == 0);
        if (i == EXPIRE) require(a.destroyed);
        require(!memcmp(a.fields, b.fields, sizeof(a.fields)));
        require(!memcmp(a.types, b.types, sizeof(a.types)));
        require(!memcmp(a.stackTypes, b.stackTypes, sizeof(a.stackTypes)));
        require(a.x == b.x && a.y == b.y && a.xscale == b.xscale && a.yscale == b.yscale &&
                a.alpha == b.alpha && a.active == b.active && a.destroyed == b.destroyed);
    }
    puts("PASS: particle state, types, fallback and opcode counts");
    return 0;
}
