/*
 * portty — Application-level glue: shortcuts, notifications, panel wiring
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_app.h"
#include "common.h"
#include "diag.h"
#include "dump.h"
#include "pager.h"
#include "portty_backend.h"
#include "portty_panel.h"
#include "portty_pty.h"
#include "rend_common.h"
#include "term.h"
#include <coffer/coffer.h>
#include <math.h>
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
        if (n > 0 && n < sizeof(url)) {
            int cell_w, cell_h;
            if (app_get_cell_size(app, &cell_w, &cell_h) && cell_w > 0 && cell_h > 0) {
                int term_rows, term_cols;
                terminal_get_dimensions(app->term, &term_rows, &term_cols);

                int display_row = py / cell_h;
                PanelRect hint = panel_link_hint_rect(term_cols, term_rows, display_row);

                app->backend->panel_show(app->backend, PANEL_ID_LINK_HINT,
                                         hint.col, hint.row, hint.cols, hint.rows,
                                         NULL, url,
                                         PORTTY_NOTIFY_INFO,
                                         PANEL_FLAG_NO_ACCENT | PANEL_FLAG_NO_CLOSE);
            }
        }
    } else {
        app->backend->panel_hide(app->backend, PANEL_ID_LINK_HINT);
    }
    return true;
}

// Resolve which panel's close button (if any) is under a pixel position and
// update close-button hover highlighting. Returns true if the hover state
// changed (caller should trigger a redraw).
static bool resolve_panel_hover(PorttyApp *app, int px, int py)
{
    if (!app->backend || !app->backend->panel_hit_test || !app->backend->panel_set_hover)
        return false;
    bool close_btn = false;
    int id = (px < 0) ? 0 : app->backend->panel_hit_test(app->backend, px, py, &close_btn);
    if (!close_btn)
        id = 0;
    if (id == app->hovered_panel_id)
        return false;
    if (app->hovered_panel_id != 0)
        app->backend->panel_set_hover(app->backend, app->hovered_panel_id, false);
    if (id != 0)
        app->backend->panel_set_hover(app->backend, id, true);
    app->hovered_panel_id = id;
    return true;
}

bool portty_app_revalidate_hover(PorttyApp *app, int px, int py)
{
    bool changed = resolve_panel_hover(app, px, py);
    if (pager_active(app->pager))
        return changed;
    if (terminal_get_mouse_mode(app->term) != 0)
        return changed;
    if (resolve_link_hover(app, px, py))
        changed = true;
    return changed;
}

void portty_app_clear_hover(PorttyApp *app)
{
    if (!app || !app->term || !app->backend)
        return;
    bool was_hovering = terminal_hovered_hyperlink(app->term) != 0;
    terminal_set_hovered_hyperlink(app->term, 0);
    app->backend->panel_hide(app->backend, PANEL_ID_LINK_HINT);
    if (was_hovering)
        app->backend->set_cursor(app->backend, PORTTY_CURSOR_TEXT);
    if (app->hovered_panel_id != 0) {
        if (app->backend->panel_set_hover)
            app->backend->panel_set_hover(app->backend, app->hovered_panel_id, false);
        app->hovered_panel_id = 0;
    }
}

// Lifetime of the transient status messages shown by the app.
#define DUMP_NOTIFY_TTL_MS       4000
#define DUMP_ERROR_NOTIFY_TTL_MS 8000
#define LINK_ERROR_NOTIFY_TTL_MS 6000
#define RESET_NOTIFY_TTL_MS      5000
#define BELL_NOTIFY_TTL_MS       2000

// Show a transient status message, if the backend supports it. The text is
// elided to the grid width so the message stays on one line per row: the
// body keeps its tail (the file name, the end of the URL), which is the
// informative part.
static void app_notify(PorttyApp *app, const char *title, const char *body,
                       PorttyNotifyLevel level, unsigned int ttl_ms)
{
    if (!app || !app->backend || !app->backend->notify)
        return;

    int rows = 0, cols = 0;
    if (app->term)
        terminal_get_dimensions(app->term, &rows, &cols);
    (void)rows; // the grid height does not affect the message width

    size_t budget = 8;
    if (cols > PANEL_DECORATION_COLS + 8)
        budget = (size_t)(cols - PANEL_DECORATION_COLS);

    char t[256];
    char b[1024];
    panel_elide_front(title, budget, t, sizeof(t));
    panel_elide_front(body, budget, b, sizeof(b));

    app->backend->notify(app->backend, t, b, level, ttl_ms);
}

// Report the outcome of a terminal-state dump: the destination path on
// success, the failure reason otherwise.
static void app_notify_dump_result(PorttyApp *app, const PorttyDumpResult *res)
{
    if (res->ok)
        app_notify(app, "State dump written", res->path, PORTTY_NOTIFY_INFO,
                   DUMP_NOTIFY_TTL_MS);
    else
        app_notify(app, "State dump failed", res->error, PORTTY_NOTIFY_ERROR,
                   DUMP_ERROR_NOTIFY_TTL_MS);
}

/* BEL from the child. The panel is the whole visual bell: there is no audible
 * bell, so a beep from a long-running command becomes a two-second status
 * message. The window title is the detail line — the only context portty has
 * about who rang.
 *
 * `bell` in the config decides when it shows: `always` (default), `unfocused`
 * (only when the window lacks focus, for people who find a flash while typing
 * distracting), or `none`. There is one panel slot, so a bell that arrives
 * while a message is already up is dropped rather than restacking it — a
 * program ringing in a loop cannot pin the panel there. */
