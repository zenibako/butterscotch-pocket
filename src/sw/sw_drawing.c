#include <stdlib.h>
#include <limits.h>
#include <float.h>
#include "sw_renderer_private.h"
#include "sw_call_notes.h"

// ==== Internal functions ====

static void swrDrawTriangleTransformed(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, int alpha);

FORCE_INLINE void swrPlotPixel_(Renderer* renderer, int x, int y, uintpixel_t color, int blendmode, int alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (x < swr->portX || y < swr->portY) return;
    if (x >= swr->maxX || y >= swr->maxY) return;
    
    swrDirtyRow(swr, y, x, x + 1);
    alphaBlend(&swr->fb[y * swr->fbPitch + x], color, blendmode, alpha);
}

// See the minification path in swrDrawSpriteInternal. Off by default; the
// platform turns it on when it renders a room below its native resolution.
bool swrSmoothMinify = false;

// Mirrored layers (see swrMirrorFlush). Layers fainter than swrMirrorFaintAlpha
// (of 256) are left out: at 16-bit colour they move a channel by one step at most.
bool swrMirrorMerge = true;

// See sw_drawing.h.
bool swrSkipFrame = false;
int swrMirrorFaintAlpha = 8;

// Trades accuracy for speed where the two can be told apart only by looking
// closely: a mirrored stack is worked out for every second pixel each way, and
// sprite draws at 8/256 opacity or less (the outer passes of glowing text) are
// left out. The platform sets it from the player's speed/accuracy choice.
bool swrFavorSpeed = false;

// Half-size copies. Drawing a texture at half size with swrSmoothMinify set
// averages the opaque texels of each 2x2 block and blends the result by how
// many of the four there were, for every pixel of every such draw. A texture
// that is never written to can have that worked out once: its half-size copy
// holds each block's average, and where some blocks are only partly opaque
// (the edge of small text, a thin line) a second array holds their coverage,
// 0..4. A draw from the copy then gives, texel for texel, what the averaging
// draw would have.
//
// The blocks start where the draw's source rectangle starts, which for a
// tileset with a one-texel border around each tile is an odd position. So a
// copy is made for one phase (0 or 1 each way), that of the first draw to
// need it; a draw at the other phase keeps the averaging path. A block cut
// by the texture's edge is whatever part of it there is. Full-size texel x
// belongs to half-size texel (x + phase) / 2.
MAYBE_UNUSED static bool swrHalfTexture(SWTexture* texture, int wantX, int wantY)
{
#if PIXEL_SIZE != 16
    (void) texture; (void) wantX; (void) wantY;
    return false;
#else
    if (texture->halfBuffer != NULL) return true;
    int phaseX = wantX & 1, phaseY = wantY & 1;
    int halfW = (texture->width + phaseX + 1) / 2, halfH = (texture->height + phaseY + 1) / 2;
    size_t texels = (size_t) halfW * halfH;
    // One allocation: the texels, each row's bounds, then room for the coverage should it be needed.
    uintpixel_t* half = (uintpixel_t*) malloc(texels * sizeof(uintpixel_t) + (size_t) halfH * 4 * sizeof(uint16_t) + texels);
    if (half == NULL) return false; // may fit another time
    uint16_t* bounds = (uint16_t*) (half + texels);
    uint16_t* fullBounds = bounds + (size_t) halfH * 2;
    uint8_t* coverage = (uint8_t*) (fullBounds + (size_t) halfH * 2);
    bool partial = false;
    
    for (int hy = 0; hy < halfH; hy++)
    {
        int y0 = hy * 2 - phaseY, y1 = y0 + 1;
        const uintpixel_t* row0 = (y0 >= 0) ? &texture->buffer[y0 * texture->width] : NULL;
        const uintpixel_t* row1 = (y1 < texture->height) ? &texture->buffer[y1 * texture->width] : NULL;
        for (int hx = 0; hx < halfW; hx++)
        {
            int x0 = hx * 2 - phaseX, x1 = x0 + 1;
            // Texels outside the texture count as transparent, as they do to the averaging draw at a sprite's edge.
            uintpixel_t texels4[4] = {
                (row0 && x0 >= 0) ? row0[x0] : 0, (row0 && x1 < texture->width) ? row0[x1] : 0,
                (row1 && x0 >= 0) ? row1[x0] : 0, (row1 && x1 < texture->width) ? row1[x1] : 0,
            };
            uint32_t red = 0, green = 0, blue = 0, covered = 0;
            for (int i = 0; i < 4; i++) {
                if (!swrIsOpaque(texels4[i])) continue;
                red += (texels4[i] >> 10) & 0x1F; green += (texels4[i] >> 5) & 0x1F; blue += texels4[i] & 0x1F;
                covered++;
            }
            uintpixel_t out = 0;
            if (covered == 4 && texels4[0] == texels4[1] && texels4[0] == texels4[2] && texels4[0] == texels4[3]) {
                out = texels4[0];
            } else if (covered > 0) {
                // The averaging draw's arithmetic: x * (65536 / n + 1) >> 16 in place of a divide.
                static const uint32_t reciprocal[5] = { 0, 65536, 32768, 21846, 16384 };
                uint32_t scale = reciprocal[covered];
                out = (uintpixel_t) (0x8000 | (((red * scale) >> 16) << 10) | (((green * scale) >> 16) << 5) | ((blue * scale) >> 16));
            }
            half[hy * halfW + hx] = out;
            coverage[hy * halfW + hx] = (uint8_t) covered;
            if (covered != 0 && covered != 4) partial = true;
        }
    }
    for (int hy = 0; hy < halfH; hy++) {
        int first = 0, end = 0;
        for (int hx = 0; hx < halfW; hx++) {
            if (coverage[hy * halfW + hx] == 0) continue;
            if (end == 0) first = hx;
            end = hx + 1;
        }
        bounds[hy * 2] = (uint16_t) first;
        bounds[hy * 2 + 1] = (uint16_t) end;
        // The longest run of whole texels: a draw at full opacity copies it as it is.
        int bestStart = 0, bestEnd = 0, runStart = -1;
        for (int hx = 0; hx <= halfW; hx++) {
            bool whole = hx < halfW && coverage[hy * halfW + hx] == 4;
            if (whole && runStart < 0) runStart = hx;
            if (!whole && runStart >= 0) {
                if (hx - runStart > bestEnd - bestStart) { bestStart = runStart; bestEnd = hx; }
                runStart = -1;
            }
        }
        fullBounds[hy * 2] = (uint16_t) bestStart;
        fullBounds[hy * 2 + 1] = (uint16_t) bestEnd;
    }
    texture->halfBuffer = half;
    texture->halfRowBounds = bounds;
    texture->halfFullBounds = fullBounds;
    texture->halfCoverage = partial ? coverage : NULL;
    texture->halfSolid = 2;
    if (!partial) {
        size_t opaque = 0;
        for (size_t i = 0; i < texels; i++) opaque += coverage[i] == 4;
        if (opaque == texels) texture->halfSolid = 1;
    }
    texture->halfPhaseX = (uint8_t) phaseX;
    texture->halfPhaseY = (uint8_t) phaseY;
    return true;
#endif
}

// The row bounds of an immutable texture (see SWTexture), or NULL.
MAYBE_UNUSED static const uint16_t* swrRowBounds(SWTexture* texture)
{
    if (texture->rowBounds != NULL) return texture->rowBounds;
    if (!texture->immutable) return NULL;
    uint16_t* bounds = (uint16_t*) malloc((size_t) texture->height * 2 * sizeof(uint16_t));
    if (bounds == NULL) return NULL;
    for (int y = 0; y < texture->height; y++) {
        const uintpixel_t* row = &texture->buffer[y * texture->width];
        int first = 0, end = 0;
        for (int x = 0; x < texture->width; x++) {
            if (!swrIsOpaque(row[x])) continue;
            if (end == 0) first = x;
            end = x + 1;
        }
        bounds[y * 2] = (uint16_t) first;
        bounds[y * 2 + 1] = (uint16_t) end;
    }
    texture->rowBounds = bounds;
    return bounds;
}

// The runs of a texture's rows that hold opaque texels other than `color`
// (see SWTexture.spans): row y's runs are runs[rows[y]] up to runs[rows[y + 1]],
// each start << 16 | end. One whose texels are mostly such runs is not worth
// keeping (worth false): its draws copy whole rows as before.
typedef struct SWColorSpans {
    uint32_t serial;    // one for each made, never 0 (see SWBaseKey)
    uintpixel_t color;
    bool worth;
    uint32_t* runs;
    uint32_t rows[1]; // height + 1 of them (not a flexible member: old compilers lack those)
} SWColorSpans;

MAYBE_UNUSED static const SWColorSpans* swrColorSpans(SWColorSpans** slot, const uintpixel_t* buffer, int width, int height, uintpixel_t color)
{
    SWColorSpans* spans = *slot;
    if (spans != NULL) return spans->color == color && spans->worth ? spans : NULL;
    size_t count = 0, covered = 0;
    for (int y = 0; y < height; y++) {
        const uintpixel_t* row = &buffer[(size_t) y * width];
        bool in = false;
        for (int x = 0; x < width; x++) {
            bool keep = row[x] != color && swrIsOpaque(row[x]);
            if (keep && !in) count++;
            covered += keep;
            in = keep;
        }
    }
    size_t header = sizeof(SWColorSpans) + ((size_t) height + 1) * sizeof(uint32_t);
    // Worth it while what is left to copy is at most half the texture.
    bool worth = covered * 2 <= (size_t) width * height;
    spans = (SWColorSpans*) malloc(header + (worth ? count : 0) * sizeof(uint32_t));
    if (spans == NULL) return NULL;
    static uint32_t serials;
    spans->serial = ++serials != 0 ? serials : ++serials;
    spans->color = color;
    spans->worth = worth;
    spans->runs = (uint32_t*) ((uint8_t*) spans + header);
    *slot = spans;
    if (!worth) return NULL;
    uint32_t n = 0;
    for (int ry = 0; ry < height; ry++) {
        const uintpixel_t* row = &buffer[(size_t) ry * width];
        spans->rows[ry] = n;
        for (int x = 0; x < width; ) {
            if (row[x] == color || !swrIsOpaque(row[x])) { x++; continue; }
            int end = x + 1;
            while (end < width && row[end] != color && swrIsOpaque(row[end])) end++;
            spans->runs[n++] = (uint32_t) x << 16 | (uint32_t) end;
            x = end;
        }
    }
    spans->rows[height] = n;
    return spans;
}

FORCE_INLINE void swrCopyPixels(uintpixel_t* dst, const uintpixel_t* src, int count)
{
    if (count < 8) { for (int i = 0; i < count; i++) dst[i] = src[i]; }
    else memcpy(dst, src, (size_t) count * sizeof(uintpixel_t));
}

// One row of an untinted 1:1 opaque draw into the main buffer, where tiles
// that still hold nothing but the clear colour (see SWDirtyBuffer) get only
// the texel runs of other colours (`run` to `runEnd`, see SWColorSpans; in
// texture columns, the row starting at column sx), so the texels of that colour
// are neither read nor written; only the tiles written to are marked. A
// backdrop that is mostly the clear colour (Undertale's battle grid) then costs
// little more than its lines. `clean` is the tiles of the tile row that were
// unmarked when the draw reached it: the draw writes each pixel once, so
// tiles it marks itself still hold the colour wherever it left out.
// Tiles in `keep` already hold what the draw writes there (see SWBaseKey) and
// are left alone. What is written is marked in `marks` (rows or baseRows).
MAYBE_UNUSED static void swrCopyOntoClean(uint32_t* marks, uint32_t clean, uint32_t keep, uintpixel_t* dstline, const uintpixel_t* srcline,
                             const uint32_t* run, const uint32_t* runEnd, int sx, int dx, int dy, int from, int to, bool solid)
{
    uint32_t wrote = 0;
    for (int x = from; x < to; ) {
        int col = (dx + x) >> SWR_DIRTY_TILE_W_SHIFT;
        int end = ((col + 1) << SWR_DIRTY_TILE_W_SHIFT) - dx;
        if (end > to) end = to;
        uint32_t bit = 1u << col;
        if (keep & bit) {
            // Holds it already.
        } else if (!(clean & bit)) {
            // Something else may be under this stretch: it all goes over.
            if (solid) {
                memcpy(&dstline[x], &srcline[x], (size_t) (end - x) * sizeof(uintpixel_t));
            } else {
                for (int i = x; i < end; i++) {
                    uintpixel_t pixel = srcline[i];
                    if (swrIsOpaque(pixel)) dstline[i] = pixel;
                }
            }
            wrote |= bit;
        } else {
            while (run < runEnd && (int) (*run & 0xFFFF) - sx <= x) run++;
            for (const uint32_t* r = run; r < runEnd; r++) {
                int a = (int) (*r >> 16) - sx, b = (int) (*r & 0xFFFF) - sx;
                if (a >= end) break;
                if (a < x) a = x;
                if (b > end) b = end;
                swrCopyPixels(&dstline[a], &srcline[a], b - a);
                wrote |= bit;
            }
        }
        x = end;
    }
    marks[dy >> SWR_DIRTY_TILE_H_SHIFT] |= wrote;
}

static void swrDrawHLineInt(Renderer* renderer, int dx, int dy, int dw, uintpixel_t color, UNUSED uintpixel_t color2, int alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    
    if (dy < swr->portY) return;
    if (dy >= swr->maxY) return;
    if (dx < swr->portX) { dw += dx - swr->portX; dx = swr->portX; }
    if (dx + dw >= swr->maxX) dw = swr->maxX - dx;
    if (dw <= 0) return;
    
    int blendmode = swr->blendMode;
    swrDirtyRow(swr, dy, dx, dx + dw);
    
    if (color == color2)
    {
        uintpixel_t *line = &swr->fb[dy * swr->fbPitch + dx];
#ifdef SW_HAS_PREMUL_BLEND
        if (blendmode == bm_normal && swrIsPartialAlpha(alpha))
        {
            uint32_t srcRedBlue = swrSpreadRedBlue(color) * alpha;
            uint32_t srcGreen = swrGreen(color) * alpha;
            uint32_t dstalpha = 256 - alpha;
            // One colour over the row: the result depends only on the pixel
            // under it, so it is worked out again only when that changes, and
            // written only when it differs. A fade to black over a screen
            // that is mostly black already then mostly just reads.
            swrBlendSolidRow(line, (size_t) dw, srcRedBlue, srcGreen, dstalpha);
            return;
        }
#endif
        for (int i = 0; i < dw; i++)
            alphaBlend(&line[i], color, blendmode, alpha);
    }
    else
    {
        uintpixel_t *line = &swr->fb[dy * swr->fbPitch + dx];
        uint32_t inc = (65536U << 14) / dw;
        uint32_t weightfp = 0;
        for (int i = 0; i < dw; i++, weightfp += inc)
        {
            uint32_t weight2 = (weightfp >> 14);
            if (weight2 > 65535) weight2 = 65535;
            uint32_t weight1 = 65535 - weight2;
            uintpixel_t result = swrTwoWayBlend(color, color2, (uint16_t) weight1, (uint16_t) weight2);
            alphaBlend(&line[i], result, blendmode, alpha);
        }
    }
}

static void swrDrawVLineInt(Renderer* renderer, int dx, int dy, int dh, uintpixel_t color, UNUSED uintpixel_t color2, int alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    
    if (dx < swr->portX) return;
    if (dx >= swr->maxX) return;
    if (dy < swr->portY) { dh += dy - swr->portY; dy = swr->portY; }
    if (dy + dh >= swr->maxY) dh = swr->maxY - dy;
    if (dh <= 0) return;
    
    int blendmode = swr->blendMode;
    swrDirtyRect(swr, dx, dy, dx + 1, dy + dh);
    
    if (color == color2)
    {
        for (int i = 0; i < dh; i++)
        {
            uintpixel_t *line = &swr->fb[(dy + i) * swr->fbPitch + dx];
            alphaBlend(&line[0], color, blendmode, alpha);
        }
    }
    else
    {
        uint32_t inc = (65536U << 14) / dh;
        uint32_t weightfp = 0;
        for (int i = 0; i < dh; i++, weightfp += inc)
        {
            uint32_t weight2 = (weightfp >> 14);
            if (weight2 > 65535) weight2 = 65535;
            uint32_t weight1 = 65535 - weight2;
            uintpixel_t result = swrTwoWayBlend(color, color2, (uint16_t) weight1, (uint16_t) weight2);
            uintpixel_t *line = &swr->fb[(dy + i) * swr->fbPitch + dx];
            alphaBlend(&line[0], result, blendmode, alpha);
        }
    }
}

static void swrDrawLineInt(Renderer* renderer, int x1, int y1, int x2, int y2, int width, uintpixel_t color1, uintpixel_t color2, int alpha, int alignment)
{
    if (x1 == x2)
    {
        if (alignment == SWR_LINE_ALIGN_CENTER)
            x1 -= width / 2;
        else if (alignment == SWR_LINE_ALIGN_M1)
            x1 -= width;
        
        for (int i = 0; i < width; i++) {
            swrDrawVLineInt(renderer, x1 + i, swrMin(y1, y2), swrAbs(y1 - y2), color1, color2, alpha);
        }
        return;
    }
    if (y1 == y2)
    {
        if (alignment == SWR_LINE_ALIGN_CENTER)
            y1 -= width / 2;
        else if (alignment == SWR_LINE_ALIGN_M1)
            y1 -= width;
        
        for (int i = 0; i < width; i++) {
            swrDrawHLineInt(renderer, swrMin(x1, x2), y1 + i, swrAbs(x1 - x2), color1, color2, alpha);
        }
        return;
    }
    
    if (UNLIKELY(width > 1))
    {
        // HACK: Just draw two triangles instead.
        int xd = x2 - x1, yd = y2 - y1;
        int line_length_sqr = xd * xd + yd * yd;
        if (line_length_sqr <= 0)
            return;
        
        float line_length = sqrtf(line_length_sqr);
        float xds = (float)-yd * (width * 0.5f) / line_length;
        float yds = (float)+xd * (width * 0.5f) / line_length;
        
        float x1l = x1 - xds, y1l = y1 - yds;
        float x1r = x1 + xds, y1r = y1 + yds;
        float x2l = x2 - xds, y2l = y2 - yds;
        float x2r = x2 + xds, y2r = y2 + yds;
        
        swrDrawTriangleTransformed(renderer, x1l, y1l, x1r, y1r, x2l, y2l, color1, color1, color2, alpha);
        swrDrawTriangleTransformed(renderer, x2r, y2r, x1r, y1r, x2l, y2l, color2, color1, color2, alpha);
        return;
    }
    
    SWRenderer* swr = (SWRenderer*) renderer;
    int dx = x2 - x1, dy = y2 - y1;
    int dx1 = swrAbs(dx), dy1 = swrAbs(dy), xe, ye, x, y;
    int px = 2 * dy1 - dx1, py = 2 * dx1 - dy1;
    int blendmode = swr->blendMode;
    
    uintpixel_t color = color1;
    uint32_t inc = (65536U << 14);
    uint32_t weightfp = 0;
    
    if (dy1 <= dx1)
    {
        if (dx >= 0)
        {
            x = x1, y = y1, xe = x2;
        }
        else
        {
            x = x2, y = y2, xe = x1;
        }
        
        if (dx1 > 0) {
            inc /= dx1;
        } else {
            inc = 0;
        }
        
        swrPlotPixel_(renderer, x, y, color, blendmode, alpha);
        
        while (x < xe)
        {
            x++;
            if (px < 0)
            {
                px += 2 * dy1;
            }
            else
            {
                if ((dx < 0 && dy < 0) || (dx > 0 && dy > 0)) y++; else y--;
                px += 2 * (dy1 - dx1);
            }

            uint32_t weight2 = (weightfp >> 14);
            if (weight2 > 65535) weight2 = 65535;
            uint32_t weight1 = 65535 - weight2;
            weightfp += inc;
            color = swrTwoWayBlend(color1, color2, (uint16_t) weight1, (uint16_t) weight2);

            swrPlotPixel_(renderer, x, y, color, blendmode, alpha);
        }
    }
    else
    {
        if (dy >= 0)
        {
            x = x1, y = y1, ye = y2;
        }
        else
        {
            x = x2, y = y2, ye = y1;
        }
        
        if (dy1 > 0) {
            inc /= dy1;
        } else {
            inc = 0;
        }
        
        swrPlotPixel_(renderer, x, y, color, blendmode, alpha);
        
        while (y < ye)
        {
            y++;
            if (py <= 0)
            {
                py += 2 * dx1;
            }
            else
            {
                if ((dx < 0 && dy < 0) || (dx > 0 && dy > 0)) x++; else x--;
                py += 2 * (dx1 - dy1);
            }

            uint32_t weight2 = (weightfp >> 14);
            if (weight2 > 65535) weight2 = 65535;
            uint32_t weight1 = 65535 - weight2;
            weightfp += inc;
            color = swrTwoWayBlend(color1, color2, (uint16_t) weight1, (uint16_t) weight2);

            swrPlotPixel_(renderer, x, y, color, blendmode, alpha);
        }
    }
}

