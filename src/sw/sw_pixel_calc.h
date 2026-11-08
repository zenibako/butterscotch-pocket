#ifndef _SW_PIXEL_CALC_H
#define _SW_PIXEL_CALC_H

#include "sw_defines.h"
#include "sw_pixel_convert.h"

// Random number generator to be used for 8-bpp blending operations.
FORCE_INLINE int swrFastRng()
{
    static uint32_t rngseed = 1337;
#ifdef SW_USE_XORSHIFT32_FOR_DITHERING
    uint32_t x = rngseed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rngseed = x;
#else
    rngseed += 1339;
    if (rngseed > 601000)
        rngseed = 0;
    return rngseed;
#endif
}

// Check if a pixel is opaque.
//
// Later, this should be changed to perform full alpha-blending
// (at least in 32-bit pixel mode)
FORCE_INLINE bool swrIsOpaque(uintpixel_t color)
{
#if PIXEL_SIZE == 8
    return (color != PXL_TRANSPARENT);
#else
#ifndef SW_NO_SEMI_TRANSPARENT_TEXTURE_SUPPORT
    return (color & TRANSPARENT_MASK) == TRANSPARENT_MASK;
#else
    return (color & TRANSPARENT_MASK) != 0;
#endif
#endif
}

#ifndef SW_NO_SEMI_TRANSPARENT_TEXTURE_SUPPORT

// NOTE: These only work properly for 32-bit color.
// (But you shouldn't have meddled with the #error I put in in sw_config.h...)
FORCE_INLINE bool swrIsFullyTransparent(uintpixel_t color)
{
    return (color & TRANSPARENT_MASK) == 0;
}

FORCE_INLINE uint8_t swrGetAlphaU8(uintpixel_t color)
{
    return color >> 24;
}

// Turns the specified color (including alpha channel) into 
FORCE_INLINE uintpixel_t swrAlphaToColor(uint8_t alpha)
{
    return alpha | (alpha << 8) | (alpha << 16);
}

#endif

// Multiplies a color value (`color`) by another color value (`tintColor`).
FORCE_INLINE uintpixel_t tint(uintpixel_t tintColor, uintpixel_t color)
{
#if PIXEL_SIZE == 8
    if (tintColor == 0xFF || tintColor == PXL_TRANSPARENT)
        return color;
#elif PIXEL_SIZE == 16
    if ((tintColor & 0x7FFF) == 0x7FFF)
        return color;
#else
    if ((tintColor & 0xFFFFFF) == 0xFFFFFF)
        return color;
#endif
    
#if PIXEL_SIZE == 8 || defined SW_INACCURATE_TINTING
    // fast but probably doesn't really work all that well
    return color & tintColor;
#elif PIXEL_SIZE == 32
    Pixel32ARGB x, y;
    
    x.l = color;
    y.l = tintColor;
    
    x.p.b = (int)x.p.b * y.p.b / 255;
    x.p.g = (int)x.p.g * y.p.g / 255;
    x.p.r = (int)x.p.r * y.p.r / 255;
    return x.l;
#elif PIXEL_SIZE == 16
    int tcb = tintColor & 0x1F;
    int tcg = (tintColor >> 5) & 0x1F;
    int tcr = (tintColor >> 10) & 0x1F;
    
    int cb = color & 0x1F;
    int cg = (color >> 5) & 0x1F;
    int cr = (color >> 10) & 0x1F;
    int ca = color & 0x8000;
    
    cb = (cb * tcb) / 32;
    cg = (cg * tcg) / 32;
    cr = (cr * tcr) / 32;
    return ca | cb | (cg << 5) | (cr << 10);
#endif
}

