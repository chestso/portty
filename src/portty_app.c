#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_app.h"
#include "common.h"
#include "diag.h"
#include "pager.h"
#include "portty_backend.h"
#include "rend_common.h"
#include "term.h"
#include "term_cfr.h"
#include <coffer/coffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Content scale calculation (unified for all backends)
float portty_compute_content_scale(float system_scale, float user_scale)
{
    float scale = system_scale;
    if (user_scale != 1.0f && scale > 0.0f)
        scale *= user_scale;
    return scale > 0.0f ? scale : 1.0f;
}

// Terminal callback implementations (registered with coffer).

void portty_app_term_output_to_pty(const char *data, size_t len, void *user_data)
{
    PorttyApp *app = (PorttyApp *)user_data;
    if (app && app->pty)
        pty_write(app->pty, data, len);
}

void portty_app_selection_change(bool active, void *user_data)
{
    PorttyApp *app = (PorttyApp *)user_data;
    if (!app || !app->backend)
        return;
    if (active)
        app->backend->pause_pty(app->backend);
    else
        app->backend->resume_pty(app->backend);
}

void portty_app_clipboard_set(const char *text, size_t len, void *user_data)
{
    PorttyApp *app = (PorttyApp *)user_data;
    if (!app || !app->backend)
        return;
    char *buf = malloc(len + 1);
    if (!buf)
        return;
    if (len)
        memcpy(buf, text, len);
    buf[len] = '\0';
    app->backend->clipboard_set(app->backend, buf);
    free(buf);
}

void portty_app_cwd_change(const char *dir, void *user_data)
{
    PorttyApp *app = (PorttyApp *)user_data;
    if (app && app->backend)
        app->backend->set_working_dir(app->backend, dir);
}

// Internal helpers.

static bool app_get_cell_size(PorttyApp *app, int *cw, int *ch)
{
    if (app && app->backend)
        return app->backend->get_cell_size(app->backend, cw, ch);
    return false;
}

static int app_get_scroll_offset(PorttyApp *app)
{
    if (app && app->backend)
        return app->backend->get_scroll_offset(app->backend);
    return 0;
}

static void app_set_autoscroll(PorttyApp *app, bool enabled)
{
    if (app && app->backend)
        app->backend->set_autoscroll(app->backend, enabled);
}

// Enable / disable the backend drag-autoscroll tick, mirroring local state.
void portty_app_set_autoscroll(PorttyApp *app, bool enabled)
{
    if (!app || app->autoscroll_active == enabled)
        return;
    app->autoscroll_active = enabled;
    if (!enabled)
        app->autoscroll_direction = 0;
    app_set_autoscroll(app, enabled);
}

bool portty_app_pixel_to_cell(PorttyApp *app, int pixel_x, int pixel_y,
                              int *out_row, int *out_col)
{
    int cell_w, cell_h;
    if (!app_get_cell_size(app, &cell_w, &cell_h) || cell_w <= 0 || cell_h <= 0)
        return false;
    *out_col = pixel_x / cell_w;
    *out_row = pixel_y / cell_h;
    return true;
}

int portty_app_display_row_to_unified(PorttyApp *app, int display_row)
{
    return rend_display_row_to_unified(app_get_scroll_offset(app), display_row);
}

// Re-extend the active selection to a raw pixel position (clamped into the
// viewport). Shared by the motion handler and the autoscroll tick.
static void extend_selection_from_pixel(PorttyApp *app, int pixel_x, int pixel_y)
{
    int term_rows, term_cols;
    terminal_get_dimensions(app->term, &term_rows, &term_cols);
    int cell_w, cell_h;
    if (!app_get_cell_size(app, &cell_w, &cell_h) || cell_w <= 0 || cell_h <= 0)
        return;
    int viewport_w = term_cols * cell_w;
    int viewport_h = term_rows * cell_h;
    rend_clamp_pixel_to_viewport(&pixel_x, &pixel_y, viewport_w, viewport_h);

    int display_row, display_col;
    if (!portty_app_pixel_to_cell(app, pixel_x, pixel_y, &display_row, &display_col))
        return;
    if (display_row >= term_rows)
        display_row = term_rows - 1;
    if (display_row < 0)
        display_row = 0;
    if (display_col < 0)
        display_col = 0;
    int unified_row = portty_app_display_row_to_unified(app, display_row);
    display_col = terminal_vis_col_to_vt_col(app->term, unified_row, display_col);
    if (display_col >= term_cols)
        display_col = term_cols - 1;
    if (display_col < 0)
        display_col = 0;
    terminal_selection_update(app->term, unified_row, display_col);
}

