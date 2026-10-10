/*
 * portty — Box-drawing glyph alignment tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "test_helpers.h"
#include "rend_common.h"
#include "font.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Rounded-corner alignment test for procedural box drawing.
 *
 * We call rend_boxdraw_render() which returns a GlyphBitmap (RGBA
 * pixel buffer).  We inspect the pixels directly to verify geometric
 * alignment between rounded corners (╭╮╯╰) and straight corners (┌┐└┘).
 * ------------------------------------------------------------------------- */

#define CELL_W 18
#define CELL_H 36

/* Double-line geometry, derived the same way rend_boxdraw.c does for a
 * CELL_W x CELL_H cell: stroke thickness, sub-line offset from the center,
 * and the resulting sub-line rect edges.  A double line is a hollow "tube";
 * the band between its two sub-lines is the tube interior. */
#define LIGHT    (CELL_W / 5)
#define OFF      (LIGHT + (LIGHT + 1) / 2)
#define CELL_CX  (CELL_W / 2)
#define CELL_CY  (CELL_H / 2)
#define LV_LEFT  (CELL_CX - OFF - LIGHT / 2) /* left vertical sub-line (V1) */
#define LV_RIGHT (LV_LEFT + LIGHT)
#define RV_LEFT  (CELL_CX + OFF - LIGHT / 2) /* right vertical sub-line (V2) */
#define RV_RIGHT (RV_LEFT + LIGHT)
#define TH_TOP   (CELL_CY - OFF - LIGHT / 2) /* top horizontal sub-line (H1) */
#define TH_BOT   (TH_TOP + LIGHT)
#define BH_TOP   (CELL_CY + OFF - LIGHT / 2) /* bottom horizontal sub-line (H2) */
#define BH_BOT   (BH_TOP + LIGHT)

/* Check if a pixel in the bitmap is set (non-transparent). */
static bool px_set(const GlyphBitmap *bmp, int x, int y)
{
    if (x < 0 || x >= bmp->width || y < 0 || y >= bmp->height)
        return false;
    return bmp->pixels[(y * bmp->width + x) * 4 + 3] > 0;
}

/* Number of set pixels in the half-open rect [x0, x1) x [y0, y1). */
static int rect_set_count(const GlyphBitmap *bmp, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (px_set(bmp, x, y))
                n++;
    return n;
}

static int col_count(const GlyphBitmap *bmp, int x, int y0, int y1)
{
    int count = 0;
    for (int y = y0; y <= y1; y++)
        if (px_set(bmp, x, y))
            count++;
    return count;
}

static int row_first(const GlyphBitmap *bmp, int y, int x0, int x1)
{
    for (int x = x0; x <= x1; x++)
        if (px_set(bmp, x, y))
            return x;
    return -1;
}

static int row_last(const GlyphBitmap *bmp, int y, int x0, int x1)
{
    for (int x = x1; x >= x0; x--)
        if (px_set(bmp, x, y))
            return x;
    return -1;
}

static int col_first(const GlyphBitmap *bmp, int x, int y0, int y1)
{
    for (int y = y0; y <= y1; y++)
        if (px_set(bmp, x, y))
            return y;
    return -1;
}

static int col_last(const GlyphBitmap *bmp, int x, int y0, int y1)
{
    for (int y = y1; y >= y0; y--)
        if (px_set(bmp, x, y))
            return y;
    return -1;
}

static GlyphBitmap *draw_cp(uint32_t cp)
{
    return rend_boxdraw_render(cp, CELL_W, CELL_H, 255, 255, 255);
}

static void free_bmp(GlyphBitmap *bmp)
{
    if (bmp) {
        free(bmp->pixels);
        free(bmp);
    }
}

