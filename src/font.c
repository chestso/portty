#include "font.h"
#include "common.h"
#include <string.h>

// Initialize the font font
bool font_init(FontBackend *font)
{
    if (!font) {
        return false;
    }

    font->loaded_styles = 0;

    // Initialize all font data pointers to NULL
    for (int i = 0; i < FONT_STYLE_COUNT; i++) {
        font->font_data[i] = NULL;
        memset(&font->metrics[i], 0, sizeof(FontMetrics));
    }

    return true;
}

// Destroy the font font
void font_destroy(FontBackend *font)
{
    if (!font)
        return;

    // Destroy all loaded font data
    for (FontStyle style = FONT_STYLE_NORMAL; style < FONT_STYLE_COUNT; style++) {
        if (font->font_data[style]) {
            font->destroy_font(font, font->font_data[style]);
            font->font_data[style] = NULL;
        }
    }
}

// Load a font with a specific style
bool font_load_font(FontBackend *font, FontStyle style,
                    const char *font_path, float font_size, const FontOptions *options)
{
    if (!font || style >= FONT_STYLE_COUNT || !font_path) {
        return false;
    }

    // Unload existing font of this style if present
    if (font->font_data[style]) {
        font->destroy_font(font, font->font_data[style]);
        font->font_data[style] = NULL;
        font->loaded_styles &= ~(1u << style);
    }

    // Load the font
    void *font_data = font->init_font(font, font_path,
                                      font_size, style, options);
    if (!font_data) {
        return false;
    }

    // Get font metrics
    if (!font->get_metrics(font, font_data,
                           &font->metrics[style])) {
        font->destroy_font(font, font_data);
        return false;
    }

    font->font_data[style] = font_data;
    font->loaded_styles |= (1u << style);

    vlog("Loaded font style %d from %s\n", style, font_path);

    return true;
}

// Get metrics for a specific style
const FontMetrics *font_get_metrics(FontBackend *font, FontStyle style)
{
    if (!font || style >= FONT_STYLE_COUNT ||
        !(font->loaded_styles & (1u << style))) {
        return NULL;
    }

    return &font->metrics[style];
}

// Unified glyph renderer for single and multiple codepoints
GlyphBitmap *font_render_glyphs(FontBackend *font, FontStyle style,
                                uint32_t *codepoints, int codepoint_count,
                                uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!font || style >= FONT_STYLE_COUNT ||
        !(font->loaded_styles & (1u << style)) || !codepoints || codepoint_count <= 0) {
        return NULL;
    }

    return font->render_glyphs(font, font->font_data[style],
                               codepoints, codepoint_count, fg_r, fg_g, fg_b);
}

// Render shaped text (multi-codepoint runs)
ShapedGlyphs *font_render_shaped_text(FontBackend *font, FontStyle style,
                                      uint32_t *codepoints, int count,
                                      uint8_t r, uint8_t g, uint8_t b)
{
    if (!font || style >= FONT_STYLE_COUNT ||
        !(font->loaded_styles & (1u << style)) || !codepoints || count <= 0) {
        return NULL;
    }

    if (!font->render_shaped)
        return NULL;

    return font->render_shaped(font, font->font_data[style], codepoints, count, r, g, b);
}

// Set variable font axis value
bool font_set_variation_axis(FontBackend *font, FontStyle style,
                             const char *axis_tag, float value)
{
    if (!font || style >= FONT_STYLE_COUNT ||
        !(font->loaded_styles & (1u << style)) || !axis_tag) {
        return false;
    }

    if (!font->set_variation_axis)
        return false;

    return font->set_variation_axis(font, font->font_data[style], axis_tag, value);
}

// Set multiple variation axis coordinates
bool font_set_variation_axes(FontBackend *font, FontStyle style,
                             float *coords, int num_coords)
{
    if (!font || style >= FONT_STYLE_COUNT ||
        !(font->loaded_styles & (1u << style)) || !coords || num_coords <= 0) {
        return false;
    }

    if (!font->set_variation_axes)
        return false;

    return font->set_variation_axes(font, font->font_data[style], coords, num_coords);
}

// Get glyph index for a codepoint without rasterizing
uint32_t font_get_glyph_index(FontBackend *font, FontStyle style, uint32_t codepoint)
{
    if (!font || style >= FONT_STYLE_COUNT ||
        !(font->loaded_styles & (1u << style)) || !font->get_glyph_index) {
        return 0;
    }
    return font->get_glyph_index(font, font->font_data[style], codepoint);
}

// Render a single glyph by its font glyph index
GlyphBitmap *font_render_glyph_id(FontBackend *font, FontStyle style,
                                  uint32_t glyph_id,
                                  uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!font || style >= FONT_STYLE_COUNT ||
        !(font->loaded_styles & (1u << style)) || !font->render_glyph_id) {
        return NULL;
    }
    return font->render_glyph_id(font, font->font_data[style], glyph_id, fg_r, fg_g, fg_b);
}

// Check if a style is loaded
bool font_has_style(FontBackend *font, FontStyle style)
{
    return font && style < FONT_STYLE_COUNT &&
           (font->loaded_styles & (1u << style));
}

// Check if a loaded style supports COLR (color glyphs)
bool font_style_has_colr(FontBackend *font, FontStyle style)
{
    if (!font || style >= FONT_STYLE_COUNT || !(font->loaded_styles & (1u << style)))
        return false;
    if (!font->style_has_colr)
        return false;
    return font->style_has_colr(font, font->font_data[style]);
}

// Set target cell width on all loaded font styles
void font_set_target_cell_width(FontBackend *font, int cell_width)
{
    if (!font || !font->set_target_cell_width)
        return;
    for (int i = 0; i < FONT_STYLE_COUNT; i++) {
        if (font->font_data[i])
            font->set_target_cell_width(font->font_data[i], cell_width);
    }
}

// Set per-render presentation width on a single font style
void font_set_presentation_width(FontBackend *font, FontStyle style, int width)
{
    if (!font || !font->set_presentation_width)
        return;
    if (style < FONT_STYLE_COUNT && font->font_data[style])
        font->set_presentation_width(font->font_data[style], width);
}
