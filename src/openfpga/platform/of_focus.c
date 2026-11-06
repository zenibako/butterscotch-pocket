#include "of_focus.h"
#ifdef OF_FOCUSED_BENCH
#include "of.h"
#include "of_bench.h"
#include "of_focus_stats.h"
#include "of_hooks.h"
#include "gettime.h"
#include "runner.h"
#include "instance.h"
#include "profiler.h"
#include "stb_ds.h"
#include "sw_call_notes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OF_FOCUS_BUILD_ID
#define OF_FOCUS_BUILD_ID "unversioned"
#endif
#define WINDOWS 7
#define NOTES 24
/* sw_renderer.c keeps at most 192 descriptions per frame. Drain them all
 * before applying the smaller per-window summary, counting dropped calls. */
#define FRAME_NOTES 192
#define FINISH_FRAME 1355
#define TILE_ROOM 65
static const struct {
    const char *name;
    int first, last, minimum;
    bool reveal;
} windows[WINDOWS] = {
    {"encounter-reveal",400,459,6,true}, {"encounter-full",465,494,20,false},
    {"observation-reveal",820,919,6,true}, {"observation-full",930,979,20,false},
    {"tiles-walk",1070,1139,60,false}, {"tiles-reset-cycle",1140,1239,90,false},
    {"tiles-still",1250,1349,80,false}
};
static const char *const modes[] = {"normal","no-render","no-audio","core","profile"};
static const OfJump jumps[] = {
    {29,-1,"darkzone=1,char[1]=2,char[2]=3,plot=60,interact=0",NULL}, {30,52,NULL,NULL},
    {999,-1,"darkzone=1,char[1]=2,char[2]=3,plot=165,interact=0",NULL}, {1000,TILE_ROOM,NULL,NULL}
};
static const char script[] = "100:R*300,500:Z,515:Z,530:Z,545:Z,560:Z,575:Z,590:Z,605:Z,620:Z,635:Z,650:Z,"
    "665:Z,680:Z,695:Z,710:Z,725:Z,740:Z,755:Z,770:Z,785:Z,800:Z,1040:D*10,1065:R*150";
static int mode=-1, inputFrame, pending=-1;
static uint64_t inputStart, presentStart;
static OfPerfTotals frameTotals;
static OfFocusStats samples[WINDOWS];
static unsigned rejected[WINDOWS];
static ProfilerEntry *profiles[WINDOWS];
static SWCallNote notes[WINDOWS][NOTES];
static unsigned noteCount[WINDOWS], missed[WINDOWS];
static int writerObject=-1, controllerObject=-1, playerObject=-1;
static int32_t posVar, lengthVar, stringVar, whiteVar, activeVar;
static int generatorObject=-1, tileObject=-1, previousWhite;
static unsigned litFrames[WINDOWS], darkFrames[WINDOWS], flashFrames[WINDOWS], resets[WINDOWS];
static bool initialized, previousPlayer;
static float previousX, previousY;
static char failure[112];