#ifndef SW_NO_SEMI_TRANSPARENT_TEXTURE_SUPPORT

FORCE_INLINE void swrCalculateAlphaBlending(
    uintpixel_t* px,
    uintpixel_t src,
    uintpixel_t tintColor,
    int alpha,
    int blendmode
)
{
    // perform alpha blending for real now.  a good bit slower
    uint8_t alphau;
    uintpixel_t color;
    int aalpha;
    
    alphau = swrGetAlphaU8(src);
    
    // first pass tint because GM does not do premultiplied alpha
    color = tint(swrAlphaToColor(alphau), src);
    
    // second pass tint for coloring
    color = tint(tintColor, color);
    
    // recalculate alpha
    aalpha = (alpha * alphau) >> 8;
    alphaBlend(px, color, blendmode, aalpha);
}

#endif

// Stacked overlays. A game can draw the same solid sprite over the same area
// many times in a row: Undertale adds one full-screen fade object per frame
// for as long as the player stands in a doorway, a dozen or more deep. Each
// is a read-blend-write of every pixel, and N blends with a constant colour
// equal one blend with the combined colour and coverage. So a solid sprite
// draw is held back, and draws of the same area that follow it directly are
// folded into it. A draw nothing follows comes out exactly as it did before;
// a folded stack is rounded once instead of once per layer.
static void swrOverlayFlushHeld(SWRenderer* swr)
{
#ifdef SW_HAS_PREMUL_BLEND
    int count = swr->overlayCount;
    if (count == 0) return;
    swrTileRunsFlush(swr); // tile pictures and a clear still held are older than the stack
    swrClearSettle(swr);
    swr->overlayCount = 0;
    
    uint32_t dstalpha, srcRedBlue, srcGreen;
    uintpixel_t fill = swr->overlayFirstColor;
    if (count == 1) {
        int alpha = swr->overlayFirstAlpha;
        dstalpha = swrIsPartialAlpha(alpha) ? (uint32_t) (256 - alpha) : 0;
        srcRedBlue = swrSpreadRedBlue(fill) * (uint32_t) alpha;
        srcGreen = swrGreen(fill) * (uint32_t) alpha;
    } else {
        uint32_t red = (uint32_t) (swr->overlayRed + 0.5f);
        uint32_t green = (uint32_t) (swr->overlayGreen + 0.5f);
        uint32_t blue = (uint32_t) (swr->overlayBlue + 0.5f);
        dstalpha = (uint32_t) (swr->overlayKeep * 256.0f + 0.5f);
        srcRedBlue = (red << 16) | blue;
        srcGreen = green;
        fill = (uintpixel_t) (0x8000 | ((red >> 8) << 10) | ((green >> 8) << 5) | (blue >> 8));
    }
    
    swrDirtyRectIn(swr, swr->overlayFb, swr->overlayX, swr->overlayY, swr->overlayX + swr->overlayW, swr->overlayY + swr->overlayH);
    for (int y = 0; y < swr->overlayH; y++)
    {
        uintpixel_t* dstline = &swr->overlayFb[(swr->overlayY + y) * swr->overlayPitch + swr->overlayX];
        if (dstalpha == 0) {
            swrFillPixels(dstline, (size_t) swr->overlayW, fill);
            continue;
        }
        // Flat-coloured art has long runs of one colour: blend each run once.
        uintpixel_t lastDst = dstline[0];
        uintpixel_t lastOut = swrBlendPremultiplied(lastDst, srcRedBlue, srcGreen, dstalpha);
        for (int x = 0; x < swr->overlayW; x++) {
            uintpixel_t dst = dstline[x];
            if (dst != lastDst) {
                lastDst = dst;
                lastOut = swrBlendPremultiplied(dst, srcRedBlue, srcGreen, dstalpha);
            }
            dstline[x] = lastOut;
        }
    }
#else
    (void) swr;
#endif
}

#ifdef SW_HAS_PREMUL_BLEND
#define SW_OVERLAY_MAX_SOURCE_TEXELS 4096

// Holds the draw back if it is a solid overlay, folding it into the one
// already held where it covers the same pixels. Returns false if the caller
// has to draw it. Takes the clipped rectangles of an unflipped draw; lastCol
// and lastRow are the furthest texels the draw would sample.
static bool swrOverlayHold(SWRenderer* swr, int dx, int dy, int dw, int dh, SWTexture* texture,
                           int sx, int sy, int lastCol, int lastRow, uintpixel_t tintColor, int alpha)
{
    if (swrGridHeld()) return false; // nothing is held over a held grid (see SWRenderer_drawSpriteGrid)
    if (lastCol >= texture->width || lastRow >= texture->height) return false;
    if ((lastCol - sx + 1) * (lastRow - sy + 1) > SW_OVERLAY_MAX_SOURCE_TEXELS) return false;
    
    uintpixel_t texel = texture->buffer[sy * texture->width + sx];
    if (!swrIsOpaque(texel)) return false;
    for (int row = sy; row <= lastRow; row++) {
        const uintpixel_t* srcline = &texture->buffer[row * texture->width];
        for (int col = sx; col <= lastCol; col++)
            if (srcline[col] != texel) return false;
    }
    
    uintpixel_t color = tint(tintColor, texel);
    float cover = alpha > 253 ? 1.0f : (float) alpha / 256.0f;
    float red = (float) ((color >> 10) & 0x1F) * 256.0f;
    float green = (float) swrGreen(color) * 256.0f;
    float blue = (float) (color & 0x1F) * 256.0f;
    
    bool sameArea = swr->overlayCount > 0 && swr->overlayFb == swr->fb && swr->overlayPitch == swr->fbPitch &&
                    swr->overlayX == dx && swr->overlayY == dy && swr->overlayW == dw && swr->overlayH == dh;
    if (sameArea) {
        swr->overlayCount++;
        swr->overlayKeep *= 1.0f - cover;
        swr->overlayRed = swr->overlayRed * (1.0f - cover) + red * cover;
        swr->overlayGreen = swr->overlayGreen * (1.0f - cover) + green * cover;
        swr->overlayBlue = swr->overlayBlue * (1.0f - cover) + blue * cover;
        return true;
    }
    
    swrOverlayFlush(swr);
    swr->overlayCount = 1;
    swr->overlayFb = swr->fb;
    swr->overlayPitch = swr->fbPitch;
    swr->overlayX = dx; swr->overlayY = dy; swr->overlayW = dw; swr->overlayH = dh;
    swr->overlayFirstColor = color;
    swr->overlayFirstAlpha = alpha;
    swr->overlayKeep = 1.0f - cover;
    swr->overlayRed = red * cover;
    swr->overlayGreen = green * cover;
    swr->overlayBlue = blue * cover;
    return true;
}
#endif

static void swrDrawSpriteInternal(
    Renderer* renderer, int dx, int dy, int dw, int dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uintpixel_t tintColor, int alpha, MAYBE_UNUSED bool pointSample, MAYBE_UNUSED int contrast
);

#ifdef SW_HAS_PREMUL_BLEND
#define SW_MIRROR_MIN_PIXELS 4096

static void swrMirrorReplay(SWRenderer* swr, const SWSpriteCall* call)
{
    swrDrawSpriteInternal((Renderer*) swr, call->dx, call->dy, call->dw, call->dh, call->texture,
                          call->sx, call->sy, call->sw, call->sh, call->tintColor, call->alpha, false, SWR_CONTRAST_NONE);
}

static void swrMirrorFillRows(SWRenderer* swr, int x, int y, int w, int h, uintpixel_t color, int alpha)
{
    for (int row = 0; row < h; row++)
        swrDrawHLineInt((Renderer*) swr, x, y + row, w, color, color, alpha);
}

// Works a whole stack out for the unflipped quarter in one pass and writes
// each pixel to all four quarters. Layer by layer it does the arithmetic the
// ordinary draws would have done. What lies under the stack is either one
// known colour, so the buffer is not read at all, or (readUnder) the same in
// all four quarters and read from the unflipped one. `step` is 1, or 2 to work
// out every second pixel each way and repeat it (swrFavorSpeed).
static void swrMirrorCompose(SWRenderer* swr, int layers, int step, bool readUnder)
{
    typedef struct {
        const uintpixel_t* buffer;
        const uintpixel_t* srcline;
        int texWidth, sx, sy, xstep, ystep;
        uintpixel_t tintColor;
        uint32_t alpha, dstalpha, srcRedBlue, srcGreen, lastPixel;
        bool solid;
    } Active;
    Active active[SW_MIRROR_MAX_LAYERS];
    int count = 0;
    
    for (int layer = 0; layer < layers; layer++)
    {
        const SWMirrorLayer* held = &swr->mirrorStack[layer];
        const SWSpriteCall* call = &held->calls[0];
        Active* a = &active[count];
        a->solid = held->solid;
        a->alpha = (uint32_t) call->alpha;
        a->dstalpha = 256 - a->alpha;
        a->tintColor = call->tintColor;
        if (held->solid) {
            a->srcRedBlue = swrSpreadRedBlue(call->tintColor) * a->alpha;
            a->srcGreen = swrGreen(call->tintColor) * a->alpha;
        } else {
            if (call->alpha < swrMirrorFaintAlpha) continue;
            a->buffer = call->texture->buffer;
            a->texWidth = call->texture->width;
            a->sx = held->sx; a->sy = held->sy;
            a->xstep = held->xstep; a->ystep = held->ystep;
            a->lastPixel = 0xFFFFFFFF;
            a->srcRedBlue = a->srcGreen = 0;
        }
        count++;
    }
    
    const int fp_prec = 14;
    int cx = swr->mirrorCx, cy = swr->mirrorCy, w = swr->mirrorW, h = swr->mirrorH;
    uintpixel_t under = swr->mirrorUnder;
    swrDirtyRect(swr, cx - w - 1, cy - h - 1, cx + w + 1, cy + h + 1); // mirrored both ways; a step of 2 can reach one past
    
    for (int y = 0; y < h; y += step)
    {
        for (int i = 0; i < count; i++) {
            Active* a = &active[i];
            if (!a->solid)
                a->srcline = &a->buffer[(a->sy + (int) (((int32_t) y * a->ystep) >> fp_prec)) * a->texWidth + a->sx];
        }
        uintpixel_t* below = &swr->fb[(cy + y) * swr->fbPitch + cx];
        uintpixel_t* above = &swr->fb[(cy - 1 - y) * swr->fbPitch + cx];
        bool second = step == 2 && y + 1 < h;
        uintpixel_t* below2 = below + swr->fbPitch;
        uintpixel_t* above2 = above - swr->fbPitch;
        
        for (int x = 0; x < w; x += step)
        {
            uintpixel_t color = readUnder ? below[x] : under;
            for (int i = 0; i < count; i++)
            {
                Active* a = &active[i];
                if (!a->solid) {
                    uintpixel_t pixel = a->srcline[(int) (((int32_t) x * a->xstep) >> fp_prec)];
                    if (!swrIsOpaque(pixel)) continue;
                    if (pixel != a->lastPixel) {
                        uintpixel_t tinted = tint(a->tintColor, pixel);
                        a->srcRedBlue = swrSpreadRedBlue(tinted) * a->alpha;
                        a->srcGreen = swrGreen(tinted) * a->alpha;
                        a->lastPixel = pixel;
                    }
                }
                color = swrBlendPremultiplied(color, a->srcRedBlue, a->srcGreen, a->dstalpha);
            }
            
            below[x] = color; below[-1 - x] = color;
            above[x] = color; above[-1 - x] = color;
            if (step == 2) {
                bool wide = x + 1 < w;
                if (wide) {
                    below[x + 1] = color; below[-2 - x] = color;
                    above[x + 1] = color; above[-2 - x] = color;
                }
                if (second) {
                    below2[x] = color; below2[-1 - x] = color;
                    above2[x] = color; above2[-1 - x] = color;
                    if (wide) {
                        below2[x + 1] = color; below2[-2 - x] = color;
                        above2[x + 1] = color; above2[-2 - x] = color;
                    }
                }
            }
        }
    }
}

// Mirrored layers. Deltarune's character creation screens draw a small image
// stretched over each quarter of the screen, flipped so that the four meet in
// the middle, at low opacity, six layers deep, and then darken the lot with a
// translucent fill: seven screens of blending per frame. Where the picture
// underneath is itself the same in all four quarters (there, a cleared screen),
// each layer's quarters come out as mirror images of one another, so one
// quarter is worked out and the other three are copies of it. The layers are
// held back until something else is drawn; if the picture underneath turns out
// not to be symmetric they are drawn the ordinary way.
static void swrMirrorFlush(SWRenderer* swr)
{
    int layers = swr->mirrorLayers, stage = swr->mirrorStage;
    if (layers == 0 && stage == 0) return;
    swrTileRunsFlush(swr); // tile pictures and a clear still held are older than the layers
    swrClearSettle(swr);
    swr->mirrorLayers = 0;
    swr->mirrorStage = 0;
    swr->mirrorReplaying = true;
    
    // The held draws belong to the target and port they were made for.
    uintpixel_t* fb = swr->fb;
    uint16_t pitch = swr->fbPitch;
    int port[6] = { swr->portX, swr->portY, swr->portW, swr->portH, swr->maxX, swr->maxY };
    int blendMode = swr->blendMode;
    swr->fb = swr->mirrorFb;
    swr->fbPitch = (uint16_t) swr->mirrorPitch;
    swr->portX = swr->mirrorPort[0]; swr->portY = swr->mirrorPort[1];
    swr->portW = swr->mirrorPort[2]; swr->portH = swr->mirrorPort[3];
    swr->maxX = swr->mirrorPort[4]; swr->maxY = swr->mirrorPort[5];
    swr->blendMode = bm_normal;
    
    int cx = swr->mirrorCx, cy = swr->mirrorCy, w = swr->mirrorW, h = swr->mirrorH;
    if (layers > 0 && swr->mirrorUnderKnown)
    {
        swrMirrorCompose(swr, layers, swrFavorSpeed ? 2 : 1, false);
    }
    else if (layers > 0)
    {
        swrOverlayFlushHeld(swr); // anything held before the layers lies under them
        
        bool symmetric = true;
        for (int y = 0; y < h && symmetric; y++)
        {
            const uintpixel_t* below = &swr->fb[(cy + y) * swr->fbPitch + cx];
            const uintpixel_t* above = &swr->fb[(cy - 1 - y) * swr->fbPitch + cx];
            for (int x = 0; x < w; x++)
            {
                uintpixel_t pixel = below[x];
                if (below[-1 - x] != pixel || above[x] != pixel || above[-1 - x] != pixel) { symmetric = false; break; }
            }
        }
        
        if (symmetric)
            swrMirrorCompose(swr, layers, swrFavorSpeed ? 2 : 1, true);
        else for (int layer = 0; layer < layers; layer++)
        {
            const SWMirrorLayer* held = &swr->mirrorStack[layer];
            if (held->solid) {
                swrMirrorFillRows(swr, cx - w, cy - h, 2 * w, 2 * h, held->calls[0].tintColor, held->calls[0].alpha);
                continue;
            }
            if (held->calls[0].alpha < swrMirrorFaintAlpha) continue;
            for (int quarter = 0; quarter < 4; quarter++)
                swrMirrorReplay(swr, &held->calls[quarter]);
            swrOverlayFlushHeld(swr);
        }
    }
    
    // Quarters of a layer that was never completed are ordinary draws.
    for (int quarter = 0; quarter < stage; quarter++)
        swrMirrorReplay(swr, &swr->mirrorStack[layers].calls[quarter]);
    swrOverlayFlushHeld(swr);
    
    swr->fb = fb;
    swr->fbPitch = pitch;
    swr->portX = port[0]; swr->portY = port[1]; swr->portW = port[2]; swr->portH = port[3];
    swr->maxX = port[4]; swr->maxY = port[5];
    swr->blendMode = blendMode;
    swr->mirrorReplaying = false;
    swr->uniformValid = false;
}

// Holds the draw back if it can be a quarter of a mirrored layer. Takes the
// draw as it was asked for (call) and as it came out of clipping. Returns
// false if the caller has to draw it; nothing is held then.
static bool swrMirrorHold(SWRenderer* swr, const SWSpriteCall* call, int dx, int dy, int dw, int dh,
                          bool flipX, bool flipY, int sx, int sy, int xstep, int ystep, int unit)
{
    if (swrGridHeld()) return false; // nothing is held over a held grid (see SWRenderer_drawSpriteGrid)
    bool eligible = swrMirrorMerge && swr->blendMode == bm_normal && swrIsPartialAlpha(call->alpha) &&
                    dw * dh >= SW_MIRROR_MIN_PIXELS && xstep <= unit && ystep <= unit;
    if (!eligible) {
        // A draw too faint to change anything (alphaBlend skips it) is no reason to let a stack out.
        bool invisible = swr->blendMode == bm_normal && call->alpha < 4;
        if (invisible && (swr->mirrorLayers > 0 || swr->mirrorStage > 0)) return true;
        swrMirrorFlush(swr);
        return false;
    }
    
    int stage = swr->mirrorStage;
    if (stage > 0)
    {
        // The quarters follow the unflipped one anticlockwise: left, above left, above.
        SWMirrorLayer* held = &swr->mirrorStack[swr->mirrorLayers];
        const SWSpriteCall* first = &held->calls[0];
        int wantX = (stage == 3) ? swr->mirrorCx : swr->mirrorCx - swr->mirrorW;
        int wantY = (stage == 1) ? swr->mirrorCy : swr->mirrorCy - swr->mirrorH;
        bool match = swr->fb == swr->mirrorFb && swr->fbPitch == swr->mirrorPitch &&
                     flipX == (stage != 3) && flipY == (stage != 1) &&
                     dx == wantX && dy == wantY && dw == swr->mirrorW && dh == swr->mirrorH &&
                     call->texture == first->texture && call->alpha == first->alpha &&
                     call->tintColor == first->tintColor &&
                     sx == held->sx && sy == held->sy && xstep == held->xstep && ystep == held->ystep;
        if (match) {
            held->calls[stage] = *call;
            if (stage == 3) {
                swr->mirrorLayers++;
                swr->mirrorStage = 0;
            } else
                swr->mirrorStage = stage + 1;
            return true;
        }
        swrMirrorFlush(swr); // not a mirrored layer after all; this draw may start one
    }
    
    if (flipX || flipY) {
        swrMirrorFlush(swr);
        return false;
    }
    
    bool sameQuad = swr->mirrorLayers > 0 && swr->fb == swr->mirrorFb && swr->fbPitch == swr->mirrorPitch &&
                    dx == swr->mirrorCx && dy == swr->mirrorCy && dw == swr->mirrorW && dh == swr->mirrorH;
    if (!sameQuad || swr->mirrorLayers >= SW_MIRROR_MAX_LAYERS)
        swrMirrorFlush(swr);
    if (swr->mirrorLayers == 0) {
        swr->mirrorFb = swr->fb;
        swr->mirrorPitch = swr->fbPitch;
        swr->mirrorPort[0] = swr->portX; swr->mirrorPort[1] = swr->portY;
        swr->mirrorPort[2] = swr->portW; swr->mirrorPort[3] = swr->portH;
        swr->mirrorPort[4] = swr->maxX; swr->mirrorPort[5] = swr->maxY;
        swr->mirrorCx = dx; swr->mirrorCy = dy; swr->mirrorW = dw; swr->mirrorH = dh;
        // Known only if nothing is waiting to be drawn underneath.
        swr->mirrorUnderKnown = swr->uniformValid && swr->fb == swr->mainFb && swr->overlayCount == 0;
        swr->mirrorUnder = swr->uniformColor;
    }
    SWMirrorLayer* held = &swr->mirrorStack[swr->mirrorLayers];
    held->solid = false;
    held->sx = sx; held->sy = sy;
    held->xstep = xstep; held->ystep = ystep;
    held->calls[0] = *call;
    swr->mirrorStage = 1;
    return true;
}

