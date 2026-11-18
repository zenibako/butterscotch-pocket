/*
 * Save-slot file system for Butterscotch.
 *
 * A game keeps its progress in a handful of small files next to it (Undertale:
 * file0, file9, undertale.ini, system_information_96x). openfpgaOS has no
 * writable directory; it has ten fixed 256 KB save slots, exposed as files
 * named in the instance JSON, which the Pocket writes back to the SD card
 * when the core is closed.
 *
 * All of the game's files live in one slot as a small archive: a header, a
 * table of names and sizes, then the contents. The archive is read into
 * memory at start, and rewritten to the slot whenever a file changes.
 *
 * The streaming file_bin_* interface and directories are not provided;
 * Undertale uses neither.
 */

#include "file_system.h"
#include "log.h"
#include "of_hooks.h"
#include "of_bench.h"
#include "of_save_format.h"

/* The Makefile names it after the game being built. */
#ifndef OF_SAVE_SLOT_FILE
#error "OF_SAVE_SLOT_FILE is not set; build with the Makefile"
#endif
#include "of_snapshot_parts.h"
#include "of_snapshot_bytes.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(OF_NATIVE_SAVESTATES) && !defined(OF_PC)
#include <errno.h>
#include <unistd.h>
#include "of_file.h"
#include "gettime.h"
#endif

typedef struct {
    char name[OF_SAVE_NAME_LEN];
    uint8_t *data;
    uint32_t size;
} OfSaveFile;

typedef struct {
    FileSystem base;
    OfSaveFile files[OF_SAVE_MAX_FILES];
    uint32_t fileCount;
} OfSaveFs;

/* When set, nothing is read from or written to the slot: the benchmark must
 * start from the same fresh state on every run and leave no trace. */
static bool g_volatile = false;

void ofSaveFsSetVolatile(bool enabled) {
    g_volatile = enabled;
}

/* ===[ Archive ]=== */

/* Game paths can carry a directory ("./file0"); only the name matters here. */
static const char *baseName(const char *path) {
    const char *base = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return base;
}

static OfSaveFile *findFile(OfSaveFs *fs, const char *path) {
    const char *name = baseName(path);
    for (uint32_t i = 0; i < fs->fileCount; i++) {
        if (strcmp(fs->files[i].name, name) == 0) return &fs->files[i];
    }
    return NULL;
}

static void loadArchive(OfSaveFs *fs) {
    ofBgReadSettle(); /* the OS refuses other reads while a background one is in flight */
    if (g_volatile) return;
    FILE *slot = fopen(OF_SAVE_SLOT_FILE, "rb");
    if (slot == NULL) {
        logInfo("Saves: no save data yet.\n");
        return;
    }

    OfSaveHeader header;
    bool valid = fread(&header, sizeof(header), 1, slot) == 1 && header.magic == OF_SAVE_MAGIC &&
                 header.fileCount <= OF_SAVE_MAX_FILES && header.totalBytes <= OF_SAVE_MAX_BYTES;
    if (!valid) {
        /* A slot that has never been written reads as blank. */
        logInfo("Saves: no save data yet.\n");
        fclose(slot);
        return;
    }

    uint8_t *archive = malloc(header.totalBytes);
    if (archive == NULL || fseek(slot, 0, SEEK_SET) != 0 ||
        fread(archive, 1, header.totalBytes, slot) != header.totalBytes) {
        logWarn("Saves: could not read the save slot.\n");
        free(archive);
        fclose(slot);
        return;
    }
    fclose(slot);

    const OfSaveEntry *entries = (const OfSaveEntry *) (archive + sizeof(OfSaveHeader));
    for (uint32_t i = 0; i < header.fileCount; i++) {
        const OfSaveEntry *entry = &entries[i];
        if ((uint64_t) entry->offset + entry->size > header.totalBytes) {
            logWarn("Saves: save data is damaged; ignoring the rest of it.\n");
            break;
        }
        OfSaveFile *file = &fs->files[fs->fileCount];
        memcpy(file->name, entry->name, OF_SAVE_NAME_LEN);
        file->name[OF_SAVE_NAME_LEN - 1] = '\0';
        file->size = entry->size;
        file->data = safeMalloc(entry->size + 1);
        memcpy(file->data, archive + entry->offset, entry->size);
        fs->fileCount++;
    }
    free(archive);
    logInfo("Saves: loaded %u files.\n", (unsigned) fs->fileCount);
}

