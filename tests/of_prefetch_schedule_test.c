/* A large first texture must yield, rather than poisoning the estimate for
 * every later texture and blocking the bounded read-ahead queue. */
#define OF_PC 1
#include "../src/openfpga/platform/of_platform.c"
#include <assert.h>
#include <stdarg.h>
static uint64_t clockNanos;
static unsigned tiles, wholeLoads, cancelCount;
uint64_t nowNanos(void) { return clockNanos; }
static int nextBattle, pollResult;
static int candidate(Runner *r,int32_t *entries,int max,int *battle,int *left) {
    (void)r; (void)max; entries[0]=7; *battle=nextBattle; *left=30; return 1;
}
const OfGame ofGame = {.battlePrefetch=candidate};
void platformLog(logType type, const char *fmt, va_list ap) { (void)type; (void)fmt; (void)ap; }
int SWRenderer_prefetchItem(Renderer *r,int32_t item) { (void)r; ++wholeLoads; clockNanos+=item==0?32000000:1000000; return 0; }
int SWRenderer_prefetchItemStep(Renderer *r,int32_t item,void **state) {
    (void)r; clockNanos+=500000; ++tiles;
    unsigned n=(unsigned)(uintptr_t)*state+1;
    if(n<(item==0?64u:2u)) { *state=(void*)(uintptr_t)n; return 4; }
    *state=NULL; return 0;
}
void SWRenderer_prefetchCancel(void **state) { if(*state)++cancelCount; *state=NULL; }
int SWRenderer_planItem(Renderer *r,int32_t i,uint32_t (*s)[2],int m) { (void)r;(void)i;(void)s;(void)m;return -1; }
uint32_t ofBgReadMax(void) { return 32768; }
int ofBgReadPoll(void) { int r=pollResult; pollResult=OF_BG_IDLE; return r; }
void ofBgReadSettle(void) {}
bool ofBgReadStart(const char*p,uint32_t o,uint32_t n,void*d) { (void)p;(void)o;(void)n;(void)d;return false; }
void SWRenderer_takeItemLoads(unsigned*n,uint64_t*t) { *n=0;*t=0; }
void ofBenchNoteBattleLoads(unsigned n,uint64_t t) { (void)n;(void)t; }
void swrTexturePackPlanReset(void) {}
int ofAudioPreloadSound(int32_t s) { (void)s;return 0; }
void ofBgReadTotals(uint32_t*r,uint64_t*n,uint64_t*s) { *r=0;*n=0;*s=0; }
bool swrTexturePackStage(uint32_t s,uint32_t n,uint16_t*d,int t) { (void)s;(void)n;(void)t;assert(d);free(d);return true; }
bool swrTexturePackHasStaged(uint32_t s,uint32_t n,int t) { (void)s;(void)n;(void)t;return false; }
uint32_t swrTexturePackUnstage(int t) { (void)t; return 0; }
void swrTexturePackGetReadTotals(SwrTexturePackReadTotals*t) { memset(t,0,sizeof(*t)); }
const char*swrTexturePackFile(void) { return "textures.bin"; }
uint32_t swrTexturePackOffset(void) { return 0; }
int ofAudioPlanSound(int32_t s,uint32_t*o,uint32_t*n) { (void)s;(void)o;(void)n;return -1; }
int ofAudioAdoptSound(int32_t s,uint8_t*d) { (void)s;free(d);return 0; }
const char*ofAudioPackPath(void) { return "music.bin"; }
void ofBenchNotePrefetch(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,uint64_t f,uint64_t g) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g; }
int main(void) {
    Runner runner; memset(&runner,0,sizeof(runner)); runner.renderer=(Renderer*)(uintptr_t)1; g_runner=&runner;
    g_prefetchCount=40; g_readEntry=40;
    for(int i=0;i<40;i++) { g_prefetchEntries[i]=i; g_slots[i].state=PF_READY; g_slots[i].plan=0; }
    for(int i=0;i<48;i++) {
        clockNanos=(uint64_t)i*33333333; g_lateUsed=0;
        uint64_t before=clockNanos;
        prefetchLoadReady(before); /* Busy draw: only bounded late work is available. */
        assert(clockNanos-before<=4000000);
    }
    assert(wholeLoads==0);
    assert(g_prefetchNext==g_prefetchCount);
    assert(tiles==64+39*2);
    uint64_t before=clockNanos; prefetchEnd(); assert(clockNanos==before);
    /* Ending an incomplete burst cancels it, without draining ready work. */
    g_prefetchCount=1; g_prefetchNext=g_readEntry=0; g_readEntry=1;
    g_slots[0].state=PF_READY; g_slots[0].plan=0; g_prefetchEntries[0]=0;
    g_lateUsed=0; prefetchLoadReady(clockNanos);
    before=clockNanos; prefetchEnd(); assert(clockNanos==before && cancelCount==1);
    /* Warming the common assets may use spare time, never the late allowance. */
    g_prefetchBattle=OF_PREFETCH_WARMUP; g_prefetchCount=1; g_prefetchNext=0; g_readEntry=1;
    g_slots[0].state=PF_READY; g_slots[0].plan=0; g_lateUsed=0;
    before=clockNanos; prefetchLoadReady(before); assert(clockNanos==before);
    prefetchLoadReady(before+5000000); assert(clockNanos>before && clockNanos<=before+5000000);
    prefetchEnd();
    /* Failed async spans return their allocation to the read-ahead budget. */
    g_readEntry=0; g_readSpan=0; g_readSpanCount=1; g_readDone=0;
    g_readSpans[0][1]=8192; g_readBuf=malloc(8192); g_aheadBytes=8192;
    g_slots[0].state=PF_READING; g_slots[0].bytes=8192;
    prefetchPieceDone(false); assert(g_aheadBytes==0 && !g_readBuf);
    /* Switching common->battle must settle using the OLD sound entry, not
     * the candidate list's texture at that index. The stage stub rejects a NULL buffer. */
    g_prefetchCount=1; g_prefetchNext=0; g_readEntry=0; g_prefetchArmed=false;
    g_prefetchBattle=OF_PREFETCH_WARMUP; g_prefetchEntries[0]=OF_PREFETCH_SOUND(1);
    g_slots[0].state=PF_READING; g_slots[0].plan=0; g_slots[0].sound=malloc(16); g_slots[0].bytes=16;
    g_aheadBytes=16; g_reading=true; g_readPiece=16; g_readDone=0; g_readSpan=0;
    g_readSpanCount=1; g_readSpans[0][1]=16; g_readBuf=NULL; pollResult=OF_BG_DONE;
    nextBattle=24; prefetchCheck();
    assert(g_prefetchBattle==24 && g_prefetchEntries[0]==7 && !g_slots[0].sound);
    prefetchEnd();
    /* Uncapped overworld warmup has no work: only actual encounters use prefetchAll. */
    g_prefetchArmed=true; g_uncapped=true; nextBattle=OF_PREFETCH_WARMUP;
    prefetchCheck(); assert(g_prefetchCount==0);
    puts("bounded prefetch scheduling: passed"); return 0;
}