// Performs alpha blending on a pixel, with another pixel.
//
// NOTE: alpha is between 0 and 256, NOT between 0 and 255!
//
// TODO: This routine could use some optimization.  Obviously I tried my best, but clearly
// it's still true that too many calculations are being performed.
//
// NOTE: Obviously I could use SIMD here, but old computers didn't have SIMD, and the code
// runs fast enough on modern computers to not need to do SIMD.
//
// Another note: Wow, it' become a huge mess of ifdef's...
FORCE_INLINE
void alphaBlend(uintpixel_t* dcolor, uintpixel_t scolor, int blendmode, int srcalpha)
{
    /* If we didn't disable subtract support and are in dithered blending mode */
#if defined SW_DITHERED_BLENDING
#ifdef SW_NO_SUBTRACT_SUPPORT

#ifndef SW_BAD_BLEND_MODE_SUPPORT
    if (UNLIKELY(blendmode == bm_subtract))
        return;
#endif // SW_NO_BLEND_MODE_SUPPORT

#else // SW_NO_SUBTRACT_SUPPORT
    if (UNLIKELY(blendmode == bm_subtract))
    {
    #if PIXEL_SIZE == 8
        srcalpha = scolor & 0x7;
        srcalpha = (srcalpha << 5) | (srcalpha << 2) | (srcalpha >> 1);
    #elif PIXEL_SIZE == 16
        srcalpha = scolor & 0x1F;
        srcalpha = (srcalpha << 3) | (srcalpha >> 2);
    #elif PIXEL_SIZE == 32
        srcalpha = scolor & 0xFF;
    #endif
    
        if (UNLIKELY(srcalpha < 5))
            return;
        if (UNLIKELY(srcalpha > 250))
            *dcolor = 0;
        
        if ((swrFastRng() & 0xFF) < srcalpha)
            *dcolor = 0;
        
        return;
    }
#endif // !SW_NO_SUBTRACT_SUPPORT
#endif // defined SW_DITHERED_BLENDING

    int dstalpha = 256;

// TODO: figure out why we aren't doing this for 8-bit?
#if PIXEL_SIZE == 32 || PIXEL_SIZE == 16

    /* Check extremely common cases in 32- and 16-bit modes */
#ifndef SW_BAD_BLEND_MODE_SUPPORT
    if (LIKELY(blendmode == bm_normal))
#endif
    {
        // it's so significant here we might as well fill in the whole color
        if (LIKELY(srcalpha > 253)) {
            *dcolor = scolor;
            return;
        }
        
        // it's so insignificant here nobody will notice if we just don't...
        if (UNLIKELY(srcalpha < 4))
            return;
        
        dstalpha -= srcalpha;
    }

#endif // PIXEL_SIZE == 32 || PIXEL_SIZE == 16

    /* With dithered blending, randomly place the color, or don't */
#ifdef SW_DITHERED_BLENDING
    if (srcalpha < 240)
    {
        if ((swrFastRng() & 0xFF) >= srcalpha)
            return;
    }
    
#ifndef SW_BAD_BLEND_MODE_SUPPORT
    if (UNLIKELY(blendmode == bm_add))
    {
        *dcolor |= scolor;
        return;
    }
#endif

    *dcolor = scolor;

#else // SW_DITHERED_BLENDING

    /* Extract pixel channels */
#if PIXEL_SIZE == 32
    Pixel32ARGB dc, sc;
    dc.l = *dcolor;
    sc.l = scolor;
    
    int scr = sc.p.r;
    int scg = sc.p.g;
    int scb = sc.p.b;
    int sca = sc.p.a;
    int dcr = dc.p.r;
    int dcg = dc.p.g;
    int dcb = dc.p.b;
    int dca = 0xFF;
#elif PIXEL_SIZE == 16
    int scb = scolor & 0x1F;
    int scg = (scolor >> 5) & 0x1F;
    int scr = (scolor >> 10) & 0x1F;
    int sca = (scolor & 0x8000) ? 255 : 0;

    uintpixel_t _dcolor = *dcolor;
    int dcb = _dcolor & 0x1F;
    int dcg = (_dcolor >> 5) & 0x1F;
    int dcr = (_dcolor >> 10) & 0x1F;
    int dca = 0xFF;
#endif // PIXEL_SIZE
    
    if (UNLIKELY(blendmode == bm_subtract))
    {
        dcr = (dcr * (255 - scr)) >> 8;
        dcg = (dcg * (255 - scg)) >> 8;
        dcb = (dcb * (255 - scb)) >> 8;
        dca = (dca * (255 - sca)) >> 8;
    }
    else
    {
        /* Perform the actual blending ops on them */
        dcr = (dcr * dstalpha + scr * srcalpha) >> 8;
        dcg = (dcg * dstalpha + scg * srcalpha) >> 8;
        dcb = (dcb * dstalpha + scb * srcalpha) >> 8;
    }
    
#ifndef SW_BAD_BLEND_MODE_SUPPORT
    /* Clamp them if needed */
    if (UNLIKELY(blendmode != bm_normal))
    {
        //clamp to 0
        dcr &= ((-dcr) >> 31);
        dcg &= ((-dcg) >> 31);
        dcb &= ((-dcb) >> 31);
    #if PIXEL_SIZE == 16
        if (dcr > 31) dcr = 31;
        if (dcg > 31) dcg = 31;
        if (dcb > 31) dcb = 31;
    #else
        //clamp to 255
        dcr |= ((signed char)(dcr >> 1) >> 7);
        dcg |= ((signed char)(dcg >> 1) >> 7);
        dcb |= ((signed char)(dcb >> 1) >> 7);
    #endif
    }
#endif // SW_BAD_BLEND_MODE_SUPPORT

    /* Then re-assemble the pixel. */
#if PIXEL_SIZE == 32
    dc.p.r = dcr;
    dc.p.g = dcg;
    dc.p.b = dcb;
    dc.p.a = dca;
    
    *dcolor = dc.l;
#elif PIXEL_SIZE == 16
    *dcolor = (dca ? 0x8000 : 0) | dcb | (dcg << 5) | (dcr << 10);
#endif

#endif // SW_DITHERED_BLENDING
}

