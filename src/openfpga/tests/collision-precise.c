/* Asset-free real pair scan versus the unchanged scalar point helper.
 * Function instrumentation and injected math wrappers observe executed setup
 * only in this fixture; ordinary builds carry no diagnostic hooks. */
#include "../../math_compat.h"
#include "../../runner.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned maskChecks, absoluteChecks, cosineChecks, checks, workFailures;
static bool behaviorOnly;
static GMLReal observedAbs(GMLReal x) {
    ++absoluteChecks;
    return GMLReal_fabs(x);
}
static GMLReal observedCos(GMLReal x) {
    ++cosineChecks;
    return GMLReal_cos(x);
}
#undef GMLReal_fabs
#undef GMLReal_cos
#define GMLReal_fabs observedAbs
#define GMLReal_cos observedCos
#include "../../collision.h"

#if defined(ENABLE_WAD16) && !defined(ENABLE_WAD17)
#define FIXTURE_WAD 16
#elif defined(ENABLE_WAD17) && !defined(ENABLE_WAD16)
#define FIXTURE_WAD 17
#else
#error Build this fixture with exactly one WAD version
#endif

void __attribute__((no_instrument_function))
__cyg_profile_func_enter(void *function, void *caller) {
    (void)caller;
    if (function == (void *)Collision_hasFrameMasks) ++maskChecks;
}
void __attribute__((no_instrument_function))
__cyg_profile_func_exit(void *function, void *caller) {
    (void)function; (void)caller;
}

typedef struct {
    Runner runner;
    DataWin data;
    Sprite sprites[2];
    Instance a, b;
    uint8_t bits[2][2][8];
    uint8_t *masks[2][2];
} Fixture;

static void init(Fixture *f) {
    memset(f, 0, sizeof(*f));
    f->runner.dataWin = &f->data;
    f->data.gen8.wadVersion = FIXTURE_WAD;
    f->data.sprt.count = 2;
    f->data.sprt.sprites = f->sprites;
    for (int s = 0; s < 2; ++s) {
        Sprite *spr = &f->sprites[s];
        spr->width = spr->height = spr->maskWidth = spr->maskHeight = 4;
        spr->bboxMode = 1; spr->sepMasks = 1; spr->maskCount = 2;
        spr->masks = f->masks[s];
        for (int frame = 0; frame < 2; ++frame) {
            f->masks[s][frame] = f->bits[s][frame];
            memset(f->bits[s][frame], 0xf0, sizeof(f->bits[s][frame]));
        }
    }
    f->a.maskIndex = f->b.maskIndex = -1;
    f->b.spriteIndex = 1;
    f->a.imageXscale = f->a.imageYscale = f->b.imageXscale = f->b.imageYscale = 1;
}
static InstanceBBox box(GMLReal left, GMLReal top, GMLReal right, GMLReal bottom) {
    return (InstanceBBox){left, right, top, bottom, true};
}
static bool pair(Fixture *f, InstanceBBox a, InstanceBBox b) {
    return Collision_instancesOverlapPrecise(&f->runner, &f->a, &f->b, a, b);
}

/* Deliberately scalar sampling oracle. It shares only the public scalar point
 * helper, never any prepared transform/mask descriptor or pair implementation.
 * The generated cases always have at least one precise sprite (SAT is outside
 * this optimization); nonprecise fast gates have independent literal checks. */
