#define FT_CONFIG_OPTION_SUBPIXEL_RENDERING
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include <freetype2/freetype/ftcolor.h>
#include FT_MULTIPLE_MASTERS_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include "common.h"
#include "font.h"
#include <fontconfig/fontconfig.h>
#include <hb-ft.h>
#include <hb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Internal structure for FreeType/Cairo font data
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

// Load font file into memory
static bool load_font_file(const char *font_path, unsigned char **data, size_t *size)
{
    FILE *file = fopen(font_path, "rb");
    if (!file) {
        vlog("Failed to open font file: %s\n", font_path);
        return false;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(file);
        vlog("Invalid font file size: %s\n", font_path);
        return false;
    }

    *data = malloc(file_size);
    if (!*data) {
        fclose(file);
        vlog("Failed to allocate memory for font data: %s\n", font_path);
        return false;
    }

    size_t bytes_read = fread(*data, 1, file_size, file);
    fclose(file);

    if (bytes_read != (size_t)file_size) {
        free(*data);
        *data = NULL;
        vlog("Failed to read font file: %s\n", font_path);
        return false;
    }

    *size = bytes_read;
    return true;
}

// Helper function to free a glyph bitmap
static void ft_free_glyph_bitmap(Font *font, GlyphBitmap *bitmap)
{
    (void)font; // Unused

    if (!bitmap)
        return;

    free(bitmap->pixels);
    free(bitmap);
}

// Cache MM_Var and axis info for a face
static void cache_mm_var(FtFontData *ft_data)
{
    if (!ft_data || !ft_data->ft_face)
        return;

    if (ft_data->mm_var)
        return; // already cached

    FT_MM_Var *mm_var = NULL;
    FT_Error err = FT_Get_MM_Var(ft_data->ft_face, &mm_var);
    if (err != 0 || !mm_var)
        return;

    ft_data->mm_var = mm_var;
    ft_data->num_axes = (int)mm_var->num_axis;

    ft_data->axes = calloc(mm_var->num_axis, sizeof(*ft_data->axes));
    if (!ft_data->axes) {
        FT_Done_MM_Var(ft_data->ft_face->glyph->library, mm_var);
        ft_data->mm_var = NULL;
        ft_data->num_axes = 0;
        return;
    }

    for (FT_UInt i = 0; i < mm_var->num_axis; i++) {
        ft_data->axes[i].tag = mm_var->axis[i].tag;
        ft_data->axes[i].min_value = (float)mm_var->axis[i].minimum / 65536.0f;
        ft_data->axes[i].default_value = (float)mm_var->axis[i].def / 65536.0f;
        ft_data->axes[i].max_value = (float)mm_var->axis[i].maximum / 65536.0f;
        ft_data->axes[i].current_value = ft_data->axes[i].default_value;
        // axis name resolution omitted for brevity
    }
}

// Apply font variations based on style (uses cached mm_var)
static void apply_font_variations(FtFontData *ft_data, FontStyle style)
{
    if (!ft_data || !ft_data->ft_face)
        return;

    cache_mm_var(ft_data);
    if (!ft_data->mm_var)
        return; // not variable

    FT_MM_Var *mm_var = ft_data->mm_var;
    vlog("Font is a variable font with %d axes\n", mm_var->num_axis);

    // Store weight information for later use
    ft_data->default_weight = 400.0; // Default weight
    ft_data->min_weight = 100.0;     // Minimum weight
    ft_data->max_weight = 900.0;     // Maximum weight

    // Try to find the 'wght' (weight) axis to get actual values
    for (FT_UInt i = 0; i < mm_var->num_axis; i++) {
        if (mm_var->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
            ft_data->default_weight = (float)mm_var->axis[i].def / 65536.0f;
            ft_data->min_weight = (float)mm_var->axis[i].minimum / 65536.0f;
            ft_data->max_weight = (float)mm_var->axis[i].maximum / 65536.0f;
            vlog("Stored font weight info: min=%f, default=%f, max=%f\n",
                 ft_data->min_weight, ft_data->default_weight, ft_data->max_weight);
            break;
        }
    }

    // Create an array of design coordinates for all axes
    FT_Fixed *coords = malloc(mm_var->num_axis * sizeof(FT_Fixed));
    if (!coords) {
        vlog("Failed to allocate memory for font variations\n");
        return;
    }

    // Initialize all coordinates to their default or cached current values
    for (FT_UInt i = 0; i < mm_var->num_axis; i++) {
        float val = ft_data->axes ? ft_data->axes[i].current_value : (float)mm_var->axis[i].def / 65536.0f;
        coords[i] = (FT_Fixed)(val * 65536.0f);
    }

    // Handle bold style by nudging 'wght' if available
    if (style == FONT_STYLE_BOLD) {
        for (FT_UInt i = 0; i < mm_var->num_axis; i++) {
            if (mm_var->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
                float default_weight = (float)mm_var->axis[i].def / 65536.0f;
                float min_weight = (float)mm_var->axis[i].minimum / 65536.0f;
                float max_weight = (float)mm_var->axis[i].maximum / 65536.0f;
                if (max_weight > default_weight && max_weight - min_weight > 100.0) {
                    float bold_weight = default_weight + (max_weight - default_weight) * 0.9f;
                    coords[i] = (FT_Fixed)(bold_weight * 65536.0f);
                    ft_data->axes[i].current_value = bold_weight;
                    vlog("Set 'wght' axis to %f for bold style\n", bold_weight);
                } else {
                    coords[i] = (FT_Fixed)(700.0f * 65536.0f);
                    ft_data->axes[i].current_value = 700.0f;
                    vlog("Set 'wght' axis to 700.0 (fallback) for bold style\n");
                }
                break;
            }
        }
    }

    // Apply coordinates to FreeType
    FT_Error ft_error = FT_Set_Var_Design_Coordinates(ft_data->ft_face, mm_var->num_axis, coords);
    if (ft_error == 0) {
        vlog("Successfully applied FreeType design coordinates\n");
        if (ft_data->hb_font)
            hb_ft_font_changed(ft_data->hb_font);
    } else {
        vlog("Failed to apply FreeType design coordinates: error %d\n", ft_error);
    }

    free(coords);
}

// Check for COLR table in the font
static bool check_colr_table(FT_Face face)
{
    // Try to load the COLR table
    FT_ULong colr_len = 0;
    FT_Error error = FT_Load_Sfnt_Table(face, FT_MAKE_TAG('C', 'O', 'L', 'R'), 0, NULL, &colr_len);
    if (error == 0 && colr_len > 0) {
        vlog("Font has COLR table (length: %lu)\n", colr_len);
        return true;
    }

    vlog("Font does not have COLR table (error: %d)\n", error);
    return false;
}

// Check and load COLR palette
static bool load_colr_palette(FtFontData *ft_data)
{
    if (!ft_data || !ft_data->ft_face)
        return false;

    FT_Palette_Data palette_data;
    FT_Error err = FT_Palette_Data_Get(ft_data->ft_face, &palette_data);
    if (err != 0)
        return false;

    if (palette_data.num_palette_entries == 0)
        return false;

    FT_Color *palette = NULL;
    err = FT_Palette_Select(ft_data->ft_face, 0, &palette);
    if (err != 0 || !palette)
        return false;

    // FT provides palette pointer ownership; store for read-only use
    ft_data->palette = palette;
    ft_data->palette_size = palette_data.num_palette_entries;
    return true;
}

// Render COLR glyph layers into RGBA bitmap
static GlyphBitmap *render_colr_glyph(FtFontData *ft_data, FT_UInt glyph_index,
                                      uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!ft_data || !ft_data->ft_face)
        return NULL;

    FT_Face face = ft_data->ft_face;

    // Get advance from base glyph if available
    int advance = 0;
    if (FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT) == 0) {
        advance = (int)(face->glyph->advance.x >> 6);
    }

    // Iterate layers
    FT_LayerIterator iterator;
    iterator.p = NULL;
    FT_UInt layer_glyph = 0;
    FT_UInt layer_color_index = 0;

    typedef struct
    {
        FT_Bitmap bmp;
        int left;
        int top;
        FT_Color color;
    } LayerBmp;
    LayerBmp *layers = NULL;
    int layer_count = 0;

    while (FT_Get_Color_Glyph_Layer(face, glyph_index, &layer_glyph, &layer_color_index, &iterator)) {
        if (layer_glyph == 0)
            break;

        FT_Int32 load_flags = FT_LOAD_DEFAULT;
        if (ft_data->antialias)
            load_flags |= FT_LOAD_TARGET_NORMAL;
        else
            load_flags |= FT_LOAD_TARGET_MONO;

        if (FT_Load_Glyph(face, layer_glyph, load_flags) != 0)
            continue;
        if (FT_Render_Glyph(face->glyph, ft_data->antialias ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO) != 0)
            continue;

        FT_Bitmap *bmp = &face->glyph->bitmap;
        int left = face->glyph->bitmap_left;
        int top = face->glyph->bitmap_top;

        LayerBmp *nl = realloc(layers, sizeof(LayerBmp) * (layer_count + 1));
        if (!nl)
            break;
        layers = nl;

        LayerBmp *lb = &layers[layer_count];
        memset(lb, 0, sizeof(LayerBmp));

        lb->bmp = *bmp;
        size_t buf_size = (size_t)bmp->rows * (size_t)bmp->pitch;
        lb->bmp.buffer = malloc(buf_size);
        if (!lb->bmp.buffer)
            break;
        memcpy(lb->bmp.buffer, bmp->buffer, buf_size);
        lb->left = left;
        lb->top = top;

        // Determine color (FT_Color is BGRA)
        FT_Color c = { 0, 0, 0, 255 };
        if (layer_color_index == 0xFFFF) {
            c.red = fg_r;
            c.green = fg_g;
            c.blue = fg_b;
            c.alpha = 255;
        } else if (ft_data->palette && layer_color_index < ft_data->palette_size) {
            c = ft_data->palette[layer_color_index];
        }
        lb->color = c;

        layer_count++;
    }

    if (layer_count == 0) {
        if (layers)
            free(layers);
        return NULL;
    }

    // Compute bounding box
    int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
    for (int i = 0; i < layer_count; i++) {
        LayerBmp *lb = &layers[i];
        int l = lb->left;
        int t = lb->top - lb->bmp.rows;
        int r = lb->left + lb->bmp.width;
        int b = lb->top;
        if (l < min_x)
            min_x = l;
        if (t < min_y)
            min_y = t;
        if (r > max_x)
            max_x = r;
        if (b > max_y)
            max_y = b;
    }

    int out_w = max_x - min_x;
    int out_h = max_y - min_y;
    if (out_w <= 0 || out_h <= 0) {
        for (int i = 0; i < layer_count; i++)
            free(layers[i].bmp.buffer);
        free(layers);
        return NULL;
    }

    GlyphBitmap *out = malloc(sizeof(GlyphBitmap));
    if (!out) {
        for (int i = 0; i < layer_count; i++)
            free(layers[i].bmp.buffer);
        free(layers);
        return NULL;
    }
    out->width = out_w;
    out->height = out_h;
    out->x_offset = min_x;
    out->y_offset = max_y; // baseline distance
    out->advance = advance;
    out->pixels = calloc(out_w * out_h, 4);
    out->glyph_id = glyph_index;
    if (!out->pixels) {
        free(out);
        for (int i = 0; i < layer_count; i++)
            free(layers[i].bmp.buffer);
        free(layers);
        return NULL;
    }

    // Composite layers (palette FT_Color is BGRA)
    for (int i = 0; i < layer_count; i++) {
        LayerBmp *lb = &layers[i];
        FT_Bitmap *bmp = &lb->bmp;
        int ox = lb->left - min_x;
        int oy = max_y - lb->top;
        uint8_t pr = lb->color.red;
        uint8_t pg = lb->color.green;
        uint8_t pb = lb->color.blue;
        uint8_t pa = lb->color.alpha;

        for (unsigned int y = 0; y < bmp->rows; y++) {
            unsigned char *src = bmp->buffer + y * bmp->pitch;
            for (unsigned int x = 0; x < bmp->width; x++) {
                unsigned char a = src[x];
                if (a == 0)
                    continue;
                int dx = ox + x;
                int dy = oy + y;
                if (dx < 0 || dx >= out_w || dy < 0 || dy >= out_h)
                    continue;

                uint8_t src_a = (uint8_t)((a * pa) / 255);
                uint8_t *dst = &out->pixels[(dy * out_w + dx) * 4];
                uint8_t dst_r = dst[0], dst_g = dst[1], dst_b = dst[2], dst_a = dst[3];

                float sa = src_a / 255.0f;
                float da = dst_a / 255.0f;
                float outa = sa + da * (1.0f - sa);
                if (outa <= 0.0f)
                    continue;

                float sr = pr / 255.0f;
                float sg = pg / 255.0f;
                float sb = pb / 255.0f;
                float dr = dst_r / 255.0f;
                float dg = dst_g / 255.0f;
                float db = dst_b / 255.0f;

                float rr = (sr * sa + dr * da * (1.0f - sa)) / outa;
                float rg = (sg * sa + dg * da * (1.0f - sa)) / outa;
                float rb = (sb * sa + db * da * (1.0f - sa)) / outa;

                dst[0] = (uint8_t)(rr * 255.0f);
                dst[1] = (uint8_t)(rg * 255.0f);
                dst[2] = (uint8_t)(rb * 255.0f);
                dst[3] = (uint8_t)(outa * 255.0f);
            }
        }
    }

    for (int i = 0; i < layer_count; i++)
        free(layers[i].bmp.buffer);
    free(layers);

    return out;
}

