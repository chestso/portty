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

// Shared application state used by both SDL3 and Sokol backends.
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

    PorttyBackend *backend;           // selected backend (filled by entry point)
    FontResolveBackend *font_resolve; // platform-specific via #ifdef
} PorttyApp;

// Terminal output callbacks (registered with coffer).
void portty_app_term_output_to_pty(const char *data, size_t len, void *user_data);
void portty_app_selection_change(bool active, void *user_data);
void portty_app_clipboard_set(const char *text, size_t len, void *user_data);
void portty_app_cwd_change(const char *dir, void *user_data);

// Input handlers called by backends.
KeyboardResult portty_app_handle_key(PorttyApp *app, int term_key,
                                     int mod, uint32_t codepoint);
KeyboardResult portty_app_handle_text(PorttyApp *app, const char *text);
bool portty_app_handle_mouse(PorttyApp *app, int px, int py,
                             int button, bool pressed, int clicks, int mod);
void portty_app_handle_scroll(PorttyApp *app, int delta);
void portty_app_handle_resize(PorttyApp *app, int pixel_w, int pixel_h);
void portty_app_handle_mouse_leave(PorttyApp *app, int px, int py);
void portty_app_handle_mouse_enter(PorttyApp *app);
bool portty_app_revalidate_hover(PorttyApp *app, int px, int py);
void portty_app_handle_autoscroll_tick(PorttyApp *app);
void portty_app_process_pty_data(PorttyApp *app, const char *data, size_t len);
void portty_app_handle_pty_closed(PorttyApp *app);
void portty_app_handle_child_exit(PorttyApp *app, int status);

// Pixel-to-cell conversion (used by both backends' mouse handlers).
bool portty_app_pixel_to_cell(PorttyApp *app, int px, int py,
                              int *row, int *col);

// Display row to unified row (scrollback rows are negative).
int portty_app_display_row_to_unified(PorttyApp *app, int display_row);

// Diagnostics report
void portty_app_show_diagnostics(PorttyApp *app);

// Lifecycle helpers.
void portty_app_set_autoscroll(PorttyApp *app, bool enabled);

#endif // PORTTY_APP_H
