/*
 * The game cores' loading screen; see of_loading.h.
 */

#include "of_loading.h"

#include "of_font.h"

#include <stdio.h>
#include <string.h>

#define OF_BLACK  0x0000
#define OF_WHITE  0x7FFF
#define OF_LIGHT  0x5AD6 /* hint text */
#define OF_GREY   0x3DEF /* the logged error */
#define OF_TRACK  0x1084 /* the bar's empty part */
#define OF_RED    0x7C00

/* The bar: a thin line along the very bottom edge, the whole width across,
 * where other Pocket cores put theirs. Text keeps a margin. */
#define OF_TEXT_MARGIN 24
#define OF_BAR_HEIGHT 2

static int textLeft(int width) { return width * OF_TEXT_MARGIN / OF_LOADING_WIDTH; }
static int barLeft(int width) { (void) width; return 0; }
static int barWidth(int width) { return width; }
static int barHeight(int height) { return height * OF_BAR_HEIGHT / OF_LOADING_HEIGHT; }
static int barTop(int height) { return height - barHeight(height); }

void ofLoadingDrawBar(uint16_t *fb, int width, int height, int permille, uint16_t color) {
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    int left = barLeft(width), span = barWidth(width), top = barTop(height), rows = barHeight(height);
    int filled = span * permille / 1000;
    for (int y = top; y < top + rows; y++) {
        uint16_t *row = fb + y * width + left;
        for (int x = 0; x < span; x++) row[x] = x < filled ? color : OF_TRACK;
    }
}

/* Before data.win has loaded there is no game font: the text is then drawn
 * in the OS terminal's 8x8 font (printable ASCII of openfpgaOS's CP437 font,
 * src/firmware/os/targets/pocket/terminal.c, Apache-2.0), which is what the
 * Pocket shows at that point anyway. */
