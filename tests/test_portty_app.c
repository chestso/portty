/*
 * portty — PorttyApp terminal callback tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "test_helpers.h"
#include "../src/portty_app.h"
#include "../src/portty_backend.h"
#include "../src/pager.h"
#include "../src/portty_panel.h"
#include "../src/common.h"
#include "../src/term.h"
#include "../src/term_cfr.h"
#include "../src/portty_pty.h"
#include <coffer/coffer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

float portty_text_gamma = 1.0f;
float portty_text_contrast = 0.0f;
float portty_dim_opacity = 0.6f;
bool portty_notification_transparent = false;

// Stub implementations for the backend functions called by the extracted
// callbacks. Avoids linking platform.c/pty.c and their heavy dependencies.
typedef struct
{
    bool pause_called;
    bool resume_called;
    char clipboard_text[64];
    const char *working_dir;
    // Hover tracking
    bool panel_show_called;
    bool panel_hide_called;
    int panel_hide_id;
    bool set_cursor_called;
    PorttyCursor set_cursor_shape;
    int set_cursor_call_count;
    // Panel close-button hover tracking
    int hit_test_id;      // id returned by panel_hit_test (0 = none)
    bool hit_test_close;  // whether hit test reports close button
    int hover_set_id;     // last id passed to panel_set_hover
    bool hover_set_value; // last value passed to panel_set_hover
    int panel_set_hover_calls;
    // Notification tracking
    bool notify_called;
    char notify_title[128];
    char notify_body[512];
    PorttyNotifyLevel notify_level;
    unsigned int notify_ttl_ms;
    bool notify_showing; // a message is on screen (notify_active)
    bool focused;        // window keyboard focus (has_focus)
    // Display-scale change (the window moved to another display)
    float display_scale; // compositor scale (get_display_scale)
    bool reload_called;  // reload_fonts was asked for a new scale
    float reload_scale;  // the content scale it was given
    bool reload_result;  // what reload_fonts reported back
    bool resize_called;  // the grid was re-derived
    // Scroll routing (pager overlay vs. the shell's terminal)
    int scroll_calls;             // times backend->scroll was asked to scroll
    int scroll_delta;             // last delta it was given
    TerminalBackend *scroll_term; // terminal it was asked to scroll
    TerminalBackend *overlay;     // overlay currently handed to the renderer
} StubPlatState;

static StubPlatState g_state = { 0 };
static PorttyBackend g_stub_backend = { 0 };

static void backend_pause_pty(PorttyBackend *self)
{
    (void)self;
    g_state.pause_called = true;
}

static void backend_resume_pty(PorttyBackend *self)
{
    (void)self;
    g_state.resume_called = true;
}

static bool backend_clipboard_set(PorttyBackend *self, const char *text)
{
    (void)self;
    strncpy(g_state.clipboard_text, text, sizeof(g_state.clipboard_text) - 1);
    g_state.clipboard_text[sizeof(g_state.clipboard_text) - 1] = '\0';
    return true;
}

static void backend_set_working_dir(PorttyBackend *self, const char *dir)
{
    (void)self;
    g_state.working_dir = dir;
}

static void backend_panel_hide(PorttyBackend *self, int id)
{
    (void)self;
    g_state.panel_hide_called = true;
    g_state.panel_hide_id = id;
}

static int backend_panel_hit_test(PorttyBackend *self, int px, int py,
                                  bool *close_btn)
{
    (void)self;
    (void)px;
    (void)py;
    if (close_btn)
        *close_btn = g_state.hit_test_close;
    return g_state.hit_test_id;
}

static int backend_get_scroll_offset(PorttyBackend *self)
{
    (void)self;
    return 0;
}

static void backend_scroll(PorttyBackend *self, TerminalBackend *term, int delta)
{
    (void)self;
    g_state.scroll_calls++;
    g_state.scroll_delta = delta;
    g_state.scroll_term = term;
}

static void backend_set_overlay(PorttyBackend *self, TerminalBackend *overlay)
{
    (void)self;
    g_state.overlay = overlay;
}

static void backend_clear_overlay(PorttyBackend *self)
{
    (void)self;
    g_state.overlay = NULL;
}

static bool backend_get_cell_size(PorttyBackend *self, int *cw, int *ch)
{
    (void)self;
    *cw = 10;
    *ch = 6;
    return true;
}

static void backend_panel_set_hover(PorttyBackend *self, int id, bool hovered)
{
    (void)self;
    g_state.panel_set_hover_calls++;
    g_state.hover_set_id = id;
    g_state.hover_set_value = hovered;
}

static void backend_set_cursor(PorttyBackend *self, PorttyCursor shape)
{
    (void)self;
    g_state.set_cursor_called = true;
    g_state.set_cursor_shape = shape;
    g_state.set_cursor_call_count++;
}

static void backend_notify(PorttyBackend *self, const char *title,
                           const char *body, PorttyNotifyLevel level,
                           unsigned int ttl_ms)
{
    (void)self;
    g_state.notify_called = true;
    g_state.notify_showing = true;
    snprintf(g_state.notify_title, sizeof(g_state.notify_title), "%s",
             title ? title : "");
    snprintf(g_state.notify_body, sizeof(g_state.notify_body), "%s",
             body ? body : "");
    g_state.notify_level = level;
    g_state.notify_ttl_ms = ttl_ms;
}

static bool backend_notify_active(PorttyBackend *self)
{
    (void)self;
    return g_state.notify_showing;
}

static bool backend_has_focus(PorttyBackend *self)
{
    (void)self;
    return g_state.focused;
}

static float backend_get_display_scale(PorttyBackend *self)
{
    (void)self;
    return g_state.display_scale;
}

static bool backend_reload_fonts(PorttyBackend *self, float content_scale)
{
    (void)self;
    g_state.reload_called = true;
    g_state.reload_scale = content_scale;
    return g_state.reload_result;
}

static void backend_resize(PorttyBackend *self, int w, int h)
{
    (void)self;
    (void)w;
    (void)h;
    g_state.resize_called = true;
}

static void reset_state(void)
{
    g_state = (StubPlatState){ 0 };
    g_stub_backend = (PorttyBackend){
        .pause_pty = backend_pause_pty,
        .resume_pty = backend_resume_pty,
        .clipboard_set = backend_clipboard_set,
        .set_working_dir = backend_set_working_dir,
        .panel_hide = backend_panel_hide,
        .panel_hit_test = backend_panel_hit_test,
        .panel_set_hover = backend_panel_set_hover,
        .get_scroll_offset = backend_get_scroll_offset,
        .scroll = backend_scroll,
        .set_overlay = backend_set_overlay,
        .clear_overlay = backend_clear_overlay,
        .get_cell_size = backend_get_cell_size,
        .set_cursor = backend_set_cursor,
        .notify = backend_notify,
        .notify_active = backend_notify_active,
        .has_focus = backend_has_focus,
        .get_display_scale = backend_get_display_scale,
        .reload_fonts = backend_reload_fonts,
        .resize = backend_resize,
    };
}

static void test_selection_change_pauses_and_resumes(void)
{
    reset_state();
    PorttyApp app = { 0 };
    app.backend = &g_stub_backend;

    portty_app_selection_change(true, &app);
    ASSERT_TRUE(g_state.pause_called);
    ASSERT_FALSE(g_state.resume_called);

    portty_app_selection_change(false, &app);
    ASSERT_TRUE(g_state.resume_called);
}

static void test_clipboard_set_forwards_text(void)
{
    reset_state();
    PorttyApp app = { 0 };
    app.backend = &g_stub_backend;

    portty_app_clipboard_set("hello", 5, &app);
    ASSERT_STR_EQ(g_state.clipboard_text, "hello");
}

static void test_cwd_change_forwards_dir(void)
{
    reset_state();
    PorttyApp app = { 0 };
    app.backend = &g_stub_backend;

    portty_app_cwd_change("/tmp", &app);
    ASSERT_STR_EQ(g_state.working_dir, "/tmp");
}

static void test_term_output_to_pty_writes(void)
{
    reset_state();
    // NULL app/pty should not crash.
    portty_app_term_output_to_pty("x", 1, NULL);

    PorttyApp app = { 0 };
    portty_app_term_output_to_pty("x", 1, &app);
    ASSERT_TRUE(true);
}

static void test_clear_hover_when_active(void)
{
    reset_state();
    TerminalBackend term = { 0 };
    term.hovered_hyperlink_id = 42;
    PorttyApp app = { .term = &term, .backend = &g_stub_backend };

    portty_app_clear_hover(&app);

    ASSERT_EQ(terminal_hovered_hyperlink(&term), 0);
    ASSERT_TRUE(g_state.panel_hide_called);
    ASSERT_EQ(g_state.panel_hide_id, PANEL_ID_LINK_HINT);
    ASSERT_TRUE(g_state.set_cursor_called);
    ASSERT_EQ(g_state.set_cursor_shape, PORTTY_CURSOR_TEXT);
}

static void test_clear_hover_noop_when_no_hover(void)
{
    reset_state();
    TerminalBackend term = { 0 };
    PorttyApp app = { .term = &term, .backend = &g_stub_backend };

    portty_app_clear_hover(&app);

    ASSERT_EQ(terminal_hovered_hyperlink(&term), 0);
    ASSERT_TRUE(g_state.panel_hide_called);
    ASSERT_FALSE(g_state.set_cursor_called);
}

static void test_clear_hover_null_safety(void)
{
    reset_state();
    // Should not crash with NULL app or NULL term.
    portty_app_clear_hover(NULL);
    ASSERT_TRUE(true);
}

static void test_clear_then_revalidate_no_link(void)
{
    // Simulates scroll: clear hover, then revalidate at a position with no link.
    // The revalidation should NOT re-show the panel or set the pointer cursor.
    reset_state();
    TerminalBackend term = { 0 };
    term.hovered_hyperlink_id = 42;
    PorttyApp app = { .term = &term, .backend = &g_stub_backend };

    // Step 1: clear hover (simulates scroll-induced clear)
    portty_app_clear_hover(&app);
    ASSERT_EQ(terminal_hovered_hyperlink(&term), 0);
    ASSERT_TRUE(g_state.panel_hide_called);

    // Step 2: revalidate at invalid position (no link under cursor)
    g_state.panel_hide_called = false;
    g_state.set_cursor_called = false;
    bool changed = portty_app_revalidate_hover(&app, -1, -1);
    ASSERT_FALSE(changed);
    ASSERT_FALSE(g_state.panel_hide_called);
    ASSERT_FALSE(g_state.set_cursor_called);
}

// ---- Real coffer terminal for keypress-during-selection tests ----

static int mock_send_key_call_count = 0;
static int mock_last_key_sent = 0;
static int mock_last_mod_sent = 0;

static void mock_send_key(TerminalBackend *term, int key, int mod)
{
    (void)term;
    mock_send_key_call_count++;
    mock_last_key_sent = key;
    mock_last_mod_sent = mod;
}

static TerminalBackend *create_key_mock_term(void)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    TerminalBackend *term = term_cfr_new(&cfg);
    if (term)
        term->send_key = mock_send_key;
    return term;
}

static void destroy_key_mock_term(TerminalBackend *term)
{
    terminal_destroy(term);
    free(term);
}

/* BEL is presented as the transient status panel — portty has no audible
 * bell. With no title set there is no detail line. */
