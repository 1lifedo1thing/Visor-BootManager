#ifndef INTRO_H
#define INTRO_H

#include <efi.h>
#include "gui.h"

/* What gui_show_intro() did with the configured media.  INTRO_BYPASSED is
 * also the "no intro happened" answer, so a caller that ignores the return
 * value still falls through to the menu. */
#define INTRO_BYPASSED 0   /* nothing configured, or unusable media */
#define INTRO_FINISHED 1   /* played out, intro_action now applies */
#define INTRO_SKIPPED  2   /* a key was pressed: show the menu */

/* Plays intro_media once, before the menu.  duration_ms is the automatic
 * deadline and is clamped internally; zero disables the intro.  loop and
 * auto_continue are intro_loop / intro_auto_continue.  Any key stops playback
 * and reports INTRO_SKIPPED. */
int gui_show_intro(gui_state_t *state, CHAR16 *path, UINTN duration_ms,
                   int loop, int auto_continue);

#endif