static bool storeArchive(OfSaveFs *fs) {
    ofBgReadSettle(); /* the OS refuses other reads while a background one is in flight */
    if (g_volatile) return true;
    uint32_t total = (uint32_t) (sizeof(OfSaveHeader) + fs->fileCount * sizeof(OfSaveEntry));
    for (uint32_t i = 0; i < fs->fileCount; i++) total += fs->files[i].size;
    if (total > OF_SAVE_MAX_BYTES) {
        logWarn("Saves: %u bytes will not fit in the save slot.\n", (unsigned) total);
        return false;
    }

    uint8_t *archive = safeCalloc(1, total);
    OfSaveHeader *header = (OfSaveHeader *) archive;
    header->magic = OF_SAVE_MAGIC;
    header->fileCount = fs->fileCount;
    header->totalBytes = total;

    OfSaveEntry *entries = (OfSaveEntry *) (archive + sizeof(OfSaveHeader));
    uint32_t offset = (uint32_t) (sizeof(OfSaveHeader) + fs->fileCount * sizeof(OfSaveEntry));
    for (uint32_t i = 0; i < fs->fileCount; i++) {
        memcpy(entries[i].name, fs->files[i].name, OF_SAVE_NAME_LEN);
        entries[i].size = fs->files[i].size;
        entries[i].offset = offset;
        memcpy(archive + offset, fs->files[i].data, fs->files[i].size);
        offset += fs->files[i].size;
    }

    FILE *slot = fopen(OF_SAVE_SLOT_FILE, "wb");
    bool ok = slot != NULL && fwrite(archive, 1, total, slot) == total;
#if defined(OF_NATIVE_SAVESTATES) && !defined(OF_PC)
    /* fclose publishes CRAM and its size; fsync also writes the archive to SD.
     * Keep this on ordinary saves: a Memories restore only rewinds CRAM. */
    if (ok && fflush(slot) != 0) ok = false;
    if (ok && fsync(fileno(slot)) != 0) {
        logWarn("Saves: SD sync failed (errno %d).\n", errno);
        ok = false;
    }
#endif
    if (slot != NULL && fclose(slot) != 0) ok = false;
    free(archive);
    if (!ok) logWarn("Saves: could not write the save slot.\n");
#if defined(OF_NATIVE_SAVESTATES) && !defined(OF_PC)
    else logInfo("Saves: SD sync ok, %u bytes.\n", (unsigned) total);
#endif
    return ok;
}

static bool putFile(OfSaveFs *fs, const char *path, const uint8_t *data, uint32_t size) {
    OfSaveFile *file = findFile(fs, path);
    if (file == NULL) {
        const char *name = baseName(path);
        if (fs->fileCount >= OF_SAVE_MAX_FILES || strlen(name) >= OF_SAVE_NAME_LEN) {
            logWarn("Saves: cannot store '%s'.\n", path);
            return false;
        }
        file = &fs->files[fs->fileCount++];
        memset(file, 0, sizeof(*file));
        strcpy(file->name, name);
    }

    uint8_t *copy = safeMalloc(size + 1);
    memcpy(copy, data, size);
    free(file->data);
    file->data = copy;
    file->size = size;
    return storeArchive(fs);
}

/* ===[ Vtable ]=== */

static char *fsResolvePath(FileSystem *base, const char *relativePath) {
    (void) base;
    return safeStrdup(relativePath);
}

/* Files the game ships beside its data file (Deltarune's lang/lang_en.json, for one) are read-only: anything
 * the game writes goes to the archive, which is looked up first. On the device they are stored in the audio
 * pack, since a core has no spare data slot; the desktop build reads them from the folder it runs in. */
static bool readBundled(const char *relativePath, uint8_t **outData, uint32_t *outSize) {
    ofBgReadSettle(); /* the OS refuses other reads while a background one is in flight */
    if (ofAudioReadPackFile(relativePath, outData, outSize)) return true;
#ifdef OF_PC
    FILE *f = fopen(relativePath, "rb");
    if (f == NULL) return false;
    bool ok = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size >= 0 && fseek(f, 0, SEEK_SET) == 0) {
            uint8_t *data = safeMalloc((size_t) size + 1);
            if (fread(data, 1, (size_t) size, f) == (size_t) size) {
                data[size] = '\0';
                *outData = data;
                *outSize = (uint32_t) size;
                ok = true;
            } else
                free(data);
        }
    }
    fclose(f);
    return ok;
#else
    return false;
#endif
}

/* Whether readBundled would find the file, without reading it. */
static bool bundledExists(const char *relativePath) {
    ofBgReadSettle(); /* the OS refuses other reads while a background one is in flight */
#ifdef OF_PC
    FILE *f = fopen(relativePath, "rb");
    if (f != NULL) {
        fclose(f);
        return true;
    }
#endif
    return ofAudioHasPackFile(relativePath);
}

