/*
 * Text as a QR code on screen, so a benchmark report or a fatal error's log
 * can be read off the Pocket with a phone photo instead of the SD card.
 *
 * The code holds "PQ1:" followed by base45 (RFC 9285) of a zlib stream of the
 * text, all in the QR alphanumeric set. To read it back:
 *   zlib.decompress(base45.b45decode(text[4:]))
 * (src/openfpga/tools/pq1-decode.py does that, from the text or from a photo).
 *
 * This file and of_qr.c only need qrcodegen and miniz's deflate, so they can
 * move to the openfpgaOS SDK as they are (openfpgaCore#11).
 */

#ifndef OF_QR_H
#define OF_QR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "qrcodegen.h"

typedef struct {
    uint8_t code[qrcodegen_BUFFER_LEN_MAX]; /* the symbol, in qrcodegen's format */
    int size;                               /* modules per side; 0 if nothing was made */
    int scale;                              /* screen pixels per module */
    int border;                             /* light modules around the code */
    size_t used;                            /* bytes of the text the code holds */
    size_t total;                           /* bytes of the text offered */
} OfQr;

/* Makes the largest code that fits a width x height screen at 2 or more
 * pixels per module (3 when the whole text fits that way). If the whole text
 * does not fit, the code holds as much as does from its start, or with
 * keepEnd from its end, and says so in a last line. header, if not NULL, is
 * put in front of the text and always kept. Returns false if nothing fits or
 * memory ran out. Takes up to a few hundred ms on the Pocket: call it once. */
bool ofQrMake(OfQr *qr, const char *header, const char *text, size_t len, bool keepEnd, int width, int height);

/* Draws the code centred on a 16-bit frame (light background over the whole
 * frame, dark modules). stride is in pixels. */
void ofQrDraw(const OfQr *qr, uint16_t *fb, int width, int height, int stride, uint16_t dark, uint16_t light);

/* The PQ1 text the last ofQrMake put in its code, for tests. */
const char *ofQrPayload(void);

#endif