#define OF_TERM_FIRST ' '
#define OF_TERM_LAST  '~'
#define OF_TERM_LINE  10
static const uint8_t g_termFont[(OF_TERM_LAST - OF_TERM_FIRST + 1) * 8] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /*   */
    0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00, /* ! */
    0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* " */
    0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00, /* # */
    0x30, 0x7C, 0xC0, 0x78, 0x0C, 0xF8, 0x30, 0x00, /* $ */
    0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00, /* % */
    0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00, /* & */
    0x60, 0x60, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, /* ' */
    0x18, 0x30, 0x60, 0x60, 0x60, 0x30, 0x18, 0x00, /* ( */
    0x60, 0x30, 0x18, 0x18, 0x18, 0x30, 0x60, 0x00, /* ) */
    0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00, /* asterisk */
    0x00, 0x30, 0x30, 0xFC, 0x30, 0x30, 0x00, 0x00, /* + */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x60, /* , */
    0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x00, /* - */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00, /* . */
    0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00, /* slash */
    0x7C, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0x7C, 0x00, /* 0 */
    0x30, 0x70, 0x30, 0x30, 0x30, 0x30, 0xFC, 0x00, /* 1 */
    0x78, 0xCC, 0x0C, 0x38, 0x60, 0xCC, 0xFC, 0x00, /* 2 */
    0x78, 0xCC, 0x0C, 0x38, 0x0C, 0xCC, 0x78, 0x00, /* 3 */
    0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x1E, 0x00, /* 4 */
    0xFC, 0xC0, 0xF8, 0x0C, 0x0C, 0xCC, 0x78, 0x00, /* 5 */
    0x38, 0x60, 0xC0, 0xF8, 0xCC, 0xCC, 0x78, 0x00, /* 6 */
    0xFC, 0xCC, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00, /* 7 */
    0x78, 0xCC, 0xCC, 0x78, 0xCC, 0xCC, 0x78, 0x00, /* 8 */
    0x78, 0xCC, 0xCC, 0x7C, 0x0C, 0x18, 0x70, 0x00, /* 9 */
    0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x00, /* : */
    0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x60, /* ; */
    0x18, 0x30, 0x60, 0xC0, 0x60, 0x30, 0x18, 0x00, /* < */
    0x00, 0x00, 0xFC, 0x00, 0x00, 0xFC, 0x00, 0x00, /* = */
    0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00, /* > */
    0x78, 0xCC, 0x0C, 0x18, 0x30, 0x00, 0x30, 0x00, /* ? */
    0x7C, 0xC6, 0xDE, 0xDE, 0xDE, 0xC0, 0x78, 0x00, /* @ */
    0x30, 0x78, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0x00, /* A */
    0xFC, 0x66, 0x66, 0x7C, 0x66, 0x66, 0xFC, 0x00, /* B */
    0x3C, 0x66, 0xC0, 0xC0, 0xC0, 0x66, 0x3C, 0x00, /* C */
    0xF8, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00, /* D */
    0xFE, 0x62, 0x68, 0x78, 0x68, 0x62, 0xFE, 0x00, /* E */
    0xFE, 0x62, 0x68, 0x78, 0x68, 0x60, 0xF0, 0x00, /* F */
    0x3C, 0x66, 0xC0, 0xC0, 0xCE, 0x66, 0x3E, 0x00, /* G */
    0xCC, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0xCC, 0x00, /* H */
    0x78, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00, /* I */
    0x1E, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78, 0x00, /* J */
    0xE6, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0xE6, 0x00, /* K */
    0xF0, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0x00, /* L */
    0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0x00, /* M */
    0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00, /* N */
    0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00, /* O */
    0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00, /* P */
    0x78, 0xCC, 0xCC, 0xCC, 0xDC, 0x78, 0x1C, 0x00, /* Q */
    0xFC, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0xE6, 0x00, /* R */
    0x78, 0xCC, 0xE0, 0x70, 0x1C, 0xCC, 0x78, 0x00, /* S */
    0xFC, 0xB4, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00, /* T */
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xFC, 0x00, /* U */
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00, /* V */
    0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00, /* W */
    0xC6, 0xC6, 0x6C, 0x38, 0x38, 0x6C, 0xC6, 0x00, /* X */
    0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x30, 0x78, 0x00, /* Y */
    0xFE, 0xC6, 0x8C, 0x18, 0x32, 0x66, 0xFE, 0x00, /* Z */
    0x78, 0x60, 0x60, 0x60, 0x60, 0x60, 0x78, 0x00, /* [ */
    0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00, /* backslash */
    0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x78, 0x00, /* ] */
    0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00, /* ^ */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, /* _ */
    0x30, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, /* ` */
    0x00, 0x00, 0x78, 0x0C, 0x7C, 0xCC, 0x76, 0x00, /* a */
    0xE0, 0x60, 0x60, 0x7C, 0x66, 0x66, 0xDC, 0x00, /* b */
    0x00, 0x00, 0x78, 0xCC, 0xC0, 0xCC, 0x78, 0x00, /* c */
    0x1C, 0x0C, 0x0C, 0x7C, 0xCC, 0xCC, 0x76, 0x00, /* d */
    0x00, 0x00, 0x78, 0xCC, 0xFC, 0xC0, 0x78, 0x00, /* e */
    0x38, 0x6C, 0x60, 0xF0, 0x60, 0x60, 0xF0, 0x00, /* f */
    0x00, 0x00, 0x76, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8, /* g */
    0xE0, 0x60, 0x6C, 0x76, 0x66, 0x66, 0xE6, 0x00, /* h */
    0x30, 0x00, 0x70, 0x30, 0x30, 0x30, 0x78, 0x00, /* i */
    0x0C, 0x00, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78, /* j */
    0xE0, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0xE6, 0x00, /* k */
    0x70, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00, /* l */
    0x00, 0x00, 0xCC, 0xFE, 0xFE, 0xD6, 0xC6, 0x00, /* m */
    0x00, 0x00, 0xF8, 0xCC, 0xCC, 0xCC, 0xCC, 0x00, /* n */
    0x00, 0x00, 0x78, 0xCC, 0xCC, 0xCC, 0x78, 0x00, /* o */
    0x00, 0x00, 0xDC, 0x66, 0x66, 0x7C, 0x60, 0xF0, /* p */
    0x00, 0x00, 0x76, 0xCC, 0xCC, 0x7C, 0x0C, 0x1E, /* q */
    0x00, 0x00, 0xDC, 0x76, 0x66, 0x60, 0xF0, 0x00, /* r */
    0x00, 0x00, 0x7C, 0xC0, 0x78, 0x0C, 0xF8, 0x00, /* s */
    0x10, 0x30, 0x7C, 0x30, 0x30, 0x34, 0x18, 0x00, /* t */
    0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0x76, 0x00, /* u */
    0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00, /* v */
    0x00, 0x00, 0xC6, 0xD6, 0xFE, 0xFE, 0x6C, 0x00, /* w */
    0x00, 0x00, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0x00, /* x */
    0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8, /* y */
    0x00, 0x00, 0xFC, 0x98, 0x30, 0x64, 0xFC, 0x00, /* z */
    0x1C, 0x30, 0x30, 0xE0, 0x30, 0x30, 0x1C, 0x00, /* { */
    0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00, /* | */
    0xE0, 0x30, 0x30, 0x1C, 0x30, 0x30, 0xE0, 0x00, /* } */
    0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* ~ */
};