// Helper: convert 16.16 fixed to double
static double ft_fixed_to_double(FT_Fixed v)
{
    return (double)v / 65536.0;
}

// Helper: convert 26.6 fixed (FT_Pos) to double
static double ft_pos_to_double(FT_Pos v)
{
    return (double)v / 64.0;
}

// Simple 2x3 affine matrix for transforms (row-major)
typedef struct
{
    double xx, xy, dx;
    double yx, yy, dy;
} Affine;

static void affine_identity(Affine *a)
{
    a->xx = 1.0;
    a->xy = 0.0;
    a->dx = 0.0;
    a->yx = 0.0;
    a->yy = 1.0;
    a->dy = 0.0;
}

static void affine_from_FT_Affine23(Affine *out, const FT_Affine23 *in)
{
    if (!out || !in)
        return;
    out->xx = ft_fixed_to_double(in->xx);
    out->xy = ft_fixed_to_double(in->xy);
    out->dx = ft_fixed_to_double(in->dx);
    out->yx = ft_fixed_to_double(in->yx);
    out->yy = ft_fixed_to_double(in->yy);
    out->dy = ft_fixed_to_double(in->dy);
}

static void affine_mul(Affine *out, const Affine *a, const Affine *b)
{
    // out = a * b
    Affine r;
    r.xx = a->xx * b->xx + a->xy * b->yx;
    r.xy = a->xx * b->xy + a->xy * b->yy;
    r.dx = a->xx * b->dx + a->xy * b->dy + a->dx;

    r.yx = a->yx * b->xx + a->yy * b->yx;
    r.yy = a->yx * b->xy + a->yy * b->yy;
    r.dy = a->yx * b->dx + a->yy * b->dy + a->dy;

    *out = r;
}

static void affine_apply(const Affine *a, double x, double y, double *rx, double *ry)
{
    if (!a) {
        *rx = x;
        *ry = y;
        return;
    }
    *rx = x * a->xx + y * a->xy + a->dx;
    *ry = x * a->yx + y * a->yy + a->dy;
}

// Affine helpers for Translate/Scale/Rotate/Skew with center handling
static void affine_from_translate(Affine *out, double dx, double dy)
{
    affine_identity(out);
    out->dx = dx;
    out->dy = dy;
}

static void affine_from_scale_center(Affine *out, double sx, double sy, double cx, double cy)
{
    out->xx = sx;
    out->xy = 0.0;
    out->dx = cx - sx * cx;
    out->yx = 0.0;
    out->yy = sy;
    out->dy = cy - sy * cy;
}

static void affine_from_rotate_center(Affine *out, double angle_rad, double cx, double cy)
{
    double c = cos(angle_rad);
    double s = sin(angle_rad);
    out->xx = c;
    out->xy = -s;
    out->dx = cx - (c * cx - s * cy);
    out->yx = s;
    out->yy = c;
    out->dy = cy - (s * cx + c * cy);
}

static void affine_from_skew_center(Affine *out, double x_skew_rad, double y_skew_rad, double cx, double cy)
{
    double tx = tan(x_skew_rad);
    double ty = tan(y_skew_rad);
    out->xx = 1.0;
    out->xy = tx;
    out->dx = cx - (1.0 * cx + tx * cy);
    out->yx = ty;
    out->yy = 1.0;
    out->dy = cy - (ty * cx + 1.0 * cy);
}

// Rasterize glyph into single-channel alpha mask via FT_Get_Glyph + FT_Glyph_To_Bitmap
static unsigned char *rasterize_glyph_mask(FtFontData *ft_data, FT_UInt glyph_index,
                                           int *out_w, int *out_h, int *out_left, int *out_top)
{
    if (!ft_data || !ft_data->ft_face)
        return NULL;
    FT_Face face = ft_data->ft_face;

    FT_Int32 load_flags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP;
    if (!ft_data->hinting)
        load_flags |= FT_LOAD_NO_HINTING;

    FT_Error err = FT_Load_Glyph(face, glyph_index, load_flags);
    if (err)
        return NULL;

    FT_Glyph glyph = NULL;
    err = FT_Get_Glyph(face->glyph, &glyph);
    if (err || !glyph)
        return NULL;

    // Convert glyph to bitmap (keeps hinting/size effects)
    err = FT_Glyph_To_Bitmap(&glyph, ft_data->antialias ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO, NULL, 1);
    if (err) {
        FT_Done_Glyph(glyph);
        return NULL;
    }

    FT_BitmapGlyph bmp_glyph = (FT_BitmapGlyph)glyph;
    FT_Bitmap *bitmap = &bmp_glyph->bitmap;
    int w = bitmap->width;
    int h = bitmap->rows;
    int left = bmp_glyph->left;
    int top = bmp_glyph->top;

    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;
    if (out_left)
        *out_left = left;
    if (out_top)
        *out_top = top;
    if (w <= 0 || h <= 0) {
        FT_Done_Glyph(glyph);
        return NULL;
    }

    unsigned char *mask = malloc((size_t)w * (size_t)h);
    if (!mask) {
        FT_Done_Glyph(glyph);
        return NULL;
    }

    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (int y = 0; y < h; y++)
            memcpy(mask + y * w, bitmap->buffer + y * bitmap->pitch, w);
    } else if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        for (int y = 0; y < h; y++) {
            unsigned char *src = bitmap->buffer + y * bitmap->pitch;
            for (int x = 0; x < w; x++) {
                unsigned char byte = src[x >> 3];
                unsigned char bit = (byte >> (7 - (x & 7))) & 1;
                mask[y * w + x] = bit ? 255 : 0;
            }
        }
    } else if (bitmap->pixel_mode == FT_PIXEL_MODE_BGRA) {
        for (int y = 0; y < h; y++) {
            unsigned char *src = bitmap->buffer + y * bitmap->pitch;
            for (int x = 0; x < w; x++)
                mask[y * w + x] = src[x * 4 + 3];
        }
    } else {
        free(mask);
        FT_Done_Glyph(glyph);
        return NULL;
    }

    FT_Done_Glyph(glyph);
    return mask;
}

// Resolve an FT_ColorIndex to RGBA (0..255)
static void resolve_colorindex(FtFontData *ft_data, FT_ColorIndex ci, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                               uint8_t *out_r, uint8_t *out_g, uint8_t *out_b, uint8_t *out_a)
{
    // Default foreground
    uint8_t pr = fg_r, pg = fg_g, pb = fg_b, pa = 255;
    if (ci.palette_index == 0xFFFFu) {
        // text foreground color
        pr = fg_r;
        pg = fg_g;
        pb = fg_b;
        pa = 255;
    } else if (ft_data->palette && ci.palette_index < ft_data->palette_size) {
        FT_Color c = ft_data->palette[ci.palette_index];
        pr = c.red;
        pg = c.green;
        pb = c.blue;
        pa = c.alpha;
    }

    // Apply alpha from FT_F2Dot14
    double a_scale = 1.0;
    if (ci.alpha) {
        a_scale = (double)ci.alpha / (double)(1 << 14);
        if (a_scale < 0.0)
            a_scale = 0.0;
        if (a_scale > 1.0)
            a_scale = 1.0;
    }

    *out_r = pr;
    *out_g = pg;
    *out_b = pb;
    *out_a = (uint8_t)round(pa * a_scale);
}

// Linear interpolation between two colors (RGBA 0..255)
static void lerp_color(uint8_t *out, uint8_t a_r, uint8_t a_g, uint8_t a_b, uint8_t a_a,
                       uint8_t b_r, uint8_t b_g, uint8_t b_b, uint8_t b_a, double t)
{
    if (t <= 0.0) {
        out[0] = a_r;
        out[1] = a_g;
        out[2] = a_b;
        out[3] = a_a;
        return;
    }
    if (t >= 1.0) {
        out[0] = b_r;
        out[1] = b_g;
        out[2] = b_b;
        out[3] = b_a;
        return;
    }
    out[0] = (uint8_t)round((1.0 - t) * a_r + t * b_r);
    out[1] = (uint8_t)round((1.0 - t) * a_g + t * b_g);
    out[2] = (uint8_t)round((1.0 - t) * a_b + t * b_b);
    out[3] = (uint8_t)round((1.0 - t) * a_a + t * b_a);
}

