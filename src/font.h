#ifndef FONT_H
#define FONT_H

#include <stdbool.h>
#include <stdint.h>

// Font metrics structure
typedef struct FontMetrics
{
    int ascent;  // Distance from baseline to top
    int descent; // Distance from baseline to bottom
    int line_gap;
    int glyph_width;  // Typical glyph advance width
    int glyph_height; // Typical glyph bounding height
    int cell_width;   // Cell width including padding
    int cell_height;  // Cell height including padding
} FontMetrics;

// Font style enumeration
typedef enum FontStyle
{
    FONT_STYLE_NORMAL = 0,
    FONT_STYLE_BOLD,
    FONT_STYLE_EMOJI,
    FONT_STYLE_FALLBACK,
    FONT_STYLE_COUNT
} FontStyle;

// Bitmap structure for glyph rendering
typedef struct GlyphBitmap
{
    uint8_t *pixels; // RGBA format, 4 bytes per pixel
    int width;
    int height;
    int x_offset; // Horizontal offset from cell origin
    int y_offset; // Vertical offset from baseline
    int advance;  // Horizontal advance to next glyph
    int glyph_id; // Underlying font glyph id (if available)
} GlyphBitmap;

// Font options structure
typedef struct FontOptions
{
    int ft_hint_target; // FT_LOAD_NO_HINTING, FT_LOAD_TARGET_LIGHT, _NORMAL, or _MONO
    int subpixel_order;
    int lcd_filter;
    int dpi_x; // Horizontal DPI for HiDPI support
    int dpi_y; // Vertical DPI for HiDPI support
} FontOptions;

// NEW: Shaped glyph output structure (for HarfBuzz-shaped runs)
typedef struct ShapedGlyphs
{
    int num_glyphs;
    GlyphBitmap **bitmaps; // Array of rasterized glyph bitmaps (owned by caller)
    int *x_positions;      // Pixel x positions for each glyph (relative to run origin)
    int *y_positions;      // Pixel y positions for each glyph (relative to run origin)
    int *x_advances;       // Pixel advances for each glyph
    int total_advance;     // Total run advance in pixels
} ShapedGlyphs;

// Abstract font font interface
struct FontBackend;
typedef struct FontBackend FontBackend;

// Backend interface definition
struct FontBackend
{
    const char *name;

    // Font data and state
    void *font_data[FONT_STYLE_COUNT];     // Backend-specific data per font style
    FontMetrics metrics[FONT_STYLE_COUNT]; // Metrics for each font style
    uint32_t loaded_styles;                // Bitmask of loaded font styles

    // Initialize font
    bool (*init)(FontBackend *font);

    // Destroy font
    void (*destroy)(FontBackend *font);

    // Initialize font-specific data for a font style
    void *(*init_font)(FontBackend *font, const char *font_path,
                       float font_size, FontStyle style, const FontOptions *options);

    // Destroy font-specific data
    void (*destroy_font)(FontBackend *font, void *font_data);

    // Get metrics for a font
    bool (*get_metrics)(FontBackend *font, void *font_data, FontMetrics *metrics);

    // Unified glyph renderer for both single and multiple codepoints
    GlyphBitmap *(*render_glyphs)(FontBackend *font, void *font_data,
                                  uint32_t *codepoints, int codepoint_count,
                                  uint8_t fg_r, uint8_t fg_g, uint8_t fg_b);

    // NEW: Render shaped multi-codepoint runs (HarfBuzz + FreeType)
    ShapedGlyphs *(*render_shaped)(FontBackend *font, void *font_data,
                                   uint32_t *codepoints, int codepoint_count,
                                   uint8_t fg_r, uint8_t fg_g, uint8_t fg_b);

    // NEW: Variable font axis control
    bool (*set_variation_axis)(FontBackend *font, void *font_data,
                               const char *axis_tag, float value);

    // NEW: Set multiple axes at once
    bool (*set_variation_axes)(FontBackend *font, void *font_data,
                               float *coords, int num_coords);

    // Get glyph metrics without rendering
    bool (*get_glyph_info)(FontBackend *font, void *font_data, uint32_t codepoint,
                           int *advance, int *left_bearing, int *top_bearing);

    // Free a glyph bitmap
    void (*free_glyph_bitmap)(FontBackend *font, GlyphBitmap *bitmap);

    // Font loading functions
    bool (*load_font)(FontBackend *font, FontStyle style,
                      const char *font_path, float font_size, const FontOptions *options);

    // Get metrics for a specific style
    const FontMetrics *(*get_style_metrics)(FontBackend *font, FontStyle style);

    // Check if a style is loaded
    bool (*has_style)(FontBackend *font, FontStyle style);

    // Check if a loaded style supports COLR (color glyphs)
    bool (*style_has_colr)(FontBackend *font, void *font_data);

    // Get glyph index for a codepoint without rasterizing
    uint32_t (*get_glyph_index)(FontBackend *font, void *font_data, uint32_t codepoint);
};

// Font font API
bool font_init(FontBackend *font);
void font_destroy(FontBackend *font);
bool font_load_font(FontBackend *font, FontStyle style,
                    const char *font_path, float font_size, const FontOptions *options);
const FontMetrics *font_get_metrics(FontBackend *font, FontStyle style);
GlyphBitmap *font_render_glyphs(FontBackend *font, FontStyle style,
                                uint32_t *codepoints, int codepoint_count,
                                uint8_t fg_r, uint8_t fg_g, uint8_t fg_b);

// NEW: Render shaped text (multi-codepoint runs)
ShapedGlyphs *font_render_shaped_text(FontBackend *font, FontStyle style,
                                      uint32_t *codepoints, int count,
                                      uint8_t r, uint8_t g, uint8_t b);

// NEW: Set variable font axis value
bool font_set_variation_axis(FontBackend *font, FontStyle style,
                             const char *axis_tag, float value);

// NEW: Set multiple axis coordinates at once
bool font_set_variation_axes(FontBackend *font, FontStyle style,
                             float *coords, int num_coords);

bool font_has_style(FontBackend *font, FontStyle style);
uint32_t font_get_glyph_index(FontBackend *font, FontStyle style, uint32_t codepoint);

// NEW: Check if a loaded style supports COLR (color glyphs)
bool font_style_has_colr(FontBackend *font, FontStyle style);

#endif // FONT_H
