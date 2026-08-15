/*
 * portty — Regression tests for altscreen and mouse-tracking mode state
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_altscreen_mouse — regression tests for altscreen and mouse-tracking
 * mode state through the coffer backend.
 *
 * The on_mouse handler in main.c dispatches mouse events based on
 * terminal_is_altscreen() and terminal_get_mouse_mode(). These tests
 * verify the backend correctly surfaces both states so the dispatch
 * logic can make the right decision.
 *
 * Motivated by the Wayland clipboard use-after-free crash: the crash
 * occurred when a selection and an OSC 52 clipboard set raced in the
 * same event loop iteration, and the deferred-free mechanism was
 * insufficient. Correct mouse-mode and altscreen reporting is essential
 * so that mouse events reach the right handler (app or terminal).
 */

#include "test_helpers.h"

#include "term.h"
#include "term_cfr.h"

#include <string.h>

extern TerminalBackend terminal_backend_cfr;

static void init_term(TerminalBackend *t, int cols, int rows)
{
    *t = terminal_backend_cfr;
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.cols = cols;
    cfg.rows = rows;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    if (!terminal_init(t, &cfg)) {
        fprintf(stderr, "  FAIL: terminal_init failed\n");
        test_fail_count++;
    }
}

static void feed(TerminalBackend *t, const char *s)
{
    terminal_process_input(t, s, strlen(s));
}

/* ------------------------------------------------------------------ */
/* Altscreen state                                                     */
/* ------------------------------------------------------------------ */

static void test_default_not_altscreen(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    ASSERT_FALSE(terminal_is_altscreen(&t));
    terminal_destroy(&t);
}

static void test_enter_altscreen(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    feed(&t, "\x1b[?1049h");
    ASSERT_TRUE(terminal_is_altscreen(&t));
    terminal_destroy(&t);
}

static void test_leave_altscreen(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    feed(&t, "\x1b[?1049h");
    ASSERT_TRUE(terminal_is_altscreen(&t));
    feed(&t, "\x1b[?1049l");
    ASSERT_FALSE(terminal_is_altscreen(&t));
    terminal_destroy(&t);
}

/* ------------------------------------------------------------------ */
/* Mouse mode state                                                    */
/* ------------------------------------------------------------------ */

static void test_default_no_mouse_mode(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    ASSERT_EQ(terminal_get_mouse_mode(&t), 0);
    terminal_destroy(&t);
}

static void test_mouse_btn_event_mode(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    feed(&t, "\x1b[?1000h");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 1);
    feed(&t, "\x1b[?1000l");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 0);
    terminal_destroy(&t);
}

static void test_mouse_drag_mode(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    feed(&t, "\x1b[?1002h");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 2);
    feed(&t, "\x1b[?1002l");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 0);
    terminal_destroy(&t);
}

static void test_mouse_any_event_mode(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    feed(&t, "\x1b[?1003h");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 3);
    feed(&t, "\x1b[?1003l");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 0);
    terminal_destroy(&t);
}

/* ------------------------------------------------------------------ */
/* Independence of altscreen and mouse mode                            */
/* ------------------------------------------------------------------ */

/* Crush scenario: altscreen + mouse drag (1002h + 1049h). Both states
 * must be true simultaneously so on_mouse forwards events to the app. */
static void test_altscreen_with_mouse_drag(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    feed(&t, "\x1b[?1049h");
    feed(&t, "\x1b[?1002h");
    ASSERT_TRUE(terminal_is_altscreen(&t));
    ASSERT_EQ(terminal_get_mouse_mode(&t), 2);
    terminal_destroy(&t);
}

/* An app can enable mouse tracking without altscreen (e.g. tmux). */
static void test_mouse_mode_without_altscreen(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    feed(&t, "\x1b[?1000h");
    ASSERT_FALSE(terminal_is_altscreen(&t));
    ASSERT_EQ(terminal_get_mouse_mode(&t), 1);
    terminal_destroy(&t);
}

