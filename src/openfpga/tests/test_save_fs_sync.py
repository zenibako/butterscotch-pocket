"""Normal native saves must reach SD before close, without exit writeback.

Compile the production archive writer with real buffered stdio. Only the
Pocket durability boundary and error returns are mocked; SD is a separate
byte array populated by fsync from the already-flushed file descriptor.
"""
from pathlib import Path
import os
import resource
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo / "src/openfpga/platform/of_save_fs.c").read_text()
start = source.index("static bool storeArchive(")
end = source.index("{", start) + 1
depth = 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
types = source[source.index("typedef struct {"):source.index("/* When set")]
pre = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "of_save_format.h"
#define OF_NATIVE_SAVESTATES 1
#define OF_SAVE_SLOT_FILE "deltarune_0.sav"
typedef struct { int marker; } FileSystem;
static bool g_volatile;
static int opens, closes, flushes, syncs, fail_open, fail_write, fail_flush, fail_sync, fail_close;
static unsigned char sd[262144];
static size_t sd_size;
static void logInfo(const char *fmt, ...) { (void)fmt; }
static void ofBgReadSettle(void) {}
static void logWarn(const char *fmt, ...) { (void)fmt; }
static void *safeCalloc(size_t n, size_t size) { void *p=calloc(n,size); assert(p); return p; }
static FILE *test_fopen(const char *name, const char *mode) {
    assert(!strcmp(name,OF_SAVE_SLOT_FILE) && !strcmp(mode,"wb"));
    opens++; if(fail_open) return NULL;
    FILE *f=tmpfile(); assert(f);
    /* Keep this small archive buffered until the production fflush. */
    assert(setvbuf(f,NULL,_IOFBF,8192)==0); return f;
}
static size_t test_fwrite(const void *p, size_t size, size_t count, FILE *f) {
    return fwrite(p,size,fail_write ? count/2 : count,f);
}
static int test_fflush(FILE *f) {
    flushes++; if(fail_flush) { errno=EIO; return EOF; }
    return fflush(f);
}
static int test_fsync(int fd) {
    syncs++;
    assert(flushes==1 && "stdio must be flushed before requesting durability");
    struct stat info; assert(fstat(fd,&info)==0);
    assert(info.st_size>0 && (size_t)info.st_size<=sizeof(sd));
    if(fail_sync) { errno=EIO; return -1; }
    sd_size=(size_t)info.st_size;
    assert(pread(fd,sd,sd_size,0)==(ssize_t)sd_size);
    return 0;
}
static int test_fclose(FILE *f) {
    closes++; int result=fclose(f); assert(!result); return fail_close ? EOF : 0;
}
#define fopen test_fopen
#define fwrite test_fwrite
#define fflush test_fflush
#define fsync test_fsync
#define fclose test_fclose
'''
post = r'''
static void reset(void) {
    opens=closes=flushes=syncs=fail_open=fail_write=fail_flush=fail_sync=fail_close=0;
    memset(sd,0x11,sizeof(sd)); sd_size=0; g_volatile=false;
}
int main(void) {
    OfSaveFs fs={0}; fs.fileCount=1;
    strcpy(fs.files[0].name,"filech1_1");
    fs.files[0].data=(uint8_t *)"new progress"; fs.files[0].size=12;
    reset(); assert(storeArchive(&fs));
    assert(syncs==1 && closes==1 && "save must be durable before exit");
    OfSaveHeader header; memcpy(&header,sd,sizeof(header));
    assert(header.magic==OF_SAVE_MAGIC && header.fileCount==1 && header.totalBytes==sd_size);
    OfSaveEntry entry; memcpy(&entry,sd+sizeof(header),sizeof(entry));
    assert(!strcmp(entry.name,"filech1_1") && entry.size==12);
    assert(entry.offset+entry.size==sd_size && !memcmp(sd+entry.offset,"new progress",12));
    reset(); fail_sync=1; assert(!storeArchive(&fs));
    assert(syncs==1 && closes==1 && !sd_size);
    reset(); fail_flush=1; assert(!storeArchive(&fs));
    assert(flushes==1 && !syncs && closes==1 && !sd_size);
    reset(); fail_write=1; assert(!storeArchive(&fs));
    assert(!syncs && closes==1 && !sd_size);
    reset(); fail_close=1; assert(!storeArchive(&fs)); assert(syncs==1 && closes==1);
    reset(); fail_open=1; assert(!storeArchive(&fs)); assert(opens==1 && !closes && !syncs);
    reset(); g_volatile=true; assert(storeArchive(&fs)); assert(!opens && !syncs);
    puts("PASS: normal archive reaches separate SD bytes before exit; errors close and propagate; volatile skips I/O");
}
'''
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix="save-sync-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(pre + types + source[start:end] + post)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-function",
                    "-fsanitize=address,undefined", "-I" + str(repo / "src/openfpga/platform"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True,
                   env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0:halt_on_error=1"})
