/* Undertale's benchmark routes: the scripted inputs, room jumps and timed
 * sections behind --bench, --bench-battles, the bullet-hell, Chaos Saber,
 * Photoshop Flowey and room 57 snow runs, and the helpers that decide when
 * a section is live. Included by game_undertale.c, whose ofGame lists them. */

/* Room 57 (room_tundra_snowpuzz), whose 16 obj_snowfloor instances draw and
 * test 400 snow dots every frame. The run jumps there from the intro, turns
 * random encounters off once the room's encounterer has reset its count,
 * stands with the patch out of view, walks down through it (the dots part
 * and the hidden switch shows) and back up, then stands with the patch in
 * view. The route is the one the desktop checks of the snow patch native
 * use, 350 frames later. */
static const char g_snowScript[] =
    "420:L*6,430:D*95,530:U*20,555:R*8,568:D*12,585:L*10,600:U*40";

static const OfJump g_snowJumps[] = {
    { 60, 57, NULL, NULL },
    { 70, -1, "encounter=-100000", NULL },
};

static const OfBenchSection g_snowSections[] = {
    { "boot and intro", 60, "boot", NULL, NULL, NULL },
    { "room 57 entry", 120, "entry", "room_tundra_snowpuzz", "obj_snowfloor", NULL },
    { "standing, patch out of view", 420, "stand", "room_tundra_snowpuzz", "obj_snowfloor", NULL },
    { "walking through the patch", 640, "walk", "room_tundra_snowpuzz", "obj_snowfloor", NULL },
    { "standing, patch in view", 820, "near", "room_tundra_snowpuzz", "obj_snowfloor", NULL },
};

/* Key presses that walk from boot to the first battle; see runInputScript()
 * in of_platform.c for the syntax. Frame numbers assume a fresh save state. */