#if PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING

// Fast path for bm_normal blending at a constant partial alpha in 15-bit mode.
//
// alphaBlend() multiplies each of the three channels of both pixels per call.
// When the source colour is constant over a run (a rectangle fill, or a small
// sprite stretched over a large area) its half can be premultiplied once, and
// the destination's red and blue can share one multiply by moving red up to
// bit 16 so the two 13-bit products cannot touch. The result is bit-identical
// to alphaBlend() for bm_normal.
#define SW_HAS_PREMUL_BLEND

// alphaBlend() treats alpha outside this range as "skip" or "opaque copy".
FORCE_INLINE bool swrIsPartialAlpha(int alpha)
{
    return alpha >= 4 && alpha <= 253;
}

FORCE_INLINE uint32_t swrSpreadRedBlue(uint32_t color)
{
    return ((color & 0x7C00) << 6) | (color & 0x1F);
}

FORCE_INLINE uint32_t swrGreen(uint32_t color)
{
    return (color >> 5) & 0x1F;
}

// srcRedBlue / srcGreen are swrSpreadRedBlue(src) * srcalpha and
// swrGreen(src) * srcalpha; dstalpha is 256 - srcalpha.
FORCE_INLINE uintpixel_t swrBlendPremultiplied(uintpixel_t dcolor, uint32_t srcRedBlue, uint32_t srcGreen, uint32_t dstalpha)
{
    uint32_t redBlue = (swrSpreadRedBlue(dcolor) * dstalpha + srcRedBlue) >> 8;
    uint32_t green = (swrGreen(dcolor) * dstalpha + srcGreen) >> 8;
    return (uintpixel_t)(0x8000 | ((redBlue >> 6) & 0x7C00) | ((green & 0x1F) << 5) | (redBlue & 0x1F));
}

// Floor the average of each RGB555 channel independently. Mask the low
// bit of each channel and the opacity bit before the packed right shift.
FORCE_INLINE uintpixel_t swrBlendHalf(uintpixel_t dst, uintpixel_t src)
{
    return (uintpixel_t) (((dst & src) + (((dst ^ src) & 0x7bdeu) >> 1)) | 0x8000u);
}

