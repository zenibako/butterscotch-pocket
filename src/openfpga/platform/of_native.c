/* Native stand-ins for game scripts.
 *
 * The interpreter costs about 2 microseconds an operation on the Pocket, and
 * a few of a game's scripts run thousands of operations a frame to do very
 * little. Such a script is written out in C, operation for operation: the
 * same arithmetic in the same order and precision, the same calls, the same
 * variables left behind. Each game lists its own in its game_<name>.c, with
 * the length and hash of the bytecode it was written against; here they are
 * installed by VM_setNativeCode, which checks that hash, so any other version
 * of the game simply runs its own bytecode.
 *
 * To add one: disassemble the entry (BS_DISASM=<name>), write it out in the
 * game's file, get the length and hash from BS_CODEHASH=<name> on the desktop
 * build, and compare frames with and without it (BS_NO_NATIVE=1). */

#include "of_native.h"

#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "of_game.h"
#include "vm.h"

void ofNativeInstall(Runner *runner) {
    VMContext *vm = runner->vmContext;
    if (vm == NULL) return;
    if (ofGame.nativesForgetVm != NULL) ofGame.nativesForgetVm(); /* a game_change gives a new VM, with ids of its own */
#ifdef OF_PC
    /* BS_CODEHASH=<code entry> prints what a table row needs; BS_NO_NATIVE=1 leaves the bytecode in charge. */
    const char *ask = getenv("BS_CODEHASH");
    if (ask != NULL) {
        uint32_t length = 0;
        uint64_t hash = VM_codeHash(vm, ask, &length);
        printf("[butterscotch] %s: length %u, hash 0x%016llxull\n", ask, (unsigned) length, (unsigned long long) hash);
    }
    if (getenv("BS_NO_NATIVE") != NULL) return;
#endif
    if (ofGame.installCollisionFilter != NULL) ofGame.installCollisionFilter(runner);
    for (size_t n = 0; n < ofGame.nativeCount; n++) {
        uint32_t length = 0;
        if (VM_codeHash(vm, ofGame.natives[n].name, &length) == 0) continue; /* another game */
        bool installed = VM_setNativeCode(vm, ofGame.natives[n].name, ofGame.natives[n].length, ofGame.natives[n].hash, ofGame.natives[n].native);
        logInfo("Native: %s %s\n", ofGame.natives[n].name, installed ? "installed" : "left to the interpreter (bytecode differs)");
    }
    for (size_t n = 0; n < ofGame.nativeScriptCount; n++) {
        uint32_t length = 0;
        if (VM_codeHash(vm, ofGame.nativeScripts[n].name, &length) == 0) continue; /* another game */
        bool installed = VM_setNativeScript(vm, ofGame.nativeScripts[n].name, ofGame.nativeScripts[n].length, ofGame.nativeScripts[n].hash, ofGame.nativeScripts[n].native);
        logInfo("Native: %s %s\n", ofGame.nativeScripts[n].name, installed ? "installed" : "left to the interpreter (bytecode differs)");
    }
}
