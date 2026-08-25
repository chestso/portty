/*
 * portty — PTY pause/resume during selection tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "term.h"
#include "term_cfr.h"
#include "portty_pty.h"
#include "test_helpers.h"
#include <coffer/coffer.h>
#include <stdlib.h>
#include <string.h>

// pty_write stub — term.c references this but the test has no real PTY.
ssize_t pty_write(PtyContext *ctx, const char *data, size_t len)
{
    (void)ctx;
    (void)data;
    return (ssize_t)len;
}

static TerminalBackend *create_test_term(void)
{
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = 24;
    cfg.cols = 80;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 6;
    return term_cfr_new(&cfg);
}

static void destroy_test_term(TerminalBackend *term)
{
    terminal_destroy(term);
    free(term);
}

// ---- Selection change callback tracking ----

static int cb_active_count = 0;
static int cb_inactive_count = 0;
static bool cb_last_active = false;

static void tracking_selection_cb(bool active, void *user_data)
{
    (void)user_data;
    cb_last_active = active;
    if (active)
        cb_active_count++;
    else
        cb_inactive_count++;
}

static int mock_pause_count = 0;
static int mock_resume_count = 0;

static void mock_selection_cb(bool active, void *user_data)
{
    (void)user_data;
    if (active)
        mock_pause_count++;
    else
        mock_resume_count++;
}

static void reset_cb_counts(void)
{
    cb_active_count = 0;
    cb_inactive_count = 0;
    cb_last_active = false;
}

// ---- Tests: process_input selection behavior ----

static void test_process_input_preserves_selection(void)
{
    TerminalBackend *term = create_test_term();

    // Start selection at row 5 — cursor is at row 0 initially
    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));

    // Write "hello" at cursor position (row 0) — doesn't intersect row 5
    terminal_process_input(term, "hello", 5);
    ASSERT_TRUE(terminal_selection_active(term));

    destroy_test_term(term);
}

// ---- Tests: selection change callback ----

static void test_callback_fires_on_selection_start(void)
{
    reset_cb_counts();
    TerminalBackend *term = create_test_term();
    terminal_set_selection_callback(term, tracking_selection_cb, NULL);

    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(cb_active_count, 1);
    ASSERT_EQ(cb_last_active, true);

    destroy_test_term(term);
}

static void test_callback_fires_on_selection_clear(void)
{
    reset_cb_counts();
    TerminalBackend *term = create_test_term();
    terminal_set_selection_callback(term, tracking_selection_cb, NULL);

    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    terminal_selection_clear(term);
    ASSERT_EQ(cb_inactive_count, 1);
    ASSERT_EQ(cb_last_active, false);

    destroy_test_term(term);
}

static void test_callback_not_fired_on_redundant_clear(void)
{
    reset_cb_counts();
    TerminalBackend *term = create_test_term();
    terminal_set_selection_callback(term, tracking_selection_cb, NULL);

    // No selection active — clear should be a no-op
    terminal_selection_clear(term);
    ASSERT_EQ(cb_inactive_count, 0);

    destroy_test_term(term);
}

// ---- Tests: callback-based pause/resume integration ----

static void test_callback_pause_resume_integration(void)
{
    mock_pause_count = 0;
    mock_resume_count = 0;
    TerminalBackend *term = create_test_term();
    terminal_set_selection_callback(term, mock_selection_cb, NULL);

    // Start selection → callback fires active=true → pause
    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 1);

    // Clear selection → callback fires active=false → resume
    terminal_selection_clear(term);
    ASSERT_EQ(mock_resume_count, 1);

    destroy_test_term(term);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("test_pty_pause\n");

    RUN_TEST(test_process_input_preserves_selection);
    RUN_TEST(test_callback_fires_on_selection_start);
    RUN_TEST(test_callback_fires_on_selection_clear);
    RUN_TEST(test_callback_not_fired_on_redundant_clear);
    RUN_TEST(test_callback_pause_resume_integration);

    TEST_SUMMARY();
}