// A translucent fill of exactly the area a held stack covers joins the stack
// as one more layer. Takes swrFillRectangle's arguments; returns false if the
// fill has to be drawn.
bool swrMirrorHoldFill(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t pxcolor, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    if (swr->mirrorLayers == 0 || swr->mirrorStage != 0 || swr->mirrorLayers >= SW_MIRROR_MAX_LAYERS) return false;
    if (swr->fb != swr->mirrorFb || swr->fbPitch != swr->mirrorPitch || swr->blendMode != bm_normal) return false;
    int alphaInt = swrIntAlpha(alpha);
    if (!swrIsPartialAlpha(alphaInt)) return false;
    
    // The area swrFillRectangle would cover, clipped as swrDrawHLineInt clips it.
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);
    int x1i = swrFloor(x1), x2i = swrCeiling(x2), y1i = swrFloor(y1), y2i = swrCeiling(y2);
    int xd = x2i - x1i, yd = y2i - y1i;
    if (xd < 0) { x1i = x2i; xd = -xd; }
    if (yd < 0) { y1i = y2i; yd = -yd; }
    if (xd <= 0 || yd <= 0) return false;
    int left = x1i < swr->portX ? swr->portX : x1i;
    int right = x1i + xd >= swr->maxX ? swr->maxX : x1i + xd;
    int top = y1i < swr->portY ? swr->portY : y1i;
    int bottom = y1i + yd + 1 > swr->maxY ? swr->maxY : y1i + yd + 1;
    
    if (left != swr->mirrorCx - swr->mirrorW || right != swr->mirrorCx + swr->mirrorW ||
        top != swr->mirrorCy - swr->mirrorH || bottom != swr->mirrorCy + swr->mirrorH)
        return false;
    
    SWMirrorLayer* held = &swr->mirrorStack[swr->mirrorLayers++];
    held->solid = true;
    held->calls[0].tintColor = pxcolor;
    held->calls[0].alpha = alphaInt;
    return true;
}
#endif

// For a change of state that draws nothing itself (a new view): a clear
// still held, with nothing held over it, can stay that way.
void swrOverlayFlushForState(SWRenderer* swr)
{
#ifdef SW_HAS_PREMUL_BLEND
    swrTileRunsFlush(swr); // takes the clear along if there were pictures
    if (swr->clearHeld && !swrGridHeld() && swr->mirrorLayers == 0 && swr->mirrorStage == 0 && swr->overlayCount == 0) {
        bool uniform = swr->uniformValid;
        swr->clearHeld = false;
        swrOverlayFlush(swr);
        swr->clearHeld = true;
        swr->uniformValid = uniform; // nothing was drawn
        return;
    }
#endif
    swrOverlayFlush(swr);
}

void swrOverlayFlush(SWRenderer* swr)
{
    swrTrailEnd();
#ifdef SW_HAS_PREMUL_BLEND
    // Held tile pictures are older than anything else held, so they go first,
    // and a held clear is older still: the pictures take it with them, and
    // it stays held for a run of tiles coming in to do the same.
    // A held grid lies between the two. With anything else held as well, all of it goes.
    bool othersHeld = swr->mirrorLayers != 0 || swr->mirrorStage != 0 || swr->overlayCount != 0;
    if (!swr->tileRunEntering || othersHeld) {
        swrTileRunsFlush(swr);
        swrGridFlush(swr);
        swrClearSettle(swr);
    }
    if (!swr->mirrorReplaying) swrMirrorFlush(swr);
#else
    swrClearSettle(swr);
#endif
    swrOverlayFlushHeld(swr);
    swr->uniformValid = false; // every caller is about to draw
}

// Small bubble glyphs need strong edge coverage:
// (n/4)^0.2, in 256ths. Zero and full coverage stay unchanged.
// Apply it per draw, never to cached texels; there is no per-pixel pow/sqrt.
#ifdef SW_HAS_PREMUL_BLEND
static int swrContrastInk(int covered)
{
    static const int strength[5] = { 0, 194, 223, 242, 256 };
    return strength[covered];
}

static int swrContrastCoverage(int covered, int alpha, int contrast)
{
    if (contrast == SWR_CONTRAST_GLYPH) return (alpha * swrContrastInk(covered)) >> 8;
    return covered == 4 ? alpha : (alpha * covered) >> 2;
}
#endif

#if PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING
// The clipped width and destination stride are even and the destination is
// word-aligned. A transparent texel leaves both destination rows untouched.
static void swrCopyDoubledSprite(uintpixel_t* dst, int dstPitch, const uintpixel_t* src, int srcPitch,
                                 int width, int height, bool flipX)
{
    int columns = width / 2;
    int start = flipX ? columns - 1 : 0;
    int step = flipX ? -1 : 1;
    for (int y = 0; y < height; y += 2) {
        uintpixel_t* top = dst + y * dstPitch;
        // Reuse the last row for an odd clipped height.
        uintpixel_t* bottom = top + (y + 1 < height ? dstPitch : 0);
#if defined(__has_builtin)
#if __has_builtin(__builtin_assume_aligned)
        top = __builtin_assume_aligned(top, 4);
        bottom = __builtin_assume_aligned(bottom, 4);
#endif
#endif
        const uintpixel_t* row = src + (y / 2) * srcPitch;
        for (int x = 0, col = start; x < width; x += 2, col += step) {
            uintpixel_t texel = row[col];
            if (!swrIsOpaque(texel)) continue;
            uint32_t pair = ((uint32_t) texel << 16) | texel;
            memcpy(top + x, &pair, sizeof(pair));
            memcpy(bottom + x, &pair, sizeof(pair));
        }
    }
}
#endif

/**
 * Clip a sprite's source and destination rectangles and draw with the requested sampling and blend.
 * A cached half-size texture can supply the coverage of each sampled pixel.
 */
static void swrDrawSpriteInternal(
    Renderer* renderer, int dx, int dy, int dw, int dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uintpixel_t tintColor, int alpha, MAYBE_UNUSED bool pointSample, MAYBE_UNUSED int contrast
)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    MAYBE_UNUSED SWTexture* source = texture; // `texture` may become its half-size copy's stand-in
    SWR_PROBE_STOP(3);
#ifdef SW_HAS_PREMUL_BLEND
    const SWSpriteCall asked = { dx, dy, dw, dh, texture, sx, sy, sw, sh, tintColor, alpha };
#endif
    
    bool flipX = false, flipY = false;
    if (dw < 0) { dx += dw; dw = -dw; flipX = true; }
    if (dh < 0) { dy += dh; dh = -dh; flipY = true; }
    
    //basic out of bound checks
    if (dw == 0 || dh == 0) return;
    if (sw == 0) sw = 1;
    if (sh == 0) sh = 1;
    if (dx + dw <= swr->portX) return;
    if (dy + dh <= swr->portY) return;
    if (dx >= swr->maxX) return;
    if (dy >= swr->maxY) return;
    
    int odw = dw, odh = dh;
    int osw = sw, osh = sh;
    
    int minx = swr->portX, miny = swr->portY, maxx = swr->portX + swr->portW, maxy = swr->portY + swr->portH;
    
    //out of bounds adjustment checks
    int diffxl = 0, diffyl = 0, diffxu = 0, diffyu = 0;
    if (dx < minx) { diffxl = minx - dx; dx = minx; dw -= diffxl; }
    if (dy < miny) { diffyl = miny - dy; dy = miny; dh -= diffyl; }
    if (dx + dw > maxx) { diffxu = dx + dw - maxx; dw -= diffxu; }
    if (dy + dh > maxy) { diffyu = dy + dh - maxy; dh -= diffyu; }
    
    if (diffxl != 0 || diffyl != 0 || diffxu != 0 || diffyu != 0)
    {
        if (pointSample && osw == odw && osh == odh) {
            // A pre-hinted native mask is already 1:1. Preserve its last visible
            // row/column, including a viewport only one pixel wide or high.
            sx += diffxl; sy += diffyl; sw = dw; sh = dh;
        } else if (contrast != SWR_CONTRAST_NONE) {
            // This sampler is exactly 2:1. An odd last block has one source
            // texel, so clipping it must not remove a texel from its neighbour.
            sx += diffxl * 2; sy += diffyl * 2;
            sw -= diffxl * 2; sh -= diffyl * 2;
            if (sw > dw * 2) sw = dw * 2;
            if (sh > dh * 2) sh = dh * 2;
        } else {
            //adjust source coordinates too
            diffxl = (int)((long)diffxl * osw / odw);
            diffyl = (int)((long)diffyl * osh / odh);
            // A flipped draw advances its source start by the clipped amount.
            // Adding the unflipped path's extra texel here starts one too far in,
            // so the last flipped sample can read beyond the texture.
            diffxu = (int)((long)(diffxu + (flipX ? 0 : 1)) * osw / odw);
            diffyu = (int)((long)(diffyu + (flipY ? 0 : 1)) * osh / odh);
            sx += flipX ? diffxu : diffxl;
            sy += flipY ? diffyu : diffyl;
            sw -= diffxl + diffxu;
            sh -= diffyl + diffyu;
        }
        if (sw <= 0 || sh <= 0) return;
    }
    
    //clip the source coords into bounds too
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sx + sw >= texture->width)  { sw = texture->width  - sx; }
    if (sy + sh >= texture->height) { sh = texture->height - sy; }
    if (sw <= 0 || sh <= 0) return;
    SWR_PROBE_STOP(4);
    
#ifdef SW_HAS_PREMUL_BLEND
    // Drawn at half size with speed favoured: take the texels from the
    // texture's half-size copy, one for one, instead of averaging four of the
    // full-size ones for every pixel of every such draw. Everything below
    // then sees an unscaled draw of a smaller texture.
    SWTexture halved;
    bool fromHalf = false;
    // The draw has to start on a block of the copy. One that needs coverage
    // is drawn right here, on the averaging draw's terms (normal blending,
    // not flipped, not too faint to show); the rest carry on below as a plain
    // unscaled draw.
    bool halfScale = (osw == 2 * odw || osw == 2 * odw - 1) && (osh == 2 * odh || osh == 2 * odh - 1);
    if (!pointSample && contrast != SWR_CONTRAST_BUTTON &&
        (contrast == SWR_CONTRAST_NONE || ((osw & 1) == 0 && (osh & 1) == 0)) &&
        swrFavorSpeed && texture->immutable && halfScale && swrHalfTexture(texture, sx, sy) &&
        ((sx + texture->halfPhaseX) & 1) == 0 && ((sy + texture->halfPhaseY) & 1) == 0 &&
        (texture->halfCoverage == NULL || (swr->blendMode == bm_normal && !flipX && !flipY && alpha >= 4)))
    {
        int halfW = (texture->width + texture->halfPhaseX + 1) / 2, halfH = (texture->height + texture->halfPhaseY + 1) / 2;
        const uint8_t* coverage = texture->halfCoverage;
        halved = *texture;
        halved.solid = texture->halfSolid;
        halved.buffer = texture->halfBuffer;
        halved.width = (uint16_t) halfW;
        halved.height = (uint16_t) halfH;
        sx = (sx + texture->halfPhaseX) / 2;
        sy = (sy + texture->halfPhaseY) / 2;
        texture = &halved;
        if (sx + dw > halfW) dw = halfW - sx;
        if (sy + dh > halfH) dh = halfH - sy;
        if (dw <= 0 || dh <= 0) return;
        sw = dw; sh = dh;
        osw = odw; osh = odh;
        fromHalf = true;
        
        if (coverage != NULL)
        {
            SWR_PROBE_STOP(5);
            SWR_PROBE_PATH(1);
            swrOverlayFlush(swr);
            SWR_PROBE_STOP(6);
            bool plainOpaque = alpha > 253 && (tintColor & 0x7FFF) == 0x7FFF;
            const SWColorSpans* spans = NULL;
            // Large backdrops can have thin, partly covered lines with wide
            // transparent gaps. A half copy with coverage always takes this
            // branch, so its spans omit zero (no coverage) instead of the clear colour.
            if (dw >= 128 && plainOpaque && contrast == SWR_CONTRAST_NONE)
                spans = swrColorSpans(&source->halfSpans, halved.buffer, halfW, halfH, 0);
            uint32_t lastColor = 0xFFFFFFFF;
            uintpixel_t lastTinted = 0;
            for (int y = 0; y < dh; y++)
            {
                int row = sy + y;
                uint32_t firstRun = spans != NULL ? spans->rows[row] : 0;
                uint32_t endRun = spans != NULL ? spans->rows[row + 1] : 1;
                if (firstRun == endRun) continue;
                uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
                swrDirtyRow(swr, dy + y, dx, dx + dw);
                const uintpixel_t* srcline = &halved.buffer[row * halfW + sx];
                const uint8_t* covline = &coverage[row * halfW + sx];
                // Only the stretch of the row that has anything in it.
                int from = (int) halved.halfRowBounds[row * 2] - sx, to = (int) halved.halfRowBounds[row * 2 + 1] - sx;
                if (from < 0) from = 0;
                if (to > dw) to = dw;
                if (alpha == 128 && contrast == SWR_CONTRAST_NONE) {
                    for (int x = from; x < to; x++) {
                        uint32_t covered = covline[x];
                        if (covered == 0) continue;
                        uintpixel_t color = srcline[x];
                        if (color != lastColor) {
                            lastTinted = tint(tintColor, color);
                            lastColor = color;
                        }
                        if (covered == 4) {
                            dstline[x] = swrBlendHalf(dstline[x], lastTinted);
                        } else {
                            uint32_t coverageAlpha = covered << 5;
                            dstline[x] = swrBlendPremultiplied(dstline[x], swrSpreadRedBlue(lastTinted) * coverageAlpha,
                                                               swrGreen(lastTinted) * coverageAlpha, 256 - coverageAlpha);
                        }
                    }
                    continue;
                }
                // Untinted at full opacity, a texel standing for all four of its own is stored as it
                // is: the row's longest run of those is copied, and only what is left on either side
                // (a sprite's edge) is gone through a pixel at a time. The loop below does the left
                // side, the copy, then the right side.
                int copyFrom = to, copyTo = to;
                if (plainOpaque) {
                    copyFrom = (int) halved.halfFullBounds[row * 2] - sx;
                    copyTo = (int) halved.halfFullBounds[row * 2 + 1] - sx;
                    if (copyFrom < from) copyFrom = from;
                    if (copyTo > to) copyTo = to;
                    if (copyTo - copyFrom < 4) copyFrom = copyTo = to; // not worth a call
                    else memcpy(&dstline[copyFrom], &srcline[copyFrom], (size_t) (copyTo - copyFrom) * sizeof(uintpixel_t));
                }
                for (uint32_t i = firstRun; i < endRun; i++)
                {
                    int start = from, end = to;
                    if (spans != NULL) {
                        int left = (int) (spans->runs[i] >> 16) - sx;
                        int right = (int) (spans->runs[i] & 0xFFFF) - sx;
                        if (left >= to) break;
                        if (start < left) start = left;
                        if (end > right) end = right;
                    }
                    for (int x = start; x < end; x++)
                    {
                        if (x == copyFrom) { x = copyTo - 1; continue; }
                        uint32_t covered = covline[x];
                        if (covered == 0) continue;
                        uintpixel_t color = srcline[x];
                        if (color != lastColor) {
                            lastTinted = tint(tintColor, color);
                            lastColor = color;
                        }
                        int coverageAlpha = swrContrastCoverage((int) covered, alpha, contrast);
                        if (coverageAlpha > 253)
                            dstline[x] = lastTinted;
                        else if (coverageAlpha >= 4)
                            dstline[x] = swrBlendPremultiplied(dstline[x], swrSpreadRedBlue(lastTinted) * coverageAlpha,
                                                               swrGreen(lastTinted) * coverageAlpha, 256 - coverageAlpha);
                    }
                }
            }
            return;
        }
    }
#endif
    
    //okay, now we can finally get on with rendering
    SWR_PROBE_STOP(5);
    
    int ixs = 0, oxs = 1, iys = 0, oys = 1;
    if (flipX) ixs = dw - 1, oxs = -1;
    if (flipY) iys = dh - 1, oys = -1;
    
    // tweak these if stuff doesn't look right
    typedef int32_t fixedp_t;
    const int fp_prec = 14;

    fixedp_t ystep = (sh == dh) ? (1 << fp_prec) : ((fixedp_t) osh << fp_prec) / odh;
    fixedp_t xstep = (sw == dw) ? (1 << fp_prec) : ((fixedp_t) osw << fp_prec) / odw;
#ifdef SW_HAS_PREMUL_BLEND
    if (contrast != SWR_CONTRAST_NONE && !fromHalf) xstep = ystep = 2 << fp_prec;
#endif
    fixedp_t oxs2 = oxs * xstep;
    fixedp_t oys2 = oys * ystep;
    fixedp_t ixs2 = ixs * xstep;
    fixedp_t iys2 = iys * ystep;
    
    int blendmode = swr->blendMode;
    
#ifdef SW_HAS_PREMUL_BLEND
    // Nothing comes of a draw this faint: alphaBlend skips it, and a 16-bit texel is opaque or absent.
    // Leaving here keeps what is held, and what is known about the buffer, as it was.
    if (blendmode == bm_normal && alpha < 4) return;
    
    // A quarter of a mirrored layer is held back; anything else lets held layers out first.
    // (A draw from a half-size copy is not offered: a held layer is worked out from the texture it names.)
    if (!pointSample && contrast == SWR_CONTRAST_NONE && !swr->mirrorReplaying && !fromHalf &&
        swrMirrorHold(swr, &asked, dx, dy, dw, dh, flipX, flipY, sx, sy, (int) xstep, (int) ystep, 1 << fp_prec))
        return;
    swr->uniformValid = false;
    {
        // Possibly the buffer's first draw, to be kept (see SWBaseKey): only an
        // untinted 1:1 opaque draw of a texture's runs (the opaque path below).
        SWBaseKey key;
        bool keyed = false;
#if PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING
        if (blendmode == bm_normal && alpha > 253 && (tintColor & 0x7FFF) == 0x7FFF && !flipX && xstep == (1 << fp_prec) &&
            texture->immutable && contrast == SWR_CONTRAST_NONE && !pointSample && swr->fb == swr->mainFb && dx >= 0) {
            const SWColorSpans* made = texture != source ? source->halfSpans : texture->spans;
            memset(&key, 0, sizeof(key));
            key.texture = texture != source ? (const void*) source->halfBuffer : (const void*) texture;
            key.serial = made != NULL ? made->serial : 0;
            key.dx = dx; key.dy = dy; key.dw = dw; key.dh = dh;
            key.sx = sx; key.sy = sy; key.sw = sw; key.sh = sh;
            key.ystep = ystep; key.flipY = flipY;
            key.portX = swr->portX; key.portY = swr->portY; key.maxX = swr->maxX; key.maxY = swr->maxY;
            keyed = true;
        }
#endif
        swrClearSettleBase(swr, keyed ? &key : NULL);
    }
    if (swrFavorSpeed && alpha <= 8 && blendmode == bm_normal) return; // too faint to miss; see swrFavorSpeed
