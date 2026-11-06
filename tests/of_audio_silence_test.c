/* Compare the actual dispatch against its retained scalar fallback. */
#define OF_PC 1
#include <stdlib.h>
#include <stdbool.h>
static size_t failMallocSize;
static void *testMalloc(size_t size) {
    if (failMallocSize && size == failMallocSize) return NULL;
    return malloc(size);
}
#define malloc testMalloc
#include "../src/openfpga/platform/of_audio_system.c"
#undef malloc
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>

void logWarn(const char *fmt, ...) { (void)fmt; }
void logError(const char *fmt, ...) { (void)fmt; abort(); }
void logInfo(const char *fmt, ...) { (void)fmt; }
uint64_t nowNanos(void) { return 0; }
bool ofPerfDetail = false;
static uint64_t loads[OF_LOAD_KINDS];
void ofPerfAddLoad(OfLoadKind kind, uint64_t value) { loads[kind] += value; }
void ofPerfPhase(OfPhase phase) { (void)phase; }
void ofLoadingTick(void) {}
bool ofSavestateDeferFileReads(void) { return false; }
static int16_t submitted[512];
static int submittedPairs;
void of_audio_init(void) { submittedPairs = 0; }
void ofPcAudioSnapshotReset(void) { submittedPairs = 0; }
int of_audio_free(void) { return 0; }
int of_audio_write(const int16_t *samples, int pairs) {
    assert(pairs <= 256); memcpy(submitted, samples, (size_t)pairs * 4);
    submittedPairs = pairs; return pairs;
}
static uint64_t comparisons, bulkPairs;
#define SOUND_BYTES 4096u
static void makePack(void) {
    FILE *f = fopen("music.bin", "wb"); assert(f);
    uint32_t count = 2;
    OfMusicTrack tracks[2] = {{.name="quiet",.sampleCount=8192,.sampleRate=32000},
                              {.name="other",.sampleCount=8192,.sampleRate=32000}};
    tracks[0].offset = 8 + sizeof(tracks);
    tracks[1].offset = tracks[0].offset + SOUND_BYTES;
    assert(fwrite("UTM1",1,4,f)==4 && fwrite(&count,4,1,f)==1);
    assert(fwrite(tracks,sizeof(tracks),1,f)==1);
    uint8_t data[SOUND_BYTES];
    for (unsigned i=0;i<SOUND_BYTES;++i) data[i]=(i&1)?0x88:0x80;
    data[16]=0x71; /* suffix starts at byte17, including pending-nibble edge */
    assert(fwrite(data,1,sizeof(data),f)==sizeof(data));
    memset(data,0,sizeof(data));assert(fwrite(data,1,sizeof(data),f)==sizeof(data));
    assert(!fclose(f));
}
static OfAudioSystem *systemForPack(void) {
    OfAudioSystem *ut = calloc(1,sizeof(*ut)); assert(ut);
    ut->base.vtable=&ofVtable;ut->masterGain=1;ut->nextInstanceId=100020;
    openPack(ut); assert(ut->file && ut->trackCount>0);
    return ut;
}
static OfVoice tailVoice(OfAudioSystem *ut, unsigned track) {
    OfVoice v={0};v.active=true;v.streamSlot=-1;v.track=(int32_t)track;
    v.sourceIndex=13;v.instanceId=100001;v.pitch=1;v.data=cachedSound(ut,(int32_t)track);assert(v.data);
    v.bufferLen=trackBytes(&ut->tracks[track]);v.bufferPos=128;v.samplesDecoded=256;
    v.frac=65536;v.step=43690;v.gainNow=v.gainTarget=OF_GAIN_ONE<<8;
    return v;
}
static unsigned compareChunk(OfAudioSystem *ut, OfVoice *a, OfVoice *b, int pairs) {
    int32_t expected[256],actual[256];
    for (int i=0;i<256;++i) expected[i]=actual[i]=17*i-2000;
    mixVoiceScalar(ut,a,expected,pairs);
    unsigned bulk=mixVoice(ut,b,actual,pairs);
    assert(!memcmp(expected,actual,sizeof(expected)));
    assert(!memcmp(a,b,sizeof(*a)));
    ++comparisons;bulkPairs+=bulk;
    return bulk;
}
static void testDispatchAndGuards(void) {
    makePack();OfAudioSystem *ut=systemForPack();OfVoice seed=tailVoice(ut,0),a=seed,b=seed;
    assert(ut->cachedSilentFrom && ut->cachedSilentFrom[0]==17);
    assert(compareChunk(ut,&a,&b,128)==128); /* must remove scalar work */
    for (int guard=0;guard<15;++guard) {
        a=b=seed;uint32_t from=ut->cachedSilentFrom[0];uint32_t *metadata=ut->cachedSilentFrom;
        switch(guard) {
            case 0: ut->cachedSilentFrom=NULL;break;
            case 1: ut->cachedSilentFrom[0]=UINT32_MAX;break;
            case 2: --a.bufferLen;--b.bufferLen;break;
            case 3: a.loop=b.loop=true;break;
            case 4: a.streamSlot=b.streamSlot=0;break;
            case 5: a.sample0=b.sample0=1;break;
            case 6: a.sample1=b.sample1=-1;break;
            case 7: a.adpcm.predictor=b.adpcm.predictor=1;break;
            case 8: a.adpcm.stepIndex=b.adpcm.stepIndex=1;break;
            case 9: a.frac=b.frac=9u*65536;break;
            case 10: a.bufferPos=b.bufferPos=16;a.samplesDecoded=b.samplesDecoded=32;break;
            case 11: a.bufferPos=b.bufferPos=17;a.samplesDecoded=b.samplesDecoded=33;
                a.havePendingCode=b.havePendingCode=true;a.pendingCode=b.pendingCode=7;break;
            case 12: a.samplesDecoded=b.samplesDecoded=8191;a.bufferPos=b.bufferPos=4096;
                a.havePendingCode=b.havePendingCode=true;a.pendingCode=b.pendingCode=8;break;
            case 13: a.data=b.data=cachedSound(ut,1);break;
            case 14: a.step=b.step=0;break;
        }
        assert(compareChunk(ut,&a,&b,128)==0);
        ut->cachedSilentFrom=metadata;ut->cachedSilentFrom[0]=from;
    }
    ut->voices[0]=seed;memset(loads,0,sizeof(loads));mixAndWrite(ut,128);
    assert(loads[OF_LOAD_MIX_SILENT_PAIRS]==128 && loads[OF_LOAD_MIX_VOICE_PAIRS]==128);
    assert(ofIsPlaying(&ut->base,seed.instanceId));
    assert(submittedPairs==128);for (int i=0;i<256;++i) assert(submitted[i]==0);
    ofDestroy(&ut->base);
}
static void testPhaseGainAndBoundaries(void) {
    makePack();OfAudioSystem *ut=systemForPack();OfVoice seed=tailVoice(ut,0);
    const uint32_t steps[]={1,32768,43690,65536,65537,131072,524288};
    const uint32_t starts[]={0,31,32,33,34,35,36,37,8100,8190,8191,8192};
    for (unsigned st=0;st<sizeof(steps)/sizeof(*steps);++st)
    for (unsigned n=0;n<sizeof(starts)/sizeof(*starts);++n)
    for (int gain=0;gain<4;++gain) {
        OfVoice a=seed;a.step=steps[st];a.samplesDecoded=starts[n];
        a.bufferPos=(starts[n]+1)/2;a.havePendingCode=starts[n]&1;
        if(a.bufferPos)a.pendingCode=a.data[a.bufferPos-1]>>4;
        a.frac=n%3==0?0:(n%3==1?65535:steps[st]+65535);
        a.gainNow=123456;a.gainTarget=(gain&1)?4000000:0;a.gainStepPerPair=(gain&2)?0:17;
        OfVoice b=a;
        for(int c=0;c<7 && a.active;++c)compareChunk(ut,&a,&b,c==0?1:(c==1?3:256));
    }
    ofDestroy(&ut->base);
}
static void testCacheLifecycleAndAllocationFailure(void) {
    makePack();failMallocSize=2*sizeof(uint32_t);OfAudioSystem *ut=systemForPack();failMallocSize=0;
    assert(!ut->cachedSilentFrom);OfVoice a=tailVoice(ut,0),b=a;
    assert(compareChunk(ut,&a,&b,128)==0);
    ut->voices[0]=a;uint8_t *snapshot=NULL;size_t snapshotSize=0;const char *reason=NULL;
    assert(ofAudioSnapshot(&ut->base,&snapshot,&snapshotSize,&reason));
    void *prepared=ofAudioPrepareSnapshot(&ut->base,snapshot,snapshotSize,&reason);assert(prepared);
    ofAudioApplySnapshot(&ut->base,prepared);ofAudioFreeSnapshot(prepared);free(snapshot);
    a=ut->voices[0];b=a;assert(compareChunk(ut,&a,&b,128)==0);ofDestroy(&ut->base);
    makePack();ut=systemForPack();a=tailVoice(ut,0);assert(cachedSound(ut,1));
    ut->voices[0]=a;evictSounds(ut,OF_SOUND_CACHE_BYTES);
    assert(ut->cachedSounds[0] && !ut->cachedSounds[1] && ut->cachedSilentFrom[1]==UINT32_MAX);
    ut->voices[0].active=false;evictSounds(ut,OF_SOUND_CACHE_BYTES);
    assert(!ut->cachedSounds[0] && ut->cachedSilentFrom[0]==UINT32_MAX && ut->cachedBytes==0);
    /* Reload different synthetic bytes to catch stale metadata reuse. */
    FILE *f=fopen("music.bin","r+b");assert(f);assert(!fseek(f,ut->tracks[0].offset,SEEK_SET));
    uint8_t nonquiet[SOUND_BYTES];memset(nonquiet,0x71,sizeof(nonquiet));
    assert(fwrite(nonquiet,1,sizeof(nonquiet),f)==sizeof(nonquiet));assert(!fclose(f));
    a=tailVoice(ut,0);b=a;assert(ut->cachedSilentFrom[0]==SOUND_BYTES);
    assert(compareChunk(ut,&a,&b,128)==0);
    evictSounds(ut,OF_SOUND_CACHE_BYTES);assert(!ut->cachedSounds[0]);
    f=fopen("music.bin","r+b");assert(f);assert(!ftruncate(fileno(f),ut->tracks[0].offset+10));fclose(f);
    assert(!cachedSound(ut,0) && ut->cachedSilentFrom[0]==UINT32_MAX && ut->cachedBytes==0);
    ofDestroy(&ut->base);
}
static void testMemoryReplacementAndFailure(void) {
    makePack();OfAudioSystem *ut=systemForPack();OfVoice original=tailVoice(ut,0);
    original.gainNow=90000;original.gainTarget=10000;original.gainStepPerPair=13;
    ut->voices[0]=original;assert(cachedSound(ut,1));ut->cachedSilentFrom[1]=UINT32_MAX;
    uint8_t *bytes=NULL,*other=NULL;size_t size=0,otherSize=0;const char *reason=NULL;
    assert(ofAudioSnapshot(&ut->base,&bytes,&size,&reason));
    uint32_t *metadata=ut->cachedSilentFrom;ut->cachedSilentFrom=NULL;
    assert(ofAudioSnapshot(&ut->base,&other,&otherSize,&reason));
    assert(size==otherSize && !memcmp(bytes,other,size));free(other);ut->cachedSilentFrom=metadata;
    uint8_t *oldData=ut->cachedSounds[0];uint32_t oldBytes=ut->cachedBytes;
    void *prepared=ofAudioPrepareSnapshot(&ut->base,bytes,size,&reason);assert(prepared);
    assert(ut->cachedSounds[0]==oldData && ut->cachedBytes==oldBytes && ut->cachedSilentFrom[0]==17);
    assert(!memcmp(&ut->voices[0],&original,sizeof(original)));
    ut->cachedSilentFrom[0]=UINT32_MAX; /* restored allocation must regain proof */
    ofAudioApplySnapshot(&ut->base,prepared);ofAudioFreeSnapshot(prepared);
    assert(ut->cachedSounds[0]!=oldData && ut->voices[0].data==ut->cachedSounds[0]);
    assert(ut->cachedBytes==oldBytes && ut->cachedSilentFrom[0]==17 && ut->cachedSilentFrom[1]==UINT32_MAX);
    OfVoice scalar=ut->voices[0],candidate=scalar;
    assert(compareChunk(ut,&scalar,&candidate,128)==128);
    /* Failure after sound preparation, and allocation failure, are atomic. */
    oldData=ut->cachedSounds[0];OfVoice live=ut->voices[0];uint32_t oldMeta=ut->cachedSilentFrom[0];
    assert(!ofAudioPrepareSnapshot(&ut->base,bytes,size-1,&reason));
    failMallocSize=SOUND_BYTES;assert(!ofAudioPrepareSnapshot(&ut->base,bytes,size,&reason));failMallocSize=0;
    assert(ut->cachedSounds[0]==oldData && ut->cachedBytes==oldBytes && ut->cachedSilentFrom[0]==oldMeta);
    assert(!memcmp(&ut->voices[0],&live,sizeof(live)));
    /* Persisted Memory bytes survive repeated restore and scalar continuation. */
    FILE *state=tmpfile();assert(state);assert(fwrite(bytes,1,size,state)==size);rewind(state);
    uint8_t *disk=malloc(size);assert(disk && fread(disk,1,size,state)==size);fclose(state);
    for(int cycle=0;cycle<3;++cycle) {
        prepared=ofAudioPrepareSnapshot(&ut->base,disk,size,&reason);assert(prepared);
        ofAudioApplySnapshot(&ut->base,prepared);ofAudioFreeSnapshot(prepared);
        scalar=ut->voices[0];candidate=scalar;
        for(int chunk=0;chunk<20 && scalar.active;++chunk) compareChunk(ut,&scalar,&candidate,127);
    }
    free(disk);free(bytes);ofDestroy(&ut->base);
}
static void testPauseSeekAndGroupGain(void) {
    makePack();OfAudioSystem *ut=systemForPack();ut->voices[0]=tailVoice(ut,0);
    OfVoice before=ut->voices[0];ofPauseSound(&ut->base,before.instanceId);
    mixAndWrite(ut,128);before.paused=true;assert(!memcmp(&before,&ut->voices[0],sizeof(before)));
    ofResumeSound(&ut->base,before.instanceId);ofPauseAll(&ut->base);before.paused=false;
    mixAndWrite(ut,128);assert(!memcmp(&before,&ut->voices[0],sizeof(before)));ofResumeAll(&ut->base);
    const float positions[]={0,0.001f,0.05f,0.5f,1.7f,5};
    DataWin dw={0};Sound sounds[14]={{0}};dw.agrp.count=2;dw.sond.count=14;dw.sond.sounds=sounds;
    sounds[13].audioGroup=1;ut->base.dw=&dw;AudioSystem_setGroupGain(&ut->base,1,0.25f,0);
    for(unsigned i=0;i<sizeof(positions)/sizeof(*positions);++i) {
        ofSetTrackPosition(&ut->base,before.instanceId,positions[i]);
        OfVoice a=ut->voices[0],b=a;for(int c=0;c<5 && a.active;++c) compareChunk(ut,&a,&b,128);
    }
    ofDestroy(&ut->base);
}
static void testRealTrack(OfAudioSystem *ut, const char *name) {
    int32_t track=findTrack(ut,name);assert(track>=0);
    OfVoice seed=tailVoice(ut,(unsigned)track);
    seed.bufferPos=seed.samplesDecoded=0;seed.frac=65536;
    const uint32_t steps[]={8192,32768,43690,65536,98304,524288};
    const int chunks[]={1,3,31,127,256};
    uint64_t bulkBefore=bulkPairs,checksBefore=comparisons;
    for(unsigned st=0;st<sizeof(steps)/sizeof(*steps);++st)for(int mode=0;mode<4;++mode) {
        seed.data=ut->cachedSounds[track]; /* a prior case may have restored the cache */
        OfVoice a=seed;a.step=steps[st];
        if(mode==1)a.loop=true;
        if(mode==2)a.streamSlot=0;
        if(mode==3){a.gainNow=4000000;a.gainTarget=0;a.gainStepPerPair=19;}
        OfVoice b=a;uint32_t done=0,maximum=ut->tracks[track].sampleCount*8+1024;
        for(unsigned c=0;a.active && done<maximum;++c) {
            int pairs=chunks[c%5];compareChunk(ut,&a,&b,pairs);done+=(uint32_t)pairs;
            if(c==23){a.gainTarget=b.gainTarget=200000;a.gainStepPerPair=b.gainStepPerPair=11;}
            if(c==50){
                a.gainTarget=b.gainTarget=4000000;a.gainStepPerPair=b.gainStepPerPair=3000;
                if(st==2 && mode==0) {
                    ut->voices[0]=b;uint8_t *bytes=NULL;size_t size=0;const char *reason=NULL;
                    assert(ofAudioSnapshot(&ut->base,&bytes,&size,&reason));
                    void *prepared=ofAudioPrepareSnapshot(&ut->base,bytes,size,&reason);assert(prepared);
                    ofAudioApplySnapshot(&ut->base,prepared);ofAudioFreeSnapshot(prepared);free(bytes);
                    a.data=ut->cachedSounds[track];b=ut->voices[0];
                }
            }
        }
    }
    ut->voices[0].active=false;
    printf("real track %s: %llu state/PCM comparisons, %llu bulk pairs\n",name,
        (unsigned long long)(comparisons-checksBefore),(unsigned long long)(bulkPairs-bulkBefore));
}
int main(int argc, char **argv) {
    char directory[]="/tmp/of-audio-silence.XXXXXX";assert(mkdtemp(directory));
    char previous[4096];assert(getcwd(previous,sizeof(previous)));assert(!chdir(directory));
    adpcmDiffInit();testDispatchAndGuards();testPhaseGainAndBoundaries();
    testCacheLifecycleAndAllocationFailure();testMemoryReplacementAndFailure();testPauseSeekAndGroupGain();
    assert(!unlink("music.bin"));
    if(argc>1) {
        assert(!symlink(argv[1],"music.bin"));OfAudioSystem *ut=systemForPack();
        for(int i=2;i<argc;++i)testRealTrack(ut,argv[i]);
        ofDestroy(&ut->base);assert(!unlink("music.bin"));
    }
    assert(!chdir(previous));assert(!rmdir(directory));
    printf("PASS silent audio: %llu scalar/state comparisons, %llu bulk pairs; cache/OOM/Memory/seek/pause/gain\n",
        (unsigned long long)comparisons,(unsigned long long)bulkPairs);
    return 0;
}