// Evaluate colorline stops into an array of stops (small dynamic array)
typedef struct
{
    double offset;
    uint8_t r, g, b, a;
} Stop;

static Stop *eval_colorline(FtFontData *ft_data, FT_ColorLine *cline, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b, int *out_count)
{
    if (!cline || !out_count)
        return NULL;
    FT_ColorStopIterator it = cline->color_stop_iterator;
    FT_ColorStop stop;
    Stop *stops = NULL;
    int count = 0;
    while (FT_Get_Colorline_Stops(ft_data->ft_face, &stop, &it)) {
        Stop *n = realloc(stops, sizeof(Stop) * (count + 1));
        if (!n)
            break;
        stops = n;
        uint8_t r, g, b, a;
        resolve_colorindex(ft_data, stop.color, fg_r, fg_g, fg_b, &r, &g, &b, &a);
        stops[count].offset = ft_fixed_to_double(stop.stop_offset); // 16.16 -> double
        stops[count].r = r;
        stops[count].g = g;
        stops[count].b = b;
        stops[count].a = a;
        count++;
    }
    *out_count = count;
    return stops;
}

// Paint a linear gradient into an RGBA buffer covering bbox (in pixels).
// p0,p1,p2 are in FT 16.16 font units and will be transformed and scaled to pixel space.
static void paint_linear_gradient(FtFontData *ft_data, FT_PaintLinearGradient *lg,
                                  Affine *matrix, bool matrix_maps_font_units, uint8_t *buf, int w, int h,
                                  int dst_x_off, int dst_y_off, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!ft_data || !lg || !buf)
        return;

    double scale_factor = matrix_maps_font_units ? 1.0 : ft_data->scale;
    // Convert p0,p1 from 16.16 font-units to device space using scale_factor
    double p0x = ft_fixed_to_double(lg->p0.x) * scale_factor;
    double p0y = ft_fixed_to_double(lg->p0.y) * scale_factor;
    double p1x = ft_fixed_to_double(lg->p1.x) * scale_factor;
    double p1y = ft_fixed_to_double(lg->p1.y) * scale_factor;
    // p2 optional used to rotate; for now ignore p2 (basic linear gradient)

    // Apply affine transform if present
    double tp0x, tp0y, tp1x, tp1y;
    affine_apply(matrix, p0x, p0y, &tp0x, &tp0y);
    affine_apply(matrix, p1x, p1y, &tp1x, &tp1y);

    // Direction vector
    double dx = tp1x - tp0x;
    double dy = tp1y - tp0y;
    double len2 = dx * dx + dy * dy;
    if (len2 <= 1e-8)
        len2 = 1e-8;

    // Evaluate color stops
    int stop_count = 0;
    Stop *stops = eval_colorline(ft_data, &lg->colorline, fg_r, fg_g, fg_b, &stop_count);
    if (!stops || stop_count == 0) {
        if (stops)
            free(stops);
        return;
    }

    // For each pixel in bbox, compute projection t along gradient (normalized by distance between p0 and p1)
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // pixel center coordinates in destination space
            double px = (double)(x + dst_x_off);
            double py = (double)(dst_y_off - y); // Y-flip: convert from pixel Y-down to FreeType Y-up
            // vector from p0 to pixel
            double vx = px - tp0x;
            double vy = py - tp0y;
            double t = (vx * dx + vy * dy) / len2; // can be <0 or >1

            // Handle extend modes: only PAD for now
            if (t < 0.0) {
                t = 0.0;
            }
            if (t > 1.0) {
                t = 1.0;
            }

            // Find stops surrounding t
            int idx = 0;
            while (idx < stop_count && stops[idx].offset < t)
                idx++;
            uint8_t outc[4] = { 0, 0, 0, 0 };
            if (idx == 0) {
                outc[0] = stops[0].r;
                outc[1] = stops[0].g;
                outc[2] = stops[0].b;
                outc[3] = stops[0].a;
            } else if (idx >= stop_count) {
                outc[0] = stops[stop_count - 1].r;
                outc[1] = stops[stop_count - 1].g;
                outc[2] = stops[stop_count - 1].b;
                outc[3] = stops[stop_count - 1].a;
            } else {
                double t0 = stops[idx - 1].offset;
                double t1 = stops[idx].offset;
                double local = (t1 - t0) == 0.0 ? 0.0 : (t - t0) / (t1 - t0);
                lerp_color(outc, stops[idx - 1].r, stops[idx - 1].g, stops[idx - 1].b, stops[idx - 1].a,
                           stops[idx].r, stops[idx].g, stops[idx].b, stops[idx].a, local);
            }

            uint8_t *pixel = buf + (y * w + x) * 4;
            pixel[0] = outc[0];
            pixel[1] = outc[1];
            pixel[2] = outc[2];
            pixel[3] = outc[3];
        }
    }

    free(stops);
}

static GlyphBitmap *rasterize_glyph_index(FtFontData *ft_data, FT_UInt glyph_index, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b);

