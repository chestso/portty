/*
 * portty — Shift+Click selection extension tests
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
    return term_cfr_new(&cfg);
}

static void destroy_test_term(TerminalBackend *term)
{
    terminal_destroy(term);
    free(term);
}

#define SEL_START_COL(term)  (cfr_selection_get(term_cfr_get_cfr_term(term))->start.col)
#define SEL_END_COL(term)    (cfr_selection_get(term_cfr_get_cfr_term(term))->end.col)
#define SEL_START_ROW(term)  (cfr_selection_get(term_cfr_get_cfr_term(term))->start.row)
#define SEL_END_ROW(term)    (cfr_selection_get(term_cfr_get_cfr_term(term))->end.row)
#define SEL_ANCHOR_COL(term) (cfr_selection_get(term_cfr_get_cfr_term(term))->anchor.col)
#define SEL_ANCHOR_ROW(term) (cfr_selection_get(term_cfr_get_cfr_term(term))->anchor.row)

static void test_extend_end_beyond_selection(void)
{
    TerminalBackend *term = create_test_term();

    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 5);
    ASSERT_EQ(SEL_START_COL(term), 0);
    ASSERT_EQ(SEL_END_COL(term), 5);

    terminal_selection_extend(term, 0, 10);
    ASSERT_EQ(SEL_START_COL(term), 0);
    ASSERT_EQ(SEL_END_COL(term), 10);
    ASSERT_EQ(SEL_ANCHOR_COL(term), 0);

    destroy_test_term(term);
}

static void test_extend_start_before_selection(void)
{
    TerminalBackend *term = create_test_term();

    terminal_selection_start(term, 0, 5, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 3);
    ASSERT_EQ(SEL_START_COL(term), 3);
    ASSERT_EQ(SEL_END_COL(term), 5);

    terminal_selection_extend(term, 0, 0);
    ASSERT_EQ(SEL_START_COL(term), 0);
    ASSERT_EQ(SEL_END_COL(term), 5);
    ASSERT_EQ(SEL_ANCHOR_COL(term), 5);

    destroy_test_term(term);
}

static void test_extend_click_between_endpoints(void)
{
    TerminalBackend *term = create_test_term();

    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 10);
    ASSERT_EQ(SEL_START_COL(term), 0);
    ASSERT_EQ(SEL_END_COL(term), 10);

    terminal_selection_extend(term, 0, 3);
    ASSERT_EQ(SEL_START_COL(term), 3);
    ASSERT_EQ(SEL_END_COL(term), 10);
    ASSERT_EQ(SEL_ANCHOR_COL(term), 10);

    destroy_test_term(term);
}

static void test_extend_then_update_uses_new_anchor(void)
{
    TerminalBackend *term = create_test_term();

    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 5);
    ASSERT_EQ(SEL_START_COL(term), 0);
    ASSERT_EQ(SEL_END_COL(term), 5);

    terminal_selection_extend(term, 0, 10);
    ASSERT_EQ(SEL_ANCHOR_COL(term), 0);
    ASSERT_EQ(SEL_END_COL(term), 10);

    terminal_selection_update(term, 0, 7);
    ASSERT_EQ(SEL_START_COL(term), 0);
    ASSERT_EQ(SEL_END_COL(term), 7);

    destroy_test_term(term);
}

static void test_extend_no_active_selection(void)
{
    TerminalBackend *term = create_test_term();

    terminal_selection_extend(term, 0, 10);
    ASSERT_FALSE(terminal_selection_active(term));

    destroy_test_term(term);
}

static void test_extend_across_rows(void)
{
    TerminalBackend *term = create_test_term();

    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 5);

    terminal_selection_extend(term, 2, 3);
    ASSERT_EQ(SEL_START_ROW(term), 0);
    ASSERT_EQ(SEL_START_COL(term), 0);
    ASSERT_EQ(SEL_END_ROW(term), 2);
    ASSERT_EQ(SEL_END_COL(term), 3);
    ASSERT_EQ(SEL_ANCHOR_ROW(term), 0);
    ASSERT_EQ(SEL_ANCHOR_COL(term), 0);

    destroy_test_term(term);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("test_selection_extend\n");

    RUN_TEST(test_extend_end_beyond_selection);
    RUN_TEST(test_extend_start_before_selection);
    RUN_TEST(test_extend_click_between_endpoints);
    RUN_TEST(test_extend_then_update_uses_new_anchor);
    RUN_TEST(test_extend_no_active_selection);
    RUN_TEST(test_extend_across_rows);

    TEST_SUMMARY();
}
