/*
 * Whole-framebuffer fills on the openfpgaOS GPU.
 *
 * The CPU clears the back buffer through its write-back data cache, which
 * reads every line from SDRAM before overwriting it and writes it back
 * later: at 640x480 that is 12.9 ms a frame on the Pocket. The GPU's
 * rectangle clear writes SDRAM directly. It repeats one byte, so it can only
 * fill colours whose two RGB555 bytes are equal (black, the usual clear).
 *
 * This is the only file that includes of_gpu.h (it keeps driver state in
 * static variables).
 */
#include "of_hooks.h"
#include "log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef OF_PC
#include "of_caps.h"
#include "of_cache.h"
#include "of_gpu.h"

static int g_gpuState; /* 0 not tried, 1 ready, -1 unavailable or turned off */

static bool gpuReady(void) {
    if (g_gpuState == 0) {
        const struct of_capabilities *caps = of_get_caps();
        if (getenv("BS_CPU_CLEAR") != NULL || caps == NULL || caps->gpu_base == 0 || caps->sdram_base == 0) {
            g_gpuState = -1;
        } else {
            of_gpu_init();
            g_gpuState = 1;
        }
        logInfo("Clears: %s\n", g_gpuState > 0 ? "GPU" : "CPU");
    }
    return g_gpuState > 0;
}

bool platformFillFramebuffer(uint16_t *fb, int width, int height, uint16_t color) {
    /* The clear repeats one byte. Black is 0x8000 here (bit 15 marks the
     * pixel opaque); the display ignores that bit and nothing reads it back
     * from the screen, so black is cleared as 0x0000. */
    if ((color & 0x7FFF) == 0) color = 0;
    if ((color >> 8) != (color & 0xFF)) return false;
    uint32_t rowBytes = (uint32_t) width * sizeof(uint16_t);
    if (width <= 0 || height <= 0 || rowBytes > 0xFFFFu || height > 0xFFFF || ((uintptr_t) fb & 3) != 0) return false;
    if (!gpuReady()) return false;
    uint32_t bytes = rowBytes * (uint32_t) height;
    /* No line of the buffer may be written back over the GPU's clear, and
     * none may stay cached with the old picture: write back and drop them all
     * first. Nothing touches the buffer again until the fence is reached. */
    of_cache_flush_range(fb, bytes);
    of_gpu_clear_rect_strided((uint32_t) (uintptr_t) fb, (uint16_t) rowBytes, (uint16_t) height, (uint16_t) rowBytes,
                              (uint8_t) color);
    of_gpu_wait(of_gpu_submit());
    return true;
}
#else
/* BS_PC_ZERO_CLEAR=1 does what the Pocket's GPU clear does to black: it
 * writes 0x0000, without the opaque bit, so a desktop run can check that
 * nothing reads that bit back from the screen. */
bool platformFillFramebuffer(uint16_t *fb, int width, int height, uint16_t color) {
    static int zero = -1;
    if (zero < 0) {
        zero = getenv("BS_PC_ZERO_CLEAR") != NULL;
        if (zero) logInfo("Clears: black as 0x0000 (BS_PC_ZERO_CLEAR)\n");
    }
    if (!zero || (color & 0x7FFF) != 0) return false;
    static unsigned fills;
    if (++fills == 1000) logInfo("Clears: 1000 black clears as 0x0000, last %dx%d\n", width, height);
    for (size_t i = 0, n = (size_t) width * (size_t) height; i < n; i++) fb[i] = 0;
    return true;
}
#endif