static const char g_script[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z,700:R*400,1100:L*10,1120:U*400,1560:Z,1605:Z,1650:Z,1695:Z,1740:Z,1785:Z,1830:Z,1875:Z,1920:Z,1965:Z,2010:Z,2055:Z,2100:Z,2145:Z,2190:Z,2235:Z,2280:Z,2325:Z,2370:Z,2415:Z,2460:Z,2505:Z,2550:Z,2595:Z,2640:Z,2685:Z";

/* Sections end on these frames; they follow the script above. */
static const OfBenchSection g_sections[] = {
    { "intro, menu, naming", 650, "intro", NULL, NULL, NULL },
    { "first room 320x240", 1220, "room", NULL, NULL, NULL },
    { "Flowey talk 320x240", 2060, "talk", NULL, NULL, NULL },
    { "Flowey battle (640x480)", 2600, "battl", NULL, NULL, NULL },
};

/* The battle tour ("--bench-battles"): a walk to the first room, then six
 * battles from across the game, each entered and left the way the game does
 * it, so its music starts and stops as in play. From a Ruins room (12, with
 * its music) the player walks a few steps, then the tour does what an
 * overworld encounter does (global.battlegroup and border set, an
 * obj_battleblcon made): the "!", the heart and the flash into room_battle.
 * The battle runs for about 350 frames with Z every 40 frames and the heart
 * moving between, HP kept topped up so no battle ends in a game over. Then
 * global.monster[0..2] is cleared, which is the game's own win: "YOU WON",
 * the battle music stopped, and a confirm press back to the Ruins room with
 * its music resumed. Each section is one visit, overworld included. Each
 * visit starts with a jump to the Ruins room, which changes nothing when the
 * last battle ended in time, but keeps a battle that did not from making the
 * next encounter inside room_battle (the game would then loop on it). A win
 * this way never takes a boss's own ending (Toriel's kill path writes
 * undertale.ini), and global.mercy, which some bosses raise to stop fleeing,
 * is reset before each encounter. Late bosses are entered without their
 * story set up, so they show their opening turns, not a real playthrough. */
static const char g_tourScript[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z,"
    "700:R*200,1030:L*20,1060:R*20,1160:Z,1168:U*20,1200:Z,1208:D*20,1240:Z,1248:L*20,1280:Z,1288:R*20,"
    "1320:Z,1328:U*20,1360:Z,1368:D*20,1400:Z,1408:L*20,1440:Z,1448:R*20,1480:Z,1488:U*20,1530:Z,1545:Z,"
    "1560:Z,1630:L*20,1660:R*20,1760:Z,1768:U*20,1800:Z,1808:D*20,1840:Z,1848:L*20,1880:Z,1888:R*20,1920:Z,"
    "1928:U*20,1960:Z,1968:D*20,2000:Z,2008:L*20,2040:Z,2048:R*20,2080:Z,2088:U*20,2130:Z,2145:Z,2160:Z,"
    "2230:L*20,2260:R*20,2360:Z,2368:U*20,2400:Z,2408:D*20,2440:Z,2448:L*20,2480:Z,2488:R*20,2520:Z,"
    "2528:U*20,2560:Z,2568:D*20,2600:Z,2608:L*20,2640:Z,2648:R*20,2680:Z,2688:U*20,2730:Z,2745:Z,2760:Z,"
    "2830:L*20,2860:R*20,2960:Z,2968:U*20,3000:Z,3008:D*20,3040:Z,3048:L*20,3080:Z,3088:R*20,3120:Z,"
    "3128:U*20,3160:Z,3168:D*20,3200:Z,3208:L*20,3240:Z,3248:R*20,3280:Z,3288:U*20,3330:Z,3345:Z,3360:Z,"
    "3430:L*20,3460:R*20,3560:Z,3568:U*20,3600:Z,3608:D*20,3640:Z,3648:L*20,3680:Z,3688:R*20,3720:Z,"
    "3728:U*20,3760:Z,3768:D*20,3800:Z,3808:L*20,3840:Z,3848:R*20,3880:Z,3888:U*20,3930:Z,3945:Z,3960:Z,"
    "4030:L*20,4060:R*20,4160:Z,4168:U*20,4200:Z,4208:D*20,4240:Z,4248:L*20,4280:Z,4288:R*20,4320:Z,"
    "4328:U*20,4360:Z,4368:D*20,4400:Z,4408:L*20,4440:Z,4448:R*20,4480:Z,4488:U*20,4530:Z,4545:Z,4560:Z";

static const OfJump g_tourJumps[] = {
    { 1000, 12, "encounter=0", NULL }, /* the Ruins room */
    { 1100, -1, "battlegroup=4,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Froggit */
    { 1250, -1, "hp=99", NULL },
    { 1350, -1, "hp=99", NULL },
    { 1450, -1, "hp=99", NULL },
    { 1500, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 1600, 12, "encounter=0", NULL },
    { 1700, -1, "battlegroup=22,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Toriel */
    { 1850, -1, "hp=99", NULL },
    { 1950, -1, "hp=99", NULL },
    { 2050, -1, "hp=99", NULL },
    { 2100, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 2200, 12, "encounter=0", NULL },
    { 2300, -1, "battlegroup=27,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Papyrus */
    { 2450, -1, "hp=99", NULL },
    { 2550, -1, "hp=99", NULL },
    { 2650, -1, "hp=99", NULL },
    { 2700, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 2800, 12, "encounter=0", NULL },
    { 2900, -1, "battlegroup=47,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Undyne */
    { 3050, -1, "hp=99", NULL },
    { 3150, -1, "hp=99", NULL },
    { 3250, -1, "hp=99", NULL },
    { 3300, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 3400, 12, "encounter=0", NULL },
    { 3500, -1, "battlegroup=81,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Mettaton EX */
    { 3650, -1, "hp=99", NULL },
    { 3750, -1, "hp=99", NULL },
    { 3850, -1, "hp=99", NULL },
    { 3900, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
    { 4000, 12, "encounter=0", NULL },
    { 4100, -1, "battlegroup=101,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" }, /* Asgore */
    { 4250, -1, "hp=99", NULL },
    { 4350, -1, "hp=99", NULL },
    { 4450, -1, "hp=99", NULL },
    { 4500, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99", NULL }, /* won */
};

static const OfBenchSection g_tourSections[] = {
    { "boot to the first room", 1000, "boot", NULL, NULL, NULL },
    { "Froggit (640x480)", 1599, "frog", NULL, NULL, NULL },
    { "Toriel (640x480)", 2199, "tori", NULL, NULL, NULL },
    { "Papyrus (640x480)", 2799, "papy", NULL, NULL, NULL },
    { "Undyne (640x480)", 3399, "undy", NULL, NULL, NULL },
    { "Mettaton EX (640x480)", 3999, "mtex", NULL, NULL, NULL },
    { "Asgore (640x480)", 4599, "asgr", NULL, NULL, NULL },
};

/* The bullet-hell tour retains the battle tour's 600-frame Asgore
 * encounter/win control, then enters Asriel through the same overworld
 * encounter object. Only HP and menu selection are replenished/controlled;
 * Asriel's ordinary turns, objects and timers remain in control. Named setup
 * sections cover the encounter and later turns, keeping the two 150-frame
 * attack sections entirely inside their natural projectile sequences. */
static const char g_bulletScript[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z,"
    "700:R*200,1030:L*20,1060:R*20,1160:Z,1168:U*20,1200:Z,1208:D*20,1240:Z,1248:L*20,1280:Z,1288:R*20,"
    "1320:Z,1328:U*20,1360:Z,1368:D*20,1400:Z,1408:L*20,1440:Z,1448:R*20,1480:Z,1488:U*20,1530:Z,1545:Z,1560:Z,"
    "1760:Z,1800:Z,1840:Z,1880:Z,1920:Z,1960:Z,2000:Z,2040:Z,2080:Z,2120:Z,2160:Z,2200:Z,2240:Z,2280:Z,"
    "2320:Z,2360:Z,2400:Z,2440:Z,2480:Z,2520:Z,2560:Z,2600:Z,2640:Z,2680:Z,2720:Z,2760:Z,2800:Z,2840:Z,"
    "2880:Z,2920:Z,2960:Z,3000:Z,3040:Z,3080:Z,3120:Z,3160:Z,3200:Z,3240:Z,3280:Z,3320:Z,3360:Z,3400:Z,"
    "3440:Z,3480:Z,3520:Z,3560:Z,3600:Z,3640:Z,3680:Z,3720:Z,3760:Z,3800:Z,3840:Z,3880:Z,3920:Z,3960:Z,"
    "4000:Z,4040:Z,4080:Z,4120:Z,4160:Z,4200:Z,4240:Z,4280:Z,4320:Z,4360:Z,4400:Z,4440:Z,4480:Z,4520:Z,4560:Z,"
    "4600:Z,4640:Z,4680:Z,4720:Z,4760:Z,4800:Z,4840:Z,4880:Z,4920:Z,4960:Z,5000:Z,5040:Z,5080:Z,5120:Z,"
    "5160:Z,5200:Z,5240:Z,5280:Z,5320:Z,5360:Z,5400:Z,5440:Z,5480:Z,5520:Z,5560:Z,5600:Z";

#define ASRIEL_ENCOUNTER_GLOBALS \
    "battlegroup=256,border=0,mercy=0,hp=99,maxhp=99,encounter=0," \
    "flag[500]=1,flag[501]=0,flag[505]=0,flag[506]=0,flag[507]=0,flag[508]=0"
#define BULLET_HP_AT(f) { (f), -1, "hp=99,maxhp=99,bmenucoord[0]=1", NULL },
#define BULLET_HP_1000(f) \
    BULLET_HP_AT((f)) BULLET_HP_AT((f) + 100) BULLET_HP_AT((f) + 200) BULLET_HP_AT((f) + 300) \
    BULLET_HP_AT((f) + 400) BULLET_HP_AT((f) + 500) BULLET_HP_AT((f) + 600) BULLET_HP_AT((f) + 700) \
    BULLET_HP_AT((f) + 800) BULLET_HP_AT((f) + 900)

static const OfJump g_bulletJumps[] = {
    { 1000, 12, "encounter=0", NULL },
    { 1100, -1, "battlegroup=101,border=0,mercy=0,hp=99,maxhp=99,encounter=0", "obj_battleblcon" },
    { 1250, -1, "hp=99,maxhp=99", NULL },
    { 1350, -1, "hp=99,maxhp=99", NULL },
    { 1450, -1, "hp=99,maxhp=99", NULL },
    { 1500, -1, "monster[0]=0,monster[1]=0,monster[2]=0,hp=99,maxhp=99", NULL },
    { 1600, 12, "encounter=0", NULL },
    { 1700, -1, ASRIEL_ENCOUNTER_GLOBALS, "obj_battleblcon" },
    BULLET_HP_1000(1800)
    BULLET_HP_1000(2800)
    BULLET_HP_1000(3800)
    BULLET_HP_AT(4800)
    BULLET_HP_AT(4900)
    BULLET_HP_AT(5000)
    BULLET_HP_AT(5100)
    BULLET_HP_AT(5200)
    BULLET_HP_AT(5300)
    BULLET_HP_AT(5400)
    BULLET_HP_AT(5500)
    BULLET_HP_AT(5600)
};
#undef BULLET_HP_1000
#undef BULLET_HP_AT

static const OfBenchSection g_bulletSections[] = {
    { "boot to the first room", 1000, "boot", NULL, NULL, NULL },
    { "Asgore control (640x480)", 1600, "asgr", "room_battle", "obj_asgoreb", NULL },
    { "Asriel encounter setup", 1920, "aset", "room_battle", NULL, NULL },
    { "Asriel Chaos Saber", 2070, "saber", "room_battle", "obj_ultimagen", NULL },
    { "Lost Soul transition", 5480, "trans", "room_battle", NULL, NULL },
    { "Lost Soul green spears", 5630, "spear", "room_battle", "obj_greenspeargen", NULL },
    /* Not an attack: keeps the final report overlay out of the 5629 probe. */
    { "benchmark capture tail", 5631, "tail", "room_battle", NULL, NULL },
};

/* Undertale v1.08: enter room_battle after the initial room has initialized.
 * Confirm through the opening menus, then let the natural turn play out. */
static const char g_saberScript[] =
    "4:Z,8:Z,12:Z,16:Z,20:Z,24:Z,28:Z,32:Z,36:Z,40:Z,44:Z,"
    "48:Z,52:Z,56:Z,60:Z,64:Z,68:Z,72:Z,76:Z,80:Z,84:Z";

static const OfJump g_saberJumps[] = {
    { 2, 306, ASRIEL_ENCOUNTER_GLOBALS, NULL },
};
#undef ASRIEL_ENCOUNTER_GLOBALS

static GMLReal benchGlobal(const Runner *runner, const char *name) {
    VMContext *vm = runner->vmContext;
    int32_t id = VM_getOrAllocateVarID(vm, name);
    return RValue_toReal(Instance_getSelfVar(vm->globalScopeInstance, id));
}

static bool saberTurnEnds(const Runner *runner) {
    /* The final attack frame enters Draw with one tick left. */
    return benchGlobal(runner, "turntimer") == 0 && benchGlobal(runner, "hp") > 0;
}

static const OfBenchSection g_saberSections[] = {
    { "Asriel encounter setup", 82, "aset", "room_battle", "obj_asrielfinal", NULL },
    { "Asriel Chaos Saber", 232, "saber", "room_battle", "obj_ultimagen", saberTurnEnds },
};

/* Undertale v1.08: room_floweyx's controller loads phase fplot - 1.
 * Let each checkpoint finish its setup, then measure the native attacks
 * for equal windows that end before the stationary player dies. */
static const char g_floweyScript[] =
    "30:Z,100:Z,160:Z,220:Z,230:D,238:D,246:D,254:D,262:D,270:D,278:D,286:D,300:R,308:R,330:Z,370:R,380:Z";

enum { FLOWEY_VINE_ENTRY = 900, FLOWEY_FINALE_ENTRY = 1430 };
static const OfJump g_floweyJumps[] = {
    { FLOWEY_VINE_ENTRY, 294, "fplot=3", NULL },
    { FLOWEY_FINALE_ENTRY, 294, "fplot=7", NULL },
};

/* The first composite lands at entry + 2. The rest of the fade ends
 * before +14, and the TV laugh begins at +33 and runs for 180 updates.
 * Both checkpoints use this same authored intro. */
static const OfBenchOpeningWindow g_floweyOpening[] = {
    { "cold",   FLOWEY_VINE_ENTRY,      FLOWEY_VINE_ENTRY + 3 },
    { "fade",   FLOWEY_VINE_ENTRY + 3,  FLOWEY_VINE_ENTRY + 14 },
    { "hold",   FLOWEY_VINE_ENTRY + 14, FLOWEY_VINE_ENTRY + 33 },
    { "laugh",  FLOWEY_VINE_ENTRY + 33, FLOWEY_VINE_ENTRY + 213 },
    { "return", FLOWEY_FINALE_ENTRY,      FLOWEY_FINALE_ENTRY + 3 },
    { "fade2",  FLOWEY_FINALE_ENTRY + 3,  FLOWEY_FINALE_ENTRY + 14 },
    { "hold2",  FLOWEY_FINALE_ENTRY + 14, FLOWEY_FINALE_ENTRY + 33 },
    { "laugh2", FLOWEY_FINALE_ENTRY + 33, FLOWEY_FINALE_ENTRY + 213 },
};

static bool floweyPhaseActive(const Runner *runner, GMLReal phase) {
    return benchGlobal(runner, "battlephase") == phase && benchGlobal(runner, "my_hp") > 0;
}

static bool floweyVinesActive(const Runner *runner) {
    return floweyPhaseActive(runner, 2);
}

static bool floweyFinaleActive(const Runner *runner) {
    return floweyPhaseActive(runner, 6);
}

static const OfBenchSection g_floweySections[] = {
    { "Boot and vine setup", 1260, "vset", "room_floweyx", "obj_flowey_master", NULL },
    { "Flowey vine pressure", 1420, "vines", "room_floweyx", "obj_gigavine", floweyVinesActive },
    { "Finale checkpoint setup", 1830, "fset", "room_floweyx", "obj_flowey_master", NULL },
    { "Flowey finale eye shots", 1990, "eyes", "room_floweyx", "obj_eyeshot_a", floweyFinaleActive },
};
