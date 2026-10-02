/* stub_pointer.c - stub when mouse support is compiled out */

#include "gui_internal.h"

void cursor_backing_restore(gui_state_t *state, INTN ox, INTN oy) {
    (void)state; (void)ox; (void)oy;
}

INTN cursor_lift(gui_state_t *state) {
    (void)state;
    return -1;
}

void cursor_compose(gui_state_t *state) {
    (void)state;
}

void cursor_move(gui_state_t *state) {
    (void)state;
}

void cursor_overlay(gui_state_t *state) {
    (void)state;
}

void gui_set_cursor(gui_state_t *state, CHAR16 *path, UINTN size,
                    INTN hot_x, INTN hot_y) {
    (void)state; (void)path; (void)size; (void)hot_x; (void)hot_y;
}

int poll_pointer(gui_state_t *state, int *menu_redraw) {
    (void)state; (void)menu_redraw;
    return 0;
}
