/* Scene contracts accept only the requested room and live object instances.
 * Invalid object indices and unconstrained NULL runners must be safe. */
#include "of_bench_scene.h"
#include "stb_ds.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    Runner runner = {0};
    Room room = {0};
    DataWin dataWin = {0};
    GameObject objects[2] = {0};
    Instance matching = {0};
    Instance other = {0};

    room.name = "room_battle";
    objects[0].name = "obj_star";
    objects[1].name = "obj_other";
    dataWin.objt.count = 2;
    dataWin.objt.objects = objects;
    runner.currentRoom = &room;
    runner.dataWin = &dataWin;

    matching.active = true;
    matching.objectIndex = 0;
    other.active = true;
    other.objectIndex = 1;
    arrput(runner.instances, &matching);
    arrput(runner.instances, NULL);
    arrput(runner.instances, &other);
    Instance **instances = runner.instances;

    OfBenchSection section = { "attack", 900, "atk1", "room_battle", "obj_star", NULL };
    assert(ofBenchSceneMatches(&runner, &section));
    instances[0]->active = false;
    assert(!ofBenchSceneMatches(&runner, &section));

    instances[0]->active = true;
    instances[0]->destroyed = true;
    assert(!ofBenchSceneMatches(&runner, &section));
    instances[0]->destroyed = false;

    section.roomName = "room_gameover";
    assert(!ofBenchSceneMatches(&runner, &section));
    section.roomName = "room_battle";

    section.requiredObject = "obj_missing";
    assert(!ofBenchSceneMatches(&runner, &section));
    section.requiredObject = "obj_star";
    instances[0]->objectIndex = -1;
    assert(!ofBenchSceneMatches(&runner, &section));
    instances[0]->objectIndex = (int32_t) dataWin.objt.count;
    assert(!ofBenchSceneMatches(&runner, &section));
    instances[0]->objectIndex = 0;

    runner.currentRoom = NULL;
    assert(!ofBenchSceneMatches(&runner, &section));
    runner.currentRoom = &room;
    section.requiredObject = NULL;
    assert(ofBenchSceneMatches(&runner, &section));
    section.requiredObject = "obj_star";
    section.roomName = NULL;
    assert(ofBenchSceneMatches(&runner, &section));

    section.requiredObject = NULL;
    section.roomName = NULL;
    assert(ofBenchSceneMatches(NULL, &section));

    arrfree(runner.instances);
    puts("PASS: benchmark scene room and live-object contracts");
    return 0;
}
