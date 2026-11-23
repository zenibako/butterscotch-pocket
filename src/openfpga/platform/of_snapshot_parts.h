#ifndef OF_SNAPSHOT_PARTS_H
#define OF_SNAPSHOT_PARTS_H
#include "audio_system.h"
#include "file_system.h"
#include <stddef.h>
#include <stdint.h>

bool ofAudioSnapshot(AudioSystem *audio, uint8_t **data, size_t *size, const char **reason);
void *ofAudioPrepareSnapshot(AudioSystem *audio, const uint8_t *data, size_t size, const char **reason);
void ofAudioApplySnapshot(AudioSystem *audio, void *prepared);
void ofAudioFreeSnapshot(void *prepared);
/* After ofAudioApplySnapshot and the host's completion: reads the restored
 * short sounds the cache lacks (a file read before completion stalls). */
void ofAudioLoadSnapshotSounds(AudioSystem *audio);
/* Stops idle-hook mixing while a snapshot is captured or prepared. */
void ofAudioSnapshotLock(bool locked);
#ifdef OF_PC
void ofPcAudioSnapshotReset(void);
#endif
bool ofSaveFsSnapshot(FileSystem *fs, uint8_t **data, size_t *size, const char **reason);
void *ofSaveFsPrepareSnapshot(FileSystem *fs, const uint8_t *data, size_t size, const char **reason);
bool ofSaveFsApplySnapshot(FileSystem *fs, void *prepared);
void ofSaveFsFreeSnapshot(void *prepared);
#endif
