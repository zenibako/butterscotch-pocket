#include "of_bench_scene.h"

#include <string.h>

bool ofBenchSceneMatches(const Runner *runner, const OfBenchSection *section) {
    if (section->roomName == NULL && section->requiredObject == NULL) return true;
    if (runner == NULL || runner->currentRoom == NULL || runner->dataWin == NULL) return false;
    if (section->roomName != NULL &&
        (runner->currentRoom->name == NULL || strcmp(runner->currentRoom->name, section->roomName) != 0)) return false;
    if (section->requiredObject == NULL) return true;
    for (ptrdiff_t i = 0; i < arrlen(runner->instances); ++i) {
        const Instance *instance = runner->instances[i];
        if (instance == NULL || !instance->active || instance->destroyed || instance->objectIndex < 0 ||
            (uint32_t) instance->objectIndex >= runner->dataWin->objt.count) continue;
        const char *name = runner->dataWin->objt.objects[instance->objectIndex].name;
        if (name != NULL && strcmp(name, section->requiredObject) == 0) return true;
    }
    return false;
}
