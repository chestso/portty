/*
 * portty — Shared GPU-agnostic renderer helper tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "test_helpers.h"
#include "../src/rend_common.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations */
static void test_nf_translate_known_mappings(void);
static void test_downscale_output_fits_cell_box(void);
static void test_apply_glyph_layout_passthrough(void);
static void test_apply_glyph_layout_center_only(void);
static void test_apply_glyph_layout_downscale_centered(void);
static void test_resolve_cell_style_normal_default(void);
static void test_resolve_cell_style_bold_italic_cascade(void);
static void test_resolve_cell_style_bold_loads_when_only_bold_present(void);
static void test_resolve_cell_style_emoji_overrides_bold(void);
static void test_resolve_cell_style_emoji_falls_back_when_glyph_missing(void);
static void test_plan_glyph_sets_emoji_flags(void);
static void test_plan_glyph_emoji_square_clamps_avail(void);
static void test_plan_glyph_regional_passes_through(void);
static void test_plan_glyph_box_matches_cell(void);
static void test_plan_glyph_box_wide_fallback_keeps_width_fit(void);
static void test_plan_glyph_box_symbol_class_height_only(void);
static void test_plan_glyph_box_dingbat_is_symbol_class(void);
static void test_plan_glyph_box_math_operator_keeps_width_fit(void);

/* Content-scale comparison — decides whether a display-scale change needs
 * a font reload. */
static void test_content_scale_changed(void);
static void test_content_scale_unchanged(void);

/* Fallback route cache — the covering font and its glyph id are resolved
 * once per codepoint. */
static void test_route_resolved_once_per_codepoint(void);
static void test_route_negative_entry_cached(void);
static void test_route_survives_face_eviction(void);
static void test_route_dies_with_state(void);

/* Helpers exported by font_stubs.c for testing the policy functions in
 * rend_common.c that consult the FontBackend for style/emoji decisions. */
FontBackend make_test_font_backend(void);
void test_font_set_loaded_styles(uint32_t mask);
void test_font_reset(void);
bool test_font_get_box(FontStyle style, int *width, int *height,
                       bool *height_only_fit);
void test_font_set_glyph(uint32_t cp, uint32_t glyph_id);
void test_resolve_add_route(uint32_t cp, const char *path);
int test_resolve_query_count(void);
void test_resolve_reset(void);

/* Helper: a letter 'A' trivially falls through the bold/italic cascade. */
#define TEST_LATIN_A ((uint32_t)'A')
/* Helper: lightning emoji, which the test stubs claim is in the emoji font. */
#define TEST_EMOJI_CP ((uint32_t)0x26A1)

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

static void test_live_row_to_display_row_no_scroll(void)
{
    // Without scrolling, live rows map 1:1 to display rows.
    ASSERT_EQ(rend_live_row_to_display_row(0, 0), 0);
    ASSERT_EQ(rend_live_row_to_display_row(0, 5), 5);
    ASSERT_EQ(rend_live_row_to_display_row(0, 23), 23);
}

static void test_live_row_to_display_row_partial_scroll(void)
{
    // Scrolled back 1 line: a live row still on screen shifts down by 1.
    // This is the regression case for `clear` leaving the prompt on row 0
    // with scrollback preserved — the cursor must stay visible.
    ASSERT_EQ(rend_live_row_to_display_row(1, 0), 1);
    ASSERT_EQ(rend_live_row_to_display_row(1, 4), 5);
}

static void test_live_row_to_display_row_scrolled_out(void)
{
    // A live row pushed above the viewport by scrolling is not visible.
    ASSERT_EQ(rend_live_row_to_display_row(1, -1), -1);
    ASSERT_EQ(rend_live_row_to_display_row(3, -2), -1);
    // Row -1 of the live screen can never be scrolled into view.
    ASSERT_EQ(rend_live_row_to_display_row(0, -1), -1);
}