static bool fsFileExists(FileSystem *base, const char *relativePath) {
    return findFile((OfSaveFs *) base, relativePath) != NULL || bundledExists(relativePath);
}

static char *fsReadFileText(FileSystem *base, const char *relativePath) {
    OfSaveFile *file = findFile((OfSaveFs *) base, relativePath);
    if (file == NULL) {
        uint8_t *data = NULL;
        uint32_t size = 0;
        return readBundled(relativePath, &data, &size) ? (char *) data : NULL;
    }
    char *text = safeMalloc(file->size + 1);
    memcpy(text, file->data, file->size);
    text[file->size] = '\0';
    return text;
}

static bool fsWriteFileText(FileSystem *base, const char *relativePath, const char *contents) {
    return putFile((OfSaveFs *) base, relativePath, (const uint8_t *) contents, (uint32_t) strlen(contents));
}

static bool fsDeleteFile(FileSystem *base, const char *relativePath) {
    OfSaveFs *fs = (OfSaveFs *) base;
    OfSaveFile *file = findFile(fs, relativePath);
    if (file == NULL) return false;

    free(file->data);
    *file = fs->files[--fs->fileCount];
    memset(&fs->files[fs->fileCount], 0, sizeof(OfSaveFile));
    return storeArchive(fs);
}

static bool fsReadFileBinary(FileSystem *base, const char *relativePath, uint8_t **outData, int32_t *outSize) {
    OfSaveFile *file = findFile((OfSaveFs *) base, relativePath);
    if (file == NULL) {
        uint32_t size = 0;
        if (!readBundled(relativePath, outData, &size)) return false;
        *outSize = (int32_t) size;
        return true;
    }
    *outData = safeMalloc(file->size + 1);
    memcpy(*outData, file->data, file->size);
    *outSize = (int32_t) file->size;
    return true;
}

static bool fsWriteFileBinary(FileSystem *base, const char *relativePath, const uint8_t *data, int32_t size) {
    if (size < 0) return false;
    return putFile((OfSaveFs *) base, relativePath, data, (uint32_t) size);
}

static bool fsRenameFile(FileSystem *base, const char *oldRelativePath, const char *newRelativePath) {
    OfSaveFs *fs = (OfSaveFs *) base;
    OfSaveFile *file = findFile(fs, oldRelativePath);
    const char *newName = baseName(newRelativePath);
    if (file == NULL || strlen(newName) >= OF_SAVE_NAME_LEN) return false;

    OfSaveFile *existing = findFile(fs, newRelativePath);
    if (existing != NULL && existing != file) {
        free(existing->data);
        *existing = fs->files[--fs->fileCount];
        memset(&fs->files[fs->fileCount], 0, sizeof(OfSaveFile));
        file = findFile(fs, oldRelativePath);
    }
    strcpy(file->name, newName);
    return storeArchive(fs);
}

/* Streaming binary files and directories are not supported. */
static void *fsBinaryOpen(FileSystem *base, const char *relativePath, int32_t mode) {
    (void) base;
    (void) mode;
    logWarn("Saves: file_bin access to '%s' is not supported.\n", relativePath);
    return NULL;
}
static void fsBinaryClose(FileSystem *base, void *handle) { (void) base; (void) handle; }
static int32_t fsBinaryRead(FileSystem *base, void *handle, void *dst, int32_t n) { (void) base; (void) handle; (void) dst; (void) n; return 0; }
static int32_t fsBinaryWrite(FileSystem *base, void *handle, const void *src, int32_t n) { (void) base; (void) handle; (void) src; (void) n; return 0; }
static int32_t fsBinaryTell(FileSystem *base, void *handle) { (void) base; (void) handle; return 0; }
static bool fsBinarySeek(FileSystem *base, void *handle, int32_t pos) { (void) base; (void) handle; (void) pos; return false; }
static int32_t fsBinarySize(FileSystem *base, void *handle) { (void) base; (void) handle; return 0; }
static void fsBinaryRewrite(FileSystem *base, void *handle) { (void) base; (void) handle; }
static bool fsDirectoryExists(FileSystem *base, const char *relativePath) { (void) base; (void) relativePath; return false; }
static bool fsCreateDirectory(FileSystem *base, const char *relativePath) { (void) base; (void) relativePath; return false; }
static bool fsDeleteDirectory(FileSystem *base, const char *relativePath) { (void) base; (void) relativePath; return false; }
static FileSystemDirEntry *fsListDirectory(FileSystem *base, const char *relativeDirPath) { (void) base; (void) relativeDirPath; return NULL; }

