/* File-backed lazy ROOM loads must settle asynchronous I/O first, including
 * room_get_info calls. Loaded and memory-backed rooms need no file handoff. */
#include "data_win.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
static bool backgroundBusy;
static unsigned reads, settles;
void platformBeforeRoomFileRead(void) { backgroundBusy=false; ++settles; }
void platformLog(logType type,const char *fmt,va_list ap) { (void)type; vfprintf(stderr,fmt,ap); }
static size_t guardedRead(void *p,size_t size,size_t count,FILE *f) {
    assert(!backgroundBusy); ++reads; return fread(p,size,count,f);
}
#define fread guardedRead
#include "../src/binary_reader.c"
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
    DataWin_freeRoomPayload(&room); fclose(dw.lazyLoadFile);
    puts("lazy ROOM file I/O handoff: passed"); return 0;
}