static bool scalar(Fixture *f, InstanceBBox a, InstanceBBox b) {
    GMLReal left = GMLReal_fmax(a.left, b.left), right = GMLReal_fmin(a.right, b.right);
    GMLReal top = GMLReal_fmax(a.top, b.top), bottom = GMLReal_fmin(a.bottom, b.bottom);
    if (left >= right || top >= bottom) return false;
    Sprite *sa = Collision_getSprite(&f->data, &f->a);
    Sprite *sb = Collision_getSprite(&f->data, &f->b);
    if (!sa || !sb) return false;
    bool compat = f->runner.collisionCompatibilityMode;
    int x0 = compat ? (int)left : (int)GMLReal_floor(left);
    int y0 = compat ? (int)top : (int)GMLReal_floor(top);
    int x1 = compat ? (int)right + 1 : (int)GMLReal_ceil(right);
    int y1 = compat ? (int)bottom + 1 : (int)GMLReal_ceil(bottom);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            GMLReal wx = (GMLReal)x + (compat ? 0.0 : 0.5);
            GMLReal wy = (GMLReal)y + (compat ? 0.0 : 0.5);
            if (Collision_pointInInstance(sa, &f->a, wx, wy) &&
                Collision_pointInInstance(sb, &f->b, wx, wy)) return true;
        }
    return false;
}
static void expect(Fixture *f, InstanceBBox a, InstanceBBox b, bool want) {
    assert(scalar(f, a, b) == want);
    assert(pair(f, a, b) == want);
    ++checks;
}
static void literals(void) {
    Fixture f; init(&f);
    InstanceBBox all = box(0, 0, 4, 4);
    expect(&f, all, all, true);
    memset(f.bits[0], 0, sizeof(f.bits[0]));
    expect(&f, all, all, false);
    f.bits[0][0][3] = 0x10; /* Only local (3,3), after fifteen misses. */
    expect(&f, all, all, true);
    f.a.imageIndex = 1;
    expect(&f, all, all, false);
    f.a.imageIndex = 2; /* Frame wraps back to zero. */
    expect(&f, all, all, true);
    expect(&f, all, box(4, 0, 8, 4), false);
    f.a.spriteIndex = -1;
    expect(&f, all, all, false);
    f.a.spriteIndex = 0; f.a.maskIndex = 1;
    expect(&f, all, all, true);

    init(&f);
    assert(Collision_pointInInstance(&f.sprites[0], &f.a, -0.5, 0.5));
    assert(!Collision_pointInInstance(&f.sprites[0], &f.a, -1, 0.5));
    assert(!Collision_pointInInstance(&f.sprites[0], &f.a, 4, 0.5));
    f.sprites[0].maskOffsetX = f.sprites[0].maskOffsetY = 1;
    f.sprites[0].maskWidth = f.sprites[0].maskHeight = 2;
    memset(f.bits[0], 0, sizeof(f.bits[0])); f.bits[0][0][0] = 0x80;
    expect(&f, box(0, 0, 1, 1), all, false);
    expect(&f, box(1, 1, 2, 2), all, true);
    expect(&f, box(2, 1, 3, 2), all, false);

    init(&f);
    f.sprites[0].width = f.sprites[1].width = 10;
    f.sprites[0].maskOffsetX = f.sprites[0].maskOffsetY = 1;
    f.sprites[0].maskWidth = 9; f.sprites[0].maskHeight = 2;
    f.sprites[1].masks = NULL;
    memset(f.bits[0], 0, sizeof(f.bits[0])); f.bits[0][0][3] = 0x80;
    /* Cropped local (9,2) uses row one, byte one: stride rounds 9 bits to 2. */
    expect(&f, box(9, 2, 10, 3), box(0, 0, 10, 4), true);
    expect(&f, box(8, 2, 9, 3), box(0, 0, 10, 4), false);
    expect(&f, box(10, 2, 11, 3), box(0, 0, 12, 4), false);

    init(&f);
    memset(f.bits[0], 0, sizeof(f.bits[0])); f.bits[0][0][0] = 0x40;
    f.a.x = 0.75;
    expect(&f, box(1, 0, 2, 1), all, false); /* Centers see local x=0.75. */
    f.runner.collisionCompatibilityMode = true;
    expect(&f, box(1, 0, 2, 1), all, true); /* Inclusive corner x=2 sees local 1.25. */

    init(&f); f.a.imageXscale = -1; f.a.x = 4;
    expect(&f, all, all, true);
    f.a.imageXscale = 0;
    expect(&f, all, all, false);
    f.a.imageXscale = 0.00005;
    expect(&f, all, all, false);
    /* Missing masks take the existing nonprecise AABB fast path, even at
     * zero scale; preparing transforms before this gate would change it. */
    f.sprites[0].masks = f.sprites[1].masks = NULL;
    assert(pair(&f, all, all)); ++checks;
    f.sprites[0].masks = f.masks[0]; f.sprites[0].maskCount = 0;
    assert(pair(&f, all, all)); ++checks;
}
static void differential(void) {
    const GMLReal angles[] = {0, 0.00005, -0.00005, 0.0001, -0.0001, 0.0002, -0.0002, 30, -90, 180};
    const GMLReal scales[] = {0, 0.00005, 0.0001, -0.0001, 0.0002, -1, 0.75, 1, 1.5};
    InstanceBBox scan = box(-2.25, -1.75, 5.25, 5.75);
    for (int compat = 0; compat < 2; ++compat)
    for (int angle = 0; angle < (int)(sizeof(angles)/sizeof(*angles)); ++angle)
    for (int scale = 0; scale < (int)(sizeof(scales)/sizeof(*scales)); ++scale)
    for (int variant = 0; variant < 16; ++variant) {
        Fixture f; init(&f);
        f.runner.collisionCompatibilityMode = compat;
        f.a.x = 0.25; f.a.y = -0.75; f.b.x = -0.25; f.b.y = 0.75;
        f.a.imageAngle = angles[angle]; f.b.imageAngle = -angles[(angle + 3) % 10];
        f.a.imageXscale = scales[scale]; f.a.imageYscale = scales[(scale + 2) % 9];
        f.b.imageXscale = scales[(scale + 4) % 9]; f.b.imageYscale = scales[(scale + 6) % 9];
        f.sprites[0].originX = 2; f.sprites[1].originY = 1;
        f.a.imageIndex = variant + 0.75; f.b.imageIndex = variant + 1.25;
        for (int s = 0; s < 2; ++s)
            for (int frame = 0; frame < 2; ++frame)
                for (int y = 0; y < 4; ++y) f.bits[s][frame][y] = (uint8_t)(0xa0 ^ (y * 0x30) ^ (frame * 0xf0) ^ (s * 0x50));
        if (variant & 1) { f.sprites[0].maskOffsetX = 1; f.sprites[0].maskOffsetY = 1;
            f.sprites[0].maskWidth = 2; f.sprites[0].maskHeight = 2; }
        if (variant & 2) f.sprites[1].masks = NULL;
        if (variant & 4) f.sprites[1].sepMasks = 2;
        if (variant & 8) { f.sprites[1].maskCount = 0; f.sprites[0].maskOffsetY = -1; }
        assert(pair(&f, scan, scan) == scalar(&f, scan, scan)); ++checks;
        /* Also exercise actual rounded/fractional/negative-scale AABBs. */
        InstanceBBox a = Collision_computeBBox(&f.runner, &f.a);
        InstanceBBox b = Collision_computeBBox(&f.runner, &f.b);
        assert(pair(&f, a, b) == scalar(&f, a, b)); ++checks;
    }
}
static void bound(const char *name, unsigned masks, unsigned absolute, unsigned cosine) {
    printf("%s: mask classifications=%u abs=%u cos=%u\n", name, maskChecks, absoluteChecks, cosineChecks);
    if (!behaviorOnly && (maskChecks > masks || absoluteChecks > absolute || cosineChecks > cosine)) ++workFailures;
}
static void resetCounts(void) { maskChecks = absoluteChecks = cosineChecks = 0; }
static void work(void) {
    Fixture f; init(&f); InstanceBBox all = box(0, 0, 4, 4);
    memset(f.bits[1], 0, sizeof(f.bits[1]));
    resetCounts(); assert(!pair(&f, all, all));
    bound("full scan", 2, 6, 0);
    init(&f);
    resetCounts(); assert(pair(&f, all, all));
    bound("first-hit return", 2, 6, 0);
    memset(f.bits[1], 0, sizeof(f.bits[1]));
    f.a.imageAngle = 15; f.b.imageAngle = -15;
    resetCounts(); assert(!pair(&f, all, all));
    bound("rotated scan", 2, 6, 2);
    memset(f.bits[0], 0, sizeof(f.bits[0])); f.b.imageXscale = 0;
    resetCounts(); assert(!pair(&f, all, all));
    bound("A misses: B transform stays cold", 2, 3, 1);
    f.a.imageXscale = 0;
    resetCounts(); assert(!pair(&f, all, all));
    bound("degenerate A: rotations stay cold", 2, 1, 0);
    resetCounts(); assert(!pair(&f, all, box(4, 0, 8, 4)));
    bound("AABB rejection: both sides stay cold", 0, 0, 0);
    /* Invalid storage must remain unread until full-sprite and crop bounds
     * accept a point. Cropped misses must not read pixel storage. */
    init(&f); f.sprites[0].masks = (uint8_t **)(uintptr_t)1;
    f.a.x = 100;
    assert(!pair(&f, all, all));
    f.a.x = 0; f.sprites[0].maskOffsetX = 10;
    f.sprites[0].masks = f.masks[0]; f.masks[0][0] = f.masks[0][1] = NULL;
    assert(!pair(&f, all, all));
}
#if defined(OF_FAST_LOOP) && defined(USE_FLOAT_REALS)
static void rotationCache(void) {
    Fixture f;
    init(&f);

    resetCounts();
    CollisionRotation first = Collision_rotation(&f.runner, 4.0f);
    CollisionRotation repeated = Collision_rotation(&f.runner, 4.0f);
    assert(first.cosine == repeated.cosine && first.sine == repeated.sine);
    bound("same angle reuses trig", 0, 0, 1);

    // Four and five degrees address the same slot. Replacing an entry
    // must still produce the exact coefficients for both angles.
    resetCounts();
    CollisionRotation other = Collision_rotation(&f.runner, 5.0f);
    CollisionRotation replaced = Collision_rotation(&f.runner, 4.0f);
    assert(other.cosine == cosf((float)(5.0 * M_PI / 180.0)));
    assert(other.sine == sinf((float)(5.0 * M_PI / 180.0)));
    assert(first.cosine == replaced.cosine && first.sine == replaced.sine);
    bound("different angle replaces trig", 0, 0, 2);
    checks += 4;
}
#endif
int main(int argc, char **argv) {
    behaviorOnly = argc > 1 && !strcmp(argv[1], "--behavior-only");
    literals(); differential(); work();
#if defined(OF_FAST_LOOP) && defined(USE_FLOAT_REALS)
    rotationCache();
#endif
    printf("WAD%d: %u behavior checks, %u work-bound failures\n", FIXTURE_WAD, checks, workFailures);
    return workFailures ? 1 : 0;
}