/* Switching from one mouse mode to another replaces the previous mode. */
static void test_mouse_mode_switch_replaces(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);
    feed(&t, "\x1b[?1000h");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 1);
    feed(&t, "\x1b[?1002h");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 2);
    feed(&t, "\x1b[?1003h");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 3);
    feed(&t, "\x1b[?1002l");
    ASSERT_EQ(terminal_get_mouse_mode(&t), 0);
    terminal_destroy(&t);
}

/* Leaving altscreen must not affect mouse mode, and vice versa. */
static void test_altscreen_and_mouse_mode_independent_lifecycles(void)
{
    TerminalBackend t;
    init_term(&t, 20, 4);

    feed(&t, "\x1b[?1049h");
    feed(&t, "\x1b[?1002h");
    ASSERT_TRUE(terminal_is_altscreen(&t));
    ASSERT_EQ(terminal_get_mouse_mode(&t), 2);

    feed(&t, "\x1b[?1049l");
    ASSERT_FALSE(terminal_is_altscreen(&t));
    ASSERT_EQ(terminal_get_mouse_mode(&t), 2);

    feed(&t, "\x1b[?1049h");
    feed(&t, "\x1b[?1002l");
    ASSERT_TRUE(terminal_is_altscreen(&t));
    ASSERT_EQ(terminal_get_mouse_mode(&t), 0);

    terminal_destroy(&t);
}

/* ------------------------------------------------------------------ */
/* Mouse dispatch decision logic                                       */
/*                                                                     */
/* These tests replicate the should_forward decision from on_mouse in  */
/* main.c. They guard against regressions where mouse events are       */
/* incorrectly swallowed or not forwarded to the app.                  */
/* ------------------------------------------------------------------ */

/* Replicate the should_forward logic from on_mouse for button events. */
static bool should_forward_button(int mouse_mode, int button, bool pressed)
{
    if (mouse_mode > 0) {
        if (button == 4 || button == 5) {
            return mouse_mode > 0;
        } else if (button > 0) {
            return true;
        } else {
            return (mouse_mode >= 2 && pressed) || (mouse_mode >= 3);
        }
    }
    return false;
}

/* After should_forward returns false and the event is not a wheel
 * button, the event may reach terminal-level selection or right-click
 * paste.  This function returns true when the event reaches the
 * selection code path (i.e. it is NOT swallowed and NOT forwarded
 * to the app).
 *
 * In altscreen with no mouse protocol active, the application owns the
 * display: left-click/drag selection is blocked to avoid clobbering
 * app clipboard operations and visual artifacts.  Shift overrides.
 * Right-click paste (button 3) is allowed through regardless. */
static bool reaches_selection(bool in_altscreen, int mouse_mode,
                              int button, bool shift_held)
{
    if (mouse_mode > 0 && !shift_held)
        return false;
    if (button == 4 || button == 5)
        return false;
    if (in_altscreen && !shift_held && mouse_mode == 0 && button != 3)
        return false;
    return true;
}

/* Crush scenario: altscreen + mouse_mode=2, left click → must forward
 * to the app, not start terminal selection. */
static void test_dispatch_altscreen_mouse2_forwards_click(void)
{
    ASSERT_TRUE(should_forward_button(2, 1, true));
    ASSERT_FALSE(reaches_selection(true, 2, 1, false));
}

/* Crush scenario: altscreen + mouse_mode=2, drag motion → must forward
 * (mouse_mode >= 2 with button held). */
static void test_dispatch_altscreen_mouse2_forwards_drag(void)
{
    ASSERT_TRUE(should_forward_button(2, 0, true));
    ASSERT_FALSE(reaches_selection(true, 2, 0, false));
}

/* Altscreen + mouse_mode=0 (no mouse protocol), left click: the app
 * hasn't claimed the pointer but it owns the display.  Terminal-level
 * selection is blocked to avoid clobbering app clipboard operations
 * and visual artifacts over the app's UI. */
static void test_dispatch_altscreen_no_mouse_blocks_selection(void)
{
    ASSERT_FALSE(should_forward_button(0, 1, true));
    ASSERT_FALSE(reaches_selection(true, 0, 1, false));
}

/* Altscreen + mouse_mode=0 + Shift: the user's escape hatch.  Selection
 * is allowed because Shift explicitly overrides the guard. */