static void test_bell_notifies_panel(void)
{
    reset_state();
    PorttyApp app = { 0 };
    app.backend = &g_stub_backend;

    portty_app_bell(&app);
    ASSERT_TRUE(g_state.notify_called);
    ASSERT_STR_EQ(g_state.notify_title, "Bell");
    ASSERT_STR_EQ(g_state.notify_body, "");
    ASSERT_EQ(g_state.notify_level, PORTTY_NOTIFY_INFO);
    ASSERT_TRUE(g_state.notify_ttl_ms > 0);

    // A NULL app must be inert.
    g_state.notify_called = false;
    portty_app_bell(NULL);
    ASSERT_FALSE(g_state.notify_called);
}

/* The detail line is the window title: the one piece of context portty has
 * about which program rang. */
static void test_bell_body_carries_window_title(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    const char *osc = "\x1b]2;make -j8\a";
    terminal_process_input(term, osc, strlen(osc));
    ASSERT_STR_EQ(terminal_get_title(term), "make -j8");

    portty_app_bell(&app);
    ASSERT_TRUE(g_state.notify_called);
    ASSERT_STR_EQ(g_state.notify_title, "Bell");
    ASSERT_STR_EQ(g_state.notify_body, "make -j8");

    destroy_key_mock_term(term);
}

