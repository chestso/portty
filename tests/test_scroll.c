/*
 * portty — Mouse-wheel scroll logic tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_scroll — tests for the consolidated mouse-wheel scroll logic.
 *
 * Two concerns are tested:
 *
 * 1. Sub-tick accumulation (platform layer)
 *    SDL3 reports wheel deltas as floats. Trackpads produce fractional
 *    values (e.g. 0.1 per event). We accumulate until a whole tick is
 *    reached before dispatching, so slow trackpad scrolling is not
 *    silently dropped.
 *
 * 2. Scroll dispatch decision tree (on_scroll in main.c)
 *    Given a number of whole ticks, the dispatch must:
 *    - Route to pager when pager is active
 *    - Convert to arrow keys in altscreen (no mouse mode)
 *    - Scroll scrollback on the primary screen
 *    - Not be called at all when mouse mode is active (on_mouse consumes)
 *
 * The accumulation and dispatch logic is replicated inline here to match
 * the test pattern used by test_cursor_render.c and test_clipboard_deferred.c.
 */

#include "test_helpers.h"

#include <stdbool.h>
#include <math.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Constants (mirrors common.h)                                        */
/* ------------------------------------------------------------------ */

#define SCROLL_LINES_PER_TICK 1

/* ------------------------------------------------------------------ */
/* Sub-tick accumulator (mirrors the wheel handler in platform_sdl3.c) */
/* ------------------------------------------------------------------ */

typedef struct
{
    float accum;
} WheelAccumulator;

/*
 * Feed a float wheel delta into the accumulator. Returns the number of
 * whole ticks to dispatch and subtracts them from the accumulator,
 * preserving the fractional remainder.
 *
 * Truncation toward zero matches (int)float cast semantics, so negative
 * deltas (scroll down) work symmetrically with positive ones.
 */
static int wheel_accum_feed(WheelAccumulator *wa, float dy)
{
    wa->accum += dy;
    int ticks = (int)wa->accum; /* truncates toward zero */
    wa->accum -= (float)ticks;
    return ticks;
}

/* ------------------------------------------------------------------ */
/* Scroll dispatch (mirrors on_scroll in main.c)                       */
/* ------------------------------------------------------------------ */

typedef enum
{
    SCROLL_RESULT_SCROLLBACK,
    SCROLL_RESULT_ALTSCREEN_ARROWS,
    SCROLL_RESULT_PAGER,
} ScrollResult;

typedef struct
{
    bool pager_active;
    bool in_altscreen;
    int mouse_mode; /* 0 = none, >0 = active */
} ScrollCtx;

/*
 * Decide what a batch of whole wheel ticks should do.
 *
 * Returns a ScrollResult so the test can verify the routing without
 * needing real terminal/renderer/pager backends.
 */
static ScrollResult scroll_dispatch(const ScrollCtx *ctx, int ticks)
{
    (void)ticks;
    if (ctx->pager_active)
        return SCROLL_RESULT_PAGER;

    if (ctx->in_altscreen && ctx->mouse_mode == 0)
        return SCROLL_RESULT_ALTSCREEN_ARROWS;

    return SCROLL_RESULT_SCROLLBACK;
}

/*
 * Compute the number of lines to scroll for scrollback/pager paths.
 * With SCROLL_LINES_PER_TICK = 1, this is 1:1 with ticks. The function
 * exists so the multiplier lives in exactly one place.
 */
static int scroll_lines(int ticks)
{
    return ticks * SCROLL_LINES_PER_TICK;
}

/*
 * Compute the number of arrow-key presses for the altscreen path.
 * Uses the same multiplier as scroll_lines so both paths scale identically.
 */
static int scroll_arrow_count(int ticks)
{
    int lines = scroll_lines(abs(ticks));
    if (lines < 1)
        lines = 1;
    return lines;
}

/* ------------------------------------------------------------------ */
/* Accumulator tests                                                   */
/* ------------------------------------------------------------------ */

