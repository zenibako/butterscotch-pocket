/* Coalesced tile copies must preserve clipped, transparent and layered pixels.
 * Count real memcpy calls to catch a return to copying every 16-pixel block. */
#include "sw_renderer_private.h"
#include <assert.h>
#include <stdio.h>
static unsigned copies;
static void *countedMemcpy(void *to, const void *from, size_t n) {
    ++copies; return memcpy(to,from,n);
}
#define memcpy countedMemcpy
#include "../../sw/sw_renderer.c"
#undef memcpy

enum { W=96, H=64, PITCH=87, HEIGHT=58 };
static unsigned checks, workFailures;
static void check(int pattern, int rx, int ry, int left, int top, bool clear, bool underOnly) {
    SWTileRun run = {.x=rx,.y=ry};
    int rw=pattern==4 ? 93 : W, rh=pattern==4 ? 61 : H;
    assert(swrRunAllocate(&run,rw,rh,false));
    for (int y=0;y<rh;++y) for (int x=0;x<rw;++x) {
        bool opaque = pattern==0 || pattern==4 || (pattern==1 && (x/16)%3!=1) || (pattern==2 && (x+y)%5!=0);
        run.pixels[y*rw+x] = opaque ? (uintpixel_t)(0x8000 | ((x*13+y*73)&0x7fff)) : 0;
    }
    swrRunClassify(&run);
    uintpixel_t actual[PITCH*HEIGHT], expected[PITCH*HEIGHT], under=0x9abc;
    for (int i=0;i<PITCH*HEIGHT;++i) actual[i]=expected[i]=0xbdef;
    const int width=70, height=49;
    /* Expected pixel composition reads the original picture directly. */
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        int sx=left+x-rx, sy=top+y-ry;
        bool inside=sx>=0 && sx<rw && sy>=0 && sy<rh;
        uintpixel_t pixel=inside ? run.pixels[sy*rw+sx] : 0;
        if (clear) expected[y*PITCH+x]=under;
        if (swrIsOpaque(pixel)) expected[y*PITCH+x]=pixel;
    }
    SWTileRun *runs[]={&run}; copies=0;
    swrRunsDrawOnto(runs,1,actual,PITCH,left,top,width,height,clear ? &under : NULL,underOnly);
    if (pattern==0 && rx==0 && ry==0 && left==2 && top==3 && !underOnly && copies>(unsigned)height)
        ++workFailures;
    if (underOnly) {
        /* The preserved multi-picture route is a second oracle for conservative
         * block coverage in the clear-only pass, including transparent edges. */
        for (int i=0;i<PITCH*HEIGHT;++i) expected[i]=0xbdef;
        SWTileRun empty={0}; SWTileRun *two[]={&run,&empty};
        swrRunsDrawOnto(two,2,expected,PITCH,left,top,width,height,clear ? &under : NULL,true);
    }
    for (int i=0;i<PITCH*HEIGHT;++i) {
        if (actual[i]!=expected[i]) {
            fprintf(stderr,"FAIL pattern%d placement%d,%d view%d,%d clear%d under%d pixel%d: %x != %x\n",
                pattern,rx,ry,left,top,clear,underOnly,i,actual[i],expected[i]); exit(1);
        }
    }
    free(run.pixels); ++checks;
}
int main(void) {
    for (int p=0;p<5;++p) for (int phase=0;phase<16;++phase) for (int clear=0;clear<2;++clear) {
        check(p,phase-8,7-phase,2,3,clear,false);
        check(p,-5,3,-17+phase,-9+phase,clear,false);
        check(p,phase-8,7-phase,2,3,clear,true);
        check(p,-5,3,-17+phase,-9+phase,clear,true);
    }
    check(0,0,0,2,3,false,false); check(0,0,0,2,3,true,false);
    check(0,0,0,2,3,true,true);
    if (workFailures) { fprintf(stderr,"FAIL: %u opaque fixtures still copy per block\n",workFailures); return 1; }
    printf("PASS: %u tile compositions preserve pixels with one copy per solid row\n",checks); return 0;
}
