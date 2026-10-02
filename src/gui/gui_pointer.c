/* gui_pointer.c - mouse cursor and pointer polling (feature: pointer) */
#include "gui_internal.h"

/* The pointer is composited straight into the backbuffer and lifted back out
 * of a saved copy of what it covered, so moving the mouse never costs a menu
 * repaint. Composing happens before the present rather than after it: a
 * present that ships a frame with the cursor missing is exactly what a blink
 * looks like on screen. */

#define CUR_DEFAULT_PX 24

static void draw_builtin_arrow(gui_state_t *state) {
    INTN cx = state->cursor_x, cy = state->cursor_y;
    for (INTN j = 0; j <= 20; j++) {
        INTN w = (j <= 14) ? j + 1 : (20 - j) * 3;
        if (w < 1) w = 1;
        fill_rect_alpha(state, cx - 1, cy + j, w + 2, 1, COLOR_BLACK, 220);
    }
    for (INTN j = 0; j <= 20; j++) {
        INTN w = (j <= 14) ? j + 1 : (20 - j) * 3;
        if (w < 1) w = 1;
        fill_rect_alpha(state, cx, cy + j, w, 1, COLOR_WHITE, 255);
    }
}

static void draw_cursor(gui_state_t *state) {
    if (state->cursor_icon) {
        draw_image_clipped_a(state, state->cursor_icon,
                             cursor_box_x(state), cursor_box_y(state),
                             state->cursor_px, 255);
        return;
    }
    draw_builtin_arrow(state);
}

/* The backing store follows the cursor box, which a theme can resize. */
static int cursor_backing_ready(gui_state_t *state) {
    if (state->cursor_box_w <= 0 || state->cursor_box_h <= 0) return 0;
    UINTN need = (UINTN)state->cursor_box_w * (UINTN)state->cursor_box_h;
    if (state->cursor_save && state->cursor_save_cap >= need) return 1;

    if (state->cursor_save) efi_free_pool(state->cursor_save);
    state->cursor_saved = 0;
    state->cursor_save = efi_allocate_pool(need * sizeof(UINT32));
    state->cursor_save_cap = state->cursor_save ? need : 0;
    return state->cursor_save != NULL;
}

static void cursor_backing_save(gui_state_t *state, INTN ox, INTN oy) {
    INTN w = state->cursor_box_w, h = state->cursor_box_h;
    for (INTN j = 0; j < h; j++)
        for (INTN i = 0; i < w; i++) {
            UINT32 *p = get_pixel(state, (UINTN)(ox + i), (UINTN)(oy + j));
            state->cursor_save[j * w + i] = p ? *p : 0;
        }
}

void cursor_backing_restore(gui_state_t *state, INTN ox, INTN oy) {
    if (!state->cursor_save) return;
    INTN w = state->cursor_box_w, h = state->cursor_box_h;
    for (INTN j = 0; j < h; j++)
        for (INTN i = 0; i < w; i++) {
            UINT32 *p = get_pixel(state, (UINTN)(ox + i), (UINTN)(oy + j));
            if (p) *p = state->cursor_save[j * w + i];
        }
}

/* Take the pointer back out of the backbuffer. Returns the top row of the
 * band it had dirtied so the caller can repaint that strip, or -1 if there
 * was nothing to lift. */
INTN cursor_lift(gui_state_t *state) {
    if (!state->cursor_saved) return -1;
    INTN y = state->cur_prev_y + state->cursor_off_y;
    cursor_backing_restore(state, state->cur_prev_x + state->cursor_off_x, y);
    state->cursor_saved = 0;
    return y;
}

/* Draw the pointer into the backbuffer and leave the screen alone. Callers
 * present afterwards, so the cursor rides along in the same blit as whatever
 * else changed instead of being painted in a second pass. */
void cursor_compose(gui_state_t *state) {
    if (!cursor_backing_ready(state)) return;
    cursor_backing_save(state, cursor_box_x(state), cursor_box_y(state));
    state->cur_prev_x = state->cursor_x;
    state->cur_prev_y = state->cursor_y;
    state->cursor_saved = 1;
    draw_cursor(state);
}

void cursor_overlay(gui_state_t *state) {
    cursor_compose(state);
    if (state->cursor_saved)
        gui_present_band(state, cursor_box_y(state), state->cursor_box_h);
}

