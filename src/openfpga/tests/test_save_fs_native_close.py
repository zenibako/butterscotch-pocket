"""Exercise native archive apply from production source, including close failure.

Real tmpfile/fwrite/setvbuf perform the I/O; only slot discovery and the
close return value are substituted because Pocket services are not host APIs.
"""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[3]
source = (repo / "src/openfpga/platform/of_save_fs.c").read_text()


def function(name):
    start = source.index("bool " + name + "(")
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


types = source[source.index("typedef struct {"):source.index("/* When set")]
snapshot_start = source.index("typedef struct {", source.index("/* Native states"))
snapshot_type = source[snapshot_start:source.index("} OfSaveSnapshot;", snapshot_start)
                       + len("} OfSaveSnapshot;")]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "of_save_format.h"
#define OF_NATIVE_SAVESTATES 1
#define OF_SAVE_SLOT_FILE "undertale_0.sav"
/* OF_PC intentionally undefined: compile the native lookup/r+b branch. */
typedef struct { int marker; } FileSystem;
static bool g_volatile;
static int fail_close, closes, lookups;
static size_t written_bytes;
static uint64_t nowNanos(void) { static uint64_t n; return ++n; }
static void logInfo(const char *fmt, ...) { (void)fmt; }
static void ofBgReadSettle(void) {}
static int of_file_slot_find(const char *name, uint32_t *slot) {
    assert(strcmp(name, OF_SAVE_SLOT_FILE) == 0);
    lookups++; *slot = 10; return 0;
}
static FILE *test_fopen(const char *name, const char *mode) {
    assert(strcmp(name, OF_SAVE_SLOT_FILE) == 0);
    assert(strcmp(mode, "r+b") == 0);
    return tmpfile();
}
static size_t test_fwrite(const void *data, size_t size, size_t count, FILE *f) {
    size_t result = fwrite(data, size, count, f);
    written_bytes += result * size;
    return result;
}
static int test_fclose(FILE *f) {
    closes++;
    int result = fclose(f);
    assert(result == 0);
    return fail_close ? EOF : result;
}
#define fopen test_fopen
#define fwrite test_fwrite
#define fclose test_fclose
'''
post = r'''
int main(void) {
    OfSaveFs live = {0};
    OfSaveSnapshot prepared = {0};
    live.fileCount = prepared.files.fileCount = 1;
    strcpy(live.files[0].name, "file0");
    live.files[0].data = malloc(4);
    live.files[0].size = 3;
    memcpy(live.files[0].data, "old", 4);
    strcpy(prepared.files.files[0].name, "file9");
    prepared.files.files[0].data = malloc(4);
    prepared.files.files[0].size = 3;
    memcpy(prepared.files.files[0].data, "new", 4);
    uint8_t archive[] = {1, 2, 3, 4};
    prepared.archive = archive; prepared.size = sizeof(archive);
    uint8_t *old = live.files[0].data;
    uint8_t *replacement = prepared.files.files[0].data;

    fail_close = 1;
    bool ok = ofSaveFsApplySnapshot(&live.base, &prepared);
    assert(written_bytes == sizeof(archive) && closes == 1 && lookups == 1);
    assert(!ok && "native close failure must reject snapshot apply");
    assert(live.fileCount == 1 && live.files[0].data == old);
    assert(strcmp(live.files[0].name, "file0") == 0);
    assert(live.files[0].size == 3 && memcmp(old, "old", 4) == 0);
    assert(prepared.files.fileCount == 1);
    assert(prepared.files.files[0].data == replacement);

    fail_close = 0;
    assert(ofSaveFsApplySnapshot(&live.base, &prepared));
    assert(closes == 2 && lookups == 2 && written_bytes == 2 * sizeof(archive));
    assert(live.fileCount == 1 && live.files[0].data == replacement);
    assert(strcmp(live.files[0].name, "file9") == 0);
    assert(memcmp(live.files[0].data, "new", 4) == 0);
    assert(prepared.files.fileCount == 0 && prepared.files.files[0].data == NULL);
    free(live.files[0].data);
    puts("PASS: native close failure preserves live/prepared ownership; successful retry commits");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.c").write_text(pre + types + snapshot_type
                                 + function("ofSaveFsApplySnapshot") + post)
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I" + str(repo / "src/openfpga/platform"),
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
