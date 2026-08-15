/*
 * portty — Shift+Click selection extension tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "term.h"
#include "test_helpers.h"
#include <stdlib.h>
#include <string.h>

// ---- Mock terminal backend (same pattern as test_pty_pause.c) ----

static int mock_term_rows = 24;
static int mock_term_cols = 80;

static bool mock_is_altscreen(TerminalBackend *term)
{
    (void)term;
    return false;
}

static int mock_get_dimensions(TerminalBackend *term, int *rows, int *cols)
{
    (void)term;
    *rows = mock_term_rows;
    *cols = mock_term_cols;
    return 0;
}

static int mock_get_cell(TerminalBackend *term, int row, int col,
                         TerminalCell *cell)
{
    (void)term;
    (void)row;
    (void)col;
    memset(cell, 0, sizeof(*cell));
    cell->cp = ' ';
    return 0;
}

static int mock_get_scrollback_cell(TerminalBackend *term, int sb_row,
                                    int col, TerminalCell *cell)
{
    (void)term;
    (void)sb_row;
    (void)col;
    memset(cell, 0, sizeof(*cell));
    cell->cp = ' ';
    return 0;
}

static int mock_get_scrollback_lines(TerminalBackend *term)
{
    (void)term;
    return 0;
}

static int mock_process_input(TerminalBackend *term, const char *input,
                              size_t len)
{
    (void)term;
    (void)input;
    return (int)len;
}

static TerminalBackend mock_term_backend = {
    .name = "mock_term",
    .is_altscreen = mock_is_altscreen,
    .get_dimensions = mock_get_dimensions,
    .get_cell = mock_get_cell,
    .get_scrollback_cell = mock_get_scrollback_cell,
    .get_scrollback_lines = mock_get_scrollback_lines,
    .process_input = mock_process_input,
};

static TerminalBackend *create_mock_term(void)
{
    TerminalBackend *term = calloc(1, sizeof(TerminalBackend));
    *term = mock_term_backend;
    return term;
}

static void destroy_mock_term(TerminalBackend *term)
{
    free(term->selection.word_chars);
    free(term);
}

// ---- Tests ----

// Shift+Click beyond the end of a selection should extend the end,
// and the anchor should move to the fixed start.
static void test_extend_end_beyond_selection(void)
{
    TerminalBackend *term = create_mock_term();

    // Select cols 0-5 on row 0
    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 5);
    ASSERT_EQ(term->selection.start.col, 0);
    ASSERT_EQ(term->selection.end.col, 5);

    // Shift+Click at col 10 — extends end
    terminal_selection_extend(term, 0, 10);
    ASSERT_EQ(term->selection.start.col, 0);
    ASSERT_EQ(term->selection.end.col, 10);
    // Anchor should be at the fixed start (col 0)
    ASSERT_EQ(term->selection.anchor.col, 0);

    destroy_mock_term(term);
}

// Shift+Click before the start of a selection should extend the start,
// and the anchor should move to the fixed end.
static void test_extend_start_before_selection(void)
{
    TerminalBackend *term = create_mock_term();

    // Select cols 3-5 on row 0
    terminal_selection_start(term, 0, 5, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 3);
    ASSERT_EQ(term->selection.start.col, 3);
    ASSERT_EQ(term->selection.end.col, 5);

    // Shift+Click at col 0 — extends start backward
    terminal_selection_extend(term, 0, 0);
    ASSERT_EQ(term->selection.start.col, 0);
    ASSERT_EQ(term->selection.end.col, 5);
    // Anchor should be at the fixed end (col 5)
    ASSERT_EQ(term->selection.anchor.col, 5);

    destroy_mock_term(term);
}

// Shift+Click between start and end should move the closer endpoint.
static void test_extend_click_between_endpoints(void)
{
    TerminalBackend *term = create_mock_term();

    // Select cols 0-10 on row 0
    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 10);
    ASSERT_EQ(term->selection.start.col, 0);
    ASSERT_EQ(term->selection.end.col, 10);

    // Shift+Click at col 3 — closer to start (distance 3) than end (distance 7)
    terminal_selection_extend(term, 0, 3);
    ASSERT_EQ(term->selection.start.col, 3);
    ASSERT_EQ(term->selection.end.col, 10);
    // Anchor should be at the fixed end (col 10)
    ASSERT_EQ(term->selection.anchor.col, 10);

    destroy_mock_term(term);
}

// After extending, subsequent terminal_selection_update() should work
// correctly because the anchor was repositioned to the fixed endpoint.
static void test_extend_then_update_uses_new_anchor(void)
{
    TerminalBackend *term = create_mock_term();

    // Select cols 0-5 on row 0
    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 5);
    ASSERT_EQ(term->selection.start.col, 0);
    ASSERT_EQ(term->selection.end.col, 5);

    // Shift+Click at col 10 — extends end, anchor becomes start (col 0)
    terminal_selection_extend(term, 0, 10);
    ASSERT_EQ(term->selection.anchor.col, 0);
    ASSERT_EQ(term->selection.end.col, 10);

    // Now drag (update) to col 7 — should contract end to 7
    terminal_selection_update(term, 0, 7);
    ASSERT_EQ(term->selection.start.col, 0);
    ASSERT_EQ(term->selection.end.col, 7);

    destroy_mock_term(term);
}

// extend on inactive selection should be a no-op
static void test_extend_no_active_selection(void)
{
    TerminalBackend *term = create_mock_term();

    terminal_selection_extend(term, 0, 10);
    ASSERT_FALSE(terminal_selection_active(term));

    destroy_mock_term(term);
}

// extend across rows: click on a later row should extend end
static void test_extend_across_rows(void)
{
    TerminalBackend *term = create_mock_term();

    // Select row 0, cols 0-5
    terminal_selection_start(term, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(term, 0, 5);

    // Shift+Click at row 2, col 3 — extends end to row 2
    terminal_selection_extend(term, 2, 3);
    ASSERT_EQ(term->selection.start.row, 0);
    ASSERT_EQ(term->selection.start.col, 0);
    ASSERT_EQ(term->selection.end.row, 2);
    ASSERT_EQ(term->selection.end.col, 3);
    ASSERT_EQ(term->selection.anchor.row, 0);
    ASSERT_EQ(term->selection.anchor.col, 0);

    destroy_mock_term(term);
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