/* Test: box-drawing support detection. */
static void test_boxdraw_is_supported(void)
{
    ASSERT_TRUE(rend_boxdraw_is_supported(0x2500));
    ASSERT_TRUE(rend_boxdraw_is_supported(0x256D));
    ASSERT_TRUE(rend_boxdraw_is_supported(0x257F));
    ASSERT_TRUE(rend_boxdraw_is_supported(0x2580));
    ASSERT_TRUE(rend_boxdraw_is_supported(0x259F));
    ASSERT_FALSE(rend_boxdraw_is_supported(0x24FF));
    ASSERT_FALSE(rend_boxdraw_is_supported(0x25A0));
}

/* Test: rend_boxdraw_render returns a valid bitmap. */
static void test_render_returns_valid_bitmap(void)
{
    GlyphBitmap *bmp = draw_cp(0x2500); /* ─ */
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(bmp->width, CELL_W);
    ASSERT_EQ(bmp->height, CELL_H);
    ASSERT_TRUE(bmp->centered);
    ASSERT_NOT_NULL(bmp->pixels);
    free_bmp(bmp);
}

/* Test: straight corners ┌┐└┘ produce horizontal and vertical stubs
 * at the cell center row and column. */
static void test_straight_corner_alignment(void)
{
    int cx = CELL_W / 2;
    int cy = CELL_H / 2;

    /* ┌ U+250C: down+right corner */
    GlyphBitmap *bmp = draw_cp(0x250C);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(row_last(bmp, cy, 0, CELL_W - 1) >= CELL_W - 2);
    ASSERT_TRUE(col_last(bmp, cx, 0, CELL_H - 1) >= CELL_H - 2);
    free_bmp(bmp);

    /* ┐ U+2510: down+left corner */
    bmp = draw_cp(0x2510);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(row_first(bmp, cy, 0, CELL_W - 1) <= 1);
    ASSERT_TRUE(col_last(bmp, cx, 0, CELL_H - 1) >= CELL_H - 2);
    free_bmp(bmp);

    /* └ U+2514: up+right corner */
    bmp = draw_cp(0x2514);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(row_last(bmp, cy, 0, CELL_W - 1) >= CELL_W - 2);
    ASSERT_TRUE(col_first(bmp, cx, 0, CELL_H - 1) <= 1);
    free_bmp(bmp);

    /* ┘ U+2518: up+left corner */
    bmp = draw_cp(0x2518);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(row_first(bmp, cy, 0, CELL_W - 1) <= 1);
    ASSERT_TRUE(col_first(bmp, cx, 0, CELL_H - 1) <= 1);
    free_bmp(bmp);
}

/* Test: rounded corners ╭╮╯╰ produce vertical stubs at the same
 * column as the straight corners. */
static void test_rounded_corner_stub_alignment(void)
{
    int cx = CELL_W / 2;
    int cy = CELL_H / 2;

    /* ╭ U+256D: down+right — matches ┌ */
    GlyphBitmap *bmp = draw_cp(0x256D);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(col_count(bmp, cx, cy, CELL_H - 1) > 0);
    free_bmp(bmp);

    /* ╮ U+256E: down+left — matches ┐ */
    bmp = draw_cp(0x256E);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(col_count(bmp, cx, cy, CELL_H - 1) > 0);
    free_bmp(bmp);

    /* ╯ U+256F: up+left — matches ┘ */
    bmp = draw_cp(0x256F);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(col_count(bmp, cx, 0, cy) > 0);
    free_bmp(bmp);

    /* ╰ U+2570: up+right — matches └ */
    bmp = draw_cp(0x2570);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(col_count(bmp, cx, 0, cy) > 0);
    free_bmp(bmp);
}

/* Test: the arc portion of each rounded corner actually produces
 * drawn pixels (i.e. the curve is not degenerate). */
