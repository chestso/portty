/*
 * portty — PorttyApp terminal callback tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "test_helpers.h"
#include "../src/portty_app.h"
#include "../src/portty_backend.h"
#include "../src/portty_panel.h"
#include "../src/common.h"
#include "../src/term.h"
#include "../src/term_cfr.h"
#include "../src/portty_pty.h"
#include <coffer/coffer.h>
#include <stdlib.h>
#include <string.h>

float portty_text_gamma = 1.0f;
float portty_text_contrast = 0.0f;
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
        .get_cell_size = backend_get_cell_size,
        .set_cursor = backend_set_cursor,
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

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_selection_change_pauses_and_resumes);
    RUN_TEST(test_clipboard_set_forwards_text);
    RUN_TEST(test_cwd_change_forwards_dir);
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

    TEST_SUMMARY();
}