#endif
    
#ifdef SW_HAS_PREMUL_BLEND
    // Solid overlays drawn at full size or enlarged may be held back and merged.
    if (swr->overlayMergeAllowed && blendmode == bm_normal && alpha >= 4 && !flipX && !flipY &&
        xstep <= (1 << fp_prec) && ystep <= (1 << fp_prec))
    {
        int lastCol = sx + (int) (((fixedp_t) (dw - 1) * xstep) >> fp_prec);
        int lastRow = sy + ((dh == sh) ? dh - 1 : (int) (((fixedp_t) (dh - 1) * ystep) >> fp_prec));
        if (swrOverlayHold(swr, dx, dy, dw, dh, texture, sx, sy, lastCol, lastRow, tintColor, alpha))
            return;
    }
    swrOverlayFlush(swr);
#endif
    SWR_PROBE_STOP(6);
    
#ifdef SW_HAS_PREMUL_BLEND
    if (contrast == SWR_CONTRAST_BUTTON || contrast == SWR_CONTRAST_UI_GLYPH || contrast == SWR_CONTRAST_BUBBLE_GLYPH) {
        // Only this exact white, full-opacity hybrid case has a derivative.
        // Key the clipped source rectangle: an odd edge or shifted phase changes
        // both the center tap and which box taps count as transparent.
        if (contrast == SWR_CONTRAST_BUTTON && alpha == 256 && (tintColor & 0x7fff) == 0x7fff &&
            sw == 2 * dw - (sw & 1) && sh == 2 * dh - (sh & 1)) {
            const SWButtonCache* cache = swrButtonCache(texture, sx, sy, sw, sh);
            if (cache) {
                for (int y = 0; y < dh; ++y) {
                    uintpixel_t* dst = swr->fb + (dy + y) * swr->fbPitch + dx;
                    swrDirtyRow(swr, dy + y, dx, dx + dw);
                    const SWButtonPixel* row = cache->pixels + y * dw;
                    for (int x = 0; x < dw; ++x) {
                        if (row[x].covered)
                            dst[x] = swrBlendPremultiplied(dst[x], row[x].redBlue, row[x].green, row[x].keep);
                    }
                }
                return;
            }
        }
        // Blend the source center with an ordinary 2x2 average, equally.
        // Full blocks stay solid; edge shades follow the centered shape without
        // the contrast curve's expansion, except for the bubble adjustment below.
        // Missing texels count as transparent;
        // the center of an odd last block clamps to its own source rectangle.
        // Use full texels so the shared half cache remains unchanged.
        int right = sx + sw - 1, bottom = sy + sh - 1;
        if (alpha > 256) alpha = 256;
        uint32_t lastColor = 0xFFFFFFFF;
        uintpixel_t lastTinted = 0;
        for (int y = 0; y < dh; ++y) {
            int row0 = sy + y * 2, row1 = row0 < bottom ? row0 + 1 : bottom;
            if (row0 > bottom) break;
            uintpixel_t* dst = swr->fb + (dy + y) * swr->fbPitch + dx;
            swrDirtyRow(swr, dy + y, dx, dx + dw);
            for (int x = 0; x < dw; ++x) {
                int col0 = sx + x * 2, col1 = col0 < right ? col0 + 1 : right;
                if (col0 > right) break;
                uintpixel_t taps[5] = {
                    texture->buffer[row1 * texture->width + col1],
                    texture->buffer[row0 * texture->width + col0],
                    col0 < right ? texture->buffer[row0 * texture->width + col0 + 1] : 0,
                    row0 < bottom ? texture->buffer[(row0 + 1) * texture->width + col0] : 0,
                    col0 < right && row0 < bottom ? texture->buffer[(row0 + 1) * texture->width + col0 + 1] : 0
                };
                uint32_t redBlue = 0, green = 0, covered = 0;
                for (int tap = 0; tap < 5; ++tap) {
                    uintpixel_t color = taps[tap];
                    if (!swrIsOpaque(color)) continue;
                    unsigned weight = tap == 0 ? 4 : 1;
                    if (color != lastColor) {
                        lastTinted = tint(tintColor, color);
                        lastColor = color;
                    }
                    redBlue += swrSpreadRedBlue(lastTinted) * weight;
                    green += swrGreen(lastTinted) * weight;
                    covered += weight;
                }
                if (covered != 0) {
                    unsigned coverageAlpha = (covered * alpha) >> 3;
                    if (contrast == SWR_CONTRAST_BUBBLE_GLYPH) {
                        // Black bubble ink splits the difference between this hybrid
                        // coverage and the earlier bold curve, restoring faint strokes.
                        unsigned boxCovered = covered - (swrIsOpaque(taps[0]) ? 4 : 0);
                        coverageAlpha = (coverageAlpha + ((alpha * swrContrastInk(boxCovered)) >> 8)) >> 1;
                    }
                    dst[x] = swrBlendPremultiplied(dst[x], (redBlue * alpha) >> 3, (green * alpha) >> 3,
                                                 256 - coverageAlpha);
                }
            }
        }
        return;
    }

    // Shrinking by about half with nearest-neighbour sampling drops every
    // other texel, which makes small text unreadable (a 640x480 screen drawn
    // into 320x240). When enabled, average each 2x2 block of texels instead
    // and use the opaque share of the block as coverage.
    if (!pointSample && swrSmoothMinify && blendmode == bm_normal && !flipX && !flipY &&
        xstep > (3 << (fp_prec - 1)) && ystep > (3 << (fp_prec - 1)) && alpha >= 4)
    {
        int srcRight = sx + sw - 1, srcBottom = sy + sh - 1;
        uint32_t lastColor = 0xFFFFFFFF;
        uintpixel_t lastTinted = 0;
        SWR_PROBE_PATH(5);
        
        fixedp_t ys2 = iys2;
        for (int y = 0; y < dh; y++, ys2 += oys2)
        {
            uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            swrDirtyRow(swr, dy + y, dx, dx + dw);
            int row0 = sy + (int)(ys2 >> fp_prec);
            if (row0 > srcBottom) break;
            int row1 = row0 < srcBottom ? row0 + 1 : -1;
            const uintpixel_t* src0 = &texture->buffer[row0 * texture->width];
            const uintpixel_t* src1 = row1 >= 0 ? &texture->buffer[row1 * texture->width] : NULL;
            
            fixedp_t xs2 = ixs2;
            for (int x = 0; x < dw; x++, xs2 += oxs2)
            {
                int col0 = sx + (int)(xs2 >> fp_prec);
                if (col0 > srcRight) break;
                int col1 = col0 < srcRight ? col0 + 1 : -1;
                
                // Texels outside the sprite's source rectangle count as transparent.
                uintpixel_t t0 = src0[col0];
                uintpixel_t t1 = col1 >= 0 ? src0[col1] : 0;
                uintpixel_t t2 = src1 ? src1[col0] : 0;
                uintpixel_t t3 = (src1 && col1 >= 0) ? src1[col1] : 0;
                
                uintpixel_t color;
                int coverageAlpha = alpha;
                if (t0 == t1 && t0 == t2 && t0 == t3) {
                    // Flat area (most of any sprite): nothing to average.
                    if (!swrIsOpaque(t0)) continue;
                    color = t0;
                } else {
                    uint32_t red = 0, green = 0, blue = 0, covered = 0;
                    #define SW_ACCUMULATE(texel) \
                        if (swrIsOpaque(texel)) { \
                            red += ((texel) >> 10) & 0x1F; green += ((texel) >> 5) & 0x1F; blue += (texel) & 0x1F; covered++; \
                        }
                    SW_ACCUMULATE(t0) SW_ACCUMULATE(t1) SW_ACCUMULATE(t2) SW_ACCUMULATE(t3)
                    #undef SW_ACCUMULATE
                    if (covered == 0) continue;
                    
                    // Divide by the 1..4 covered texels without a hardware divide: x * (65536 / n + 1) >> 16.
                    static const uint32_t reciprocal[5] = { 0, 65536, 32768, 21846, 16384 };
                    uint32_t scale = reciprocal[covered];
                    color = (uintpixel_t)(0x8000 | (((red * scale) >> 16) << 10) | (((green * scale) >> 16) << 5) | ((blue * scale) >> 16));
                    coverageAlpha = swrContrastCoverage((int) covered, alpha, contrast);
                }
                
                if (color != lastColor) {
                    lastTinted = tint(tintColor, color);
                    lastColor = color;
                }
                
                if (coverageAlpha > 253)
                    dstline[x] = lastTinted;
                else if (coverageAlpha >= 4)
                    dstline[x] = swrBlendPremultiplied(dstline[x], swrSpreadRedBlue(lastTinted) * coverageAlpha,
                                                       swrGreen(lastTinted) * coverageAlpha, 256 - coverageAlpha);
            }
        }
        return;
    }
    
    // Translucent sprites (fades, overlays): premultiply the tinted source
    // colour and reuse it while consecutive source pixels are the same.
    if (blendmode == bm_normal && swrIsPartialAlpha(alpha))
    {
        const uint16_t* rowBounds = NULL;
        if (!flipX && xstep == (1 << fp_prec) && dh == sh)
            rowBounds = texture->buffer == texture->halfBuffer ? texture->halfRowBounds : swrRowBounds(texture); // the half-size copy stands in as a texture of its own
        uint32_t dstalpha = 256 - alpha;
        uint32_t lastPixel = 0xFFFFFFFF;
        uint32_t srcRedBlue = 0, srcGreen = 0;
        uintpixel_t lastTinted = 0;
        SWR_PROBE_PATH(4);
        
        fixedp_t ys2 = iys2;
        for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
        {
            uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            swrDirtyRow(swr, dy + y, dx, dx + dw);
            const uintpixel_t* srcline;
            if (dh == sh)
                srcline = &texture->buffer[(sy + ys) * texture->width + sx];
            else
                srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];
            
            fixedp_t xs2 = ixs2;
            int from = 0, to = dw;
            if (rowBounds != NULL) {
                // Unscaled and not mirrored: only the stretch of the row that has anything in it.
                int row = sy + ys;
                from = (int) rowBounds[row * 2] - sx;
                to = (int) rowBounds[row * 2 + 1] - sx;
                if (from < 0) from = 0;
                if (to > dw) to = dw;
                xs2 += (fixedp_t) from * oxs2;
            }
            if (alpha == 128 && swrFavorSpeed) {
                for (int x = from; x < to; x++, xs2 += oxs2) {
                    uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
                    if (!swrIsOpaque(pixel)) continue;
                    if (pixel != lastPixel) {
                        lastTinted = tint(tintColor, pixel);
                        lastPixel = pixel;
                    }
                    dstline[x] = swrBlendHalf(dstline[x], lastTinted);
                }
                continue;
            }
            for (int x = from; x < to; x++, xs2 += oxs2)
            {
                uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
                if (!swrIsOpaque(pixel))
                    continue;
                
                if (pixel != lastPixel) {
                    uintpixel_t tinted = tint(tintColor, pixel);
                    srcRedBlue = swrSpreadRedBlue(tinted) * alpha;
                    srcGreen = swrGreen(tinted) * alpha;
                    lastPixel = pixel;
                }
                dstline[x] = swrBlendPremultiplied(dstline[x], srcRedBlue, srcGreen, dstalpha);
            }
        }
        return;
    }
#endif
    
#if PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING
    // Fully opaque draws, the common case: copy opaque texels straight
    // through, tinting once per run of identical source pixels. alphaBlend()
    // reduces to a plain store for bm_normal at this alpha.
    if (blendmode == bm_normal && alpha > 253)
    {
        bool untinted = (tintColor & 0x7FFF) == 0x7FFF;
        if (untinted && texture->immutable && !flipY && xstep == (1 << (fp_prec - 1)) && ystep == xstep &&
            !((dx | dw | swr->fbPitch) & 1) && !((uintptr_t) swr->fb & 3)) {
            SWR_PROBE_PATH(texture->solid == 1 ? 2 : 3);
            swrDirtyRect(swr, dx, dy, dx + dw, dy + dh);
            swrCopyDoubledSprite(swr->fb + dy * swr->fbPitch + dx, swr->fbPitch,
                                 texture->buffer + sy * texture->width + sx, texture->width, dw, dh, flipX);
            return;
        }
        uint32_t lastPixel = 0xFFFFFFFF;
        uintpixel_t lastTinted = 0;
        
        // An immutable texture is looked over once to learn whether it has any transparent texel.
        if (texture->solid == 0 && texture->immutable && untinted && !flipX && xstep == (1 << fp_prec)) {
            size_t count = (size_t) texture->width * texture->height, opaque = 0;
            for (size_t i = 0; i < count; i++) opaque += swrIsOpaque(texture->buffer[i]) ? 1 : 0;
            texture->solid = opaque == count ? 1 : 2;
        }
        bool solid = texture->solid == 1;
        SWR_PROBE_PATH(solid ? 2 : 3);
        const uint16_t* rowBounds = NULL;
        if (!solid && untinted && !flipX && xstep == (1 << fp_prec) && dh == sh)
            rowBounds = texture->buffer == texture->halfBuffer ? texture->halfRowBounds : swrRowBounds(texture); // the half-size copy stands in as a texture of its own
        
        // Onto tiles still holding only the clear colour, texels of that colour are left out (swrCopyOntoClean).
        static int noCleanSkip = -1;
        if (noCleanSkip < 0) noCleanSkip = getenv("BS_NO_CLEAN_SKIP") != NULL;
        SWDirtyBuffer* clear = swr->dirtyMark;
        const SWColorSpans* spans = NULL;
        if (noCleanSkip || clear == NULL || !clear->known || swr->fb != swr->mainFb || !untinted || flipX || xstep != (1 << fp_prec)
            || dx < 0 || dx + dw > clear->width || !texture->immutable)
            clear = NULL;
        else {
            spans = swrColorSpans(texture != source ? &source->halfSpans : &texture->spans,
                                  texture->buffer, texture->width, texture->height, clear->color);
            if (spans == NULL) clear = NULL;
        }
        int cleanRow = -1;
        uint32_t clean = 0, keep = 0;
        // The buffer's first draw (see SWBaseKey) marks baseRows, and stays kept only if every row went through the runs.
        bool asBase = swr->baseMode != 0 && clear != NULL && clear == swr->dirty;
        bool baseWhole = asBase;
        uint32_t* marks = clear == NULL ? NULL : asBase ? clear->baseRows : clear->rows;
        
        fixedp_t ys2 = iys2;
        for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
        {
            uintpixel_t* dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            const uintpixel_t* srcline;
            if (dh == sh)
                srcline = &texture->buffer[(sy + ys) * texture->width + sx];
            else
                srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];
            
            if (clear != NULL && dy + y >= 0 && dy + y < clear->height) {
                int tileRow = (dy + y) >> SWR_DIRTY_TILE_H_SHIFT;
                if (tileRow != cleanRow) {
                    // First row of this draw in the tile row: what is clean before the draw touches it.
                    cleanRow = tileRow;
                    uint32_t mask = swrDirtyMask(dx, dx + dw);
                    clean = ~(clear->rows[tileRow] | clear->baseRows[tileRow]) & mask;
                    keep = swr->baseMode == 2 && asBase ? clear->keep[tileRow] & mask : 0;
                }
                if ((clean | keep) != 0) {
                    int from = 0, to = dw;
                    if (rowBounds != NULL) {
                        int row = sy + ys;
                        from = (int) rowBounds[row * 2] - sx;
                        to = (int) rowBounds[row * 2 + 1] - sx;
                        if (from < 0) from = 0;
                        if (to > dw) to = dw;
                    }
                    int srcRow = dh == sh ? sy + ys : sy + (int) (ys2 >> fp_prec);
                    swrCopyOntoClean(marks, clean, keep, dstline, srcline, &spans->runs[spans->rows[srcRow]], &spans->runs[spans->rows[srcRow + 1]],
                                     sx, dx, dy + y, from, to, solid);
                    continue;
                }
            }
            baseWhole = false;
            swrDirtyRow(swr, dy + y, dx, dx + dw);
            
            fixedp_t xs2 = ixs2;
            if (untinted && !flipX && xstep == (1 << fp_prec) && solid)
            {
                // Nothing transparent anywhere in the texture: the row goes over as it is.
                memcpy(dstline, srcline, (size_t) dw * sizeof(uintpixel_t));
            }
            else if (untinted && !flipX && xstep == (1 << fp_prec))
            {
                // Unscaled: no stepping through the source needed, and only
                // the stretch of the row that has anything in it looked at.
                int from = 0, to = dw;
                if (rowBounds != NULL) {
                    int row = sy + ys;
                    from = (int) rowBounds[row * 2] - sx;
                    to = (int) rowBounds[row * 2 + 1] - sx;
                    if (from < 0) from = 0;
                    if (to > dw) to = dw;
                }
                for (int x = from; x < to; x++)
                {
                    uintpixel_t pixel = srcline[x];
                    if (swrIsOpaque(pixel))
                        dstline[x] = pixel;
                }
            }
            else if (untinted)
            {
                for (int x = 0; x < dw; x++, xs2 += oxs2)
                {
                    uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
                    if (swrIsOpaque(pixel))
                        dstline[x] = pixel;
                }
            }
            else
            {
                for (int x = 0; x < dw; x++, xs2 += oxs2)
                {
                    uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
                    if (!swrIsOpaque(pixel))
                        continue;
                    
                    if (pixel != lastPixel) {
                        lastTinted = tint(tintColor, pixel);
                        lastPixel = pixel;
                    }
                    dstline[x] = lastTinted;
                }
            }
        }
        if (swr->baseMode != 0) {
            if (baseWhole && swr->baseMode == 1) clear->baseKey.serial = spans->serial; // the runs may have been made just now
            swrBaseFinish(swr, baseWhole);
        }
        return;
    }
#endif
    
    if (sw == dw)
    {
        fixedp_t ys2 = (fixedp_t) iys2;
        for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
        {
            uintpixel_t* dstline;
            const uintpixel_t* srcline;
            dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            swrDirtyRow(swr, dy + y, dx, dx + dw);
            if (dh == sh)
                srcline = &texture->buffer[(sy + ys) * texture->width + sx];
            else
                srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];
            
            for (int x = 0, xs = ixs; x < dw; x++, xs += oxs)
            {
                uintpixel_t pixel = srcline[xs];
                if (swrIsOpaque(pixel))
                    alphaBlend(&dstline[x], tint(tintColor, pixel), blendmode, alpha);
#ifndef SW_NO_SEMI_TRANSPARENT_TEXTURE_SUPPORT
                else if (!swrIsFullyTransparent(pixel))
                    swrCalculateAlphaBlending(&dstline[x], pixel, tintColor, alpha, blendmode);
#endif
            }
        }
    }
    else
    {
        fixedp_t ys2 = iys2;
        for (int y = 0, ys = iys; y < dh; y++, ys += oys, ys2 += oys2)
        {
            uintpixel_t* dstline;
            const uintpixel_t* srcline;
            dstline = &swr->fb[(dy + y) * swr->fbPitch + dx];
            swrDirtyRow(swr, dy + y, dx, dx + dw);
            if (dh == sh)
                srcline = &texture->buffer[(sy + ys) * texture->width + sx];
            else
                srcline = &texture->buffer[(sy + (int)(ys2 >> fp_prec)) * texture->width + sx];
            
            fixedp_t xs2 = ixs2;
            for (int x = 0; x < dw; x++, xs2 += oxs2)
            {
                uintpixel_t pixel = srcline[(int)(xs2 >> fp_prec)];
                if (swrIsOpaque(pixel))
                    alphaBlend(&dstline[x], tint(tintColor, pixel), blendmode, alpha);
#ifndef SW_NO_SEMI_TRANSPARENT_TEXTURE_SUPPORT
                else if (!swrIsFullyTransparent(pixel))
                    swrCalculateAlphaBlending(&dstline[x], pixel, tintColor, alpha, blendmode);
#endif
            }
        }
    }
}