static void test_rounded_corner_arc_present(void)
{
    int cy = CELL_H / 2;

    /* ╭: arc curves from vertical stub to top-right of cell */
    GlyphBitmap *bmp = draw_cp(0x256D);
    ASSERT_NOT_NULL(bmp);
    int arc = 0;
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < CELL_W; x++)
            if (px_set(bmp, x, y))
                arc++;
    ASSERT_TRUE(arc > 0);
    free_bmp(bmp);

    /* ╮: arc curves from vertical stub to top-left of cell */
    bmp = draw_cp(0x256E);
    ASSERT_NOT_NULL(bmp);
    arc = 0;
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < CELL_W; x++)
            if (px_set(bmp, x, y))
                arc++;
    ASSERT_TRUE(arc > 0);
    free_bmp(bmp);

    /* ╯: arc in bottom portion of cell */
    bmp = draw_cp(0x256F);
    ASSERT_NOT_NULL(bmp);
    arc = 0;
    for (int y = cy; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++)
            if (px_set(bmp, x, y))
                arc++;
    ASSERT_TRUE(arc > 0);
    free_bmp(bmp);

    /* ╰: arc in bottom portion of cell */
    bmp = draw_cp(0x2570);
    ASSERT_NOT_NULL(bmp);
    arc = 0;
    for (int y = cy; y < CELL_H; y++)
        for (int x = 0; x < CELL_W; x++)
            if (px_set(bmp, x, y))
                arc++;
    ASSERT_TRUE(arc > 0);
    free_bmp(bmp);
}

/* Test: the rounded corner's vertical stub is at the same column as
 * the straight corner's vertical line. */
static void test_rounded_matches_straight_direction(void)
{
    int cy = CELL_H / 2;

    /* ┌: find the vertical line column */
    GlyphBitmap *bmp = draw_cp(0x250C);
    ASSERT_NOT_NULL(bmp);
    int straight_v_col = -1;
    for (int x = 0; x < CELL_W; x++) {
        int consecutive = 0;
        for (int y = cy; y < CELL_H; y++) {
            if (px_set(bmp, x, y))
                consecutive++;
            else
                break;
        }
        if (consecutive >= 3) {
            straight_v_col = x;
            break;
        }
    }
    ASSERT_TRUE(straight_v_col >= 0);
    free_bmp(bmp);

    /* ╭: check that the same column has a vertical run */
    bmp = draw_cp(0x256D);
    ASSERT_NOT_NULL(bmp);
    int best_run = 0, consecutive = 0;
    for (int y = cy; y < CELL_H; y++) {
        if (px_set(bmp, straight_v_col, y)) {
            consecutive++;
            if (consecutive > best_run)
                best_run = consecutive;
        } else {
            consecutive = 0;
        }
    }
    ASSERT_TRUE(best_run >= 3);
    free_bmp(bmp);
}

/* Test: horizontal lines (─) span the full cell width at the center row. */
static void test_horizontal_line_spans_cell(void)
{
    int cy = CELL_H / 2;
    GlyphBitmap *bmp = draw_cp(0x2500);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(row_first(bmp, cy, 0, CELL_W - 1) <= 1);
    ASSERT_TRUE(row_last(bmp, cy, 0, CELL_W - 1) >= CELL_W - 2);
    free_bmp(bmp);
}

/* Test: vertical lines (│) span the full cell height at the center column. */
static void test_vertical_line_spans_cell(void)
{
    int cx = CELL_W / 2;
    GlyphBitmap *bmp = draw_cp(0x2502);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(col_first(bmp, cx, 0, CELL_H - 1) <= 1);
    ASSERT_TRUE(col_last(bmp, cx, 0, CELL_H - 1) >= CELL_H - 2);
    free_bmp(bmp);
}

/* Test: double-line junctions are open.
 *
 * Where two double tubes cross, the through tube's wall is cut over the
 * crossing tube's interior, so the two interiors join instead of the wall
 * sealing the branch off.  A closed junction fills the band asserted empty
 * below. */