/* `bell` decides when the panel shows: `none` never, `unfocused` only while
 * the window lacks focus, `always` regardless. */
static void test_bell_respects_config_modes(void)
{
    reset_state();
    PorttyApp app = { 0 };
    app.backend = &g_stub_backend;
    PorttyConf conf = { 0 };
    app.conf = &conf;

    conf.bell = PORTTY_BELL_NONE;
    g_state.focused = true;
    portty_app_bell(&app);
    ASSERT_FALSE(g_state.notify_called);

    conf.bell = PORTTY_BELL_UNFOCUSED;
    portty_app_bell(&app);
    ASSERT_FALSE(g_state.notify_called); // focused: silent

    g_state.focused = false;
    portty_app_bell(&app);
    ASSERT_TRUE(g_state.notify_called);

    reset_state();
    conf.bell = PORTTY_BELL_ALWAYS;
    g_state.focused = true;
    portty_app_bell(&app);
    ASSERT_TRUE(g_state.notify_called);
}

/* One panel slot: a BEL arriving while a message is still up is dropped
 * instead of restacking the slot, so a program ringing in a loop cannot pin
 * the panel there. */
static void test_bell_does_not_restack_message(void)
{
    reset_state();
    PorttyApp app = { 0 };
    app.backend = &g_stub_backend;

    g_state.notify_showing = true;
    portty_app_bell(&app);
    ASSERT_FALSE(g_state.notify_called);
}