void cursor_move(gui_state_t *state) {
    INTN old_y = cursor_lift(state);

    cursor_compose(state);
    if (!state->cursor_saved) return;

    INTN ny = cursor_box_y(state);
    INTN h  = state->cursor_box_h;
    if (old_y < 0) { gui_present_band(state, ny, h); return; }

    INTN lo = old_y < ny ? old_y : ny;
    INTN hi = (old_y > ny ? old_y : ny) + h;
    if (hi - lo <= 2 * h)
        gui_present_band(state, lo, hi - lo);
    else {
        gui_present_band(state, old_y, h);
        gui_present_band(state, ny, h);
    }
}

void gui_set_cursor(gui_state_t *state, CHAR16 *path, UINTN size,
                    INTN hot_x, INTN hot_y) {
    if (!state) return;

    if (state->cursor_icon) {
        if (state->cursor_icon->scaled) efi_free_pool(state->cursor_icon->scaled);
        if (state->cursor_icon->pixels) efi_free_pool(state->cursor_icon->pixels);
        efi_free_pool(state->cursor_icon);
        state->cursor_icon = NULL;
    }

    /* Back to the built-in arrow, whose tip sits one pixel right of the box. */
    state->cursor_px    = 0;
    state->cursor_hot_x = state->cursor_hot_y = 0;
    state->cursor_off_x = -1;
    state->cursor_off_y = 0;
    state->cursor_box_w = CUR_W;
    state->cursor_box_h = CUR_H;
    state->cursor_saved = 0;

    if (!path || !path[0]) return;

    efi_log(L"cursor: loading pointer image");
    icon_t *ic = gui_load_image(path);
    if (!ic) {
        efi_log(L"WARN: cursor image unusable - keeping the built-in arrow");
        return;
    }

    UINTN px = size ? size : ic->width;
    if (px < CUR_MIN_PX) px = CUR_MIN_PX;
    if (px > CUR_MAX_PX) px = CUR_MAX_PX;

    /* A negative hotspot means centre it - what a crosshair wants. */
    if (hot_x < 0) hot_x = (INTN)px / 2;
    if (hot_y < 0) hot_y = (INTN)px / 2;
    if (hot_x > (INTN)px - 1) hot_x = (INTN)px - 1;
    if (hot_y > (INTN)px - 1) hot_y = (INTN)px - 1;

    state->cursor_icon  = ic;
    state->cursor_px    = px;
    state->cursor_hot_x = hot_x;
    state->cursor_hot_y = hot_y;
    state->cursor_off_x = -hot_x;
    state->cursor_off_y = -hot_y;
    state->cursor_box_w = (INTN)px;
    state->cursor_box_h = (INTN)px;

    {
        CHAR16 d[112];
        SPrint(d, sizeof(d), L"cursor: %dx%d image drawn at %d px, hotspot %d,%d",
               (int)ic->width, (int)ic->height, (int)px, (int)hot_x, (int)hot_y);
        efi_log(d);
    }
}