static void test_double_junction_open(void)
{
    GlyphBitmap *bmp;

    /* ╠ U+2560: vertical through, horizontal right — V2 is cut between the
     * two horizontals. */
    bmp = draw_cp(0x2560);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, RV_LEFT, TH_BOT, RV_RIGHT, BH_TOP), 0);
    free_bmp(bmp);

    /* ╣ U+2563: vertical through, horizontal left — V1 is cut. */
    bmp = draw_cp(0x2563);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, LV_LEFT, TH_BOT, LV_RIGHT, BH_TOP), 0);
    free_bmp(bmp);

    /* ╦ U+2566: horizontal through, vertical down — H2 is cut. */
    bmp = draw_cp(0x2566);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, LV_RIGHT, BH_TOP, RV_LEFT, BH_BOT), 0);
    free_bmp(bmp);

    /* ╩ U+2569: horizontal through, vertical up — H1 is cut. */
    bmp = draw_cp(0x2569);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, LV_RIGHT, TH_TOP, RV_LEFT, TH_BOT), 0);
    free_bmp(bmp);

    /* ╬ U+256C: both tubes through — all four walls are cut. */
    bmp = draw_cp(0x256C);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, LV_LEFT, TH_BOT, LV_RIGHT, BH_TOP), 0);
    ASSERT_EQ(rect_set_count(bmp, RV_LEFT, TH_BOT, RV_RIGHT, BH_TOP), 0);
    ASSERT_EQ(rect_set_count(bmp, LV_RIGHT, TH_TOP, RV_LEFT, TH_BOT), 0);
    ASSERT_EQ(rect_set_count(bmp, LV_RIGHT, BH_TOP, RV_LEFT, BH_BOT), 0);
    free_bmp(bmp);
}

/* Test: a single/heavy stub that joins a double tube reaches its wall with
 * no gap and without intruding into the tube interior. */
static void test_single_stub_joins_double_tube(void)
{
    GlyphBitmap *bmp;
    int vc_left = CELL_CX - LIGHT / 2; /* single vertical stub band */
    int hc_top = CELL_CY - LIGHT / 2;  /* single horizontal stub band */

    /* ╒ U+2552: down single + right double.  The stub spans from H1's top
     * edge to the cell bottom. */
    bmp = draw_cp(0x2552);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, vc_left, TH_TOP, vc_left + LIGHT, CELL_H),
              LIGHT * (CELL_H - TH_TOP));
    free_bmp(bmp);

    /* ╘ U+2558: up single + right double.  The stub spans from the cell top
     * down to H2's bottom edge. */
    bmp = draw_cp(0x2558);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, vc_left, 0, vc_left + LIGHT, BH_BOT),
              LIGHT * BH_BOT);
    free_bmp(bmp);

    /* ╓ U+2553: down double + right single.  The single horizontal starts at
     * V1's outer edge, capping both verticals. */
    bmp = draw_cp(0x2553);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(px_set(bmp, LV_LEFT, CELL_CY));
    ASSERT_FALSE(px_set(bmp, LV_LEFT - 1, CELL_CY));
    free_bmp(bmp);

    /* ╙ U+2559: up double + right single — same, mirrored. */
    bmp = draw_cp(0x2559);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(px_set(bmp, LV_LEFT, CELL_CY));
    ASSERT_FALSE(px_set(bmp, LV_LEFT - 1, CELL_CY));
    free_bmp(bmp);

    /* ╟ U+255F: vertical through + right single.  The stub branches off the
     * near wall (V2) and leaves the vertical interior open. */
    bmp = draw_cp(0x255F);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(px_set(bmp, RV_LEFT, CELL_CY));
    ASSERT_FALSE(px_set(bmp, RV_LEFT - 1, CELL_CY));
    free_bmp(bmp);

    /* ╢ U+2562: vertical through + left single — mirrored. */
    bmp = draw_cp(0x2562);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(px_set(bmp, LV_RIGHT - 1, CELL_CY));
    ASSERT_FALSE(px_set(bmp, LV_RIGHT, CELL_CY));
    free_bmp(bmp);

    /* ╤ U+2564: down single + horizontal double.  The stub hangs below H2
     * and leaves the horizontal interior open. */
    bmp = draw_cp(0x2564);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, vc_left, TH_BOT, vc_left + LIGHT, BH_TOP), 0);
    ASSERT_TRUE(px_set(bmp, CELL_CX, BH_BOT));
    free_bmp(bmp);

    /* ╧ U+2567: up single + horizontal double — mirrored. */
    bmp = draw_cp(0x2567);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, vc_left, TH_BOT, vc_left + LIGHT, BH_TOP), 0);
    ASSERT_TRUE(px_set(bmp, CELL_CX, TH_TOP - 1));
    free_bmp(bmp);

    /* ╞ U+255E: single vertical + right double.  The double horizontals
     * start at the single line's outer edge, so the corner is filled. */
    bmp = draw_cp(0x255E);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(px_set(bmp, vc_left, TH_TOP));
    ASSERT_FALSE(px_set(bmp, vc_left - 1, TH_TOP));
    ASSERT_EQ(rect_set_count(bmp, hc_top, TH_TOP, vc_left, TH_BOT), 0);
    free_bmp(bmp);
}