static void test_keypress_does_not_clear_selection(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    // Start a selection
    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));

    // Press arrow down — should NOT clear selection
    KeyboardResult r = portty_app_handle_key(&app, TERM_KEY_DOWN,
                                             TERM_MOD_NONE, 0);
    ASSERT_TRUE(terminal_selection_active(term));
    ASSERT_TRUE(r.handled);
    ASSERT_EQ(mock_send_key_call_count, 1);
    ASSERT_EQ(mock_last_key_sent, TERM_KEY_DOWN);

    destroy_key_mock_term(term);
}

static void test_keypress_resumes_pty_during_selection(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    // Start selection → callback would pause PTY
    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    // Simulate the pause
    g_stub_backend.pause_pty(&g_stub_backend);
    ASSERT_TRUE(g_state.pause_called);
    ASSERT_FALSE(g_state.resume_called);

    // Reset resume flag to detect the resume from keypress
    g_state.resume_called = false;
    g_state.pause_called = false;

    // Press a key — should resume PTY so the app can respond
    portty_app_handle_key(&app, TERM_KEY_DOWN, TERM_MOD_NONE, 0);
    ASSERT_TRUE(g_state.resume_called);

    destroy_key_mock_term(term);
}

static void test_ctrl_c_still_copies_and_clears(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    // Use word selection so there's actual text to copy
    terminal_selection_start(term, 5, 10, TERM_SELECT_WORD);
    ASSERT_TRUE(terminal_selection_active(term));

    // Ctrl+C should copy and clear
    KeyboardResult r = portty_app_handle_key(&app, TERM_KEY_NONE,
                                             TERM_MOD_CTRL, 'c');
    ASSERT_FALSE(terminal_selection_active(term));
    ASSERT_TRUE(r.handled);

    destroy_key_mock_term(term);
}

