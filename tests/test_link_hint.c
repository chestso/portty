/*
 * portty — OSC-8 link hint panel geometry tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_link_hint.c — unit tests for OSC-8 link hint panel geometry
 *
 * The hint is a full-width strip, so the only thing that varies with the
 * link is the row it lands on (above the link when that fits, below it
 * otherwise).
 */

#include "test_helpers.h"
#include "portty_panel.h"

static const int HINT_ROWS = 1 + PANEL_DECORATION_ROWS; /* 3 */

/* The strip spans the whole grid, in every placement. */
static void test_hint_spans_full_width(void)
{
    PanelRect r = panel_link_hint_rect(80, 24, 5);
    ASSERT_EQ(r.col, 0);
    ASSERT_EQ(r.cols, 80);

    /* Same width for a link in the right half — there is no right-aligned
     * short form anymore. */
    r = panel_link_hint_rect(80, 24, 70);
    ASSERT_EQ(r.col, 0);
    ASSERT_EQ(r.cols, 80);

    /* And on a narrow grid. */
    r = panel_link_hint_rect(20, 6, 3);
    ASSERT_EQ(r.col, 0);
    ASSERT_EQ(r.cols, 20);
}

static void test_hint_rows_are_one_content_row(void)
{
    PanelRect r = panel_link_hint_rect(80, 24, 5);
    ASSERT_EQ(r.rows, 3);
    ASSERT_EQ(r.rows, HINT_ROWS);
}

/* Link at row 5: the strip sits on rows 2..4, one row above the link. */
static void test_hint_above_link(void)
{
    PanelRect r = panel_link_hint_rect(80, 24, 5);
    ASSERT_EQ(r.row, 2);
    ASSERT_EQ(r.row + r.rows, 5);
}

/* Link at the top row has no room above, so the strip goes below it. */
static void test_hint_below_link_at_top_row(void)
{
    PanelRect r = panel_link_hint_rect(80, 24, 0);
    ASSERT_EQ(r.row, 1);
}

static void test_hint_below_link_at_row_one(void)
{
    PanelRect r = panel_link_hint_rect(80, 24, 1);
    ASSERT_EQ(r.row, 2);
}

/* A grid too short for either placement bottom-aligns the strip. */
static void test_hint_bottom_aligned_on_short_grid(void)
{
    PanelRect r = panel_link_hint_rect(80, 6, 2);
    ASSERT_EQ(r.row, 3);
    ASSERT_EQ(r.row + r.rows, 6);
}

static void test_hint_rows_clamp_to_grid_height(void)
{
    PanelRect r = panel_link_hint_rect(80, 2, 1);
    ASSERT_EQ(r.rows, 2);
    ASSERT_EQ(r.row, 0);
}

/* Link rows outside the grid clamp, so the strip still lands on a visible
 * row. */
static void test_hint_link_row_clamped_into_grid(void)
{
    PanelRect low = panel_link_hint_rect(80, 24, -5);
    PanelRect high = panel_link_hint_rect(80, 24, 999);
    ASSERT_EQ(low.row, 1);   /* clamped to row 0, so it goes below */
    ASSERT_EQ(high.row, 20); /* clamped to row 23, so it goes above */

    PanelRect one = panel_link_hint_rect(80, 1, 0);
    ASSERT_EQ(one.rows, 1);
    ASSERT_EQ(one.row, 0);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_hint_spans_full_width);
    RUN_TEST(test_hint_rows_are_one_content_row);
    RUN_TEST(test_hint_above_link);
    RUN_TEST(test_hint_below_link_at_top_row);
    RUN_TEST(test_hint_below_link_at_row_one);
    RUN_TEST(test_hint_bottom_aligned_on_short_grid);
    RUN_TEST(test_hint_rows_clamp_to_grid_height);
    RUN_TEST(test_hint_link_row_clamped_into_grid);

    TEST_SUMMARY();
}