static bool noRender(void) { return mode==1 || mode==3; }
static bool noAudio(void) { return mode==2 || mode==3; }
bool ofFocusStart(const char *argument) {
    const char *prefix="--focus-";
    if (strncmp(argument,prefix,strlen(prefix))) return false;
    for (int i=0; i<5; ++i) if (!strcmp(argument+strlen(prefix),modes[i])) mode=i;
    if (mode<0) return false;
    ofSaveFsSetVolatile(true);
    ofPlatformSetSmoothLowres(true);
    ofPlatformSetUncapped(true);
    ofPlatformSetInputScript(script);
    ofPlatformSetJumps(jumps,sizeof(jumps)/sizeof(jumps[0]));
    ofPlatformSetForcedSkip(noRender() ? 100000 : 0);
    ofAudioSetMode(noAudio() ? OF_AUDIO_DISABLED : OF_AUDIO_NORMAL);
    ofPerfHideOverlays();
    ofPerfDetailEvery(mode==4 ? 1 : 0);
    if (mode==4) ofPlatformSetScriptProfile(2147483647);
    return true;
}
static void fail(const char *message) {
    if (!failure[0]) snprintf(failure,sizeof(failure),"%s (input frame %d)",message,inputFrame);
}
static int object(Runner *r, const char *name) {
    for (uint32_t i=0; i<r->dataWin->objt.count; ++i)
        if (!strcmp(r->dataWin->objt.objects[i].name,name)) return (int)i;
    return -1;
}
static Instance *onlyInstance(Runner *r, int objectIndex) {
    if (objectIndex<0 || !r->instancesByObject) return NULL;
    Instance *one=NULL;
    for (ptrdiff_t i=0; i<arrlen(r->instancesByObject[objectIndex]); ++i) {
        Instance *inst=r->instancesByObject[objectIndex][i];
        if (!inst->active || inst->destroyed) continue;
        if (one) return NULL;
        one=inst;
    }
    return one;
}
static unsigned instanceCount(Runner *r, int objectIndex) {
    if (objectIndex<0 || !r->instancesByObject) return 0;
    unsigned count=0;
    for (ptrdiff_t i=0; i<arrlen(r->instancesByObject[objectIndex]); ++i) {
        Instance *inst=r->instancesByObject[objectIndex][i];
        count+=inst->active && !inst->destroyed;
    }
    return count;
}
void ofFocusInput(Runner *r, int frame) {
    if (mode<0) return;
    inputFrame=frame;
    if (!initialized) {
        initialized=true;
        if (!r || r->dataWin->gen8.wadVersion!=17 || strcmp(r->dataWin->gen8.name,"DELTARUNE") ||
            r->dataWin->room.count<=TILE_ROOM || strcmp(r->dataWin->room.rooms[52].name,"room_field2") ||
            strcmp(r->dataWin->room.rooms[TILE_ROOM].name,"room_field_checkers4")) {
            fail("unsupported game/room identity"); return;
        }
        writerObject=object(r,"obj_writer"); controllerObject=object(r,"obj_battlecontroller");
        playerObject=object(r,"obj_mainchara");
        generatorObject=object(r,"obj_ob_gen"); tileObject=object(r,"obj_ob_checkertile");
        if (writerObject<0 || controllerObject<0 || playerObject<0 || generatorObject<0 || tileObject<0) fail("missing scene objects");
        posVar=VM_getOrAllocateVarID(r->vmContext,"pos");
        lengthVar=VM_getOrAllocateVarID(r->vmContext,"length");
        stringVar=VM_getOrAllocateVarID(r->vmContext,"mystring");
        whiteVar=VM_getOrAllocateVarID(r->vmContext,"whitecon");
        activeVar=VM_getOrAllocateVarID(r->vmContext,"active");
    }
    /* These totals contain the last presented frame. Room-transition frames
     * may not present; frame IDs below are input IDs, never presentation IDs. */
    ofPerfTakeTotals(NULL);
    if (mode==4 && r->vmContext->profiler) Profiler_reset(r->vmContext->profiler);
    if (mode==4) { swrCallNotes=true; swrCallNotesTake(NULL,0,NULL); }
    inputStart=nowNanos(); pending=-1;
}
static bool matches(Runner *r, int w, bool moving, int white) {
    if (!ofPlatformSpeedMode() || ofAudioMode()!=(noAudio() ? OF_AUDIO_DISABLED : OF_AUDIO_NORMAL)) {
        fail("performance/audio mode changed"); return false;
    }
    if (w>=4) {
        return r->currentRoomIndex==TILE_ROOM && instanceCount(r,controllerObject)==0 &&
            instanceCount(r,writerObject)==0 && onlyInstance(r,playerObject)!=NULL &&
            onlyInstance(r,generatorObject)!=NULL && instanceCount(r,tileObject)>0 &&
            previousPlayer && white>=0 && (w==5 || (white==0 && moving==(w==4)));
    }
    if (r->currentRoomIndex!=52 || onlyInstance(r,controllerObject)==NULL) return false;
    Instance *writer=onlyInstance(r,writerObject);
    if (!writer) return false;
    RValue text=Instance_getSelfVar(writer,stringVar), pos=Instance_getSelfVar(writer,posVar);
    RValue length=Instance_getSelfVar(writer,lengthVar);
    const char *expected=w<2 ? "* A necklace of Rudinns blocks&||your path." : "* Rudinn is glimmering plainly.";
    if (text.type!=RVALUE_STRING || strcmp(text.string,expected) ||
        (pos.type!=RVALUE_REAL && pos.type!=RVALUE_INT32) ||
        (length.type!=RVALUE_REAL && length.type!=RVALUE_INT32)) return false;
    GMLReal p=RValue_toReal(pos), n=RValue_toReal(length);
    return p>1 && ((p<n)==windows[w].reveal);
}
/* Observe the real hazard state without changing game variables or input. */
static int tileState(Runner *r, bool *lit) {
    Instance *generator=onlyInstance(r,generatorObject);
    if (!generator) return -1;
    RValue white=Instance_getSelfVar(generator,whiteVar);
    if (white.type!=RVALUE_REAL && white.type!=RVALUE_INT32) return -1;
    GMLReal state=RValue_toReal(white);
    if (state!=0 && state!=1 && state!=2) return -1;
    *lit=false;
    for (ptrdiff_t i=0; i<arrlen(r->instancesByObject[tileObject]); ++i) {
        Instance *tile=r->instancesByObject[tileObject][i];
        if (!tile->active || tile->destroyed) continue;
        RValue active=Instance_getSelfVar(tile,activeVar);
        if (active.type!=RVALUE_REAL && active.type!=RVALUE_INT32) return -1;
        if (RValue_toReal(active)==1) *lit=true;
    }
    return (int)state;
}
static void collectProfile(Runner *r, int w) {
    Profiler *profiler=r->vmContext->profiler;
    if (profiler) for (ptrdiff_t i=0; i<shlen(profiler->entries); ++i) {
        ProfilerEntry entry=profiler->entries[i];
        ProfilerStats total=shget(profiles[w],entry.key);
        total.nanos+=entry.value.nanos; total.ops+=entry.value.ops; total.calls+=entry.value.calls;
        shput(profiles[w],entry.key,total);
    }
    static SWCallNote frame[FRAME_NOTES]; uint32_t dropped=0;
    int count=swrCallNotesTake(frame,FRAME_NOTES,&dropped); missed[w]+=dropped;
    for (int n=0; n<count; ++n) {
        unsigned i=0;
        for (; i<noteCount[w]; ++i) if (!strcmp(notes[w][i].what,frame[n].what)) break;
        if (i==noteCount[w]) {
            if (i==NOTES) { missed[w]+=frame[n].calls; continue; }
            notes[w][i]=frame[n]; ++noteCount[w];
        } else { notes[w][i].nanos+=frame[n].nanos; notes[w][i].calls+=frame[n].calls; }
    }
}
void ofFocusPresent(Runner *r, bool skipped) {
    if (mode<0) return;
    ofPerfTakeTotals(&frameTotals);
    Instance *player=r->currentRoomIndex==TILE_ROOM ? onlyInstance(r,playerObject) : NULL;
    bool lit=false;
    int white=player ? tileState(r,&lit) : -1;
    bool reset=player && previousPlayer && previousWhite==1 && white==2 &&
        player->x>=336 && player->x<=344 && previousX-player->x>40;
    bool moving=player && previousPlayer && (player->x!=previousX || player->y!=previousY);
    for (int w=0; w<WINDOWS; ++w) if (inputFrame>=windows[w].first && inputFrame<=windows[w].last) {
        if (matches(r,w,moving,white)) {
            if (skipped!=noRender()) fail("unexpected rendered/skipped sample");
            else {
                pending=w;
                if (w>=4) {
                    if (lit) ++litFrames[w]; else ++darkFrames[w];
                    if (white>0) ++flashFrames[w];
                    if (reset) ++resets[w];
                }
            }
        } else ++rejected[w];
    }
    previousPlayer=player!=NULL; previousWhite=white;
    if (player) { previousX=player->x; previousY=player->y; }
    if (mode==4 && pending>=0) collectProfile(r,pending);
    presentStart=nowNanos();
}
static unsigned micros(uint64_t nanos) { return (unsigned)(nanos/1000); }
static int compareNotes(const void *a, const void *b) {
    const SWCallNote *x=a,*y=b;
    return (x->nanos<y->nanos)-(x->nanos>y->nanos);
}
/* ofLogPrint formats into a 256-byte buffer, including its terminator. */
static void logReport(const char *report) {
    while (*report) {
        size_t length=strlen(report);
        if (length>240) length=240;
        ofLogPrint("%.*s",(int)length,report);
        report+=length;
    }
    ofLogPrint("\n");
}
static void finish(void) {
    if (!resets[5]) fail("no tile-triggered return in reset window");
    if (!litFrames[5] || !darkFrames[5] || !flashFrames[5]) fail("missing tile/white-flash cycle");
    bool good=!failure[0];
    for (int w=0; w<WINDOWS; ++w) if (samples[w].count<(unsigned)windows[w].minimum) good=false;
    ofLogPrint("=== Focused Deltarune benchmark %s ===\n",modes[mode]);
    ofLogPrint("route card-castle-tiles; room 65 room_field_checkers4\n");
    ofLogPrint("build %s; seed 7; Speed; uncapped; input-frame windows\n",OF_FOCUS_BUILD_ID);
#ifndef OF_PC
    const struct of_capabilities *caps=of_get_caps();
    ofLogPrint("OS %u.%u.%u core %u CPU %uMHz\n",(unsigned)(caps->os_version>>16&255),
        (unsigned)(caps->os_version>>8&255),(unsigned)(caps->os_version&255),
        (unsigned)caps->core_variant,(unsigned)(caps->cpu_freq_hz/1000000));
#endif
    ofLogPrint("counterfactual: render=%s audio=%s profile=%s\n",noRender()?"off":"on",noAudio()?"off":"on",mode==4?"ON (instrumented)":"off");
    ofLogPrint("us/frame; work=own frame wall minus own flip; correlated samples\n");
    ofLogPrint("window N filtered mean median p95 max flip present\n");
    for (int w=0; w<WINDOWS; ++w) {
        const OfFocusStats *s=&samples[w]; unsigned n=s->count, divisor=n ? n : 1;
        ofLogPrint("%s %u %u %u %u %u %u %u %u\n",windows[w].name,n,rejected[w],
            micros((s->total-s->flip)/divisor),micros(ofFocusStatsPercentile(s,50)),
            micros(ofFocusStatsPercentile(s,95)),micros(ofFocusStatsPercentile(s,100)),
            micros(s->flip/divisor),micros(s->present/divisor));
        if (w>=4) ofLogPrint("  tiles lit/dark/white-flash frames %u/%u/%u; verified returns %u\n",
            litFrames[w],darkFrames[w],flashFrames[w],resets[w]);
        ofLogPrint("  blocks10 us:");
        for (unsigned i=0; i<n; i+=10) ofLogPrint(" %u",micros(ofFocusStatsBlockMean(s,i,10)));
        ofLogPrint("\n");
        const OfPerfTotals *t=&s->totals; unsigned detail=t->detailFrames ? t->detailFrames : 1;
        ofLogPrint("  step/draw/audio-update us %u %u %u; drawcalls spr/part/text/bkg/rect %u/%u/%u/%u/%u\n",
            micros(t->phaseNanos[OF_PHASE_STEP]/divisor),micros(t->phaseNanos[OF_PHASE_DRAW]/divisor),
            micros(t->phaseNanos[OF_PHASE_AUDIO]/divisor),(unsigned)(t->drawCalls[0]/divisor),
            (unsigned)(t->drawCalls[1]/divisor),(unsigned)(t->drawCalls[2]/divisor),
            (unsigned)(t->drawCalls[3]/divisor),(unsigned)(t->drawCalls[4]/divisor));
        ofLogPrint("  load us room/texture/music/fx %u/%u/%u/%u; audio pairs/fr %u\n",
            micros(t->loadNanos[OF_LOAD_ROOM]/divisor),micros(t->loadNanos[OF_LOAD_TEXTURE]/divisor),
            micros(t->loadNanos[OF_LOAD_MUSIC]/divisor),micros(t->loadNanos[OF_LOAD_SOUND]/divisor),
            (unsigned)(t->loadNanos[OF_LOAD_MIX_PAIRS]/divisor));
        ofLogPrint("  audio output/voice/bulk-silent pairs per frame %u/%u/%u\n",
            (unsigned)(t->loadNanos[OF_LOAD_MIX_PAIRS]/divisor),
            (unsigned)(t->loadNanos[OF_LOAD_MIX_VOICE_PAIRS]/divisor),
            (unsigned)(t->loadNanos[OF_LOAD_MIX_SILENT_PAIRS]/divisor));
        if (mode==4) {
            ofLogPrint("  detail %u; draw us spr/part/text/bkg/rect %u/%u/%u/%u/%u; mix/voices/write %u/%u/%u\n",t->detailFrames,
                micros(t->drawNanos[0]/detail),micros(t->drawNanos[1]/detail),micros(t->drawNanos[2]/detail),
                micros(t->drawNanos[3]/detail),micros(t->drawNanos[4]/detail),micros(t->loadNanos[OF_LOAD_MIX]/detail),
                micros(t->loadNanos[OF_LOAD_MIX_VOICES]/detail),micros(t->loadNanos[OF_LOAD_MIX_WRITE]/detail));
            Profiler view={.entries=profiles[w]}; char *report=Profiler_createReport(&view,8,(int)divisor);
            if (report) { logReport(report); free(report); }
            qsort(notes[w],noteCount[w],sizeof(notes[w][0]),compareNotes);
            ofLogPrint("  draw notes dropped %u\n",missed[w]);
            for (unsigned i=0; i<noteCount[w] && i<5; ++i)
                ofLogPrint("  %u us/fr %.1f calls/fr %s\n",micros(notes[w][i].nanos/divisor),
                    (double)notes[w][i].calls/divisor,notes[w][i].what);
        }
    }
    ofLogPrint("Profile clocks slow work; do not compare profile totals as baseline.\n");
    ofLogPrint("Draw includes scripts + raster + busy-tick audio; mix overlaps draw.\n");
    ofLogPrint("No-render retains Draw scripts/setup; no-audio also removes reads/decode.\n");
    for (int w=0; w<WINDOWS; ++w) if (samples[w].count<(unsigned)windows[w].minimum)
        ofLogPrint("FOCUS WINDOW FAIL %s needs %d samples\n",windows[w].name,windows[w].minimum);
    if (failure[0]) ofLogPrint("FOCUS ERROR %s\n",failure);
    ofLogPrint("FOCUS %s %s\n",good?"PASS":"FAIL",modes[mode]);
    ofLogPrint("--- end of report ---\n");
    bool dumped=ofLogDump();
    ofLogPrint("Focus %s; log %s. Quit via Analogue menu.\n",good?"PASS":"FAIL",dumped?"saved":"WRITE FAILED");
    ofLogPrint("Return Saves/butterscotch/common/deltarune_tiles_1.sav\n");
    mode=-1; swrCallNotes=false;
#ifdef OF_PC
    if (!good || !dumped) exit(1);
#endif
    if (!ofPlatformShowLogAndHalt()) exit(good && dumped ? 0 : 1);
}
void ofFocusEnd(uint64_t flipNanos) {
    if (mode<0) return;
    uint64_t end=nowNanos();
    if (pending>=0) {
        if (end<presentStart || end<inputStart || end-presentStart<flipNanos ||
            !ofFocusStatsAdd(&samples[pending],end-inputStart,flipNanos,end-presentStart-flipNanos,&frameTotals))
            fail("invalid timestamp/sample capacity");
    }
    pending=-1;
    if (failure[0] || inputFrame>=FINISH_FRAME) finish();
}
#endif
