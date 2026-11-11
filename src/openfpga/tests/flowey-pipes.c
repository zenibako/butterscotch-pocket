/* The full-resolution Speed override disables the replacement backdrop.
 * Real Draw bytecode must then retain every pipe draw and state write. */
#include "of_game.h"
#include "of_native.h"
#include "profiler.h"
#include "stb_ds.h"

extern bool swrSmoothMinify;

static const char *const events[] = {
    "gml_Object_obj_floweypipetest_Draw_0",
    "gml_Object_obj_floweypipetest_2_Draw_0",
    "gml_Object_obj_floweypipetest_3_Draw_0"
};
static const char *const objects[] = {"obj_floweypipetest", "obj_floweypipetest_2", "obj_floweypipetest_3"};
static const char *const fields[] = {
    "siner", "frozen", "flash", "blend2", "i", "op", "offx", "offy", "offs", "offv",
    "offx2", "offy2", "offs2", "offv2", "magicfactor", "magicfactor2", "magicfactor3"
};
enum { FIELD_COUNT = sizeof(fields) / sizeof(fields[0]) };

typedef struct {
    unsigned draws;
    uint64_t trace, ops;
    GMLReal values[FIELD_COUNT];
    uint8_t types[FIELD_COUNT], stackTypes[FIELD_COUNT];
    uint32_t blend;
} Outcome;

typedef struct {
    Renderer base;
    Outcome *out;
} TraceRenderer;

static void hashWord(Outcome *out, uint32_t word) {
    out->trace = (out->trace ^ word) * 1099511628211ULL;
}

static void drawSprite(Renderer *renderer, int32_t item, float x, float y, float ox, float oy,
                       float sx, float sy, float angle, uint32_t colour, float alpha) {
    Outcome *out = ((TraceRenderer *)renderer)->out;
    ++out->draws;
    hashWord(out, (uint32_t)item);
    hashWord(out, colour);
    const float args[] = {x, y, ox, oy, sx, sy, angle, alpha};
    for (unsigned i = 0; i < sizeof(args) / sizeof(args[0]); i++) {
        uint32_t bits;
        memcpy(&bits, &args[i], sizeof(bits));
        hashWord(out, bits);
    }
}

static Outcome run(const char *path, int pipe, bool native, bool lowres) {
    DataWinParserOptions opts = {0};
    opts.parseGen8 = opts.parseScpt = opts.parseCode = opts.parseVari = opts.parseFunc = opts.parseStrg = true;
    opts.parseObjt = opts.parseSprt = opts.parseTpag = true;
    DataWin *dw = DataWin_parse(path, opts);
    require(dw != NULL);
    VMContext *vm = VM_create(dw);
    VM_reset(vm);
    Room room = {.name = "room_floweyx"};
    Outcome out = {.trace = 1469598103934665603ULL};
    RendererVtable vtable = {.drawSprite = drawSprite};
    TraceRenderer renderer = {.base = {.vtable = &vtable, .dataWin = dw}, .out = &out};
    Runner runner = {.dataWin = dw, .vmContext = vm, .renderer = &renderer.base,
                     .currentRoom = &room, .mathEpsilon = GML_MATH_EPSILON};
    vm->runner = &runner;
    int code = -1, sprite = -1, object = -1;
    for (uint32_t i = 0; i < dw->code.count; i++)
        if (dw->code.entries[i].name != NULL && !strcmp(dw->code.entries[i].name, events[pipe])) code = (int)i;
    for (uint32_t i = 0; i < dw->sprt.count; i++)
        if (!strcmp(dw->sprt.sprites[i].name, "spr_pipepart")) sprite = (int)i;
    for (uint32_t i = 0; i < dw->objt.count; i++)
        if (!strcmp(dw->objt.objects[i].name, objects[pipe])) object = (int)i;
    require(code >= 0 && sprite >= 0 && object >= 0);
    Instance *self = Instance_create(100001, object, 230, 132);
    self->spriteIndex = sprite;
    int32_t ids[FIELD_COUNT];
    for (int i = 0; i < FIELD_COUNT; i++) {
        ids[i] = VM_getOrAllocateVarID(vm, fields[i]);
        GMLReal number = i == 0 ? (GMLReal)12.25 : i == 5 ? (GMLReal)180 : 0;
        Instance_setSelfVar(self, ids[i], RValue_makeReal(number));
    }
    Instance_setSelfVar(vm->globalScopeInstance, VM_getOrAllocateVarID(vm, "debug"), RValue_makeReal(0));
    Instance_setSelfVar(vm->globalScopeInstance, VM_getOrAllocateVarID(vm, "soul_rescue"), RValue_makeReal(0));

    ofPlatformSetSmoothLowres(true); /* Both cases select Speed. */
    swrSmoothMinify = lowres; /* Full-resolution battles turn off only minification. */
    if (native) {
        ofNativeInstall(&runner);
        require(vm->nativeCode != NULL && vm->nativeCode[code] != NULL);
    }
    Profiler_setEnabled(&vm->profiler, true);
    RValue result = VM_executeCode(vm, code, self, NULL, -1, EVENT_DRAW, DRAW_NORMAL, NULL, 0);
    RValue_free(&result);
    out.ops = shget(vm->profiler->entries, (char *)events[pipe]).ops;
    for (int i = 0; i < FIELD_COUNT; i++) {
        RValue value = Instance_getSelfVar(self, ids[i]);
        out.values[i] = RValue_toReal(value);
        out.types[i] = value.type;
        out.stackTypes[i] = value.gmlStackType;
    }
    out.blend = self->imageBlend;
    Instance_free(self);
    VM_free(vm);
    DataWin_free(dw);
    return out;
}

int main(int argc, char **argv) {
    require(argc == 2);
    unsetenv("BS_NO_NATIVE");
    const unsigned draws[] = {20, 26, 32};
    for (int pipe = 0; pipe < 3; pipe++) {
        Outcome original = run(argv[1], pipe, false, false);
        Outcome fullres = run(argv[1], pipe, true, false);
        require(original.draws == draws[pipe] && original.ops > 0);
        require(fullres.draws == original.draws && fullres.trace == original.trace);
        require(fullres.ops == original.ops);
        require(!memcmp(fullres.values, original.values, sizeof(original.values)));
        require(!memcmp(fullres.types, original.types, sizeof(original.types)));
        require(!memcmp(fullres.stackTypes, original.stackTypes, sizeof(original.stackTypes)));
        require(fullres.blend == original.blend);
        Outcome lowres = run(argv[1], pipe, true, true);
        require(lowres.ops == 0); /* The ordinary 320x240 fast path stays active. */
        require(!memcmp(lowres.values, original.values, sizeof(original.values)));
        printf("PASS: pipe %d retains %u full-resolution draws and state; half-size native uses zero opcodes\n",
               pipe + 1, fullres.draws);
    }
    return 0;
}