// Drag-autoscroll tick — invoked by the backend timer while the user is
// holding a selection drag past the top/bottom edge of the viewport.
void portty_app_handle_autoscroll_tick(PorttyApp *app)
{
    if (!terminal_selection_active(app->term)) {
        portty_app_set_autoscroll(app, false);
        return;
    }

    int dir = app->autoscroll_direction;
    if (dir == 0) {
        portty_app_set_autoscroll(app, false);
        return;
    }

    int term_rows, term_cols;
    terminal_get_dimensions(app->term, &term_rows, &term_cols);
    int cell_w, cell_h;
    if (!app_get_cell_size(app, &cell_w, &cell_h) || cell_w <= 0 || cell_h <= 0)
        return;
    (void)term_cols;
    int viewport_h = term_rows * cell_h;
    int py = app->drag_last_pixel_y;

    int delta;
    if (dir > 0) {
        // Scroll down (pointer above the top edge)
        int overshoot = (py < 0) ? -py : 0;
        delta = 1 + overshoot / cell_h;
        if (delta > 5)
            delta = 5;
    } else {
        // Scroll up (pointer below the bottom edge)
        int overshoot = (py >= viewport_h) ? (py - (viewport_h - 1)) : 0;
        delta = -(1 + overshoot / cell_h);
        if (delta < -5)
            delta = -5;
    }

    app->backend->scroll(app->backend, app->term, delta);
    extend_selection_from_pixel(app, app->drag_last_pixel_x, app->drag_last_pixel_y);
}

// Mouse left the window during a drag — start autoscroll if a drag is
// in progress and the pointer exited through the top or bottom edge. On
// Wayland, SDL_CaptureMouse is a no-op, so this is the only signal we get
// that the pointer has exited while the user is dragging.
void portty_app_handle_mouse_leave(PorttyApp *app, int pixel_x, int pixel_y)
{
    app->pointer_outside = true;

    if (!app->drag_in_progress)
        return;

    app->drag_last_pixel_x = pixel_x;
    app->drag_last_pixel_y = pixel_y;

    int term_rows, term_cols;
    terminal_get_dimensions(app->term, &term_rows, &term_cols);
    int cell_w, cell_h;
    if (!app_get_cell_size(app, &cell_w, &cell_h) || cell_h <= 0)
        return;
    (void)term_cols;
    int viewport_h = term_rows * cell_h;

    if (pixel_y < cell_h) {
        app->autoscroll_direction = 1;
        portty_app_set_autoscroll(app, true);
    } else if (pixel_y >= viewport_h - cell_h) {
        app->autoscroll_direction = -1;
        portty_app_set_autoscroll(app, true);
    }
}

// Mouse re-entered the window — stop autoscroll.
void portty_app_handle_mouse_enter(PorttyApp *app)
{
    app->pointer_outside = false;
    portty_app_set_autoscroll(app, false);
}