/* Test: double-line corners keep their nested outer/inner pairing.
 * The outer sub-lines (V1 + H1 for ╔) meet each other, as do the inner
 * sub-lines (V2 + H2), and the tube interiors stay hollow. */
static void test_double_corner_pairing(void)
{
    GlyphBitmap *bmp;

    /* ╔ U+2554: outer L = V1 + H1, inner L = V2 + H2. */
    bmp = draw_cp(0x2554);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, LV_LEFT, TH_TOP, CELL_W, TH_BOT),
              (CELL_W - LV_LEFT) * LIGHT); /* H1 from V1's outer edge */
    ASSERT_EQ(rect_set_count(bmp, LV_LEFT, TH_TOP, LV_RIGHT, CELL_H),
              LIGHT * (CELL_H - TH_TOP)); /* V1 from H1's top edge */
    ASSERT_EQ(rect_set_count(bmp, RV_LEFT, BH_TOP, CELL_W, BH_BOT),
              (CELL_W - RV_LEFT) * LIGHT); /* H2 from V2's outer edge */
    ASSERT_EQ(rect_set_count(bmp, RV_LEFT, BH_TOP, RV_RIGHT, CELL_H),
              LIGHT * (CELL_H - BH_TOP)); /* V2 from H2's top edge */
    ASSERT_EQ(rect_set_count(bmp, LV_RIGHT, TH_BOT, RV_LEFT, BH_TOP), 0);
    free_bmp(bmp);

    /* ╗ U+2557: outer L = V2 + H1, inner L = V1 + H2. */
    bmp = draw_cp(0x2557);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, 0, TH_TOP, RV_RIGHT, TH_BOT),
              RV_RIGHT * LIGHT); /* H1 to V2's outer edge */
    ASSERT_EQ(rect_set_count(bmp, RV_LEFT, TH_TOP, RV_RIGHT, CELL_H),
              LIGHT * (CELL_H - TH_TOP)); /* V2 from H1's top edge */
    ASSERT_EQ(rect_set_count(bmp, 0, BH_TOP, LV_RIGHT, BH_BOT),
              LV_RIGHT * LIGHT); /* H2 to V1's outer edge */
    ASSERT_EQ(rect_set_count(bmp, LV_LEFT, BH_TOP, LV_RIGHT, CELL_H),
              LIGHT * (CELL_H - BH_TOP)); /* V1 from H2's top edge */
    ASSERT_EQ(rect_set_count(bmp, LV_RIGHT, TH_BOT, RV_LEFT, BH_TOP), 0);
    free_bmp(bmp);

    /* ╚ U+255A: outer L = V1 + H2, inner L = V2 + H1. */
    bmp = draw_cp(0x255A);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, LV_LEFT, BH_TOP, CELL_W, BH_BOT),
              (CELL_W - LV_LEFT) * LIGHT); /* H2 from V1's outer edge */
    ASSERT_EQ(rect_set_count(bmp, LV_LEFT, 0, LV_RIGHT, BH_BOT),
              LIGHT * BH_BOT); /* V1 from the top to H2's bottom edge */
    ASSERT_EQ(rect_set_count(bmp, RV_LEFT, TH_TOP, CELL_W, TH_BOT),
              (CELL_W - RV_LEFT) * LIGHT); /* H1 from V2's outer edge */
    ASSERT_EQ(rect_set_count(bmp, RV_LEFT, 0, RV_RIGHT, TH_BOT),
              LIGHT * TH_BOT); /* V2 from the top to H1's bottom edge */
    ASSERT_EQ(rect_set_count(bmp, LV_RIGHT, TH_BOT, RV_LEFT, BH_TOP), 0);
    free_bmp(bmp);

    /* ╝ U+255D: outer L = V2 + H2, inner L = V1 + H1. */
    bmp = draw_cp(0x255D);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(rect_set_count(bmp, 0, BH_TOP, RV_RIGHT, BH_BOT),
              RV_RIGHT * LIGHT); /* H2 to V2's outer edge */
    ASSERT_EQ(rect_set_count(bmp, RV_LEFT, 0, RV_RIGHT, BH_BOT),
              LIGHT * BH_BOT); /* V2 from the top to H2's bottom edge */
    ASSERT_EQ(rect_set_count(bmp, 0, TH_TOP, LV_RIGHT, TH_BOT),
              LV_RIGHT * LIGHT); /* H1 to V1's outer edge */
    ASSERT_EQ(rect_set_count(bmp, LV_LEFT, 0, LV_RIGHT, TH_BOT),
              LIGHT * TH_BOT); /* V1 from the top to H1's bottom edge */
    ASSERT_EQ(rect_set_count(bmp, LV_RIGHT, TH_BOT, RV_LEFT, BH_TOP), 0);
    free_bmp(bmp);
}

