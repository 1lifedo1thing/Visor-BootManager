/* stub_audio.c - stub when audio is compiled out */

#include "menu_sound.h"

int menu_sound_prepare(int enabled, CHAR16 *path) {
    (void)enabled; (void)path;
    return 0;
}

void menu_sound_start(void)  { }
void menu_sound_poll(void)   { }
void menu_sound_stop(void)   { }
void menu_sound_finish(void) { }

/* Nothing is ever audible, so a recording gets a silent track. */
int hda_tap(const INT16 **pcm, UINTN *frames, UINT64 *start_us) {
    (void)pcm; (void)frames; (void)start_us;
    return 0;
}
