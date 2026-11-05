/*
 * The functions Butterscotch calls into this port. Butterscotch declares
 * each one itself next to the call, so without this header nothing checks
 * the definitions here against a prototype.
 */
#ifndef OF_HOOKS_H
#define OF_HOOKS_H

#include "audio_system.h"
#include "file_system.h"
#include "log.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

struct Runner;
/* Private-use Pocket prompt glyphs, consumed by the software text renderer. */
void *platformButtonGlyphTexture(uint16_t codepoint);
#if defined(OF_NATIVE_SAVESTATES) && !defined(OF_PC)
/* Positive proves the reserve now; zero requires the usual malloc probe. */
int platformHeapCanReserve(size_t bytes);
#endif
bool platformSnapshotBoundary(struct Runner *runner);
AudioSystem *platformCreateAudioSystem(void);
FileSystem *platformCreateFileSystem(void);
void platformDestroyFileSystem(FileSystem *base);
uint16_t *platformAcquireFramebuffer(int width, int height);
void platformSetNextFramebuffer(uint16_t *framebuffer, int width, int height, int bpp);
bool platformPackRead(const char *path, uint32_t offset, uint32_t size, void *dest);
uint64_t platformPackRefusedNanos(void);
/* Port-internal: a file stored in the audio pack (of_audio_system.c). */
bool ofAudioReadPackFile(const char *path, uint8_t **outData, uint32_t *outSize);
bool ofAudioHasPackFile(const char *path);

/* What is heard (a debug setting): a muted sound runs its course unheard and costs almost nothing. Music is
 * what the game streams or keeps in a file of its own, a sound effect what is embedded in its data. */
typedef enum { OF_AUDIO_NORMAL, OF_AUDIO_DISABLED, OF_AUDIO_MUSIC_ONLY, OF_AUDIO_SOUND_ONLY, OF_AUDIO_MODE_COUNT } OfAudioMode;
void ofAudioSetMode(OfAudioMode mode);
OfAudioMode ofAudioMode(void);
void platformLog(const logType type, const char *format, va_list va) __attribute__((format(printf, 2, 0)));

#endif