/* The failure screen settles on one font before drawing (ofLoadingFail): -1
 * until then, so asking can't load a texture (and fail again) mid-screen. */
static int g_gameFont = -1;

static bool gameFont(void) {
    return g_gameFont >= 0 ? g_gameFont != 0 : ofFontReady();
}

static int lineHeight(void) {
    return gameFont() ? ofFontLineHeight(1) : OF_TERM_LINE;
}

static int textWidth(const char *text) {
    return gameFont() ? ofFontWidth(text, 1) : (int) strlen(text) * 8;
}

static void drawText(uint16_t *fb, int width, int height, int x, int y, const char *text, uint16_t color) {
    if (gameFont()) {
        ofFontDraw(fb, width, height, x, y, text, color, 1);
        return;
    }
    for (; *text != '\0'; text++, x += 8) {
        if (*text < OF_TERM_FIRST || *text > OF_TERM_LAST) continue;
        const uint8_t *glyph = &g_termFont[(*text - OF_TERM_FIRST) * 8];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++) {
                int px = x + col, py = y + row;
                if ((glyph[row] & (0x80 >> col)) && px >= 0 && px < width && py >= 0 && py < height)
                    fb[py * width + px] = color;
            }
    }
}

/* Draws `text` from (x, y), broken between words to fit `right`; returns the
 * y below it. With no frame it only measures. */
static int drawWrapped(uint16_t *fb, int width, int height, int x, int y, int right, const char *text, uint16_t color) {
    int line = lineHeight();
    char buffer[128];
    while (*text != '\0') {
        while (*text == ' ') text++;
        if (*text == '\0') break;
        /* The longest run of whole words that fits, or one word cut where it must be. */
        size_t fit = 0, end = 0;
        for (;;) {
            size_t next = end;
            while (text[next] == ' ') next++;
            while (text[next] != '\0' && text[next] != ' ') next++;
            if (next == end || next >= sizeof(buffer)) break;
            memcpy(buffer, text, next);
            buffer[next] = '\0';
            if (x + textWidth(buffer) > right) break;
            fit = end = next;
            if (text[next] == '\0') break;
        }
        if (fit == 0) { /* a word wider than the line */
            while (text[fit] != '\0' && text[fit] != ' ' && fit + 1 < sizeof(buffer)) {
                buffer[fit] = text[fit];
                buffer[fit + 1] = '\0';
                if (x + textWidth(buffer) > right) break;
                fit++;
            }
            if (fit == 0) fit = 1;
        }
        memcpy(buffer, text, fit);
        buffer[fit] = '\0';
        if (fb != NULL) drawText(fb, width, height, x, y, buffer, color);
        y += line;
        text += fit;
    }
    return y;
}