// Resolve the OSC-8 hyperlink id at a pixel position. Returns 0 if no link
// or out-of-bounds. Also returns the resolved (unified_row, vt_col) for
// callers that want to fetch the URI without redoing the math.
static uint16_t hyperlink_id_at(PorttyApp *app, int pixel_x, int pixel_y,
                                int *out_unified_row, int *out_vt_col)
{
    int display_row, display_col;
    if (!portty_app_pixel_to_cell(app, pixel_x, pixel_y, &display_row, &display_col))
        return 0;
    int term_rows, term_cols;
    terminal_get_dimensions(app->term, &term_rows, &term_cols);
    if (display_row < 0 || display_row >= term_rows ||
        display_col < 0 || display_col >= term_cols)
        return 0;
    int unified_row = portty_app_display_row_to_unified(app, display_row);
    int vt_col = terminal_vis_col_to_vt_col(app->term, unified_row, display_col);
    if (vt_col < 0)
        vt_col = 0;
    if (vt_col >= term_cols)
        vt_col = term_cols - 1;

    TerminalCell cell;
    int rc = (unified_row >= 0)
                 ? terminal_get_cell(app->term, unified_row, vt_col, &cell)
                 : terminal_get_scrollback_cell(app->term, -(unified_row + 1),
                                                vt_col, &cell);
    if (rc < 0)
        return 0;
    if (out_unified_row)
        *out_unified_row = unified_row;
    if (out_vt_col)
        *out_vt_col = vt_col;
    return cell.hyperlink_id;
}

// Resolve the OSC-8 hyperlink under a pixel position and update hover state +
// cursor shape. px<0 (or out-of-bounds) resolves to "no link". Returns true if
// the hovered id changed (caller should trigger a redraw).
static bool resolve_link_hover(PorttyApp *app, int px, int py)
{
    int link_row = 0, link_col = 0;
    uint16_t hid = (px < 0) ? 0 : hyperlink_id_at(app, px, py, &link_row, &link_col);
    if (hid == terminal_hovered_hyperlink(app->term))
        return false;
    terminal_set_hovered_hyperlink(app->term, hid);
    app->backend->set_cursor(app->backend,
                             hid != 0 ? PORTTY_CURSOR_POINTER : PORTTY_CURSOR_TEXT);
    if (hid != 0) {
        char url[4096];
        size_t n = terminal_cell_get_hyperlink(app->term, link_row, link_col, url,
                                               sizeof(url));
        int cell_w, cell_h;
        int anchor_py = py;
        if (app_get_cell_size(app, &cell_w, &cell_h) && cell_h > 0) {
            int display_row = py / cell_h;
            if (display_row < 0)
                display_row = 0;
            anchor_py = display_row * cell_h;
        }
        // TODO: panel_show - link hint panels coming soon
        (void)url;
        (void)anchor_py;
    } else {
        // TODO: panel_hide - link hint panels coming soon
    }
    return true;
}

bool portty_app_revalidate_hover(PorttyApp *app, int px, int py)
{
    if (pager_active(app->pager))
        return false;
    if (terminal_get_mouse_mode(app->term) != 0)
        return false;
    return resolve_link_hover(app, px, py);
}