static void test_scroll_clamp_after_scrollback_purge(void)
{
    // RIS (`reset`) / ED 3 purge scrollback; the stored offset must clamp
    // down to the new count instead of pointing past the end (blank rows).
    RendScrollState st = { 0 };
    st.scroll_offset = 5;
    rend_scroll_clamp(&st, NULL); // stub: scrollback lines = 0
    ASSERT_EQ(st.scroll_offset, 0);
    // No-op when the offset is already within bounds.
    st.scroll_offset = 0;
    rend_scroll_clamp(&st, NULL);
    ASSERT_EQ(st.scroll_offset, 0);
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

/* Helper: a letter 'A' trivially falls through the bold/italic cascade. */
#define TEST_LATIN_A ((uint32_t)'A')
/* Helper: ⚡ which the test stubs claim is in the emoji font. */
#define TEST_EMOJI_CP ((uint32_t)0x26A1)

static void test_resolve_cell_style_normal_default(void)
{
    // No SGR bits, no emoji glyph: stays on NORMAL.
    FontBackend fb = make_test_font_backend();
    test_font_set_loaded_styles(0); // nothing loaded — all queries false
    uint32_t cps[] = { TEST_LATIN_A };
    RendCellStyle s = rend_resolve_cell_style(&fb, false, false, cps, 1);
    ASSERT_EQ(s.style, FONT_STYLE_NORMAL);
    ASSERT_FALSE(s.use_emoji);
    test_font_reset();
}

static void test_resolve_cell_style_bold_italic_cascade(void)
{
    // bold+italic SGR with only NORMAL loaded: cascade prefers NORMAL
    // (no BOLD_ITALIC, no BOLD, no ITALIC). Confirms the cascade tolerates
    // a partial backend install.
    FontBackend fb = make_test_font_backend();
    test_font_set_loaded_styles(1u << FONT_STYLE_NORMAL);
    uint32_t cps[] = { TEST_LATIN_A };
    RendCellStyle s = rend_resolve_cell_style(&fb, true, true, cps, 1);
    ASSERT_EQ(s.style, FONT_STYLE_NORMAL);
    ASSERT_FALSE(s.use_emoji);
    test_font_reset();
}

static void test_resolve_cell_style_bold_loads_when_only_bold_present(void)
{
    // Cascade should grab BOLD when BOLD_ITALIC isn't loaded but BOLD is.
    test_font_reset();
    test_font_set_loaded_styles((1u << FONT_STYLE_NORMAL) | (1u << FONT_STYLE_BOLD));
    FontBackend fb = make_test_font_backend();
    uint32_t cps[] = { TEST_LATIN_A };
    RendCellStyle s = rend_resolve_cell_style(&fb, true, true, cps, 1);
    ASSERT_EQ(s.style, FONT_STYLE_BOLD);
}

static void test_resolve_cell_style_emoji_overrides_bold(void)
{
    // Even when the SGR cascade picks BOLD, an emoji-presentation
    // codepoint whose char also lives in the emoji font forces
    // FONT_STYLE_EMOJI. The stub claims ⚡ has a glyph in emoji.
    test_font_reset();
    test_font_set_loaded_styles((1u << FONT_STYLE_NORMAL) |
                                (1u << FONT_STYLE_BOLD) |
                                (1u << FONT_STYLE_EMOJI));
    FontBackend fb = make_test_font_backend();
    uint32_t cps[] = { TEST_EMOJI_CP };
    RendCellStyle s = rend_resolve_cell_style(&fb, true, false, cps, 1);
    ASSERT_EQ(s.style, FONT_STYLE_EMOJI);
    ASSERT_TRUE(s.use_emoji);
}

static void test_resolve_cell_style_emoji_falls_back_when_glyph_missing(void)
{
    // Emoji font loaded but the emoji codepoint has no glyph in the
    // stub (returns 0). Helper stays on whatever the cascade picked.
    FontBackend fb = make_test_font_backend();
    test_font_set_loaded_styles((1u << FONT_STYLE_NORMAL) | (1u << FONT_STYLE_EMOJI));
    uint32_t cps[] = { TEST_LATIN_A }; // not in the emoji stub
    RendCellStyle s = rend_resolve_cell_style(&fb, false, false, cps, 1);
    ASSERT_EQ(s.style, FONT_STYLE_NORMAL);
    ASSERT_FALSE(s.use_emoji);
    test_font_reset();
}

static void test_plan_glyph_sets_emoji_flags(void)
{
    // For an emoji-style glyph that's color-baked, downscale_glyph must
    // be true (the 4x emoji pipeline is the only place that may resample)
    // and center_horizontally false.
    FontBackend fb = make_test_font_backend();
    test_font_set_loaded_styles(1u << FONT_STYLE_EMOJI);
    uint32_t cps[] = { TEST_EMOJI_CP };
    RendGlyphPlan p = { 0 };
    rend_plan_glyph(&fb, FONT_STYLE_EMOJI, true, cps, 1,
                    8, 16, 1, /*fg*/ 255, 128, 64, &p);
    ASSERT_TRUE(p.downscale_glyph);
    ASSERT_FALSE(p.center_horizontally);
    ASSERT_TRUE(p.color_baked);
    ASSERT_EQ(p.render_r, 255);
    ASSERT_EQ(p.render_g, 128);
    ASSERT_EQ(p.render_b, 64);
    ASSERT_TRUE(p.color_key != 0);
    test_font_reset();
}

static void test_plan_glyph_emoji_square_clamps_avail(void)
{
    // avail_w is columns_to_consume * cell_w; for emoji the helper
    // clamps avail_w to the smaller axis so we get a square atlas entry.
    // cell_w = 14, cell_h = 24, columns_to_consume = 2 -> avail_w starts
    // at 28. avail_h = 24. expect avail_w clamped down to 24.
    FontBackend fb = make_test_font_backend();
    test_font_set_loaded_styles(1u << FONT_STYLE_EMOJI);
    uint32_t cps[] = { TEST_EMOJI_CP };
    RendGlyphPlan p = { 0 };
    rend_plan_glyph(&fb, FONT_STYLE_EMOJI, true, cps, 1,
                    14, 24, 2, 255, 255, 255, &p);
    ASSERT_EQ(p.avail_w, 24);
    ASSERT_EQ(p.avail_h, 24);
    test_font_reset();
}

static void test_plan_glyph_regional_passes_through(void)
{
    // A regional indicator codepoint routes through rend_plan_glyph and
    // gets the now-uniform cache=avail paths. The plan no longer
    // wraps regional codepoints in a square clamp — FreeType's output
    // is the font's intent (same principle applied to NF / symbol-class
    // glyphs in commits 848b5df and dbfda7a).
    test_font_reset();
    test_font_set_loaded_styles(1u << FONT_STYLE_EMOJI);
    FontBackend fb = make_test_font_backend();
    uint32_t cps[] = { 0x1F1FA }; // regional indicator letter
    RendGlyphPlan p = { 0 };
    rend_plan_glyph(&fb, FONT_STYLE_EMOJI, true, cps, 1,
                    14, 24, 2, 255, 255, 255, &p);
    // Emoji square clamp pulls avail_w down to avail_h=24; cache_w /
    // cache_h track avail without further reduction.
    ASSERT_EQ(p.avail_w, 24);
    ASSERT_EQ(p.avail_h, 24);
    ASSERT_EQ(p.cache_w, 24);
    ASSERT_EQ(p.cache_h, 24);
}

static void test_plan_glyph_box_matches_cell(void)
{
    // A plain 1-cell letter hands every style the whole cell box and asks
    // for nothing special.
    test_font_reset();
    test_font_set_loaded_styles(1u << FONT_STYLE_NORMAL);
    FontBackend fb = make_test_font_backend();
    uint32_t cps[] = { TEST_LATIN_A };
    RendGlyphPlan p = { 0 };
    rend_plan_glyph(&fb, FONT_STYLE_NORMAL, false, cps, 1,
                    11, 21, 1, 255, 255, 255, &p);
    ASSERT_FALSE(p.symbol_cell);
    ASSERT_FALSE(p.height_only_fit);
    for (int s = 0; s < FONT_STYLE_COUNT; s++) {
        int w = 0, h = 0;
        bool hof = true;
        ASSERT_TRUE(test_font_get_box((FontStyle)s, &w, &h, &hof));
        ASSERT_EQ(w, 11);
        ASSERT_EQ(h, 21);
        ASSERT_FALSE(hof);
    }
    test_font_reset();
}

static void test_plan_glyph_box_wide_fallback_keeps_width_fit(void)
{
    // Wide (2-cell) fallback text keeps the width-fit policy: the box is
    // twice the cell width and height_only_fit stays off, so a CJK glyph too
    // wide for its slot is still scaled to fit.
    test_font_reset();
    test_font_set_loaded_styles(1u << FONT_STYLE_NORMAL);
    FontBackend fb = make_test_font_backend();
    uint32_t cps[] = { 0x4E00 }; // 一
    RendGlyphPlan p = { 0 };
    rend_plan_glyph(&fb, FONT_STYLE_NORMAL, false, cps, 1,
                    11, 21, 2, 255, 255, 255, &p);
    ASSERT_FALSE(p.symbol_cell);
    ASSERT_FALSE(p.height_only_fit);
    int w = 0, h = 0;
    bool hof = true;
    ASSERT_TRUE(test_font_get_box(FONT_STYLE_FALLBACK, &w, &h, &hof));
    ASSERT_EQ(w, 22);
    ASSERT_EQ(h, 21);
    ASSERT_FALSE(hof);
    test_font_reset();
}

static void test_plan_glyph_box_symbol_class_height_only(void)
{
    // ❖ U+2756 is Geometric Shapes: symbol-class, so the rasterizer is told
    // to allow horizontal overhang and scale only on vertical overflow.
    test_font_reset();
    test_font_set_loaded_styles(1u << FONT_STYLE_NORMAL);
    FontBackend fb = make_test_font_backend();
    uint32_t cps[] = { 0x2756 };
    RendGlyphPlan p = { 0 };
    rend_plan_glyph(&fb, FONT_STYLE_NORMAL, false, cps, 1,
                    11, 21, 1, 255, 255, 255, &p);
    ASSERT_TRUE(p.symbol_cell);
    ASSERT_TRUE(p.height_only_fit);
    ASSERT_TRUE(p.center_horizontally);
    int w = 0, h = 0;
    bool hof = false;
    ASSERT_TRUE(test_font_get_box(FONT_STYLE_FALLBACK, &w, &h, &hof));
    ASSERT_EQ(w, 11);
    ASSERT_EQ(h, 21);
    ASSERT_TRUE(hof);
    test_font_reset();
}

static void test_plan_glyph_box_dingbat_is_symbol_class(void)
{
    // ✶ U+2736 is Dingbats, which reaches the symbol class through the emoji
    // presentation band (U+2600–27BF), so it gets height-only fitting like
    // the Geometric Shapes glyphs.
    test_font_reset();
    test_font_set_loaded_styles(1u << FONT_STYLE_NORMAL);
    FontBackend fb = make_test_font_backend();
    uint32_t cps[] = { 0x2736 };
    RendGlyphPlan p = { 0 };
    rend_plan_glyph(&fb, FONT_STYLE_NORMAL, false, cps, 1,
                    11, 21, 1, 255, 255, 255, &p);
    ASSERT_TRUE(p.symbol_cell);
    ASSERT_TRUE(p.height_only_fit);
    test_font_reset();
}

static void test_plan_glyph_box_math_operator_keeps_width_fit(void)
{
    // ∀ U+2200 (Mathematical Operators) is not symbol-class: only the bands
    // in rend_is_symbol_cell_cp qualify, so ordinary fallback text keeps the
    // width-fit policy.
    test_font_reset();
    test_font_set_loaded_styles(1u << FONT_STYLE_NORMAL);
    FontBackend fb = make_test_font_backend();
    uint32_t cps[] = { 0x2200 };
    RendGlyphPlan p = { 0 };
    rend_plan_glyph(&fb, FONT_STYLE_NORMAL, false, cps, 1,
                    11, 21, 1, 255, 255, 255, &p);
    ASSERT_FALSE(p.symbol_cell);
    ASSERT_FALSE(p.height_only_fit);
    ASSERT_FALSE(p.center_horizontally);
    int w = 0, h = 0;
    bool hof = true;
    ASSERT_TRUE(test_font_get_box(FONT_STYLE_FALLBACK, &w, &h, &hof));
    ASSERT_FALSE(hof);
    test_font_reset();
}

/* Build a shaped run from parallel arrays. */
static ShapedGlyphs make_run(const uint32_t *ids, const int *advances, int n)
{
    ShapedGlyphs run;
    memset(&run, 0, sizeof(run));
    run.num_glyphs = n;
    run.glyph_ids = (uint32_t *)ids;
    run.x_advances = (int *)advances;
    return run;
}

/* A two-component run the font has no ligature for (🐦‍🟥 in a font that
 * composes neither) must spread its glyphs across the cluster's two cells
 * instead of piling them on one spot. Equal advances → one cell each. */
static void test_layout_shaped_run_two_components(void)
{
    uint32_t ids[3] = { 506, 1, 2280 };
    int advances[3] = { 1275, 0, 1275 };
    ShapedGlyphs run = make_run(ids, advances, 3);

    RendRunGlyphSlot slots[8];
    int n = rend_layout_shaped_run(&run, 20, 10, 2, slots, 8);

    ASSERT_EQ(n, 2);
    /* The zero-advance ZWJ placeholder (gid 1) gets no slot; the two real
     * components land in the first and second cell. */
    ASSERT_EQ(slots[0].index, 0);
    ASSERT_EQ(slots[0].x, 0);
    ASSERT_EQ(slots[0].slot_cells, 1);
    ASSERT_EQ(slots[1].index, 2);
    ASSERT_EQ(slots[1].x, 10);
    ASSERT_EQ(slots[1].slot_cells, 1);
}

/* A composed run (one glyph) needs no distribution — the caller keeps its
 * single-glyph placement. */
static void test_layout_shaped_run_single_glyph(void)
{
    uint32_t ids[1] = { 508 };
    int advances[1] = { 1275 };
    ShapedGlyphs run = make_run(ids, advances, 1);

    RendRunGlyphSlot slots[8];
    ASSERT_EQ(rend_layout_shaped_run(&run, 20, 10, 2, slots, 8), 1);
    ASSERT_EQ(slots[0].x, 0);
}

/* Unequal advances get advance-proportional origins and budgets. */
static void test_layout_shaped_run_proportional(void)
{
    uint32_t ids[2] = { 10, 20 };
    int advances[2] = { 300, 100 };
    ShapedGlyphs run = make_run(ids, advances, 2);

    RendRunGlyphSlot slots[8];
    int n = rend_layout_shaped_run(&run, 20, 10, 2, slots, 8);

    ASSERT_EQ(n, 2);
    ASSERT_EQ(slots[0].x, 0);          /* 20 * 0 / 400 */
    ASSERT_EQ(slots[1].x, 15);         /* 20 * 300 / 400 */
    ASSERT_EQ(slots[0].slot_cells, 2); /* 15px slot → 2 cells */
    ASSERT_EQ(slots[1].slot_cells, 1);
}

/* A three-glyph run in a two-cell cluster clamps every slot to the cluster's
 * own budget — the run must not claim a third cell it doesn't occupy. */
static void test_layout_shaped_run_clamps_to_cluster_cells(void)
{
    uint32_t ids[3] = { 1, 2, 3 };
    int advances[3] = { 100, 100, 100 };
    ShapedGlyphs run = make_run(ids, advances, 3);

    RendRunGlyphSlot slots[8];
    int n = rend_layout_shaped_run(&run, 20, 10, 2, slots, 8);

    ASSERT_EQ(n, 3);
    for (int i = 0; i < n; i++)
        ASSERT_TRUE(slots[i].slot_cells <= 2);
}

/* Nothing drawable (all notdef, or no advance at all) → no slots, so the
 * caller falls back to its own placement instead of drawing nothing. */
static void test_layout_shaped_run_nothing_drawable(void)
{
    uint32_t ids[2] = { 0, 0 };
    int advances[2] = { 100, 100 };
    ShapedGlyphs run = make_run(ids, advances, 2);

    RendRunGlyphSlot slots[8];
    ASSERT_EQ(rend_layout_shaped_run(&run, 20, 10, 2, slots, 8), 0);

    uint32_t ids2[2] = { 5, 6 };
    int advances2[2] = { 0, 0 };
    ShapedGlyphs run2 = make_run(ids2, advances2, 2);
    ASSERT_EQ(rend_layout_shaped_run(&run2, 20, 10, 2, slots, 8), 0);
}

static void test_content_scale_changed(void)
{
    // A window moving from a 1x display to a 2x one.
    ASSERT_TRUE(rend_content_scale_changed(1.0f, 2.0f));
    ASSERT_TRUE(rend_content_scale_changed(2.0f, 1.0f));
    // A compositor scale that moves by a whole step.
    ASSERT_TRUE(rend_content_scale_changed(1.0f, 1.5f));
    // The user's --dpi-scale folds in: 1x compositor with a 2x user scale
    // is the same content scale as a 2x compositor with no user scale.
    ASSERT_TRUE(rend_content_scale_changed(2.0f, 1.0f));
}
static void test_content_scale_unchanged(void)
{
    ASSERT_FALSE(rend_content_scale_changed(1.0f, 1.0f));
    ASSERT_FALSE(rend_content_scale_changed(2.0f, 2.0f));
    // Compositor jitter: a hair below the epsilon is not a change.
    ASSERT_FALSE(rend_content_scale_changed(1.0f, 1.001f));
    ASSERT_FALSE(rend_content_scale_changed(2.0f, 2.0f - REND_CONTENT_SCALE_EPSILON / 2.0f));
}

/* The fallback route cache: a codepoint's covering font and its glyph id
 * are resolved once and memoised for the life of the fallback state. */

static void test_route_resolved_once_per_codepoint(void)
{
    test_resolve_reset();
    test_font_reset();
    test_resolve_add_route(0x1234, "/fonts/a.ttf");
    test_font_set_glyph(0x1234, 77);

    FontBackend fb = make_test_font_backend();
    FontResolveBackend resolve = { 0 };
    FontOptions opts = { 0 };
    RendFallbackState st;
    rend_fallback_init(&st);

    RendRoute r1 = rend_route_get(&st, &fb, &resolve, 0x1234, 12.0f, &opts, 10);
    RendRoute r2 = rend_route_get(&st, &fb, &resolve, 0x1234, 12.0f, &opts, 10);

    // The resolver is consulted once; the second call is served from the
    // route table.
    ASSERT_EQ(test_resolve_query_count(), 1);
    ASSERT_NOT_NULL(r1.identity);
    ASSERT_TRUE(r1.identity == r2.identity);
    ASSERT_EQ(r1.glyph_id, 77);
    ASSERT_EQ(r2.glyph_id, 77);

    rend_fallback_destroy(&st, &fb);
    test_font_reset();
    test_resolve_reset();
}

static void test_route_negative_entry_cached(void)
{
    test_resolve_reset();
    test_font_reset();

    FontBackend fb = make_test_font_backend();
    FontResolveBackend resolve = { 0 };
    FontOptions opts = { 0 };
    RendFallbackState st;
    rend_fallback_init(&st);

    // No font covers this codepoint — the negative answer is memoised too.
    RendRoute r1 = rend_route_get(&st, &fb, &resolve, 0x9999, 12.0f, &opts, 10);
    RendRoute r2 = rend_route_get(&st, &fb, &resolve, 0x9999, 12.0f, &opts, 10);

    ASSERT_EQ(test_resolve_query_count(), 1);
    ASSERT_NULL(r1.identity);
    ASSERT_EQ(r1.glyph_id, 0);
    ASSERT_NULL(r2.identity);
    ASSERT_EQ(r2.glyph_id, 0);

    rend_fallback_destroy(&st, &fb);
    test_font_reset();
    test_resolve_reset();
}

static void test_route_survives_face_eviction(void)
{
    test_resolve_reset();
    test_font_reset();

    // More distinct paths than the pool holds, so the oldest face is
    // evicted while the routes live on.
    const int n = REND_MAX_LOADED_FALLBACKS + 1;
    uint32_t cps[REND_MAX_LOADED_FALLBACKS + 1];
    for (int i = 0; i < n; i++) {
        cps[i] = 0x2000 + i;
        char path[64];
        snprintf(path, sizeof(path), "/fonts/f%d.ttf", i);
        test_resolve_add_route(cps[i], path);
        test_font_set_glyph(cps[i], (uint32_t)(100 + i));
    }

    FontBackend fb = make_test_font_backend();
    FontResolveBackend resolve = { 0 };
    FontOptions opts = { 0 };
    RendFallbackState st;
    rend_fallback_init(&st);

    RendRoute first[REND_MAX_LOADED_FALLBACKS + 1];
    for (int i = 0; i < n; i++)
        first[i] = rend_route_get(&st, &fb, &resolve, cps[i], 12.0f, &opts, 10);

    for (int i = 0; i < n; i++) {
        RendRoute again =
            rend_route_get(&st, &fb, &resolve, cps[i], 12.0f, &opts, 10);
        ASSERT_EQ(test_resolve_query_count(), n);
        ASSERT_TRUE(first[i].identity != NULL);
        ASSERT_TRUE(first[i].identity == again.identity);
        ASSERT_EQ(first[i].glyph_id, again.glyph_id);
        ASSERT_EQ(again.glyph_id, (uint32_t)(100 + i));
    }

    rend_fallback_destroy(&st, &fb);
    test_font_reset();
    test_resolve_reset();
}

static void test_route_dies_with_state(void)
{
    test_resolve_reset();
    test_font_reset();
    test_resolve_add_route(0x1234, "/fonts/a.ttf");
    test_font_set_glyph(0x1234, 77);

    FontBackend fb = make_test_font_backend();
    FontResolveBackend resolve = { 0 };
    FontOptions opts = { 0 };
    RendFallbackState st;
    rend_fallback_init(&st);

    RendRoute r1 = rend_route_get(&st, &fb, &resolve, 0x1234, 12.0f, &opts, 10);
    ASSERT_NOT_NULL(r1.identity);

    // The route cache lives inside the fallback state, so a fresh state
    // asks the resolver again.
    rend_fallback_destroy(&st, &fb);
    rend_fallback_init(&st);
    RendRoute r2 = rend_route_get(&st, &fb, &resolve, 0x1234, 12.0f, &opts, 10);
    ASSERT_EQ(test_resolve_query_count(), 2);
    ASSERT_NOT_NULL(r2.identity);
    ASSERT_TRUE(r1.identity != r2.identity);

    rend_fallback_destroy(&st, &fb);
    test_font_reset();
    test_resolve_reset();
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
    RUN_TEST(test_live_row_to_display_row_no_scroll);
    RUN_TEST(test_live_row_to_display_row_partial_scroll);
    RUN_TEST(test_live_row_to_display_row_scrolled_out);
    RUN_TEST(test_scroll_clamp_after_scrollback_purge);
    RUN_TEST(test_clamp_pixel_to_viewport);
    RUN_TEST(test_downscale_output_fits_cell_box);
    RUN_TEST(test_apply_glyph_layout_passthrough);
    RUN_TEST(test_apply_glyph_layout_center_only);
    RUN_TEST(test_apply_glyph_layout_downscale_centered);
    RUN_TEST(test_layout_shaped_run_two_components);
    RUN_TEST(test_layout_shaped_run_single_glyph);
    RUN_TEST(test_layout_shaped_run_proportional);
    RUN_TEST(test_layout_shaped_run_clamps_to_cluster_cells);
    RUN_TEST(test_layout_shaped_run_nothing_drawable);
    RUN_TEST(test_resolve_cell_style_normal_default);
    RUN_TEST(test_resolve_cell_style_bold_italic_cascade);
    RUN_TEST(test_resolve_cell_style_bold_loads_when_only_bold_present);
    RUN_TEST(test_resolve_cell_style_emoji_overrides_bold);
    RUN_TEST(test_resolve_cell_style_emoji_falls_back_when_glyph_missing);
    RUN_TEST(test_plan_glyph_sets_emoji_flags);
    RUN_TEST(test_plan_glyph_emoji_square_clamps_avail);
    RUN_TEST(test_plan_glyph_regional_passes_through);
    RUN_TEST(test_plan_glyph_box_matches_cell);
    RUN_TEST(test_plan_glyph_box_wide_fallback_keeps_width_fit);
    RUN_TEST(test_plan_glyph_box_symbol_class_height_only);
    RUN_TEST(test_plan_glyph_box_dingbat_is_symbol_class);
    RUN_TEST(test_plan_glyph_box_math_operator_keeps_width_fit);
    RUN_TEST(test_content_scale_changed);
    RUN_TEST(test_content_scale_unchanged);
    RUN_TEST(test_route_resolved_once_per_codepoint);
    RUN_TEST(test_route_negative_entry_cached);
    RUN_TEST(test_route_survives_face_eviction);
    RUN_TEST(test_route_dies_with_state);

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

// The downscale-or-center policy helper decides per-cell what to do
// with a freshly rasterized glyph bitmap. The integration tests cover
// the policy decision in render_cell, but the helper's branching is
// small and high-impact — locking the three branches down keeps the
// refactor honest.
static GlyphBitmap *make_alpha_bitmap(int w, int h)
{
    GlyphBitmap *b = calloc(1, sizeof(GlyphBitmap));
    b->width = w;
    b->height = h;
    b->pixels = calloc((size_t)w * h * 4, 1);
    for (int p = 0; p < w * h; p++)
        b->pixels[p * 4 + 3] = 200;
    return b;
}

static void test_apply_glyph_layout_passthrough(void)
{
    // Neither downscale nor center: helper leaves the bitmap untouched
    // and returns NULL. Caller inserts the input bitmap itself.
    GlyphBitmap *b = make_alpha_bitmap(7, 11);
    GlyphBitmap *out = rend_apply_glyph_layout(b, false, false, 16, 16);
    ASSERT_NULL(out);
    ASSERT_FALSE(b->centered);
    ASSERT_EQ(b->x_offset, 0);
    ASSERT_EQ(b->width, 7); // not mutated
    ASSERT_EQ(b->height, 11);
    free(b->pixels);
    free(b);
}

static void test_apply_glyph_layout_center_only(void)
{
    // Center only (no downscale): the bitmap's x_offset is rewritten so a
    // mono font whose FreeType bitmap_left is calibrated against an
    // oversized advance has its ink centered in the cell. The bitmap is
    // returned as NULL — caller inserts the (now-mutated) input.
    GlyphBitmap *b = make_alpha_bitmap(6, 14);
    GlyphBitmap *out = rend_apply_glyph_layout(b, false, true, 16, 16);
    ASSERT_NULL(out);
    ASSERT_FALSE(b->centered);            // center_horizontal sets x_offset, not the centered bit
    ASSERT_EQ(b->x_offset, (16 - 6) / 2); // just the horizontal centering
    ASSERT_EQ(b->width, 6);
    free(b->pixels);
    free(b);
}

static void test_apply_glyph_layout_downscale_centered(void)
{
    // Downscale path: a fresh bitmap is returned (the original is left
    // untouched), centered bit is set on both, fits within max_w x max_h.
    GlyphBitmap *b = make_alpha_bitmap(48, 48);
    GlyphBitmap *out = rend_apply_glyph_layout(b, true, false, 16, 16);
    ASSERT_NOT_NULL(out);
    ASSERT_TRUE(out->centered);
    ASSERT_TRUE(b->centered);
    // Downscale fits (clamp invariant from commit 27b55a2 holds).
    ASSERT_TRUE(out->width <= 16);
    ASSERT_TRUE(out->height <= 16);
    free(out->pixels);
    free(out);
    free(b->pixels);
    free(b);
}