// Sprite chains reuse the same angles. Cache their exact float keys so nearby
// angles keep their own sine and cosine and the sampler retains its rounding.
static SWRotation swrSpriteRotation(SWRenderer* swr, float angleDeg)
{
    uint32_t key;
    memcpy(&key, &angleDeg, sizeof(key));
    uint32_t hash = (key ^ (key >> 16)) * 0x9e3779b9u;
    SWRotation* entry = &swr->rotationCache[hash >> 26];
    if (key != 0 && entry->angleBits == key) return *entry;

    float radians = -angleDeg * M_PI / 180.0f;
    SWRotation result = { key, cosf(radians), sinf(radians) };
    *entry = result;
    return result;
}

static void swrDrawSpriteRotatedInternal(
    Renderer* renderer, int dx, int dy, int dw, int dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uintpixel_t tintColor, int alpha,
    float angleDeg,
    float pivotX,
    float pivotY
)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    bool flipX = false, flipY = false;
    if (dw < 0) { dw = -dw; dx -= dw; pivotX = dw - pivotX; flipX = true; }
    if (dh < 0) { dh = -dh; dy -= dh; pivotY = dh - pivotY; flipY = true; }
    
    SWRotation rotation = swrSpriteRotation(swr, angleDeg);
    float cosA = rotation.cosine, sinA = rotation.sine;
    
    float cnrx[4], cnry[4];
    cnrx[0] = cnrx[3] = dx;
    cnry[0] = cnry[1] = dy;
    cnrx[1] = cnrx[2] = dx + dw;
    cnry[2] = cnry[3] = dy + dh;
    
    float pxa = pivotX + dx;
    float pya = pivotY + dy;
    
    float minXf = FLT_MAX, minYf = FLT_MAX, maxXf = -FLT_MAX, maxYf = -FLT_MAX;
    float quadX[4], quadY[4]; // the turned rectangle's corners, in order round it
    for (int i = 0; i < 4; i++)
    {
        float cxi = cnrx[i] - pxa;
        float cyi = cnry[i] - pya;
        float rx = cosA * cxi - sinA * cyi + pxa;
        float ry = sinA * cxi + cosA * cyi + pya;
        quadX[i] = rx;
        quadY[i] = ry;
        if (minXf > rx) minXf = rx;
        if (maxXf < rx) maxXf = rx;
        if (minYf > ry) minYf = ry;
        if (maxYf < ry) maxYf = ry;
    }

    // minX, minY, maxX, maxY now represent an AABB of pixels we should loop over
    int minX = swrFloor(minXf);
    int minY = swrFloor(minYf);
    int maxX = swrCeiling(maxXf);
    int maxY = swrCeiling(maxYf);
    
    // basic out-of-bound checks
    if (maxX < swr->portX) return;
    if (maxY < swr->portY) return;
    if (minX >= swr->maxX) return;
    if (minY >= swr->maxY) return;
    
    // however, we'll need to clip it against out of bounds first
    int minXc = minX, minYc = minY, maxXc = maxX, maxYc = maxY;
    int minx = swr->portX, miny = swr->portY, maxx = swr->portX + swr->portW, maxy = swr->portY + swr->portH;
    
    if (minXc < minx) minXc = minx;
    if (minYc < miny) minYc = miny;
    if (maxXc >= maxx) maxXc = maxx;
    if (maxYc >= maxy) maxYc = maxy;
    
    // some final clip checks
    if (minXc >= maxXc || minYc >= maxYc) return;
    
    int sox = flipX ? sw - 1 : 0;
    int soy = flipY ? sh - 1 : 0;
    int six = flipX ? -1 : 1;
    int siy = flipY ? -1 : 1;
    
    float sw_dw = (float) sw / dw;
    float sh_dh = (float) sh / dh;
    const uintpixel_t* srcPixels = texture->buffer;
    int srcPitch = texture->width;
    ptrdiff_t srcOffset = (ptrdiff_t) sy * srcPitch + sx;
    
    int blendmode = swr->blendMode;
#if PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING && !defined SW_INACCURATE_TINTING
    bool copyHalfTint = blendmode == bm_normal && alpha > 253 && (tintColor & 0x7fff) == 0x4210;
#endif

    // Half of the box round a turned sprite is outside the sprite, and the
    // test below costs a pixel there most of what one inside costs. So each
    // row is narrowed first to where the rectangle's edges cross it, with
    // pixels to spare on either side: the test still decides every pixel
    // that is looked at, and the ones left out were certain to fail it.
    // Each edge's x for a row is its x at its first corner plus the row's
    // distance from that corner times its slope; a level edge crosses no row.
    #define SWR_TURNED_SPARE 2.0f
    float edgeSlope[4], edgeLow[4], edgeHigh[4], edgeReach[4];
    bool edgeLevel[4];
    for (int e = 0; e < 4; e++) // not i: a compiler without C99 for-scoping still has the corners' i
    {
        int next = (e + 1) & 3;
        float y0 = quadY[e], y1 = quadY[next];
        float ex = quadX[next] - quadX[e], ey = y1 - y0;
        edgeLevel[e] = ey > -0.001f && ey < 0.001f;
        float slope = edgeLevel[e] ? 0.0f : ex / ey;
        edgeSlope[e] = slope;
        edgeReach[e] = slope < 0.0f ? -slope : slope;
        edgeLow[e] = (y0 < y1 ? y0 : y1) - 1.0f;
        edgeHigh[e] = (y0 < y1 ? y1 : y0) + 1.0f;
    }
    
    // The numbers the loop needs as floats are worked out once: a conversion
    // from an integer for every pixel holds the processor up on the Pocket.
    // These are the same values the conversions gave.
    float soxf = (float) sox, soyf = (float) soy, sixf = (float) six, siyf = (float) siy;
    float dwf = (float) dw, dhf = (float) dh;
    float localPivotX = pxa - dx, localPivotY = pya - dy;
    
    // A pixel's distance across from the pivot depends on its column alone,
    // so it is worked out once for each column the draw can reach, by the
    // sum the loop would otherwise do for every pixel of every row.
    #define SWR_TURNED_COLUMNS 640
    // For following the texel in whole numbers along a row (see the loop):
    // how near a line between texels a pixel is left to the float sum, in
    // 65536ths of a texel; the longest row and the furthest texel coordinate
    // it is trusted over, which keep the drift well inside that and the
    // numbers inside 32 bits.
    #define SWR_TURNED_NEAR 2048
    #define SWR_TURNED_STEP_SPAN 2048
    #define SWR_TURNED_STEP_REACH 16000.0f
    // A step across the screen moves the texel by this much each way.
    float texStepXf = cosA * sw_dw * 65536.0f, texStepYf = -sinA * sh_dh * 65536.0f;
    bool steppable = sw < 8192 && sh < 8192 &&
                     texStepXf > -1.0e9f && texStepXf < 1.0e9f && texStepYf > -1.0e9f && texStepYf < 1.0e9f;
    int32_t texStepX = steppable ? (int32_t) swrFloor(texStepXf + 0.5f) : 0;
    int32_t texStepY = steppable ? (int32_t) swrFloor(texStepYf + 0.5f) : 0;
    float acrossKept[SWR_TURNED_COLUMNS];
    bool acrossHeld = maxXc - minXc <= SWR_TURNED_COLUMNS; // the table starts at column minXc
    if (acrossHeld) {
        float colf = (float) minXc; // whole numbers this small are exact in a float, and stay so a step at a time
        for (int col = minXc; col < maxXc; col++, colf += 1.0f) acrossKept[col - minXc] = colf + 0.5f - pxa;
    }

    for (int cy = minYc; cy < maxYc; cy++)
    {
        float cyf = (float) cy;
        
        // Where the rectangle can be on this row.
        float rowY = cyf + 0.5f;
        float rowLeft = FLT_MAX, rowRight = -FLT_MAX;
        for (int i = 0; i < 4; i++)
        {
            if (rowY < edgeLow[i] || rowY > edgeHigh[i]) continue;
            if (edgeLevel[i]) {
                float x0 = quadX[i], x1 = quadX[(i + 1) & 3];
                if (rowLeft > x0) rowLeft = x0;
                if (rowLeft > x1) rowLeft = x1;
                if (rowRight < x0) rowRight = x0;
                if (rowRight < x1) rowRight = x1;
                continue;
            }
            // The crossing, and where it would be a row up and a row down:
            // near a corner the edge may end short of this row's middle.
            float at = quadX[i] + (rowY - quadY[i]) * edgeSlope[i];
            float reach = edgeReach[i];
            if (rowLeft > at - reach) rowLeft = at - reach;
            if (rowRight < at + reach) rowRight = at + reach;
        }
        if (rowLeft > rowRight) continue;
        int rowFrom = minXc, rowTo = maxXc;
        if (rowLeft - SWR_TURNED_SPARE > (float) minXc) rowFrom = swrFloor(rowLeft - SWR_TURNED_SPARE);
        if (rowRight + SWR_TURNED_SPARE < (float) maxXc) rowTo = swrCeiling(rowRight + SWR_TURNED_SPARE) + 1;
        if (rowTo > maxXc) rowTo = maxXc;
        if (rowFrom >= rowTo) continue;
        uintpixel_t *dstline = &swr->fb[cy * swr->fbPitch];
        // Only this span can affect the buffer's next partial clear.
        swrDirtyRow(swr, cy, rowFrom, rowTo);
        float oy = rowY - pya;
        
        // Along a row the texel a pixel takes moves by the same amount at
        // every step, so it is followed in whole numbers (sixteen bits below
        // the point) from where the sum below puts the row's first pixel.
        // That drifts from the sum by a few thousandths of a texel over a
        // row, which only matters to a pixel that lands that close to the
        // line between two texels or to the sprite's edge. Any pixel within
        // SWR_TURNED_NEAR of one is worked out by the sum as before; for the
        // rest the two agree on the texel, so the picture is the same.
        bool stepped = steppable && rowTo - rowFrom <= SWR_TURNED_STEP_SPAN;
        int32_t texX = 0, texY = 0; // the unmirrored texel coordinate, 16.16
        if (stepped) {
            float ox = acrossHeld ? acrossKept[rowFrom - minXc] : (float) rowFrom + 0.5f - pxa;
            float lx =  cosA * ox + sinA * oy;
            float ly = -sinA * ox + cosA * oy;
            lx += localPivotX;
            ly += localPivotY;
            lx = lx * sw_dw;
            ly = ly * sh_dh;
            if (lx > -SWR_TURNED_STEP_REACH && lx < SWR_TURNED_STEP_REACH && ly > -SWR_TURNED_STEP_REACH && ly < SWR_TURNED_STEP_REACH) {
                texX = (int32_t) swrFloor(lx * 65536.0f + 0.5f);
                texY = (int32_t) swrFloor(ly * 65536.0f + 0.5f);
            } else {
                stepped = false;
            }
        }
        
        for (int cx = rowFrom; cx < rowTo; cx++, texX += texStepX, texY += texStepY)
        {
            int tx, ty;
            // Far enough from every line between texels (the sprite's edges are two of them)?
            if (stepped && (uint32_t) ((texX & 0xFFFF) - SWR_TURNED_NEAR) < (uint32_t) (0x10000 - 2 * SWR_TURNED_NEAR) &&
                           (uint32_t) ((texY & 0xFFFF) - SWR_TURNED_NEAR) < (uint32_t) (0x10000 - 2 * SWR_TURNED_NEAR))
            {
                int wholeX = texX >> 16, wholeY = texY >> 16;
                if ((uint32_t) wholeX >= (uint32_t) sw || (uint32_t) wholeY >= (uint32_t) sh) continue;
                // Mirrored, the sum below takes the whole part of (size - 1 - coordinate):
                // the texel before the mirror image, and never less than the first.
                tx = wholeX;
                ty = wholeY;
                if (flipX) { tx = sw - 2 - wholeX; if (tx < 0) tx = 0; }
                if (flipY) { ty = sh - 2 - wholeY; if (ty < 0) ty = 0; }
            }
            else
            {
                // we need to determine the texture-space coordinate of cx/cy
                float ox = acrossHeld ? acrossKept[cx - minXc] : (float) cx + 0.5f - pxa;
                
                // "undo" the rotation
                float lx =  cosA * ox + sinA * oy;
                float ly = -sinA * ox + cosA * oy;
                
                // turn it into a texture-local coordinate
                lx += localPivotX;
                ly += localPivotY;
                
                if (lx < 0 || ly < 0 || lx >= dwf || ly >= dhf) continue;
                
                lx = lx * sw_dw;
                ly = ly * sh_dh;
                
                // Not mirrored, that is zero plus the coordinate times one: the coordinate itself.
                tx = flipX ? (int)(soxf + lx * sixf) : (int) lx;
                ty = flipY ? (int)(soyf + ly * siyf) : (int) ly;
                
                if (tx < 0) tx = 0;
                if (ty < 0) ty = 0;
                if (tx >= sw) tx = sw - 1;
                if (ty >= sh) ty = sh - 1;
            }
            
            uintpixel_t src = srcPixels[srcOffset + (ptrdiff_t) ty * srcPitch + tx];

            if (swrIsOpaque(src)) {
#if PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING && !defined SW_INACCURATE_TINTING
                if (copyHalfTint) {
                    dstline[cx] = swrTintHalf(src);
                    continue;
                }
#endif
                alphaBlend(&dstline[cx], tint(tintColor, src), blendmode, alpha);
            }
#ifndef SW_NO_SEMI_TRANSPARENT_TEXTURE_SUPPORT
            else if (!swrIsFullyTransparent(src))
                swrCalculateAlphaBlending(&dstline[cx], src, tintColor, alpha, blendmode);
#endif
        }
    }
    #undef SWR_TURNED_SPARE
    #undef SWR_TURNED_COLUMNS
    #undef SWR_TURNED_NEAR
    #undef SWR_TURNED_STEP_SPAN
    #undef SWR_TURNED_STEP_REACH
}

static void swrDrawTriangleInternal(SWRenderer* swr, int xup, int yup, int xleft, int yleft, int xright, int yright, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, int alpha)
{
    int blendmode = swr->blendMode;
    
    // Figure out the maximum Y extent of the triangle.
    // (Note that we know yup is the minimum.)
    int xmid, ymid, xmid2 = xup, xmax, ymax;
    if (yleft < yright) {
        xmax = xright, ymax = yright;
        xmid = xleft, ymid = yleft;
        if (yright != yup)
            xmid2 = xup + (xright - xup) * (ymid - yup) / (yright - yup);
    } else {
        xmax = xleft, ymax = yleft;
        xmid = xright, ymid = yright;
        if (yleft != yup)
            xmid2 = xup + (xleft - xup) * (ymid - yup) / (yleft - yup);
    }
    
    if (color1 == color2 && color2 == color3)
    {
        // fast path: the triangle is all the same color
        for (int y = yup; y < ymax; y++)
        {
            if (y >= swr->height) break;
            
            int x1 = xup, x2 = xup;
            if (y <= ymid)
            {
                // Lines: between up and mid, and between up and max
                if (ymid != yup)
                    x1 = xup + (xmid - xup) * (y - yup) / (ymid - yup);
                
                if (ymid != yup)
                    x2 = xup + (xmid2 - xup) * (y - yup) / (ymid - yup);
            }
            else
            {
                // Lines: between mid and max, and between up and max
                if (ymax != yup)
                    x1 = xup + (xmax - xup) * (y - yup) / (ymax - yup);
                
                if (ymax != ymid)
                    x2 = xmid + (xmax - xmid) * (y - ymid) / (ymax - ymid);
            }
            
            if (x1 >= x2) {
                int tmp = x1;
                x1 = x2;
                x2 = tmp;
            }
            
            if (x1 < swr->portX) x1 = swr->portX;
            if (x1 >= swr->maxX) continue;
            if (x2 < swr->portX) continue;
            if (x2 >= swr->maxX) x2 = swr->maxX - 1;
            if (x1 > x2) continue;
            
            if (y >= 0) {
                uintpixel_t* line = &swr->fb[y * swr->width];
                swrDirtyRow(swr, y, x1, x2 + 1);
                for (int x = x1; x < x2; x++) {
                    alphaBlend(&line[x], color1, blendmode, alpha);
                }
            }
        }
        
        return;
    }
    
    // slow path: the triangle is all three different colors
    
    int area = (xleft - xup) * (yright - yup) - (yleft - yup) * (xright - xup);
    if (area == 0) {
        //logDebug("SWR: Area is 0, returning early. Coords: (%d,%d) (%d,%d) (%d,%d)\n", xup, yup, xleft, yleft, xright, yright);
        return;
    }
    
    // tried making this 32-bit and it didn't work.  maybe another day.
    int64_t areaReciprocal = ((int64_t) 65535 << 16) / area;
    for (int y = yup; y < ymax; y++)
    {
        if (y >= swr->height) break;
        
        int x1 = xup, x2 = xup;
        if (y <= ymid)
        {
            // Lines: between up and mid, and between up and max
            if (ymid != yup)
                x1 = xup + (xmid - xup) * (y - yup) / (ymid - yup);
            
            if (ymid != yup)
                x2 = xup + (xmid2 - xup) * (y - yup) / (ymid - yup);
        }
        else
        {
            // Lines: between mid and max, and between up and max
            if (ymax != yup)
                x1 = xup + (xmax - xup) * (y - yup) / (ymax - yup);
            
            if (ymax != ymid)
                x2 = xmid + (xmax - xmid) * (y - ymid) / (ymax - ymid);
        }
        
        if (x1 >= x2) {
            int tmp = x1;
            x1 = x2;
            x2 = tmp;
        }
        
        if (x1 < swr->portX) x1 = swr->portX;
        if (x1 >= swr->maxX) continue;
        if (x2 < swr->portX) continue;
        if (x2 >= swr->maxX) x2 = swr->maxX - 1;
        if (x1 > x2) continue;
        
        if (y < 0) continue;
        
        uintpixel_t* line = &swr->fb[y * swr->width];
        swrDirtyRow(swr, y, x1, x2 + 1);
        
        int e_up   = (xleft - x1) * (yright - y) - (yleft - y) * (xright - x1);
        int e_left = (xright - x1) * (yup - y) - (yright - y) * (xup - x1);
        int d_up   = (yleft - yright);
        int d_left = (yright - yup);
        
        for (int x = x1; x < x2; x++)
        {
            int w1 = (int)((e_up * areaReciprocal) >> 16);
            int w2 = (int)((e_left * areaReciprocal) >> 16);
            
            if (w1 < 0) w1 = 0;
            if (w1 > 65535) w1 = 65535;
            if (w2 < 0) w2 = 0;
            if (w2 > 65535 - w1) w2 = 65535 - w1;
            
            uintpixel_t blended = swrThreeWayBlend(color1, color2, color3, w1, w2, 65535 - w1 - w2);
            alphaBlend(&line[x], blended, blendmode, alpha);
            
            e_up += d_up;
            e_left += d_left;
        }
    }
}

