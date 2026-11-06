#include "sw_renderer_private.h"
#include "sw_drawing.h"
#include <stdio.h>

enum { TEX_SIZE = 8, FRAME_SIZE = 12, REFERENCE_SIZE = 32, REFERENCE_ORIGIN = 8 };
static const uintpixel_t background = 0x8c63;
static unsigned checked;

static void setup(SWRenderer* renderer, int size)
{
    memset(renderer, 0, sizeof(*renderer));
    renderer->mainWidth = renderer->width = (uint16_t) size;
    renderer->mainHeight = renderer->height = (uint16_t) size;
    renderer->mainPitch = renderer->fbPitch = (uint16_t) size;
    renderer->portW = renderer->maxX = size;
    renderer->portH = renderer->maxY = size;
    renderer->blendMode = bm_normal;
    renderer->writeMask = WRITE_MASK_ALL;
    renderer->scaleX = renderer->scaleY = 1.0f;
    renderer->mainFb = renderer->fb = malloc((size_t) size * size * sizeof(uintpixel_t));
    if (!renderer->fb) abort();
}

static bool checkDraw(SWRenderer* reference, SWRenderer* clipped, SWTexture* texture,
                      int left, int top, float scale, bool flipX, bool flipY, float alpha)
{
    int size = (int) (TEX_SIZE * scale);
    float width = flipX ? (float) -size : (float) size;
    float height = flipY ? (float) -size : (float) size;
    swrFillPixels(reference->fb, REFERENCE_SIZE * REFERENCE_SIZE, background);
    swrFillPixels(clipped->fb, FRAME_SIZE * FRAME_SIZE, background);

    // Render the complete sprite without clipping, then compare a crop of it.
    swrDrawSprite((Renderer*) reference,
                  (float) (REFERENCE_ORIGIN + (flipX ? size : 0)),
                  (float) (REFERENCE_ORIGIN + (flipY ? size : 0)),
                  width, height, texture, 0, 0, TEX_SIZE, TEX_SIZE, 0xffffff, alpha);
    swrOverlayFlush(reference);
    bool visible = false;
    for (int i = 0; i < REFERENCE_SIZE * REFERENCE_SIZE; ++i)
        if (reference->fb[i] != background) visible = true;
    if (!visible) {
        fprintf(stderr, "FAIL: unclipped reference drew no pixels\n");
        return false;
    }
    swrDrawSprite((Renderer*) clipped, (float) (left + (flipX ? size : 0)),
                  (float) (top + (flipY ? size : 0)), width, height,
                  texture, 0, 0, TEX_SIZE, TEX_SIZE, 0xffffff, alpha);
    swrOverlayFlush(clipped);

    for (int y = 0; y < FRAME_SIZE; ++y) {
        for (int x = 0; x < FRAME_SIZE; ++x) {
            uintpixel_t expected = background;
            if (x >= 2 && x < 10 && y >= 2 && y < 10 &&
                x >= left && x < left + size && y >= top && y < top + size)
                expected = reference->fb[(REFERENCE_ORIGIN + y - top) * REFERENCE_SIZE +
                                         REFERENCE_ORIGIN + x - left];
            if (clipped->fb[y * FRAME_SIZE + x] != expected) {
                fprintf(stderr, "FAIL: clipped sprite (%d,%d) scale %.1f flip %d/%d alpha %.2f pixel %d,%d\n",
                        left, top, (double) scale, flipX, flipY, (double) alpha, x, y);
                return false;
            }
        }
    }
    ++checked;
    return true;
}

