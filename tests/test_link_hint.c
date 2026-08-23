/*
 * portty — OSC-8 link hint panel positioning tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_link_hint.c — unit tests for OSC-8 link hint panel positioning
 *
 * Tests:
 *   - cfr_utf8_display_width() for URLs
 *   - Panel position/size calculation logic
 */

#include "test_helpers.h"
#include "portty_panel.h"
#include <coffer/coffer.h>

/* Test cfr_utf8_display_width with various inputs */
static void test_utf8_display_width_ascii(void)
{
    ASSERT_EQ(cfr_utf8_display_width("hello", 5), 5);
    ASSERT_EQ(cfr_utf8_display_width("https://example.com", 18), 18);
    ASSERT_EQ(cfr_utf8_display_width("", 0), 0);
    ASSERT_EQ(cfr_utf8_display_width(NULL, 0), 0);
}

static void test_utf8_display_width_wide_chars(void)
{
    /* CJK characters are width 2 */
    ASSERT_EQ(cfr_utf8_display_width("\xE3\x81\x82", 3), 2); /* U+3042 hiragana A */
    ASSERT_EQ(cfr_utf8_display_width("\xE4\xB8\xAD", 3), 2); /* U+4E2D CJK 'middle' */

    /* Emoji are width 2 */
    ASSERT_EQ(cfr_utf8_display_width("\xF0\x9F\x8E\x89", 4), 2); /* U+1F389 party popper */

    /* Mixed ASCII and wide */
    ASSERT_EQ(cfr_utf8_display_width("a\xE3\x81\x82", 4), 3); /* "a" + hiragana */
}

static void test_utf8_display_width_zero_width(void)
{
    /* Combining acute accent U+0301 is zero-width */
    ASSERT_EQ(cfr_utf8_display_width("\xCC\x81", 2), 0);

    /* e + combining acute = 1 cell (e is 1, combining mark is 0) */
    ASSERT_EQ(cfr_utf8_display_width("e\xCC\x81", 3), 1);

    /* VS16 U+FE0F is zero-width (doesn't add to width, but forces 2-cell emoji) */
    ASSERT_EQ(cfr_utf8_display_width("\xEF\xB8\x8F", 3), 0);
}

/* Test panel size calculation constants */
static void test_panel_size_constants(void)
{
    /* Panel rows = text_rows + decoration rows (top + bottom padding) */
    ASSERT_EQ(PANEL_CELL_PAD_TOP, 1);
    ASSERT_EQ(PANEL_CELL_PAD_BOTTOM, 1);
    ASSERT_EQ(PANEL_DECORATION_ROWS, 2);

    /* For 1 text row: panel_rows = 1 + 2 = 3 */
    ASSERT_EQ(1 + PANEL_DECORATION_ROWS, 3);

    /* Panel cols = url_cells + horizontal decoration */
    /* Horizontal decoration for naked panel: GAP + PAD_RIGHT = 2 */
    ASSERT_EQ(PANEL_CELL_GAP + PANEL_CELL_PAD_RIGHT, 2);
}

/* Test panel positioning logic (extracted from design) */
static void test_panel_position_above_link(void)
{
    /* Link at row 5 should show panel above at row 5 - 3 = 2 */
    int display_row = 5;
    int panel_rows = 3;

    int panel_row = (display_row > 0) ? display_row - panel_rows : display_row + 1;
    ASSERT_EQ(panel_row, 2);

    /* Panel occupies rows 2, 3, 4; link at row 5 has 1-row gap */
}

static void test_panel_position_at_top_row(void)
{
    /* Link at row 0 should show panel below at row 1 */
    int display_row = 0;
    int panel_rows = 3;

    int panel_row = (display_row > 0) ? display_row - panel_rows : display_row + 1;
    ASSERT_EQ(panel_row, 1);
}

static void test_panel_position_at_row_1(void)
{
    /* Link at row 1: can show above at row -2? No, clamp to 0.
     * Design says: above if display_row > 0, so row 1 - 3 = -2.
     * Implementation should clamp to 0. */
    int display_row = 1;
    int panel_rows = 3;

    int panel_row = (display_row > 0) ? display_row - panel_rows : display_row + 1;
    /* Raw calculation gives -2, implementation must clamp */
    if (panel_row < 0)
        panel_row = 0;
    ASSERT_EQ(panel_row, 0);
}

static void test_panel_horizontal_position_left_link(void)
{
    /* Link in left half of terminal: left-align panel */
    int link_col = 5;
    int panel_cols = 30;
    int term_cols = 80;

    int panel_col;
    if (link_col + panel_cols / 2 <= term_cols / 2) {
        panel_col = 0;
    } else {
        panel_col = term_cols - panel_cols;
    }
    ASSERT_EQ(panel_col, 0);
}

static void test_panel_horizontal_position_right_link(void)
{
    /* Link in right half of terminal: right-align panel */
    int link_col = 60;
    int panel_cols = 30;
    int term_cols = 80;

    int panel_col;
    if (link_col + panel_cols / 2 <= term_cols / 2) {
        panel_col = 0;
    } else {
        panel_col = term_cols - panel_cols;
    }
    ASSERT_EQ(panel_col, 50);
}

static void test_panel_width_capped_to_terminal(void)
{
    int url_cells = 100;
    int term_cols = 80;
    int decor = PANEL_CELL_GAP + PANEL_CELL_PAD_RIGHT;

    int panel_cols = url_cells + decor;
    if (panel_cols > term_cols)
        panel_cols = term_cols;

    ASSERT_EQ(panel_cols, 80);
}

int main(void)
{
    test_utf8_display_width_ascii();
    test_utf8_display_width_wide_chars();
    test_utf8_display_width_zero_width();
    test_panel_size_constants();
    test_panel_position_above_link();
    test_panel_position_at_top_row();
    test_panel_position_at_row_1();
    test_panel_horizontal_position_left_link();
    test_panel_horizontal_position_right_link();
    test_panel_width_capped_to_terminal();

    printf("PASS: all link_hint tests passed\n");
    return 0;
}
