/*
 * Save-slot file system for Butterscotch.
 *
 * Undertale keeps its progress in a handful of small files next to the game
 * (file0, file9, undertale.ini, system_information_96x). openfpgaOS has no
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
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UT_SAVE_SLOT_FILE "undertale_0.sav"
#define UT_SAVE_MAGIC 0x31565455u /* "UTV1" */
#define UT_SAVE_MAX_FILES 16
#define UT_SAVE_NAME_LEN 40
/* Stay well inside the 256 KB slot. */
#define UT_SAVE_MAX_BYTES (192 * 1024)

typedef struct {
    uint32_t magic;
    uint32_t fileCount;
    uint32_t totalBytes; /* size of the whole archive, header included */
    uint32_t reserved;
} UtSaveHeader;

typedef struct {
    char name[UT_SAVE_NAME_LEN];
    uint32_t size;
    uint32_t offset; /* from the start of the archive */
} UtSaveEntry;

typedef struct {
    char name[UT_SAVE_NAME_LEN];
    uint8_t *data;
    uint32_t size;
} UtSaveFile;

typedef struct {
    FileSystem base;
    UtSaveFile files[UT_SAVE_MAX_FILES];
    uint32_t fileCount;
} UtSaveFs;

/* When set, nothing is read from or written to the slot: the benchmark must
 * start from the same fresh state on every run and leave no trace. */
static bool g_volatile = false;