KeyboardResult portty_app_handle_key(PorttyApp *app, int term_key,
                                     int mod, uint32_t codepoint)
{
    KeyboardResult result = { 0 };

    if (pager_active(app->pager)) {
        result.handled = pager_key(app->pager, term_key, mod, codepoint);
        return result;
    }

    // Ctrl+C with active selection → copy and cancel (not SIGINT)
    if (codepoint == 'c' && (mod & TERM_MOD_CTRL) && !(mod & TERM_MOD_SHIFT) &&
        terminal_selection_active(app->term)) {
        char *text = terminal_selection_get_text(app->term);
        if (text) {
            app->backend->clipboard_set(app->backend, text);
            free(text);
        }
        terminal_selection_clear(app->term);
        result.force_redraw = true;
        result.handled = true;
        return result;
    }

    // Ctrl+Shift+C → copy
    if (term_key == TERM_KEY_NONE && codepoint == 'C' && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        if (terminal_selection_active(app->term)) {
            char *text = terminal_selection_get_text(app->term);
            if (text) {
                app->backend->clipboard_set(app->backend, text);
                free(text);
            }
            terminal_selection_clear(app->term);
            result.force_redraw = true;
        }
        result.handled = true;
        return result;
    }
    // Any key with active selection → cancel selection
    if (terminal_selection_active(app->term)) {
        terminal_selection_clear(app->term);
        result.force_redraw = true;
    }

    // Ctrl+Shift+V → paste
    if (term_key == TERM_KEY_NONE && codepoint == 'V' && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        // Paste synchronously
        if (!app->backend->clipboard_paste_async(app->backend, app->term, app->pty)) {
            char *clipboard = app->backend->clipboard_get(app->backend);
            if (clipboard && clipboard[0] != '\0') {
                size_t plen = 0;
                char *paste = terminal_paste_normalize(clipboard, strlen(clipboard), &plen);
                if (paste) {
                    terminal_start_paste(app->term);
                    pty_write(app->pty, paste, plen);
                    terminal_end_paste(app->term);
                    free(paste);
                }
            }
            app->backend->clipboard_free(app->backend, clipboard);
        }
        result.handled = true;
        return result;
    }

    // Shift+PageUp/Down: scrollback navigation (normal screen only)
    if ((mod & TERM_MOD_SHIFT) && !terminal_is_altscreen(app->term)) {
        if (term_key == TERM_KEY_PAGEUP || term_key == TERM_KEY_PAGEDOWN) {
            int rows, cols;
            terminal_get_dimensions(app->term, &rows, &cols);
            app->backend->scroll(app->backend, app->term,
                                 term_key == TERM_KEY_PAGEUP ? rows : -rows);
            result.force_redraw = true;
            result.handled = true;
            return result;
        }
    }

    // Ctrl+Shift+F6 → diagnostics report in the internal pager
    if (term_key == TERM_KEY_F6 && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        portty_app_show_diagnostics(app);
        result.handled = true;
        return result;
    }

    // Ctrl+Shift+N → spawn a new terminal window in the shell's CWD
    if (term_key == TERM_KEY_NONE && codepoint == 'N' && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        app->backend->spawn_new_terminal(app->backend);
        result.handled = true;
        return result;
    }

    // Special keys
    if (term_key != TERM_KEY_NONE) {
        terminal_send_key(app->term, term_key, mod);
        result.handled = true;
        return result;
    }

    // Ctrl/Alt + printable
    if (codepoint && (mod & (TERM_MOD_CTRL | TERM_MOD_ALT))) {
        terminal_send_char(app->term, codepoint, mod);
        result.handled = true;
    }

    return result;
}

KeyboardResult portty_app_handle_text(PorttyApp *app, const char *text)
{
    KeyboardResult result = { 0 };

    if (pager_active(app->pager)) {
        for (const char *t = text; *t; t++)
            pager_key(app->pager, TERM_KEY_NONE, TERM_MOD_NONE, (unsigned char)*t);
        result.handled = true;
        return result;
    }

    if (terminal_selection_active(app->term)) {
        terminal_selection_clear(app->term);
        result.force_redraw = true;
    }

    size_t text_len = strlen(text);
    if (text_len > 0 && text_len < sizeof(result.data)) {
        memcpy(result.data, text, text_len);
        result.len = text_len;
    }

    return result;
}

void portty_app_handle_resize(PorttyApp *app, int pixel_w, int pixel_h)
{
    terminal_selection_clear(app->term);
    app->backend->resize(app->backend, pixel_w, pixel_h);

    int cell_w, cell_h;
    if (app_get_cell_size(app, &cell_w, &cell_h)) {
        terminal_set_cell_px(app->term, cell_w, cell_h);
        float content_scale = portty_compute_content_scale(
            app->backend->get_display_scale(app->backend), app->dpi_scale);
        terminal_set_content_scale(app->term, content_scale);
        int cols = pixel_w / cell_w;
        int rows = pixel_h / cell_h;
        if (cols > 0 && rows > 0) {
            terminal_resize(app->term, cols, rows);
            pty_resize(app->pty, rows, cols);
            if (pager_active(app->pager))
                pager_resize(app->pager, cols, rows);
        }
    }
}

