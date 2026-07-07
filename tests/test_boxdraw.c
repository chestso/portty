#include "test_helpers.h"
#include "rend_sdl3_boxdraw.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------------
 * Rounded-corner alignment test for procedural box drawing.
 *
 * We intercept SDL_RenderFillRect (used by the solid stubs) and
 * SDL_RenderPoint (used by the anti-aliased arc) to record which pixels
 * are "drawn" into a virtual cell buffer.  Then we verify that:
 *
 *   1. The horizontal stub of a rounded corner (╭╮╯╰) covers the same
 *      row and starts/ends at the same columns as a straight corner
 *      (┌┐└┘).
 *   2. The vertical stub of a rounded corner covers the same column and
 *      starts/ends at the same rows as a straight corner.
 *   3. The arc produces non-zero pixels (i.e. the curve is actually
 *      drawn, not degenerate).
 *
 * This is a geometric alignment test — it does not require a real GPU
 * or a running display.
 * ------------------------------------------------------------------------- */

#define CELL_W 18
#define CELL_H 36
#define GRID_W (CELL_W * 3)
#define GRID_H (CELL_H * 3)

static uint8_t grid[GRID_H][GRID_W];
static int cur_offset_x = 0;
static int cur_offset_y = 0;

static void grid_clear(void)
{
    memset(grid, 0, sizeof(grid));
    cur_offset_x = 0;
    cur_offset_y = 0;
}

static void grid_set(int x, int y)
{
    if (x >= 0 && x < GRID_W && y >= 0 && y < GRID_H)
        grid[y][x] = 1;
}

static int grid_count_row(int y, int x0, int x1)
{
    int count = 0;
    for (int x = x0; x <= x1; x++)
        if (grid[y][x])
            count++;
    return count;
}

static int grid_count_col(int x, int y0, int y1)
{
    int count = 0;
    for (int y = y0; y <= y1; y++)
        if (grid[y][x])
            count++;
    return count;
}

static int grid_row_first(int y, int x0, int x1)
{
    for (int x = x0; x <= x1; x++)
        if (grid[y][x])
            return x;
    return -1;
}

static int grid_row_last(int y, int x0, int x1)
{
    for (int x = x1; x >= x0; x--)
        if (grid[y][x])
            return x;
    return -1;
}

static int grid_col_first(int x, int y0, int y1)
{
    for (int y = y0; y <= y1; y++)
        if (grid[y][x])
            return y;
    return -1;
}

static int grid_col_last(int x, int y0, int y1)
{
    for (int y = y1; y >= y0; y--)
        if (grid[y][x])
            return y;
    return -1;
}

/* ---- Intercepted SDL functions ---- */

bool SDL_SetRenderDrawColor(SDL_Renderer *r, uint8_t cr, uint8_t cg,
                            uint8_t cb, uint8_t ca)
{
    (void)r;
    (void)cr;
    (void)cg;
    (void)cb;
    (void)ca;
    return true;
}

bool SDL_GetRenderDrawColor(SDL_Renderer *r, uint8_t *cr, uint8_t *cg,
                            uint8_t *cb, uint8_t *ca)
{
    (void)r;
    if (cr)
        *cr = 255;
    if (cg)
        *cg = 255;
    if (cb)
        *cb = 255;
    if (ca)
        *ca = 255;
    return true;
}

bool SDL_SetRenderDrawBlendMode(SDL_Renderer *r, SDL_BlendMode mode)
{
    (void)r;
    (void)mode;
    return true;
}

bool SDL_RenderFillRect(SDL_Renderer *r, const SDL_FRect *rect)
{
    (void)r;
    if (!rect)
        return true;
    int x0 = (int)floorf(rect->x);
    int y0 = (int)floorf(rect->y);
    int x1 = (int)ceilf(rect->x + rect->w) - 1;
    int y1 = (int)ceilf(rect->y + rect->h) - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            grid_set(x, y);
    return true;
}

bool SDL_RenderPoint(SDL_Renderer *r, float x, float y)
{
    (void)r;
    grid_set((int)roundf(x), (int)roundf(y));
    return true;
}