void ofLoadingDrawError(uint16_t *fb, int width, int height, const OfLoadError *error, int permille) {
    for (int i = 0; i < width * height; i++) fb[i] = OF_BLACK;
    int line = lineHeight();
    int left = textLeft(width), right = width - left;
    int y = height / 6;
    y = drawWrapped(fb, width, height, left, y, right, error->title, OF_WHITE) + line;
    y = drawWrapped(fb, width, height, left, y, right, error->reason, OF_WHITE);
    if (error->hint != NULL) y = drawWrapped(fb, width, height, left, y, right, error->hint, OF_LIGHT);
    if (error->detail != NULL && error->detail[0] != '\0')
        drawWrapped(fb, width, height, left, y + line, right, error->detail, OF_GREY);
    if (error->footer != NULL) {
        /* Above the bar, as many lines as it wraps to. */
        int lines = 0;
        for (int probe = drawWrapped(NULL, 0, 0, left, 0, right, error->footer, 0); probe > 0; probe -= line) lines++;
        drawWrapped(fb, width, height, left, barTop(height) - line / 2 - lines * line, right, error->footer, OF_LIGHT);
    }
    ofLoadingDrawBar(fb, width, height, permille < 30 ? 30 : permille, OF_RED);
}

static bool mentions(const char *text, const char *what) {
    return text != NULL && strstr(text, what) != NULL;
}

void ofLoadingDescribe(OfLoadError *error, const char *title, const char *dataFiles, const char *lastError,
                       const char *why, bool dataLoaded, const char *footer) {
    static char heading[64], hint[160];
    snprintf(heading, sizeof(heading), "Couldn't load %s", title);
    error->title = heading;
    error->footer = footer;
    error->hint = NULL;
    error->detail = lastError != NULL && lastError[0] != '\0' ? lastError : why;

    if (mentions(lastError, "annot open") || mentions(lastError, "Failed to open")) {
        error->reason = "The game data is missing.";
        snprintf(hint, sizeof(hint), "Copy %s to /Assets/butterscotch/common/ on the SD card.", dataFiles);
        error->hint = hint;
    } else if (mentions(lastError, "alloc(")) {
        error->reason = "It ran out of memory while loading.";
    } else if (!dataLoaded) {
        error->reason = "The game data could not be read.";
        snprintf(hint, sizeof(hint), "Prepare %s again and copy them to /Assets/butterscotch/common/.", dataFiles);
        error->hint = hint;
    } else {
        error->reason = "The game stopped while starting.";
    }
}

#ifndef OF_LOADING_PREVIEW

#include "log.h"
#include "of_game.h"
#include "of_perf.h"

#include <unistd.h>
#ifndef OF_PC
#include "of.h"
#endif

/* Chunks as the loader meets them, with what each weighs. */
#define OF_LOADING_CHUNKS 48
static struct { char name[4]; uint32_t weight; } g_plan[OF_LOADING_CHUNKS];
static int g_planCount = 0;
static int g_planNext = 0;

/* A data file of about Undertale's weight, for when the chunk table can't be read. */
#define OF_GUESS_DATA_WEIGHT 16000u
/* The rest of loading, after the data file, against the data file's weight. */
#define OF_POST_SHARE_NUM 1
#define OF_POST_SHARE_DEN 4
/* A long read moves the bar this much a piece (BULK_READ_PIECE, 64 KB). */
#define OF_TICK_WEIGHT 64u

static bool g_active = false;
static bool g_dataLoaded = false;
static uint32_t g_dataWeight = 0;  /* the data file: the strings pre-read and every chunk */
static uint32_t g_postWeight = 0;
static uint32_t g_done = 0;        /* whole stages behind */
static uint32_t g_stage = 0;       /* weight of the stage under way */
static uint32_t g_stageDone = 0;
static int g_permille = 0;
static int g_drawnFill = -1;       /* bar pixels drawn on the current surface */

bool ofLoadingActive(void) {
    return g_active;
}

int ofLoadingPermille(void) {
    return g_permille;
}

static uint32_t total(void) {
    return g_dataWeight + g_postWeight;
}

static void present(void) {
    bool fresh = false;
    uint16_t *fb = ofPlatformLoadingSurface(&fresh);
    if (fb == NULL) return;
    if (fresh) for (int i = 0; i < OF_LOADING_WIDTH * OF_LOADING_HEIGHT; i++) fb[i] = OF_BLACK;
    ofLoadingDrawBar(fb, OF_LOADING_WIDTH, OF_LOADING_HEIGHT, g_permille, OF_WHITE);
    ofPlatformLoadingFlip();
}