static void test_text_input_does_not_clear_selection(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));

    // Text input should NOT clear selection
    KeyboardResult r = portty_app_handle_text(&app, "a");
    ASSERT_TRUE(terminal_selection_active(term));
    ASSERT_EQ(r.len, 1);

    destroy_key_mock_term(term);
}

// ---- Panel close-button hover (motion wiring) ----

static void test_panel_hover_on_motion(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    // Motion over a panel close button: hover is set and reported changed.
    g_state.hit_test_id = 3;
    g_state.hit_test_close = true;
    bool changed = portty_app_handle_mouse(&app, 100, 50, 0, false, 0, 0);
    ASSERT_TRUE(changed);
    ASSERT_EQ(app.hovered_panel_id, 3);
    ASSERT_EQ(g_state.panel_set_hover_calls, 1);
    ASSERT_EQ(g_state.hover_set_id, 3);
    ASSERT_TRUE(g_state.hover_set_value);

    // Motion off the close button: hover is cleared.
    g_state.hit_test_close = false;
    changed = portty_app_handle_mouse(&app, 100, 50, 0, false, 0, 0);
    ASSERT_TRUE(changed);
    ASSERT_EQ(app.hovered_panel_id, 0);
    ASSERT_EQ(g_state.panel_set_hover_calls, 2);
    ASSERT_EQ(g_state.hover_set_id, 3);
    ASSERT_FALSE(g_state.hover_set_value);

    // Motion with no change: no redundant backend calls.
    changed = portty_app_handle_mouse(&app, 100, 50, 0, false, 0, 0);
    ASSERT_FALSE(changed);
    ASSERT_EQ(g_state.panel_set_hover_calls, 2);

    destroy_key_mock_term(term);
}

static void test_panel_hover_click_hides_and_resets(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    // Hover the close button...
    g_state.hit_test_id = 3;
    g_state.hit_test_close = true;
    portty_app_handle_mouse(&app, 100, 50, 0, false, 0, 0);
    ASSERT_EQ(app.hovered_panel_id, 3);

    // ...then click it: panel hides and hover state resets.
    bool changed = portty_app_handle_mouse(&app, 100, 50, 1, true, 1, 0);
    ASSERT_TRUE(changed);
    ASSERT_TRUE(g_state.panel_hide_called);
    ASSERT_EQ(g_state.panel_hide_id, 3);
    ASSERT_EQ(app.hovered_panel_id, 0);

    destroy_key_mock_term(term);
}

static void test_clear_hover_clears_panel_hover(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    g_state.hit_test_id = 3;
    g_state.hit_test_close = true;
    portty_app_handle_mouse(&app, 100, 50, 0, false, 0, 0);
    ASSERT_EQ(app.hovered_panel_id, 3);

    // clear_hover (focus lost, scroll, leave) must also drop panel hover.
    portty_app_clear_hover(&app);
    ASSERT_EQ(app.hovered_panel_id, 0);
    ASSERT_EQ(g_state.hover_set_id, 3);
    ASSERT_FALSE(g_state.hover_set_value);

    destroy_key_mock_term(term);
}

// ---- State dump notification ----

// The path separator the dump uses when it joins a directory and a file
// name.
#ifdef _WIN32
#define DUMP_SEP '\\'
#else
#define DUMP_SEP '/'
#endif

// The last path separator in `s`, of either kind: a path can carry both
// (a Windows temp path handed to the test by the environment, say).
static const char *last_sep(const char *s)
{
    const char *last = NULL;
    for (const char *p = s; *p; p++)
        if (*p == '/' || *p == '\\')
            last = p;
    return last;
}