static void test_accum_single_whole_tick(void)
{
    WheelAccumulator wa = { 0 };
    int ticks = wheel_accum_feed(&wa, 1.0f);
    ASSERT_EQ(ticks, 1);
    ASSERT_EQ(wa.accum, 0.0f);
}

static void test_accum_single_negative_tick(void)
{
    WheelAccumulator wa = { 0 };
    int ticks = wheel_accum_feed(&wa, -1.0f);
    ASSERT_EQ(ticks, -1);
    ASSERT_EQ(wa.accum, 0.0f);
}

static void test_accum_sub_tick_drops_to_zero(void)
{
    /* The old code: (int)0.5f == 0, event silently dropped. */
    WheelAccumulator wa = { 0 };
    int ticks = wheel_accum_feed(&wa, 0.5f);
    ASSERT_EQ(ticks, 0);
    /* Remainder is preserved, not lost */
    ASSERT_TRUE(wa.accum > 0.0f);
}

static void test_accum_sub_tick_accumulates(void)
{
    /* 10 x 0.1f should produce 1 tick with ~0 remainder */
    WheelAccumulator wa = { 0 };
    int total = 0;
    for (int i = 0; i < 10; i++)
        total += wheel_accum_feed(&wa, 0.1f);
    ASSERT_EQ(total, 1);
    /* Remainder should be near zero (float rounding tolerance) */
    ASSERT_TRUE(fabsf(wa.accum) < 0.01f);
}

static void test_accum_slow_negative_accumulates(void)
{
    /* Slow scroll-down: 5 x -0.2f should produce -1 tick */
    WheelAccumulator wa = { 0 };
    int total = 0;
    for (int i = 0; i < 5; i++)
        total += wheel_accum_feed(&wa, -0.2f);
    ASSERT_EQ(total, -1);
}

static void test_accum_mixed_sizes(void)
{
    /* 0.3 + 0.3 + 0.3 = 0.9 (no tick), + 0.3 = 1.2 (1 tick, 0.2 remainder) */
    WheelAccumulator wa = { 0 };
    ASSERT_EQ(wheel_accum_feed(&wa, 0.3f), 0);
    ASSERT_EQ(wheel_accum_feed(&wa, 0.3f), 0);
    ASSERT_EQ(wheel_accum_feed(&wa, 0.3f), 0);
    int ticks = wheel_accum_feed(&wa, 0.3f);
    ASSERT_EQ(ticks, 1);
    ASSERT_TRUE(fabsf(wa.accum - 0.2f) < 0.01f);
}

static void test_accum_large_delta_multi_tick(void)
{
    /* Fast mouse wheel: 3.0f in one event = 3 ticks */
    WheelAccumulator wa = { 0 };
    int ticks = wheel_accum_feed(&wa, 3.0f);
    ASSERT_EQ(ticks, 3);
    ASSERT_EQ(wa.accum, 0.0f);
}

static void test_accum_large_negative_multi_tick(void)
{
    WheelAccumulator wa = { 0 };
    int ticks = wheel_accum_feed(&wa, -2.0f);
    ASSERT_EQ(ticks, -2);
    ASSERT_EQ(wa.accum, 0.0f);
}

static void test_accum_remainder_carries_across_batches(void)
{
    /* 0.6 (0 ticks, 0.6 rem) + 0.6 (1 tick, 0.2 rem) + 0.6 (0 ticks, 0.8 rem)
     * + 0.6 (1 tick, 0.4 rem) */
    WheelAccumulator wa = { 0 };
    ASSERT_EQ(wheel_accum_feed(&wa, 0.6f), 0);
    ASSERT_EQ(wheel_accum_feed(&wa, 0.6f), 1);
    ASSERT_EQ(wheel_accum_feed(&wa, 0.6f), 0);
    ASSERT_EQ(wheel_accum_feed(&wa, 0.6f), 1);
}

static void test_accum_zero_delta_no_op(void)
{
    WheelAccumulator wa = { 0 };
    wa.accum = 0.5f;
    ASSERT_EQ(wheel_accum_feed(&wa, 0.0f), 0);
    ASSERT_TRUE(fabsf(wa.accum - 0.5f) < 0.01f);
}