void utSaveFsSetVolatile(bool enabled) {
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

static UtSaveFile *findFile(UtSaveFs *fs, const char *path) {
    const char *name = baseName(path);
    for (uint32_t i = 0; i < fs->fileCount; i++) {
        if (strcmp(fs->files[i].name, name) == 0) return &fs->files[i];
    }
    return NULL;
}

static void loadArchive(UtSaveFs *fs) {
    if (g_volatile) return;
    FILE *slot = fopen(UT_SAVE_SLOT_FILE, "rb");
    if (slot == NULL) {
        logInfo("Saves: no save data yet.\n");
        return;
    }

    UtSaveHeader header;
    bool valid = fread(&header, sizeof(header), 1, slot) == 1 && header.magic == UT_SAVE_MAGIC &&
                 header.fileCount <= UT_SAVE_MAX_FILES && header.totalBytes <= UT_SAVE_MAX_BYTES;
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

    const UtSaveEntry *entries = (const UtSaveEntry *) (archive + sizeof(UtSaveHeader));
    for (uint32_t i = 0; i < header.fileCount; i++) {
        const UtSaveEntry *entry = &entries[i];
        if ((uint64_t) entry->offset + entry->size > header.totalBytes) {
            logWarn("Saves: save data is damaged; ignoring the rest of it.\n");
            break;
        }
        UtSaveFile *file = &fs->files[fs->fileCount];
        memcpy(file->name, entry->name, UT_SAVE_NAME_LEN);
        file->name[UT_SAVE_NAME_LEN - 1] = '\0';
        file->size = entry->size;
        file->data = safeMalloc(entry->size + 1);
        memcpy(file->data, archive + entry->offset, entry->size);
        fs->fileCount++;
    }
    free(archive);
    logInfo("Saves: loaded %u files.\n", (unsigned) fs->fileCount);
}

static bool storeArchive(UtSaveFs *fs) {
    if (g_volatile) return true;
    uint32_t total = sizeof(UtSaveHeader) + fs->fileCount * sizeof(UtSaveEntry);
    for (uint32_t i = 0; i < fs->fileCount; i++) total += fs->files[i].size;
    if (total > UT_SAVE_MAX_BYTES) {
        logWarn("Saves: %u bytes will not fit in the save slot.\n", (unsigned) total);
        return false;
    }

    uint8_t *archive = safeCalloc(1, total);
    UtSaveHeader *header = (UtSaveHeader *) archive;
    header->magic = UT_SAVE_MAGIC;
    header->fileCount = fs->fileCount;
    header->totalBytes = total;

    UtSaveEntry *entries = (UtSaveEntry *) (archive + sizeof(UtSaveHeader));
    uint32_t offset = sizeof(UtSaveHeader) + fs->fileCount * sizeof(UtSaveEntry);
    for (uint32_t i = 0; i < fs->fileCount; i++) {
        memcpy(entries[i].name, fs->files[i].name, UT_SAVE_NAME_LEN);
        entries[i].size = fs->files[i].size;
        entries[i].offset = offset;
        memcpy(archive + offset, fs->files[i].data, fs->files[i].size);
        offset += fs->files[i].size;
    }

    FILE *slot = fopen(UT_SAVE_SLOT_FILE, "wb");
    bool ok = slot != NULL && fwrite(archive, 1, total, slot) == total;
    if (slot != NULL && fclose(slot) != 0) ok = false;
    free(archive);
    if (!ok) logWarn("Saves: could not write the save slot.\n");
    return ok;
}

static bool putFile(UtSaveFs *fs, const char *path, const uint8_t *data, uint32_t size) {
    UtSaveFile *file = findFile(fs, path);
    if (file == NULL) {
        const char *name = baseName(path);
        if (fs->fileCount >= UT_SAVE_MAX_FILES || strlen(name) >= UT_SAVE_NAME_LEN) {
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

static bool fsFileExists(FileSystem *base, const char *relativePath) {
    return findFile((UtSaveFs *) base, relativePath) != NULL;
}

static char *fsReadFileText(FileSystem *base, const char *relativePath) {
    UtSaveFile *file = findFile((UtSaveFs *) base, relativePath);
    if (file == NULL) return NULL;
    char *text = safeMalloc(file->size + 1);
    memcpy(text, file->data, file->size);
    text[file->size] = '\0';
    return text;
}

static bool fsWriteFileText(FileSystem *base, const char *relativePath, const char *contents) {
    return putFile((UtSaveFs *) base, relativePath, (const uint8_t *) contents, (uint32_t) strlen(contents));
}

static bool fsDeleteFile(FileSystem *base, const char *relativePath) {
    UtSaveFs *fs = (UtSaveFs *) base;
    UtSaveFile *file = findFile(fs, relativePath);
    if (file == NULL) return false;

    free(file->data);
    *file = fs->files[--fs->fileCount];
    memset(&fs->files[fs->fileCount], 0, sizeof(UtSaveFile));
    return storeArchive(fs);
}

static bool fsReadFileBinary(FileSystem *base, const char *relativePath, uint8_t **outData, int32_t *outSize) {
    UtSaveFile *file = findFile((UtSaveFs *) base, relativePath);
    if (file == NULL) return false;
    *outData = safeMalloc(file->size + 1);
    memcpy(*outData, file->data, file->size);
    *outSize = (int32_t) file->size;
    return true;
}

static bool fsWriteFileBinary(FileSystem *base, const char *relativePath, const uint8_t *data, int32_t size) {
    if (size < 0) return false;
    return putFile((UtSaveFs *) base, relativePath, data, (uint32_t) size);
}

static bool fsRenameFile(FileSystem *base, const char *oldRelativePath, const char *newRelativePath) {
    UtSaveFs *fs = (UtSaveFs *) base;
    UtSaveFile *file = findFile(fs, oldRelativePath);
    const char *newName = baseName(newRelativePath);
    if (file == NULL || strlen(newName) >= UT_SAVE_NAME_LEN) return false;

    UtSaveFile *existing = findFile(fs, newRelativePath);
    if (existing != NULL && existing != file) {
        free(existing->data);
        *existing = fs->files[--fs->fileCount];
        memset(&fs->files[fs->fileCount], 0, sizeof(UtSaveFile));
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
    UtSaveFs *fs = safeCalloc(1, sizeof(UtSaveFs));
    fs->base.vtable = &g_vtable;
    loadArchive(fs);
    return &fs->base;
}

void platformDestroyFileSystem(FileSystem *base) {
    UtSaveFs *fs = (UtSaveFs *) base;
    for (uint32_t i = 0; i < fs->fileCount; i++) free(fs->files[i].data);
    free(fs);
}
