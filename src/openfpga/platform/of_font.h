#ifndef OF_FONT_H
#define OF_FONT_H

#include <stdint.h>

/* Text for the port's own screens (the menu and the notices in the top right
 * corner) in the game's own font, so they read like the game around them.
 *
 * The glyphs are copied out of the game's texture page the first time text
 * is drawn, into a small bitmap of this module's, so later text never waits
 * on the texture cache or loses its glyphs to it. If the game has no font
 * that fits, the log overlay's small font is used instead. */
struct Runner;
void ofFontSetRunner(struct Runner *runner);

/* Whether the game's font is there to draw with (data.win has loaded and
 * has one); before that the functions below use the log's small font. */
#include <stdbool.h>
bool ofFontReady(void);

/* Scale for a frame: 1 at 320x240, 2 at 640x480, so text is the same size on
 * the screen whatever the room's resolution. */
int ofFontScale(int frameWidth);
/* Height of a line and width of a string, in frame pixels. */
int ofFontLineHeight(int scale);
int ofFontWidth(const char *text, int scale);
/* Draws one line of text with its top left at (x, y); returns the x after it.
 * Pixels outside the frame are left out. */
int ofFontDraw(uint16_t *fb, int width, int height, int x, int y, const char *text, uint16_t color, int scale);

#endif