static FileSystemVtable g_vtable = {
    .resolvePath = fsResolvePath,
    .fileExists = fsFileExists,
    .readFileText = fsReadFileText,
    .writeFileText = fsWriteFileText,
    .deleteFile = fsDeleteFile,
    .readFileBinary = fsReadFileBinary,
    .writeFileBinary = fsWriteFileBinary,
    .renameFile = fsRenameFile,
    .binaryOpen = fsBinaryOpen,
    .binaryClose = fsBinaryClose,
    .binaryRead = fsBinaryRead,
    .binaryWrite = fsBinaryWrite,
    .binaryTell = fsBinaryTell,
    .binarySeek = fsBinarySeek,
    .binarySize = fsBinarySize,
    .binaryRewrite = fsBinaryRewrite,
    .directoryExists = fsDirectoryExists,
    .createDirectory = fsCreateDirectory,
    .deleteDirectory = fsDeleteDirectory,
    .listDirectory = fsListDirectory,
};

FileSystem *platformCreateFileSystem(void) {
    OfSaveFs *fs = safeCalloc(1, sizeof(OfSaveFs));
    fs->base.vtable = &g_vtable;
    loadArchive(fs);
    return &fs->base;
}

void platformDestroyFileSystem(FileSystem *base) {
    OfSaveFs *fs = (OfSaveFs *) base;
    for (uint32_t i = 0; i < fs->fileCount; i++) free(fs->files[i].data);
    free(fs);
}


/* Native states include the virtual progress files. Preparing only allocates;
 * committing writes the already prepared archive before swapping live files. */
typedef struct {
    OfSaveFs files;
    uint8_t *archive;
    size_t size;
} OfSaveSnapshot;

bool ofSaveFsSnapshot(FileSystem *base, uint8_t **data, size_t *size, const char **reason) {
    OfSaveFs *fs = (OfSaveFs *)base;
    OfSnapshotWriter w = {.ok = true};
    *data = NULL; *size = 0;
    if (!fs || fs->fileCount > OF_SAVE_MAX_FILES) {
        *reason = "Unsupported save-file backend"; return false;
    }
    ofSnapU32(&w, OF_SAVE_MAGIC); ofSnapU32(&w, fs->fileCount);
    ofSnapU32(&w, 0); ofSnapU32(&w, 0);
    size_t off = sizeof(OfSaveHeader) + fs->fileCount * sizeof(OfSaveEntry);
    for (uint32_t i = 0; i < fs->fileCount; i++) {
        ofSnapWrite(&w, fs->files[i].name, OF_SAVE_NAME_LEN);
        ofSnapU32(&w, fs->files[i].size); ofSnapU32(&w, (uint32_t)off);
        off += fs->files[i].size;
    }
    for (uint32_t i = 0; i < fs->fileCount; i++)
        ofSnapWrite(&w, fs->files[i].data, fs->files[i].size);
    if (!w.ok || w.size > OF_SAVE_MAX_BYTES) {
        free(w.data); *reason = "Progress files are too large"; return false;
    }
    uint32_t total = (uint32_t)w.size;
    for (unsigned i = 0; i < 4; i++) w.data[8 + i] = (uint8_t)(total >> (i * 8));
    *data = w.data; *size = w.size; return true;
}

void ofSaveFsFreeSnapshot(void *ptr) {
    OfSaveSnapshot *p = ptr;
    if (!p) return;
    for (uint32_t i = 0; i < p->files.fileCount; i++) free(p->files.files[i].data);
    free(p->archive); free(p);
}