static bool checkSparseHalfDraw(void)
{
    enum { WIDTH = 144, HEIGHT = 3, LEFT = 7, RIGHT = 142 };
    const uintpixel_t under = 0xfc00, ink = 0x83e0, halfInk = 0xbde0;
    uintpixel_t pixels[WIDTH * HEIGHT * 4] = { 0 };
    SWTexture texture = { 0 };
    texture.buffer = pixels;
    texture.width = WIDTH * 2;
    texture.height = HEIGHT * 2;
    texture.immutable = true;
    for (int x = 0; x < WIDTH; x++) {
        if ((x >= 5 && x < 14) || x >= 139) {
            pixels[x * 2] = pixels[x * 2 + 1] = ink;
            pixels[WIDTH * 2 + x * 2] = pixels[WIDTH * 2 + x * 2 + 1] = ink;
        }
    }
    pixels[80] = pixels[81] = ink; // a half-covered pixel between the two clipped runs
    SWRenderer renderer;
    setup(&renderer, WIDTH);
    renderer.portX = renderer.viewX = LEFT;
    renderer.portW = RIGHT - LEFT;
    renderer.maxX = RIGHT;
    swrFillPixels(renderer.fb, WIDTH * WIDTH, under);
    swrFavorSpeed = swrSmoothMinify = true;
    swrDrawSprite((Renderer*) &renderer, 0, 0, WIDTH, HEIGHT, &texture,
                  0, 0, texture.width, texture.height, 0xffffff, 1.0f);
    swrOverlayFlush(&renderer);
    bool ok = texture.halfSpans != NULL;
    if (!ok) fprintf(stderr, "FAIL: sparse half-size sprite did not cache its runs\n");
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            uintpixel_t expected = under;
            if (y == 0 && x >= LEFT && x < RIGHT) {
                if (x < 14 || x >= 139) expected = ink;
                else if (x == 40) expected = halfInk;
            }
            if (renderer.fb[y * WIDTH + x] != expected) {
                fprintf(stderr, "FAIL: sparse half-size sprite pixel %d,%d\n", x, y);
                ok = false;
            }
        }
    }
    free(texture.halfSpans);
    free(texture.halfBuffer);
    free(renderer.fb);
    return ok;
}

int main(void)
{
    SWRenderer reference, clipped;
    SWTexture texture = { 0 };
    texture.buffer = malloc(TEX_SIZE * TEX_SIZE * sizeof(uintpixel_t));
    if (!texture.buffer) abort();
    texture.width = texture.height = TEX_SIZE;
    texture.immutable = true;
    for (int i = 0; i < TEX_SIZE * TEX_SIZE; ++i)
        texture.buffer[i] = i % 5 == 0 ? 0 : (uintpixel_t) (0x8000 | ((i * 491) & 0x7fff));
    setup(&reference, REFERENCE_SIZE);
    setup(&clipped, FRAME_SIZE);
    clipped.portX = clipped.portY = 2;
    clipped.viewX = clipped.viewY = 2;
    clipped.portW = clipped.portH = 8;
    clipped.maxX = clipped.maxY = 10;
    swrFavorSpeed = false;
    swrSmoothMinify = false;

    // Start with bottom/right clipping: the old flipped source start reads past the allocation.
    if (!checkDraw(&reference, &clipped, &texture, 8, 8, 1.0f, true, true, 1.0f)) return 1;
    const int positions[] = { 0, 2, 6, 8, 10 };
    const float scales[] = { 0.5f, 1.0f, 2.0f };
    const float alphas[] = { 1.0f, 0.63f };
    for (unsigned s = 0; s < sizeof(scales) / sizeof(scales[0]); ++s)
        for (unsigned a = 0; a < sizeof(alphas) / sizeof(alphas[0]); ++a)
            for (int flips = 0; flips < 4; ++flips)
                for (unsigned x = 0; x < sizeof(positions) / sizeof(positions[0]); ++x)
                    for (unsigned y = 0; y < sizeof(positions) / sizeof(positions[0]); ++y) {
                        // Unflipped minification has separate clipping-rounding behavior.
                        // Keep those axes inside the viewport to isolate flipped clipping.
                        if (scales[s] == 0.5f &&
                            (((flips & 1) == 0 && (positions[x] < 2 || positions[x] + 4 > 10)) ||
                             ((flips & 2) == 0 && (positions[y] < 2 || positions[y] + 4 > 10)))) continue;
                        if (!checkDraw(&reference, &clipped, &texture, positions[x], positions[y],
                                       scales[s], (flips & 1) != 0, (flips & 2) != 0, alphas[a])) return 1;
                    }
    free(reference.fb);
    free(clipped.fb);
    free(texture.rowBounds);
    free(texture.buffer);
    if (!checkSparseHalfDraw()) return 1;
    printf("PASS: %u clipped sprite cases match the unclipped crop\n", checked);
    puts("PASS: clipped sparse half-size sprite matches expected coverage");
    return 0;
}