// A constant translucent fill reads and writes pairs on a 32-bit bus. The
// scalar ends keep unaligned rows and odd surface pitches within their bounds.
// memcpy permits word access without aliasing uintpixel_t storage as uint32_t.
FORCE_INLINE void swrBlendSolidRow(uintpixel_t* line, size_t count, uint32_t srcRedBlue, uint32_t srcGreen, uint32_t dstalpha)
{
    if (!count) return;
    if (count && ((uintptr_t) line & 3)) {
        uintpixel_t result = swrBlendPremultiplied(*line, srcRedBlue, srcGreen, dstalpha);
        if (result != *line) *line = result;
        line++;
        count--;
    }
#if defined(__has_builtin)
#if __has_builtin(__builtin_assume_aligned)
    line = (uintpixel_t*) __builtin_assume_aligned(line, 4);
#endif
#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7))
    line = (uintpixel_t*) __builtin_assume_aligned(line, 4);
#endif
    if (dstalpha == 128 && (srcRedBlue | srcGreen) == 0) {
        // Half black halves each channel. Mask out bits shifted across
        // channel boundaries and restore the compositor's opaque result.
        while (count >= 2) {
            uint32_t under;
            memcpy(&under, line, sizeof(under));
            uint32_t darkened = ((under >> 1) & 0x3def3defu) | 0x80008000u;
            if (darkened != under) memcpy(line, &darkened, sizeof(darkened));
            line += 2;
            count -= 2;
        }
    }
    if (count >= 2) {
        // Each channel occupies a separate 16-bit lane; its product plus
        // premultiplied source is at most 31 * 256, so lanes cannot carry.
        uint32_t red = (srcRedBlue >> 16) * 0x10001u;
        uint32_t green = srcGreen * 0x10001u;
        uint32_t blue = (srcRedBlue & 0xffffu) * 0x10001u;
        uint32_t first;
        memcpy(&first, line, sizeof(first));
        uint32_t lastUnder = ~first, lastResult = 0;
        while (count >= 2) {
            uint32_t under;
            memcpy(&under, line, sizeof(under));
            if (under != lastUnder) {
                uint32_t outRed = (((((under >> 10) & 0x001f001fu) * dstalpha + red) >> 8) & 0x001f001fu);
                uint32_t outGreen = (((((under >> 5) & 0x001f001fu) * dstalpha + green) >> 8) & 0x001f001fu);
                uint32_t outBlue = ((((under & 0x001f001fu) * dstalpha + blue) >> 8) & 0x001f001fu);
                lastUnder = under;
                lastResult = 0x80008000u | (outRed << 10) | (outGreen << 5) | outBlue;
            }
            if (lastResult != under) memcpy(line, &lastResult, sizeof(lastResult));
            line += 2;
            count -= 2;
        }
    }
    if (count) {
        uintpixel_t result = swrBlendPremultiplied(*line, srcRedBlue, srcGreen, dstalpha);
        if (result != *line) *line = result;
    }
}

#endif // PIXEL_SIZE == 16 && !defined SW_DITHERED_BLENDING

// Calculates an internal "alpha" value from GML-provided "alpha" values.
FORCE_INLINE int swrIntAlpha(float alphaf)
{
    return (int)(alphaf * 256);
}

// Blends a pixel between two colors.
// frac means 0-65535 where 65535 means one.  And frac1 + frac2 MUST be equal to 65535.
FORCE_INLINE uintpixel_t swrTwoWayBlend(uintpixel_t color1, uintpixel_t color2, uint16_t frac1, uint16_t frac2)
{
#if defined SW_DITHERED_BLENDING
    int rng = swrFastRng() & 0xFFFF;
    if (rng < frac1) return color1;
    (void) frac2;
    return color2;
#elif PIXEL_SIZE == 32
    Pixel32ARGB x1, x2, out;
    x1.l = color1;
    x2.l = color2;
    out.p.r = (x1.p.r * frac1 + x2.p.r * frac2) >> 16;
    out.p.g = (x1.p.g * frac1 + x2.p.g * frac2) >> 16;
    out.p.b = (x1.p.b * frac1 + x2.p.b * frac2) >> 16;
    out.p.a = (x1.p.a * frac1 + x2.p.a * frac2) >> 16;
    return out.l;
#elif PIXEL_SIZE == 16
    int c1b = color1 & 0x1F, c1g = (color1 >> 5) & 0x1F, c1r = (color1 >> 10) & 0x1F;
    int c2b = color2 & 0x1F, c2g = (color2 >> 5) & 0x1F, c2r = (color2 >> 10) & 0x1F;
    int ca = color1 & 0x8000;
    int cr = (c1r * frac1 + c2r * frac2) >> 16;
    int cg = (c1g * frac1 + c2g * frac2) >> 16;
    int cb = (c1b * frac1 + c2b * frac2) >> 16;
    return ca | cb | (cg << 5) | (cr << 10);
#endif
}

