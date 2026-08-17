/*
 * portty — GPU-agnostic renderer helper interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef REND_COMMON_H
#define REND_COMMON_H

#include <stdbool.h>
#include <stdint.h>

// Underline style values (matches terminal cell attr.underline)
typedef enum
{
    UNDERLINE_NONE = 0,
    UNDERLINE_SINGLE = 1,
    UNDERLINE_DOUBLE = 2,
    UNDERLINE_CURLY = 3,
    UNDERLINE_DOTTED = 4,
    UNDERLINE_DASHED = 5
} UnderlineStyle;

// Unicode codepoint constants used by emoji routing
#define UNICODE_VARIATION_SELECTOR_15 0xFE0E
#define UNICODE_VARIATION_SELECTOR_16 0xFE0F

// Emoji detection functions (moved from the former unicode module)
bool is_emoji_presentation(uint32_t cp);
bool is_regional_indicator(uint32_t cp);

// Returns true if any codepoint in the cell's chars[] is U+FE0E (VS15).
// VS15 narrows the cell to 1 column but does NOT block color emoji routing.
bool unicode_cell_has_vs15(const uint32_t *chars, int max);

// sRGB transfer function, single-sourced here for every CPU-side color-space
// conversion in the renderer (the color-glyph atlas linearize LUT and the
// notification/link-hint panel glyph blend). The standard sRGB EOTF/OETF.
float rend_srgb_to_linear(uint8_t v);
uint8_t rend_linear_to_srgb(float lin);

// Convert RGBA pixel data from sRGB to linear in-place. Used for sixel/lottie
// textures on the linear-light path so they round-trip correctly through the
// SRGB_LINEAR render target. Alpha channel is preserved unchanged.
void rend_linearize_rgba_in_place(uint8_t *pixels, int w, int h);

// Nerd Fonts v2 -> v3 codepoint translation. Returns the v3 equivalent if
// `cp` is in the legacy U+F900-U+FAFF range, otherwise returns `cp` unchanged.
uint32_t rend_nf_translate_codepoint(uint32_t cp);

// Convert a display row to a unified row. Scrollback rows are negative:
// -1 is the most recent scrollback line. `scroll_offset` is the renderer's
// current scroll position (0 = no scrollback visible).
int rend_display_row_to_unified(int scroll_offset, int display_row);

// Clamp a pixel coordinate into the viewport. Helper for selection drag.
void rend_clamp_pixel_to_viewport(int *px, int *py, int viewport_w, int viewport_h);

// =============================================================================
// Coordinate space conversion — HiDPI scaling
// =============================================================================
//
// portty uses two coordinate spaces:
//
// LOGICAL (from terminal engine/coffer):
//   - sixel.width_px, sixel.height_px: unscaled pixel dimensions
//   - lottie.canvas_w, lottie.canvas_h: unscaled pixel dimensions
//   - Terminal column/row positions: always logical
//   - Font metrics: unscaled
//
// PHYSICAL (backend-internal):
//   - Window/framebuffer dimensions
//   - cell_w, cell_h: scaled by content_scale during font load
//   - Mouse coordinates: from SDL in physical pixels
//   - All rendering coordinates
//
// content_scale multiplies logical → physical.
// Use these helpers consistently throughout both backends.

static inline int logical_to_physical(int logical, float scale)
{
    return (int)(logical * scale + 0.5f);
}

static inline int physical_to_logical(int physical, float scale)
{
    return (int)(physical / scale + 0.5f);
}

static inline float logical_to_physical_f(float logical, float scale)
{
    return logical * scale;
}

// =============================================================================
// Glyph atlas packing — GPU-agnostic
// =============================================================================
//
// Used by the SDL3 backend. The packing algorithm (shelf-based
// bin packing, hash table, eviction) is pure integer math. Only the texture
// upload path differs between backends (SDL_Texture vs sg_image).
//
// Each backend embeds a RendAtlas struct inside its own atlas struct and
// calls these functions for packing/lookup. The backend implements only
// init (create GPU texture), flush (upload dirty staging buffer to GPU),
// and destroy (free GPU texture).

#include "font.h"

// Downscale a glyph bitmap to fit within max_w × max_h using area-averaging.
// The bitmap is scaled by the smaller ratio to preserve aspect. Returns a
// new GlyphBitmap (caller frees pixels + struct), or NULL if no scaling was
// needed or on allocation failure.
GlyphBitmap *rend_downscale_bitmap(GlyphBitmap *src, int max_w, int max_h);

// Check if a codepoint is a symbol/dingbat that may overflow the text cell
// and needs centered placement (no downscale — symbol glyphs are rasterized
// at the text font's native metrics, like ASCII, so they stay crisp).
bool rend_is_symbol_cell_cp(uint32_t cp);

// Check if a font style produces color (COLR v1 or emoji) glyphs.
bool rend_is_color_font(FontBackend *font, FontStyle style);

// Decide whether a codepoint cluster should route to the color emoji font.
// Returns true if the emoji font should be used. VS15 narrows the cell to
// 1 column but does NOT block color emoji routing — if the emoji font
// carries the glyph, portty prefers it (beauty over spec).
//
// `cps` is the codepoint array for the cell (base + variation selectors).
// `cp_count` is the number of codepoints. `has_glyph` should be the result
// of font_get_glyph_index(font, FONT_STYLE_EMOJI, cps[0]) != 0.
bool rend_should_use_emoji(const uint32_t *cps, int cp_count,
                           bool emoji_font_available, bool emoji_has_glyph);

// Resolve the FontStyle to use for a cell from its SGR attributes and
// codepoint sequence. Wraps the bold/italic cascade with graceful fallback
// when a requested style isn't loaded (bold+italic prefers bold or italic
// before falling back to NORMAL — preserves exising glyph styling instead
// of dropping it altogether), then consults `rend_should_use_emoji` to
// route through the color emoji font when applicable.
// Used by the SDL3 renderer.
typedef struct
{
    FontStyle style;
    bool use_emoji;
} RendCellStyle;

RendCellStyle rend_resolve_cell_style(FontBackend *font,
                                      bool cell_bold, bool cell_italic,
                                      const uint32_t *cps, int cp_count);

// Apply the downscale-or-center policy to a freshly rasterized glyph
// bitmap before it goes into the per-backend atlas. The SDL3
// `cache_glyph` wrapper does the same three things://
//   * if downscale: scale the bitmap to fit max_w x max_h via the 4x emoji
//     pipeline (which supersampled before clamping) and mark the result
//     as centered;
//   * else if center_horizontally: override x_offset so the ink sits
//     centered in the cell while bitmap_top still anchors to the
//     typographic baseline — for mono fonts whose freeType bitmap_left
//     is calibrated against an oversized advance;
//   * else: leave the bitmap untouched and let the backend position
//     it by FreeType's natural metrics on the baseline.
//
// Downscale is reserved for the 4x emoji path; never applied to plain
// text, Nerd Fonts, or symbol-class glyphs (they render at FreeType's
// native metrics, so resampling would destroy crispness). Consumes
// `bitmap` and returns either a freshly-allocated scaled copy or NULL
// if neither downscale nor center_horizontal was requested (caller
// inserts `bitmap` itself in that case).
//
// Caller frees the returned bitmap's pixels + struct (if non-NULL) and
// inserts itself — the helper does not touch any atlas.
GlyphBitmap *rend_apply_glyph_layout(GlyphBitmap *bitmap,
                                     bool downscale,
                                     bool center_horizontally,
                                     int max_w, int max_h);

// Per-cell glyph rendering decisions: presentation sizes, color-baked
// flag, atlas color key, render colors, symbol-class flag, and the
// downscale-or-center policy. Caller already picked a FontStyle via
// rend_resolve_cell_style. The helper also pushes font_set_presentation_width
// into the font backend for every style — backends must call this once at
// the top of render_cell before font_render_*.
typedef struct
{
    void *font_data;
    bool color_baked;
    uint8_t render_r, render_g, render_b; // for font_render_*
    uint32_t color_key;                   // for atlas lookup
    bool symbol_cell;
    bool downscale_glyph;     // 4x emoji pipeline only
    bool center_horizontally; // symbol-class without color-baked
    int avail_w, avail_h;     // post-emoji square clamp
    int cache_w, cache_h;     // — initially equals avail_w/avail_h.
                              // Kept as a separate field so backends can
                              // override without recomputing presentation_width.
} RendGlyphPlan;

void rend_plan_glyph(FontBackend *font, FontStyle style, bool emoji_render,
                     const uint32_t *cps, int cp_count,
                     int cell_w, int cell_h, int columns_to_consume,
                     uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                     RendGlyphPlan *out);

#define REND_ATLAS_HASH_SIZE    8192
#define REND_ATLAS_TEXTURE_SIZE 2048
#define REND_ATLAS_MAX_SHELVES  128

typedef struct
{
    int x, y, w, h;
} RendAtlasRegion;

typedef struct
{
    int y;
    int height;
    int cursor_x;
} RendAtlasShelf;

typedef struct
{
    void *font_data;
    int glyph_id;
    uint32_t color;
    RendAtlasRegion region;
    int x_offset, y_offset;
    uint64_t last_used_frame;
    bool occupied;
    bool centered;
} RendAtlasEntry;

typedef struct
{
    uint8_t *staging;
    bool dirty;
    RendAtlasRegion dirty_rect;
    RendAtlasShelf shelves[REND_ATLAS_MAX_SHELVES];
    int num_shelves;
    int next_shelf_y;
    RendAtlasEntry entries[REND_ATLAS_HASH_SIZE];
    int entry_count;
    uint64_t current_frame;
    bool eviction_occurred;
} RendAtlas;

// Initialize the packing state. `staging` must be a pre-allocated RGBA buffer
// of size REND_ATLAS_TEXTURE_SIZE * REND_ATLAS_TEXTURE_SIZE * 4.
void rend_atlas_init(RendAtlas *atlas, uint8_t *staging);

// Begin a new frame: increments the frame counter for LRU tracking.
void rend_atlas_begin_frame(RendAtlas *atlas);

// Look up a cached glyph. Returns NULL on miss.
RendAtlasEntry *rend_atlas_lookup(RendAtlas *atlas, void *font_data,
                                  int glyph_id, uint32_t color);

// Insert a rasterized glyph bitmap into the staging buffer and hash table.
// `is_color` and `linearize_color` control sRGB→linear decode for color glyphs.
// Returns the entry, or NULL if the glyph is too large even for an empty atlas.
RendAtlasEntry *rend_atlas_insert(RendAtlas *atlas, void *font_data,
                                  int glyph_id, uint32_t color,
                                  GlyphBitmap *bmp, bool is_color,
                                  bool linearize_color);

// Insert a placeholder entry (no pixels) to mark a glyph as missing.
RendAtlasEntry *rend_atlas_insert_empty(RendAtlas *atlas, void *font_data,
                                        int glyph_id, uint32_t color);

// Log packing statistics.
void rend_atlas_log_stats(RendAtlas *atlas);

// =============================================================================
// Font loading orchestration — GPU-agnostic
// =============================================================================
//
// Used by the SDL3 backend. Resolves font paths via the
// platform-specific FontResolveBackend, loads all styles via FreeType,
// and computes cell metrics. The backend provides the font resolver
// and font backend; the function fills in the result struct.

#include "font.h"
#include "font_resolve.h"

#define REND_EMOJI_FONT_SCALE 4.0f

typedef struct
{
    FontBackend *font;
    FontResolveBackend *resolve;
    float font_size;
    FontOptions font_options;
    char *font_path;
    int cell_width;
    int cell_height;
    int font_ascent;
    int font_descent;
    int font_cap_height;
    int char_width;
    int char_height;
    const char *hint_name;
} RendFontLoadResult;

// Load all font styles (normal, bold, italic, bold-italic, emoji) and
// compute cell metrics. `content_scale` is used for DPI scaling.
//
// `font` must be initialized (font_init called). `resolve` must be
// initialized (font_resolve_init called). On failure, the caller is
// responsible for cleaning up `font` and `resolve`.
//
// Returns 0 on success, -1 on failure.
// `hint_name` is a human-readable string for logging (e.g. "light", "normal").
int rend_load_fonts(RendFontLoadResult *result,
                    FontBackend *font, FontResolveBackend *resolve,
                    float font_size, const char *font_name,
                    int ft_hint_target, float content_scale,
                    const char *hint_name);

// =============================================================================
// Box-drawing glyph rasterization — GPU-agnostic
// =============================================================================

// Returns true if the codepoint is a box drawing (U+2500-U+257F)
// or block element (U+2580-U+259F) character we can draw procedurally.
bool rend_boxdraw_is_supported(uint32_t cp);

// Render a box drawing or block element character into a CPU-side RGBA
// pixel buffer (GlyphBitmap). The bitmap is cell-sized with centered=true
// so the atlas pipeline places it at the cell centre.
// Caller must free: free(bmp->pixels); free(bmp);
GlyphBitmap *rend_boxdraw_render(uint32_t cp, int cell_w, int cell_h,
                                 uint8_t r, uint8_t g, uint8_t b);

// Sentinel font_data pointer used as the atlas key for box-drawing
// glyphs, distinguishing them from real font glyphs.
#define BOXDRAW_FONT_DATA ((void *)(intptr_t)1)

// Atlas glyph ID for close button (distinguishes from box-drawing)
#define CLOSE_BUTTON_GLYPH_ID 0xFFFFFFFE

// =============================================================================
// Close "×" bitmap helper — used by the SDL3 backend
// =============================================================================
// Fill a `size`×`size` RGBA buffer with an anti-aliased white "×".
// The bitmap is white (255,255,255) with alpha coverage in the A channel;
// callers tint it via their renderer. `buf` must hold `size*size*4` bytes.
void rend_make_close_x_bitmap(uint8_t *buf, int size);

// =============================================================================
// Fallback font resolution — GPU-agnostic
// =============================================================================

#define REND_FALLBACK_CACHE_SIZE  2048
#define REND_MAX_LOADED_FALLBACKS 8

typedef struct
{
    uint32_t codepoint;
    char *font_path;
} RendFallbackCacheEntry;

typedef struct
{
    char *font_path;
    void *font_data;
} RendLoadedFallbackFont;

typedef struct
{
    RendFallbackCacheEntry cache[REND_FALLBACK_CACHE_SIZE];
    int cache_count;
    RendLoadedFallbackFont loaded[REND_MAX_LOADED_FALLBACKS];
    int loaded_count;
} RendFallbackState;

void rend_fallback_init(RendFallbackState *st);
void rend_fallback_destroy(RendFallbackState *st, FontBackend *font);
const char *rend_fallback_lookup(RendFallbackState *st,
                                 FontResolveBackend *resolve,
                                 uint32_t codepoint);
bool rend_fallback_ensure(RendFallbackState *st, FontBackend *font,
                          const char *font_path, float font_size,
                          const FontOptions *options, int cell_width);

// =============================================================================
// Scroll offset management — GPU-agnostic
// =============================================================================

#include "term.h"

typedef struct
{
    int scroll_offset;
    int saved_scroll_offset;
    TerminalBackend *overlay;
} RendScrollState;

void rend_scroll(RendScrollState *st, TerminalBackend *term, int delta);
void rend_reset_scroll(RendScrollState *st);
int rend_get_scroll_offset(RendScrollState *st);
void rend_set_overlay(RendScrollState *st, TerminalBackend *overlay);
void rend_clear_overlay(RendScrollState *st);
bool rend_has_overlay(RendScrollState *st);

// =============================================================================
// Diagnostics helpers — GPU-agnostic
// =============================================================================

// Classify whether a GPU driver is open-source (libre) based on its
// driver name and driver info strings. Returns true if the strings
// indicate a Mesa or open-source driver.
bool rend_classify_gpu_driver_libre(const char *driver_name,
                                    const char *driver_info);

// Infer the graphics API (Vulkan, OpenGL, Metal, D3D) from the driver name.
// Returns NULL if unknown.
const char *rend_infer_graphics_api(const char *driver_name);

// Format a GPU driver string for diagnostics output.
// `buf` must be at least 160 bytes. Produces e.g. "Mesa (open source) — 24.1".
void rend_format_gpu_driver(char *buf, size_t bufsz,
                            const char *driver_name,
                            const char *driver_info,
                            const char *driver_version);

#endif // REND_COMMON_H
