/* The loading screen's progress bar and failure screen (src/openfpga/platform/of_loading.c).
 *
 * Replays an Undertale load as the Pocket logged it (2026-11-06, rc.25) against a data file with the same chunk
 * table, and checks that the bar only grows, follows the clock closely, and fills at the first room. Then checks
 * what the failure screen says for each kind of failure. With OF_LOADING_PREVIEW_DIR set it also writes the
 * screens as PNG files there (the text in the OS terminal font, as on the device before data.win has loaded). */
#define OF_PC 1
#define OF_GAME_NAME "undertale"
#define OF_LOG_SLOT_FILE "undertale_1.sav"
#include "../src/openfpga/platform/of_loading.c"
#include "../src/openfpga/platform/debug_font.c"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <assert.h>
#include <stdlib.h>

const OfGame ofGame = { .title = "Undertale", .dataFiles = "data.win, textures.bin and music.bin" };

static bool g_console = true;
void ofLogSetConsole(bool enabled) { g_console = enabled; }
static const char *g_lastError = "";
const char *ofLogLastError(void) { return g_lastError; }

bool ofFontReady(void) { return false; }
/* ofFontDraw before the game's font is ready: of_perf.c's 6x12 fallback, copied. */
int ofFontLineHeight(int scale) { (void) scale; return OF_LOG_CELL_H; }
int ofFontWidth(const char *text, int scale) { (void) scale; return (int) strlen(text) * OF_LOG_CELL_W; }
int ofFontDraw(uint16_t *fb, int width, int height, int x, int y, const char *text, uint16_t color, int scale) {
    (void) scale;
    for (; *text != '\0'; text++, x += OF_LOG_CELL_W) {
        char c = *text;
        if (c < DEBUGFONT_FIRST_CP || c > DEBUGFONT_LAST_CP) continue;
        const DebugFontGlyphEntry *glyph = &debugFontGlyphs[c - DEBUGFONT_FIRST_CP];
        for (int gy = 0; gy < glyph->h; gy += 3)
            for (int gx = 0; gx < glyph->w; gx += 3) {
                int sum = 0;
                for (int sy = 0; sy < 3 && gy + sy < glyph->h; sy++)
                    for (int sx = 0; sx < 3 && gx + sx < glyph->w; sx++)
                        sum += debugFontPixels[(glyph->y + gy + sy) * DEBUGFONT_ATLAS_W + glyph->x + gx + sx];
                if (sum < 80 * 9) continue;
                int px = x + (glyph->xoffset + gx) / 3, py = y + (glyph->yoffset + gy) / 3;
                if (px >= 0 && px < width && py >= 0 && py < height) fb[py * width + px] = color;
            }
    }
    return x;
}

static uint16_t g_fb[OF_LOADING_WIDTH * OF_LOADING_HEIGHT];
static int g_flips = 0;
uint16_t *ofPlatformLoadingSurface(bool *fresh) { *fresh = g_flips == 0; return g_fb; }
void ofPlatformLoadingFlip(void) { g_flips++; }

