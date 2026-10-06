/* stub_intro.c - stub when the boot intro is compiled out */

#include "gui_internal.h"
#include "intro.h"

int gui_show_intro(gui_state_t *state, CHAR16 *path, UINTN duration_ms,
                   int loop, int auto_continue) {
    (void)state; (void)path; (void)duration_ms;
    (void)loop; (void)auto_continue;
    return INTRO_BYPASSED;
}