// Create a scratch directory for the dump test. Returns a malloc'd path the
// caller must free, or NULL on failure.
static char *make_temp_dir(void)
{
#ifdef _WIN32
    char base[MAX_PATH];
    if (GetTempPathA(MAX_PATH, base) == 0)
        return NULL;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sportty_test_dump_%lu", base,
             (unsigned long)GetCurrentProcessId());
    if (CreateDirectoryA(path, NULL) == 0 &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return NULL;
    return strdup(path);
#else
    char tmpl[] = "/tmp/portty_test_dump_XXXXXX";
    if (!mkdtemp(tmpl))
        return NULL;
    return strdup(tmpl);
#endif
}

// Create a regular file inside `dir`; the caller must free the path.
static char *make_blocker_file(const char *dir)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/blocker", dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    fputs("x", f);
    fclose(f);
    return strdup(path);
}

static void remove_temp_dir(const char *dir)
{
#ifdef _WIN32
    _rmdir(dir);
#else
    rmdir(dir);
#endif
}

static void test_ctrl_shift_f7_notifies_dump_result(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    char *dir = make_temp_dir();
    ASSERT_NOT_NULL(dir);

    PorttyConf conf = { 0 };
    conf.dump_dir = dir; // keep the dump out of the user's state directory
    PorttyApp app = { .term = term, .backend = &g_stub_backend, .conf = &conf };

    KeyboardResult r = portty_app_handle_key(&app, TERM_KEY_F7,
                                             TERM_MOD_CTRL | TERM_MOD_SHIFT, 0);
    ASSERT_TRUE(r.handled);
    ASSERT_TRUE(g_state.notify_called);
    ASSERT_EQ(g_state.notify_level, PORTTY_NOTIFY_INFO);
    ASSERT_TRUE(g_state.notify_ttl_ms > 0);
    ASSERT_STR_EQ(g_state.notify_title, "State dump written");

    // The panel shows the path elided to the grid width, so it keeps the
    // tail: the file name stays visible however long the path is.
    const char *sep = last_sep(g_state.notify_body);
    ASSERT_NOT_NULL(sep);
    const char *name = sep + 1;
    ASSERT_TRUE(strncmp(name, "portty-state-", 13) == 0);
    ASSERT_TRUE(strstr(name, ".json") != NULL);

    // The dump landed in the scratch directory, under the name the panel
    // shows.
    char full[1024];
    snprintf(full, sizeof(full), "%s%c%s", dir, DUMP_SEP, name);
    FILE *f = fopen(full, "rb");
    ASSERT_NOT_NULL(f);
    if (f)
        fclose(f);

    remove(full);
    remove_temp_dir(dir);
    free(dir);
    destroy_key_mock_term(term);
}

/* Ctrl+Delete resets the input-protocol state. A program that pushed
 * the kitty keyboard protocol and died without popping it leaves every
 * Ctrl+key encoded as CSI-u, which a shell cannot read. */
static void test_ctrl_delete_resets_input_state(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    terminal_process_input(term, "\x1b[>1u", 6);
    ASSERT_EQ(terminal_get_kitty_kb_flags(term), 1);

    int keys_before = mock_send_key_call_count;
    KeyboardResult r = portty_app_handle_key(&app, TERM_KEY_DEL,
                                             TERM_MOD_CTRL, 0);
    ASSERT_TRUE(r.handled);
    ASSERT_EQ(terminal_get_kitty_kb_flags(term), 0);

    /* The chord is consumed by portty — it never reaches the app. */
    ASSERT_EQ(mock_send_key_call_count, keys_before);
    ASSERT_TRUE(g_state.notify_called);
    ASSERT_EQ(g_state.notify_level, PORTTY_NOTIFY_INFO);
    ASSERT_STR_EQ(g_state.notify_title, "Terminal reset");

    destroy_key_mock_term(term);
}

/* Ctrl+Shift+F8 does the full reset: the screen is cleared, like the
 * shell command `reset` does. */
