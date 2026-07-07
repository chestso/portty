#include "test_helpers.h"
#include "rend_sdl3_boxdraw.h"
#include "font.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Rounded-corner alignment test for procedural box drawing.
 *
 * We call rend_sdl3_boxdraw_render() which returns a GlyphBitmap (RGBA
 * pixel buffer).  We inspect the pixels directly to verify geometric
 * alignment between rounded corners (╭╮╯╰) and straight corners (┌┐└┘).
 * ------------------------------------------------------------------------- */

#define CELL_W 18
#define CELL_H 36

/* Check if a pixel in the bitmap is set (non-transparent). */
static bool px_set(const GlyphBitmap *bmp, int x, int y)
{
    if (x < 0 || x >= bmp->width || y < 0 || y >= bmp->height)
        return false;
    return bmp->pixels[(y * bmp->width + x) * 4 + 3] > 0;
}

static int row_count(const GlyphBitmap *bmp, int y, int x0, int x1)
{
    int count = 0;
    for (int x = x0; x <= x1; x++)
        if (px_set(bmp, x, y))
            count++;
    return count;
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
    return rend_sdl3_boxdraw_render(cp, CELL_W, CELL_H, 255, 255, 255);
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
    ASSERT_TRUE(rend_sdl3_boxdraw_is_supported(0x2500));
    ASSERT_TRUE(rend_sdl3_boxdraw_is_supported(0x256D));
    ASSERT_TRUE(rend_sdl3_boxdraw_is_supported(0x257F));
    ASSERT_TRUE(rend_sdl3_boxdraw_is_supported(0x2580));
    ASSERT_TRUE(rend_sdl3_boxdraw_is_supported(0x259F));
    ASSERT_FALSE(rend_sdl3_boxdraw_is_supported(0x24FF));
    ASSERT_FALSE(rend_sdl3_boxdraw_is_supported(0x25A0));
}

/* Test: rend_sdl3_boxdraw_render returns a valid bitmap. */
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
    int cx = CELL_W / 2;
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
    int cx = CELL_W / 2;
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

/* Test: at a larger cell size, rounded corners still produce proper
 * vertical stubs and arc pixels. */
static void test_rounded_corner_large_cell(void)
{
    int big_w = 36, big_h = 72;
    int cx = big_w / 2, cy = big_h / 2;

    GlyphBitmap *bmp = rend_sdl3_boxdraw_render(0x256D, big_w, big_h, 255, 255, 255);
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
    RUN_TEST(test_rounded_corner_large_cell);

    TEST_SUMMARY();
}
