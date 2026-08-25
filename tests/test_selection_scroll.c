/*
 * portty — selection scroll adjustment and damage-aware clearing tests
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
    TerminalBackend *term = term_cfr_new(&cfg);
    if (term)
        cfr_set_scrollback_size(term_cfr_get_cfr_term(term), 100);
    return term;
}

static void destroy_test_term(TerminalBackend *term)
{
    terminal_destroy(term);
    free(term);
}

// ---- Tests: terminal_selection_adjust_scroll is now a no-op ----

static void test_adjust_scroll_is_noop(void)
{
    TerminalBackend *term = create_test_term();

    terminal_selection_start(term, 10, 5, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));

    // adjust_scroll is now a no-op — coffer handles scroll inline
    terminal_selection_adjust_scroll(term, 2);
    // Selection should still be active (no-op didn't clear it)
    ASSERT_TRUE(terminal_selection_active(term));

    destroy_test_term(term);
}

static void test_adjust_scroll_inactive_is_noop(void)
{
    TerminalBackend *term = create_test_term();

    terminal_selection_adjust_scroll(term, 5);
    ASSERT_FALSE(terminal_selection_active(term));

    destroy_test_term(term);
}

// ---- Tests: overlaps_damage is now always false ----

static void test_overlaps_damage_always_false(void)
{
    TerminalBackend *term = create_test_term();

    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_FALSE(terminal_selection_overlaps_damage(term, 5, 10, 5, 20));

    destroy_test_term(term);
}

static void test_overlaps_damage_inactive_is_false(void)
{
    TerminalBackend *term = create_test_term();

    ASSERT_FALSE(terminal_selection_overlaps_damage(term, 0, 0, 23, 79));

    destroy_test_term(term);
}

// ---- Tests: process_input no longer clears selection ----
// (coffer handles draw-clear inline)

static void test_process_input_draw_clears(void)
{
    TerminalBackend *term = create_test_term();

    // Write text, then select it
    CfrTerm *vt = term_cfr_get_cfr_term(term);
    cfr_input_write(vt, (const uint8_t *)"hello", 5);

    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));

    // Draw at same position — coffer clears inline
    terminal_process_input(term, "X", 1);
    ASSERT_FALSE(terminal_selection_active(term));

    destroy_test_term(term);
}

static void test_process_input_draw_outside_preserves(void)
{
    TerminalBackend *term = create_test_term();

    CfrTerm *vt = term_cfr_get_cfr_term(term);
    cfr_input_write(vt, (const uint8_t *)"hello", 5);

    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));

    // Move to row 5 and draw — selection on row 0 survives
    cfr_input_write(vt, (const uint8_t *)"\x1b[6;1HX", 7);
    ASSERT_TRUE(terminal_selection_active(term));

    destroy_test_term(term);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("test_selection_scroll\n");

    RUN_TEST(test_adjust_scroll_is_noop);
    RUN_TEST(test_adjust_scroll_inactive_is_noop);
    RUN_TEST(test_overlaps_damage_always_false);
    RUN_TEST(test_overlaps_damage_inactive_is_false);
    RUN_TEST(test_process_input_draw_clears);
    RUN_TEST(test_process_input_draw_outside_preserves);

    TEST_SUMMARY();
}