static void swrDrawTriangleTransformed(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, int alpha)
{
    float xup, yup, xleft, yleft, xright, yright;
    uint32_t colorup, colorleft, colorright;
    
    SWRenderer* swr = (SWRenderer*) renderer;
    
    //which vertex is higher?
    xup = x1, yup = y1; colorup = color1;
    xleft = x2, yleft = y2; colorleft = color2;
    xright = x3, yright = y3; colorright = color3;
    if (yup > y2) {
        xup = x2, yup = y2, colorup = color2;
        xleft = x1, yleft = y1, colorleft = color1;
        //xright = x3, yright = y3;
    }
    if (yup > y3) {
        xup = x3, yup = y3, colorup = color3;
        xleft = x1, yleft = y1, colorleft = color1;
        xright = x2, yright = y2, colorright = color2;
    }
    
    if (xleft > xright) {
        float tmp = xleft;
        xleft = xright;
        xright = tmp;
        tmp = yleft;
        yleft = yright;
        yright = tmp;
        uint32_t tmp2 = colorleft;
        colorleft = colorright;
        colorright = tmp2;
    }
    
    swrDrawTriangleInternal(
        swr,
        swrFloor(xup), swrFloor(yup),
        swrFloor(xleft), swrCeiling(yleft),
        swrFloor(xright), swrCeiling(yright),
        colorup,
        colorleft,
        colorright,
        alpha
    );
}

// ==== Exposed interface ====

bool swrSwitchToSurface(Renderer* renderer, int32_t targetSurfaceId, bool restoreOldView)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    if (swr->drawingToSurface) {
        swrCommitShadowWritesToSurfaceIfNeeded(swr, swr->surfaces[swr->currentSurfaceIndex]);
    }
    
    if (targetSurfaceId == RENDER_TARGET_HOST_FRAMEBUFFER)
    {
        if (!swr->drawingToSurface)
            return true;
        
        // restore the original framebuffer
        //logDebug("swr: back to original framebuffer (%d)\n", targetSurfaceId);
        swr->drawingToSurface = false;
        swr->fb = swr->mainFb;
        swr->width = swr->mainWidth;
        swr->height = swr->mainHeight;
        swr->fbPitch = swr->mainPitch;
        swr->currentSurfaceIndex = -1;
        swr->writeMask = WRITE_MASK_ALL;
        
        if (restoreOldView) {
            // restore the old transform, if needed
            swr->viewX = swr->lastViewX;
            swr->viewY = swr->lastViewY;
            swr->viewW = swr->lastViewW;
            swr->viewH = swr->lastViewH;
            swr->portX = swr->lastPortX;
            swr->portY = swr->lastPortY;
            swr->portW = swr->lastPortW;
            swr->portH = swr->lastPortH;
            swr->gameW = swr->lastGameW;
            swr->gameH = swr->lastGameH;
            swr->maxX = swr->lastMaxX;
            swr->maxY = swr->lastMaxY;
            swr->scaleX = swr->lastScaleX;
            swr->scaleY = swr->lastScaleY;
        }
        return true;
    }
    
    if (targetSurfaceId < 0 || (size_t) targetSurfaceId >= swr->surfaceCount || swr->surfaces[targetSurfaceId] == NULL) {
        logError("swr: Invalid surface id %d\n", targetSurfaceId);
        return false;
    }
    
    if (!swr->drawingToSurface)
    {
        // back up the original framebuffer
        swr->drawingToSurface = true;
        swr->mainFb = swr->fb;
        swr->mainWidth = swr->width;
        swr->mainHeight = swr->height;
        swr->mainPitch = swr->fbPitch;
        
        // and the old transform
        swr->lastViewX = swr->viewX;
        swr->lastViewY = swr->viewY;
        swr->lastViewW = swr->viewW;
        swr->lastViewH = swr->viewH;
        swr->lastPortX = swr->portX;
        swr->lastPortY = swr->portY;
        swr->lastPortW = swr->portW;
        swr->lastPortH = swr->portH;
        swr->lastGameW = swr->gameW;
        swr->lastGameH = swr->gameH;
        swr->lastMaxX = swr->maxX;
        swr->lastMaxY = swr->maxY;
        swr->lastScaleX = swr->scaleX;
        swr->lastScaleY = swr->scaleY;
    }
    
    SWTexture* surface = swr->surfaces[targetSurfaceId]->texture;
    swr->fb = surface->buffer;
    swr->width = surface->width;
    swr->height = surface->height;
    swr->fbPitch = surface->width;
    swr->drawingToSurface = true;
    swr->currentSurfaceIndex = targetSurfaceId;
    swr->writeMask = WRITE_MASK_ALL;
    
    swr->viewX = swr->portX = 0;
    swr->viewY = swr->portY = 0;
    swr->maxX = swr->viewW = swr->portW = surface->width;
    swr->maxY = swr->viewH = swr->portH = surface->height;
    swr->scaleX = swr->scaleY = 1.0f;
    
    logDebug("swr: switching to surface %d -> %p, fb %p, %dx%d\n", targetSurfaceId, surface, swr->fb, swr->width, swr->height);
    
    return true;
}

void swrPlotPixel(Renderer* renderer, float x, float y, uintpixel_t color, float alpha)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    int blendmode = swr->blendMode;
    swrPlotPixel_(renderer, (float) x, (float) y, color, blendmode, alpha);
}

void swrDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uintpixel_t color, uintpixel_t color2, float alpha, int alignment)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);
    swrTransformSizeIfNeeded(swr, &width, NULL);
    int iwidth = swrRound(width);
    swrDrawLineInt(renderer, swrFloor(x1), swrFloor(y1), swrFloor(x2), swrFloor(y2), iwidth, color, color2, swrIntAlpha(alpha), alignment);
}

void swrDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color, float alpha)
{
    int x1i = swrRound(x1), x2i = swrRound(x2), y1i = swrRound(y1), y2i = swrRound(y2);
    swrDrawLine(renderer, x1i, y1i, x2i, y1i, 1.0f, color, color, alpha, SWR_LINE_ALIGN_P1);
    swrDrawLine(renderer, x1i, y2i, x2i, y2i, 1.0f, color, color, alpha, SWR_LINE_ALIGN_M1);
    swrDrawLine(renderer, x1i, y1i, x1i, y2i, 1.0f, color, color, alpha, SWR_LINE_ALIGN_P1);
    swrDrawLine(renderer, x2i, y1i, x2i, y2i, 1.0f, color, color, alpha, SWR_LINE_ALIGN_M1);
}

void swrDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, uintpixel_t color4, float alpha)
{
    int x1i = swrRound(x1), x2i = swrRound(x2), y1i = swrRound(y1), y2i = swrRound(y2);
    swrDrawLine(renderer, x1i, y1i, x2i, y1i, 1.0f, color1, color2, alpha, SWR_LINE_ALIGN_P1);
    swrDrawLine(renderer, x1i, y2i, x2i, y2i, 1.0f, color3, color4, alpha, SWR_LINE_ALIGN_M1);
    swrDrawLine(renderer, x1i, y1i, x1i, y2i, 1.0f, color1, color3, alpha, SWR_LINE_ALIGN_P1);
    swrDrawLine(renderer, x2i, y1i, x2i, y2i, 1.0f, color2, color4, alpha, SWR_LINE_ALIGN_M1);
}

// Whether swrFillRectangle with these arguments would paint every pixel of
// the main buffer opaquely: nothing drawn there before it can then show.
bool swrFillCoversMain(SWRenderer* swr, float x1, float y1, float x2, float y2, float alpha)
{
    if (swr->blendMode != bm_normal || swrIntAlpha(alpha) <= 253) return false;
    return swrFillCoversWhole(swr, x1, y1, x2, y2);
}

// Whether swrFillRectangle with these corners would reach every pixel of the main buffer.
bool swrFillCoversWhole(SWRenderer* swr, float x1, float y1, float x2, float y2)
{
    if (swr->fb != swr->mainFb) return false;
    if (swr->portX != 0 || swr->portY != 0 || swr->maxX < swr->mainWidth || swr->maxY < swr->mainHeight) return false;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);
    int x1i = swrFloor(x1), x2i = swrCeiling(x2), y1i = swrFloor(y1), y2i = swrCeiling(y2);
    int xd = x2i - x1i;
    int yd = y2i - y1i;
    if (xd < 0) { x1i = x2i; xd = -xd; }
    if (yd < 0) { y1i = y2i; yd = -yd; }
    if (xd <= 0 || yd <= 0) return false;
    return x1i <= 0 && y1i <= 0 && x1i + xd >= swr->mainWidth && y1i + yd + 1 >= swr->mainHeight;
}

void swrFillRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t pxcolor, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);

    int alphaInt = swrIntAlpha(alpha);
    int x1i = swrFloor(x1), x2i = swrCeiling(x2), y1i = swrFloor(y1), y2i = swrCeiling(y2);
    int xd = x2i - x1i;
    int yd = y2i - y1i;
    if (xd < 0) { x1i = x2i; xd = -xd; }
    if (yd < 0) { y1i = y2i; yd = -yd; }
    bool clearTaken = swr->clearHeldForFill;
    swr->clearHeldForFill = false;
    if (xd <= 0 || yd <= 0) {
        swr->clearHeld = clearTaken; // nothing drawn: held as before
        return;
    }
    
    // A fill of the whole main buffer. While the buffer is known to hold one
    // colour, the fill turns it into one other colour that can be worked out
    // without touching it: the fill's own if opaque, the blend of the two if
    // translucent. Nothing is written when that is the colour already there
    // (a game darkening a black screen, or clearing one twice), and one plain
    // fill replaces a read-blend-write of every pixel otherwise.
    bool wholeBuffer = swr->fb == swr->mainFb && swr->blendMode == bm_normal && alphaInt >= 4 &&
                       swr->portX == 0 && swr->portY == 0 && swr->maxX >= swr->mainWidth && swr->maxY >= swr->mainHeight &&
                       x1i <= 0 && y1i <= 0 && x1i + xd >= swr->mainWidth && y1i + yd + 1 >= swr->mainHeight;
    bool opaque = alphaInt > 253;
    // The result is itself held back as a clear (swrClearSettle) when the
    // buffer is exactly what a clear covers: a room's tiles tend to follow.
    bool holdable = swr->fbPitch == swr->mainWidth;
    if (clearTaken && !holdable) {
        swr->clearHeld = true;
        swrClearSettle(swr);
        clearTaken = false;
    }
#ifdef SW_HAS_PREMUL_BLEND
    if (wholeBuffer && swr->uniformKept) {
        uintpixel_t result = opaque ? pxcolor
            : swrBlendPremultiplied(swr->uniformColor, swrSpreadRedBlue(pxcolor) * (uint32_t) alphaInt,
                                    swrGreen(pxcolor) * (uint32_t) alphaInt, (uint32_t) (256 - alphaInt));
        if (holdable && (clearTaken || result != swr->uniformColor)) {
            swr->clearHeld = true;
            swr->clearHeldColor = result;
        } else if (result != swr->uniformColor) {
            swrDirtyAll(swr);
            for (int y = 0; y < swr->mainHeight; y++)
                swrFillPixels(&swr->fb[y * swr->fbPitch], (size_t) swr->mainWidth, result);
        }
        swr->uniformColor = result;
        swr->uniformValid = true;
        return;
    }
#endif
    if (clearTaken) {
        swr->clearHeld = true;
        swrClearSettle(swr);
    }
    if (wholeBuffer && opaque && holdable) {
        swr->clearHeld = true;
        swr->clearHeldColor = pxcolor;
        swr->uniformValid = true;
        swr->uniformColor = pxcolor;
        return;
    }
    
    for (int y = 0; y <= yd; y++) {
        swrDrawHLineInt(renderer, x1i, y1i + y, xd, pxcolor, pxcolor, alphaInt);
    }
    if (wholeBuffer && opaque) {
        swr->uniformValid = true;
        swr->uniformColor = pxcolor;
    }
}

void swrFillRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uintpixel_t pxcolor1, uintpixel_t pxcolor2, uintpixel_t pxcolor3, uintpixel_t pxcolor4, float alpha)
{
    if (pxcolor1 == pxcolor2 && pxcolor2 == pxcolor3 && pxcolor3 == pxcolor4) {
        swrFillRectangle(renderer, x1, y1, x2, y2, pxcolor1, alpha);
        return;
    }
    
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);

    int alphaInt = swrIntAlpha(alpha);
    int x1i = swrFloor(x1), x2i = swrCeiling(x2), y1i = swrFloor(y1), y2i = swrCeiling(y2);
    int xd = x2i - x1i;
    int yd = y2i - y1i;
    if (xd < 0) { x1i = x2i; xd = -xd; }
    if (yd < 0) { y1i = y2i; yd = -yd; }
    if (xd <= 0 || yd <= 0) return;
    
#ifndef SW_DITHERED_BLENDING
    uint32_t inc = (65536U << 14) / yd;
    uint32_t weightfp = 0;
    for (int y = 0; y <= yd; y++, weightfp += inc)
    {
        uint32_t weight2 = (uint32_t)(weightfp >> 14);
        if (weight2 > 65535) weight2 = 65535;
        uint32_t weight1 = 65535 - weight2;
        
        uintpixel_t intcolor1, intcolor2;
        intcolor1 = swrTwoWayBlend(pxcolor1, pxcolor4, (uint16_t) weight1, (uint16_t) weight2);
        intcolor2 = swrTwoWayBlend(pxcolor2, pxcolor3, (uint16_t) weight1, (uint16_t) weight2);
        
        swrDrawHLineInt(renderer, x1i, y1i + y, xd, intcolor1, intcolor2, alphaInt);
    }
#else
    // Dithered blending CANNOT use the above code because of its inherent randomness.
    // It chooses either one color or the other based on the alpha / probability.
    // As such, we need a slightly more complex four-way operation.
    int blendmode = swr->blendMode;
    
    uint32_t incx = (65536U << 14) / xd;
    uint32_t incy = (65536U << 14) / yd;
    uint32_t weightfpy = 0;
    for (int y = 0, ay = y1i; y <= yd; y++, ay++, weightfpy += incy)
    {
        uint32_t weightD = (uint32_t)(weightfpy >> 14);
        if (weightD > 65535) weightD = 65535;
        uint32_t weightU = 65535 - weightD;
        
        uintpixel_t *line = &swr->fb[ay * swr->fbPitch + x1i];
        swrDirtyRow(swr, ay, x1i, x1i + xd + 1);
        uint32_t weightfpx = 0;
        for (int x = 0; x <= xd; x++, weightfpx += incx)
        {
            if (UNLIKELY(srcalpha < 250)) {
                if ((swrFastRng() & 0xFF) >= srcalpha)
                    continue;
            }
            
            uint32_t weightR = (uint32_t)(weightfpx >> 14);
            if (weightR > 65535) weightR = 65535;
            uint32_t weightL = 65535 - weightR;
            
            uint16_t weight1 = (uint16_t)((weightL * weightU) >> 16);
            uint16_t weight2 = (uint16_t)((weightR * weightU) >> 16);
            uint16_t weight3 = (uint16_t)((weightR * weightD) >> 16);
            uint16_t weight4 = (uint16_t)((weightL * weightD) >> 16);
            
            uintpixel_t result = swrFourWayBlend(pxcolor1, pxcolor2, pxcolor3, pxcolor4, weight1, weight2, weight3, weight4);
            alphaBlend(&line[x], result, blendmode, 256, 0);
        }
    }
#endif
}

// A trail: one translucent sprite stamped again and again a pixel or two
// along, each stamp a little more opaque (Deltarune's attack burst draws
// about 80 in a row, so every pixel under it is blended some 24 times).
// With speed favoured, every second stamp is left out and the one after it
// is drawn with both stamps' opacity, 1 - (1 - a)(1 - b): half the blending
// for a picture that differs by a step of a pixel or two along the trail.
// Only plain sprite draws (SWRenderer_drawSprite, which allows overlay
// merging) take part, and anything else drawn in between ends the trail
// (swrOverlayFlush). A stamp left out is drawn after all when the next
// sprite is not the trail's; one followed by something else is lost.
#ifdef SW_HAS_PREMUL_BLEND
#define SWR_TRAIL_STEP 3      // the furthest a stamp may be from the last one, in pixels each way
#define SWR_TRAIL_MIN_SIDE 16 // smaller sprites (text, sparks) are left alone
typedef struct {
    bool known;
    const uintpixel_t* fb;
    SWTexture* texture;
    int dx, dy, dw, dh, sx, sy, sw, sh, alpha;
    uintpixel_t tint;
} SWTrailStamp;
static SWTrailStamp swrTrailLast;   // the stamp drawn last, while the trail goes on
static SWTrailStamp swrTrailMissed; // a stamp left out, its opacity to go with the next
static bool swrTrailDrawing = false;

static bool swrTrailFollows(const SWTrailStamp* before, const SWTrailStamp* stamp)
{
    if (!before->known || before->fb != stamp->fb || before->texture != stamp->texture) return false;
    if (before->sx != stamp->sx || before->sy != stamp->sy || before->sw != stamp->sw || before->sh != stamp->sh) return false;
    if (before->dw != stamp->dw || before->dh != stamp->dh || before->tint != stamp->tint) return false;
    int ax = stamp->dx - before->dx, ay = stamp->dy - before->dy;
    return ax >= -SWR_TRAIL_STEP && ax <= SWR_TRAIL_STEP && ay >= -SWR_TRAIL_STEP && ay <= SWR_TRAIL_STEP;
}
#endif

// Something other than a trail's next stamp is being drawn.
void swrTrailEnd(void)
{
#ifdef SW_HAS_PREMUL_BLEND
    if (swrTrailDrawing) return;
    swrTrailLast.known = false;
    swrTrailMissed.known = false;
#endif
}

void swrDrawSprite(
    Renderer* renderer, float dx, float dy, float dw, float dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uint32_t tintColor, float alpha
)
{
    SWRenderer *swr = (SWRenderer*) renderer;
    
    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &dw, &dh);
    
#ifdef SW_HAS_PREMUL_BLEND
    if (swrFavorSpeed && swr->overlayMergeAllowed) {
        SWTrailStamp stamp = { true, swr->fb, texture, swrFloor(dx), swrFloor(dy), swrCeiling(dw), swrCeiling(dh),
                               sx, sy, sw, sh, swrIntAlpha(alpha), swrConvertPixel(tintColor) };
        int sideW = stamp.dw < 0 ? -stamp.dw : stamp.dw, sideH = stamp.dh < 0 ? -stamp.dh : stamp.dh;
        bool translucent = swr->blendMode == bm_normal && stamp.alpha >= 4 && stamp.alpha <= 253 &&
                           sideW >= SWR_TRAIL_MIN_SIDE && sideH >= SWR_TRAIL_MIN_SIDE;
        if (translucent && swrTrailFollows(&swrTrailMissed, &stamp)) {
            int missed = swrTrailMissed.alpha;
            stamp.alpha = missed + stamp.alpha - (missed * stamp.alpha + 128) / 256;
            swrTrailMissed.known = false;
        } else if (translucent && !swrTrailMissed.known && swrTrailFollows(&swrTrailLast, &stamp)) {
            swrTrailMissed = stamp;
            return;
        }
        swrTrailDrawing = true;
        if (swrTrailMissed.known) // the trail ends with a stamp left out: it goes down after all, before this
            swrDrawSpriteInternal(renderer, swrTrailMissed.dx, swrTrailMissed.dy, swrTrailMissed.dw, swrTrailMissed.dh, swrTrailMissed.texture,
                                  swrTrailMissed.sx, swrTrailMissed.sy, swrTrailMissed.sw, swrTrailMissed.sh, swrTrailMissed.tint, swrTrailMissed.alpha, false, SWR_CONTRAST_NONE);
        swrDrawSpriteInternal(renderer, stamp.dx, stamp.dy, stamp.dw, stamp.dh, texture, sx, sy, sw, sh, stamp.tint, stamp.alpha, false, SWR_CONTRAST_NONE);
        swrTrailDrawing = false;
        swrTrailMissed.known = false;
        swrTrailLast = stamp;
        swrTrailLast.known = translucent;
        return;
    }
