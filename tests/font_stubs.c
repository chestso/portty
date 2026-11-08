/*
 * portty — Test stubs for font resolution functions
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

// Stubs for font_resolve functions used by rend_common.c's font loading
// code. The atlas/rend_common tests don't actually call rend_load_fonts,
// but the linker pulls in all symbols from rend_common.o.
#include "font_resolve.h"
#include <stdlib.h>
#include <string.h>

// --- Configurable codepoint routes (route-cache tests) ---
static char *s_cp_paths[64];
static uint32_t s_cp_codepoints[64];
static int s_cp_count;
static int s_resolve_queries;

void test_resolve_add_route(uint32_t cp, const char *path)
{
    if (s_cp_count < 64) {
        s_cp_codepoints[s_cp_count] = cp;
        s_cp_paths[s_cp_count] = strdup(path);
        s_cp_count++;
    }
}

int test_resolve_query_count(void) { return s_resolve_queries; }

void test_resolve_reset(void)
{
    for (int i = 0; i < s_cp_count; i++)
        free(s_cp_paths[i]);
    s_cp_count = 0;
    s_resolve_queries = 0;
}

FontResolveBackend *font_resolve_init(FontResolveBackend *backend)
{
    (void)backend;
    return NULL;
}

void font_resolve_destroy(FontResolveBackend *resolve)
{
    (void)resolve;
}

int font_resolve_find_font(FontResolveBackend *resolve, FontType type,
                           const char *pattern, FontResolutionResult *result)
{
    (void)resolve;
    (void)type;
    (void)pattern;
    memset(result, 0, sizeof(*result));
    return -1;
}

int font_resolve_find_font_for_codepoint(FontResolveBackend *resolve,
                                         uint32_t codepoint,
                                         FontResolutionResult *result)
{
    (void)resolve;
    memset(result, 0, sizeof(*result));
    s_resolve_queries++;
    for (int i = 0; i < s_cp_count; i++) {
        if (s_cp_codepoints[i] == codepoint) {
            result->font_path = strdup(s_cp_paths[i]);
            return 0;
        }
    }
    return -1;
}

void font_resolve_free_result(FontResolutionResult *result)
{
    (void)result;
}

void font_resolve_list_monospace(FontResolveBackend *resolve)
{
    (void)resolve;
}

// =============================================================================
// Configurable FontBackend stub for rend_common unit tests
// =============================================================================
//
// Tests configure loaded_styles via test_font_set_loaded_styles, then hand
// a FontBackend configured by make_test_font_backend to the helper under
// test. Only get_glyph_index is implemented; the rest are NULL and the
// font.c dispatch wrappers short-circuit on a missing function pointer,
// so the stubs only need to fake the data-driven branch decisions:
//   - which styles are "loaded" (loaded_styles bitmask)
//   - whether the emoji font carries the test codepoint (get_glyph_index)
//
// The stub returns non-zero from get_glyph_index for U+26A1 (lightning
// emoji) and 0 otherwise, so tests can use that codepoint to trigger
// the emoji routing branch deterministically.
#include "font.h"

static uint32_t s_test_loaded_styles = 0;
#define TEST_STUB_EMOJI_CP 0x26A1 // ⚡ — has emoji presentation + stub claims glyph

// Glyph ids the stub hands out for specific codepoints, so a test can pin
// the id a route memoises.
static uint32_t s_glyph_map[64];
static uint32_t s_glyph_cps[64];
static int s_glyph_map_count;

void test_font_set_glyph(uint32_t cp, uint32_t glyph_id)
{
    for (int i = 0; i < s_glyph_map_count; i++)
        if (s_glyph_cps[i] == cp) {
            s_glyph_map[i] = glyph_id;
            return;
        }
    if (s_glyph_map_count < 64) {
        s_glyph_cps[s_glyph_map_count] = cp;
        s_glyph_map[s_glyph_map_count] = glyph_id;
        s_glyph_map_count++;
    }
}

// Recorded per-style presentation budget, so tests can assert what
// rend_plan_glyph hands the backend for a given cell.
typedef struct
{
    int width;
    int height;
    bool height_only_fit;
} TestFontBox;

static TestFontBox s_boxes[FONT_STYLE_COUNT];
static bool s_box_set[FONT_STYLE_COUNT];

// Per-face shaped-run map (cp -> gid) and the shaping counter, so tests
// can pin exactly which faces cover which cluster codepoints.
typedef struct
{
    void *face;
    uint32_t cp;
    uint32_t gid;
} TestFaceGlyph;

static TestFaceGlyph s_face_glyphs[16384];
static int s_face_glyph_count;
static int s_shape_count;

void test_font_set_face_glyph(void *face_tag, uint32_t cp, uint32_t gid)
{
    for (int i = 0; i < s_face_glyph_count; i++) {
        if (s_face_glyphs[i].face == face_tag && s_face_glyphs[i].cp == cp) {
            s_face_glyphs[i].gid = gid;
            return;
        }
    }
    if (s_face_glyph_count < 16384) {
        s_face_glyphs[s_face_glyph_count].face = face_tag;
        s_face_glyphs[s_face_glyph_count].cp = cp;
        s_face_glyphs[s_face_glyph_count].gid = gid;
        s_face_glyph_count++;
    }
}

int test_shape_count(void) { return s_shape_count; }

void test_font_set_loaded_styles(uint32_t mask) { s_test_loaded_styles = mask; }
uint32_t test_font_get_loaded_styles(void) { return s_test_loaded_styles; }
void test_font_reset(void)
{
    s_test_loaded_styles = 0;
    memset(s_boxes, 0, sizeof(s_boxes));
    memset(s_box_set, 0, sizeof(s_box_set));
    memset(s_glyph_map, 0, sizeof(s_glyph_map));
    memset(s_glyph_cps, 0, sizeof(s_glyph_cps));
    s_glyph_map_count = 0;
    memset(s_face_glyphs, 0, sizeof(s_face_glyphs));
    s_face_glyph_count = 0;
    s_shape_count = 0;
}

// Read back the budget the helper pushed for `style`. Returns false when the
// style was never given one. Out-params avoid sharing the recording struct
// across translation units.
bool test_font_get_box(FontStyle style, int *width, int *height,
                       bool *height_only_fit)
{
    if ((int)style < 0 || style >= FONT_STYLE_COUNT || !s_box_set[style])
        return false;
    if (width)
        *width = s_boxes[style].width;
    if (height)
        *height = s_boxes[style].height;
    if (height_only_fit)
        *height_only_fit = s_boxes[style].height_only_fit;
    return true;
}

static void stub_set_presentation_box(void *font_data, int width, int height,
                                      bool height_only_fit)
{
    int style = (int)(intptr_t)font_data - 1;
    if (style < 0 || style >= FONT_STYLE_COUNT)
        return;
    s_boxes[style].width = width;
    s_boxes[style].height = height;
    s_boxes[style].height_only_fit = height_only_fit;
    s_box_set[style] = true;
}

static uint32_t stub_get_glyph_index(FontBackend *font, void *font_data,
                                     uint32_t cp)
{
    (void)font;
    (void)font_data;
    for (int i = 0; i < s_glyph_map_count; i++)
        if (s_glyph_cps[i] == cp)
            return s_glyph_map[i];
    return cp == TEST_STUB_EMOJI_CP ? 1 : 0;
}

// The fallback loader calls these directly on the backend, so the stub
// backend needs them: a face handle per load, a no-op destroy, and metrics
// that report success.
static void *stub_init_font(FontBackend *font, const char *font_path,
                            float font_size, FontStyle style,
                            const FontOptions *options)
{
    (void)font;
    (void)font_size;
    (void)style;
    (void)options;
    // Deterministic handle per path, so a face reloaded after eviction
    // keeps the glyph map the test pinned for that file.
    static char *paths[256];
    static int path_count = 0;
    for (int i = 0; i < path_count; i++) {
        if (strcmp(paths[i], font_path) == 0)
            return (void *)(intptr_t)(2000 + i);
    }
    if (path_count < 256) {
        paths[path_count] = strdup(font_path);
        return (void *)(intptr_t)(2000 + path_count++);
    }
    static int seq = 0;
    return (void *)(intptr_t)(++seq + 1000);
}

static void stub_destroy_font(FontBackend *font, void *font_data)
{
    (void)font;
    (void)font_data;
}

static bool stub_get_metrics(FontBackend *font, void *font_data,
                             FontMetrics *metrics)
{
    (void)font;
    (void)font_data;
    memset(metrics, 0, sizeof(*metrics));
    return true;
}

// One glyph per codepoint, advance 6. Unmapped codepoints shape to
// .notdef (gid 0); joiners and variation selectors shape to a non-zero
// invisible glyph, like HarfBuzz's default-ignorable handling.
static ShapedGlyphs *stub_render_shaped(FontBackend *font, void *font_data,
                                        uint32_t *codepoints, int count,
                                        uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    (void)font;
    (void)fg_r;
    (void)fg_g;
    (void)fg_b;
    s_shape_count++;
    ShapedGlyphs *run = calloc(1, sizeof(ShapedGlyphs));
    if (!run)
        return NULL;
    run->num_glyphs = count;
    run->glyph_ids = calloc((size_t)count, sizeof(uint32_t));
    run->x_positions = calloc((size_t)count, sizeof(int));
    run->y_positions = calloc((size_t)count, sizeof(int));
    run->x_advances = calloc((size_t)count, sizeof(int));
    if (!run->glyph_ids || !run->x_positions || !run->y_positions ||
        !run->x_advances) {
        free(run->glyph_ids);
        free(run->x_positions);
        free(run->y_positions);
        free(run->x_advances);
        free(run);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        uint32_t gid = 0;
        for (int j = 0; j < s_face_glyph_count; j++) {
            if (s_face_glyphs[j].face == font_data &&
                s_face_glyphs[j].cp == codepoints[i]) {
                gid = s_face_glyphs[j].gid;
                break;
            }
        }
        if (gid == 0 && (codepoints[i] == 0x200D ||
                         (codepoints[i] >= 0xFE00 && codepoints[i] <= 0xFE0F)))
            gid = 4242;
        run->glyph_ids[i] = gid;
        run->x_positions[i] = i * 6;
        run->x_advances[i] = 6;
        run->total_advance += 6;
    }
    return run;
}

FontBackend make_test_font_backend(void)
{
    FontBackend fb = { 0 };
    fb.name = "test-font-stub";
    fb.loaded_styles = s_test_loaded_styles;
    fb.get_glyph_index = stub_get_glyph_index;
    fb.set_presentation_box = stub_set_presentation_box;
    fb.render_shaped = stub_render_shaped;
    fb.init_font = stub_init_font;
    fb.destroy_font = stub_destroy_font;
    fb.get_metrics = stub_get_metrics;
    // Non-NULL font_data per style: font.c's set_presentation_box wrapper
    // (like every other wrapper) skips styles with no loaded face. The value
    // encodes the style index so the recorder can attribute the call.
    for (int i = 0; i < FONT_STYLE_COUNT; i++)
        fb.font_data[i] = (void *)(intptr_t)(i + 1);
    // All other callbacks left NULL — font.c's wrapper layer short-circuits
    // when a function pointer is missing, so the unit tests only need to
    // drive decisions via loaded_styles + the emoji-codepoint branch.
    return fb;
}