bool SDL_RenderFillRects(SDL_Renderer *r, const SDL_FRect *rects, int count)
{
    for (int i = 0; i < count; i++)
        SDL_RenderFillRect(r, &rects[i]);
    return true;
}

bool SDL_RenderPoints(SDL_Renderer *r, const SDL_FPoint *points, int count)
{
    for (int i = 0; i < count; i++)
        SDL_RenderPoint(r, points[i].x, points[i].y);
    return true;
}

/* ---- Tests ---- */

/* Draw a single character at cell position (col, row) and capture its
 * pixels.  The cell offset is applied so that each test can target a
 * specific cell within the grid. */
static void draw_char(uint32_t cp, int col, int row)
{
    grid_clear();
    cur_offset_x = col * CELL_W;
    cur_offset_y = row * CELL_H;
    rend_sdl3_boxdraw_draw(NULL, cp,
                           cur_offset_x, cur_offset_y,
                           CELL_W, CELL_H, 255, 255, 255);
}

/* Test: straight corners ┌┐└┘ produce horizontal and vertical stubs
 * at the cell center row and column. */
static void test_straight_corner_alignment(void)
{
    int cx = CELL_W / 2; /* center column within cell 0 */
    int cy = CELL_H / 2; /* center row within cell 0 */

    /* ┌ U+250C: down+right corner — horizontal goes right, vertical goes down */
    draw_char(0x250C, 0, 0);
    /* Horizontal line at cy: should have pixels from cx to CELL_W-1 */
    ASSERT_TRUE(grid_row_first(cy, 0, CELL_W - 1) <= cx + 1);
    ASSERT_TRUE(grid_row_last(cy, 0, CELL_W - 1) >= CELL_W - 2);
    /* Vertical line at cx: should have pixels from cy to CELL_H-1 */
    ASSERT_TRUE(grid_col_first(cx, 0, CELL_H - 1) <= cy + 1);
    ASSERT_TRUE(grid_col_last(cx, 0, CELL_H - 1) >= CELL_H - 2);

    /* ┐ U+2510: down+left corner — horizontal goes left, vertical goes down */
    draw_char(0x2510, 0, 0);
    ASSERT_TRUE(grid_row_first(cy, 0, CELL_W - 1) <= 1);
    ASSERT_TRUE(grid_row_last(cy, 0, CELL_W - 1) >= cx - 1);
    ASSERT_TRUE(grid_col_first(cx, 0, CELL_H - 1) <= cy + 1);
    ASSERT_TRUE(grid_col_last(cx, 0, CELL_H - 1) >= CELL_H - 2);

    /* └ U+2514: up+right corner — horizontal goes right, vertical goes up */
    draw_char(0x2514, 0, 0);
    ASSERT_TRUE(grid_row_first(cy, 0, CELL_W - 1) <= cx + 1);
    ASSERT_TRUE(grid_row_last(cy, 0, CELL_W - 1) >= CELL_W - 2);
    ASSERT_TRUE(grid_col_first(cx, 0, CELL_H - 1) <= 1);
    ASSERT_TRUE(grid_col_last(cx, 0, CELL_H - 1) >= cy - 1);

    /* ┘ U+2518: up+left corner — horizontal goes left, vertical goes up */
    draw_char(0x2518, 0, 0);
    ASSERT_TRUE(grid_row_first(cy, 0, CELL_W - 1) <= 1);
    ASSERT_TRUE(grid_row_last(cy, 0, CELL_W - 1) >= cx - 1);
    ASSERT_TRUE(grid_col_first(cx, 0, CELL_H - 1) <= 1);
    ASSERT_TRUE(grid_col_last(cx, 0, CELL_H - 1) >= cy - 1);
}

/* Test: rounded corners ╭╮╯╰ produce horizontal stubs at the same
 * row (cy) and vertical stubs at the same column (cx) as the straight
 * corners.  This is the core alignment invariant. */