static void test_ctrl_shift_f8_resets_terminal(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };

    terminal_process_input(term, "hello", 5);
    TerminalCell cell;
    ASSERT_EQ(terminal_get_cell(term, 0, 0, &cell), 0);
    ASSERT_EQ(cell.cp, (uint32_t)'h');

    KeyboardResult r = portty_app_handle_key(&app, TERM_KEY_F8,
                                             TERM_MOD_CTRL | TERM_MOD_SHIFT, 0);
    ASSERT_TRUE(r.handled);
    ASSERT_EQ(terminal_get_cell(term, 0, 0, &cell), 0);
    ASSERT_EQ(cell.cp, 0);
    ASSERT_TRUE(g_state.notify_called);
    ASSERT_STR_EQ(g_state.notify_title, "Terminal fully reset");

    destroy_key_mock_term(term);
}

static void test_ctrl_shift_f7_notifies_dump_failure(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    char *dir = make_temp_dir();
    ASSERT_NOT_NULL(dir);
    // A regular file where the dump directory should be: the write fails.
    char *blocker = make_blocker_file(dir);
    ASSERT_NOT_NULL(blocker);

    PorttyConf conf = { 0 };
    conf.dump_dir = blocker;
    PorttyApp app = { .term = term, .backend = &g_stub_backend, .conf = &conf };

    KeyboardResult r = portty_app_handle_key(&app, TERM_KEY_F7,
                                             TERM_MOD_CTRL | TERM_MOD_SHIFT, 0);
    ASSERT_TRUE(r.handled);
    ASSERT_TRUE(g_state.notify_called);
    ASSERT_EQ(g_state.notify_level, PORTTY_NOTIFY_ERROR);
    ASSERT_STR_EQ(g_state.notify_title, "State dump failed");
    ASSERT_TRUE(strlen(g_state.notify_body) > 0);

    remove(blocker);
    free(blocker);
    remove_temp_dir(dir);
    free(dir);
    destroy_key_mock_term(term);
}

/* The window moved to a display with another scale factor: the fonts are
 * re-rasterized for the new DPI, and the grid is derived from the new
 * cell size. */
static void test_display_scale_change_reloads_fonts(void)
{
    reset_state();
    g_state.display_scale = 2.0f;
    g_state.reload_result = true;

    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend, .dpi_scale = 1.0f };

    bool changed = portty_app_handle_display_scale_changed(&app, 800, 600);

    ASSERT_TRUE(changed);
    ASSERT_TRUE(g_state.reload_called);
    ASSERT_FLOAT_NEAR(g_state.reload_scale, 2.0f, 0.01f);
    ASSERT_TRUE(g_state.resize_called);

    // The grid follows the new cell size (the stub reports 10x6 px).
    int rows = 0, cols = 0;
    terminal_get_dimensions(term, &rows, &cols);
    ASSERT_EQ(cols, 80);
    ASSERT_EQ(rows, 100);

    destroy_key_mock_term(term);
}

/* A scale change that does not move the cell size leaves the grid alone. */
static void test_display_scale_change_without_reload_keeps_grid(void)
{
    reset_state();
    g_state.display_scale = 1.0f;
    g_state.reload_result = false;

    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend, .dpi_scale = 1.0f };

    bool changed = portty_app_handle_display_scale_changed(&app, 800, 600);

    ASSERT_FALSE(changed);
    ASSERT_TRUE(g_state.reload_called);
    ASSERT_FALSE(g_state.resize_called);

    int rows = 0, cols = 0;
    terminal_get_dimensions(term, &rows, &cols);
    ASSERT_EQ(cols, 80);
    ASSERT_EQ(rows, 24);

    destroy_key_mock_term(term);
}

/* The user's --dpi-scale folds into the content scale the fonts are
 * rasterized for. */
static void test_display_scale_change_applies_user_scale(void)
{
    reset_state();
    g_state.display_scale = 1.5f;
    g_state.reload_result = true;

    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend, .dpi_scale = 2.0f };

    portty_app_handle_display_scale_changed(&app, 800, 600);

    ASSERT_TRUE(g_state.reload_called);
    ASSERT_FLOAT_NEAR(g_state.reload_scale, 3.0f, 0.01f);

    destroy_key_mock_term(term);
}

