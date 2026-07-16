#include "test_helpers.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// Minimal reproduction of the decoration coalescing logic added to the
// Sokol backend. We cannot link backend_sokol.c without a GPU context, so
// this test exercises the run-coalescing algorithms in isolation.

#define MAX_RUNS 8

typedef struct
{
    unsigned int underline;
    bool strikethrough;
} MockAttrs;

typedef struct
{
    uint8_t r, g, b;
    bool is_default;
} MockColor;

typedef struct
{
    MockAttrs attrs;
    MockColor fg;
    MockColor ul;
    int width;
} MockCell;

// Run records emitted by the coalescing pass.
typedef struct
{
    int start;
    int end;
    unsigned int style;
    uint8_t color[3];
} Run;

static int run_count;
static Run runs[MAX_RUNS];

static void reset_runs(void)
{
    run_count = 0;
    memset(runs, 0, sizeof(runs));
}

static void emit_underline(int start, int end, unsigned int style,
                           const uint8_t color[3])
{
    ASSERT_TRUE(run_count < MAX_RUNS);
    runs[run_count].start = start;
    runs[run_count].end = end;
    runs[run_count].style = style;
    runs[run_count].color[0] = color[0];
    runs[run_count].color[1] = color[1];
    runs[run_count].color[2] = color[2];
    run_count++;
}

static void resolve_ul_color(const MockColor *ul, uint8_t out[3])
{
    if (ul->is_default) {
        out[0] = 0x6B;
        out[1] = 0x50;
        out[2] = 0xFF;
    } else {
        out[0] = ul->r;
        out[1] = ul->g;
        out[2] = ul->b;
    }
}

static void coalesce_underlines(const MockCell *cells, int n)
{
    int run_start = -1;
    int vis_run_end = 0;
    unsigned int run_style = 0;
    uint8_t run_color[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        const MockCell *cell = &cells[i];
        uint8_t color[3];
        resolve_ul_color(&cell->ul, color);
        bool same_run = (run_style != 0 &&
                         cell->attrs.underline == run_style &&
                         color[0] == run_color[0] &&
                         color[1] == run_color[1] &&
                         color[2] == run_color[2]);
        if (run_style != 0 && !same_run) {
            emit_underline(run_start, vis_run_end, run_style, run_color);
            run_style = 0;
        }
        if (cell->attrs.underline != 0 && run_style == 0) {
            run_start = i;
            run_style = cell->attrs.underline;
            run_color[0] = color[0];
            run_color[1] = color[1];
            run_color[2] = color[2];
        }
        vis_run_end = i + cell->width;
    }
    if (run_style != 0) {
        emit_underline(run_start, vis_run_end, run_style, run_color);
    }
}

static void emit_strike(int start, int end, const uint8_t color[3])
{
    ASSERT_TRUE(run_count < MAX_RUNS);
    runs[run_count].start = start;
    runs[run_count].end = end;
    runs[run_count].color[0] = color[0];
    runs[run_count].color[1] = color[1];
    runs[run_count].color[2] = color[2];
    run_count++;
}

static void coalesce_strikethroughs(const MockCell *cells, int n)
{
    int run_start = -1;
    int vis_run_end = 0;
    bool in_run = false;
    uint8_t run_color[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        const MockCell *cell = &cells[i];
        bool cs = cell->attrs.strikethrough;
        uint8_t cr[3] = { cell->fg.r, cell->fg.g, cell->fg.b };
        bool same_run = in_run && cs &&
                        cr[0] == run_color[0] &&
                        cr[1] == run_color[1] &&
                        cr[2] == run_color[2];
        if (in_run && !same_run) {
            emit_strike(run_start, vis_run_end, run_color);
            in_run = false;
        }
        if (cs && !in_run) {
            run_start = i;
            in_run = true;
            run_color[0] = cr[0];
            run_color[1] = cr[1];
            run_color[2] = cr[2];
        }
        vis_run_end = i + cell->width;
    }
    if (in_run) {
        emit_strike(run_start, vis_run_end, run_color);
    }
}

static MockCell make_cell(unsigned int ul, bool strike,
                          uint8_t r, uint8_t g, uint8_t b,
                          bool fg_default, uint8_t ul_r, uint8_t ul_g,
                          uint8_t ul_b, bool ul_default)
{
    MockCell cell = {
        .attrs = { .underline = ul, .strikethrough = strike },
        .fg = { r, g, b, fg_default },
        .ul = { ul_r, ul_g, ul_b, ul_default },
        .width = 1,
    };
    return cell;
}

