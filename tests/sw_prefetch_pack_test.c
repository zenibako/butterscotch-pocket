/* Synthetic tiled pack: incremental loading must yield without changing pixels,
 * and must never fall through to SD reads when staged data is missing. */
#include "../src/sw/sw_texture_pack.c"
#include <assert.h>
#include "../src/sw/sw_texture_lru.c"
#include <stdarg.h>

uint64_t nowNanos(void) { return 0; }
void platformLog(logType type, const char *fmt, va_list ap) { (void) type; vfprintf(stderr, fmt, ap); }

int main(void) {
    PackPage page = { 32, 32, 0 };
    PackTile tiles[4] = { {100,4}, {104,4}, {108,4}, {112,4} };
    uint16_t words[] = {0x80ff,1, 0x80ff,2, 0x80ff,3, 0x80ff,4};
    uintpixel_t pixels[30*30], reference[30*30];
    packPages = &page; packTiles = tiles; packCount = 1; packTileCount = 4;
    packTile = 16; packProbed = true;
    packFile = tmpfile(); assert(packFile);
    assert(fseek(packFile,100,SEEK_SET)==0);
    assert(fwrite(words,1,sizeof(words),packFile)==sizeof(words)); fflush(packFile);
    assert(swrTexturePackDecodeRect(0,1,1,30,30,reference));
    windowStart=windowEnd=0;
    uint16_t *copy=malloc(sizeof(words)); memcpy(copy,words,sizeof(words));
    assert(swrTexturePackStage(100,sizeof(words),copy,0));
    memset(pixels,0xff,sizeof(pixels));
    unsigned next=0;
    for (unsigned i=0;i<4;i++) {
        assert(swrTexturePackDecodeRectStep(0,1,1,30,30,pixels,&next,true)==(i==3 ? 1 : 0));
        assert(next==i+1);
        /* Intervening draws may replace the ordinary decode window. */
        windowStart=windowEnd=0;
    }
    assert(memcmp(reference,pixels,sizeof(pixels))==0);
    SwrTexturePackReadTotals before,after;
    swrTexturePackGetReadTotals(&before);
    assert(swrTexturePackHasStaged(100,4,2));
    assert(swrTexturePackUnstage(0)==0);
    assert(swrTexturePackUnstage(2)==sizeof(words));
    next=0;
    assert(swrTexturePackDecodeRectStep(0,1,1,30,30,pixels,&next,true)==-1);
    swrTexturePackGetReadTotals(&after);
    assert(before.stdioReads==after.stdioReads && before.directReads==after.directReads);
    /* Plans must not depend on the window left by another item. */
    SwrPackSpan spans[16];
    int n=swrTexturePackPlanRect(0,1,1,30,30,spans,16); assert(n>0);
    assert(swrTexturePackPlanRect(0,1,1,30,30,spans,16)==n);
    /* Missing/corrupt staged tiles fail without publishing partial data. */
    copy=malloc(sizeof(words)); memcpy(copy,words,sizeof(words)); copy[0]=0xffff;
    assert(swrTexturePackStage(100,sizeof(words),copy,0)); next=0;
    assert(swrTexturePackDecodeRectStep(0,1,1,30,30,pixels,&next,true)==-1);
    swrTexturePackUnstage(0);
    tiles[0].size=0; next=0; memset(pixels,0xff,sizeof(pixels));
    assert(swrTexturePackDecodeRectStep(0,1,1,30,30,pixels,&next,true)==0);
    assert(pixels[0]==0);
    tiles[0].size=4;
    copy=malloc(sizeof(words)); memcpy(copy,words,sizeof(words));
    assert(swrTexturePackStage(100,sizeof(words),copy,0));
    DataWin dw; SWRenderer swr; TexturePageItem tpag;
    memset(&dw,0,sizeof(dw)); memset(&swr,0,sizeof(swr)); memset(&tpag,0,sizeof(tpag));
    tpag.texturePageId=0; tpag.sourceX=1; tpag.sourceY=1; tpag.sourceWidth=30; tpag.sourceHeight=30;
    dw.tpag.count=1; dw.tpag.items=&tpag;
    SWTexture *pages[1]={NULL};
    swr.base.dataWin=&dw; swr.originalTPagCount=1; swr.textureCount=1; swr.textures=pages;
    void *work=NULL;
    for (unsigned i=0;i<4;i++) {
        assert(swrPrefetchItemStep(&swr,0,&work)==(i==3 ? SWR_PREFETCH_LOADED : 4));
        assert((work==NULL)==(i==3));
        assert((swr.itemTextures && swr.itemTextures[0])==(i==3));
    }
    assert(swr.itemTextures[0]->width==32 && swr.itemTextures[0]->height==32);
    for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x++)
        assert(swr.itemTextures[0]->buffer[y*32+x]==1+x/16+2*(y/16));
    assert(swr.itemBytes==32*32*sizeof(uintpixel_t));
    assert(swrPrefetchItemStep(&swr,0,&work)==SWR_PREFETCH_PRESENT);
    free(swr.itemTextures[0]->buffer); free(swr.itemTextures[0]); swr.itemTextures[0]=NULL; swr.itemBytes=0;
    assert(swrPrefetchItemStep(&swr,0,&work)==4 && work);
    swrPrefetchCancel(&work); assert(!work && !swr.itemTextures[0] && !swr.itemBytes);
    assert(swrPrefetchItemStep(&swr,0,&work)==4 && work);
    assert(swrPrefetchItemStep(&swr,-1,&work)==SWR_PREFETCH_SKIPPED && !work);
    free(swr.itemTextures);
    swrTexturePackUnstage(0);
    fclose(packFile);
    puts("incremental prefetch pack: passed");
    return 0;
}