void portty_app_bell(void *user_data)
{
    PorttyApp *app = (PorttyApp *)user_data;
    if (!app || !app->backend)
        return;

    PorttyBellMode mode = app->conf ? app->conf->bell : PORTTY_BELL_ALWAYS;
    if (mode == PORTTY_BELL_NONE)
        return;
    if (mode == PORTTY_BELL_UNFOCUSED && app->backend->has_focus &&
        app->backend->has_focus(app->backend))
        return;
    if (app->backend->notify_active && app->backend->notify_active(app->backend))
        return;

    const char *title = app->term ? terminal_get_title(app->term) : NULL;
    app_notify(app, "Bell", (title && title[0]) ? title : NULL,
               PORTTY_NOTIFY_INFO, BELL_NOTIFY_TTL_MS);
}

/* ------------------------------------------------------------------ */
/* Terminal reset                                                      */
/* ------------------------------------------------------------------ */

/* Describe the input-protocol state that is currently active, so the
 * reset notification can name what was stuck. */
static void input_state_summary(TerminalBackend *term, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return;
    buf[0] = '\0';
    if (!term)
        return;

    size_t used = 0;
    uint32_t flags = terminal_get_kitty_kb_flags(term);
    if (flags)
        used += (size_t)snprintf(buf + used, cap - used,
                                 "kitty keyboard flags 0x%x", flags);

    if (terminal_get_mode(term, CFR_MODE_MOUSE_ANY_EVENT) ||
        terminal_get_mode(term, CFR_MODE_MOUSE_BTN_EVENT) ||
        terminal_get_mode(term, CFR_MODE_MOUSE_DRAG) ||
        terminal_get_mode(term, CFR_MODE_MOUSE_X10)) {
        used += (size_t)snprintf(buf + used, cap - used, "%smouse reporting",
                                 used ? ", " : "");
    }

    if (used == 0)
        snprintf(buf, cap, "no input protocol was active");
    else if (used >= cap)
        buf[cap - 1] = '\0';
}

/* Reset the input-protocol state: kitty keyboard flags, modifyOtherKeys,
 * application cursor/keypad modes, mouse and focus reporting. The
 * screen and the scrollback are left alone. This is the recovery for a
 * program that enabled one of those protocols and died without
 * cleaning up — the shell then gets legacy key encodings again. */
static void portty_app_reset_input_state(PorttyApp *app)
{
    if (!app || !app->term)
        return;

    char what[256];
    input_state_summary(app->term, what, sizeof(what));

    terminal_reset_input_state(app->term);
    terminal_selection_clear(app->term);

    vlog("Terminal input state reset: %s\n", what);
    app_notify(app, "Terminal reset", what, PORTTY_NOTIFY_INFO,
               RESET_NOTIFY_TTL_MS);
}

/* Full reset (RIS) — what the shell command `reset` does: the screen,
 * the scrollback and every mode go back to their initial state, and
 * the line discipline is restored so a program that died in raw mode
 * does not leave the shell without echo. */
static void portty_app_full_reset(PorttyApp *app)
{
    if (!app || !app->term)
        return;

    terminal_reset(app->term);
    terminal_selection_clear(app->term);

    bool modes_restored = false;
    if (app->pty)
        modes_restored = pty_reset_termios(app->pty);

    if (app->backend && app->backend->reset_scroll)
        app->backend->reset_scroll(app->backend);

    vlog("Terminal fully reset (termios restored: %s)\n",
         modes_restored ? "yes" : "no");
    app_notify(app, "Terminal fully reset",
               modes_restored ? "screen and scrollback cleared, terminal modes restored"
                              : "screen and scrollback cleared",
               PORTTY_NOTIFY_INFO, RESET_NOTIFY_TTL_MS);
}

