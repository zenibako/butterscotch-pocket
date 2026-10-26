/* Real game bytecode is the oracle for state, RNG and renderer command order.
 * Input scripts are deterministic test utilities; real input is covered by replays. */
#ifdef NDEBUG
#undef NDEBUG /* These executables must retain their behavioral checks. */
#endif
#include "../../vm.c"
#include "of_native.h"
#include "of_game.h"
#include <assert.h>

static const char *assetPath;
static const char *writerName = "gml_Object_obj_writer_Draw_0";
static unsigned writerEntries, calls;
static bool observe;
static uint64_t trace;
static int button1, button2, button3, debug;
static const char *builtins[] = {"draw_set_font", "draw_set_color", "draw_get_color", "draw_text_color", "draw_text",
                                "random", "string_char_at", "string_length", "string_hash_to_newline"};
static BuiltinFunc originals[9];
static uint32_t color;
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *caller) {
    (void)caller;
    if (observe && fn == (void *)executeBytecodeInstructions) ++writerEntries;
}
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *caller) { (void)fn; (void)caller; }
static void word(uint64_t x) { trace = (trace ^ x) * 1099511628211ULL; }
static void value(RValue x) {
    word(x.type);
    if (x.type == RVALUE_STRING) {
        word(1);
        for (const unsigned char *p = (const unsigned char *)x.string; *p; ++p) word(*p);
        word(0);
    } else if (x.type == RVALUE_ARRAY) {
        word(2); int n = GMLArray_length1D(x.array); word(n);
        for (int i = 0; i < n; ++i) value(GMLArray_get(x.array, i));
    } else if (x.type == RVALUE_UNDEFINED) word(3);
    else { double d = (double)RValue_toReal(x); uint64_t bits; memcpy(&bits, &d, 8); word(4); word(bits); }
}
static RValue traced(int which, VMContext *vm, RValue *args, int32_t n) {
    ++calls; word(which); word(n);
    for (int i = 0; i < n; ++i) value(args[i]);
    RValue result;
    if (which == 1) color = (uint32_t)RValue_toInt32(args[0]);
    if (which == 2) result = RValue_makeReal(color);
    else if (which < 5) result = RValue_makeUndefined();
    else result = originals[which](vm, args, n);
    value(result); return result;
}
#define WRAP(n) static RValue trace##n(VMContext *vm, RValue *a, int32_t c) { return traced(n, vm, a, c); }
WRAP(0) WRAP(1) WRAP(2) WRAP(3) WRAP(4) WRAP(5) WRAP(6) WRAP(7) WRAP(8)
static BuiltinFunc wrappers[] = {trace0, trace1, trace2, trace3, trace4, trace5, trace6, trace7, trace8};
#define INPUT(name, input, id) static bool name(VMContext *vm, RValue *a, int32_t n, RValue *out) { \
    (void)vm; (void)a; (void)n; word(id); *out = RValue_makeReal(input); return true; }
