/*
 * Layout of the save archive kept in the first save slot. Shared by the
 * device-side file system (ut_save_fs.c) and the host tool (tools/mksave.c).
 *
 * A header, a table of names and sizes, then the file contents. All fields
 * are little-endian, which is native on both the device and the hosts the
 * tool runs on.
 */

#pragma once

#include <stdint.h>

#define UT_SAVE_SLOT_FILE "undertale_0.sav"
#define UT_SAVE_MAGIC 0x31565455u /* "UTV1" */
#define UT_SAVE_MAX_FILES 16
#define UT_SAVE_NAME_LEN 40
/* Stay well inside the 256 KB slot. */
#define UT_SAVE_MAX_BYTES (192 * 1024)
#define UT_SAVE_SLOT_BYTES (256 * 1024)

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
