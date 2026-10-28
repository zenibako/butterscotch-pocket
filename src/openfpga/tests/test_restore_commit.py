from pathlib import Path
import subprocess,tempfile,re
source=(Path(__file__).resolve().parents[3] / 'src/openfpga/platform/of_state_requests.c').read_text()
m=re.search(r'static bool restoreState\(',source)
assert m, 'missing restore commit callback'
a=source.index('{',m.start());p=a+1;depth=1
while depth:
 depth+=(source[p]=='{')-(source[p]=='}');p+=1
func=source[m.start():p]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
typedef struct { int state; void *audioSystem,*fileSystem; } Runner;
typedef struct { const uint8_t *data; size_t size; } OfSnapshotSection;
typedef struct { const uint8_t *data; size_t size,offset; bool ok; } OfSnapshotReader;
typedef struct { int unused; } RunnerSnapshot;
static Runner live;
static RunnerSnapshot core;
static int audio, files, failure, callbacks, frees, locked;
static uint64_t nowNanos(void) { static uint64_t n;return n+=1000000; }
static void logInfo(const char *s,...) { (void)s; }
static bool ofSnapshotUnpack(uint64_t id,const uint8_t *d,size_t n,OfSnapshotSection *p) {
 (void)id;(void)d;(void)n;static uint8_t b[12];
 for(int i=0;i<4;i++){p[i].data=b;p[i].size=12;}return failure!=5;
}
static bool ofSnapshotFromOtherGame(uint64_t id,const uint8_t *d,size_t n) { (void)id;(void)d;(void)n;return false; }
static uint64_t g_gameId;
static uint32_t ofSnapGetU32(OfSnapshotReader *r) { r->offset+=4;return 0; }
static void ofAudioSnapshotLock(bool b) {locked=b;}
static RunnerSnapshot *Runner_snapshotPrepare(Runner *r,uint64_t id,const uint8_t*d,size_t n,const char **s) {
 (void)r;(void)id;(void)d;(void)n;if(failure==1){*s="core";return NULL;}return &core;
}
static void *ofAudioPrepareSnapshot(void *r,const uint8_t*d,size_t n,const char **s) {
 (void)r;(void)d;(void)n;if(failure==2){*s="audio";return NULL;}return &audio;
}
static void *ofSaveFsPrepareSnapshot(void *r,const uint8_t*d,size_t n,const char **s) {
 (void)r;(void)d;(void)n;if(failure==3){*s="files";return NULL;}return &files;
}
static bool ofSaveFsApplySnapshot(void *r,void*p) {(void)r;(void)p;if(failure==4)return false;live.state|=1;return true;}
static void Runner_snapshotApply(Runner*r,RunnerSnapshot*p){(void)p;r->state|=2;}
static void ofAudioApplySnapshot(void*r,void*p){(void)r;(void)p;live.state|=4;}
static void ofPlatformSnapshotResumed(Runner*r,const uint32_t*v){(void)v;r->state|=8;}
static void Runner_snapshotFree(RunnerSnapshot *p){if(p)frees++;}
static void ofAudioFreeSnapshot(void*p){if(p)frees++;}
static void ofSaveFsFreeSnapshot(void*p){if(p)frees++;}
static void committed(void){assert(live.state==15);assert(frees==0);assert(locked);callbacks++;}
'''
post=r'''
int main(void){uint8_t payload[4]={'U','T','S','1'};
 for(failure=0;failure<6;failure++){
  const char *reason=NULL;live.state=callbacks=frees=locked=0;
  bool ok=restoreState(&live,payload,4,&reason,committed);
  assert(ok==(failure==0));assert(callbacks==(failure==0));assert(!locked);
  if(failure)assert(reason);
 }
 failure=0;live.state=callbacks=frees=locked=0;const char*reason=NULL;
 assert(restoreState(&live,payload,4,&reason,NULL));assert(callbacks==0&&live.state==15&&frees==3);
 puts("PASS: completion follows committed state, precedes cleanup, never acknowledges rejection");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(pre+func+post)
 subprocess.run(['clang','-std=c11','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