static void preview(const char *name) {
    const char *dir = getenv("OF_LOADING_PREVIEW_DIR");
    if (dir == NULL) return;
    static uint8_t rgb[OF_LOADING_WIDTH * OF_LOADING_HEIGHT * 3];
    for (int i = 0; i < OF_LOADING_WIDTH * OF_LOADING_HEIGHT; i++) {
        uint16_t p = g_fb[i];
        rgb[i * 3] = (uint8_t) (((p >> 10) & 31) * 255 / 31);
        rgb[i * 3 + 1] = (uint8_t) (((p >> 5) & 31) * 255 / 31);
        rgb[i * 3 + 2] = (uint8_t) ((p & 31) * 255 / 31);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    assert(stbi_write_png(path, OF_LOADING_WIDTH, OF_LOADING_HEIGHT, 3, rgb, OF_LOADING_WIDTH * 3));
}

/* Undertale's chunks (KB) and when the Pocket started each, in seconds. */
static const struct { const char *name; uint32_t kb; double at; } g_chunks[] = {
    { "GEN8", 1, 2.95 }, { "OPTN", 0, 2.96 }, { "LANG", 0, 2.96 }, { "EXTN", 0, 2.97 }, { "SOND", 17, 2.97 },
    { "AGRP", 0, 2.98 }, { "SPRT", 1930, 2.99 }, { "BGND", 5, 4.88 }, { "PATH", 3, 4.91 }, { "SCPT", 2, 4.92 },
    { "GLOB", 0, 4.93 }, { "SHDR", 0, 4.93 }, { "FONT", 145, 4.93 }, { "TMLN", 0, 5.15 }, { "OBJT", 731, 5.16 },
    { "ROOM", 4401, 6.11 }, { "DAFL", 0, 9.88 }, { "TPAG", 166, 9.88 }, { "CODE", 5141, 10.16 },
    { "VARI", 213, 14.93 }, { "FUNC", 108, 15.16 }, { "STRG", 2791, 15.34 }, { "TXTR", 12423, 15.47 },
    { "AUDO", 33398, 15.73 },
};
#define CHUNKS (int) (sizeof(g_chunks) / sizeof(g_chunks[0]))
/* After the data file: the line, and when it was logged. */
static const struct { const char *format, *text; double at; } g_after[] = {
    { "Loaded \"%s\"", "Loaded \"UNDERTALE\"", 15.75 },
    { "VM: Initialized with %u functions mapped\n", "VM: Initialized with 6509 functions mapped\n", 16.48 },
    { "SWRenderer initialized.\n", "SWRenderer initialized.\n", 16.83 },
    { "Audio: pack with %u tracks.\n", "Audio: pack with 443 tracks.\n", 16.94 },
    { "Buttons: attack prompt %s\n", "Buttons: attack prompt installed\n", 17.14 },
    { "Runner: built without physics support\n", "Runner: built without physics support\n", 17.80 },
    { "Runner: Room loaded: %s\n", "Runner: Room loaded: room_start\n", 19.51 },
};
#define LOAD_SECONDS 19.51

static void makeDataWin(const char *path) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    uint32_t form = 0;
    for (int i = 0; i < CHUNKS; i++) form += 8 + g_chunks[i].kb * 1024u;
    fwrite("FORM", 1, 4, f);
    fwrite(&form, 4, 1, f); /* little-endian hosts only, as the device */
    long at = 8;
    for (int i = 0; i < CHUNKS; i++) {
        uint32_t length = g_chunks[i].kb * 1024u;
        fseek(f, at, SEEK_SET);
        fwrite(g_chunks[i].name, 1, 4, f);
        fwrite(&length, 4, 1, f);
        at += 8 + (long) length;
    }
    fclose(f);
}

static int g_last = 0;
static double g_worst = 0; /* furthest the bar strays from the clock, in thousandths */
static void check(double at) {
    int now = ofLoadingPermille();
    assert(now >= g_last);
    g_last = now;
    double off = now - at / LOAD_SECONDS * 1000.0;
    if (off < 0) off = -off;
    if (off > g_worst) g_worst = off;
}

/* `count` reads spread over the stage that ends at `end`, checked as they go. */
static void ticks(int count, double start, double end) {
    for (int i = 1; i <= count; i++) {
        ofLoadingTick();
        check(start + (end - start) * i / count);
    }
}

