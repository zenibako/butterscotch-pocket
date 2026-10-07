/*
 * mksave — move Undertale saves between a desktop save folder and the
 * port's save slot file.
 *
 *   mksave pack   <save folder> <undertale_0.sav>   desktop save -> slot file
 *   mksave unpack <undertale_0.sav> <folder>        slot file -> desktop save
 *   mksave list   <undertale_0.sav>                 show what a slot file holds
 *
 * The desktop game keeps its saves in
 *   macOS    ~/Library/Application Support/com.tobyfox.undertale/
 *   Windows  %LOCALAPPDATA%\UNDERTALE\
 *   Linux    ~/.config/UNDERTALE/
 * On the Pocket the slot file lives at Saves/undertale/common/ on the SD card.
 */

#include "../platform/ut_save_format.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void die(const char *what, const char *path) {
    fprintf(stderr, "mksave: %s '%s'\n", what, path);
    exit(1);
}

static uint8_t *readWhole(const char *path, uint32_t *outSize) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) die("cannot open", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t) size + 1);
    if (size < 0 || data == NULL || fread(data, 1, (size_t) size, f) != (size_t) size) die("cannot read", path);
    fclose(f);
    *outSize = (uint32_t) size;
    return data;
}

/* Only the game's own save files are wanted: file0, file8, file9,
 * undertale.ini, system_information_962/963 and the like. Anything else in
 * the folder (.DS_Store, Steam leftovers) is skipped. */
static bool isSaveName(const char *name) {
    if (name[0] == '.') return false;
    if (strlen(name) >= UT_SAVE_NAME_LEN) return false;
    return strncmp(name, "file", 4) == 0 || strcmp(name, "undertale.ini") == 0 ||
           strncmp(name, "system_information_", 19) == 0 || strcmp(name, "config.ini") == 0;
}

static int compareNames(const void *a, const void *b) {
    return strcmp((const char *) a, (const char *) b);
}

static int pack(const char *folder, const char *outPath) {
    DIR *dir = opendir(folder);
    if (dir == NULL) die("cannot open folder", folder);

    char names[UT_SAVE_MAX_FILES][UT_SAVE_NAME_LEN];
    uint32_t count = 0;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (!isSaveName(item->d_name)) {
            if (item->d_name[0] != '.') printf("  skipped   %s\n", item->d_name);
            continue;
        }
        if (count == UT_SAVE_MAX_FILES) die("too many save files in", folder);
        strcpy(names[count++], item->d_name);
    }
    closedir(dir);
    if (count == 0) die("no Undertale save files in", folder);
    qsort(names, count, UT_SAVE_NAME_LEN, compareNames);

    /* Written at the full slot size; the game only reads totalBytes of it. */
    uint8_t *archive = calloc(1, UT_SAVE_SLOT_BYTES);
    UtSaveHeader *header = (UtSaveHeader *) archive;
    UtSaveEntry *entries = (UtSaveEntry *) (archive + sizeof(UtSaveHeader));
    uint32_t offset = sizeof(UtSaveHeader) + count * sizeof(UtSaveEntry);

    for (uint32_t i = 0; i < count; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", folder, names[i]);
        uint32_t size;
        uint8_t *data = readWhole(path, &size);
        if (offset + size > UT_SAVE_MAX_BYTES) die("save data too large to fit the slot, at", names[i]);
        strcpy(entries[i].name, names[i]);
        entries[i].size = size;
        entries[i].offset = offset;
        memcpy(archive + offset, data, size);
        free(data);
        offset += size;
        printf("  packed    %-28s %6u bytes\n", names[i], (unsigned) size);
    }
    header->magic = UT_SAVE_MAGIC;
    header->fileCount = count;
    header->totalBytes = offset;

    FILE *out = fopen(outPath, "wb");
    if (out == NULL || fwrite(archive, 1, UT_SAVE_SLOT_BYTES, out) != UT_SAVE_SLOT_BYTES || fclose(out) != 0)
        die("cannot write", outPath);
    printf("%s: %u files, %u bytes used\n", outPath, (unsigned) count, (unsigned) offset);
    return 0;
}

/* Reads and checks a slot file; returns the archive and its header. */
static uint8_t *openArchive(const char *path, UtSaveHeader *header) {
    uint32_t size;
    uint8_t *archive = readWhole(path, &size);
    if (size < sizeof(UtSaveHeader)) die("no save data in", path);
    memcpy(header, archive, sizeof(*header));
    if (header->magic != UT_SAVE_MAGIC) die("no save data in", path);
    uint64_t tableEnd = sizeof(UtSaveHeader) + (uint64_t) header->fileCount * sizeof(UtSaveEntry);
    if (header->fileCount > UT_SAVE_MAX_FILES || header->totalBytes > size || tableEnd > header->totalBytes)
        die("damaged save data in", path);
    return archive;
}

static int unpack(const char *path, const char *folder) {
    UtSaveHeader header;
    uint8_t *archive = openArchive(path, &header);
    const UtSaveEntry *entries = (const UtSaveEntry *) (archive + sizeof(UtSaveHeader));

    if (folder != NULL) mkdir(folder, 0755);
    for (uint32_t i = 0; i < header.fileCount; i++) {
        char name[UT_SAVE_NAME_LEN];
        memcpy(name, entries[i].name, UT_SAVE_NAME_LEN);
        name[UT_SAVE_NAME_LEN - 1] = '\0';
        if ((uint64_t) entries[i].offset + entries[i].size > header.totalBytes) die("damaged save data in", path);
        if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL || name[0] == '.' || name[0] == '\0')
            die("unexpected file name in", path);

        if (folder == NULL) {
            printf("  %-28s %6u bytes\n", name, (unsigned) entries[i].size);
            continue;
        }
        char outPath[1024];
        snprintf(outPath, sizeof(outPath), "%s/%s", folder, name);
        FILE *out = fopen(outPath, "wb");
        if (out == NULL || fwrite(archive + entries[i].offset, 1, entries[i].size, out) != entries[i].size ||
            fclose(out) != 0)
            die("cannot write", outPath);
        printf("  unpacked  %-28s %6u bytes\n", name, (unsigned) entries[i].size);
    }
    printf("%s: %u files, %u bytes used\n", path, (unsigned) header.fileCount, (unsigned) header.totalBytes);
    free(archive);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "pack") == 0) return pack(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "unpack") == 0) return unpack(argv[2], argv[3]);
    if (argc == 3 && strcmp(argv[1], "list") == 0) return unpack(argv[2], NULL);
    fprintf(stderr, "usage: mksave pack   <save folder> <undertale_0.sav>\n"
                    "       mksave unpack <undertale_0.sav> <folder>\n"
                    "       mksave list   <undertale_0.sav>\n");
    return 2;
}