#endif
    
    swrDrawSpriteInternal(
        renderer,
        swrFloor(dx),
        swrFloor(dy),
        swrCeiling(dw),
        swrCeiling(dh),
        texture,
        sx, sy,
        sw, sh,
        swrConvertPixel(tintColor),
        swrIntAlpha(alpha), false, SWR_CONTRAST_NONE
    );
}

void swrDrawDoubledGlyph(Renderer* renderer, float dx, float dy, float dw, float dh,
                         SWTexture* texture, int sx, int sy, int sw, int sh, uint32_t tintColor, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    float screenW = dw, screenH = dh;
    swrTransformSizeIfNeeded(swr, &screenW, &screenH);
    if (swrCeiling(screenW) * 2 != sw || swrCeiling(screenH) * 2 != sh) {
        swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, tintColor, alpha);
        return;
    }
    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrOverlayFlush(swr);
    swrDrawSpriteInternal(renderer, swrFloor(dx), swrFloor(dy), sw / 2, sh / 2,
                          texture, sx, sy, sw, sh, swrConvertPixel(tintColor), swrIntAlpha(alpha), true, SWR_CONTRAST_NONE);
}

// Build a tiny derivative once per immutable source lifetime. Source identity
// includes every texel, so localized/replaced lettering never receives a patch.
// Neither the original texture nor its shared half-size cache is modified.
bool swrDrawHintedGlyph(Renderer* renderer, float dx, float dy, float dw, float dh,
                        SWTexture* texture, int sx, int sy, int sw, int sh,
                        uint32_t tintColor, float alpha, const SWHintedGlyphAsset* asset)
{
#ifdef SW_HAS_PREMUL_BLEND
    SWRenderer* swr = (SWRenderer*) renderer;
    float screenW = dw, screenH = dh;
    swrTransformSizeIfNeeded(swr, &screenW, &screenH);
    if (!swr->contrastEnabled || !swrFavorSpeed || !swrSmoothMinify || swr->drawingToSurface ||
        swr->scaleX != 0.5f || swr->scaleY != 0.5f || !texture->immutable || swr->blendMode != bm_normal ||
        screenW <= 0 || screenH <= 0 || screenW * 2 != sw || screenH * 2 != sh ||
        sw != asset->width || sh != asset->height || sw > 64 || sh > 16 ||
        sx < 0 || sy < 0 || sx > texture->width - sw || sy > texture->height - sh ||
        asset->patchWidth > 8 || asset->patchX + asset->patchWidth > (sw + 1) / 2) return false;
    SWGlyphHintCache* cache;
    unsigned count = 0;
    for (cache = texture->glyphHints; cache; cache = cache->next) {
        ++count;
        if (cache->sx == sx && cache->sy == sy && cache->asset.width == sw && cache->asset.height == sh &&
            cache->asset.sourceHash == asset->sourceHash && cache->asset.patchX == asset->patchX &&
            cache->asset.patchWidth == asset->patchWidth &&
            memcmp(cache->asset.patchRows, asset->patchRows, sizeof(asset->patchRows)) == 0) break;
    }
    if (!cache) {
        if (count >= 16) return false;
        cache = (SWGlyphHintCache*) calloc(1, sizeof(*cache));
        if (!cache) return false;
        cache->asset = *asset; cache->sx = sx; cache->sy = sy;
        cache->next = texture->glyphHints; texture->glyphHints = cache;
        uint32_t hash = 2166136261u;
        bool white = true;
        for (int y = 0; y < sh; ++y) {
            for (int x = 0; x < sw; ++x) {
                uintpixel_t pixel = texture->buffer[(sy + y) * texture->width + sx + x];
                white &= pixel == 0 || pixel == 0xffff;
                hash = (hash ^ (pixel & 255)) * 16777619u;
                hash = (hash ^ (pixel >> 8)) * 16777619u;
                if ((x & 1) == 0 && (y & 1) == 0) cache->pixels[(y / 2) * ((sw + 1) / 2) + x / 2] = pixel;
            }
        }
        cache->valid = white && hash == asset->sourceHash;
        if (cache->valid) {
            for (int y = 0; y < (sh + 1) / 2; ++y)
                for (int x = 0; x < asset->patchWidth; ++x)
                    cache->pixels[y * ((sw + 1) / 2) + asset->patchX + x] =
                        asset->patchRows[y] & (1u << (asset->patchWidth - 1 - x)) ? 0xffff : 0;
        }
    }
    if (!cache->valid) return false;
    SWTexture hinted = {0};
    hinted.buffer = cache->pixels;
    hinted.width = (uint16_t) ((sw + 1) / 2); hinted.height = (uint16_t) ((sh + 1) / 2);
    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrOverlayFlush(swr);
    // The wrapper is local: never let deferred overlay merging retain its address.
    bool mergeAllowed = swr->overlayMergeAllowed;
    swr->overlayMergeAllowed = false;
    swrDrawSpriteInternal(renderer, swrFloor(dx), swrFloor(dy), hinted.width, hinted.height,
                          &hinted, 0, 0, hinted.width, hinted.height, swrConvertPixel(tintColor),
                          swrIntAlpha(alpha), true, SWR_CONTRAST_NONE);
    swr->overlayMergeAllowed = mergeAllowed;
    return true;
#else
    (void) renderer; (void) dx; (void) dy; (void) dw; (void) dh; (void) texture;
    (void) sx; (void) sy; (void) sw; (void) sh; (void) tintColor; (void) alpha; (void) asset;
    return false;
#endif
}

// The platform selects only battle UI assets. Surfaces, rotated/flipped/scaled
// alternatives and non-normal blending retain their ordinary rendering. Hybrid
// buttons and HUD ink mix center and box sampling; black bubble ink also
// increases partial coverage. These paths leave shared texture caches unchanged.
void swrDrawContrastSprite(Renderer* renderer, float dx, float dy, float dw, float dh,
                           SWTexture* texture, int sx, int sy, int sw, int sh,
                           uint32_t tintColor, float alpha, int contrast)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    float screenW = dw, screenH = dh;
    swrTransformSizeIfNeeded(swr, &screenW, &screenH);
    bool eligible = swr->contrastEnabled && swrFavorSpeed && swrSmoothMinify && !swr->drawingToSurface &&
                    swr->scaleX == 0.5f && swr->scaleY == 0.5f &&
                    texture->immutable && swr->blendMode == bm_normal &&
                    screenW > 0 && screenH > 0 && screenW * 2 == sw && screenH * 2 == sh &&
                    (contrast != SWR_CONTRAST_BUBBLE_GLYPH || (tintColor & 0xFFFFFF) == 0);
    if (!eligible) {
        swrDrawSprite(renderer, dx, dy, dw, dh, texture, sx, sy, sw, sh, tintColor, alpha);
        return;
    }
    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrOverlayFlush(swr);
    bool fullTexels = contrast == SWR_CONTRAST_BUTTON || contrast == SWR_CONTRAST_UI_GLYPH || contrast == SWR_CONTRAST_BUBBLE_GLYPH;
    swrDrawSpriteInternal(renderer, swrFloor(dx), swrFloor(dy), swrCeiling(screenW), swrCeiling(screenH),
                          texture, sx, sy, sw, sh, swrConvertPixel(tintColor), swrIntAlpha(alpha), fullTexels, contrast);
}

// ===[ Tiled sprites ]===
// A sprite tiled over the room, unscaled, is drawn in passes over the
// screen's rows, not as a sprite draw for every copy in view (hundreds for a
// small tile, most of what each costs being getting started). Every screen
// row takes its stretch of the tile's row (see SWTexture.rowBounds) once per
// copy. The pixels come out as those draws leave them: the same texels, from
// the half-size copy when the room is drawn at half size with speed favoured,
// put down with the same arithmetic.
//
// Measured on the Pocket, a pass of a tile of thin lines cost 6 ms for about
// 6,000 pixels whatever was done about reading the buffer or about the
// blend's arithmetic: each pixel is on a cache line of its own, and a store
// to a line that is not in the cache costs about a microsecond. So a pass
// drawn straight after a clear of the whole buffer is held with the clear
// (swrTiledHold), and the two are written together, a row at a time: the
// row is filled, and the pass's pixels go onto lines that the fill has just
// brought in.
#if PIXEL_SIZE == 16 && defined SW_HAS_PREMUL_BLEND && !defined SW_DITHERED_BLENDING
#define SWR_TILED_OWN_SIDE 64   // the largest tile a held pass keeps a copy of
#define SWR_TILED_HELD_MAX 2

typedef struct {
    const uintpixel_t* texels;  // rows of `pitch`, the copy's top left at [0]
    const uint8_t* coverage;    // the same for a half-size copy's coverage, or NULL
    const uint16_t* bounds;     // each row's first opaque column and the one after the last, counted from `boundsBase`
    int pitch, boundsBase;
    int width, height, stepX, stepY, originX, originY, countX, countY, alpha;
    int minX, minY, maxX, maxY;
    // A held pass outlives the draw call, and the texture might not: it keeps its own copy of the tile.
    uintpixel_t ownTexels[SWR_TILED_OWN_SIDE * SWR_TILED_OWN_SIDE];
    uint8_t ownCoverage[SWR_TILED_OWN_SIDE * SWR_TILED_OWN_SIDE];
    uint16_t ownBounds[SWR_TILED_OWN_SIDE * 2];
} SWTiledPass;

static SWTiledPass swrTiledHeld[SWR_TILED_HELD_MAX];

// Fills held over those passes (swrFillHold), as swrBlendPremultiplied takes them.
#define SWR_FILL_HELD_MAX 2
#define SWR_HELD_ROW_MAX 1024
static struct { uint32_t redBlue, green, inverse; } swrFillHeld[SWR_FILL_HELD_MAX];
static int swrFillHeldCount = 0;
static SWTiledPass swrTiledNow; // the pass being drawn or offered for holding

// What one pixel of a copy leaves where `under` was: the tile's texel (with
// its coverage, for a half-size copy) put down at `alpha`, as the sprite
// draws do it. Returns `under` where the tile has nothing.
FORCE_INLINE uintpixel_t swrTiledPixel(uintpixel_t under, uintpixel_t pixel, const uint8_t* covered, int alpha)
{
    if (covered != NULL) {
        uint32_t count = *covered;
        if (count == 0) return under;
        int coverageAlpha = count == 4 ? alpha : (alpha * (int) count) >> 2;
        if (coverageAlpha > 253) return pixel;
        if (coverageAlpha < 4) return under;
        return swrBlendPremultiplied(under, swrSpreadRedBlue(pixel) * coverageAlpha, swrGreen(pixel) * coverageAlpha, 256 - coverageAlpha);
    }
    if (!swrIsOpaque(pixel)) return under;
    if (alpha > 253) return pixel;
    return swrBlendPremultiplied(under, swrSpreadRedBlue(pixel) * (uint32_t) alpha, swrGreen(pixel) * (uint32_t) alpha, (uint32_t) (256 - alpha));
}

// swrTiledPixel with the last answer kept: a tile of lines is one colour at
// one or two coverages over one colour.
typedef struct {
    bool filled;
    uintpixel_t under, pixel, result;
    uint8_t covered;
} SWTiledMemo;

FORCE_INLINE uintpixel_t swrTiledPixelMemo(SWTiledMemo* memo, uintpixel_t under, uintpixel_t pixel, const uint8_t* covered, int alpha)
{
    uint8_t count = covered != NULL ? *covered : 0;
    if (memo->filled && memo->under == under && memo->pixel == pixel && memo->covered == count) return memo->result;
    memo->filled = true;
    memo->under = under; memo->pixel = pixel; memo->covered = count;
    memo->result = swrTiledPixel(under, pixel, covered, alpha);
    return memo->result;
}

// Works out the pass for these arguments (see swrDrawSpriteTiledRows).
// Returns false when the draws would not be that simple.
static bool swrTiledPassSetUp(SWRenderer* swr, SWTexture* texture, int sx, int sy, int sw, int sh,
                              int firstX, int firstY, int tileW, int tileH, int countX, int countY,
                              uint32_t color, float alphaf, SWTiledPass* pass)
{
    int alpha = swrIntAlpha(alphaf);
    if (swr->blendMode != bm_normal || alpha <= 8 || !texture->immutable) return false;
    if ((swrConvertPixel(color) & 0x7FFF) != 0x7FFF) return false; // tinted
    if (sw <= 0 || sh <= 0 || tileW <= 0 || tileH <= 0 || countX <= 0 || countY <= 0) return false;
    if (sx < 0 || sy < 0 || sx + sw > texture->width || sy + sh > texture->height) return false;
    
    // What a copy is drawn from, and how far apart the copies land on the screen.
    const uintpixel_t* buffer;
    const uint16_t* bounds;
    const uint8_t* coverage = NULL;
    int pitch, srcX, srcY, width, height, stepX, stepY;
    if (swr->scaleX == 1.0f && swr->scaleY == 1.0f) {
        bounds = swrRowBounds(texture);
        buffer = texture->buffer;
        pitch = texture->width;
        srcX = sx; srcY = sy; width = sw; height = sh;
        stepX = tileW; stepY = tileH;
    } else if (swr->scaleX == 0.5f && swr->scaleY == 0.5f && swrFavorSpeed) {
        // Even sizes only: each copy then starts on a whole pixel, the same part of one as the last.
        if (((sw | sh | tileW | tileH) & 1) != 0) return false;
        if (!swrHalfTexture(texture, sx, sy)) return false;
        if (((sx + texture->halfPhaseX) & 1) != 0 || ((sy + texture->halfPhaseY) & 1) != 0) return false;
        int halfW = (texture->width + texture->halfPhaseX + 1) / 2, halfH = (texture->height + texture->halfPhaseY + 1) / 2;
        bounds = texture->halfRowBounds;
        buffer = texture->halfBuffer;
        coverage = texture->halfCoverage;
        pitch = halfW;
        srcX = (sx + texture->halfPhaseX) / 2; srcY = (sy + texture->halfPhaseY) / 2;
        width = sw / 2; height = sh / 2;
        if (srcX + width > halfW) width = halfW - srcX;
        if (srcY + height > halfH) height = halfH - srcY;
        stepX = tileW / 2; stepY = tileH / 2;
    } else {
        return false;
    }
    if (bounds == NULL || width <= 0 || height <= 0 || width > stepX || height > stepY) return false;
    
    // Where the first copy lands, as swrDrawSprite places it.
    float fx = (float) firstX, fy = (float) firstY;
    swrTransformPosIfNeeded(swr, &fx, &fy);
    pass->originX = swrFloor(fx); pass->originY = swrFloor(fy);
    pass->minX = swr->portX; pass->minY = swr->portY;
    pass->maxX = swr->portX + swr->portW; pass->maxY = swr->portY + swr->portH;
    if (pass->maxX > swr->maxX) pass->maxX = swr->maxX;
    if (pass->maxY > swr->maxY) pass->maxY = swr->maxY;
    
    pass->texels = &buffer[srcY * pitch + srcX];
    pass->coverage = coverage != NULL ? &coverage[srcY * pitch + srcX] : NULL;
    pass->bounds = &bounds[srcY * 2];
    pass->pitch = pitch;
    pass->boundsBase = srcX;
    pass->width = width; pass->height = height;
    pass->stepX = stepX; pass->stepY = stepY;
    pass->countX = countX; pass->countY = countY;
    pass->alpha = alpha;
    return true;
}

// What the fills being held make of a colour, the last answer kept: a clear
// and a pass or two leave few colours to ask about.
typedef struct { uintpixel_t from, to; bool known; } SWFilledMemo;

FORCE_INLINE uintpixel_t swrFilled(SWFilledMemo* memo, int fills, uintpixel_t under)
{
    if (memo->known && memo->from == under) return memo->to;
    uintpixel_t result = under;
    for (int f = 0; f < fills; f++)
        result = swrBlendPremultiplied(result, swrFillHeld[f].redBlue, swrFillHeld[f].green, swrFillHeld[f].inverse);
    memo->from = under;
    memo->to = result;
    memo->known = true;
    return result;
}

// Draws what the pass puts on row y of the buffer, whose pixels are at dstline.
// With `filled`, dstline is a copy of the row as it is without the fills held
// over the passes, and each pixel written there also goes to `filled` as the
// fills leave it.
FORCE_INLINE void swrTiledPassRow(const SWTiledPass* pass, int y, uintpixel_t* dstline, SWTiledMemo* memo,
                                  uintpixel_t* filled, int fills, SWFilledMemo* filledMemo)
{
    int fromOrigin = y - pass->originY;
    if (y < pass->minY || y >= pass->maxY || fromOrigin < 0) return;
    int copyY = fromOrigin / pass->stepY, row = fromOrigin - copyY * pass->stepY;
    if (copyY >= pass->countY || row >= pass->height) return;
    
    // The stretch of this row of the tile that has anything in it, in columns of the copy.
    int from = (int) pass->bounds[row * 2] - pass->boundsBase, to = (int) pass->bounds[row * 2 + 1] - pass->boundsBase;
    if (from < 0) from = 0;
    if (to > pass->width) to = pass->width;
    if (from >= to) return;
    
    const uintpixel_t* srcline = &pass->texels[row * pass->pitch];
    const uint8_t* covline = pass->coverage != NULL ? &pass->coverage[row * pass->pitch] : NULL;
    // Start at the first copy whose stretch reaches the port: in a wide room most lie to its left.
    int copyX = 0, left = pass->originX;
    if (left + to <= pass->minX) {
        copyX = (pass->minX - to - left) / pass->stepX + 1;
        left += copyX * pass->stepX;
    }
    for (; copyX < pass->countX && left + from < pass->maxX; copyX++, left += pass->stepX)
    {
        int x0 = left + from, x1 = left + to;
        if (x1 <= pass->minX) continue;
        if (x0 < pass->minX) x0 = pass->minX;
        if (x1 > pass->maxX) x1 = pass->maxX;
        for (int x = x0; x < x1; x++) {
            const uint8_t* covered = covline != NULL ? &covline[x - left] : NULL;
            if (covered != NULL ? *covered != 0 : swrIsOpaque(srcline[x - left])) {
                uintpixel_t result = swrTiledPixelMemo(memo, dstline[x], srcline[x - left], covered, pass->alpha);
                dstline[x] = result;
                if (filled != NULL) filled[x] = swrFilled(filledMemo, fills, result);
            }
        }
    }
}
#endif

// Draws the texture's part (sx, sy, sw, sh) at (firstX, firstY) in the room
// and again every tileW and tileH, countX by countY times. Returns false,
// having drawn nothing, when the draws would not be that simple.
bool swrDrawSpriteTiledRows(SWRenderer* swr, SWTexture* texture, int sx, int sy, int sw, int sh,
                            int firstX, int firstY, int tileW, int tileH, int countX, int countY,
                            uint32_t color, float alphaf)
{
#if PIXEL_SIZE != 16 || !defined SW_HAS_PREMUL_BLEND || defined SW_DITHERED_BLENDING
    (void) swr; (void) texture; (void) sx; (void) sy; (void) sw; (void) sh; (void) firstX; (void) firstY;
    (void) tileW; (void) tileH; (void) countX; (void) countY; (void) color; (void) alphaf;
    return false;
#else
    if (!swrTiledPassSetUp(swr, texture, sx, sy, sw, sh, firstX, firstY, tileW, tileH, countX, countY, color, alphaf, &swrTiledNow)) return false;
    SWTiledMemo memo = { 0 };
    swrDirtyRect(swr, 0, swrTiledNow.minY, swr->width, swrTiledNow.maxY);
    for (int y = swrTiledNow.minY; y < swrTiledNow.maxY; y++)
        swrTiledPassRow(&swrTiledNow, y, &swr->fb[y * swr->fbPitch], &memo, NULL, 0, NULL);
    return true;
#endif
}

