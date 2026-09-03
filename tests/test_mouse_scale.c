/*
 * portty — Mouse coordinate scaling regression tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/* Regression test for the HiDPI mouse-scale contract in rend_common.h:
 * SDL input coordinates convert with the compositor's window_scale, while
 * cell metrics are sized by content_scale (= window_scale × --dpi-scale).
 * Mixing the two shifts every mouse hit by the user scale factor — the bug
 * that broke selection under --dpi-scale. */
#include "test_helpers.h"
#include "../src/rend_common.h"
#include <stdio.h>

/* Inline implementation matching portty_app.c */
static float portty_compute_content_scale(float system_scale, float user_scale)
{
    float scale = system_scale;
    if (user_scale != 1.0f && scale > 0.0f)
        scale *= user_scale;
    return scale > 0.0f ? scale : 1.0f;
}

/* Compute the mouse hit-test column the way portty_app_pixel_to_cell does:
 * logical mouse points → physical px (window_scale) → cell index (cells
 * sized by content_scale). `base_cell` is the unscaled cell width. */
static int mouse_hit_col(float logical_x, float window_scale, float user_scale,
                         int base_cell)
{
    float content_scale = portty_compute_content_scale(window_scale, user_scale);
    int cell_w = (int)(base_cell * content_scale + 0.5f);
    int px = (int)mouse_logical_to_px(logical_x, window_scale);
    return px / cell_w;
}

/* --- Tests --- */

/* --dpi-scale 2 on a 1x display: the original bug scenario. Every hit was
 * 2x off because mouse coords were converted with content_scale. */
static void test_mouse_user_scale_2_on_1x(void)
{
    /* 10 logical points into a 10-point-wide logical window of 40 cells,
     * each base cell 10 points wide at user scale 2. The cursor sits at the
     * middle of cell 2 (cells are 20 logical points wide to the user). */
    int col = mouse_hit_col(35.0f, 1.0f, 2.0f, 10);
    ASSERT_EQ(col, 1); // 35 pt * 1 = 35 px / (10 * 2) = cell 1
}

/* Same scenario, confirming hits track the pointer across the grid. */
static void test_mouse_user_scale_2_sweep(void)
{
    /* Cell k spans logical points [k*20, (k+1)*20) for the user. */
    ASSERT_EQ(mouse_hit_col(0.0f, 1.0f, 2.0f, 10), 0);
    ASSERT_EQ(mouse_hit_col(19.9f, 1.0f, 2.0f, 10), 0);
    ASSERT_EQ(mouse_hit_col(20.0f, 1.0f, 2.0f, 10), 1);
    ASSERT_EQ(mouse_hit_col(39.9f, 1.0f, 2.0f, 10), 1);
    ASSERT_EQ(mouse_hit_col(40.0f, 1.0f, 2.0f, 10), 2);
    ASSERT_EQ(mouse_hit_col(100.0f, 1.0f, 2.0f, 10), 5);
}

/* 1x user scale on a 2x hidpi display: content_scale == window_scale here,
 * so the old code was accidentally correct. Guard it stays correct. */
static void test_mouse_hidpi_no_user_scale(void)
{
    /* 2x display: 10 logical points = 20 physical px; cells are 20 px. */
    ASSERT_EQ(mouse_hit_col(10.0f, 2.0f, 1.0f, 10), 1);
    ASSERT_EQ(mouse_hit_col(19.9f, 2.0f, 1.0f, 10), 1);
    ASSERT_EQ(mouse_hit_col(20.0f, 2.0f, 1.0f, 10), 2);
}

/* Both scales at once: hidpi display AND --dpi-scale. */
static void test_mouse_hidpi_plus_user_scale(void)
{
    /* 2x display, --dpi-scale 2: cells are 40 px = 20 logical points, so
     * 40 logical points is the middle of cell 2. */
    ASSERT_EQ(mouse_hit_col(40.0f, 2.0f, 2.0f, 10), 2);
    ASSERT_EQ(mouse_hit_col(59.9f, 2.0f, 2.0f, 10), 2);
    ASSERT_EQ(mouse_hit_col(60.0f, 2.0f, 2.0f, 10), 3);
}

/* Fractional scales (e.g. 1.25x compositor, 1.5x user). */
static void test_mouse_fractional_scales(void)
{
    /* 1.25x display: 100 logical points = 125 px; cell = 10 * 1.875 = 19 px. */
    ASSERT_EQ(mouse_hit_col(100.0f, 1.25f, 1.5f, 10), 6); // 125 / 19 = 6
    ASSERT_EQ(mouse_hit_col(30.0f, 1.25f, 1.5f, 10), 1);  // 37 / 19 = 1
}

/* Baseline: no scaling anywhere. */
static void test_mouse_no_scaling(void)
{
    ASSERT_EQ(mouse_hit_col(0.0f, 1.0f, 1.0f, 10), 0);
    ASSERT_EQ(mouse_hit_col(9.9f, 1.0f, 1.0f, 10), 0);
    ASSERT_EQ(mouse_hit_col(10.0f, 1.0f, 1.0f, 10), 1);
    ASSERT_EQ(mouse_hit_col(79.0f, 1.0f, 1.0f, 10), 7);
}

/* The conversion helper itself: window_scale is applied, non-positive
 * window scales fall back to 1.0. */
static void test_mouse_logical_to_px_helper(void)
{
    ASSERT_FLOAT_NEAR(mouse_logical_to_px(10.0f, 2.0f), 20.0f, 0.001f);
    ASSERT_FLOAT_NEAR(mouse_logical_to_px(10.0f, 1.0f), 10.0f, 0.001f);
    ASSERT_FLOAT_NEAR(mouse_logical_to_px(10.0f, 0.0f), 10.0f, 0.001f);
    ASSERT_FLOAT_NEAR(mouse_logical_to_px(10.0f, -1.0f), 10.0f, 0.001f);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("Running mouse scale regression tests...\n");

    RUN_TEST(test_mouse_user_scale_2_on_1x);
    RUN_TEST(test_mouse_user_scale_2_sweep);
    RUN_TEST(test_mouse_hidpi_no_user_scale);
    RUN_TEST(test_mouse_hidpi_plus_user_scale);
    RUN_TEST(test_mouse_fractional_scales);
    RUN_TEST(test_mouse_no_scaling);
    RUN_TEST(test_mouse_logical_to_px_helper);

    TEST_SUMMARY();
}