/* The pager draws its own overlay terminal, but the wheel is still the host's
 * scroll path: the platform asks pager_mouse first and only scrolls when it
 * declines. A pager that consumed button 4/5 would leave the document stuck. */
static void test_pager_wheel_declines_to_the_scroll_path(void)
{
    reset_state();
    TerminalBackend *term = create_key_mock_term();
    PorttyApp app = { .term = term, .backend = &g_stub_backend };
    app.pager = pager_create(&g_stub_backend, &app);
    ASSERT_NOT_NULL(app.pager);

    ASSERT_TRUE(pager_open(app.pager, "one\ntwo\nthree\nfour\nfive\n", 80, 24));
    ASSERT_TRUE(pager_active(app.pager));
    ASSERT_NOT_NULL(g_state.overlay);

    // Wheel up: not consumed, and no scroll happened behind the app's back.
    ASSERT_FALSE(portty_app_handle_mouse(&app, 100, 50, 4, true, 0, 0));
    ASSERT_EQ(g_state.scroll_calls, 0);

    // The fall-through scroll path moves the overlay, not the shell's grid.
    portty_app_handle_scroll(&app, 1);
    ASSERT_EQ(g_state.scroll_calls, 1);
    ASSERT_EQ(g_state.scroll_delta, SCROLL_LINES_PER_TICK);
    ASSERT_EQ(g_state.scroll_term, g_state.overlay);
    ASSERT_TRUE(g_state.scroll_term != term);

    // Wheel down declines the same way, with the opposite direction.
    ASSERT_FALSE(portty_app_handle_mouse(&app, 100, 50, 5, true, 0, 0));
    portty_app_handle_scroll(&app, -1);
    ASSERT_EQ(g_state.scroll_calls, 2);
    ASSERT_EQ(g_state.scroll_delta, -SCROLL_LINES_PER_TICK);

    // Buttons stay modal: a click in the pager never reaches the shell.
    ASSERT_TRUE(portty_app_handle_mouse(&app, 100, 50, 1, true, 1, 0));

    pager_destroy(app.pager);
    destroy_key_mock_term(term);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_selection_change_pauses_and_resumes);
    RUN_TEST(test_clipboard_set_forwards_text);
    RUN_TEST(test_cwd_change_forwards_dir);
    RUN_TEST(test_bell_notifies_panel);
    RUN_TEST(test_bell_body_carries_window_title);
    RUN_TEST(test_bell_respects_config_modes);
    RUN_TEST(test_bell_does_not_restack_message);
    RUN_TEST(test_term_output_to_pty_writes);
    RUN_TEST(test_clear_hover_when_active);
    RUN_TEST(test_clear_hover_noop_when_no_hover);
    RUN_TEST(test_clear_hover_null_safety);
    RUN_TEST(test_clear_then_revalidate_no_link);
    RUN_TEST(test_panel_hover_on_motion);
    RUN_TEST(test_panel_hover_click_hides_and_resets);
    RUN_TEST(test_clear_hover_clears_panel_hover);

    // Keypress-during-selection behavior
    RUN_TEST(test_keypress_does_not_clear_selection);
    RUN_TEST(test_keypress_resumes_pty_during_selection);
    RUN_TEST(test_ctrl_c_still_copies_and_clears);
    RUN_TEST(test_text_input_does_not_clear_selection);

    // State dump notification
    RUN_TEST(test_ctrl_shift_f7_notifies_dump_result);
    RUN_TEST(test_ctrl_shift_f7_notifies_dump_failure);
    RUN_TEST(test_ctrl_delete_resets_input_state);
    RUN_TEST(test_ctrl_shift_f8_resets_terminal);

    // Display-scale change (the window moved to another display)
    RUN_TEST(test_display_scale_change_reloads_fonts);
    RUN_TEST(test_display_scale_change_without_reload_keeps_grid);
    RUN_TEST(test_display_scale_change_applies_user_scale);

    // Pager overlay: the wheel declines to the host scroll path
    RUN_TEST(test_pager_wheel_declines_to_the_scroll_path);

    TEST_SUMMARY();
}
