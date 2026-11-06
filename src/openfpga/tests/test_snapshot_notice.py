"""Exercise immediate failure-notice presentation with real buffer bounds."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'platform/of_platform.c').read_text()
start = source.index('void ofPlatformSnapshotNotice(')
body = source.index('{', start)
end, depth = body + 1, 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[start:end]
pre = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
static uint16_t *g_nextFb;
static int g_nextW,g_nextH,g_modeW,g_modeH,g_modeStride;
static bool g_snapshotNotice,g_showingFramebuffer;
static uint64_t g_noticeUntil;
static uint8_t *surface;
static int requests,flips,draws,notices;
static void showNotice(const char *text) { assert(!strcmp(text,"Refused")); notices++; }
static void drawSnapshotNotice(uint16_t *fb,int w,int h) {
 assert(fb && w>0 && h>0); draws++; fb[0]=0x1234;
}
static uint8_t *of_video_surface(void) { requests++; return surface; }
static void of_video_flip(void) { flips++; }
static void swrFramebufferTouched(void) {}
static void clear(void) { requests=flips=draws=notices=0; g_snapshotNotice=false; g_noticeUntil=0; }
static void pending(void) { assert(notices==1 && g_snapshotNotice && g_noticeUntil==UINT64_MAX); }
'''
post = r'''
int main(void) {
 g_nextW=16;g_nextH=8;g_modeW=8;g_modeH=4;g_modeStride=20;g_showingFramebuffer=true;
 g_nextFb=calloc(16*8,2);surface=malloc(20*4);memset(surface,0xa5,20*4);
 clear();ofPlatformSnapshotNotice("Refused");pending();
 assert(!requests && !flips && !draws); /* mismatched 640/320-style mode */
 for(int i=0;i<80;i++)assert(surface[i]==0xa5);
 free(g_nextFb);free(surface);
 g_nextW=g_modeW=8;g_nextH=g_modeH=4;g_modeStride=20;
 g_nextFb=calloc(8*4,2);surface=malloc(20*4);memset(surface,0xa5,20*4);
 g_showingFramebuffer=false;clear();ofPlatformSnapshotNotice("Refused");pending();
 assert(!requests && !flips && !draws);
 g_showingFramebuffer=true;clear();ofPlatformSnapshotNotice("Refused");pending();
 assert(requests==1 && flips==1 && draws==1);
 for(int row=0;row<4;row++) {
  assert(!memcmp(surface+row*20,g_nextFb+row*8,16));
  for(int i=16;i<20;i++)assert(surface[row*20+i]==0xa5);
 }
 free(g_nextFb);free(surface);
 g_modeStride=16;surface=calloc(8*4,2);g_nextFb=(uint16_t *)surface;
 clear();ofPlatformSnapshotNotice("Refused");pending();
 assert(requests==1 && flips==1 && draws==1 && g_nextFb[0]==0x1234);
 free(surface);g_nextFb=NULL;clear();ofPlatformSnapshotNotice("Refused");pending();
 assert(!requests && !flips && !draws);
 puts("PASS: notices defer unsafe frames and preserve valid padded/in-place presentation");
}
'''
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'test.c').write_text(pre + function + post)
    subprocess.run(['clang','-std=c11','-fsanitize=address,undefined',
        str(root / 'test.c'),'-o',str(root / 'test')],check=True)
    subprocess.run([str(root / 'test')],check=True)
