#include "test_helpers.h"
#include "../src/rend_common.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
static void test_nf_translate_known_mappings(void);
static void test_downscale_output_fits_cell_box(void);

static void test_nf_translate_known_mappings(void)
{
    ASSERT_EQ(rend_nf_translate_codepoint(0xF900), 0xF0401);
    ASSERT_EQ(rend_nf_translate_codepoint(0xF901), 0xF0402);
    ASSERT_EQ(rend_nf_translate_codepoint(0xFAFF), 0xF0600);
}

static void test_nf_translate_outside_range_unchanged(void)
{
    ASSERT_EQ(rend_nf_translate_codepoint(0xF899), 0xF899);
    ASSERT_EQ(rend_nf_translate_codepoint(0xFB00), 0xFB00);
    ASSERT_EQ(rend_nf_translate_codepoint(0x41), 0x41);
    ASSERT_EQ(rend_nf_translate_codepoint(0x1F600), 0x1F600);
}

static void test_nf_translate_unmapped_in_range_unchanged(void)
{
    // 0xF907 is skipped in the map (not present between 0xF906 and 0xF908)
    ASSERT_EQ(rend_nf_translate_codepoint(0xF907), 0xF907);
}

static void test_srgb_roundtrip(void)
{
    // Black and white should round-trip exactly
    ASSERT_EQ(rend_linear_to_srgb(rend_srgb_to_linear(0)), 0);
    ASSERT_EQ(rend_linear_to_srgb(rend_srgb_to_linear(255)), 255);

    // A mid-gray value should round-trip within rounding error (<=1)
    uint8_t mid = 128;
    uint8_t out = rend_linear_to_srgb(rend_srgb_to_linear(mid));
    int diff = out > mid ? out - mid : mid - out;
    ASSERT_TRUE(diff <= 1);
}

static void test_srgb_linear_monotonic(void)
{
    float prev = rend_srgb_to_linear(0);
    for (int i = 1; i <= 255; i++) {
        float cur = rend_srgb_to_linear((uint8_t)i);
        ASSERT_TRUE(cur > prev);
        prev = cur;
    }
}

static void test_display_row_to_unified_no_scroll(void)
{
    // With scroll_offset 0, display rows map 1:1 to visible rows.
    ASSERT_EQ(rend_display_row_to_unified(0, 0), 0);
    ASSERT_EQ(rend_display_row_to_unified(0, 10), 10);
    ASSERT_EQ(rend_display_row_to_unified(0, 23), 23);
}

static void test_display_row_to_unified_with_scrollback(void)
{
    // scroll_offset 3 means 3 scrollback rows are visible above the viewport.
    // display_row 0 is the topmost visible row, which is scrollback row 2.
    ASSERT_EQ(rend_display_row_to_unified(3, 0), -3);
    ASSERT_EQ(rend_display_row_to_unified(3, 1), -2);
    ASSERT_EQ(rend_display_row_to_unified(3, 2), -1);
    // After scrollback is exhausted, display rows map to visible rows starting at 0.
    ASSERT_EQ(rend_display_row_to_unified(3, 3), 0);
    ASSERT_EQ(rend_display_row_to_unified(3, 4), 1);
}

static void test_clamp_pixel_to_viewport(void)
{
    int x = -5, y = -10;
    rend_clamp_pixel_to_viewport(&x, &y, 100, 50);
    ASSERT_EQ(x, 0);
    ASSERT_EQ(y, 0);

    x = 150;
    y = 60;
    rend_clamp_pixel_to_viewport(&x, &y, 100, 50);
    ASSERT_EQ(x, 99);
    ASSERT_EQ(y, 49);

    x = 50;
    y = 25;
    rend_clamp_pixel_to_viewport(&x, &y, 100, 50);
    ASSERT_EQ(x, 50);
    ASSERT_EQ(y, 25);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_nf_translate_known_mappings);
    RUN_TEST(test_nf_translate_outside_range_unchanged);
    RUN_TEST(test_nf_translate_unmapped_in_range_unchanged);
    RUN_TEST(test_srgb_roundtrip);
    RUN_TEST(test_srgb_linear_monotonic);
    RUN_TEST(test_display_row_to_unified_no_scroll);
    RUN_TEST(test_display_row_to_unified_with_scrollback);
    RUN_TEST(test_clamp_pixel_to_viewport);
    RUN_TEST(test_downscale_output_fits_cell_box);

    TEST_SUMMARY();
}

// The downscaler contract: result bitmap always fits within (max_w, max_h).
// The +0.5 round can push a stray pixel above the box for awkward source
// ratios; the clamp in rend_common.c keeps it honest so backend placement
// math doesn't have to defend against overflow.
static void test_downscale_output_fits_cell_box(void)
{
    int targets[][2] = {
        { 16, 17 }, // square-ish
        { 8, 16 },  // tall
        { 16, 8 },  // wide
        { 1, 1 },   // minimum (degenerate)
        { 64, 65 }, // odd aspect
    };
    int source_sizes[][2] = {
        { 128, 256 }, // 4x emoji pipeline
        { 64, 64 },
        { 33, 33 }, // prime-numbered source — round-up hazard
        { 7, 11 },  // tiny primes
    };
    for (int ti = 0; ti < (int)(sizeof(targets) / sizeof(targets[0])); ti++) {
        int mw = targets[ti][0], mh = targets[ti][1];
        for (int si = 0; si < (int)(sizeof(source_sizes) / sizeof(source_sizes[0])); si++) {
            int sw = source_sizes[si][0], sh = source_sizes[si][1];
            GlyphBitmap src = { 0 };
            src.width = sw;
            src.height = sh;
            src.pixels = calloc((size_t)sw * sh * 4, 1);
            for (int p = 0; p < sw * sh; p++) {
                src.pixels[p * 4 + 0] = 200;
                src.pixels[p * 4 + 3] = 200;
            }
            GlyphBitmap *out = rend_downscale_bitmap(&src, mw, mh);
            if (out) {
                ASSERT_TRUE(out->width <= mw);
                ASSERT_TRUE(out->height <= mh);
                free(out->pixels);
                free(out);
            }
            free(src.pixels);
        }
    }
}