// Recursive paint evaluator for FT_COLR v1 paints. This function paints into `buf` (RGBA) of size w*h
// with an origin offset (dst_x_off,dst_y_off) applied to gradient coordinate space. The `opaque` argument
// references the paint table to evaluate.
static bool paint_colr_paint_recursive(FtFontData *ft_data, FT_OpaquePaint opaque, Affine *matrix,
                                       bool matrix_maps_font_units,
                                       uint8_t *buf, int w, int h, int dst_x_off, int dst_y_off,
                                       uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!ft_data || !buf)
        return false;

    FT_COLR_Paint paint;
    if (!FT_Get_Paint(ft_data->ft_face, opaque, &paint))
        return false;

    switch (paint.format) {
    case FT_COLR_PAINTFORMAT_SOLID:
    {
        uint8_t r, g, b, a;
        resolve_colorindex(ft_data, paint.u.solid.color, fg_r, fg_g, fg_b, &r, &g, &b, &a);
        for (int i = 0; i < w * h; i++) {
            buf[i * 4 + 0] = r;
            buf[i * 4 + 1] = g;
            buf[i * 4 + 2] = b;
            buf[i * 4 + 3] = a;
        }
        return true;
    }
    case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
    {
        paint_linear_gradient(ft_data, &paint.u.linear_gradient, matrix, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
        return true;
    }
    case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
    {
        // Radial gradient implementation
        FT_PaintRadialGradient *rg = &paint.u.radial_gradient;
        // Convert centers and radii from 16.16 to pixel space and apply affine
        double scale_factor = matrix_maps_font_units ? 1.0 : ft_data->scale;
        double c0x = ft_fixed_to_double(rg->c0.x) * scale_factor;
        double c0y = ft_fixed_to_double(rg->c0.y) * scale_factor;
        double c1x = ft_fixed_to_double(rg->c1.x) * scale_factor;
        double c1y = ft_fixed_to_double(rg->c1.y) * scale_factor;
        double r0 = ft_fixed_to_double(rg->r0) * scale_factor;
        double r1 = ft_fixed_to_double(rg->r1) * scale_factor;

        double tc0x, tc0y, tc1x, tc1y;
        affine_apply(matrix, c0x, c0y, &tc0x, &tc0y);
        affine_apply(matrix, c1x, c1y, &tc1x, &tc1y);

        int stop_count = 0;
        Stop *stops = eval_colorline(ft_data, &rg->colorline, fg_r, fg_g, fg_b, &stop_count);
        if (!stops || stop_count == 0) {
            if (stops)
                free(stops);
            return false;
        }

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double px = (double)(x + dst_x_off);
                double py = (double)(dst_y_off - y); // Y-flip: convert from pixel Y-down to FreeType Y-up
                // distance to c0
                double d = hypot(px - tc0x, py - tc0y);
                double denom = (r1 - r0);
                double t = denom == 0.0 ? 0.0 : (d - r0) / denom;
                if (t < 0.0) {
                    t = 0.0;
                }
                if (t > 1.0) {
                    t = 1.0;
                }

                int idx = 0;
                while (idx < stop_count && stops[idx].offset < t)
                    idx++;
                uint8_t outc[4];
                if (idx == 0) {
                    outc[0] = stops[0].r;
                    outc[1] = stops[0].g;
                    outc[2] = stops[0].b;
                    outc[3] = stops[0].a;
                } else if (idx >= stop_count) {
                    outc[0] = stops[stop_count - 1].r;
                    outc[1] = stops[stop_count - 1].g;
                    outc[2] = stops[stop_count - 1].b;
                    outc[3] = stops[stop_count - 1].a;
                } else {
                    double t0 = stops[idx - 1].offset;
                    double t1 = stops[idx].offset;
                    double local = (t1 - t0) == 0.0 ? 0.0 : (t - t0) / (t1 - t0);
                    lerp_color(outc, stops[idx - 1].r, stops[idx - 1].g, stops[idx - 1].b, stops[idx - 1].a,
                               stops[idx].r, stops[idx].g, stops[idx].b, stops[idx].a, local);
                }
                uint8_t *pixel = buf + (y * w + x) * 4;
                pixel[0] = outc[0];
                pixel[1] = outc[1];
                pixel[2] = outc[2];
                pixel[3] = outc[3];
            }
        }
        free(stops);
        return true;
    }
    case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT:
    {
        // Sweep gradient implementation
        FT_PaintSweepGradient *sg = &paint.u.sweep_gradient;
        double scale_factor = matrix_maps_font_units ? 1.0 : ft_data->scale;
        double cx = ft_fixed_to_double(sg->center.x) * scale_factor;
        double cy = ft_fixed_to_double(sg->center.y) * scale_factor;
        double tcx, tcy;
        affine_apply(matrix, cx, cy, &tcx, &tcy);

        double start_deg = ft_fixed_to_double(sg->start_angle) * 180.0;
        double end_deg = ft_fixed_to_double(sg->end_angle) * 180.0;

        int stop_count = 0;
        Stop *stops = eval_colorline(ft_data, &sg->colorline, fg_r, fg_g, fg_b, &stop_count);
        if (!stops || stop_count == 0) {
            if (stops)
                free(stops);
            return false;
        }

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                double px = (double)(x + dst_x_off);
                double py = (double)(dst_y_off - y); // Y-flip: convert from pixel Y-down to FreeType Y-up
                double vx = px - tcx;
                double vy = py - tcy;
                // compute angle in degrees measured counter-clockwise from positive y axis
                double ang_rad = atan2(vx, vy); // swap to get from positive y
                double ang_deg = ang_rad * (180.0 / M_PI);
                if (ang_deg < 0)
                    ang_deg += 360.0;

                // Normalize between start_deg and end_deg
                double total = end_deg - start_deg;
                if (total < 0)
                    total += 360.0;
                double rel = ang_deg - start_deg;
                if (rel < 0)
                    rel += 360.0;
                double t = total == 0.0 ? 0.0 : rel / total;
                if (t < 0.0) {
                    t = 0.0;
                }
                if (t > 1.0) {
                    t = 1.0;
                }

                int idx = 0;
                while (idx < stop_count && stops[idx].offset < t)
                    idx++;
                uint8_t outc[4];
                if (idx == 0) {
                    outc[0] = stops[0].r;
                    outc[1] = stops[0].g;
                    outc[2] = stops[0].b;
                    outc[3] = stops[0].a;
                } else if (idx >= stop_count) {
                    outc[0] = stops[stop_count - 1].r;
                    outc[1] = stops[stop_count - 1].g;
                    outc[2] = stops[stop_count - 1].b;
                    outc[3] = stops[stop_count - 1].a;
                } else {
                    double t0 = stops[idx - 1].offset;
                    double t1 = stops[idx].offset;
                    double local = (t1 - t0) == 0.0 ? 0.0 : (t - t0) / (t1 - t0);
                    lerp_color(outc, stops[idx - 1].r, stops[idx - 1].g, stops[idx - 1].b, stops[idx - 1].a,
                               stops[idx].r, stops[idx].g, stops[idx].b, stops[idx].a, local);
                }
                uint8_t *pixel = buf + (y * w + x) * 4;
                pixel[0] = outc[0];
                pixel[1] = outc[1];
                pixel[2] = outc[2];
                pixel[3] = outc[3];
            }
        }
        free(stops);
        return true;
    }
    case FT_COLR_PAINTFORMAT_COMPOSITE:
    {
        FT_PaintComposite *pc = &paint.u.composite;
        uint8_t *tmp_back = calloc((size_t)w * (size_t)h, 4);
        uint8_t *tmp_src = calloc((size_t)w * (size_t)h, 4);
        if (!tmp_back || !tmp_src) {
            if (tmp_back)
                free(tmp_back);
            if (tmp_src)
                free(tmp_src);
            return false;
        }

        // Evaluate backdrop and source paints into temporary buffers
        paint_colr_paint_recursive(ft_data, pc->backdrop_paint, matrix, matrix_maps_font_units, tmp_back, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
        paint_colr_paint_recursive(ft_data, pc->source_paint, matrix, matrix_maps_font_units, tmp_src, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);

        switch (pc->composite_mode) {
        case FT_COLR_COMPOSITE_SRC_OVER:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa + br * ba * (1.0f - sa)) / outa;
                float rg = (sg * sa + bg * ba * (1.0f - sa)) / outa;
                float rb = (sb * sa + bb * ba * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_PLUS:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                int r = s[0] + b[0];
                int g = s[1] + b[1];
                int bl = s[2] + b[2];
                int a = s[3] + b[3];
                dst[0] = (uint8_t)(r > 255 ? 255 : r);
                dst[1] = (uint8_t)(g > 255 ? 255 : g);
                dst[2] = (uint8_t)(bl > 255 ? 255 : bl);
                dst[3] = (uint8_t)(a > 255 ? 255 : a);
            }
            break;
        }
        case FT_COLR_COMPOSITE_MULTIPLY:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                float rr = (sa * (1.0f - ba) * sr + sa * ba * (sr * br) + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * (sg * bg) + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * (sb * bb) + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SCREEN:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                // Screen: B(Cs,Cb) = Cs + Cb - Cs*Cb
                float Br = sr + br - sr * br;
                float Bg = sg + bg - sg * bg;
                float Bb = sb + bb - sb * bb;
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_OVERLAY:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                // Overlay: B(Cs,Cb) = HardLight(Cb,Cs)
                float Br = br < 0.5f ? 2.0f * sr * br : 1.0f - 2.0f * (1.0f - sr) * (1.0f - br);
                float Bg = bg < 0.5f ? 2.0f * sg * bg : 1.0f - 2.0f * (1.0f - sg) * (1.0f - bg);
                float Bb = bb < 0.5f ? 2.0f * sb * bb : 1.0f - 2.0f * (1.0f - sb) * (1.0f - bb);
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_DARKEN:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                float Br = sr < br ? sr : br;
                float Bg = sg < bg ? sg : bg;
                float Bb = sb < bb ? sb : bb;
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_LIGHTEN:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                float Br = sr > br ? sr : br;
                float Bg = sg > bg ? sg : bg;
                float Bb = sb > bb ? sb : bb;
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SOFT_LIGHT:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f, ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                // SoftLight: if Cs <= 0.5: B = Cb - (1-2*Cs)*Cb*(1-Cb)
                //            else: B = Cb + (2*Cs-1)*(D(Cb)-Cb)
                //   where D(Cb) = sqrt(Cb) if Cb > 0.25
                //                  ((16*Cb-12)*Cb+4)*Cb otherwise
                float Dr, Dg, Db;
                Dr = br > 0.25f ? sqrtf(br) : ((16.0f * br - 12.0f) * br + 4.0f) * br;
                Dg = bg > 0.25f ? sqrtf(bg) : ((16.0f * bg - 12.0f) * bg + 4.0f) * bg;
                Db = bb > 0.25f ? sqrtf(bb) : ((16.0f * bb - 12.0f) * bb + 4.0f) * bb;
                float Br = sr <= 0.5f ? br - (1.0f - 2.0f * sr) * br * (1.0f - br)
                                      : br + (2.0f * sr - 1.0f) * (Dr - br);
                float Bg = sg <= 0.5f ? bg - (1.0f - 2.0f * sg) * bg * (1.0f - bg)
                                      : bg + (2.0f * sg - 1.0f) * (Dg - bg);
                float Bb = sb <= 0.5f ? bb - (1.0f - 2.0f * sb) * bb * (1.0f - bb)
                                      : bb + (2.0f * sb - 1.0f) * (Db - bb);
                float rr = (sa * (1.0f - ba) * sr + sa * ba * Br + (1.0f - sa) * ba * br) / outa;
                float rg = (sa * (1.0f - ba) * sg + sa * ba * Bg + (1.0f - sa) * ba * bg) / outa;
                float rb = (sa * (1.0f - ba) * sb + sa * ba * Bb + (1.0f - sa) * ba * bb) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_CLEAR:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                dst[0] = dst[1] = dst[2] = dst[3] = 0;
            }
            break;
        }
        case FT_COLR_COMPOSITE_SRC:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST:
        {
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                dst[0] = back[0];
                dst[1] = back[1];
                dst[2] = back[2];
                dst[3] = back[3];
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST_OVER:
        {
            // DEST_OVER: Fs = (1-backA), Fd = 1
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = (1.0f - ba) * sa + ba;
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa * (1.0f - ba) + br * ba) / outa;
                float rg = (sg * sa * (1.0f - ba) + bg * ba) / outa;
                float rb = (sb * sa * (1.0f - ba) + bb * ba) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SRC_IN:
        {
            // SRC_IN: outA = srcA * backA; outRGB = srcRGB
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa * ba;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST_IN:
        {
            // DEST_IN: outA = backA * srcA; outRGB = backRGB
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = ba * sa;
                dst[0] = back[0];
                dst[1] = back[1];
                dst[2] = back[2];
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SRC_OUT:
        {
            // SRC_OUT: outA = srcA * (1-backA); outRGB = srcRGB
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa * (1.0f - ba);
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST_OUT:
        {
            // DEST_OUT: outA = backA * (1-srcA); outRGB = backRGB
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = ba * (1.0f - sa);
                dst[0] = back[0];
                dst[1] = back[1];
                dst[2] = back[2];
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_SRC_ATOP:
        {
            // SRC_ATOP: Fs = backA, Fd = (1-srcA); outA = backA
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = ba;
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa * ba + br * ba * (1.0f - sa)) / outa;
                float rg = (sg * sa * ba + bg * ba * (1.0f - sa)) / outa;
                float rb = (sb * sa * ba + bb * ba * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_DEST_ATOP:
        {
            // DEST_ATOP: Fs = (1-backA), Fd = srcA; outA = srcA
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa;
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa * (1.0f - ba) + br * ba * sa) / outa;
                float rg = (sg * sa * (1.0f - ba) + bg * ba * sa) / outa;
                float rb = (sb * sa * (1.0f - ba) + bb * ba * sa) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        case FT_COLR_COMPOSITE_XOR:
        {
            // XOR: Fs = (1-backA), Fd = (1-srcA)
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *src = &tmp_src[i * 4];
                uint8_t *back = &tmp_back[i * 4];
                float sa = src[3] / 255.0f;
                float ba = back[3] / 255.0f;
                float outa = sa * (1.0f - ba) + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float br = back[0] / 255.0f, bg = back[1] / 255.0f, bb = back[2] / 255.0f;
                float rr = (sr * sa * (1.0f - ba) + br * ba * (1.0f - sa)) / outa;
                float rg = (sg * sa * (1.0f - ba) + bg * ba * (1.0f - sa)) / outa;
                float rb = (sb * sa * (1.0f - ba) + bb * ba * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        default:
        {
            // Fallback for unimplemented composite modes: use SRC_OVER
            for (int i = 0; i < w * h; i++) {
                uint8_t *dst = &buf[i * 4];
                uint8_t *s = &tmp_src[i * 4];
                uint8_t *b = &tmp_back[i * 4];
                float sa = s[3] / 255.0f;
                float ba = b[3] / 255.0f;
                float outa = sa + ba * (1.0f - sa);
                if (outa <= 0.0f) {
                    dst[0] = dst[1] = dst[2] = dst[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float br = b[0] / 255.0f, bg = b[1] / 255.0f, bb = b[2] / 255.0f;
                float rr = (sr * sa + br * ba * (1.0f - sa)) / outa;
                float rg = (sg * sa + bg * ba * (1.0f - sa)) / outa;
                float rb = (sb * sa + bb * ba * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
            break;
        }
        }
        free(tmp_back);
        free(tmp_src);
        return true;
    }
    case FT_COLR_PAINTFORMAT_TRANSLATE:
    {
        FT_PaintTranslate *pt = &paint.u.translate;
        double dx = ft_fixed_to_double(pt->dx) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        double dy = ft_fixed_to_double(pt->dy) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        Affine local;
        affine_from_translate(&local, dx, dy);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, pt->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_SCALE:
    {
        FT_PaintScale *ps = &paint.u.scale;
        double sx = ft_fixed_to_double(ps->scale_x);
        double sy = ft_fixed_to_double(ps->scale_y);
        double cx = ft_fixed_to_double(ps->center_x) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        double cy = ft_fixed_to_double(ps->center_y) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        Affine local;
        affine_from_scale_center(&local, sx, sy, cx, cy);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, ps->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_ROTATE:
    {
        FT_PaintRotate *pr = &paint.u.rotate;
        double angle_deg = ft_fixed_to_double(pr->angle) * 180.0;
        double angle_rad = angle_deg * (M_PI / 180.0);
        double cx = ft_fixed_to_double(pr->center_x) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        double cy = ft_fixed_to_double(pr->center_y) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        Affine local;
        affine_from_rotate_center(&local, angle_rad, cx, cy);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, pr->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_SKEW:
    {
        FT_PaintSkew *psk = &paint.u.skew;
        double xsk = ft_fixed_to_double(psk->x_skew_angle) * M_PI; // approximate
        double ysk = ft_fixed_to_double(psk->y_skew_angle) * M_PI; // approximate
        double cx = ft_fixed_to_double(psk->center_x) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        double cy = ft_fixed_to_double(psk->center_y) * (matrix_maps_font_units ? 1.0 : ft_data->scale);
        Affine local;
        affine_from_skew_center(&local, xsk, ysk, cx, cy);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, psk->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_COLR_GLYPH:
    {
        FT_PaintColrGlyph *pcg = &paint.u.colr_glyph;
        FT_OpaquePaint nested = { NULL, 0 };
        if (!FT_Get_Color_Glyph_Paint(ft_data->ft_face, pcg->glyphID, FT_COLOR_NO_ROOT_TRANSFORM, &nested))
            return false;
        return paint_colr_paint_recursive(ft_data, nested, matrix, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    case FT_COLR_PAINTFORMAT_COLR_LAYERS:
    {
        FT_PaintColrLayers *pcl = &paint.u.colr_layers;
        FT_LayerIterator it = pcl->layer_iterator; // copy iterator
        FT_OpaquePaint layer_opaque = { NULL, 0 };
        while (FT_Get_Paint_Layers(ft_data->ft_face, &it, &layer_opaque)) {
            uint8_t *tmp = calloc((size_t)w * (size_t)h, 4);
            if (!tmp)
                return false;
            paint_colr_paint_recursive(ft_data, layer_opaque, matrix, matrix_maps_font_units, tmp, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
            // composite tmp over buf (src-over)
            for (int i = 0; i < w * h; i++) {
                uint8_t *s = &tmp[i * 4];
                uint8_t *d = &buf[i * 4];
                float sa = s[3] / 255.0f;
                float da = d[3] / 255.0f;
                float outa = sa + da * (1.0f - sa);
                if (outa <= 0.0f) {
                    d[0] = d[1] = d[2] = d[3] = 0;
                    continue;
                }
                float sr = s[0] / 255.0f, sg = s[1] / 255.0f, sb = s[2] / 255.0f;
                float dr = d[0] / 255.0f, dg = d[1] / 255.0f, db = d[2] / 255.0f;
                float rr = (sr * sa + dr * da * (1.0f - sa)) / outa;
                float rg = (sg * sa + dg * da * (1.0f - sa)) / outa;
                float rb = (sb * sa + db * da * (1.0f - sa)) / outa;
                d[0] = (uint8_t)round(rr * 255.0f);
                d[1] = (uint8_t)round(rg * 255.0f);
                d[2] = (uint8_t)round(rb * 255.0f);
                d[3] = (uint8_t)round(outa * 255.0f);
            }
            free(tmp);
        }
        return true;
    }
    case FT_COLR_PAINTFORMAT_GLYPH:
    {
        FT_PaintGlyph *pg = &paint.u.glyph;
        FT_Face face = ft_data->ft_face;

        // Apply accumulated transform (relative to root scale) via FT_Set_Transform
        // so that the glyph outline is rendered at the correct transformed position.
        // Only apply when there's a non-trivial transform (translation or non-identity matrix).
        double div = matrix_maps_font_units ? ft_data->scale : 1.0;
        bool need_transform = (fabs(matrix->dx) > 1e-6 || fabs(matrix->dy) > 1e-6 ||
                               fabs(matrix->xx / div - 1.0) > 1e-6 || fabs(matrix->xy / div) > 1e-6 ||
                               fabs(matrix->yx / div) > 1e-6 || fabs(matrix->yy / div - 1.0) > 1e-6);
        if (need_transform) {
            FT_Matrix ft_matrix;
            ft_matrix.xx = (FT_Fixed)((matrix->xx / div) * 0x10000);
            ft_matrix.xy = (FT_Fixed)((matrix->xy / div) * 0x10000);
            ft_matrix.yx = (FT_Fixed)((matrix->yx / div) * 0x10000);
            ft_matrix.yy = (FT_Fixed)((matrix->yy / div) * 0x10000);
            FT_Vector ft_delta;
            ft_delta.x = (FT_Pos)round(matrix->dx * 64.0);
            ft_delta.y = (FT_Pos)round(matrix->dy * 64.0);
            FT_Set_Transform(face, &ft_matrix, &ft_delta);
        }

        int mw = 0, mh = 0, left = 0, top = 0;
        unsigned char *mask = rasterize_glyph_mask(ft_data, (FT_UInt)pg->glyphID, &mw, &mh, &left, &top);
        if (need_transform)
            FT_Set_Transform(face, NULL, NULL);
        if (!mask)
            return false;

        uint8_t *tmp = calloc((size_t)mw * (size_t)mh, 4);
        if (!tmp) {
            free(mask);
            return false;
        }

        FT_OpaquePaint child = pg->paint;
        paint_colr_paint_recursive(ft_data, child, matrix, matrix_maps_font_units, tmp, mw, mh, left, top, fg_r, fg_g, fg_b);

        for (int y = 0; y < mh; y++) {
            for (int x = 0; x < mw; x++) {
                uint8_t mask_a = mask[y * mw + x];
                if (mask_a == 0)
                    continue;
                int dst_x = left + x - dst_x_off;
                int dst_y = dst_y_off - top + y; // Y-flip: convert from FreeType Y-up to pixel Y-down
                if (dst_x < 0 || dst_x >= w || dst_y < 0 || dst_y >= h)
                    continue;

                uint8_t *dst = &buf[(dst_y * w + dst_x) * 4];
                uint8_t *src = &tmp[(y * mw + x) * 4];

                float sa = (mask_a / 255.0f) * (src[3] / 255.0f);
                float da = dst[3] / 255.0f;
                float outa = sa + da * (1.0f - sa);
                if (outa <= 0.0f)
                    continue;
                float sr = src[0] / 255.0f, sg = src[1] / 255.0f, sb = src[2] / 255.0f;
                float dr = dst[0] / 255.0f, dg = dst[1] / 255.0f, db = dst[2] / 255.0f;
                float rr = (sr * sa + dr * da * (1.0f - sa)) / outa;
                float rg = (sg * sa + dg * da * (1.0f - sa)) / outa;
                float rb = (sb * sa + db * da * (1.0f - sa)) / outa;
                dst[0] = (uint8_t)round(rr * 255.0f);
                dst[1] = (uint8_t)round(rg * 255.0f);
                dst[2] = (uint8_t)round(rb * 255.0f);
                dst[3] = (uint8_t)round(outa * 255.0f);
            }
        }

        free(mask);
        free(tmp);
        return true;
    }
    case FT_COLR_PAINTFORMAT_TRANSFORM:
    {
        FT_PaintTransform *pt = &paint.u.transform;
        Affine local;
        affine_from_FT_Affine23(&local, &pt->affine);
        Affine next;
        affine_mul(&next, matrix, &local);
        return paint_colr_paint_recursive(ft_data, pt->paint, &next, matrix_maps_font_units, buf, w, h, dst_x_off, dst_y_off, fg_r, fg_g, fg_b);
    }
    default:
        // Unsupported paint types fall back to solid transparent
        vlog("Unsupported FT_COLR paint format: %d\n", paint.format);
        for (int i = 0; i < w * h; i++) {
            buf[i * 4 + 0] = 0;
            buf[i * 4 + 1] = 0;
            buf[i * 4 + 2] = 0;
            buf[i * 4 + 3] = 0;
        }
        return false;
    }
}

// Render COLR v1 paint for a glyph id into an RGBA bitmap using FT_Get_Color_Glyph_Paint
static GlyphBitmap *render_colr_paint_glyph(FtFontData *ft_data, FT_UInt glyph_index,
                                            uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!ft_data || !ft_data->ft_face)
        return NULL;

    FT_OpaquePaint root = { NULL, 0 };
    bool have_root_transform = false;
    int got = FT_Get_Color_Glyph_Paint(ft_data->ft_face, glyph_index, FT_COLOR_INCLUDE_ROOT_TRANSFORM, &root);
    if (got) {
        have_root_transform = true;
    } else {
        vlog("FT_Get_Color_Glyph_Paint with INCLUDE_ROOT_TRANSFORM failed for glyph %u, trying NO_ROOT_TRANSFORM\n", glyph_index);
        if (!FT_Get_Color_Glyph_Paint(ft_data->ft_face, glyph_index, FT_COLOR_NO_ROOT_TRANSFORM, &root)) {
            vlog("FT_Get_Color_Glyph_Paint failed for glyph %u (both INCLUDE_ROOT_TRANSFORM and NO_ROOT_TRANSFORM)\n", glyph_index);
            return NULL;
        }
    }

    // Get root paint
    FT_COLR_Paint root_paint;
    if (!FT_Get_Paint(ft_data->ft_face, root, &root_paint)) {
        vlog("FT_Get_Paint failed for glyph %u\n", glyph_index);
        return NULL;
    }

    // Determine bounding box for glyph using FT_Get_Color_Glyph_ClipBox if available
    FT_ClipBox clip;
    int have_clip = FT_Get_Color_Glyph_ClipBox(ft_data->ft_face, glyph_index, &clip);
    int out_w = 0, out_h = 0, xoff = 0, yoff = 0;
    if (have_clip) {
        // clip coordinates are in 26.6 fixed; convert to pixels
        double blx = ft_pos_to_double(clip.bottom_left.x);
        double bly = ft_pos_to_double(clip.bottom_left.y);
        double trx = ft_pos_to_double(clip.top_right.x);
        double try_ = ft_pos_to_double(clip.top_right.y);
        vlog("COLR clipbox raw 26.6: bl=(%ld,%ld) tr=(%ld,%ld)\n",
             (long)clip.bottom_left.x, (long)clip.bottom_left.y,
             (long)clip.top_right.x, (long)clip.top_right.y);
        vlog("COLR clipbox pixels: bl=(%.2f,%.2f) tr=(%.2f,%.2f)\n", blx, bly, trx, try_);
        xoff = (int)floor(blx);
        yoff = (int)ceil(try_);
        out_w = (int)ceil(trx - blx);
        out_h = (int)ceil(try_ - bly);
        vlog("COLR clipbox result: xoff=%d yoff=%d w=%d h=%d\n", xoff, yoff, out_w, out_h);
        if (out_w <= 0 || out_h <= 0)
            return NULL;
    } else {
        // Fallback to rasterizing a simple monochrome glyph to determine bbox
        GlyphBitmap *gb = rasterize_glyph_index(ft_data, glyph_index, 255, 255, 255);
        if (!gb)
            return NULL;
        out_w = gb->width;
        out_h = gb->height;
        xoff = gb->x_offset;
        yoff = gb->y_offset;
        if (gb->pixels)
            free(gb->pixels);
        free(gb);
        if (out_w <= 0 || out_h <= 0)
            return NULL;
    }

    GlyphBitmap *out = malloc(sizeof(GlyphBitmap));
    if (!out)
        return NULL;
    out->width = out_w;
    out->height = out_h;
    out->x_offset = xoff;
    out->y_offset = yoff;
    out->advance = 0;
    out->glyph_id = glyph_index;
    out->pixels = calloc(out_w * out_h, 4);
    if (!out->pixels) {
        free(out);
        return NULL;
    }

    // Evaluate root paint into out->pixels
    // Decide whether paint coordinates are in font units or pixels
    // When FT_COLOR_INCLUDE_ROOT_TRANSFORM is used, FreeType returns all paint
    // coordinates in font units (unscaled). Otherwise, they're in pixels.
    bool matrix_maps_font_units = have_root_transform;
    Affine identity;
    affine_identity(&identity);
    paint_colr_paint_recursive(ft_data, root, &identity, matrix_maps_font_units, out->pixels, out_w, out_h, xoff, yoff, fg_r, fg_g, fg_b);

    return out;
}

// Initialize FreeType/Cairo font
static void *ft_init_font(Font *font, const char *font_path,
                          float font_size, FontStyle style, const FontOptions *options)
{
    (void)font; // Unused

    vlog("Initializing FreeType/Cairo font from %s, size %.1f, style %d\n", font_path, font_size, style);

    // Load font file
    unsigned char *font_data = NULL;
    size_t font_data_size = 0;
    if (!load_font_file(font_path, &font_data, &font_data_size)) {
        return NULL;
    }

    // Allocate font data structure
    FtFontData *ft_data = calloc(1, sizeof(FtFontData));
    if (!ft_data) {
        free(font_data);
        return NULL;
    }

    ft_data->font_data = font_data;
    ft_data->font_data_size = font_data_size;
    ft_data->font_size = font_size;
    ft_data->style = style;

    // Set font options from Fontconfig
    if (options) {
        ft_data->antialias = options->antialias;
        ft_data->hinting = options->hinting;
        ft_data->hint_style = options->hint_style;
        ft_data->subpixel_order = options->subpixel_order;
        ft_data->lcd_filter = options->lcd_filter;
        ft_data->ft_load_flags = options->ft_load_flags;
        ft_data->dpi_x = options->dpi_x;
        ft_data->dpi_y = options->dpi_y;
    } else {
        // Set default options if none provided
        ft_data->antialias = true;
        ft_data->hinting = true;
        ft_data->hint_style = FC_HINT_SLIGHT;
        ft_data->subpixel_order = FC_RGBA_NONE;
        ft_data->lcd_filter = FC_LCD_DEFAULT;
        ft_data->ft_load_flags = FT_LOAD_TARGET_LIGHT;
        ft_data->dpi_x = 96;
        ft_data->dpi_y = 96;
    }

    // Initialize FreeType library (this should ideally be done once globally)
    static FT_Library ft_library = NULL;
    if (!ft_library) {
        FT_Error error = FT_Init_FreeType(&ft_library);
        if (error) {
            vlog("Failed to initialize FreeType library\n");
            free(font_data);
            free(ft_data);
            return NULL;
        }
    }

    // Create FreeType face from memory
    FT_Error error = FT_New_Memory_Face(ft_library, font_data, font_data_size, 0, &ft_data->ft_face);
    if (error) {
        vlog("Failed to create FreeType face from %s\n", font_path);
        free(font_data);
        free(ft_data);
        return NULL;
    }

    // Set character size with DPI support (using 26.6 fixed-point format)
    // This is the correct approach for HiDPI support as per the design document
    error = FT_Set_Char_Size(ft_data->ft_face, 0, (FT_F26Dot6)(font_size * 64), ft_data->dpi_x, ft_data->dpi_y);
    if (error) {
        vlog("Failed to set character size for FreeType face\n");
        FT_Done_Face(ft_data->ft_face);
        free(font_data);
        free(ft_data);
        return NULL;
    }

    // Calculate scale: pixels-per-em / units-per-em
    // FT_Set_Char_Size with DPI means actual ppem = font_size * dpi / 72
    float ppem = font_size * ft_data->dpi_x / 72.0f;
    ft_data->scale = ppem / (float)ft_data->ft_face->units_per_EM;
    vlog("Font scale factor: %f (ppem=%.1f, upem=%d, dpi=%d)\n",
         ft_data->scale, ppem, ft_data->ft_face->units_per_EM, ft_data->dpi_x);

    // Initialize HarfBuzz font from FreeType face
    ft_data->hb_font = hb_ft_font_create_referenced(ft_data->ft_face);
    if (!ft_data->hb_font) {
        vlog("Warning: failed to create HarfBuzz font for %s\n", font_path);
    }

    // Check for COLR table
    ft_data->has_colr = check_colr_table(ft_data->ft_face);
    if (ft_data->has_colr) {
        if (load_colr_palette(ft_data)) {
            vlog("Loaded COLR palette for %s\n", font_path);
        } else {
            vlog("COLR palette not loaded for %s; color glyphs will use grayscale fallback\n", font_path);
        }
    }

    // Apply font variations based on style
    apply_font_variations(ft_data, style);

    vlog("Created FreeType font from %s\n", font_path);

    return ft_data;
}

// Destroy FreeType/Cairo font
static void ft_destroy_font(Font *font, void *font_data)
{
    (void)font; // Unused

    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data)
        return;

    if (ft_data->hb_font) {
        hb_font_destroy(ft_data->hb_font);
        ft_data->hb_font = NULL;
    }

    if (ft_data->mm_var) {
        // Free cached MM_Var
        FT_Done_MM_Var(ft_data->ft_face->glyph->library, ft_data->mm_var);
        ft_data->mm_var = NULL;
    }
    if (ft_data->axes) {
        free(ft_data->axes);
        ft_data->axes = NULL;
    }

    if (ft_data->ft_face) {
        FT_Done_Face(ft_data->ft_face);
    }

    free(ft_data->font_data);
    free(ft_data);
}

// Get font metrics
static bool ft_get_metrics(Font *font, void *font_data, FontMetrics *metrics)
{
    (void)font; // Unused

    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data || !metrics)
        return false;

    FT_Face face = ft_data->ft_face;

    // Calculate metrics using 26.6 fixed-point conversion as per design document
    // Cell width = max_advance_width (converted from 26.6 fixed-point)
    // Cell height = ascender + descender (ensuring baseline alignment)
    metrics->ascent = face->size->metrics.ascender >> 6;    // Convert from 26.6 fixed-point
    metrics->descent = -face->size->metrics.descender >> 6; // Convert from 26.6 fixed-point
    metrics->line_gap = (face->size->metrics.height - face->size->metrics.ascender + face->size->metrics.descender) >> 6;

    // Precise monospace cell_width from 'm' glyph advance (fallback to max_advance)
    FT_UInt m_index = FT_Get_Char_Index(face, 'm');
    int orig_max_advance = face->size->metrics.max_advance >> 6;
    if (m_index != 0) {
        FT_Error m_err = FT_Load_Glyph(face, m_index, FT_LOAD_NO_BITMAP);
        if (m_err == 0) {
            metrics->cell_width = (int)(face->glyph->advance.x >> 6);
            vlog("cell_width from 'm' advance: %d (max_advance fallback: %d)\n",
                 metrics->cell_width, orig_max_advance);
            goto m_done; // Skip max_advance
        }
    }
    metrics->cell_width = orig_max_advance;
    vlog("cell_width fallback to max_advance: %d\n", metrics->cell_width);
m_done:;

    // For cell height, use ascender + descender for proper baseline alignment
    metrics->cell_height = metrics->ascent + metrics->descent;

    // For glyph dimensions, we'll use the actual metrics from the face
    // This is a simplified approach - in a full implementation we'd measure actual glyphs
    metrics->glyph_width = metrics->cell_width;   // Approximation for now
    metrics->glyph_height = metrics->cell_height; // Approximation for now

    vlog("FreeType/Cairo metrics (26.6 fixed-point): asc=%d, des=%d, line_gap=%d, glyph=%dx%d, cell=%dx%d\n",
         metrics->ascent, metrics->descent, metrics->line_gap,
         metrics->glyph_width, metrics->glyph_height,
         metrics->cell_width, metrics->cell_height);

    return true;
}

// Render glyphs using FreeType directly (unified function for single/multiple codepoints)
static GlyphBitmap *ft_render_glyph(Font *font, void *font_data,
                                    uint32_t *codepoints, int codepoint_count,
                                    uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    (void)font; // Unused

    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data || !codepoints || codepoint_count <= 0)
        return NULL;

    // For now, only handle the first codepoint (same behavior as before)
    uint32_t codepoint = codepoints[0];
    vlog("Rendering glyph U+%04X with direct FreeType (style=%d)\n", codepoint, ft_data->style);

    FT_Face face = ft_data->ft_face;

    // Load the glyph index for this codepoint
    FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
    if (glyph_index == 0) {
        vlog("No glyph index found for U+%04X\n", codepoint);
        return NULL;
    }

    // If this is a COLR face, try the COLR paint/layer render path first (rasterize_glyph_index implements this)
    if (ft_data->has_colr && FT_HAS_COLOR(face)) {
        GlyphBitmap *colr_gb = rasterize_glyph_index(ft_data, glyph_index, fg_r, fg_g, fg_b);
        if (colr_gb)
            return colr_gb;
        // If COLR render failed, continue to attempt grayscale/outlines below
        vlog("COLR render path failed for U+%04X, falling back to outline/grayscale rendering\n", codepoint);
    }

    // Load glyph with appropriate flags
    FT_Int32 load_flags = FT_LOAD_DEFAULT;
    if (ft_data->antialias) {
        load_flags |= FT_LOAD_TARGET_NORMAL;
    } else {
        load_flags |= FT_LOAD_TARGET_MONO;
    }

    // Apply hinting settings
    if (ft_data->hinting) {
        switch (ft_data->hint_style) {
        case FC_HINT_NONE:
            load_flags |= FT_LOAD_NO_HINTING;
            break;
        case FC_HINT_SLIGHT:
            load_flags |= FT_LOAD_TARGET_LIGHT;
            break;
        case FC_HINT_MEDIUM:
        case FC_HINT_FULL:
            load_flags |= FT_LOAD_TARGET_NORMAL;
            break;
        }
    } else {
        load_flags |= FT_LOAD_NO_HINTING;
    }

    FT_Error error = FT_Load_Glyph(face, glyph_index, load_flags);
    if (error) {
        vlog("Failed to load glyph for U+%04X: error %d\n", codepoint, error);
        return NULL;
    }

    // Render the glyph to a bitmap
    error = FT_Render_Glyph(face->glyph, ft_data->antialias ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO);
    if (error) {
        vlog("Failed to render glyph for U+%04X: error %d\n", codepoint, error);
        return NULL;
    }

    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bitmap = &slot->bitmap;

    // Allocate glyph bitmap
    GlyphBitmap *glyph_bitmap = malloc(sizeof(GlyphBitmap));
    if (!glyph_bitmap) {
        return NULL;
    }

    glyph_bitmap->width = bitmap->width;
    glyph_bitmap->height = bitmap->rows;
    glyph_bitmap->x_offset = slot->bitmap_left;
    glyph_bitmap->y_offset = slot->bitmap_top;
    glyph_bitmap->advance = (int)(slot->advance.x >> 6); // Convert from 26.6 fixed point
    glyph_bitmap->glyph_id = glyph_index;

    // Handle zero-width or zero-height glyphs (e.g., spaces)
    if (glyph_bitmap->width <= 0 || glyph_bitmap->height <= 0) {
        vlog("Glyph has zero dimensions: %dx%d, creating empty bitmap\n", glyph_bitmap->width, glyph_bitmap->height);
        glyph_bitmap->pixels = NULL; // No pixels for empty glyphs
        return glyph_bitmap;
    }

    // Allocate RGBA pixels
    glyph_bitmap->pixels = malloc(glyph_bitmap->width * glyph_bitmap->height * 4);
    if (!glyph_bitmap->pixels) {
        free(glyph_bitmap);
        return NULL;
    }

    // Convert FreeType bitmap to RGBA
    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        // Grayscale bitmap
        for (int y = 0; y < (int)bitmap->rows; y++) {
            unsigned char *src_row = bitmap->buffer + y * bitmap->pitch;
            unsigned char *dst_row = glyph_bitmap->pixels + y * glyph_bitmap->width * 4;
            for (int x = 0; x < (int)bitmap->width; x++) {
                unsigned char alpha = src_row[x];
                dst_row[x * 4 + 0] = fg_r;  // R
                dst_row[x * 4 + 1] = fg_g;  // G
                dst_row[x * 4 + 2] = fg_b;  // B
                dst_row[x * 4 + 3] = alpha; // A
            }
        }
    } else if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        // Monochrome bitmap
        for (int y = 0; y < (int)bitmap->rows; y++) {
            unsigned char *src_row = bitmap->buffer + y * bitmap->pitch;
            unsigned char *dst_row = glyph_bitmap->pixels + y * glyph_bitmap->width * 4;
            for (int x = 0; x < (int)bitmap->width; x++) {
                unsigned char byte = src_row[x >> 3];
                unsigned char bit = (byte >> (7 - (x & 7))) & 1;
                unsigned char alpha = bit ? 255 : 0;
                dst_row[x * 4 + 0] = fg_r;  // R
                dst_row[x * 4 + 1] = fg_g;  // G
                dst_row[x * 4 + 2] = fg_b;  // B
                dst_row[x * 4 + 3] = alpha; // A
            }
        }
    } else {
        vlog("Unsupported pixel mode: %d\n", bitmap->pixel_mode);
        free(glyph_bitmap->pixels);
        free(glyph_bitmap);
        return NULL;
    }

    vlog("Successfully rendered glyph U+%04X with direct FreeType: %dx%d pixels, x_offset=%d, y_offset=%d, advance=%d\n",
         codepoint, glyph_bitmap->width, glyph_bitmap->height, glyph_bitmap->x_offset, glyph_bitmap->y_offset, glyph_bitmap->advance);
    return glyph_bitmap;

    // COLR rendering not yet implemented with FreeType directly; fall back to simple grayscale rendering
    vlog("COLR glyph rendering requested for U+%04X but COLR support is not implemented; attempting grayscale fallback\n", codepoint);
    // Try to render a grayscale glyph as a fallback (may lose color information)
    FT_Error colr_err = FT_Render_Glyph(face->glyph, ft_data->antialias ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO);
    if (colr_err) {
        vlog("Fallback grayscale render failed for U+%04X: error %d\n", codepoint, colr_err);
        return NULL;
    }

    FT_GlyphSlot slot2 = face->glyph;
    FT_Bitmap *bitmap2 = &slot2->bitmap;

    GlyphBitmap *glyph_bitmap_fallback = malloc(sizeof(GlyphBitmap));
    if (!glyph_bitmap_fallback) {
        return NULL;
    }

    glyph_bitmap_fallback->width = bitmap2->width;
    glyph_bitmap_fallback->height = bitmap2->rows;
    glyph_bitmap_fallback->x_offset = slot2->bitmap_left;
    glyph_bitmap_fallback->y_offset = slot2->bitmap_top;
    glyph_bitmap_fallback->advance = (int)(slot2->advance.x >> 6);

    if (glyph_bitmap_fallback->width <= 0 || glyph_bitmap_fallback->height <= 0) {
        glyph_bitmap_fallback->pixels = NULL;
        return glyph_bitmap_fallback;
    }

    glyph_bitmap_fallback->pixels = malloc(glyph_bitmap_fallback->width * glyph_bitmap_fallback->height * 4);
    if (!glyph_bitmap_fallback->pixels) {
        free(glyph_bitmap_fallback);
        return NULL;
    }

    if (bitmap2->pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (int y = 0; y < (int)bitmap2->rows; y++) {
            unsigned char *src_row = bitmap2->buffer + y * bitmap2->pitch;
            unsigned char *dst_row = glyph_bitmap_fallback->pixels + y * glyph_bitmap_fallback->width * 4;
            for (int x = 0; x < (int)bitmap2->width; x++) {
                unsigned char alpha = src_row[x];
                dst_row[x * 4 + 0] = fg_r;
                dst_row[x * 4 + 1] = fg_g;
                dst_row[x * 4 + 2] = fg_b;
                dst_row[x * 4 + 3] = alpha;
            }
        }
    } else if (bitmap2->pixel_mode == FT_PIXEL_MODE_MONO) {
        for (int y = 0; y < (int)bitmap2->rows; y++) {
            unsigned char *src_row = bitmap2->buffer + y * bitmap2->pitch;
            unsigned char *dst_row = glyph_bitmap_fallback->pixels + y * glyph_bitmap_fallback->width * 4;
            for (int x = 0; x < (int)bitmap2->width; x++) {
                unsigned char byte = src_row[x >> 3];
                unsigned char bit = (byte >> (7 - (x & 7))) & 1;
                unsigned char alpha = bit ? 255 : 0;
                dst_row[x * 4 + 0] = fg_r;
                dst_row[x * 4 + 1] = fg_g;
                dst_row[x * 4 + 2] = fg_b;
                dst_row[x * 4 + 3] = alpha;
            }
        }
    } else {
        free(glyph_bitmap_fallback->pixels);
        free(glyph_bitmap_fallback);
        return NULL;
    }

    vlog("Rendered grayscale fallback for COLR glyph U+%04X: %dx%d\n", codepoint, glyph_bitmap_fallback->width, glyph_bitmap_fallback->height);
    return glyph_bitmap_fallback;
}

// Helper: rasterize a glyph by glyph index
static GlyphBitmap *rasterize_glyph_index(FtFontData *ft_data, FT_UInt glyph_index,
                                          uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    if (!ft_data || !ft_data->ft_face)
        return NULL;

    // Handle COLR color glyphs if supported (best-effort)
    if (ft_data->has_colr && FT_HAS_COLOR(ft_data->ft_face)) {
        // Try COLR v1 paint graph rendering first
        GlyphBitmap *colr_p = render_colr_paint_glyph(ft_data, glyph_index, fg_r, fg_g, fg_b);
        if (colr_p) {
            vlog("rasterize_glyph_index: COLR v1 paint rendered for glyph %u: %dx%d\n", glyph_index, colr_p->width, colr_p->height);
            return colr_p;
        }
        vlog("rasterize_glyph_index: COLR v1 paint not available for glyph %u, trying COLR v0 fallback\n", glyph_index);
        // Fallback to COLR v0 layer rendering
        GlyphBitmap *colr = render_colr_glyph(ft_data, glyph_index, fg_r, fg_g, fg_b);
        if (colr) {
            vlog("rasterize_glyph_index: COLR v0 layers rendered for glyph %u: %dx%d\n", glyph_index, colr->width, colr->height);
            return colr;
        }
        // Otherwise fall back to grayscale rasterization below
        vlog("rasterize_glyph_index: COLR v0 also not available for glyph %u, falling back to grayscale\n", glyph_index);
    }

    FT_Face face = ft_data->ft_face;

    FT_Int32 load_flags = FT_LOAD_DEFAULT;
    if (ft_data->antialias) {
        load_flags |= FT_LOAD_TARGET_NORMAL;
    } else {
        load_flags |= FT_LOAD_TARGET_MONO;
    }

    if (ft_data->hinting) {
        switch (ft_data->hint_style) {
        case FC_HINT_NONE:
            load_flags |= FT_LOAD_NO_HINTING;
            break;
        case FC_HINT_SLIGHT:
            load_flags |= FT_LOAD_TARGET_LIGHT;
            break;
        case FC_HINT_MEDIUM:
        case FC_HINT_FULL:
            load_flags |= FT_LOAD_TARGET_NORMAL;
            break;
        }
    } else {
        load_flags |= FT_LOAD_NO_HINTING;
    }

    // If COLR and FreeType supports FT_LOAD_COLOR, try color render path first
    if (ft_data->has_colr && FT_HAS_COLOR(face)) {
        FT_Error color_err = FT_Load_Glyph(face, glyph_index, load_flags | FT_LOAD_COLOR);
        if (color_err == 0) {
            color_err = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
            if (color_err == 0 && face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
                // Convert BGRA bitmap to RGBA
                GlyphBitmap *glyph_bitmap = malloc(sizeof(GlyphBitmap));
                if (!glyph_bitmap)
                    return NULL;

                FT_Bitmap *bitmap = &face->glyph->bitmap;
                glyph_bitmap->width = bitmap->width;
                glyph_bitmap->height = bitmap->rows;
                glyph_bitmap->x_offset = face->glyph->bitmap_left;
                glyph_bitmap->y_offset = face->glyph->bitmap_top;
                glyph_bitmap->advance = (int)(face->glyph->advance.x >> 6);
                glyph_bitmap->glyph_id = glyph_index;

                if (glyph_bitmap->width <= 0 || glyph_bitmap->height <= 0) {
                    glyph_bitmap->pixels = NULL;
                    return glyph_bitmap;
                }

                glyph_bitmap->pixels = malloc(glyph_bitmap->width * glyph_bitmap->height * 4);
                if (!glyph_bitmap->pixels) {
                    free(glyph_bitmap);
                    return NULL;
                }

                // BGRA -> RGBA
                unsigned char *src = bitmap->buffer;
                unsigned char *dst = glyph_bitmap->pixels;
                for (int y = 0; y < (int)bitmap->rows; y++) {
                    unsigned char *srow = src + y * bitmap->pitch;
                    unsigned char *drow = dst + y * glyph_bitmap->width * 4;
                    for (int x = 0; x < (int)bitmap->width; x++) {
                        unsigned char b = srow[x * 4 + 0];
                        unsigned char g = srow[x * 4 + 1];
                        unsigned char r = srow[x * 4 + 2];
                        unsigned char a = srow[x * 4 + 3];
                        drow[x * 4 + 0] = r;
                        drow[x * 4 + 1] = g;
                        drow[x * 4 + 2] = b;
                        drow[x * 4 + 3] = a;
                    }
                }

                return glyph_bitmap;
            }
        }
        // fallthrough to non-color rendering if unsupported
    }

    FT_Error error = FT_Load_Glyph(face, glyph_index, load_flags);
    if (error) {
        vlog("rasterize_glyph_index: Failed to load glyph index %u: error %d\n", glyph_index, error);
        return NULL;
    }

    // Render glyph to bitmap
    error = FT_Render_Glyph(face->glyph, ft_data->antialias ? FT_RENDER_MODE_NORMAL : FT_RENDER_MODE_MONO);
    if (error) {
        vlog("rasterize_glyph_index: Failed to render glyph index %u: error %d\n", glyph_index, error);
        return NULL;
    }

    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bitmap = &slot->bitmap;

    GlyphBitmap *glyph_bitmap = malloc(sizeof(GlyphBitmap));
    if (!glyph_bitmap)
        return NULL;

    glyph_bitmap->width = bitmap->width;
    glyph_bitmap->height = bitmap->rows;
    glyph_bitmap->x_offset = slot->bitmap_left;
    glyph_bitmap->y_offset = slot->bitmap_top;
    glyph_bitmap->advance = (int)(slot->advance.x >> 6);
    glyph_bitmap->glyph_id = glyph_index;

    if (glyph_bitmap->width <= 0 || glyph_bitmap->height <= 0) {
        glyph_bitmap->pixels = NULL;
        return glyph_bitmap;
    }

    glyph_bitmap->pixels = malloc(glyph_bitmap->width * glyph_bitmap->height * 4);
    if (!glyph_bitmap->pixels) {
        free(glyph_bitmap);
        return NULL;
    }

    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (int y = 0; y < (int)bitmap->rows; y++) {
            unsigned char *src_row = bitmap->buffer + y * bitmap->pitch;
            unsigned char *dst_row = glyph_bitmap->pixels + y * glyph_bitmap->width * 4;
            for (int x = 0; x < (int)bitmap->width; x++) {
                unsigned char alpha = src_row[x];
                dst_row[x * 4 + 0] = fg_r;
                dst_row[x * 4 + 1] = fg_g;
                dst_row[x * 4 + 2] = fg_b;
                dst_row[x * 4 + 3] = alpha;
            }
        }
    } else if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        for (int y = 0; y < (int)bitmap->rows; y++) {
            unsigned char *src_row = bitmap->buffer + y * bitmap->pitch;
            unsigned char *dst_row = glyph_bitmap->pixels + y * glyph_bitmap->width * 4;
            for (int x = 0; x < (int)bitmap->width; x++) {
                unsigned char byte = src_row[x >> 3];
                unsigned char bit = (byte >> (7 - (x & 7))) & 1;
                unsigned char alpha = bit ? 255 : 0;
                dst_row[x * 4 + 0] = fg_r;
                dst_row[x * 4 + 1] = fg_g;
                dst_row[x * 4 + 2] = fg_b;
                dst_row[x * 4 + 3] = alpha;
            }
        }
    } else {
        free(glyph_bitmap->pixels);
        free(glyph_bitmap);
        return NULL;
    }

    vlog("rasterize_glyph_index: Grayscale fallback rendered for glyph %u: %dx%d\n", glyph_index, glyph_bitmap->width, glyph_bitmap->height);
    return glyph_bitmap;
}

// Render a shaped run: shape with HarfBuzz, rasterize each glyph with FreeType
static ShapedGlyphs *ft_render_shaped(Font *font, void *font_data,
                                      uint32_t *codepoints, int codepoint_count,
                                      uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    (void)font;
    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data || !ft_data->hb_font || !codepoints || codepoint_count <= 0)
        return NULL;

    hb_buffer_t *buf = hb_buffer_create();
    if (!buf)
        return NULL;

    hb_buffer_add_utf32(buf, codepoints, codepoint_count, 0, codepoint_count);
    hb_buffer_guess_segment_properties(buf);

    hb_shape(ft_data->hb_font, buf, NULL, 0);

    unsigned int glyph_count = 0;
    hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
    hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buf, &glyph_count);

    if (!glyph_info || glyph_count == 0) {
        hb_buffer_destroy(buf);
        return NULL;
    }

    ShapedGlyphs *run = calloc(1, sizeof(ShapedGlyphs));
    if (!run) {
        hb_buffer_destroy(buf);
        return NULL;
    }

    run->num_glyphs = (int)glyph_count;
    run->bitmaps = calloc(glyph_count, sizeof(GlyphBitmap *));
    run->x_positions = calloc(glyph_count, sizeof(int));
    run->y_positions = calloc(glyph_count, sizeof(int));
    run->x_advances = calloc(glyph_count, sizeof(int));
    run->total_advance = 0;

    int32_t cursor_x = 0, cursor_y = 0;
    for (unsigned int i = 0; i < glyph_count; i++) {
        FT_UInt glyph_index = (FT_UInt)glyph_info[i].codepoint; // glyph id

        // Rasterize glyph
        GlyphBitmap *gb = rasterize_glyph_index(ft_data, glyph_index, fg_r, fg_g, fg_b);
        run->bitmaps[i] = gb;

        // Positions/advances are in 26.6 fixed point from HarfBuzz
        int x_pos = (int)((cursor_x + glyph_pos[i].x_offset) >> 6);
        int y_pos = (int)((cursor_y + glyph_pos[i].y_offset) >> 6);
        int x_adv = (int)(glyph_pos[i].x_advance >> 6);

        run->x_positions[i] = x_pos;
        run->y_positions[i] = y_pos;
        run->x_advances[i] = x_adv;

        run->total_advance += x_adv;

        cursor_x += glyph_pos[i].x_advance;
        cursor_y += glyph_pos[i].y_advance;
    }

    hb_buffer_destroy(buf);
    return run;
}

// Set a single variation axis value
static bool ft_set_variation_axis(Font *font, void *font_data, const char *axis_tag, float value)
{
    (void)font;
    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data || !ft_data->ft_face || !axis_tag || strlen(axis_tag) < 4)
        return false;

    cache_mm_var(ft_data);
    if (!ft_data->mm_var)
        return false;

    FT_MM_Var *mm_var = ft_data->mm_var;
    FT_Fixed *coords = malloc(mm_var->num_axis * sizeof(FT_Fixed));
    if (!coords)
        return false;

    for (FT_UInt i = 0; i < mm_var->num_axis; i++) {
        coords[i] = (FT_Fixed)(ft_data->axes ? ft_data->axes[i].current_value * 65536.0f : mm_var->axis[i].def);
    }

    FT_ULong tag = FT_MAKE_TAG(axis_tag[0], axis_tag[1], axis_tag[2], axis_tag[3]);
    for (FT_UInt i = 0; i < mm_var->num_axis; i++) {
        if (mm_var->axis[i].tag == tag) {
            coords[i] = (FT_Fixed)(value * 65536.0f);
            if (ft_data->axes)
                ft_data->axes[i].current_value = value;
            break;
        }
    }

    FT_Error err = FT_Set_Var_Design_Coordinates(ft_data->ft_face, mm_var->num_axis, coords);
    free(coords);

    if (err == 0) {
        if (ft_data->hb_font)
            hb_ft_font_changed(ft_data->hb_font);
        return true;
    }
    return false;
}

// Set multiple variation axis coordinates
static bool ft_set_variation_axes(Font *font, void *font_data, float *coords_in, int num_coords)
{
    (void)font;
    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data || !ft_data->ft_face || !coords_in || num_coords <= 0)
        return false;

    cache_mm_var(ft_data);
    if (!ft_data->mm_var)
        return false;

    FT_MM_Var *mm_var = ft_data->mm_var;
    int axes = mm_var->num_axis;
    FT_Fixed *coords = malloc(axes * sizeof(FT_Fixed));
    if (!coords)
        return false;

    // Fill coordinates from input; if fewer provided, use defaults for remaining
    for (int i = 0; i < axes; i++) {
        float val = (i < num_coords) ? coords_in[i] : (float)mm_var->axis[i].def / 65536.0f;
        coords[i] = (FT_Fixed)(val * 65536.0f);
        if (ft_data->axes)
            ft_data->axes[i].current_value = val;
    }

    FT_Error err = FT_Set_Var_Design_Coordinates(ft_data->ft_face, axes, coords);
    free(coords);

    if (err == 0) {
        if (ft_data->hb_font)
            hb_ft_font_changed(ft_data->hb_font);
        return true;
    }
    return false;
}

// Get glyph info without rendering
static bool ft_get_glyph_info(Font *font, void *font_data, uint32_t codepoint,
                              int *advance, int *left_bearing, int *top_bearing)
{
    (void)font; // Unused

    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data)
        return false;

    FT_UInt glyph_index = FT_Get_Char_Index(ft_data->ft_face, codepoint);
    if (glyph_index == 0) {
        return false;
    }

    FT_Error error = FT_Load_Glyph(ft_data->ft_face, glyph_index, FT_LOAD_DEFAULT);
    if (error) {
        return false;
    }

    if (advance)
        *advance = (int)(ft_data->ft_face->glyph->advance.x >> 6); // Convert from 26.6 fixed point
    if (left_bearing)
        *left_bearing = ft_data->ft_face->glyph->metrics.horiBearingX >> 6;

    // For top bearing, we need the glyph box
    if (top_bearing) {
        *top_bearing = ft_data->ft_face->glyph->metrics.horiBearingY >> 6;
    }

    return true;
}

// FreeType font implementation
static bool ft_style_has_colr(Font *font, void *font_data)
{
    (void)font;
    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data)
        return false;
    return ft_data->has_colr;
}

Font font = {
    .name = "freetype",
    .init = font_init,
    .destroy = font_destroy,
    .init_font = ft_init_font,
    .destroy_font = ft_destroy_font,
    .get_metrics = ft_get_metrics,
    .render_glyphs = ft_render_glyph,            // Unified renderer (single codepoint)
    .render_shaped = ft_render_shaped,           // HarfBuzz-shaped multi-codepoint runs
    .set_variation_axis = ft_set_variation_axis, // Variable font control
    .set_variation_axes = ft_set_variation_axes, // Set multiple axes
    .get_glyph_info = ft_get_glyph_info,
    .free_glyph_bitmap = ft_free_glyph_bitmap,
    .load_font = font_load_font,
    .get_style_metrics = font_get_metrics,
    .has_style = font_has_style,
    .style_has_colr = ft_style_has_colr
};