void portty_app_handle_scroll(PorttyApp *app, int delta)
{
    if (delta == 0)
        return;

    if (pager_active(app->pager)) {
        pager_scroll(app->pager, delta * SCROLL_LINES_PER_TICK);
        return;
    }

    if (terminal_is_altscreen(app->term) && terminal_get_mouse_mode(app->term) == 0) {
        int key = (delta > 0) ? TERM_KEY_UP : TERM_KEY_DOWN;
        int count = abs(delta) * SCROLL_LINES_PER_TICK;
        if (count < 1)
            count = 1;
        for (int i = 0; i < count; i++)
            terminal_send_key(app->term, key, TERM_MOD_NONE);
        return;
    }

    app->backend->scroll(app->backend, app->term, delta * SCROLL_LINES_PER_TICK);
}

bool portty_app_handle_mouse(PorttyApp *app, int pixel_x, int pixel_y,
                             int button, bool pressed, int clicks, int mod)
{
    if (pager_active(app->pager))
        return pager_mouse(app->pager, pixel_x, pixel_y, button, pressed, clicks, mod);

    // Check for panel hit first
    if (button == 1 && pressed) {
        bool close_btn = false;
        int panel_id = app->backend->panel_hit_test(app->backend, pixel_x, pixel_y, &close_btn);
        if (panel_id > 0) {
            if (close_btn) {
                app->backend->panel_hide(app->backend, panel_id);
                return true;
            }
            // Clicked on panel but not close button - consume event
            return true;
        }
    }

    int mouse_mode = terminal_get_mouse_mode(app->term);
    bool shift_held = (mod & TERM_MOD_SHIFT) != 0;

    bool hover_changed = false;
    if (mouse_mode == 0 || shift_held) {
        hover_changed = resolve_link_hover(app, pixel_x, pixel_y);

        if (button == 1 && pressed && (mod & TERM_MOD_CTRL)) {
            int link_row, link_col;
            uint16_t hid = hyperlink_id_at(app, pixel_x, pixel_y, &link_row, &link_col);
            if (hid != 0) {
                char url[4096];
                size_t n = terminal_cell_get_hyperlink(app->term, link_row,
                                                       link_col, url, sizeof(url));
                if (n > 0 && terminal_hyperlink_is_safe(url)) {
                    vlog("Opening OSC-8 URL: %s\n", url);
                    char err[256];
                    if (!app->backend->open_url(app->backend, url, err, sizeof(err))) {
                        char title[256];
                        char body[320];
                        snprintf(title, sizeof(title), "Couldn't open %.200s", url);
                        snprintf(body, sizeof(body), "Error is: %.256s", err);
                        fprintf(stderr, "ERROR: %s — %s\n", title, body);
                        // TODO: panel_show - notification panels coming soon
                        (void)title;
                        (void)body;
                    }
                } else if (n > 0) {
                    char title[256];
                    snprintf(title, sizeof(title), "Refusing to open %.200s", url);
                    fprintf(stderr, "WARNING: %s (disallowed scheme)\n", title);
                    // TODO: panel_show - notification panels coming soon
                    (void)title;
                }
                return true;
            }
        }
    } else {
        // TODO: panel_hide - link hint panels coming soon
    }

    bool in_altscreen = terminal_is_altscreen(app->term);

    if (mouse_mode > 0 && !shift_held) {
        bool should_forward = false;

        if (button == 4 || button == 5) {
            should_forward = in_altscreen || (mouse_mode > 0);
        } else if (button > 0) {
            should_forward = true;
        } else {
            should_forward = (mouse_mode >= 2 && pressed) || (mouse_mode >= 3);
        }

        if (should_forward) {
            int cell_w, cell_h;
            if (!app_get_cell_size(app, &cell_w, &cell_h) || cell_w <= 0 || cell_h <= 0)
                return false;
            int col = pixel_x / cell_w;
            int row = pixel_y / cell_h;

            int term_rows, term_cols;
            terminal_get_dimensions(app->term, &term_rows, &term_cols);
            if (col >= term_cols)
                col = term_cols - 1;
            if (row >= term_rows)
                row = term_rows - 1;
            if (col < 0)
                col = 0;
            if (row < 0)
                row = 0;

            terminal_send_mouse_event(app->term, row, col, button, pressed, mod);
            return true;
        }

        if (button == 4 || button == 5)
            return false;
    }

    if (button == 4 || button == 5)
        return false;

    if (in_altscreen && !shift_held && mouse_mode == 0 && button != 3)
        return hover_changed;

    int display_row, display_col;
    if (!portty_app_pixel_to_cell(app, pixel_x, pixel_y, &display_row, &display_col))
        return false;

    int term_rows, term_cols;
    terminal_get_dimensions(app->term, &term_rows, &term_cols);
    if (display_row >= term_rows)
        display_row = term_rows - 1;
    if (display_row < 0)
        display_row = 0;
    if (display_col < 0)
        display_col = 0;

    int unified_row = portty_app_display_row_to_unified(app, display_row);
    display_col = terminal_vis_col_to_vt_col(app->term, unified_row, display_col);
    if (display_col >= term_cols)
        display_col = term_cols - 1;
    if (display_col < 0)
        display_col = 0;

    if (button == 1 && pressed) {
        app->drag_in_progress = true;
        if (clicks >= 3) {
            app->drag_pending = false;
            terminal_selection_start(app->term, unified_row, display_col, TERM_SELECT_LINE);
        } else if (clicks == 2) {
            app->drag_pending = false;
            terminal_selection_start(app->term, unified_row, display_col, TERM_SELECT_WORD);
        } else if (shift_held && terminal_selection_active(app->term)) {
            app->drag_pending = false;
            terminal_selection_extend(app->term, unified_row, display_col);
            terminal_mark_dirty(app->term);
        } else if (terminal_selection_active(app->term)) {
            app->drag_pending = false;
            terminal_selection_clear(app->term);
        } else {
            app->drag_pending = true;
            app->drag_start_row = unified_row;
            app->drag_start_col = display_col;
        }
        return true;
    }

    if (button == 1 && !pressed) {
        app->drag_in_progress = false;
        app->drag_pending = false;
        app->pointer_outside = false;
        portty_app_set_autoscroll(app, false);
        return false;
    }

    if (button == 0 && pressed) {
        if (app->drag_pending) {
            app->drag_pending = false;
            terminal_selection_start(app->term, app->drag_start_row, app->drag_start_col,
                                     TERM_SELECT_CHAR);
            terminal_selection_update(app->term, unified_row, display_col);
        } else if (terminal_selection_active(app->term)) {
            terminal_selection_update(app->term, unified_row, display_col);
        } else {
            return hover_changed;
        }

        app->drag_last_pixel_x = pixel_x;
        app->drag_last_pixel_y = pixel_y;
        int cell_w, cell_h;
        if (app_get_cell_size(app, &cell_w, &cell_h) && cell_h > 0) {
            int viewport_h = term_rows * cell_h;
            if (pixel_y < 0) {
                app->autoscroll_direction = 1;
                portty_app_set_autoscroll(app, true);
            } else if (pixel_y >= viewport_h) {
                app->autoscroll_direction = -1;
                portty_app_set_autoscroll(app, true);
            } else {
                portty_app_set_autoscroll(app, false);
            }
        }
        return true;
    }

    if (button != 0 && pressed)
        portty_app_set_autoscroll(app, false);

    if (button == 3 && pressed) {
        if (terminal_selection_active(app->term)) {
            char *text = terminal_selection_get_text(app->term);
            if (text) {
                app->backend->clipboard_set(app->backend, text);
                free(text);
            }
            terminal_selection_clear(app->term);
        } else {
            if (!app->backend->clipboard_paste_async(app->backend, app->term, app->pty)) {
                char *clipboard = app->backend->clipboard_get(app->backend);
                if (clipboard && clipboard[0] != '\0') {
                    terminal_start_paste(app->term);
                    pty_write(app->pty, clipboard, strlen(clipboard));
                    terminal_end_paste(app->term);
                }
                app->backend->clipboard_free(app->backend, clipboard);
            }
        }
        return true;
    }

    return hover_changed;
}

