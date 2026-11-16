/* Lazy ROOM and texture file reads must settle asynchronous I/O first.
 * Loaded assets and memory-backed rooms need no file handoff. */
#include "data_win.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool backgroundBusy;
static unsigned reads, settles;
void platformBeforeDataFileRead(void) { backgroundBusy=false; ++settles; }
void platformLog(logType type,const char *fmt,va_list ap) { (void)type; vfprintf(stderr,fmt,ap); }
static size_t guardedRead(void *p,size_t size,size_t count,FILE *f) {
    assert(!backgroundBusy); ++reads; return fread(p,size,count,f);
}
#define fread guardedRead
#include "../src/binary_reader.c"
#include "../src/data_win.c"
#undef fread
int main(void) {
    DataWin dw={0}; Room room={0}; uint8_t emptyTables[20]={0};
    dw.gen8.wadVersion=16; dw.room.count=1; dw.room.rooms=&room;
    dw.fileSize=sizeof(emptyTables); dw.lazyLoadFile=tmpfile(); assert(dw.lazyLoadFile);
    assert(fwrite(emptyTables,1,sizeof(emptyTables),dw.lazyLoadFile)==sizeof(emptyTables));
    fflush(dw.lazyLoadFile);
    room.backgroundsFileOffset=4; room.viewsFileOffset=8; room.gameObjectsFileOffset=12; room.tilesFileOffset=16;
    backgroundBusy=true; DataWin_loadRoomPayload(&dw,0);
    assert(room.payloadLoaded && !backgroundBusy && settles==1 && reads>0);
    unsigned before=reads;
    backgroundBusy=true; DataWin_loadRoomPayload(&dw,0);
    assert(backgroundBusy && settles==1 && reads==before);
    DataWin_freeRoomPayload(&room);
    dw.roomChunkBuffer=emptyTables; dw.roomChunkBase=0; dw.roomChunkSize=sizeof(emptyTables);
    DataWin_loadRoomPayload(&dw,0);
    assert(room.payloadLoaded && backgroundBusy && settles==1 && reads==before);
    DataWin_freeRoomPayload(&room);
    const uint8_t textureBytes[]={0x12,0x34,0x56,0x78};
    assert(fseek(dw.lazyLoadFile,4,SEEK_SET)==0);
    assert(fwrite(textureBytes,1,sizeof(textureBytes),dw.lazyLoadFile)==sizeof(textureBytes));
    assert(fflush(dw.lazyLoadFile)==0);
    Texture texture={.blobOffset=4,.blobSize=sizeof(textureBytes)};
    dw.txtr.count=1; dw.txtr.textures=&texture;
    assert(fseek(dw.lazyLoadFile,2,SEEK_SET)==0);
    DataWin_loadTxtrIfNeeded(&dw,0);
    assert(!backgroundBusy && settles==2 && reads==before+1);
    assert(texture.blobData && memcmp(texture.blobData,textureBytes,sizeof(textureBytes))==0);
    assert(ftell(dw.lazyLoadFile)==2);
    before=reads; backgroundBusy=true;
    DataWin_loadTxtrIfNeeded(&dw,0);
    assert(backgroundBusy && settles==2 && reads==before);
    free(texture.blobData); texture.blobData=NULL; texture.blobSize=0;
    DataWin_loadTxtrIfNeeded(&dw,0);
    assert(backgroundBusy && settles==2 && reads==before);
    fclose(dw.lazyLoadFile);
    puts("lazy data file I/O handoff: passed"); return 0;
}