/* ------------------------------------------------------------------ */
/* Dispatch tests                                                      */
/* ------------------------------------------------------------------ */

static void test_dispatch_primary_screen_scrollback(void)
{
    ScrollCtx ctx = { .pager_active = false, .in_altscreen = false, .mouse_mode = 0 };
    ASSERT_EQ(scroll_dispatch(&ctx, 1), SCROLL_RESULT_SCROLLBACK);
}

static void test_dispatch_altscreen_no_mouse_arrows(void)
{
    ScrollCtx ctx = { .pager_active = false, .in_altscreen = true, .mouse_mode = 0 };
    ASSERT_EQ(scroll_dispatch(&ctx, 1), SCROLL_RESULT_ALTSCREEN_ARROWS);
}

static void test_dispatch_pager_intercepts(void)
{
    /* Pager is modal — it takes priority over everything */
    ScrollCtx ctx = { .pager_active = true, .in_altscreen = true, .mouse_mode = 0 };
    ASSERT_EQ(scroll_dispatch(&ctx, 1), SCROLL_RESULT_PAGER);
}

static void test_dispatch_pager_intercepts_primary(void)
{
    ScrollCtx ctx = { .pager_active = true, .in_altscreen = false, .mouse_mode = 0 };
    ASSERT_EQ(scroll_dispatch(&ctx, 1), SCROLL_RESULT_PAGER);
}

static void test_dispatch_mouse_mode_not_handled_by_scroll(void)
{
    /* When mouse mode is active, on_mouse returns true (consumed) and
     * on_scroll is never called. But if it were called, the dispatch
     * should fall through to scrollback, not arrows — the app owns the
     * pointer. This test documents that scroll_dispatch does not need
     * to handle the mouse-mode case because on_mouse consumes first. */
    ScrollCtx ctx = { .pager_active = false, .in_altscreen = true, .mouse_mode = 1 };
    /* In altscreen + mouse mode, on_mouse forwards to terminal and
     * returns true. on_scroll is not called. But if we did call it,
     * it should not produce arrows (mouse_mode != 0). */
    ASSERT_NEQ(scroll_dispatch(&ctx, 1), SCROLL_RESULT_ALTSCREEN_ARROWS);
}

static void test_dispatch_negative_ticks_scrollback(void)
{
    ScrollCtx ctx = { .pager_active = false, .in_altscreen = false, .mouse_mode = 0 };
    ASSERT_EQ(scroll_dispatch(&ctx, -3), SCROLL_RESULT_SCROLLBACK);
}

static void test_dispatch_negative_ticks_altscreen(void)
{
    ScrollCtx ctx = { .pager_active = false, .in_altscreen = true, .mouse_mode = 0 };
    ASSERT_EQ(scroll_dispatch(&ctx, -3), SCROLL_RESULT_ALTSCREEN_ARROWS);
}

/* ------------------------------------------------------------------ */
/* Line-count tests                                                    */
/* ------------------------------------------------------------------ */

static void test_scroll_lines_one_per_tick(void)
{
    ASSERT_EQ(scroll_lines(1), 1);
    ASSERT_EQ(scroll_lines(3), 3);
    ASSERT_EQ(scroll_lines(-2), -2);
}

static void test_scroll_arrow_count_matches_lines(void)
{
    ASSERT_EQ(scroll_arrow_count(1), 1);
    ASSERT_EQ(scroll_arrow_count(3), 3);
}

static void test_scroll_arrow_count_negative_uses_abs(void)
{
    /* Arrow count is always positive; direction is chosen by caller */
    ASSERT_EQ(scroll_arrow_count(-1), 1);
    ASSERT_EQ(scroll_arrow_count(-3), 3);
}

static void test_scroll_arrow_count_zero_clamped(void)
{
    /* Defensive: zero ticks should not produce zero arrows if called */
    ASSERT_EQ(scroll_arrow_count(0), 1);
}