void portty_app_process_pty_data(PorttyApp *app, const char *data, size_t len)
{
    terminal_process_input(app->term, data, len);
    terminal_flush_damage(app->term);
}

void portty_app_feed_terminal(void *app_ptr, const char *data, size_t len)
{
    PorttyApp *app = (PorttyApp *)app_ptr;
    if (!app || !app->term || !data || len == 0)
        return;
    terminal_process_input(app->term, data, len);
    terminal_flush_damage(app->term);
}

void portty_app_handle_pty_closed(PorttyApp *app)
{
    (void)app;
}

void portty_app_handle_child_exit(PorttyApp *app, int status)
{
    (void)app;
    (void)status;
}

void portty_app_show_diagnostics(PorttyApp *app)
{
    const PorttyConf *c = app->conf;
    PorttyDiag pd = { 0 };
    app->backend->get_diag(app->backend, &pd);

    int rows = 0, cols = 0;
    terminal_get_dimensions(app->term, &rows, &cols);

    DiagSources src = {
        .renderer_name = pd.backend_name,
        .platform_name = pd.platform_name,
        .gpu_device = pd.gpu_device,
        .gpu_driver = pd.gpu_driver,
        .gpu_driver_libre = pd.gpu_driver_libre,
        .linear_light = pd.linear_light,
        .glyph_shader = pd.glyph_shader,
        .content_scale = pd.content_scale,
        .pixel_width = pd.pixel_width,
        .pixel_height = pd.pixel_height,
        .cell_width = pd.cell_width,
        .cell_height = pd.cell_height,
        .cols = cols,
        .rows = rows,
        .display_session = pd.display_session,
        .display_xwayland = pd.display_xwayland,
        .display_screen = pd.display_screen,
        .display_dpi = pd.display_dpi,
        .display_scale = pd.display_scale,
        .display_physical = pd.display_physical,
        .config_path = c ? c->source_path : NULL,
        .font_pattern = c ? c->font : NULL,
        .font_path = pd.font_path,
        .font_source = app->font_source,
        .hinting = pd.hinting,
        .scrollback = terminal_get_scrollback_capacity(app->term),
        .text_gamma = portty_text_gamma,
        .text_contrast = portty_text_contrast,
        .word_chars = c ? c->word_chars : NULL,
        .term_env = "portty-vty-256color",
        .colorterm_env = "truecolor",
        .lang_env = getenv("LANG"),
        .title = terminal_get_title(app->term),
        .altscreen = terminal_is_altscreen(app->term),
        .mouse_mode = terminal_get_mouse_mode(app->term),
        .vt_backend = app->term->name,
        .lottie_rasterizer = cfr_have_lottie(),
        .osc52 = app->term->clipboard_set_cb != NULL,
        .bracketed_paste = terminal_get_mode(app->term, CFR_MODE_BRACKETED_PASTE),
        .sync_output = terminal_get_mode(app->term, CFR_MODE_SYNC_OUTPUT),
        .focus_reporting = terminal_get_mode(app->term, CFR_MODE_FOCUS_REPORTING),
        .sixel_scrolling = terminal_get_mode(app->term, CFR_MODE_SIXEL_SCROLLING),
#ifdef PORTTY_HARDEN_HEAP
        .hardened_heap = true,
#else
        .hardened_heap = false,
#endif
    };

    char *report = diag_build_report(&src);
    if (!report)
        return;

    if (cols > 0 && rows > 0 && app->pager)
        pager_open(app->pager, report, cols, rows);
    free(report);
}