// The same draw held back, to be written together with the clear of the
// whole main buffer that is being held (the caller has checked that one is,
// with nothing over it but earlier passes). Returns false, holding nothing,
// when the pass cannot be held: the caller then draws it.
bool swrTiledHold(SWRenderer* swr, SWTexture* texture, int sx, int sy, int sw, int sh,
                  int firstX, int firstY, int tileW, int tileH, int countX, int countY,
                  uint32_t color, float alphaf)
{
#if PIXEL_SIZE != 16 || !defined SW_HAS_PREMUL_BLEND || defined SW_DITHERED_BLENDING
    (void) swr; (void) texture; (void) sx; (void) sy; (void) sw; (void) sh; (void) firstX; (void) firstY;
    (void) tileW; (void) tileH; (void) countX; (void) countY; (void) color; (void) alphaf;
    return false;
#else
    if (swr->tiledHeldCount >= SWR_TILED_HELD_MAX || swr->fb != swr->mainFb || swr->fbPitch != swr->mainWidth) return false;
    if (swrFillHeldCount > 0) return false; // it would go under the fills held over the earlier passes
    SWTiledPass* pass = &swrTiledHeld[swr->tiledHeldCount];
    if (!swrTiledPassSetUp(swr, texture, sx, sy, sw, sh, firstX, firstY, tileW, tileH, countX, countY, color, alphaf, pass)) return false;
    if (pass->width > SWR_TILED_OWN_SIDE || pass->height > SWR_TILED_OWN_SIDE) return false;
    
    // Its own copy of the tile, rows of `width`, with bounds counted from column 0.
    for (int row = 0; row < pass->height; row++) {
        memcpy(&pass->ownTexels[row * pass->width], &pass->texels[row * pass->pitch], (size_t) pass->width * sizeof(uintpixel_t));
        if (pass->coverage != NULL) memcpy(&pass->ownCoverage[row * pass->width], &pass->coverage[row * pass->pitch], (size_t) pass->width);
        int from = (int) pass->bounds[row * 2] - pass->boundsBase, to = (int) pass->bounds[row * 2 + 1] - pass->boundsBase;
        if (from < 0) from = 0;
        if (to > pass->width) to = pass->width;
        if (to < from) to = from;
        pass->ownBounds[row * 2] = (uint16_t) from;
        pass->ownBounds[row * 2 + 1] = (uint16_t) to;
    }
    pass->texels = pass->ownTexels;
    if (pass->coverage != NULL) pass->coverage = pass->ownCoverage;
    pass->bounds = pass->ownBounds;
    pass->pitch = pass->width;
    pass->boundsBase = 0;
    swr->tiledHeldCount++;
    return true;
#endif
}

// A translucent fill of the whole main buffer that comes straight after the
// held clear and its tiled passes (a battle darkening its background) is held
// with them: read, blend and write of every pixel of the buffer, which on the
// Pocket is memory outside the cache and costs a quarter of a microsecond a
// pixel to read (28 ms for the screen), becomes a blend of each colour the
// rows are being put together from. Returns false, holding nothing, when it
// cannot join; the caller then draws it.
bool swrFillHold(SWRenderer* swr, uintpixel_t pxcolor, int alphaInt)
{
#if PIXEL_SIZE != 16 || !defined SW_HAS_PREMUL_BLEND || defined SW_DITHERED_BLENDING
    (void) swr; (void) pxcolor; (void) alphaInt;
    return false;
#else
    if (swr->tiledHeldCount <= 0 || swrFillHeldCount >= SWR_FILL_HELD_MAX || swr->mainWidth > SWR_HELD_ROW_MAX) return false;
    swrFillHeld[swrFillHeldCount].redBlue = swrSpreadRedBlue(pxcolor) * (uint32_t) alphaInt;
    swrFillHeld[swrFillHeldCount].green = swrGreen(pxcolor) * (uint32_t) alphaInt;
    swrFillHeld[swrFillHeldCount].inverse = (uint32_t) (256 - alphaInt);
    swrFillHeldCount++;
    return true;
#endif
}

void swrFillHeldDrop(void)
{
#if PIXEL_SIZE == 16 && defined SW_HAS_PREMUL_BLEND && !defined SW_DITHERED_BLENDING
    swrFillHeldCount = 0;
#endif
}

#if PIXEL_SIZE == 16 && defined SW_HAS_PREMUL_BLEND && !defined SW_DITHERED_BLENDING
// Whether every held pass repeats across the whole of each row it touches
// with the same step: copies from the row's left end to its right, none
// clipped by a port narrower than the buffer. A battle's scrolling grid is
// that. Each row is then one stretch of `step` pixels over and over.
#define SWR_TILED_PERIOD_MAX 256
static bool swrTiledHeldRepeats(const SWRenderer* swr, int count, int* step)
{
    int period = swrTiledHeld[0].stepX;
    if (period <= 0 || period > SWR_TILED_PERIOD_MAX || swr->mainWidth > SWR_HELD_ROW_MAX) return false;
    for (int p = 0; p < count; p++) {
        const SWTiledPass* pass = &swrTiledHeld[p];
        if (pass->stepX != period || pass->minX > 0 || pass->maxX < swr->mainWidth) return false;
        if (pass->originX > 0 || pass->originX + pass->countX * period < swr->mainWidth) return false;
    }
    *step = period;
    return true;
}

// What the pass puts on row y, into one stretch of the row: `pattern[i]`
// stands for every pixel x of the row with x % period == i. The same pixels
// as swrTiledPassRow, from one copy instead of every copy across the row.
FORCE_INLINE void swrTiledPassPattern(const SWTiledPass* pass, int y, uintpixel_t* pattern, int period, SWTiledMemo* memo)
{
    int fromOrigin = y - pass->originY;
    if (y < pass->minY || y >= pass->maxY || fromOrigin < 0) return;
    int copyY = fromOrigin / pass->stepY, row = fromOrigin - copyY * pass->stepY;
    if (copyY >= pass->countY || row >= pass->height) return;
    int from = (int) pass->bounds[row * 2] - pass->boundsBase, to = (int) pass->bounds[row * 2 + 1] - pass->boundsBase;
    if (from < 0) from = 0;
    if (to > pass->width) to = pass->width;
    
    const uintpixel_t* srcline = &pass->texels[row * pass->pitch];
    const uint8_t* covline = pass->coverage != NULL ? &pass->coverage[row * pass->pitch] : NULL;
    // A copy's column `col` lands on x = originX + copy * period + col.
    int at = (pass->originX + from) % period;
    if (at < 0) at += period;
    for (int col = from; col < to; col++, at = at + 1 == period ? 0 : at + 1) {
        const uint8_t* covered = covline != NULL ? &covline[col] : NULL;
        if (covered != NULL ? *covered != 0 : swrIsOpaque(srcline[col]))
            pattern[at] = swrTiledPixelMemo(memo, pattern[at], srcline[col], covered, pass->alpha);
    }
}
#endif

// Writes the held clear's colour over the whole main buffer, the tiled passes
// held over it and the fills held over those, a row at a time. Leaves none
// held. With fills, the buffer's row is filled with what they make of the
// clear's colour, and the passes work on a copy of the row without the fills
// (a pass over another reads what is under it), putting each pixel they
// write into the buffer as the fills leave it: per pixel only where a pass
// draws, which for a grid of lines is little of the screen.
//
// When the passes repeat across every row (swrTiledHeldRepeats), as a
// battle's grid does, that per-copy work is most of the cost: on a 320 px
// row a 25 px grid is 13 copies a pass, each a pixel or two, every row. So
// the row is instead worked out for one stretch of `step` pixels, with the
// fills, and the stretch is laid along a row in ordinary memory and copied
// out. Every pixel comes out as the copies would have left it.
void swrTiledHeldWrite(SWRenderer* swr, uintpixel_t color)
{
    swrDirtyAll(swr);
#if PIXEL_SIZE != 16 || !defined SW_HAS_PREMUL_BLEND || defined SW_DITHERED_BLENDING
    swrFillPixels(swr->mainFb, (size_t) swr->mainWidth * swr->mainHeight, color);
#else
    int count = swr->tiledHeldCount, fills = swrFillHeldCount;
    swr->tiledHeldCount = 0;
    swrFillHeldCount = 0;
    SWTiledMemo memos[SWR_TILED_HELD_MAX] = { 0 };
    int period;
    if (count > 0 && swrTiledHeldRepeats(swr, count, &period)) {
        uintpixel_t pattern[SWR_TILED_PERIOD_MAX], row[SWR_HELD_ROW_MAX];
        SWFilledMemo filledMemo = { 0 };
        int width = swr->mainWidth, first = period < width ? period : width;
        bool laid = false; // row[] holds a row laid out from the stretch now in row[0..first)
        for (int y = 0; y < swr->mainHeight; y++) {
            swrFillPixels(pattern, (size_t) period, color);
            for (int p = 0; p < count; p++) swrTiledPassPattern(&swrTiledHeld[p], y, pattern, period, &memos[p]);
            if (fills > 0)
                for (int i = 0; i < period; i++) pattern[i] = swrFilled(&filledMemo, fills, pattern[i]);
            // Most rows of a grid are the row before again.
            if (!laid || memcmp(row, pattern, (size_t) first * sizeof(uintpixel_t)) != 0) {
                int done = first;
                memcpy(row, pattern, (size_t) done * sizeof(uintpixel_t));
                while (done < width) {
                    int more = done < width - done ? done : width - done;
                    memcpy(&row[done], row, (size_t) more * sizeof(uintpixel_t));
                    done += more;
                }
                laid = true;
            }
            memcpy(&swr->mainFb[y * width], row, (size_t) width * sizeof(uintpixel_t));
        }
        return;
    }
    if (fills == 0) {
        for (int y = 0; y < swr->mainHeight; y++) {
            uintpixel_t* dstline = &swr->mainFb[y * swr->mainWidth];
            swrFillPixels(dstline, (size_t) swr->mainWidth, color);
            for (int p = 0; p < count; p++) swrTiledPassRow(&swrTiledHeld[p], y, dstline, &memos[p], NULL, 0, NULL);
        }
        return;
    }
    uintpixel_t row[SWR_HELD_ROW_MAX]; // swrFillHold has checked that a row fits
    SWFilledMemo filledMemo = { 0 };
    uintpixel_t filledColor = swrFilled(&filledMemo, fills, color);
    for (int y = 0; y < swr->mainHeight; y++) {
        uintpixel_t* dstline = &swr->mainFb[y * swr->mainWidth];
        swrFillPixels(dstline, (size_t) swr->mainWidth, filledColor);
        swrFillPixels(row, (size_t) swr->mainWidth, color);
        for (int p = 0; p < count; p++) swrTiledPassRow(&swrTiledHeld[p], y, row, &memos[p], dstline, fills, &filledMemo);
    }
#endif
}

#if PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING
// Scope the cache to Flowey's 45x31 pipe footprint. Bound total residency and
// the number of cold builds a source can trigger in one frame.
enum { SWR_RASTER_W = 45, SWR_RASTER_H = 31, SWR_RASTER_SLOTS = 64,
       SWR_RASTER_BYTES = 192 * 1024, SWR_RASTER_BUILDS = 8 };
typedef struct {
    int sx, sy, dw, dh;
    float angle, px, py;
    uint32_t tint;
} SWRasterKey;
typedef struct { int16_t x, y; uint16_t count, offset; } SWRasterSpan;
typedef struct {
    SWRasterKey key;
    size_t bytes;
    int count;
    SWRasterSpan spans[];
} SWRotatedRaster;
typedef struct SWRotatedRasterCache {
    SWRotatedRaster* entries[SWR_RASTER_SLOTS];
    uint32_t frame;
    int first, count, builds;
} SWRotatedRasterCache;

static size_t swrRotatedRasterBytes;

static void swrDropRotatedRaster(SWRotatedRasterCache* cache)
{
    SWRotatedRaster* entry = cache->entries[cache->first];
    swrRotatedRasterBytes -= entry->bytes;
    free(entry);
    cache->first = (cache->first + 1) % SWR_RASTER_SLOTS;
    cache->count--;
}

static SWRotatedRaster* swrBuildRotatedRaster(SWRotatedRasterCache* cache, SWTexture* texture,
                                              const SWRasterKey* key, int radius)
{
    int side = radius * 2 + 1;
    uintpixel_t* pixels = calloc((size_t) side * side, sizeof(*pixels));
    if (pixels == NULL) return NULL;
    SWRenderer scratch = {0};
    scratch.fb = pixels;
    scratch.fbPitch = (uint16_t) side;
    scratch.portW = scratch.maxX = scratch.portH = scratch.maxY = side;
    scratch.blendMode = bm_normal;
    int x = radius + (key->dw < 0 ? (int) key->px : -(int) key->px);
    int y = radius + (key->dh < 0 ? (int) key->py : -(int) key->py);
    swrDrawSpriteRotatedInternal(&scratch.base, x, y, key->dw, key->dh, texture,
                                 key->sx, key->sy, SWR_RASTER_W, SWR_RASTER_H,
                                 (uintpixel_t) key->tint, 256, key->angle, key->px, key->py);

    int spans = 0, opaque = 0;
    for (int row = 0; row < side; ++row) {
        bool inside = false;
        for (int col = 0; col < side; ++col) {
            bool covered = swrIsOpaque(pixels[row * side + col]);
            spans += covered && !inside;
            opaque += covered;
            inside = covered;
        }
    }
    size_t bytes = sizeof(SWRotatedRaster) + spans * sizeof(SWRasterSpan) + opaque * sizeof(*pixels);
    while (cache->count > 0 && (cache->count == SWR_RASTER_SLOTS ||
                                bytes > SWR_RASTER_BYTES - swrRotatedRasterBytes))
        swrDropRotatedRaster(cache);
    SWRotatedRaster* entry = bytes <= SWR_RASTER_BYTES - swrRotatedRasterBytes ? malloc(bytes) : NULL;
    if (entry != NULL) {
        entry->key = *key;
        entry->bytes = bytes;
        entry->count = spans;
        uintpixel_t* colors = (uintpixel_t*) (entry->spans + spans);
        int index = 0, offset = 0;
        for (int row = 0; row < side; ++row) {
            const uintpixel_t* line = pixels + row * side;
            for (int col = 0; col < side; ) {
                if (!swrIsOpaque(line[col])) { ++col; continue; }
                int first = col++;
                while (col < side && swrIsOpaque(line[col])) ++col;
                int length = col - first;
                entry->spans[index++] = (SWRasterSpan) { first - x, row - y, length, offset };
                memcpy(colors + offset, line + first, (size_t) length * sizeof(*colors));
                offset += length;
            }
        }
        cache->entries[(cache->first + cache->count++) % SWR_RASTER_SLOTS] = entry;
        swrRotatedRasterBytes += bytes;
    }
    free(pixels);
    return entry;
}

static bool swrDrawRotatedRaster(SWRenderer* swr, SWTexture* texture, int x, int y,
                                  const SWRasterKey* key)
{
    // With a half-grid pivot and bounded integer positions, translating the
    // draw preserves the inverse sampler's pixel-center offsets exactly.
    if (key->dw == 0 || key->dw < -64 || key->dw > 64 || key->dh == 0 || key->dh < -64 || key->dh > 64 ||
        x < -4096 || x > 4096 || y < -4096 || y > 4096 || !isfinite(key->angle)) return false;
    int w = swrAbs(key->dw), h = swrAbs(key->dh);
    if (!(key->px >= 0.0f && key->px <= w && key->py >= 0.0f && key->py <= h) ||
        key->px * 2 != (int) (key->px * 2) || key->py * 2 != (int) (key->py * 2)) return false;
    SWRotatedRasterCache* cache = texture->rotatedRasters;
    if (cache == NULL) {
        if (sizeof(*cache) > SWR_RASTER_BYTES - swrRotatedRasterBytes) return false;
        cache = calloc(1, sizeof(*cache));
        if (cache == NULL) return false;
        texture->rotatedRasters = cache;
        swrRotatedRasterBytes += sizeof(*cache);
    }
    const SWRotatedRaster* entry = NULL;
    for (int i = 0; i < cache->count; ++i) {
        const SWRotatedRaster* have = cache->entries[(cache->first + i) % SWR_RASTER_SLOTS];
        if (memcmp(&have->key.angle, &key->angle, sizeof(key->angle)) == 0 &&
            memcmp(&have->key, key, sizeof(*key)) == 0) {
            entry = have;
            break;
        }
    }
    if (entry == NULL) {
        if (cache->frame != swr->frameCounter) {
            cache->frame = swr->frameCounter;
            cache->builds = 0;
        }
        if (cache->builds == SWR_RASTER_BUILDS) return false;
        // The Manhattan extent bounds the rotated corners in both axes.
        int radius = swrCeiling(fmaxf(key->px, w - key->px) + fmaxf(key->py, h - key->py)) + 2;
        if (radius > 64) return false;
        cache->builds++;
        entry = swrBuildRotatedRaster(cache, texture, key, radius);
        if (entry == NULL) return false;
    }
    int right = swr->portX + swr->portW, bottom = swr->portY + swr->portH;
    const uintpixel_t* colors = (const uintpixel_t*) (entry->spans + entry->count);
    for (int i = 0; i < entry->count; ++i) {
        SWRasterSpan span = entry->spans[i];
        int row = y + span.y;
        if (row < swr->portY || row >= bottom) continue;
        int col = x + span.x;
        int first = swrMax(col, swr->portX), last = swrMin(col + span.count, right);
        if (first >= last) continue;
        swrDirtyRow(swr, row, first, last);
        swrCopyPixels(swr->fb + row * swr->fbPitch + first, colors + span.offset + first - col, last - first);
    }
    return true;
}
#endif

void swrFreeRotatedRasters(SWTexture* texture)
{
#if PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING
    SWRotatedRasterCache* cache = texture->rotatedRasters;
    if (cache == NULL) return;
    while (cache->count > 0) swrDropRotatedRaster(cache);
    swrRotatedRasterBytes -= sizeof(*cache);
    free(cache);
    texture->rotatedRasters = NULL;
#else
    (void) texture;
#endif
}

void swrDrawSpriteRotated(
    Renderer* renderer, float dx, float dy, float dw, float dh,
    SWTexture* texture, int sx, int sy, int sw, int sh,
    uint32_t tintColor, float alpha,
    float angleDeg,
    float pivotX,
    float pivotY
)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    
    swrTransformPosIfNeeded(swr, &dx, &dy);
    swrTransformSizeIfNeeded(swr, &pivotX, &pivotY);
    swrTransformSizeIfNeeded(swr, &dw, &dh);
    int x = swrFloor(dx), y = swrFloor(dy), w = swrCeiling(dw), h = swrCeiling(dh);
    uintpixel_t packed = swrConvertPixel(tintColor);
    int opacity = swrIntAlpha(alpha);

#if PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING
    if (sw == SWR_RASTER_W && sh == SWR_RASTER_H && texture->immutable &&
        sx >= 0 && sy >= 0 && sx <= texture->width - sw && sy <= texture->height - sh &&
        swr->blendMode == bm_normal && opacity > 253) {
        SWRasterKey key = { sx, sy, w, h, angleDeg, pivotX, pivotY, packed };
        if (swrDrawRotatedRaster(swr, texture, x, y, &key)) return;
    }
#endif
    
    swrDrawSpriteRotatedInternal(
        renderer, x, y, w, h, texture, sx, sy, sw, sh, packed, opacity, angleDeg, pivotX, pivotY
    );
}

void swrDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1, uint32_t color2, uint32_t color3, float alpha)
{
    SWRenderer* swr = (SWRenderer*) renderer;
    swrTransformPosIfNeeded(swr, &x1, &y1);
    swrTransformPosIfNeeded(swr, &x2, &y2);
    swrTransformPosIfNeeded(swr, &x3, &y3);
    swrDrawTriangleTransformed(
        renderer,
        x1, y1,
        x2, y2,
        x3, y3,
        swrConvertPixel(color1),
        swrConvertPixel(color2),
        swrConvertPixel(color3),
        swrIntAlpha(alpha)
    );
}