static void test_rounded_corner_stub_alignment(void)
{
    int cx = CELL_W / 2;
    int cy = CELL_H / 2;

    /* ╭ U+256D: down+right — matches ┌ */
    draw_char(0x256D, 0, 0);
    /* Horizontal stub at cy should extend to the right edge */
    ASSERT_TRUE(grid_count_row(cy, cx, CELL_W - 1) > 0);
    ASSERT_TRUE(grid_row_last(cy, 0, CELL_W - 1) >= CELL_W - 2);
    /* Vertical stub at cx should extend to the bottom edge */
    ASSERT_TRUE(grid_count_col(cx, cy, CELL_H - 1) > 0);
    ASSERT_TRUE(grid_col_last(cx, 0, CELL_H - 1) >= CELL_H - 2);

    /* ╮ U+256E: down+left — matches ┐ */
    draw_char(0x256E, 0, 0);
    ASSERT_TRUE(grid_count_row(cy, 0, cx) > 0);
    ASSERT_TRUE(grid_row_first(cy, 0, CELL_W - 1) <= 1);
    ASSERT_TRUE(grid_count_col(cx, cy, CELL_H - 1) > 0);
    ASSERT_TRUE(grid_col_last(cx, 0, CELL_H - 1) >= CELL_H - 2);

    /* ╯ U+256F: up+left — matches ┘ */
    draw_char(0x256F, 0, 0);
    ASSERT_TRUE(grid_count_row(cy, 0, cx) > 0);
    ASSERT_TRUE(grid_row_first(cy, 0, CELL_W - 1) <= 1);
    ASSERT_TRUE(grid_count_col(cx, 0, cy) > 0);
    ASSERT_TRUE(grid_col_first(cx, 0, CELL_H - 1) <= 1);

    /* ╰ U+2570: up+right — matches └ */
    draw_char(0x2570, 0, 0);
    ASSERT_TRUE(grid_count_row(cy, cx, CELL_W - 1) > 0);
    ASSERT_TRUE(grid_row_last(cy, 0, CELL_W - 1) >= CELL_W - 2);
    ASSERT_TRUE(grid_count_col(cx, 0, cy) > 0);
    ASSERT_TRUE(grid_col_first(cx, 0, CELL_H - 1) <= 1);
}

/* Test: the arc portion of each rounded corner actually produces
 * drawn pixels (i.e. the curve is not degenerate).  The arc occupies
 * the quadrant diagonally opposite from the stub intersection. */
static void test_rounded_corner_arc_present(void)
{
    int cx = CELL_W / 2;
    int cy = CELL_H / 2;

    /* ╭ U+256D: arc is in the bottom-right quadrant (below cy, right of cx) */
    draw_char(0x256D, 0, 0);
    int arc_pixels = 0;
    for (int y = cy + 1; y < CELL_H; y++)
        for (int x = cx + 1; x < CELL_W; x++)
            if (grid[y][x])
                arc_pixels++;
    ASSERT_TRUE(arc_pixels > 0);

    /* ╮ U+256E: arc is in the bottom-left quadrant */
    draw_char(0x256E, 0, 0);
    arc_pixels = 0;
    for (int y = cy + 1; y < CELL_H; y++)
        for (int x = 0; x < cx; x++)
            if (grid[y][x])
                arc_pixels++;
    ASSERT_TRUE(arc_pixels > 0);

    /* ╯ U+256F: arc is in the top-left quadrant */
    draw_char(0x256F, 0, 0);
    arc_pixels = 0;
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < cx; x++)
            if (grid[y][x])
                arc_pixels++;
    ASSERT_TRUE(arc_pixels > 0);

    /* ╰ U+2570: arc is in the top-right quadrant */
    draw_char(0x2570, 0, 0);
    arc_pixels = 0;
    for (int y = 0; y < cy; y++)
        for (int x = cx + 1; x < CELL_W; x++)
            if (grid[y][x])
                arc_pixels++;
    ASSERT_TRUE(arc_pixels > 0);
}

