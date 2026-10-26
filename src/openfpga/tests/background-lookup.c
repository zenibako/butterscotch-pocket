/* Real game bytecode is the behavioral oracle. Instrument only this VM copy
 * to prove successful lookups avoid interpretation, independently of pixels. */
#ifdef NDEBUG
#undef NDEBUG /* These executables must retain their behavioral checks. */
#endif
#include "../../vm.c"
#include "of_native.h"
#include "of_game.h"
#include <assert.h>
static unsigned entries, workFailures, checks;
static bool observe;
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *caller) {
    (void)caller;if (observe && fn==(void*)executeBytecodeInstructions) ++entries;
}
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *caller) {(void)fn;(void)caller;}
static const char *asset, *script="gml_Script___background_get_element";
typedef struct { RValue values[3]; intptr_t owner, currentOwner; int layers; uint32_t nextId; } Outcome;
static Outcome run(int native, int scenario, GMLReal epsilon, RValue bind, bool fast) {
    DataWinParserOptions options={0};
    options.parseGen8=options.parseScpt=options.parseCode=options.parseVari=options.parseFunc=options.parseStrg=true;
    DataWin *data=DataWin_parse(asset,options);assert(data);
    Runner r={0};r.dataWin=data;r.pendingRoom=-1;r.mathEpsilon=epsilon;r.nextLayerId=1000;
    VMContext *vm=VM_create(data);r.vmContext=vm;vm->runner=&r;
    CallFrame caller={0};vm->currentCallFrame=&caller;
    caller.self=Instance_create(50,-1,0,0);
    Room room={0};RoomLayer staticLayers[3]={0};room.width=320;room.height=240;
    room.layers=staticLayers;room.layerCount=3;r.currentRoom=&room;
    const char *name=scenario==1 ? "Compatibility_Foreground_0" : "Compatibility_Background_0";
    if (scenario==3) name="XCompatibility_Background_0";
    if (scenario==4) name="Compatibility_Foreground_0Compatibility_Background_1";
    if (scenario==11) name="\xc3\xa9" "Compatibility_Background_0";
    if (scenario==12) name="Compatibility_Background_A";
    if (scenario==13) name="Compatibility_Background_";
    for (int i=0;i<3;++i) {
        staticLayers[i].id=(uint32_t)(10+i);staticLayers[i].name=(char*)(i==0 ? name : "decor");
        RuntimeLayer layer={0};layer.id=(uint32_t)(10+i);arrput(r.runtimeLayers,layer);
    }
    if (scenario==2) {staticLayers[0].name="decor";staticLayers[1].name=(char*)name;}
    if (scenario==5) staticLayers[1].name=(char*)name;
    if (scenario==8) {r.runtimeLayers[0].dynamic=true;r.runtimeLayers[0].dynamicName=strdup(name);staticLayers[0].name="decor";}
    if (scenario==14) staticLayers[0].name="Compatibility_Background_9";
    if (scenario==15) {r.runtimeLayers[1].id=10;staticLayers[1].name="ignored duplicate id";}
    for (int i=0;i<4;++i) {
        RuntimeLayerElement el={0};el.id=(uint32_t)(100+i);
        el.type=i==0 ? RuntimeLayerElementType_Tile : RuntimeLayerElementType_Background;
        if (scenario==7 && i==3) el.id=100;
        int target=scenario==2 ? 1 : 0;
        if (scenario==5) target=1; /* first matching layer is empty: do not search next */
        arrput(r.runtimeLayers[target].elements,el);
    }
    if (scenario==16) shdel(vm->builtinMap,"layer_get_element_type");
    if (scenario==17) {r.runtimeLayers[0].id=16777217u;staticLayers[0].id=16777217u;}
    if (native) unsetenv("BS_NO_NATIVE");else setenv("BS_NO_NATIVE","1",1);
    ofNativeInstall(&r);
    int code=findCodeIndexByName(vm,script);assert(code>=0);
    if (native && vm->nativeScript && vm->nativeScript[code]) {
        VMNativeScript f=vm->nativeScript[code];
        for (size_t i=0;i<ofGame.nativeScriptCount;++i) {
            const OfNativeScript *row=&ofGame.nativeScripts[i];
            if (strcmp(row->name,script)) continue;
            bool wrongLength=VM_setNativeScript(vm,row->name,row->length+4,row->hash,row->native);
            bool wrongHash=VM_setNativeScript(vm,row->name,row->length,row->hash^1,row->native);
            assert(!wrongLength && !wrongHash);
            assert(vm->nativeScript[code]==f);
        }
        void *before=vm->currentArrayOwner;RValue unused=RValue_makeUndefined();
        bool accepted=f(vm,NULL,0,&unused);
        assert(!accepted && vm->currentArrayOwner==before);
        RValue wrong=RValue_makeString("0");accepted=f(vm,&wrong,1,&unused);
        assert(!accepted);
        assert(vm->currentArrayOwner==before && unused.type==RVALUE_UNDEFINED);
        RValue nan=RValue_makeReal(NAN);accepted=f(vm,&nan,1,&unused);
        assert(!accepted);
        assert(vm->currentArrayOwner==before && unused.type==RVALUE_UNDEFINED);
    }
    RValue local=RValue_makeString("caller"),outer=RValue_makeInt32(789);
    vm->currentCallFrame->localVars=&local;vm->currentCallFrame->localVarsCount=1;vm->currentCallFrame->scriptArgs=&outer;vm->currentCallFrame->scriptArgsCount=1;
    vm->currentCallFrame->codeIndex=-1;
    stackPush(vm,RValue_makeInt32(456));vm->currentArrayOwner=(void*)(intptr_t)123;
    Outcome out={0};
    for (int pass=0;pass<(scenario==9 || scenario==10 || scenario==18 || scenario==19 ? 2 : 1);++pass) {
        if (pass && scenario==9) {staticLayers[0].name="decor";staticLayers[1].name=(char*)name;}
        if (pass && scenario==10) {r.runtimeLayers[0].elements[3].type=RuntimeLayerElementType_Tile;}
        if (pass && scenario==18) {
            RValue id=RValue_makeInt32(10), unused=VM_findBuiltin(vm,"layer_destroy")(vm,&id,1);RValue_free(&unused);
            RValue make[]={RValue_makeInt32(0),RValue_makeString("Compatibility_Background_0")};
            RValue layer=VM_findBuiltin(vm,"layer_create")(vm,make,2);
            RValue bg[]={layer,RValue_makeInt32(-1)};
            unused=VM_findBuiltin(vm,"layer_background_create")(vm,bg,2);RValue_free(&unused);
        }
        if (pass && scenario==19) {
            RValue move[]={RValue_makeInt32(103),RValue_makeInt32(11)};
            RValue unused=VM_findBuiltin(vm,"layer_element_move")(vm,move,2);RValue_free(&unused);
        }
        entries=0;observe=true;RValue result=VM_callCodeIndex(vm,code,&bind,1);observe=false;
        assert(result.type==RVALUE_ARRAY && GMLArray_length1D(result.array)==3);
        for (int i=0;i<3;++i) out.values[i]=GMLArray_get(result.array,i);
        out.owner=(intptr_t)result.array->owner;out.currentOwner=(intptr_t)vm->currentArrayOwner;
        assert(out.owner==18385354 && out.currentOwner==18385354);
        if (scenario==0 && RValue_toReal(bind)<0.1f) {assert(out.values[0].type==RVALUE_REAL && out.values[0].real==103);
            assert(out.values[1].type==RVALUE_REAL && out.values[1].real==10);
            assert(out.values[2].type==RVALUE_INT32 && out.values[2].gmlStackType==GML_TYPE_VARIABLE && out.values[2].int32==0);}
        if (native && fast && entries) ++workFailures;
        if (!native || !fast) assert(entries>0);
        RValue alias=RValue_makeIndependent(result);
        vm->currentArrayOwner=(void*)(intptr_t)999;
        VM_arraySetWithCoW(vm,&alias,0,RValue_makeInt32(777));
        assert(alias.array!=result.array && GMLArray_get(alias.array,0).int32==777);
        assert(GMLArray_get(result.array,0).type==out.values[0].type &&
               RValue_toReal(GMLArray_get(result.array,0))==RValue_toReal(out.values[0]));
        RValue_free(&alias);RValue_free(&result);
        assert(vm->currentCallFrame->localVars==&local && vm->currentCallFrame->localVarsCount==1 && !strcmp(local.string,"caller"));
        assert(vm->currentCallFrame->scriptArgs==&outer && vm->currentCallFrame->scriptArgsCount==1 && outer.int32==789);
        assert(vm->stack.top==1 && vm->stack.slots[0].int32==456 && caller.parent==NULL);
        assert(vm->currentCallFrame==&caller && caller.codeIndex==UINT32_MAX);
        ++checks;
    }
    out.layers=(int)arrlen(r.runtimeLayers);out.nextId=r.nextLayerId;
    vm->currentCallFrame->localVars=NULL;vm->currentCallFrame->localVarsCount=0;vm->currentCallFrame->scriptArgs=NULL;vm->currentCallFrame->scriptArgsCount=0;
    RValue sentinelValue=stackPop(vm);assert(sentinelValue.type==RVALUE_INT32 && sentinelValue.int32==456);
    RValue_free(&sentinelValue);
    Instance_free(caller.self);vm->currentCallFrame=NULL;VM_free(vm);
    for (ptrdiff_t i=0;i<arrlen(r.runtimeLayers);++i) {
        for (ptrdiff_t j=0;j<arrlen(r.runtimeLayers[i].elements);++j)
            if (r.runtimeLayers[i].elements[j].backgroundElement) free(r.runtimeLayers[i].elements[j].backgroundElement);
        arrfree(r.runtimeLayers[i].elements);free(r.runtimeLayers[i].dynamicName);
    }
    arrfree(r.runtimeLayers);DataWin_free(data);return out;
}
int main(int argc,char **argv) {
    assert(argc==2);asset=argv[1];
    for (int s=0;s<20;++s) {
        bool fast=s!=3 && s!=11 && s!=12 && s!=13 && s!=16 && s!=17;
        RValue bind=RValue_makeReal(s==14 ? 9 : 0);
        Outcome a=run(0,s,GML_MATH_EPSILON,bind,false),b=run(1,s,GML_MATH_EPSILON,bind,fast);
        assert(!memcmp(&a,&b,sizeof(a)));
    }
    Outcome a=run(0,0,0.5f,RValue_makeReal(0),false),b=run(1,0,0.5f,RValue_makeReal(0),false);
    assert(!memcmp(&a,&b,sizeof(a)));
    const RValue nearby[]={RValue_makeInt32(0),RValue_makeReal(GML_MATH_EPSILON/2),RValue_makeReal(0.5f)};
    for (int i=0;i<3;++i) {
        a=run(0,0,GML_MATH_EPSILON,nearby[i],false);
        b=run(1,0,GML_MATH_EPSILON,nearby[i],i<2);
        assert(!memcmp(&a,&b,sizeof(a)));
    }
    printf("background lookup: %u checks, %u work failures\n",checks,workFailures);
    return workFailures ? 1 : 0;
}
