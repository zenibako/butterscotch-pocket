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
/* Reading in the background (of_pack_read.c), one read at a time, while the
 * game goes on. ofBgReadStart starts reading `size` bytes at `offset` of the
 * file at `path` into `dest`, which must stay valid until the read is over;
 * it returns false if it cannot now (a read is in flight or its result not yet
 * taken, the OS is busy, a state request is pending) or at all (size 0 or over
 * ofBgReadMax, which is 0 where there is no background reading). ofBgReadPoll
 * says how it is going, and gives a finished read's result once. While a read
 * is in flight the OS refuses every other read: anything that reads a file
 * calls ofBgReadSettle first, which waits for it to finish (its result is still
 * given by the next poll). */
enum { OF_BG_IDLE, OF_BG_BUSY, OF_BG_DONE, OF_BG_FAILED };
uint32_t ofBgReadMax(void);
bool ofBgReadStart(const char *path, uint32_t offset, uint32_t size, void *dest);
int ofBgReadPoll(void);
void ofBgReadSettle(void);
void platformBeforeDataFileRead(void);
/* Between draw calls: a battle's loading takes a finished background read
 * and starts the next, so the card is kept busy while the frame is drawn. */
void ofPrefetchTick(void);
/* Forgets any read, after a state has been restored. */
void ofBgReadReset(void);
/* Totals so far: reads finished, their time from start to finish, and the time
 * ofBgReadSettle has waited. */
void ofBgReadTotals(uint32_t *reads, uint64_t *readNanos, uint64_t *settleNanos);
uint64_t platformPackRefusedNanos(void);
/* The direct-read log (--read-log): every direct read of the texture pack or the music,
 * noted with what came before it, for the benchmark's read table and, when asked, the log. */
enum { OF_READLOG_TEX, OF_READLOG_MUSIC };
void ofReadLogNote(int kind, uint32_t size, uint64_t refusedNanos, uint64_t waitNanos);
void ofReadLogSetVerbose(bool verbose);
/* The scripted update number (of_platform.c), for log lines. */
int ofPlatformScriptFrame(void);
void platformTextureLoadTime(uint64_t nanos);
/* Port-internal: a file stored in the audio pack (of_audio_system.c). */
bool ofAudioReadPackFile(const char *path, uint8_t **outData, uint32_t *outSize);
bool ofAudioHasPackFile(const char *path);
/* Reads a short sound (a SOND index) into the sound cache ahead of its first play: 0 read now, 1 cached already,
 * -1 not read (no such short sound, effects muted, or a failed read). */
int ofAudioPreloadSound(int32_t soundIndex);
/* ofAudioPreloadSound in two halves, for a read made in the background:
 * ofAudioPlanSound gives where in the music pack (OF_MUSIC_PATH) the sound
 * lies and returns 0, or 1 when it is cached already, -1 when a first play
 * would not read it; ofAudioAdoptSound takes the read sound (malloc'd; it is
 * kept or freed) and returns as ofAudioPreloadSound does. */
int ofAudioPlanSound(int32_t soundIndex, uint32_t *offset, uint32_t *bytes);
int ofAudioAdoptSound(int32_t soundIndex, uint8_t *data);
const char *ofAudioPackPath(void);
#ifdef OF_PC
/* BS_AUDIO_LOG: "at <frame> in <room> by <object>" for a sound being started (of_platform.c). */
void platformSoundSource(char *out, size_t size);
#endif

/* What is heard (a debug setting): a muted sound runs its course unheard and costs almost nothing. Music is
 * what the game streams or keeps in a file of its own, a sound effect what is embedded in its data. */
typedef enum { OF_AUDIO_NORMAL, OF_AUDIO_DISABLED, OF_AUDIO_MUSIC_ONLY, OF_AUDIO_SOUND_ONLY, OF_AUDIO_MODE_COUNT } OfAudioMode;
void ofAudioSetMode(OfAudioMode mode);
OfAudioMode ofAudioMode(void);
/* Mix in software even where openfpgaOS has the hardware mixer; set before the audio system starts. */
void ofAudioUseSoftwareMixer(bool software);
void platformLog(const logType type, const char *format, va_list va) __attribute__((format(printf, 2, 0)));

#endif