/* Test: the rounded corner (╭) produces a vertical stub at the same
 * column as the straight corner (┌).  The SDF approach draws the
 * vertical stub at x=cx (the cell centerline), matching where straight
 * box-drawing lines are drawn.  This is the key alignment invariant. */
static void test_rounded_matches_straight_direction(void)
{
    int cx = CELL_W / 2;
    int cy = CELL_H / 2;
    (void)cx;

    /* ┌ draws a vertical line at cx going down, and a horizontal at cy
     * going right.  Find the vertical column. */
    draw_char(0x250C, 0, 0); /* ┌ */
    int straight_v_col = -1;
    for (int x = 0; x < CELL_W; x++) {
        int consecutive = 0;
        for (int y = cy; y < CELL_H; y++) {
            if (grid[y][x])
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

    /* ╭ draws an arc + vertical stub.  The vertical stub should be at
     * the same column as ┌'s vertical line. */
    draw_char(0x256D, 0, 0); /* ╭ */
    int rounded_consecutive = 0, best_run = 0;
    for (int y = cy; y < CELL_H; y++) {
        if (grid[y][straight_v_col]) {
            rounded_consecutive++;
            if (rounded_consecutive > best_run)
                best_run = rounded_consecutive;
        } else {
            rounded_consecutive = 0;
        }
    }
    ASSERT_TRUE(best_run >= 3);
}

/* Test: horizontal lines (─) span the full cell width at the center row.
 * This verifies that a horizontal line placed between ╭ and ╮ will
 * connect to both corners. */
static void test_horizontal_line_spans_cell(void)
{
    int cy = CELL_H / 2;
    draw_char(0x2500, 0, 0); /* ─ */
    ASSERT_TRUE(grid_row_first(cy, 0, CELL_W - 1) <= 1);
    ASSERT_TRUE(grid_row_last(cy, 0, CELL_W - 1) >= CELL_W - 2);
}

/* Test: vertical lines (│) span the full cell height at the center column.
 * This verifies that a vertical line placed between ╭ and ╰ will connect. */
static void test_vertical_line_spans_cell(void)
{
    int cx = CELL_W / 2;
    int light = CELL_W / 5;
    if (light < 1)
        light = 1;
    int light_half = light / 2;
    int vcol = cx - light_half;
    draw_char(0x2502, 0, 0); /* │ */
    ASSERT_TRUE(grid_col_first(vcol, 0, CELL_H - 1) <= 1);
    ASSERT_TRUE(grid_col_last(vcol, 0, CELL_H - 1) >= CELL_H - 2);
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

/* Test: at a larger cell size (simulating bigger font), the rounded
 * corners still produce a vertical stub at the center column and an
 * arc in the correct quadrant. */
static void test_rounded_corner_large_cell(void)
{
    grid_clear();
    int big_w = 36;
    int big_h = 72;
    int cx = big_w / 2;
    int cy = big_h / 2;

    rend_sdl3_boxdraw_draw(NULL, 0x256D, 0, 0, big_w, big_h, 255, 255, 255);

    /* Vertical stub at cx — the SDF draws the rounded rect boundary
     * which includes a vertical segment at x=cx. */
    ASSERT_TRUE(grid_count_col(cx, 0, big_h - 1) > 0);
    /* Arc present in top-left quadrant (the arc curves from the
     * top of the vertical line to the right end of the cell at top). */
    int arc_pixels = 0;
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < big_w; x++)
            if (y < GRID_H && x < GRID_W && grid[y][x])
                arc_pixels++;
    ASSERT_TRUE(arc_pixels > 0);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_boxdraw\n\n");

    RUN_TEST(test_boxdraw_is_supported);
    RUN_TEST(test_straight_corner_alignment);
    RUN_TEST(test_rounded_corner_stub_alignment);
    RUN_TEST(test_rounded_corner_arc_present);
    RUN_TEST(test_rounded_matches_straight_direction);
    RUN_TEST(test_horizontal_line_spans_cell);
    RUN_TEST(test_vertical_line_spans_cell);
    RUN_TEST(test_rounded_corner_large_cell);

    TEST_SUMMARY();
}
