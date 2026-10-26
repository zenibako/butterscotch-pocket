/* Supplied game bytecode is the oracle; no game data is embedded here.
 * Instrument this test translation unit only to prove interpreter work leaves. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../../vm.c"
#include "of_game.h"
#include "of_native.h"
#include <assert.h>

static const char *assetPath;
static const char *eventName = "gml_Object_obj_darkponman_ow_Step_0";
static unsigned entries, calls, checks, workFailures;
static bool observe, behaviorOnly;
static uint64_t trace;
static const char *names[] = {"lengthdir_x", "lengthdir_y", "sin", "instance_exists"};
static BuiltinFunc originals[4];
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *caller) {
    (void)caller;
    if (observe && fn == (void *)executeBytecodeInstructions) ++entries;
}
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *caller) {
    (void)fn; (void)caller;
}
static void word(uint64_t x) { trace = (trace ^ x) * 1099511628211ULL; }
static void value(RValue x) {
    word(x.type);
    if (x.type == RVALUE_ARRAY) {
        int n = GMLArray_length1D(x.array); word(n);
        for (int i = 0; i < n; ++i) value(GMLArray_get(x.array,i));
    } else if (x.type == RVALUE_STRING) {
        for (const unsigned char *p = (const unsigned char *)x.string; *p; ++p) word(*p);
    } else if (x.type != RVALUE_UNDEFINED) {
        GMLReal n = RValue_toReal(x); uint32_t bits; memcpy(&bits,&n,sizeof(bits)); word(bits);
    }
}
static RValue traced(int i, VMContext *vm, RValue *a, int32_t n) {
    ++calls; word(i); word(n); word((uintptr_t)vm->currentArrayOwner);
    for (int k=0;k<n;++k) value(a[k]);
    RValue result=originals[i](vm,a,n); value(result);
    if(getenv("SENTRY_TRACE") && calls<24) {
        fprintf(stderr,"CALL %s",names[i]);
        for(int k=0;k<n;++k)fprintf(stderr," %d:%.9g",a[k].type,(double)RValue_toReal(a[k]));
        fprintf(stderr," => %d:%.9g\n",result.type,(double)RValue_toReal(result));
    }
    return result;
}
#define WRAP(n) static RValue trace##n(VMContext *v,RValue *a,int32_t c) { return traced(n,v,a,c); }
WRAP(0) WRAP(1) WRAP(2) WRAP(3)
static BuiltinFunc wrappers[]={trace0,trace1,trace2,trace3};
static void set(VMContext *vm, Instance *i, const char *name, RValue x) {
    Instance_setSelfVar(i,VM_getOrAllocateVarID(vm,name),x);
}
static Instance *add(Runner *r,int id,int obj,float x,float y) {
    Instance *i=Instance_create(id,obj,x,y);
    arrput(r->instances,i); hmput(r->instancesById,id,i); arrput(r->instancesByObject[obj],i); return i;
}
static uint64_t state(VMContext *vm) {
    uint64_t saved=trace; trace=1469598103934665603ULL;
    Runner *r=vm->runner;
    for (int k=0;k<arrlen(r->instances);++k) {
        Instance *i=r->instances[k]; word(i->instanceId);
        float fields[]={i->x,i->y,i->imageAngle,i->imageAlpha,i->imageSpeed};
        for (unsigned j=0;j<sizeof(fields)/sizeof(fields[0]);++j) value(RValue_makeReal(fields[j]));
        uint64_t variables=0;
        for (uint32_t j=0;j<i->selfVars.capacity;++j) {
            IntRValueEntry *e=&i->selfVars.entries[j]; if(e->key==INT_RVALUE_HASHMAP_EMPTY_KEY)continue;
            uint64_t before=trace; trace=1469598103934665603ULL;
            const char *name=VM_getVariableNameByVarId(vm,e->key);
            for(const char *p=name;*p;++p)word((unsigned char)*p);
            value(e->value); variables^=trace; trace=before;
        }
        word(variables); word(i->activeAlarmMask);
        for(int j=0;j<12;++j)word(i->alarm[j]);
    }
    for(int j=0;j<arrlen(r->spatialGrid->dirtyInstances);++j)word(r->spatialGrid->dirtyInstances[j]);
    word((uintptr_t)vm->currentArrayOwner);
    uint64_t result=trace; trace=saved; return result;
}
typedef struct { uint64_t commands,state; unsigned calls,entries; Random random; } Outcome;
static Outcome run(int scenario,bool native,bool decline) {
    DataWinParserOptions opts={0};
    opts.parseGen8=opts.parseScpt=opts.parseCode=opts.parseVari=opts.parseFunc=opts.parseStrg=opts.parseObjt=true;
    DataWin *dw=DataWin_parse(assetPath,opts); assert(dw);
    if(scenario==27)for(uint32_t i=0;i<dw->vari.variableCount;++i)
        if(dw->vari.variables[i].varID>=0)dw->vari.variables[i].varID+=100000;
    Runner r={0}; r.dataWin=dw; r.mathEpsilon=GML_MATH_EPSILON; r.pendingRoom=-1; r.random=Random_create(7);
    VMContext *vm=VM_create(dw); r.vmContext=vm; vm->runner=&r;
    arrsetlen(r.instancesByObject,dw->objt.count);
    memset(r.instancesByObject,0,dw->objt.count*sizeof(*r.instancesByObject));
    r.spatialGrid=SpatialGrid_create(2048,2048);
    Instance *self=add(&r,100001,185,430.25f,650.75f); CallFrame caller={0};caller.self=self;vm->currentCallFrame=&caller;
    Instance *global=Instance_create(0,STRUCT_OBJECT_INDEX,0,0); vm->globalScopeInstance=global;
    const char *vars[]={"rot","rotspeed","eyer","offx","offy","boss","con","made","bmax","siner","xrange","yrange"};
    int nums[]={276,4,11,30,30,1,1,1,8,69,300,240};
    for(unsigned i=0;i<sizeof(nums)/sizeof(nums[0]);++i)
        set(vm,self,vars[i],scenario==1?RValue_makeReal((GMLReal)nums[i]+(i<5?0.25f:0)):RValue_makeInt32(nums[i]));
    Instance *player=add(&r,100002,30,500,660);
    Instance *otherPlayer=add(&r,100003,30,1200,900);
    set(vm,player,"battlemode",RValue_makeInt32(9)); set(vm,otherPlayer,"battlemode",RValue_makeInt32(9));
    GMLArray *fb=GMLArray_create(dw,16);
    for(int i=0;i<16;++i) {
        Instance *bullet=add(&r,100010+i,187,10+i,20+i);
        GMLArray_set(fb,i,RValue_makeReal(bullet->instanceId));
        if(scenario==4 && i%2==0)bullet->active=false;
        if(scenario==5 && i==3)hmput(r.instancesById,bullet->instanceId,NULL);
    }
    set(vm,self,"fb",RValue_makeArray(fb));
    set(vm,global,"alias",RValue_makeArray(fb)); /* read-only aliases must remain shared */
    GMLArray_decRef(fb);
    if(scenario==2)set(vm,self,"rotspeed",RValue_makeInt32(-4));
    if(scenario==3)set(vm,self,"rotspeed",RValue_makeInt32(0));
    if(scenario==6)set(vm,self,"boss",RValue_makeInt32(0));
    if(scenario==7)player->active=otherPlayer->active=false;
    if(scenario==8)set(vm,self,"bmax",RValue_makeInt32(0));
    if(scenario==9)set(vm,self,"bmax",RValue_makeReal(10.5f));
    if(scenario==10)set(vm,self,"boss",RValue_makeInt32(0)),set(vm,self,"con",RValue_makeInt32(0));
    if(scenario==11)r.mathEpsilon=0.5f;
    if(scenario==12)set(vm,self,"eyer",RValue_makeReal(NAN));
    if(scenario==13)set(vm,self,"rot",RValue_makeString("276"));
    if(scenario==14)set(vm,self,"bmax",RValue_makeInt32(17));
    if(scenario==15)set(vm,self,"rotspeed",RValue_makeReal(INFINITY));
    if(scenario==16)set(vm,self,"made",RValue_makeInt32(0));
    for(int i=0;i<4;++i) {
        originals[i]=VM_findBuiltin(vm,names[i]); assert(originals[i]); shput(vm->builtinMap,names[i],wrappers[i]);
        for(uint32_t j=0;j<dw->func.functionCount;++j)
            if(!strcmp(dw->func.functions[j].name,names[i]))vm->funcCallCache[j].builtin=(void*)wrappers[i];
    }
    if(scenario==17)shdel(vm->builtinMap,"sin");
    if(scenario==21)player->x=1500;
    if(scenario==22 || scenario==23) {
        player->x=1500;
        Instance *statue=add(&r,100100,185,1500,660);
        set(vm,statue,"xrange",RValue_makeReal(20));set(vm,statue,"yrange",RValue_makeReal(20));
        if(scenario==23)statue->active=false;
    }
    if(scenario==24)GMLArray_set(fb,3,GMLArray_get(fb,0)); /* repeated target, same ordered writes */
    if(scenario==25)set(vm,self,"con",RValue_makeReal(0.5f));
    if(scenario==26)for(int j=0;j<3;++j) {
        Instance *statue=add(&r,100100+j,185,480+j*400,660);
        set(vm,statue,"xrange",RValue_makeReal(30));set(vm,statue,"yrange",RValue_makeReal(20));
    }
    if(native && scenario!=18 && scenario!=19 && scenario!=20)unsetenv("BS_NO_NATIVE");else setenv("BS_NO_NATIVE","1",1);
    ofNativeInstall(&r);
    int code=findCodeIndexByName(vm,eventName); assert(code>=0);
    if(native && (scenario==19 || scenario==20)) {
        for(size_t i=0;i<ofGame.nativeCount;++i) {
            const OfNativeCode *n=&ofGame.natives[i];if(strcmp(n->name,eventName))continue;
            assert(!VM_setNativeCode(vm,n->name,n->length+(scenario==19?4:0),n->hash^(scenario==20?1:0),n->native));
        }
    }
    if(native && vm->nativeCode && vm->nativeCode[code]) {
        const OfNativeCode *n=NULL;
        for(size_t i=0;i<ofGame.nativeCount;++i)if(!strcmp(ofGame.natives[i].name,eventName))n=&ofGame.natives[i];
        assert(n);
        assert(!VM_setNativeCode(vm,n->name,n->length+4,n->hash,n->native));
        assert(!VM_setNativeCode(vm,n->name,n->length,n->hash^1,n->native));
    }
    trace=1469598103934665603ULL; calls=entries=0; vm->currentArrayOwner=(void*)(uintptr_t)123;
    uint64_t initial=state(vm); observe=true;
    if(decline) {
        if(!vm->nativeCode || !vm->nativeCode[code]) ++workFailures;
        else assert(!vm->nativeCode[code](vm));
        assert(calls==0 && entries==0 && initial==state(vm));
    } else for(int step=0;step<3;++step) {
        RValue result=VM_executeCode(vm,code,self,NULL,185,EVENT_STEP,0,NULL,0); RValue_free(&result);
    }
    observe=false;
    assert(vm->currentCallFrame==&caller && caller.self==self && vm->globalScopeInstance==global);
    assert(Instance_getSelfVar(self,VM_getOrAllocateVarID(vm,"fb")).array==
           Instance_getSelfVar(global,VM_getOrAllocateVarID(vm,"alias")).array);
    Outcome out={trace,state(vm),calls,entries,r.random};
    if(getenv("SENTRY_TRACE")) {
        fprintf(stderr,"STATE scenario=%d native=%d owner=%p\n",scenario,native,vm->currentArrayOwner);
        for(int i=0;i<arrlen(r.instances);++i) {
            Instance *in=r.instances[i];
            fprintf(stderr,"FIELD %u %.9g %.9g %.9g %.9g\n",in->instanceId,(double)in->x,(double)in->y,(double)in->imageAngle,(double)in->imageAlpha);
        }
    }
    for(int i=0;i<arrlen(r.instances);++i)Instance_free(r.instances[i]);
    for(uint32_t i=0;i<dw->objt.count;++i)arrfree(r.instancesByObject[i]);
    arrfree(r.instancesByObject); arrfree(r.instances); arrfree(r.instanceSnapshots); hmfree(r.instancesById);
    SpatialGrid_free(r.spatialGrid); Instance_free(global); vm->currentCallFrame=NULL;vm->globalScopeInstance=NULL;
    VM_free(vm); DataWin_free(dw); return out;
}
int main(int argc,char **argv) {
    assert(argc>=2); assetPath=argv[1]; behaviorOnly=argc>2 && !strcmp(argv[2],"--behavior-only");
#ifndef GAME_DELTARUNE
    for(size_t i=0;i<ofGame.nativeCount;++i)assert(strcmp(ofGame.natives[i].name,eventName));
    puts("PASS: sentry native excluded from Undertale"); return 0;
#else
    for(int s=0;s<=27;++s) {
        if(s>=10 && s<=17) { run(s,true,true); ++checks; continue; }
        Outcome a=run(s,false,false),b=run(s,true,false);
        if(a.commands!=b.commands || a.state!=b.state || a.calls!=b.calls)
            fprintf(stderr,"DIFF scenario %d calls %u/%u trace %llx/%llx state %llx/%llx\n",s,a.calls,b.calls,
                (unsigned long long)a.commands,(unsigned long long)b.commands,(unsigned long long)a.state,(unsigned long long)b.state);
        assert(a.commands==b.commands && a.state==b.state && a.calls==b.calls && !memcmp(&a.random,&b.random,sizeof(a.random)));
        if((s<18 || s>20) && b.entries!=0)++workFailures;
        if(s>=18 && s<=20)assert(b.entries==3);
        ++checks;
    }
    if(workFailures && !behaviorOnly) { fprintf(stderr,"FAIL: %u sentry cases lack native execution\n",workFailures); return 1; }
    printf("PASS: %u sentry cases preserve calls, state, ownership and RNG (%u work failures)\n",checks,workFailures); return 0;
#endif
}