static void update(void) {
    if (!g_active) return;
    uint32_t sum = total();
    if (sum == 0) return;
    uint32_t stage = g_stageDone < g_stage ? g_stageDone : g_stage;
    int permille = (int) ((uint64_t) (g_done + stage) * 1000u / sum);
    if (permille > 1000) permille = 1000;
    if (permille <= g_permille) return; /* never backwards */
    g_permille = permille;
    /* Only when the bar grows by a pixel: a flip costs a frame buffer each time. */
    int fill = barWidth(OF_LOADING_WIDTH) * permille / 1000;
    if (fill == g_drawnFill) return;
    g_drawnFill = fill;
    present();
}

void ofLoadingBegin(void) {
    g_active = true;
    g_dataLoaded = false;
    g_done = g_stageDone = 0;
    g_permille = 0;
    g_drawnFill = -1;
    g_planCount = g_planNext = 0;
    g_dataWeight = OF_GUESS_DATA_WEIGHT;
    g_postWeight = g_dataWeight * OF_POST_SHARE_NUM / OF_POST_SHARE_DEN;
    /* With no plan, the reads move the bar through the guessed data file. */
    g_stage = g_dataWeight;
    ofLogSetConsole(false);
    present();
}

static uint32_t readLe32(const uint8_t *p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

/* What a chunk costs to load, in about milliseconds: its size in KB, except
 * where the loader reads less of it (data_win.c). */
static uint32_t chunkWeight(const uint8_t name[4], uint32_t length) {
    uint32_t kb = length / 1024u + 1u;
    if (memcmp(name, "AUDO", 4) == 0) return 0;               /* sounds load as played */
    if (memcmp(name, "TXTR", 4) == 0) return kb < 256u ? kb : 256u; /* the entry table only */
    if (memcmp(name, "STRG", 4) == 0) return kb / 16u + 1u;   /* read already, parsed here */
    static const char *const parsed[] = {
        "GEN8", "OPTN", "LANG", "EXTN", "SOND", "AGRP", "SPRT", "BGND", "PATH", "SCPT", "GLOB", "SHDR",
        "FONT", "TMLN", "OBJT", "ROOM", "TGIN", "TPAG", "CODE", "VARI", "FUNC", "ACRV",
    };
    for (size_t i = 0; i < sizeof(parsed) / sizeof(parsed[0]); i++)
        if (memcmp(name, parsed[i], 4) == 0) return kb;
    return 0; /* skipped */
}

void ofLoadingPlan(const char *dataWinPath) {
    if (!g_active) return;
    FILE *file = fopen(dataWinPath, "rb");
    if (file == NULL) return;
    uint8_t header[8];
    uint32_t weight = 0;
    if (fread(header, 1, 8, file) == 8 && memcmp(header, "FORM", 4) == 0) {
        long end = 8 + (long) readLe32(header + 4);
        long at = 8;
        g_planCount = 0;
        while (at + 8 <= end && g_planCount < OF_LOADING_CHUNKS) {
            if (fseek(file, at, SEEK_SET) != 0 || fread(header, 1, 8, file) != 8) break;
            uint32_t length = readLe32(header + 4);
            memcpy(g_plan[g_planCount].name, header, 4);
            g_plan[g_planCount].weight = chunkWeight(header, length);
            weight += g_plan[g_planCount].weight;
            /* The loader reads all of STRG before the first chunk is parsed. */
            if (memcmp(header, "STRG", 4) == 0) weight += length / 1024u;
            g_planCount++;
            at += 8 + (long) length;
        }
    }
    fclose(file);
    if (weight == 0) {
        g_planCount = 0;
        return;
    }
    g_dataWeight = weight;
    g_postWeight = weight * OF_POST_SHARE_NUM / OF_POST_SHARE_DEN;
    /* Until the first chunk: the strings being read, what the data file weighs beyond its chunks. */
    g_stage = g_dataWeight;
    for (int i = 0; i < g_planCount; i++) g_stage -= g_plan[i].weight;
    g_stageDone = 0;
}

static void nextStage(uint32_t weight) {
    g_done += g_stage;
    g_stage = weight;
    g_stageDone = 0;
}

/* After the data file: where each step ends, in hundredths of the rest, by an
 * Undertale load on the Pocket (2026-11-06). */
static const struct { const char *format; int at; } g_milestones[] = {
    { "VM: Initialized", 20 },
    { "SWRenderer initialized", 29 },
    { "Audio: pack", 33 },
    { "Buttons:", 38 },
    { "Runner: built without physics", 55 },
    { "Runner: Room loaded:", 100 },
};

void ofLoadingNote(const char *format, const char *text) {
    if (!g_active) return;
    if (strncmp(format, "DataWin: %.4s", 13) == 0) {
        if (g_planCount == 0) return; /* no plan: the guessed stage runs on */
        uint32_t weight = 0;
        /* The next chunk of the plan with this name. */
        for (int i = g_planNext; i < g_planCount; i++) {
            if (memcmp(g_plan[i].name, text + 9, 4) == 0) {
                weight = g_plan[i].weight;
                g_planNext = i + 1;
                break;
            }
        }
        nextStage(weight);
    } else if (strncmp(format, "Loaded \"", 8) == 0) {
        g_dataLoaded = true;
        g_done = g_dataWeight;
        g_stage = g_stageDone = 0;
    } else if (g_dataLoaded) {
        for (size_t i = 0; i < sizeof(g_milestones) / sizeof(g_milestones[0]); i++) {
            if (strncmp(format, g_milestones[i].format, strlen(g_milestones[i].format)) != 0) continue;
            uint32_t at = g_dataWeight + g_postWeight * (uint32_t) g_milestones[i].at / 100u;
            if (at > g_done) g_done = at;
            g_stage = g_stageDone = 0;
        }
    }
    update();
}

void ofLoadingTick(void) {
    if (!g_active) return;
    /* Short of the stage's end: what the stage parses after reading is not counted. */
    if (g_stageDone + OF_TICK_WEIGHT <= g_stage - g_stage / 8) g_stageDone += OF_TICK_WEIGHT;
    update();
}

void ofLoadingEnd(void) {
    if (!g_active) return;
    g_active = false;
    ofLogSetConsole(true);
}

void ofLoadingFail(const char *why, bool logSaved) {
    if (!g_active) return;
    static char footer[128];
    if (logSaved)
        snprintf(footer, sizeof(footer), "%s",
                 "Quit from the Analogue menu to save the log: the entry's _1.sav file in Saves/butterscotch/common.");
    /* What went wrong first, kept: choosing the game's font below may load its
     * texture, and if that fails too it comes back here with a newer error. */
    static bool failing = false;
    static OfLoadError error;
    static char detail[160];
    if (failing) {
        g_gameFont = 0; /* the terminal font, and the first error */
    } else {
        failing = true;
        ofLoadingDescribe(&error, ofGame.title, ofGame.dataFiles, ofLogLastError(), why, g_dataLoaded,
                          logSaved ? footer : NULL);
        if (error.detail != NULL) {
            snprintf(detail, sizeof(detail), "%s", error.detail);
            error.detail = detail;
        }
        /* One font for the whole screen. */
        g_gameFont = 0;
        g_gameFont = ofFontReady() ? 1 : 0;
    }
    for (;;) {
        bool fresh = false;
        uint16_t *fb = ofPlatformLoadingSurface(&fresh);
        if (fb == NULL) {
            /* The caller falls back to the text terminal: say it there. */
            ofLoadingEnd();
            ofLogPrint("%s\n%s\n", error.title, error.reason);
            if (error.detail != NULL && error.detail[0] != '\0') ofLogPrint("%s\n", error.detail);
            return;
        }
        ofLoadingDrawError(fb, OF_LOADING_WIDTH, OF_LOADING_HEIGHT, &error, g_permille);
        ofPlatformLoadingFlip();
#ifdef OF_PC
        return;
#else
        of_input_poll();
        usleep(100000);
#endif
    }
}

#endif /* !OF_LOADING_PREVIEW */