static void test_dispatch_altscreen_no_mouse_shift_allows_selection(void)
{
    ASSERT_FALSE(should_forward_button(0, 1, true));
    ASSERT_TRUE(reaches_selection(true, 0, 1, true));
}

/* Altscreen + mouse_mode=0, right-click: paste is still useful even in
 * altscreen apps, so it must not be blocked. */
static void test_dispatch_altscreen_no_mouse_allows_right_click(void)
{
    ASSERT_FALSE(should_forward_button(0, 3, true));
    ASSERT_TRUE(reaches_selection(true, 0, 3, false));
}

/* Altscreen + mouse_mode=0, drag motion: blocked like left click. */
static void test_dispatch_altscreen_no_mouse_blocks_drag(void)
{
    ASSERT_FALSE(should_forward_button(0, 0, true));
    ASSERT_FALSE(reaches_selection(true, 0, 0, false));
}

/* Primary screen + mouse_mode=0: normal selection works (no guard). */
static void test_dispatch_primary_no_mouse_allows_selection(void)
{
    ASSERT_FALSE(should_forward_button(0, 1, true));
    ASSERT_TRUE(reaches_selection(false, 0, 1, false));
}

/* Primary screen + mouse_mode=1 (BTN_EVENT): click forwarded. */
static void test_dispatch_primary_mouse1_forwards_click(void)
{
    ASSERT_TRUE(should_forward_button(1, 1, true));
    ASSERT_FALSE(reaches_selection(false, 1, 1, false));
}

/* mouse_mode=1 does NOT forward motion events (only button press/release). */
static void test_dispatch_mouse1_no_motion_forward(void)
{
    ASSERT_FALSE(should_forward_button(1, 0, true));
}

/* mouse_mode=2 forwards motion while button held (drag). */
static void test_dispatch_mouse2_forwards_held_motion(void)
{
    ASSERT_TRUE(should_forward_button(2, 0, true));
}

/* mouse_mode=2 does NOT forward motion with no button held. */
static void test_dispatch_mouse2_no_idle_motion(void)
{
    ASSERT_FALSE(should_forward_button(2, 0, false));
}

/* mouse_mode=3 forwards ALL motion events. */
static void test_dispatch_mouse3_forwards_all_motion(void)
{
    ASSERT_TRUE(should_forward_button(3, 0, false));
    ASSERT_TRUE(should_forward_button(3, 0, true));
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_default_not_altscreen);
    RUN_TEST(test_enter_altscreen);
    RUN_TEST(test_leave_altscreen);
    RUN_TEST(test_default_no_mouse_mode);
    RUN_TEST(test_mouse_btn_event_mode);
    RUN_TEST(test_mouse_drag_mode);
    RUN_TEST(test_mouse_any_event_mode);
    RUN_TEST(test_altscreen_with_mouse_drag);
    RUN_TEST(test_mouse_mode_without_altscreen);
    RUN_TEST(test_mouse_mode_switch_replaces);
    RUN_TEST(test_altscreen_and_mouse_mode_independent_lifecycles);

    RUN_TEST(test_dispatch_altscreen_mouse2_forwards_click);
    RUN_TEST(test_dispatch_altscreen_mouse2_forwards_drag);
    RUN_TEST(test_dispatch_altscreen_no_mouse_blocks_selection);
    RUN_TEST(test_dispatch_altscreen_no_mouse_shift_allows_selection);
    RUN_TEST(test_dispatch_altscreen_no_mouse_allows_right_click);
    RUN_TEST(test_dispatch_altscreen_no_mouse_blocks_drag);
    RUN_TEST(test_dispatch_primary_no_mouse_allows_selection);
    RUN_TEST(test_dispatch_primary_mouse1_forwards_click);
    RUN_TEST(test_dispatch_mouse1_no_motion_forward);
    RUN_TEST(test_dispatch_mouse2_forwards_held_motion);
    RUN_TEST(test_dispatch_mouse2_no_idle_motion);
    RUN_TEST(test_dispatch_mouse3_forwards_all_motion);

    TEST_SUMMARY();
}
