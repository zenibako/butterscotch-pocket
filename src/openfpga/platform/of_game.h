#ifndef OF_GAME_H
#define OF_GAME_H

/*
 * What differs between the games this port runs. Each game has one file,
 * game_<name>.c, and the Makefile compiles the one GAME names; everything
 * else in platform/ is the same for every game.
 */

#include <stddef.h>
#include <stdint.h>

#include "data_win.h"
#include "of_bench.h"
#include "runner.h"
#include "vm.h"

#define OF_PREFETCH_WARMUP (-1) /* common assets in overworld spare time */

/* A sound in battlePrefetch's entries, and back. */
#define OF_PREFETCH_SOUND(index) (-1 - (int32_t) (index))
#define OF_PREFETCH_SOUND_INDEX(entry) (-1 - (entry))

/* A game string replaced as loaded (see of_strings.c). */
typedef struct {
    const char *from;
    const char *to;
} OfStringPatch;

/* A code entry or script written out in C (see of_native.c). It goes in only
 * if the game's bytecode for it has exactly this length and hash. */
typedef struct {
    const char *name;
    uint32_t length;
    uint64_t hash;
    VMNativeCode native;
} OfNativeCode;

typedef struct {
    const char *name;
    uint32_t length;
    uint64_t hash;
    VMNativeScript native;
} OfNativeScript;

/* A stretch of the benchmark run, ending on lastFrame. */
typedef struct {
    const char *name;
    int lastFrame;
    const char *tag; /* five letters at most, for the breakdown table */
    /* Optional scene contract; NULL means no requirement. */
    const char *roomName;
    const char *requiredObject;
    /* Optional end-of-window condition for scenes with shared objects. */
    bool (*endValid)(const Runner *runner);
} OfBenchSection;

#define OF_BENCH_MAX_SECTIONS 8
#define OF_BENCH_MAX_OPENING_WINDOWS 8

/* A window in scripted update numbers, including updates with no swap. */
typedef struct {
    const char *tag;
    int firstUpdate, endUpdate;
} OfBenchOpeningWindow;

typedef struct {
    /* The game's name, and its files on the card, for the loading screen's errors. */
    const char *title;
    const char *dataFiles;
    /* The loading bar's colours (RGB555), as the game's own HP bar; 0 for
     * both: white on grey. */
    uint16_t loadingFill, loadingTrack;
    /* Prompts that name keyboard keys, beyond the ones every game shares. */
    const OfStringPatch *strings;
    size_t stringCount;
    /* Changes to the game's code, after its strings are patched (or NULL). */
    void (*patchCode)(DataWin *dw);
    /* Cosmetic script/draw replacements; retained when performance natives
     * are disabled for renderer comparisons. Each checks its own code hash. */
    void (*installPrompts)(Runner *runner);

    const OfNativeCode *natives;
    size_t nativeCount;
    const OfNativeScript *nativeScripts;
    size_t nativeScriptCount;
    /* Called for each new VM (a game_change gives one), to forget its ids (or NULL). */
    void (*nativesForgetVm)(void);
    void (*installCollisionFilter)(Runner *runner);

    /* Derived battle appearance: selected assets and whether the scene is active. */
    void (*battleUiInstall)(Runner *runner);
    bool (*battleUiActive)(Runner *runner);
    /* Speed mode may keep battles (battleUiActive) at full resolution instead
     * of drawing them at half size: the game's own text, at a cost in frames.
     * With speedHalfBattle, Speed draws each battle at full size unless it says
     * the battle is one of the heavy ones, kept at half size; asked once, as
     * the battle starts. Without it, only when asked (ofPlatformSetSpeedFullBattles). */
    bool speedFullResBattles;
    bool (*speedHalfBattle)(Runner *runner);
    /* How many updates in a row Speed may leave undrawn when it is behind: 2 where
     * the game is in steady action that keeps its pace better that way (Flowey's
     * fight once its intro is over), otherwise 1. NULL: always 1. */
    int (*speedSkipRun)(Runner *runner);
    /* What a battle about to start will draw and play first, for the platform to
     * load during its encounter (see swrPrefetchItem and ofAudioPreloadSound): at
     * most max entries, first needed first, each a texture item (TPAG index) or,
     * as OF_PREFETCH_SOUND(index), a sound (SOND index); and the battle's number
     * into battle; and into left, -1 while what is on screen should keep its pace
     * (load in spare time only), otherwise how many updates are left before the
     * battle starts (at least 1); the platform now reads in the background and
     * keeps the pace either way, so it does not use left. 0 when no battle is
     * starting. OF_PREFETCH_WARMUP in battle requests common assets in spare
     * time before an encounter. NULL: none ahead. */
    int (*battlePrefetch)(Runner *runner, int32_t *entries, int max, int *battle, int *left);

    /* The built-in benchmark (see of_bench.h). */
    const char *benchTitle;
    const char *benchScript;
    const OfJump *benchJumps;
    int benchJumpCount;
    const OfBenchSection *benchSections;
    int benchSectionCount;
    /* A second benchmark ("--bench-battles"), or NULL: the same report over another script. */
    const char *tourTitle;
    const char *tourScript;
    const OfJump *tourJumps;
    int tourJumpCount;
    const OfBenchSection *tourSections;
    int tourSectionCount;
    /* Optional: the battle tour over other battles ("--bench-battles-list 4,81,101"), each visited
     * the way the tour visits its own. Fills in a script, jumps and sections that last for the run;
     * false if the list is empty, too long or has an id the game does not have. */
    bool (*tourForList)(const char *list, const char **script, const OfJump **jumps, int *jumpCount,
                        const OfBenchSection **sections, int *sectionCount);
    /* A third benchmark ("--bench-bullet-hell"), or NULL: the same report over another script. */
    const char *bulletTitle;
    const char *bulletScript;
    const OfJump *bulletJumps;
    int bulletJumpCount;
    const OfBenchSection *bulletSections;
    int bulletSectionCount;
    /* Optional short Asriel encounter. */
    const char *saberTitle;
    const char *saberScript;
    const OfJump *saberJumps;
    int saberJumpCount;
    const OfBenchSection *saberSections;
    int saberSectionCount;
    /* Optional Photoshop Flowey checkpoint tour. */
    const char *floweyTitle;
    const char *floweyScript;
    const OfJump *floweyJumps;
    int floweyJumpCount;
    const OfBenchSection *floweySections;
    int floweySectionCount;
    const OfBenchOpeningWindow *floweyOpening;
    int floweyOpeningCount;
    /* Optional single-room benchmark ("--bench-room"): Undertale's room 57 snow patch, Deltarune's room 85 dancers. */
    const char *roomTitle;
    const char *roomScript;
    const OfJump *roomJumps;
    int roomJumpCount;
    const OfBenchSection *roomSections;
    int roomSectionCount;
    /* The SD read tests read megabyte n of the audio pack as n / ioDivisor, and the
     * async ones megabyte ioAsyncBase + i * ioAsyncStep, to stay inside a smaller pack. */
    uint32_t ioDivisor;
    uint32_t ioAsyncBase;
    uint32_t ioAsyncStep;

    /* Why the current scene can't be saved as a state, or NULL if it can
     * (NULL here: the game has no save states yet). The runner's own
     * resource check (Runner_snapshotCheck) runs after it. */
    const char *(*savestateReason)(Runner *runner);
    /* Interactive sleep benchmark: advance its game-specific checkpoints
     * and return the current on-screen instruction (NULL: unavailable). */
    const char *(*sleepTest)(Runner *runner);
} OfGame;

extern const OfGame ofGame;

#endif /* OF_GAME_H */
