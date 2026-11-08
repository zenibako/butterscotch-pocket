/* Compare the row fast path with the general per-pixel compositor, including
 * transparent destinations, odd alignment/length and clipped surface pitches. */
#include "sw/sw_drawing.c"
#include <stdio.h>

static unsigned long checks;
static bool negativeControl;

static void checkHalfBlend(void)
{
    // The packed average must never carry across channels or use the opacity bit.
    for (unsigned i = 0; i < 65536; i++) {
        for (unsigned c = 0; c < 32; c++) {
            uintpixel_t src = (uintpixel_t) (0x8000u | (c << 10) | ((31u - c) << 5) | c);
            uintpixel_t expected = (uintpixel_t) i;
            alphaBlend(&expected, src, bm_normal, 128);
            if (swrBlendHalf((uintpixel_t) i, src) != expected) {
                fprintf(stderr, "FAIL: half blend dst=%x src=%x\n", i, (unsigned) src);
                exit(1);
            }
            ++checks;
        }
    }
}

static void checkRow(int offset, int width, int alpha, uintpixel_t color, unsigned first)
{
    enum { COUNT = 260 };
    uintpixel_t actual[COUNT], expected[COUNT];
    for (unsigned i = 0; i < COUNT; i++)
        actual[i] = expected[i] = (uintpixel_t) (first + i);
    SWRenderer swr = {0};
    swr.fb = actual;
    swr.fbPitch = COUNT;
    swr.portX = offset;
    swr.maxX = offset + width;
    swr.maxY = 1;
    swr.blendMode = bm_normal;
    for (int i = offset; i < offset + width; i++)
        alphaBlend(&expected[i], color, bm_normal, alpha);
    swrDrawHLineInt(&swr.base, offset - 2, 0, width + 4, color, color, alpha);
    if (negativeControl) actual[offset] ^= 1;
    if (memcmp(actual, expected, sizeof(actual))) {
        fprintf(stderr, "FAIL: offset=%d width=%d alpha=%d color=%x first=%u\n",
                offset, width, alpha, color, first);
        exit(1);
    }
    checks += COUNT;
}

static void checkPitch(int pitch, int alpha, uintpixel_t color, int pattern)
{
    uintpixel_t actual[5 * 24], expected[5 * 24];
    for (unsigned i = 0; i < sizeof(actual) / sizeof(actual[0]); i++) {
        uintpixel_t under = pattern == 0 ? 0x8000 : pattern == 1 ? 0xffff :
                            pattern == 2 ? (i & 1 ? 0x7fff : 0x8421) : (uintpixel_t) (i * 7919);
        actual[i] = expected[i] = under;
    }
    SWRenderer swr = {0};
    swr.fb = actual;
    swr.fbPitch = (uint16_t) pitch;
    swr.portX = 2;
    swr.maxX = 21;
    swr.portY = 1;
    swr.maxY = 4;
    swr.blendMode = bm_normal;
    for (int y = 1; y < 4; y++)
        for (int x = 2; x < 21; x++)
            alphaBlend(&expected[y * pitch + x], color, bm_normal, alpha);
    for (int y = 0; y < 5; y++)
        swrDrawHLineInt(&swr.base, -1, y, 25, color, color, alpha);
    if (memcmp(actual, expected, sizeof(actual))) {
        fprintf(stderr, "FAIL: pitch=%d alpha=%d color=%x pattern=%d\n", pitch, alpha, color, pattern);
        exit(1);
    }
    checks += sizeof(actual) / sizeof(actual[0]);
}

int main(int argc, char** argv)
{
    (void) argv;
    checkHalfBlend();
    negativeControl = argc > 1;
    if (negativeControl) checkRow(1, 1, 133, 0x8000, 0);
    static const uintpixel_t colors[] = {0, 0x8000, 0xffff, 0xfc00, 0x83e0, 0x801f, 0x1234};
    /* Every 16-bit destination at every partial opacity, for each source. */
    for (unsigned c = 0; c < sizeof(colors) / sizeof(colors[0]); c++)
        for (int alpha = 4; alpha <= 253; alpha++)
            for (unsigned first = 0; first < 65536; first += 256)
                checkRow(2, 256, alpha, colors[c], first - 2);
    for (int offset = 1; offset <= 2; offset++)
        for (int width = 0; width < 20; width++)
            for (int alpha = 0; alpha <= 256; alpha++)
                checkRow(offset, width, alpha, 0x8d67, 0x7ff0);
    for (int pitch = 23; pitch <= 24; pitch++)
        for (int alpha = 0; alpha <= 256; alpha++)
            for (unsigned c = 0; c < sizeof(colors) / sizeof(colors[0]); c++)
                for (int pattern = 0; pattern < 4; pattern++)
                    checkPitch(pitch, alpha, colors[c], pattern);
    printf("solid-fill: %lu pixel/guard comparisons passed\n", checks);
    return 0;
}