/* Test: at a larger cell size, rounded corners still produce proper
 * vertical stubs and arc pixels. */
static void test_rounded_corner_large_cell(void)
{
    int big_w = 36, big_h = 72;
    int cx = big_w / 2, cy = big_h / 2;

    GlyphBitmap *bmp = rend_boxdraw_render(0x256D, big_w, big_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);
    ASSERT_TRUE(col_count(bmp, cx, 0, big_h - 1) > 0);

    int arc = 0;
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < big_w; x++)
            if (px_set(bmp, x, y))
                arc++;
    ASSERT_TRUE(arc > 0);
    free_bmp(bmp);
}

/* Test: diagonal bitmaps (╱╲╳) have 10% proportional margins.
 * Bitmap size should be cell_w * 1.2 x cell_h * 1.2 (cell + 2 * 10% margin). */
static void test_diagonal_proportional_margins(void)
{
    int cell_w = 32, cell_h = 96;

    /* ╱ U+2571 */
    GlyphBitmap *bmp = rend_boxdraw_render(0x2571, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);

    int expected_margin_x = (int)roundf((float)cell_w * 0.10f);
    int expected_margin_y = (int)roundf((float)cell_h * 0.10f);
    int expected_w = cell_w + 2 * expected_margin_x;
    int expected_h = cell_h + 2 * expected_margin_y;

    ASSERT_EQ(bmp->width, expected_w);
    ASSERT_EQ(bmp->height, expected_h);
    free_bmp(bmp);

    /* ╲ U+2572 */
    bmp = rend_boxdraw_render(0x2572, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(bmp->width, expected_w);
    ASSERT_EQ(bmp->height, expected_h);
    free_bmp(bmp);

    /* ╳ U+2573 */
    bmp = rend_boxdraw_render(0x2573, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);
    ASSERT_EQ(bmp->width, expected_w);
    ASSERT_EQ(bmp->height, expected_h);
    free_bmp(bmp);
}

/* Test: diagonal lines reach bitmap corners for seamless tiling.
 * When cells are stacked, the overhang from adjacent cells should meet
 * at the cell boundary. Lines must reach the bitmap edges. */
static void test_diagonal_lines_reach_bitmap_corners(void)
{
    int cell_w = 32, cell_h = 96;
    int margin_x = (int)roundf((float)cell_w * 0.10f);
    int margin_y = (int)roundf((float)cell_h * 0.10f);
    int bmp_w = cell_w + 2 * margin_x;
    int bmp_h = cell_h + 2 * margin_y;

    /* ╱ U+2571: bottom-left to top-right */
    GlyphBitmap *bmp = rend_boxdraw_render(0x2571, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);

    /* Check that line reaches near bottom-left corner */
    bool bottom_left_touched = false;
    for (int dx = 0; dx < 5 && !bottom_left_touched; dx++) {
        for (int dy = 0; dy < 5 && !bottom_left_touched; dy++) {
            if (px_set(bmp, dx, bmp_h - 1 - dy))
                bottom_left_touched = true;
        }
    }
    ASSERT_TRUE(bottom_left_touched);

    /* Check that line reaches near top-right corner */
    bool top_right_touched = false;
    for (int dx = 0; dx < 5 && !top_right_touched; dx++) {
        for (int dy = 0; dy < 5 && !top_right_touched; dy++) {
            if (px_set(bmp, bmp_w - 1 - dx, dy))
                top_right_touched = true;
        }
    }
    ASSERT_TRUE(top_right_touched);
    free_bmp(bmp);

    /* ╲ U+2572: top-left to bottom-right */
    bmp = rend_boxdraw_render(0x2572, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);

    /* Check that line reaches near top-left corner */
    bool top_left_touched = false;
    for (int dx = 0; dx < 5 && !top_left_touched; dx++) {
        for (int dy = 0; dy < 5 && !top_left_touched; dy++) {
            if (px_set(bmp, dx, dy))
                top_left_touched = true;
        }
    }
    ASSERT_TRUE(top_left_touched);

    /* Check that line reaches near bottom-right corner */
    bool bottom_right_touched = false;
    for (int dx = 0; dx < 5 && !bottom_right_touched; dx++) {
        for (int dy = 0; dy < 5 && !bottom_right_touched; dy++) {
            if (px_set(bmp, bmp_w - 1 - dx, bmp_h - 1 - dy))
                bottom_right_touched = true;
        }
    }
    ASSERT_TRUE(bottom_right_touched);
    free_bmp(bmp);
}

/* Test: diagonal bitmaps scale margins proportionally at different cell sizes.
 * 10% margin should work at various DPI/font sizes. */
static void test_diagonal_margins_scale_with_cell_size(void)
{
    /* Small cell (low DPI) */
    int cell_w = 10, cell_h = 22;
    GlyphBitmap *bmp = rend_boxdraw_render(0x2571, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);
    int margin_x = (int)roundf((float)cell_w * 0.10f);
    int margin_y = (int)roundf((float)cell_h * 0.10f);
    ASSERT_EQ(bmp->width, cell_w + 2 * margin_x);
    ASSERT_EQ(bmp->height, cell_h + 2 * margin_y);
    free_bmp(bmp);

    /* Medium cell (medium DPI) */
    cell_w = 20;
    cell_h = 44;
    bmp = rend_boxdraw_render(0x2571, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);
    margin_x = (int)roundf((float)cell_w * 0.10f);
    margin_y = (int)roundf((float)cell_h * 0.10f);
    ASSERT_EQ(bmp->width, cell_w + 2 * margin_x);
    ASSERT_EQ(bmp->height, cell_h + 2 * margin_y);
    free_bmp(bmp);

    /* Large cell (high DPI) */
    cell_w = 40;
    cell_h = 88;
    bmp = rend_boxdraw_render(0x2571, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);
    margin_x = (int)roundf((float)cell_w * 0.10f);
    margin_y = (int)roundf((float)cell_h * 0.10f);
    ASSERT_EQ(bmp->width, cell_w + 2 * margin_x);
    ASSERT_EQ(bmp->height, cell_h + 2 * margin_y);
    free_bmp(bmp);
}

/* Test: diagonal line slope matches the exact cell aspect ratio, not the
 * rounded bitmap aspect ratio.  The 10% margins are rounded to integers,
 * which shifts the bitmap's aspect ratio away from the cell's.  The line
 * direction must use the exact (unrounded) cell dimensions so that stacked
 * cells produce a seamless diagonal.
 *
 * We measure the rendered line's slope via least-squares regression through
 * all set pixels, then check it is closer to cell_h/cell_w than to
 * bmp_h/bmp_w.
 *
 * cell_w=12, cell_h=55 maximizes the rounding discrepancy:
 *   margin_x = round(1.2) = 1, margin_y = round(5.5) = 6
 *   bmp = 14 x 67,  bmp slope = 67/14 = 4.786
 *   cell slope = 55/12 = 4.583  ← what we want
 */
static void test_diagonal_slope_matches_cell_aspect_ratio(void)
{
    int cell_w = 12, cell_h = 55;

    /* ╱ U+2571: bottom-left to top-right (negative slope) */
    GlyphBitmap *bmp = rend_boxdraw_render(0x2571, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);

    /* Least-squares regression: slope = (N*Sxy - Sx*Sy) / (N*Sxx - Sx*Sx) */
    long n = 0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int y = 0; y < bmp->height; y++) {
        for (int x = 0; x < bmp->width; x++) {
            if (px_set(bmp, x, y)) {
                n++;
                sx += x;
                sy += y;
                sxx += (double)x * x;
                sxy += (double)x * y;
            }
        }
    }
    ASSERT_TRUE(n > 0);
    double denom = (double)n * sxx - sx * sx;
    ASSERT_TRUE(denom != 0.0);
    double measured_slope = ((double)n * sxy - sx * sy) / denom;
    double measured_abs = fabs(measured_slope);
    double cell_slope = (double)cell_h / (double)cell_w;
    double bmp_slope = (double)bmp->height / (double)bmp->width;
    double err_to_cell = fabs(measured_abs - cell_slope);
    double err_to_bmp = fabs(measured_abs - bmp_slope);
    ASSERT_TRUE(err_to_cell < err_to_bmp);
    free_bmp(bmp);

    /* ╲ U+2572: top-left to bottom-right (positive slope) */
    bmp = rend_boxdraw_render(0x2572, cell_w, cell_h, 255, 255, 255);
    ASSERT_NOT_NULL(bmp);

    n = 0;
    sx = 0;
    sy = 0;
    sxx = 0;
    sxy = 0;
    for (int y = 0; y < bmp->height; y++) {
        for (int x = 0; x < bmp->width; x++) {
            if (px_set(bmp, x, y)) {
                n++;
                sx += x;
                sy += y;
                sxx += (double)x * x;
                sxy += (double)x * y;
            }
        }
    }
    ASSERT_TRUE(n > 0);
    denom = (double)n * sxx - sx * sx;
    ASSERT_TRUE(denom != 0.0);
    measured_slope = ((double)n * sxy - sx * sy) / denom;
    measured_abs = fabs(measured_slope);
    err_to_cell = fabs(measured_abs - cell_slope);
    err_to_bmp = fabs(measured_abs - bmp_slope);
    ASSERT_TRUE(err_to_cell < err_to_bmp);
    free_bmp(bmp);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_boxdraw\n\n");

    RUN_TEST(test_boxdraw_is_supported);
    RUN_TEST(test_render_returns_valid_bitmap);
    RUN_TEST(test_straight_corner_alignment);
    RUN_TEST(test_rounded_corner_stub_alignment);
    RUN_TEST(test_rounded_corner_arc_present);
    RUN_TEST(test_rounded_matches_straight_direction);
    RUN_TEST(test_horizontal_line_spans_cell);
    RUN_TEST(test_vertical_line_spans_cell);
    RUN_TEST(test_double_junction_open);
    RUN_TEST(test_single_stub_joins_double_tube);
    RUN_TEST(test_double_corner_pairing);
    RUN_TEST(test_rounded_corner_large_cell);
    RUN_TEST(test_diagonal_proportional_margins);
    RUN_TEST(test_diagonal_lines_reach_bitmap_corners);
    RUN_TEST(test_diagonal_margins_scale_with_cell_size);
    RUN_TEST(test_diagonal_slope_matches_cell_aspect_ratio);

    TEST_SUMMARY();
}