static void test_underline_single_run(void)
{
    reset_runs();
    MockCell cells[] = {
        make_cell(1, false, 0, 0, 0, false, 0, 0, 0, true),
        make_cell(1, false, 0, 0, 0, false, 0, 0, 0, true),
        make_cell(1, false, 0, 0, 0, false, 0, 0, 0, true),
    };
    coalesce_underlines(cells, 3);
    ASSERT_EQ(run_count, 1);
    ASSERT_EQ(runs[0].start, 0);
    ASSERT_EQ(runs[0].end, 3);
    ASSERT_EQ(runs[0].style, 1);
}

static void test_underline_break_on_style(void)
{
    reset_runs();
    MockCell cells[] = {
        make_cell(1, false, 0, 0, 0, false, 0, 0, 0, true),
        make_cell(2, false, 0, 0, 0, false, 0, 0, 0, true),
        make_cell(2, false, 0, 0, 0, false, 0, 0, 0, true),
    };
    coalesce_underlines(cells, 3);
    ASSERT_EQ(run_count, 2);
    ASSERT_EQ(runs[0].style, 1);
    ASSERT_EQ(runs[0].end, 1);
    ASSERT_EQ(runs[1].style, 2);
    ASSERT_EQ(runs[1].start, 1);
    ASSERT_EQ(runs[1].end, 3);
}

static void test_underline_break_on_color(void)
{
    reset_runs();
    MockCell cells[] = {
        make_cell(3, false, 0, 0, 0, false, 0xFF, 0, 0, false),
        make_cell(3, false, 0, 0, 0, false, 0x00, 0xFF, 0, false),
        make_cell(3, false, 0, 0, 0, false, 0x00, 0xFF, 0, false),
    };
    coalesce_underlines(cells, 3);
    ASSERT_EQ(run_count, 2);
    ASSERT_EQ(runs[0].start, 0);
    ASSERT_EQ(runs[0].end, 1);
    ASSERT_EQ(runs[0].color[0], 0xFF);
    ASSERT_EQ(runs[1].start, 1);
    ASSERT_EQ(runs[1].end, 3);
    ASSERT_EQ(runs[1].color[1], 0xFF);
}

static void test_underline_default_color(void)
{
    reset_runs();
    MockCell cells[] = {
        make_cell(1, false, 0, 0, 0, false, 0, 0, 0, true),
    };
    coalesce_underlines(cells, 1);
    ASSERT_EQ(run_count, 1);
    ASSERT_EQ(runs[0].color[0], 0x6B);
    ASSERT_EQ(runs[0].color[1], 0x50);
    ASSERT_EQ(runs[0].color[2], 0xFF);
}

static void test_strikethrough_run(void)
{
    reset_runs();
    MockCell cells[] = {
        make_cell(0, true, 0x12, 0x34, 0x56, false, 0, 0, 0, true),
        make_cell(0, true, 0x12, 0x34, 0x56, false, 0, 0, 0, true),
        make_cell(0, false, 0x12, 0x34, 0x56, false, 0, 0, 0, true),
    };
    coalesce_strikethroughs(cells, 3);
    ASSERT_EQ(run_count, 1);
    ASSERT_EQ(runs[0].start, 0);
    ASSERT_EQ(runs[0].end, 2);
    ASSERT_EQ(runs[0].color[0], 0x12);
}

static void test_strikethrough_break_on_color(void)
{
    reset_runs();
    MockCell cells[] = {
        make_cell(0, true, 0xAA, 0, 0, false, 0, 0, 0, true),
        make_cell(0, true, 0x00, 0xBB, 0, false, 0, 0, 0, true),
        make_cell(0, true, 0x00, 0xBB, 0, false, 0, 0, 0, true),
    };
    coalesce_strikethroughs(cells, 3);
    ASSERT_EQ(run_count, 2);
    ASSERT_EQ(runs[0].end, 1);
    ASSERT_EQ(runs[0].color[0], 0xAA);
    ASSERT_EQ(runs[1].start, 1);
    ASSERT_EQ(runs[1].end, 3);
    ASSERT_EQ(runs[1].color[1], 0xBB);
}

int main(int argc, char **argv)
{
    test_parse_args(argc, argv);
    RUN_TEST(test_underline_single_run);
    RUN_TEST(test_underline_break_on_style);
    RUN_TEST(test_underline_break_on_color);
    RUN_TEST(test_underline_default_color);
    RUN_TEST(test_strikethrough_run);
    RUN_TEST(test_strikethrough_break_on_color);
    TEST_SUMMARY();
}