/* Pop the kitty keyboard protocol flags when the program that pushed
 * them is gone. A program that enables the protocol and dies without
 * popping it leaves every Ctrl+key routed through CSI-u, and the shell
 * can no longer read a line.
 *
 * The owner is the foreground process group at the time the flags
 * appeared. When the foreground group changes and the owner is gone,
 * the program died and the flags are stale. A suspended job (Ctrl+Z)
 * keeps its process group alive, so its flags are left alone. */
void portty_app_check_kb_protocol_owner(PorttyApp *app)
{
    if (!app || !app->term || !app->pty)
        return;

    uint32_t flags = terminal_get_kitty_kb_flags(app->term);
    if (flags == 0) {
        app->kb_flags_seen = 0;
        app->kb_flags_owner_pgrp = 0;
        return;
    }

    int fg = pty_get_fg_pgrp(app->pty);

    if (flags != app->kb_flags_seen) {
        /* New flags: the program in the foreground owns them. */
        app->kb_flags_seen = flags;
        app->kb_flags_owner_pgrp = fg;
        return;
    }

    if (fg <= 0 || app->kb_flags_owner_pgrp <= 0)
        return;
    if (fg == app->kb_flags_owner_pgrp)
        return;
    if (pty_pgrp_alive(app->kb_flags_owner_pgrp))
        return;

    vlog("Foreground program %d died holding the kitty keyboard flags "
         "(0x%x) — popping them\n",
         app->kb_flags_owner_pgrp, flags);

    terminal_reset_input_state(app->term);
    app->kb_flags_seen = 0;
    app->kb_flags_owner_pgrp = 0;

    app_notify(app, "Keyboard protocol reset",
               "the previous program exited without clearing it",
               PORTTY_NOTIFY_INFO, RESET_NOTIFY_TTL_MS);
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
    // Any key with active selection → resume PTY so the app can
    // respond. The selection itself is NOT cleared — the damage
    // overlap check handles clearing if output overwrites selected
    // content, and scroll adjustment keeps selection coordinates
    // valid as content shifts.
    if (terminal_selection_active(app->term)) {
        app->backend->resume_pty(app->backend);
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

    // Ctrl+Shift+PageUp/Down: scrollback navigation (normal screen only)
    if ((mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT) && !terminal_is_altscreen(app->term)) {
        if (term_key == TERM_KEY_PAGEUP || term_key == TERM_KEY_PAGEDOWN) {
            portty_app_clear_hover(app);
            int rows, cols;
            terminal_get_dimensions(app->term, &rows, &cols);
            app->backend->scroll(app->backend, app->term,
                                 term_key == TERM_KEY_PAGEUP ? rows : -rows);
            result.force_redraw = true;
            result.handled = true;
            return result;
        }
        if (term_key == TERM_KEY_UP || term_key == TERM_KEY_DOWN) {
            portty_app_clear_hover(app);
            app->backend->scroll(app->backend, app->term,
                                 term_key == TERM_KEY_UP ? 1 : -1);
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

    // Ctrl+Shift+F7 → dump the full terminal state to a JSON file
    if (term_key == TERM_KEY_F7 && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        PorttyDumpResult res;
        portty_app_dump_state(app, NULL, &res);
        app_notify_dump_result(app, &res);
        result.handled = true;
        return result;
    }

    // Ctrl+Delete → reset the terminal's input-protocol state.
    // Same chord as kitty's "reset terminal" (kitty_mod+delete): a
    // program that enabled the kitty keyboard protocol or application
    // cursor keys and died leaves the shell unable to read a line.
    if (term_key == TERM_KEY_DEL && (mod & TERM_MOD_CTRL)) {
        portty_app_reset_input_state(app);
        result.force_redraw = true;
        result.handled = true;
        return result;
    }

    // Ctrl+Shift+F8 → full reset (RIS), like the shell command `reset`:
    // the screen, the scrollback and every mode go back to their
    // initial state, and the terminal line discipline is restored.
    if (term_key == TERM_KEY_F8 && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        portty_app_full_reset(app);
        result.force_redraw = true;
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

    if (terminal_selection_active(app->term))
        app->backend->resume_pty(app->backend);

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
            if (app->pty) {
                // The child measures the cell in logical pixels; the
                // winsize report and the engine's scaling then agree.
                int lcw = cfr_px_logical(cell_w, content_scale);
                int lch = cfr_px_logical(cell_h, content_scale);
                pty_resize(app->pty, rows, cols, cols * lcw, rows * lch);
            }
            if (pager_active(app->pager))
                pager_resize(app->pager, cols, rows);
        }
    }
}

bool portty_app_handle_display_scale_changed(PorttyApp *app, int pixel_w, int pixel_h)
{
    if (!app || !app->backend)
        return false;
    if (!app->backend->reload_fonts || !app->backend->get_display_scale)
        return false;

    // The compositor scale moved: the fonts are rasterized for the
    // content scale, so they have to be redone for the new DPI.
    float content_scale = portty_compute_content_scale(
        app->backend->get_display_scale(app->backend), app->dpi_scale);
    if (!app->backend->reload_fonts(app->backend, content_scale))
        return false; // the cell size did not move

    // The cell size moved, so the grid is derived from it again.
    portty_app_handle_resize(app, pixel_w, pixel_h);
    return true;
}

void portty_app_handle_scroll(PorttyApp *app, int delta)
{
    if (delta == 0)
        return;

    portty_app_clear_hover(app);

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
                app->hovered_panel_id = 0;
                return true;
            }
            // Clicked on panel but not close button - consume event
            return true;
        }
    }

    int mouse_mode = terminal_get_mouse_mode(app->term);
    bool shift_held = (mod & TERM_MOD_SHIFT) != 0;

    bool hover_changed = resolve_panel_hover(app, pixel_x, pixel_y);
    if (mouse_mode == 0 || shift_held) {
        if (resolve_link_hover(app, pixel_x, pixel_y))
            hover_changed = true;

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
                        app_notify(app, "Couldn't open link", body,
                                   PORTTY_NOTIFY_ERROR, LINK_ERROR_NOTIFY_TTL_MS);
                    }
                } else if (n > 0) {
                    char title[256];
                    snprintf(title, sizeof(title), "Refusing to open %.200s", url);
                    fprintf(stderr, "WARNING: %s (disallowed scheme)\n", title);
                    app_notify(app, "Refusing to open link",
                               "Disallowed URL scheme", PORTTY_NOTIFY_WARNING,
                               LINK_ERROR_NOTIFY_TTL_MS);
                }
                return true;
            }
        }
    } else {
        app->backend->panel_hide(app->backend, PANEL_ID_LINK_HINT);
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

void portty_app_feed_terminal(void *app_ptr, const char *data, size_t len)
{
    PorttyApp *app = (PorttyApp *)app_ptr;
    if (!app || !app->term || !data || len == 0)
        return;
    terminal_consume_pushed_rows(app->term);
    terminal_process_input(app->term, data, len);
    int pushed = terminal_consume_pushed_rows(app->term);
    terminal_flush_damage(app->term);
    if (pushed > 0)
        portty_app_clear_hover(app);
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
        .graphics_api = pd.graphics_api,
        .gpu_driver_libre = pd.gpu_driver_libre,
        .linear_light = pd.linear_light,
        .glyph_shader = pd.glyph_shader,
        .content_scale = pd.content_scale,
        .pixel_width = pd.pixel_width,
        .pixel_height = pd.pixel_height,
        .cell_width = pd.cell_width,
        .cell_height = pd.cell_height,
        .logical_cell_width = cfr_px_logical(pd.cell_width, pd.content_scale),
        .logical_cell_height = cfr_px_logical(pd.cell_height, pd.content_scale),
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
        .dim_opacity = portty_dim_opacity,
        .bell = c ? portty_bell_mode_name(c->bell) : "always",
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
        .kitty_kb_flags = terminal_get_kitty_kb_flags(app->term),
#ifdef _WIN32
        .conpty_dcs_ok = pty_conpty_dcs_passthrough(),
        .conpty_host = pty_conpty_host_name(),
#endif
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

bool portty_app_dump_state(PorttyApp *app, const char *path,
                           PorttyDumpResult *res)
{
    if (res) {
        res->ok = false;
        res->path[0] = '\0';
        res->error[0] = '\0';
    }
    if (!app || !app->term)
        return false;

    PorttyDiag pd = { 0 };
    if (app->backend && app->backend->get_diag)
        app->backend->get_diag(app->backend, &pd);

    TerminalDumpMeta meta = {
        .diag = &pd,
        .conf = app->conf,
        .scroll_offset = (app->backend && app->backend->get_scroll_offset)
                             ? app->backend->get_scroll_offset(app->backend)
                             : 0,
    };

    char resolved[4096];
    char err[256];
    if (!portty_dump_write(app->term, &meta, path, resolved, sizeof(resolved),
                           err, sizeof(err))) {
        vlog("terminal state dump failed: %s\n", err);
        if (res)
            snprintf(res->error, sizeof(res->error), "%s", err);
        return false;
    }

    vlog("terminal state dump written to %s\n", resolved);
    fprintf(stderr, "Terminal state dump written to %s\n", resolved);
    if (res) {
        res->ok = true;
        snprintf(res->path, sizeof(res->path), "%.*s",
                 (int)sizeof(res->path) - 1, resolved);
    }
    return true;
}