int poll_pointer(gui_state_t *state, int *menu_redraw) {
    if (!state->mouse_enabled || !state->has_pointer) return 0;
    static int prev_btn = 0;
    int moved = 0, btn = 0, scroll = 0;

    if (state->app) {
        EFI_ABSOLUTE_POINTER_PROTOCOL *ap = state->app;
        EFI_ABSOLUTE_POINTER_STATE st;
        while (!EFI_ERROR(ap->GetState(ap, &st)) && ap->Mode) {
            UINT64 minx = ap->Mode->AbsoluteMinX, maxx = ap->Mode->AbsoluteMaxX;
            UINT64 miny = ap->Mode->AbsoluteMinY, maxy = ap->Mode->AbsoluteMaxY;
            if (maxx > minx)
                state->cursor_x = (INTN)((st.CurrentX - minx) * (state->screen_width - 1) / (maxx - minx));
            if (maxy > miny)
                state->cursor_y = (INTN)((st.CurrentY - miny) * (state->screen_height - 1) / (maxy - miny));
            moved = 1;
            if (st.ActiveButtons & EFI_ABSP_TouchActive) btn = 1;
        }
    }
    if (state->spp) {
        EFI_SIMPLE_POINTER_PROTOCOL *sp = state->spp;
        EFI_SIMPLE_POINTER_STATE st;
        INTN dx = 0, dy = 0;
        while (!EFI_ERROR(sp->GetState(sp, &st))) {
            UINT64 rx = sp->Mode ? sp->Mode->ResolutionX : 0;
            UINT64 ry = sp->Mode ? sp->Mode->ResolutionY : 0;
            INTN mx = st.RelativeMovementX, my = st.RelativeMovementY;
            if (rx > 1) mx = mx / (INTN)rx;
            if (ry > 1) my = my / (INTN)ry;
            dx += mx; dy += my;
            if (st.RelativeMovementZ > 0) scroll = 1;
            else if (st.RelativeMovementZ < 0) scroll = -1;
            if (st.LeftButton) btn = 1;
        }
        if (dx || dy) {
            UINTN speed = state->pointer_speed;
            if (speed < 1) speed = 1;
            if (speed > 20) speed = 20;
            state->cursor_x += dx * (INTN)speed;
            state->cursor_y += dy * (INTN)speed;
            moved = 1;
        }
    }

    if (state->cursor_x < 0) state->cursor_x = 0;
    if (state->cursor_y < 0) state->cursor_y = 0;
    if (state->cursor_x >= (INTN)state->screen_width)  state->cursor_x = (INTN)state->screen_width - 1;
    if (state->cursor_y >= (INTN)state->screen_height) state->cursor_y = (INTN)state->screen_height - 1;

    if (state->version_mode || state->snap_mode) {
        boot_entry_t *se = entry_at(state, state->selected);
        if (state->snap_mode) {
            if (se && se->snap_count > 0) {
                if (scroll > 0 && se->snap_sel + 1 < se->snap_count) { se->snap_sel++; *menu_redraw = 1; }
                else if (scroll < 0 && se->snap_sel > 0) { se->snap_sel--; *menu_redraw = 1; }
            }
        } else if (se && se->deploy_count > 1) {
            if (scroll > 0 && se->deploy_sel + 1 < se->deploy_count) { se->deploy_sel++; apply_deploy(se); *menu_redraw = 1; }
            else if (scroll < 0 && se->deploy_sel > 0) { se->deploy_sel--; apply_deploy(se); *menu_redraw = 1; }
        }
        if (moved) state->cursor_active = 1;
        prev_btn = btn;
        return moved ? 2 : 0;
    }

    if (state->browse) {
        if (scroll > 0) fb_move(state->browse, 1);
        else if (scroll < 0) fb_move(state->browse, -1);
        if (scroll && browse_band_set(state)) *menu_redraw = 1;
        if (moved) state->cursor_active = 1;
        prev_btn = btn;
        return moved ? 2 : 0;
    }

    if (scroll > 0 && state->selected + 1 < state->entry_count) {
        state->selected++; state->focus = FOCUS_ENTRIES; *menu_redraw = 1;
    } else if (scroll < 0 && state->selected > 0) {
        state->selected--; state->focus = FOCUS_ENTRIES; *menu_redraw = 1;
    }

    if (moved) {
        state->cursor_active = 1;
        if (state->timeout_active) { state->timeout_active = 0; *menu_redraw = 1; }
        int hovered = 0;
        for (int i = 0; i < state->hit_n; i++) {
            if (point_in(state->cursor_x, state->cursor_y,
                         state->hit_x[i], state->hit_y[i], state->hit_w[i], state->hit_h[i])) {
                if (state->hit_idx[i] != state->selected || state->focus != FOCUS_ENTRIES) {
                    state->selected = state->hit_idx[i];
                    state->focus = FOCUS_ENTRIES;
                    *menu_redraw = 1;
                }
                hovered = 1;
                break;
            }
        }
        if (!hovered)
            for (int i = 0; i < 3; i++) {
                if (state->pwr_w[i] <= 0) continue;
                if (point_in(state->cursor_x, state->cursor_y,
                             state->pwr_x[i], state->pwr_y[i], state->pwr_w[i], state->pwr_h[i])) {
                    if (state->focus != FOCUS_POWER || state->power_sel != (UINTN)i) {
                        state->focus = FOCUS_POWER;
                        state->power_sel = (UINTN)i;
                        *menu_redraw = 1;
                    }
                    break;
                }
            }
    }

    int clicked = (btn && !prev_btn);
    prev_btn = btn;
    if (clicked) {
        for (int i = 0; i < state->hit_n; i++) {
            if (point_in(state->cursor_x, state->cursor_y,
                         state->hit_x[i], state->hit_y[i], state->hit_w[i], state->hit_h[i])) {
                state->selected = state->hit_idx[i];
                state->focus = FOCUS_ENTRIES;
                state->action = VISOR_ACTION_BOOT;
                return 1;
            }
        }
        for (int i = 0; i < 3; i++) {
            if (state->pwr_w[i] <= 0) continue;
            if (point_in(state->cursor_x, state->cursor_y,
                         state->pwr_x[i], state->pwr_y[i], state->pwr_w[i], state->pwr_h[i])) {
                state->action = VISOR_ACTION_SHUTDOWN + i;
                return 1;
            }
        }
    }
    return moved ? 2 : 0;
}