// Blends a pixel between three colors.
// frac means 0-65535 where 65535 means one.  And frac1 + frac2 + frac3 MUST be equal to 65535.
FORCE_INLINE uintpixel_t swrThreeWayBlend(uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, uint16_t frac1, uint16_t frac2, uint16_t frac3)
{
#if defined SW_DITHERED_BLENDING
    int rng = swrFastRng() & 0xFFFF;
    if (rng < frac1) return color1; else rng -= frac1;
    if (rng < frac2) return color2;
    (void) frac3;
    return color3;
#elif PIXEL_SIZE == 32
    Pixel32ARGB x1, x2, x3, out;
    x1.l = color1;
    x2.l = color2;
    x3.l = color3;
    out.p.r = (x1.p.r * frac1 + x2.p.r * frac2 + x3.p.r * frac3) >> 16;
    out.p.g = (x1.p.g * frac1 + x2.p.g * frac2 + x3.p.g * frac3) >> 16;
    out.p.b = (x1.p.b * frac1 + x2.p.b * frac2 + x3.p.b * frac3) >> 16;
    out.p.a = (x1.p.a * frac1 + x2.p.a * frac2 + x3.p.a * frac3) >> 16;
    return out.l;
#elif PIXEL_SIZE == 16
    int c1b = color1 & 0x1F, c1g = (color1 >> 5) & 0x1F, c1r = (color1 >> 10) & 0x1F;
    int c2b = color2 & 0x1F, c2g = (color2 >> 5) & 0x1F, c2r = (color2 >> 10) & 0x1F;
    int c3b = color3 & 0x1F, c3g = (color3 >> 5) & 0x1F, c3r = (color3 >> 10) & 0x1F;
    int ca = color1 & 0x8000;
    int cr = (c1r * frac1 + c2r * frac2 + c3r * frac3) >> 16;
    int cg = (c1g * frac1 + c2g * frac2 + c3g * frac3) >> 16;
    int cb = (c1b * frac1 + c2b * frac2 + c3b * frac3) >> 16;
    return ca | cb | (cg << 5) | (cr << 10);
#endif
}

// Blends a pixel between four colors.
// frac means 0-65535 where 65535 means one.  And frac1 + frac2 + frac3 + frac4 MUST be equal to 65535.
FORCE_INLINE uintpixel_t swrFourWayBlend(uintpixel_t color1, uintpixel_t color2, uintpixel_t color3, uintpixel_t color4, uint16_t frac1, uint16_t frac2, uint16_t frac3, uint16_t frac4)
{
#if defined SW_DITHERED_BLENDING
    int rng = swrFastRng() & 0xFFFF;
    if (rng < frac1) return color1; else rng -= frac1;
    if (rng < frac2) return color2;
    if (rng < frac3) return color3;
    (void) frac4;
    return color4;
#elif PIXEL_SIZE == 32
    Pixel32ARGB x1, x2, x3, x4, out;
    x1.l = color1;
    x2.l = color2;
    x3.l = color3;
    x4.l = color4;
    out.p.r = (x1.p.r * frac1 + x2.p.r * frac2 + x3.p.r * frac3 + x4.p.r * frac4) >> 16;
    out.p.g = (x1.p.g * frac1 + x2.p.g * frac2 + x3.p.g * frac3 + x4.p.g * frac4) >> 16;
    out.p.b = (x1.p.b * frac1 + x2.p.b * frac2 + x3.p.b * frac3 + x4.p.b * frac4) >> 16;
    out.p.a = (x1.p.a * frac1 + x2.p.a * frac2 + x3.p.a * frac3 + x4.p.a * frac4) >> 16;
    return out.l;
#elif PIXEL_SIZE == 16
    int c1b = color1 & 0x1F, c1g = (color1 >> 5) & 0x1F, c1r = (color1 >> 10) & 0x1F;
    int c2b = color2 & 0x1F, c2g = (color2 >> 5) & 0x1F, c2r = (color2 >> 10) & 0x1F;
    int c3b = color3 & 0x1F, c3g = (color3 >> 5) & 0x1F, c3r = (color3 >> 10) & 0x1F;
    int c4b = color4 & 0x1F, c4g = (color4 >> 5) & 0x1F, c4r = (color4 >> 10) & 0x1F;
    int ca = color1 & 0x8000;
    int cr = (c1r * frac1 + c2r * frac2 + c3r * frac3 + c4r * frac4) >> 16;
    int cg = (c1g * frac1 + c2g * frac2 + c3g * frac3 + c4g * frac4) >> 16;
    int cb = (c1b * frac1 + c2b * frac2 + c3b * frac3 + c4b * frac4) >> 16;
    return ca | cb | (cg << 5) | (cr << 10);
#endif
}

#endif//_SW_PIXEL_CALC_H