static void testLoad(void) {
    const char *path = "of_loading_test.win";
    makeDataWin(path);
    ofLoadingBegin();
    assert(ofLoadingActive() && !g_console && g_flips == 1);
    ofLoadingPlan(path);
    remove(path);

    ticks((int) (2791 / 64), 0.0, g_chunks[0].at); /* the strings, read before the first chunk */
    for (int i = 0; i < CHUNKS; i++) {
        char text[64];
        snprintf(text, sizeof(text), "DataWin: %s, %u KB\n", g_chunks[i].name, g_chunks[i].kb);
        ofLoadingNote("DataWin: %.4s, %u KB%s\n", text);
        check(g_chunks[i].at);
        double end = i + 1 < CHUNKS ? g_chunks[i + 1].at : g_after[0].at;
        uint32_t read = strcmp(g_chunks[i].name, "TXTR") == 0 ? 256 : strcmp(g_chunks[i].name, "AUDO") == 0 ? 0
                      : strcmp(g_chunks[i].name, "STRG") == 0 ? 0 : g_chunks[i].kb;
        ticks((int) ((read + 63) / 64), g_chunks[i].at, end);
    }
    int beforeAfter = ofLoadingPermille();
    for (size_t i = 0; i < sizeof(g_after) / sizeof(g_after[0]); i++) {
        ofLoadingNote(g_after[i].format, g_after[i].text);
        check(g_after[i].at);
    }
    assert(beforeAfter > 700 && beforeAfter < 850);
    assert(ofLoadingPermille() == 1000);
    printf("loading bar: %d flips, at most %.0f thousandths off the clock\n", g_flips, g_worst);
    assert(g_worst < 120);
    assert(g_flips < 300);

    ofLoadingEnd();
    assert(!ofLoadingActive() && g_console);
}

static void testDescribe(void) {
    OfLoadError e;
    ofLoadingDescribe(&e, "Undertale", "data.win, textures.bin and music.bin", "Cannot open data.win", "app exited",
                      false, NULL);
    assert(strstr(e.reason, "missing") && strstr(e.hint, "data.win") && strcmp(e.detail, "Cannot open data.win") == 0);
    ofLoadingDescribe(&e, "Undertale", "x", "FATAL: malloc(4507012) failed at src/data_win.c:3290", "abort() called", false, NULL);
    assert(strstr(e.reason, "memory") && e.hint == NULL);
    ofLoadingDescribe(&e, "Undertale", "x", "DataWin: short read on chunk CODE (expected 5264384, got 0)", "app exited",
                      false, NULL);
    assert(strstr(e.reason, "could not be read") && e.hint != NULL);
    ofLoadingDescribe(&e, "Undertale", "x", "", "app exited", true, NULL);
    assert(strstr(e.reason, "stopped") && strcmp(e.detail, "app exited") == 0);
}

static void testScreens(void) {
    memset(g_fb, 0, sizeof(g_fb));
    ofLoadingDrawBar(g_fb, OF_LOADING_WIDTH, OF_LOADING_HEIGHT, 420, 0x7FFF);
    /* The bar sits at the bottom: nothing above it, white then grey along it. */
    for (int y = 0; y < barTop(OF_LOADING_HEIGHT); y++)
        for (int x = 0; x < OF_LOADING_WIDTH; x++) assert(g_fb[y * OF_LOADING_WIDTH + x] == 0);
    int row = barTop(OF_LOADING_HEIGHT) * OF_LOADING_WIDTH;
    assert(g_fb[row + barLeft(OF_LOADING_WIDTH)] == 0x7FFF && g_fb[row + OF_LOADING_WIDTH - barLeft(OF_LOADING_WIDTH) - 1] == OF_TRACK);
    preview("loading-bar");

    static const char *const cases[][3] = {
        { "missing", "Cannot open data.win", "app exited" },
        { "bad-data", "DataWin: short read on chunk CODE (expected 5264384, got 1048576)", "app exited" },
    };
    for (int i = 0; i < 2; i++) {
        OfLoadError e;
        ofLoadingDescribe(&e, "Undertale", "data.win, textures.bin and music.bin", cases[i][1], cases[i][2], false,
                          "Quit from the Analogue menu to save the log: the entry's _1.sav file in Saves/butterscotch/common.");
        ofLoadingDrawError(g_fb, OF_LOADING_WIDTH, OF_LOADING_HEIGHT, &e, i == 0 ? 0 : 520);
        assert(g_fb[row + barLeft(OF_LOADING_WIDTH)] == OF_RED);
        char name[64];
        snprintf(name, sizeof(name), "loading-error-%s", cases[i][0]);
        preview(name);
    }
}

int main(void) {
    testLoad();
    testDescribe();
    testScreens();
    printf("loading screen: passed\n");
    return 0;
}
