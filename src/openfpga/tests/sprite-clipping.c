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

/**
 * Verify that cached runs honor both clip edges and blend partial coverage
 * over the existing background while leaving transparent gaps untouched.
 */
static bool checkSparseHalfDraw(float alpha)
{
    enum { WIDTH = 144, HEIGHT = 3, LEFT = 7, RIGHT = 142 };
    const uintpixel_t under = 0xfc00, ink = 0x83e0;
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
    pixels[80] = pixels[81] = ink;
    pixels[82] = ink;
    pixels[84] = pixels[85] = pixels[WIDTH * 2 + 84] = ink;
    SWRenderer renderer;
    setup(&renderer, WIDTH);
    renderer.portX = renderer.viewX = LEFT;
    renderer.portW = RIGHT - LEFT;
    renderer.maxX = RIGHT;
    swrFillPixels(renderer.fb, WIDTH * WIDTH, under);
    swrFavorSpeed = swrSmoothMinify = true;
    swrDrawSprite((Renderer*) &renderer, 0, 0, WIDTH, HEIGHT, &texture,
                  0, 0, texture.width, texture.height, 0xffffff, alpha);
    swrOverlayFlush(&renderer);
    bool ok = alpha < 1.0f || texture.halfSpans != NULL;
    if (!ok) fprintf(stderr, "FAIL: sparse half-size sprite did not cache its runs\n");
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            uintpixel_t expected = under;
            if (y == 0 && x >= LEFT && x < RIGHT) {
                int covered = 0;
                if (x < 14 || x >= 139) covered = 4;
                else if (x == 40) covered = 2;
                else if (x == 41) covered = 1;
                else if (x == 42) covered = 3;
                alphaBlend(&expected, ink, bm_normal, swrIntAlpha(alpha) * covered / 4);
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

static bool checkRotatedDraw(SWRenderer* renderer, SWTexture* texture, float angle,
                             bool flipX, uint32_t color, float alpha, int blendmode)
{
    const int left = 1, top = -1, width = 11, height = 9;
    const int sx = 1, sy = 2, sw = 6, sh = 5;
    const float px = 5.0f, py = 3.0f;
    float radians = -angle * M_PI / 180.0f;
    float c = cosf(radians), s = sinf(radians);
    uintpixel_t ink = swrConvertPixel(color);

    renderer->blendMode = blendmode;
    swrFillPixels(renderer->fb, FRAME_SIZE * FRAME_SIZE, background);
    swrDrawSpriteRotated((Renderer*) renderer, (float) (left + (flipX ? width : 0)), (float) top,
                         (float) (flipX ? -width : width), (float) height, texture, sx, sy, sw, sh,
                         color, alpha, angle, flipX ? width - px : px, py);

    // Scalar inverse transform and ordinary compositing at each pixel center.
    for (int y = 0; y < FRAME_SIZE; y++) {
        for (int x = 0; x < FRAME_SIZE; x++) {
            uintpixel_t expected = background;
            float ox = x + 0.5f - (left + px), oy = y + 0.5f - (top + py);
            float lx = c * ox + s * oy + px, ly = -s * ox + c * oy + py;
            if (x >= 2 && x < 10 && y >= 2 && y < 10 && lx >= 0 && lx < width && ly >= 0 && ly < height) {
                float u = lx * ((float) sw / width), v = ly * ((float) sh / height);
                int tx = flipX ? (int) (sw - 1 - u) : (int) u;
                int ty = (int) v;
                uintpixel_t sample = texture->buffer[(sy + ty) * TEX_SIZE + sx + tx];
                if (swrIsOpaque(sample)) alphaBlend(&expected, tint(ink, sample), blendmode, swrIntAlpha(alpha));
            }
            if (renderer->fb[y * FRAME_SIZE + x] != expected) {
                fprintf(stderr, "FAIL: rotated angle %.1f flip %d tint %x alpha %.2f blend %d pixel %d,%d\n",
                        (double) angle, flipX, (unsigned) color, (double) alpha, blendmode, x, y);
                return false;
            }
        }
    }
    return true;
}

/** Run the clipped sprite matrix and the sparse half-size coverage regression. */
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
    const float alphas[] = { 1.0f, 0.63f, 0.5f };
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
    swrFavorSpeed = true;
    if (!checkDraw(&reference, &clipped, &texture, 8, 8, 1.0f, true, true, 0.5f)) return 1;
    const float angles[] = { 0.1f, 19.0f, 89.9f, 141.0f };
    for (unsigned a = 0; a < sizeof(angles) / sizeof(angles[0]); a++) {
        for (int flip = 0; flip < 2; flip++) {
            if (!checkRotatedDraw(&clipped, &texture, angles[a], flip, 0xffffff, 1.0f, bm_normal) ||
                !checkRotatedDraw(&clipped, &texture, angles[a], flip, 0x907f20, 1.0f, bm_normal) ||
                !checkRotatedDraw(&clipped, &texture, angles[a], flip, 0x907f20, 0.5f, bm_normal) ||
                !checkRotatedDraw(&clipped, &texture, angles[a], flip, 0x907f20, 1.0f, bm_add)) return 1;
        }
    }
    free(reference.fb);
    free(clipped.fb);
    free(texture.rowBounds);
    free(texture.buffer);
    if (!checkSparseHalfDraw(1.0f) || !checkSparseHalfDraw(0.5f)) return 1;
    printf("PASS: %u clipped sprite cases match the unclipped crop\n", checked);
    puts("PASS: clipped sparse half-size sprite matches expected coverage");
    puts("PASS: clipped rotated sprites match scalar sampling and blending");
    return 0;
}
