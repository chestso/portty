#ifndef FONT_FT_INTERNAL_H
#define FONT_FT_INTERNAL_H

#include <ft2build.h>
#include FT_FREETYPE_H
#include <freetype2/freetype/ftcolor.h>
#include FT_MULTIPLE_MASTERS_H
#include <hb.h>
#include <stdbool.h>
#include <stdint.h>

#include "font.h"

// Internal structure for FreeType font data
typedef struct
{
    FT_Face ft_face;          // FreeType face
    hb_font_t *hb_font;       // HarfBuzz font wrapper
    float font_size;          // Requested font size in pixels (default 14pt)
    float scale;              // Scale factor for this font size
    unsigned char *font_data; // Raw font file data (if loaded from memory)
    size_t font_data_size;    // Size of font data
    FontStyle style;          // The style this font was loaded for
    float default_weight;     // Default weight for this font
    float min_weight;         // Minimum weight for this font
    float max_weight;         // Maximum weight for this font

    // Variable font support
    FT_MM_Var *mm_var; // Cached MM_Var for performance
    int num_axes;
    struct
    {
        FT_ULong tag;
        char name[64];
        float min_value;
        float default_value;
        float max_value;
        float current_value;
    } *axes;

    bool has_colr;     // Whether the font has COLR table
    FT_Color *palette; // COLR palette data (if any)
    FT_UShort palette_size;

    // Font rendering options from Fontconfig
    bool antialias;
    int hinting;
    int hint_style;
    int subpixel_order;
    int lcd_filter;
    int ft_load_flags; // FreeType load flags
    int dpi_x;         // Horizontal DPI for HiDPI support
    int dpi_y;         // Vertical DPI for HiDPI support
} FtFontData;

// Shared functions used by colr_paint.c
void resolve_colorindex(FtFontData *ft_data, FT_ColorIndex ci, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                        uint8_t *out_r, uint8_t *out_g, uint8_t *out_b, uint8_t *out_a);

unsigned char *rasterize_glyph_mask(FtFontData *ft_data, FT_UInt glyph_index,
                                    int *out_w, int *out_h, int *out_left, int *out_top);

GlyphBitmap *rasterize_glyph_index(FtFontData *ft_data, FT_UInt glyph_index,
                                   uint8_t fg_r, uint8_t fg_g, uint8_t fg_b);

// COLR v1 custom paint entry point (defined in colr_paint.c)
GlyphBitmap *render_colr_paint_glyph(FtFontData *ft_data, FT_UInt glyph_index,
                                     uint8_t fg_r, uint8_t fg_g, uint8_t fg_b);

#endif // FONT_FT_INTERNAL_H
