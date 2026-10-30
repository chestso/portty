/*
 * portty — Application-level glue interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef PORTTY_APP_H
#define PORTTY_APP_H

#include "font_resolve.h"
#include "portty_backend.h"
#include "portty_conf.h"
#include "portty_pty.h"
#include "term.h"
#include <stdbool.h>
#include <stdint.h>

struct Pager;
typedef struct Pager Pager;

// Shared application state used by the SDL3 backend.
// Backends call the portty_app_handle_*() functions when input arrives;
// those handlers call back into the backend via app->backend.
//
// Field layout note: the first fields are ordered to match the legacy
// MainContext struct in main.c so that `typedef PorttyApp MainContext`
// remains binary-compatible during the transition.
typedef struct PorttyApp
{
    TerminalBackend *term;
    PtyContext *pty;
    const PorttyConf *conf;
    const char *font_source;
    Pager *pager;
    bool drag_pending;
    int drag_start_row;
    int drag_start_col;
    int drag_last_pixel_x;
    int drag_last_pixel_y;
    bool autoscroll_active;
    int autoscroll_direction;
    bool pointer_outside;
    bool drag_in_progress;
    bool cursor_visible;
    int hovered_panel_id; // panel whose close button is hovered (0 = none)

    // Kitty keyboard protocol recovery. `kb_flags_seen` is the flag mask
    // last observed in the terminal, `kb_flags_owner_pgrp` the foreground
    // process group that was in the foreground when those flags appeared.
    // When the foreground group changes and the owner is gone, the
    // program that pushed the flags died without popping them.
    uint32_t kb_flags_seen;
    int kb_flags_owner_pgrp;

    PorttyBackend *backend;           // selected backend (filled by entry point)
    FontResolveBackend *font_resolve; // platform-specific via #ifdef

    // Command-line launch configuration.
    const char *demo_text;   // -d TEXT, NULL if none
    char **exec_argv;        // fully-resolved command for pty_create
    float font_size;         // default 12.0f, not in PorttyConf
    const char *font_name;   // -f FONT or config file font
    const char *script_path; // -S FILE, NULL if none
    float dpi_scale;         // --dpi-scale multiplier (default 1.0f)
    bool borderless;         // --borderless or borderless=true in config
} PorttyApp;

// Terminal output callbacks (registered with coffer).
void portty_app_term_output_to_pty(const char *data, size_t len, void *user_data);
void portty_app_selection_change(bool active, void *user_data);
void portty_app_clipboard_set(const char *text, size_t len, void *user_data);
void portty_app_cwd_change(const char *dir, void *user_data);
// BEL from the child: shown as a transient status panel.
void portty_app_bell(void *user_data);

// Input handlers called by backends.
KeyboardResult portty_app_handle_key(PorttyApp *app, int term_key,
                                     int mod, uint32_t codepoint);
KeyboardResult portty_app_handle_text(PorttyApp *app, const char *text);
bool portty_app_handle_mouse(PorttyApp *app, int px, int py,
                             int button, bool pressed, int clicks, int mod);
void portty_app_handle_scroll(PorttyApp *app, int delta);
void portty_app_handle_resize(PorttyApp *app, int pixel_w, int pixel_h);
// The window moved to a display with another scale factor: the fonts are
// re-rasterized for the new DPI and the grid is derived from the new cell
// size. Returns true when the grid was re-derived.
bool portty_app_handle_display_scale_changed(PorttyApp *app, int pixel_w, int pixel_h);
void portty_app_handle_mouse_leave(PorttyApp *app, int px, int py);
void portty_app_handle_mouse_enter(PorttyApp *app);
bool portty_app_revalidate_hover(PorttyApp *app, int px, int py);
void portty_app_clear_hover(PorttyApp *app);
void portty_app_handle_autoscroll_tick(PorttyApp *app);
void portty_app_feed_terminal(void *app, const char *data, size_t len);
void portty_app_handle_pty_closed(PorttyApp *app);
void portty_app_handle_child_exit(PorttyApp *app, int status);

// Pop the kitty keyboard protocol flags when the program that pushed
// them has died without popping them. Call after PTY output is fed to
// the terminal.
void portty_app_check_kb_protocol_owner(PorttyApp *app);

// Pixel-to-cell conversion (used by both backends' mouse handlers).
bool portty_app_pixel_to_cell(PorttyApp *app, int px, int py,
                              int *row, int *col);

// Display row to unified row (scrollback rows are negative).
int portty_app_display_row_to_unified(PorttyApp *app, int display_row);

// Diagnostics report
void portty_app_show_diagnostics(PorttyApp *app);

// Outcome of a terminal-state dump.
typedef struct
{
    bool ok;         // the dump was written
    char path[1024]; // resolved destination, when ok
    char error[256]; // reason for the failure, when not ok
} PorttyDumpResult;

// Dump the entire terminal state to a JSON file for debugging. `path` NULL
// selects the default location; "-" writes to stdout. `res` (when non-NULL)
// receives the outcome. Returns true on success.
bool portty_app_dump_state(PorttyApp *app, const char *path,
                           PorttyDumpResult *res);

// Lifecycle helpers.
void portty_app_set_autoscroll(PorttyApp *app, bool enabled);

// Content scale calculation (unified for all backends).
float portty_compute_content_scale(float system_scale, float user_scale);

// The cell's size in LOGICAL (unscaled) pixels — the unit a child measures
// in: TIOCGWINSZ ws_xpixel/ws_ypixel and the cell-size queries carry it,
// and everything a client sends back is scaled by the content scale once.
int portty_logical_cell_px(int cell_px, float content_scale);

#endif // PORTTY_APP_H