/* ------------------------------------------------------------------ */
/* Integration: accumulator feeds dispatch                             */
/* ------------------------------------------------------------------ */

static void test_integration_slow_scroll_produces_scrollback(void)
{
    WheelAccumulator wa = { 0 };
    ScrollCtx ctx = { .pager_active = false, .in_altscreen = false, .mouse_mode = 0 };
    int total_lines = 0;

    /* Simulate 20 x 0.1f trackpad events = 2 ticks = 2 lines */
    for (int i = 0; i < 20; i++) {
        int ticks = wheel_accum_feed(&wa, 0.1f);
        if (ticks != 0) {
            ASSERT_EQ(scroll_dispatch(&ctx, ticks), SCROLL_RESULT_SCROLLBACK);
            total_lines += scroll_lines(ticks);
        }
    }
    ASSERT_EQ(total_lines, 2);
}

static void test_integration_slow_scroll_altscreen_arrows(void)
{
    WheelAccumulator wa = { 0 };
    ScrollCtx ctx = { .pager_active = false, .in_altscreen = true, .mouse_mode = 0 };
    int total_arrows = 0;

    for (int i = 0; i < 15; i++) {
        int ticks = wheel_accum_feed(&wa, 0.1f);
        if (ticks != 0) {
            ASSERT_EQ(scroll_dispatch(&ctx, ticks), SCROLL_RESULT_ALTSCREEN_ARROWS);
            total_arrows += scroll_arrow_count(ticks);
        }
    }
    /* 15 x 0.1 = 1.5 → 1 tick → 1 arrow */
    ASSERT_EQ(total_arrows, 1);
}

static void test_integration_fast_scroll_multi_tick(void)
{
    WheelAccumulator wa = { 0 };
    ScrollCtx ctx = { .pager_active = false, .in_altscreen = false, .mouse_mode = 0 };
    int total_lines = 0;

    /* Fast wheel: 2.0f in one event */
    int ticks = wheel_accum_feed(&wa, 2.0f);
    ASSERT_EQ(ticks, 2);
    ASSERT_EQ(scroll_dispatch(&ctx, ticks), SCROLL_RESULT_SCROLLBACK);
    total_lines = scroll_lines(ticks);
    ASSERT_EQ(total_lines, 2);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    /* Accumulator */
    RUN_TEST(test_accum_single_whole_tick);
    RUN_TEST(test_accum_single_negative_tick);
    RUN_TEST(test_accum_sub_tick_drops_to_zero);
    RUN_TEST(test_accum_sub_tick_accumulates);
    RUN_TEST(test_accum_slow_negative_accumulates);
    RUN_TEST(test_accum_mixed_sizes);
    RUN_TEST(test_accum_large_delta_multi_tick);
    RUN_TEST(test_accum_large_negative_multi_tick);
    RUN_TEST(test_accum_remainder_carries_across_batches);
    RUN_TEST(test_accum_zero_delta_no_op);

    /* Dispatch */
    RUN_TEST(test_dispatch_primary_screen_scrollback);
    RUN_TEST(test_dispatch_altscreen_no_mouse_arrows);
    RUN_TEST(test_dispatch_pager_intercepts);
    RUN_TEST(test_dispatch_pager_intercepts_primary);
    RUN_TEST(test_dispatch_mouse_mode_not_handled_by_scroll);
    RUN_TEST(test_dispatch_negative_ticks_scrollback);
    RUN_TEST(test_dispatch_negative_ticks_altscreen);

    /* Line counts */
    RUN_TEST(test_scroll_lines_one_per_tick);
    RUN_TEST(test_scroll_arrow_count_matches_lines);
    RUN_TEST(test_scroll_arrow_count_negative_uses_abs);
    RUN_TEST(test_scroll_arrow_count_zero_clamped);

    /* Integration */
    RUN_TEST(test_integration_slow_scroll_produces_scrollback);
    RUN_TEST(test_integration_slow_scroll_altscreen_arrows);
    RUN_TEST(test_integration_fast_scroll_multi_tick);

    TEST_SUMMARY();
}
