/* Exercise the real platform event handler with injected physical buttons.
 * No game assets or initialized window/audio are needed. */
#define of_input_poll fixtureInputPoll
#define of_btn fixtureButton
#define of_btn_pressed fixtureButtonPressed
#define of_btn_released fixtureButtonReleased
#include "../platform/of_platform.c"
#undef of_input_poll
#undef of_btn
#undef of_btn_pressed
#undef of_btn_released
#include <assert.h>
static uint32_t held, pressed, released;
void fixtureInputPoll(void) {}
int fixtureButton(uint32_t mask) { return (held & mask) != 0; }
int fixtureButtonPressed(uint32_t mask) { return (pressed & mask) != 0; }
int fixtureButtonReleased(uint32_t mask) { return (released & mask) != 0; }
int main(void) {
    g_uncapped = true;
    for (int speed = 0; speed <= 1; speed++) {
        g_smoothLowres = speed != 0;
        g_modeShownUntil = 0;
        for (int frame = 0; frame < 3; frame++) {
            held = frame < 2 ? OF_BTN_L1 : 0;
            pressed = frame == 0 ? OF_BTN_L1 : 0;
            released = frame == 2 ? OF_BTN_L1 : 0;
            assert(!platformHandleEvents());
            assert(g_smoothLowres == (speed != 0));
            assert(g_modeShownUntil == 0);
        }
        /* The existing Performance Mode menu row remains functional. */
        assert(!menuVideo(1));
        assert(g_smoothLowres == (speed == 0));
        assert(!strcmp(menuVideoValue(), speed ? "Accuracy" : "Speed"));
    }
    puts("Performance Mode: L inactive, menu selection retained in both modes");
    return 0;
}
