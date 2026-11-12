/*
 * Text as a QR code on screen: see of_qr.h for the format.
 */

#include "of_qr.h"

#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most an alphanumeric code holds (version 40, low error correction). */
#define OF_QR_MAX_CHARS 4296
#define OF_QR_PREFIX "PQ1:"
/* A log's end is cut to this before anything else; far more than one code holds. */
#define OF_QR_MAX_TEXT (24u * 1024u)

static char g_payload[OF_QR_MAX_CHARS + 1];
static uint8_t g_temp[qrcodegen_BUFFER_LEN_MAX];

static const char g_base45[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

/* "PQ1:" + base45 of bytes into g_payload; false if it would not fit one code. */
static bool makePayload(const uint8_t *bytes, size_t len) {
    size_t prefix = sizeof(OF_QR_PREFIX) - 1;
    size_t need = prefix + len / 2 * 3 + (len & 1) * 2;
    if (need > OF_QR_MAX_CHARS) return false;
    memcpy(g_payload, OF_QR_PREFIX, prefix);
    char *out = g_payload + prefix;
    size_t i = 0;
    for (; i + 1 < len; i += 2) {
        unsigned n = (unsigned) bytes[i] * 256u + bytes[i + 1];
        *out++ = g_base45[n % 45];
        *out++ = g_base45[n / 45 % 45];
        *out++ = g_base45[n / 2025];
    }
    if (i < len) {
        *out++ = g_base45[bytes[i] % 45];
        *out++ = g_base45[bytes[i] / 45];
    }
    *out = '\0';
    return true;
}

/* Compresses header + text (+ note) and encodes it into qr->code within
 * maxVersion. False if it does not fit or memory ran out (*noMemory). */
static bool encode(OfQr *qr, const char *header, const char *text, size_t len, const char *note, int maxVersion,
                   bool *noMemory) {
    size_t headerLen = header != NULL ? strlen(header) : 0;
    size_t noteLen = note != NULL ? strlen(note) : 0;
    size_t whole = headerLen + len + noteLen;
    char *joined = malloc(whole ? whole : 1);
    if (joined == NULL) {
        *noMemory = true;
        return false;
    }
    if (headerLen) memcpy(joined, header, headerLen);
    if (len) memcpy(joined + headerLen, text, len);
    if (noteLen) memcpy(joined + headerLen + len, note, noteLen);

    size_t packedLen = 0;
    int flags = (int) tdefl_create_comp_flags_from_zip_params(MZ_BEST_COMPRESSION, MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
    void *packed = tdefl_compress_mem_to_heap(joined, whole, &packedLen, flags);
    free(joined);
    if (packed == NULL) {
        *noMemory = true;
        return false;
    }
    bool fits = makePayload(packed, packedLen);
    free(packed);
    if (!fits) return false;
    if (!qrcodegen_encodeText(g_payload, g_temp, qr->code, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, maxVersion,
                              qrcodegen_Mask_AUTO, true))
        return false;
    qr->size = qrcodegen_getSize(qr->code);
    return true;
}

/* The highest version that fits the screen at this scale and border. */
static int fittingVersion(int width, int height, int scale, int border) {
    int side = (width < height ? width : height) / scale - 2 * border;
    int version = (side - 17) / 4;
    if (side < 21) return 0;
    return version > qrcodegen_VERSION_MAX ? qrcodegen_VERSION_MAX : version;
}

bool ofQrMake(OfQr *qr, const char *header, const char *text, size_t len, bool keepEnd, int width, int height) {
    memset(qr, 0, sizeof(*qr));
    g_payload[0] = '\0';
    qr->total = len;
    bool noMemory = false;

    /* Bigger modules first: they survive a blurred or slanted photo better.
     * Always the standard four-module quiet zone: past the frame's edge is
     * the Pocket's dark bezel. */
    static const int layouts[][2] = { { 3, 4 }, { 2, 4 } };
    int count = (int) (sizeof(layouts) / sizeof(layouts[0]));
    if (len <= OF_QR_MAX_TEXT) {
        for (int i = 0; i < count; i++) {
            int version = fittingVersion(width, height, layouts[i][0], layouts[i][1]);
            if (version == 0) continue;
            if (encode(qr, header, text, len, NULL, version, &noMemory)) {
                qr->scale = layouts[i][0];
                qr->border = layouts[i][1];
                qr->used = len;
                return true;
            }
            if (noMemory) return false;
        }
    }

    /* Too much text: keep what fits the most roomy layout. */
    int scale = layouts[count - 1][0], border = layouts[count - 1][1];
    int version = fittingVersion(width, height, scale, border);
    if (version == 0) return false;
    size_t low = 0, high = len < OF_QR_MAX_TEXT ? len : OF_QR_MAX_TEXT;
    char note[96];
    size_t best = 0;
    bool any = false;
    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        const char *from = keepEnd ? text + len - mid : text;
        snprintf(note, sizeof(note), "\n(cut for the QR code: it holds the %s %u of %u bytes)\n",
                 keepEnd ? "last" : "first", (unsigned) mid, (unsigned) len);
        if (encode(qr, header, from, mid, note, version, &noMemory)) {
            best = mid;
            any = true;
            low = mid + 1;
        } else {
            if (noMemory || mid == 0) break;
            high = mid - 1;
        }
    }
    if (!any) {
        qr->size = 0;
        return false;
    }
    /* Whole lines only: cut back to a line break if there is one. */
    size_t kept = best;
    if (keepEnd) {
        /* Start just after the first line break, unless that loses half. */
        const char *start = text + len - best;
        const char *brk = memchr(start, '\n', best);
        bool atLineStart = start == text || start[-1] == '\n';
        if (!atLineStart && brk != NULL && (size_t) (brk + 1 - start) < best / 2) kept = (size_t) (text + len - (brk + 1));
    } else {
        while (kept > best / 2 && text[kept - 1] != '\n') kept--;
        if (kept <= best / 2) kept = best;
    }
    const char *from = keepEnd ? text + len - kept : text;
    snprintf(note, sizeof(note), "\n(cut for the QR code: it holds the %s %u of %u bytes)\n",
             keepEnd ? "last" : "first", (unsigned) kept, (unsigned) len);
    if (!encode(qr, header, from, kept, note, version, &noMemory)) {
        /* Cannot happen (less text than a fit), but stay with the fit if it does. */
        from = keepEnd ? text + len - best : text;
        snprintf(note, sizeof(note), "\n(cut for the QR code: it holds the %s %u of %u bytes)\n",
                 keepEnd ? "last" : "first", (unsigned) best, (unsigned) len);
        if (!encode(qr, header, from, best, note, version, &noMemory)) {
            qr->size = 0;
            return false;
        }
        kept = best;
    }
    qr->scale = scale;
    qr->border = border;
    qr->used = kept;
    return true;
}

void ofQrDraw(const OfQr *qr, uint16_t *fb, int width, int height, int stride, uint16_t dark, uint16_t light) {
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) fb[y * stride + x] = light;
    if (qr->size == 0) return;
    int side = qr->size * qr->scale;
    int left = (width - side) / 2, top = (height - side) / 2;
    for (int my = 0; my < qr->size; my++)
        for (int mx = 0; mx < qr->size; mx++) {
            if (!qrcodegen_getModule(qr->code, mx, my)) continue;
            uint16_t *at = fb + (top + my * qr->scale) * stride + left + mx * qr->scale;
            for (int sy = 0; sy < qr->scale; sy++)
                for (int sx = 0; sx < qr->scale; sx++) at[sy * stride + sx] = dark;
        }
}

const char *ofQrPayload(void) {
    return g_payload;
}