void *ofSaveFsPrepareSnapshot(FileSystem *base, const uint8_t *data, size_t size, const char **reason) {
    (void)base;
    OfSnapshotReader r = {data, size, 0, true};
    if (size < sizeof(OfSaveHeader) || size > OF_SAVE_MAX_BYTES ||
        ofSnapGetU32(&r) != OF_SAVE_MAGIC) goto bad;
    uint32_t count = ofSnapGetU32(&r), total = ofSnapGetU32(&r);
    if (ofSnapGetU32(&r) != 0 || count > OF_SAVE_MAX_FILES || total != size ||
        sizeof(OfSaveHeader) + count * sizeof(OfSaveEntry) > size) goto bad;
    OfSaveSnapshot *p = calloc(1, sizeof(*p));
    if (!p) goto bad;
    p->archive = malloc(size); p->size = size;
    if (!p->archive) { ofSaveFsFreeSnapshot(p); goto bad; }
    memcpy(p->archive, data, size);
    size_t next = sizeof(OfSaveHeader) + count * sizeof(OfSaveEntry);
    for (uint32_t i = 0; i < count; i++) {
        OfSaveFile *f = &p->files.files[i];
        ofSnapRead(&r, f->name, OF_SAVE_NAME_LEN);
        f->size = ofSnapGetU32(&r); uint32_t off = ofSnapGetU32(&r);
        if (!r.ok || !f->name[0] || !memchr(f->name, 0, OF_SAVE_NAME_LEN) ||
            strchr(f->name, '/') || strchr(f->name, '\\') || off != next ||
            f->size > size - next) { ofSaveFsFreeSnapshot(p); goto bad; }
        for (uint32_t j = 0; j < i; j++)
            if (strcmp(f->name, p->files.files[j].name) == 0) {
                ofSaveFsFreeSnapshot(p); goto bad;
            }
        f->data = malloc((size_t)f->size + 1);
        if (!f->data) { ofSaveFsFreeSnapshot(p); goto bad; }
        memcpy(f->data, data + off, f->size); f->data[f->size] = 0;
        p->files.fileCount++;
        next += f->size;
    }
    if (next != size) { ofSaveFsFreeSnapshot(p); goto bad; }
    return p;
bad:
    *reason = "Invalid progress files in state"; return NULL;
}

bool ofSaveFsApplySnapshot(FileSystem *base, void *ptr) {
    ofBgReadSettle(); /* the OS refuses other reads while a background one is in flight */
    OfSaveFs *fs = (OfSaveFs *)base;
    OfSaveSnapshot *p = ptr;
    if (!g_volatile) {
#ifdef OF_PC
        /* Only rename after the entire temporary archive has closed. */
        const char *temporary = OF_SAVE_SLOT_FILE ".snapshot-tmp";
        FILE *slot = fopen(temporary, "wb");
        bool ok = slot && fwrite(p->archive, 1, p->size, slot) == p->size;
        if (slot && fclose(slot) != 0) ok = false;
        if (ok) ok = rename(temporary, OF_SAVE_SLOT_FILE) == 0;
        if (!ok) remove(temporary);
#else
        /* Reject the kernel's save-name fallback: the slot must have been
         * discovered from the Pocket instance, so close can publish its size. */
#ifdef OF_NATIVE_SAVESTATES
        uint64_t timings[6] = {nowNanos(), 0, 0, 0, 0, 0};
        uint32_t slotId = 0;
        if (of_file_slot_find(OF_SAVE_SLOT_FILE, &slotId) != 0 || slotId != 10) return false;
        timings[1] = nowNanos();
#endif
        /* The pinned Pocket kernel validates a whole CRAM write before its
         * memcpy. One unbuffered write fits the 256 KiB slot; r+b avoids
         * O_TRUNC before that validation. Header.totalBytes ignores old tail
         * bytes when the restored archive is shorter. */
        FILE *slot = fopen(OF_SAVE_SLOT_FILE, "r+b");
#ifdef OF_NATIVE_SAVESTATES
        timings[2] = nowNanos();
#endif
        bool ok = slot && setvbuf(slot, NULL, _IONBF, 0) == 0;
#ifdef OF_NATIVE_SAVESTATES
        timings[3] = nowNanos();
#endif
        if (ok) ok = fwrite(p->archive, 1, p->size, slot) == p->size;
#ifdef OF_NATIVE_SAVESTATES
        timings[4] = nowNanos();
#endif
        if (slot && fclose(slot) != 0) ok = false;
#ifdef OF_NATIVE_SAVESTATES
        timings[5] = nowNanos();
        logInfo("State archive phases: lookup %llu open %llu buffer %llu write %llu close %llu ms; %zu bytes, %s\n",
            (unsigned long long)((timings[1] - timings[0]) / 1000000),
            (unsigned long long)((timings[2] - timings[1]) / 1000000),
            (unsigned long long)((timings[3] - timings[2]) / 1000000),
            (unsigned long long)((timings[4] - timings[3]) / 1000000),
            (unsigned long long)((timings[5] - timings[4]) / 1000000),
            p->size, ok ? "ok" : "failed");
#endif
#endif
        if (!ok) return false;
    }
    for (uint32_t i = 0; i < fs->fileCount; i++) free(fs->files[i].data);
    memcpy(fs->files, p->files.files, sizeof(fs->files));
    fs->fileCount = p->files.fileCount;
    memset(p->files.files, 0, sizeof(p->files.files)); p->files.fileCount = 0;
    return true;
}