INPUT(key1, button1, 101) INPUT(key2, button2, 102) INPUT(key3, button3, 103) INPUT(isDebug, debug, 104)
static void set(VMContext *vm, Instance *inst, const char *name, RValue x) {
    Instance_setSelfVar(inst, VM_getOrAllocateVarID(vm, name), x);
}
typedef struct { uint64_t commands, state; unsigned calls, entries; Random random; int alarms[12]; uint16_t mask; void *owner; } Outcome;
static uint64_t stateHash(VMContext *vm, Instance *self) {
    uint64_t saved = trace;
    /* Order-independent state digest includes every field, not just coordinates. */
    uint64_t state = 0;
    for (uint32_t i = 0; i < self->selfVars.capacity; ++i) {
        IntRValueEntry *e = &self->selfVars.entries[i]; if (e->key == INT_RVALUE_HASHMAP_EMPTY_KEY) continue;
        trace = 1469598103934665603ULL;
        const char *name = VM_getVariableNameByVarId(vm,e->key);
        for (const char *p = name; *p; ++p) word((unsigned char)*p);
        value(e->value); state ^= trace;
    }
    trace = saved; return state;
}
static unsigned checks, failures;
static Outcome run(const char *text, int pos, int scenario, bool native) {
    DataWinParserOptions opts = {0};
    opts.parseGen8 = opts.parseScpt = opts.parseCode = opts.parseVari = opts.parseFunc = opts.parseStrg = true;
    DataWin *dw = DataWin_parse(assetPath, opts); assert(dw);
    Runner r = {0}; Renderer renderer = {0}; r.dataWin = dw; r.mathEpsilon = GML_MATH_EPSILON;
    r.renderer = &renderer; r.random = Random_create(7); r.pendingRoom = -1;
    VMContext *vm = VM_create(dw); r.vmContext = vm; vm->runner = &r;
    Instance *self = Instance_create(100001, 10, 0, 0); CallFrame caller = {0}; caller.self = self; vm->currentCallFrame = &caller;
    Instance *global = Instance_create(0, STRUCT_OBJECT_INDEX, 0, 0); vm->globalScopeInstance = global;
    const char *num[] = {"formatted", "myfont", "special", "shake", "halt", "mycolor", "pos", "length", "writingx",
        "writingy", "hspace", "vspace", "siner", "skippable", "skipme", "automash_timer", "dialoguer", "wxskip"};
    double val[] = {1,5,1,0,0,16777215,pos,strlen(text),1061,454,16,28,11,1,0,0,0,0};
    for (unsigned i = 0; i < sizeof(val)/sizeof(val[0]); ++i) set(vm, self, num[i], RValue_makeReal(val[i]));
    set(vm, self, "mystring", RValue_makeString(text));
    set(vm, global, "lang", RValue_makeString("en"));
    GMLArray *flags = GMLArray_create(dw, 12);
    set(vm, global, "flag", RValue_makeArray(flags)); GMLArray_decRef(flags);
    for (int i = 0; i < 12; ++i) self->alarm[i] = 7 + i;
    self->activeAlarmMask = 4095;
    button1 = scenario == 1; button2 = scenario == 2; button3 = scenario == 3; debug = scenario == 3;
    if (scenario == 4) set(vm, self, "skipme", RValue_makeReal(1));
    if (scenario == 5) set(vm, self, "shake", RValue_makeReal(1));
    if (scenario == 6) set(vm, self, "myfont", RValue_makeReal(11));
    if (scenario == 7) set(vm, self, "wxskip", RValue_makeReal(1));
    if (scenario == 8) set(vm, self, "skippable", RValue_makeReal(0));
    if (scenario == 9) {
        set(vm, self, "automash_timer", RValue_makeReal(1));
        button3 = debug = 1;
    }
    bool decline = (scenario >= 10 && scenario <= 23) || scenario == 25;
    if (scenario == 10) set(vm, self,"formatted",RValue_makeReal(0));
    if (scenario == 11) set(vm, self,"special",RValue_makeReal(2));
    if (scenario == 12) set(vm, self,"mycolor",RValue_makeReal(0));
    if (scenario == 13) set(vm, self,"halt",RValue_makeReal(1));
    if (scenario == 14) set(vm, global,"lang",RValue_makeString("ja"));
    if (scenario == 15) set(vm, self,"pos",RValue_makeReal(NAN));
    if (scenario == 16) r.mathEpsilon = 0.5f;
    if (scenario == 17) set(vm, self,"length",RValue_makeReal(-1));
    if (scenario == 18) set(vm, self,"hspace",RValue_makeReal(INFINITY));
    if (scenario == 19) set(vm, global,"flag",RValue_makeReal(0));
    if (scenario == 20) set(vm, self,"mystring",RValue_makeString("* Stop/"));
    if (scenario == 21) set(vm, self,"mystring",RValue_makeString("* UTF8 \xc3\xa9"));
    const char *scripts[] = {"gml_Script_button1_p", "gml_Script_button2_h", "gml_Script_button3_h", "gml_Script_scr_debug"};
    VMNativeScript funcs[] = {key1,key2,key3,isDebug};
    for (int i = 0; i < 4; ++i) {
        uint32_t len; uint64_t hash = VM_codeHash(vm, scripts[i], &len);
        assert(hash);
        bool installed = VM_setNativeScript(vm, scripts[i], len, hash, funcs[i]);
        assert(installed);
    }
    for (int i = 0; i < 9; ++i) {
        originals[i] = VM_findBuiltin(vm, builtins[i]); assert(originals[i]);
        shput(vm->builtinMap, builtins[i], wrappers[i]);
        for (uint32_t j = 0; j < dw->func.functionCount; ++j)
            if (!strcmp(dw->func.functions[j].name, builtins[i])) vm->funcCallCache[j].builtin = (void *)wrappers[i];
    }
    if (native) {
        if (scenario == 24) setenv("BS_NO_NATIVE","1",1); else unsetenv("BS_NO_NATIVE");
        ofNativeInstall(&r);
        for (size_t i=0;i<ofGame.nativeCount;++i) {
            const OfNativeCode *entry=&ofGame.natives[i];
            if (strcmp(entry->name,writerName)) continue;
            bool wrongLength = VM_setNativeCode(vm,entry->name,entry->length+4,entry->hash,entry->native);
            bool wrongHash = VM_setNativeCode(vm,entry->name,entry->length,entry->hash^1,entry->native);
            assert(!wrongLength && !wrongHash);
        }
    }
    if (scenario == 22) shdel(vm->builtinMap,"random");
    if (scenario == 23) { dw->code.entries[findCodeIndexByName(vm,scripts[0])].length += 4; }
    if (scenario == 25) {
        CodeEntry *entry = &dw->code.entries[findCodeIndexByName(vm,scripts[0])];
        uint8_t *bytes = dw->bytecodeBuffer + entry->bytecodeAbsoluteOffset - dw->bytecodeBufferBase;
        bytes[entry->offset] ^= 1; /* same length, different callee fingerprint */
    }
    trace = 1469598103934665603ULL; calls = writerEntries = 0; color = 0x123456;
    observe = true;
    if (decline) {
        int code = findCodeIndexByName(vm,writerName);
        assert(native && vm->nativeCode && vm->nativeCode[code]);
        uint32_t count = self->selfVars.count;
        uint64_t beforeSelf = stateHash(vm,self), beforeGlobal = stateHash(vm,global);
        uint16_t beforeMask = self->activeAlarmMask; void *owner = vm->currentArrayOwner;
        bool accepted = vm->nativeCode[code](vm);
        assert(!accepted);
        assert(calls == 0 && writerEntries == 0 && self->selfVars.count == count);
        assert(stateHash(vm,self)==beforeSelf && stateHash(vm,global)==beforeGlobal);
        assert(self->activeAlarmMask==beforeMask && vm->currentArrayOwner==owner && color==0x123456);
        Random initial = Random_create(7); assert(!memcmp(&r.random,&initial,sizeof(initial)));
        for (int i = 0; i < 12; ++i) assert(self->alarm[i] == 7+i);
    } else {
        RValue result = VM_executeCode(vm,findCodeIndexByName(vm,writerName),self,NULL,CODE_NOT_RELATED_TO_EVENTS,CODE_NOT_RELATED_TO_EVENTS,CODE_NOT_RELATED_TO_EVENTS,NULL,0); RValue_free(&result);
    }
    observe = false;
    Outcome out = {.commands=trace, .calls=calls, .entries=writerEntries, .random=r.random, .mask=self->activeAlarmMask, .owner=vm->currentArrayOwner};
    memcpy(out.alarms, self->alarm, sizeof(out.alarms));
    trace = 1469598103934665603ULL;
    out.state = stateHash(vm,self);
    Instance_free(self); Instance_free(global); vm->currentCallFrame = NULL; vm->globalScopeInstance = NULL;
    VM_free(vm); DataWin_free(dw); return out;
}
static void pair(const char *text, int pos, int scenario, bool fast) {
    Outcome a = run(text,pos,scenario,false), b = run(text,pos,scenario,true);
    if (a.commands!=b.commands || a.calls!=b.calls || a.state!=b.state)
        fprintf(stderr,"DIFF scenario%d pos%d commands%llx/%llx calls%u/%u state%llx/%llx\n",scenario,pos,
            (unsigned long long)a.commands,(unsigned long long)b.commands,a.calls,b.calls,
            (unsigned long long)a.state,(unsigned long long)b.state);
    assert(a.commands == b.commands && a.calls == b.calls && a.state == b.state);
    assert(!memcmp(&a.random,&b.random,sizeof(a.random)) && !memcmp(a.alarms,b.alarms,sizeof(a.alarms)) && a.mask == b.mask && a.owner == b.owner);
    if (fast && b.entries != 0) ++failures;
    if (!fast) assert(b.entries == 1);
    ++checks;
}
int main(int argc, char **argv) {
    assert(argc == 2); assetPath = argv[1];
    for (int pos = 1; pos <= 33; ++pos) pair("* Rudinn is glimmering plainly.",pos,0,true);
    for (int pos = 1; pos <= 45; ++pos) pair("* A necklace of Rudinns blocks&||your path.",pos,0,true);
    for (int s = 1; s <= 9; ++s) pair("* A plain sentence.",8,s,s != 5 && s != 6);
    pair("* A plain sentence.",8,5,false);
    pair("* A plain sentence.",8,6,false);
    pair("* Control^1text",8,0,false);
    pair("* First&||second",16,7,true);
    pair("",1,0,true); pair("",2,0,true);
    for (int s = 10; s <= 23; ++s) { run("* Plain text",8,s,true); ++checks; }
    pair("* Native disable.",8,24,false);
    run("* Plain text",8,25,true); ++checks;
    /* An incompatible VM followed by a fresh compatible VM must recover. */
    pair("* Cache reset.",8,0,true);
    if (failures) { fprintf(stderr,"FAIL: %u writer cases still interpreted\n",failures); return 1; }
    printf("PASS: %u writer cases preserve calls, state, alarms and RNG\n",checks); return 0;
}
