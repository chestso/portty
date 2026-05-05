#define FT_CONFIG_OPTION_SUBPIXEL_RENDERING
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include <freetype2/freetype/ftcolor.h>
#include FT_MULTIPLE_MASTERS_H
#include FT_SFNT_NAMES_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H
#include "common.h"
#include "font.h"
#include "font_ft_internal.h"
#include <hb-ft.h>
#include <hb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static void ft_free_glyph_bitmap(FontBackend *font, GlyphBitmap *bitmap)
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

    ft_data->coords_scratch = malloc(mm_var->num_axis * sizeof(FT_Fixed));
    if (!ft_data->coords_scratch) {
        free(ft_data->axes);
        ft_data->axes = NULL;
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

    // Use pre-allocated coords buffer
    FT_Fixed *coords = ft_data->coords_scratch;
    if (!coords) {
        vlog("Failed to allocate memory for font variations\n");
        return;
    }

    // Initialize all coordinates to their default or cached current values
    for (FT_UInt i = 0; i < mm_var->num_axis; i++) {
        float val = ft_data->axes ? ft_data->axes[i].current_value : (float)mm_var->axis[i].def / 65536.0f;
        coords[i] = (FT_Fixed)(val * 65536.0f);
    }

    // Handle bold weight for BOLD and BOLD_ITALIC styles
    if (style == FONT_STYLE_BOLD || style == FONT_STYLE_BOLD_ITALIC) {
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

    // Handle italic axes for ITALIC and BOLD_ITALIC styles
    if (style == FONT_STYLE_ITALIC || style == FONT_STYLE_BOLD_ITALIC) {
        for (FT_UInt i = 0; i < mm_var->num_axis; i++) {
            if (mm_var->axis[i].tag == FT_MAKE_TAG('i', 't', 'a', 'l')) {
                coords[i] = (FT_Fixed)(1.0f * 65536.0f);
                ft_data->axes[i].current_value = 1.0f;
                vlog("Set 'ital' axis to 1.0 for italic style\n");
            } else if (mm_var->axis[i].tag == FT_MAKE_TAG('s', 'l', 'n', 't')) {
                float min_slant = (float)mm_var->axis[i].minimum / 65536.0f;
                float target = -12.0f;
                if (target < min_slant)
                    target = min_slant;
                coords[i] = (FT_Fixed)(target * 65536.0f);
                ft_data->axes[i].current_value = target;
                vlog("Set 'slnt' axis to %f for italic style\n", target);
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
    enum
    {
        COLR_STACK_LAYERS = 32
    };
    LayerBmp stack_layers[COLR_STACK_LAYERS];
    LayerBmp *layers = stack_layers;
    int layer_count = 0;
    int layer_capacity = COLR_STACK_LAYERS;
    bool layers_on_heap = false;

    while (FT_Get_Color_Glyph_Layer(face, glyph_index, &layer_glyph, &layer_color_index, &iterator)) {
        if (layer_glyph == 0)
            break;

        if (FT_Load_Glyph(face, layer_glyph, ft_data->ft_hint_target) != 0)
            continue;
        if (FT_Render_Glyph(face->glyph, FT_LOAD_TARGET_MODE(ft_data->ft_hint_target)) != 0)
            continue;

        FT_Bitmap *bmp = &face->glyph->bitmap;
        int left = face->glyph->bitmap_left;
        int top = face->glyph->bitmap_top;

        if (layer_count >= layer_capacity) {
            int new_capacity = layer_capacity * 2;
            LayerBmp *nl;
            if (layers_on_heap) {
                nl = realloc(layers, sizeof(LayerBmp) * new_capacity);
            } else {
                nl = malloc(sizeof(LayerBmp) * new_capacity);
                if (nl)
                    memcpy(nl, stack_layers, sizeof(LayerBmp) * layer_count);
            }
            if (!nl)
                break;
            layers = nl;
            layer_capacity = new_capacity;
            layers_on_heap = true;
        }

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

    GlyphBitmap *out = NULL;

    if (layer_count == 0)
        goto cleanup;

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
    if (out_w <= 0 || out_h <= 0)
        goto cleanup;

    out = malloc(sizeof(GlyphBitmap));
    if (!out)
        goto cleanup;
    out->width = out_w;
    out->height = out_h;
    out->x_offset = min_x;
    out->y_offset = max_y; // baseline distance
    out->advance = advance;
    out->pixels = calloc(out_w * out_h, 4);
    out->glyph_id = glyph_index;
    if (!out->pixels) {
        free(out);
        out = NULL;
        goto cleanup;
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

cleanup:
    for (int i = 0; i < layer_count; i++)
        free(layers[i].bmp.buffer);
    if (layers_on_heap)
        free(layers);

    return out;
}

// Rasterize glyph into single-channel alpha mask via FT_Get_Glyph + FT_Glyph_To_Bitmap
unsigned char *rasterize_glyph_mask(FtFontData *ft_data, FT_UInt glyph_index,
                                    int *out_w, int *out_h, int *out_left, int *out_top)
{
    if (!ft_data || !ft_data->ft_face)
        return NULL;
    FT_Face face = ft_data->ft_face;

    FT_Int32 load_flags = FT_LOAD_NO_BITMAP | ft_data->ft_hint_target;

    FT_Error err = FT_Load_Glyph(face, glyph_index, load_flags);
    if (err)
        return NULL;

    FT_Glyph glyph = NULL;
    err = FT_Get_Glyph(face->glyph, &glyph);
    if (err || !glyph)
        return NULL;

    // Convert glyph to bitmap (keeps hinting/size effects)
    err = FT_Glyph_To_Bitmap(&glyph, FT_LOAD_TARGET_MODE(ft_data->ft_hint_target), NULL, 1);
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
void resolve_colorindex(FtFontData *ft_data, FT_ColorIndex ci, uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
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

    // Apply alpha from FT_F2Dot14 (2.14 fixed point: 0x4000 = 1.0, 0 = 0.0)
    // Always compute a_scale from ci.alpha - when alpha=0, result should be transparent
    double a_scale = (double)ci.alpha / (double)(1 << 14);
    if (a_scale < 0.0)
        a_scale = 0.0;
    if (a_scale > 1.0)
        a_scale = 1.0;

    *out_r = pr;
    *out_g = pg;
    *out_b = pb;
    *out_a = (uint8_t)round(pa * a_scale);
}

// Initialize FreeType font
static void *ft_init_font(FontBackend *font, const char *font_path,
                          float font_size, FontStyle style, const FontOptions *options)
{
    (void)font; // Unused

    vlog("Initializing FreeType font from %s, size %.1f, style %d\n", font_path, font_size, style);

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

    // Set font options
    if (options) {
        ft_data->ft_hint_target = options->ft_hint_target;
        ft_data->dpi_x = options->dpi_x;
        ft_data->dpi_y = options->dpi_y;
    } else {
        ft_data->ft_hint_target = FT_LOAD_NO_HINTING;
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
        // Bitmap-only fonts (e.g. CBDT/CBLC emoji) don't support arbitrary sizes;
        // fall back to the first available bitmap strike.
        if (FT_HAS_FIXED_SIZES(ft_data->ft_face) && ft_data->ft_face->num_fixed_sizes > 0) {
            if (FT_Select_Size(ft_data->ft_face, 0) != 0) {
                vlog("Failed to select bitmap strike for FreeType face\n");
                FT_Done_Face(ft_data->ft_face);
                free(font_data);
                free(ft_data);
                return NULL;
            }
            vlog("Selected bitmap strike (size %d) for %s\n",
                 ft_data->ft_face->size->metrics.y_ppem, font_path);
        } else {
            vlog("Failed to set character size for FreeType face\n");
            FT_Done_Face(ft_data->ft_face);
            free(font_data);
            free(ft_data);
            return NULL;
        }
    }

    // Calculate scale: pixels-per-em / units-per-em
    // Bitmap-only fonts have units_per_EM == 0; their glyphs are pre-rendered
    // at a fixed size so scale is 1.0.
    float scale;
    if (ft_data->ft_face->units_per_EM > 0) {
        float ppem = font_size * ft_data->dpi_x / 72.0f;
        scale = ppem / (float)ft_data->ft_face->units_per_EM;
        vlog("Font scale factor: %f (ppem=%.1f, upem=%d, dpi=%d)\n",
             scale, ppem, ft_data->ft_face->units_per_EM, ft_data->dpi_x);
    } else {
        scale = 1.0f;
        vlog("Font scale factor: 1.0 (bitmap-only font)\n");
    }
    ft_data->scale = scale;

    // Initialize HarfBuzz font from FreeType face
    ft_data->hb_font = hb_ft_font_create_referenced(ft_data->ft_face);
    if (!ft_data->hb_font) {
        vlog("Warning: failed to create HarfBuzz font for %s\n", font_path);
    }

    // Create reusable HarfBuzz shaping buffer
    ft_data->hb_buf = hb_buffer_create();
    if (!ft_data->hb_buf) {
        vlog("Warning: failed to create HarfBuzz buffer for %s\n", font_path);
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

    // Detect whether synthetic bold is needed: if we requested bold but the
    // font isn't actually bold (no variable weight axis was applied, and the
    // OS/2 weight class is below semi-bold threshold of 600).
    if (style == FONT_STYLE_BOLD || style == FONT_STYLE_BOLD_ITALIC) {
        bool has_weight_axis = false;
        if (ft_data->mm_var) {
            for (FT_UInt i = 0; i < ft_data->mm_var->num_axis; i++) {
                if (ft_data->mm_var->axis[i].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
                    has_weight_axis = true;
                    break;
                }
            }
        }
        if (!has_weight_axis) {
            TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table(ft_data->ft_face, FT_SFNT_OS2);
            if (os2 && os2->usWeightClass < 600) {
                ft_data->synthetic_bold = true;
                vlog("Enabling synthetic bold for %s (weight class %d, no variable weight axis)\n",
                     font_path, os2->usWeightClass);
            }
        }
    }

    // Detect whether synthetic italic is needed: if we requested italic but
    // the font has no ital/slnt axis and the face isn't natively italic.
    if (style == FONT_STYLE_ITALIC || style == FONT_STYLE_BOLD_ITALIC) {
        bool has_italic_axis = false;
        if (ft_data->mm_var) {
            for (FT_UInt i = 0; i < ft_data->mm_var->num_axis; i++) {
                if (ft_data->mm_var->axis[i].tag == FT_MAKE_TAG('i', 't', 'a', 'l') ||
                    ft_data->mm_var->axis[i].tag == FT_MAKE_TAG('s', 'l', 'n', 't')) {
                    has_italic_axis = true;
                    break;
                }
            }
        }
        if (!has_italic_axis && !(ft_data->ft_face->style_flags & FT_STYLE_FLAG_ITALIC)) {
            ft_data->synthetic_italic = true;
            vlog("Enabling synthetic italic for %s (no ital/slnt axis, face not italic)\n",
                 font_path);
        }
    }

    vlog("Created FreeType font from %s\n", font_path);

    return ft_data;
}

// Destroy FreeType font
static void ft_destroy_font(FontBackend *font, void *font_data)
{
    (void)font; // Unused

    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data)
        return;

    if (ft_data->hb_buf) {
        hb_buffer_destroy(ft_data->hb_buf);
        ft_data->hb_buf = NULL;
    }

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
    free(ft_data->coords_scratch);
    ft_data->coords_scratch = NULL;

    if (ft_data->ft_face) {
        FT_Done_Face(ft_data->ft_face);
    }

    free(ft_data->font_data);
    free(ft_data);
}

// Get font metrics
static bool ft_get_metrics(FontBackend *font, void *font_data, FontMetrics *metrics)
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

    // Cell height = font's recommended baseline-to-baseline distance (height metric)
    metrics->cell_height = face->size->metrics.height >> 6;

    // Measure actual cap height from 'X' for visual centering
    metrics->cap_height = metrics->ascent; // fallback: use declared ascent
    FT_UInt x_index = FT_Get_Char_Index(face, 'X');
    if (x_index != 0) {
        FT_Error x_err = FT_Load_Glyph(face, x_index, ft_data->ft_hint_target);
        if (x_err == 0) {
            FT_Error r_err = FT_Render_Glyph(face->glyph,
                                             FT_LOAD_TARGET_MODE(ft_data->ft_hint_target));
            if (r_err == 0)
                metrics->cap_height = face->glyph->bitmap_top;
        }
    }

    // For glyph dimensions, we'll use the actual metrics from the face
    // This is a simplified approach - in a full implementation we'd measure actual glyphs
    metrics->glyph_width = metrics->cell_width;   // Approximation for now
    metrics->glyph_height = metrics->cell_height; // Approximation for now

    vlog("FreeType metrics (26.6 fixed-point): asc=%d, des=%d, line_gap=%d, cap_height=%d, glyph=%dx%d, cell=%dx%d\n",
         metrics->ascent, metrics->descent, metrics->line_gap, metrics->cap_height,
         metrics->glyph_width, metrics->glyph_height,
         metrics->cell_width, metrics->cell_height);

    return true;
}

// Render glyphs using FreeType directly (unified function for single/multiple codepoints)
static GlyphBitmap *ft_render_glyph(FontBackend *font, void *font_data,
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

    // Load the glyph index for this codepoint
    FT_UInt glyph_index = FT_Get_Char_Index(ft_data->ft_face, codepoint);
    if (glyph_index == 0) {
        vlog("No glyph index found for U+%04X\n", codepoint);
        return NULL;
    }

    return rasterize_glyph_index(ft_data, glyph_index, fg_r, fg_g, fg_b);
}

// Helper: rasterize a glyph by glyph index
GlyphBitmap *rasterize_glyph_index(FtFontData *ft_data, FT_UInt glyph_index,
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

    // Set hinting flags
    FT_Int32 load_flags = ft_data->ft_hint_target;

    // If COLR and FreeType supports FT_LOAD_COLOR, try color render path first
    // (handles COLR v0 layers and bitmap color fonts like sbix/CBDT;
    //  COLR v1 paint glyphs are handled above via render_colr_paint_glyph)
    if (FT_HAS_COLOR(face)) {
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

    // If the glyph's actual ink width exceeds the available pixel budget,
    // apply a uniform scale transform and re-rasterize so the bitmap fits
    // (avoids lossy post-render bitmap scaling in the renderer).
    // Use metrics.width (ink extent) not advance.x (which includes sidebearings).
    FT_GlyphSlot slot_pre = face->glyph;
    int ink_width = (int)(slot_pre->metrics.width >> 6);
    int bearing_x = (int)(slot_pre->metrics.horiBearingX >> 6);
    int glyph_extent = bearing_x + ink_width; // rightmost pixel
    double glyph_scale = 0.0;                 // non-zero when glyph was scaled down
    // CJK fallback: pre-scale via FreeType so the rasterized glyph fits the
    // cell at high quality and stays baseline-aware.
    // Symbols/emoji are intentionally not pre-scaled here — the renderer
    // routes them through its CPU box-filter downscale (same path color
    // emoji use), which gives a softer result than re-rasterizing at a
    // non-design-grid size.
    // Plain text glyphs (ASCII bold) that overflow are left to the renderer
    // to clip so stroke weight is preserved.
    bool is_fallback = (ft_data->style == FONT_STYLE_FALLBACK);
    int target = ft_data->presentation_width > 0 ? ft_data->presentation_width
                                                 : ft_data->target_cell_width;
    if (is_fallback && target > 0 && glyph_extent > target) {
        glyph_scale = (double)target / glyph_extent;
        FT_Fixed scale_16_16 = (FT_Fixed)(glyph_scale * 0x10000);
        FT_Matrix matrix = { scale_16_16, 0, 0, scale_16_16 };
        vlog("rasterize_glyph_index: glyph %u extent %d (ink=%d+bearing=%d) > target %d, scaling by %.3f\n",
             glyph_index, glyph_extent, ink_width, bearing_x,
             target, glyph_scale);
        FT_Set_Transform(face, &matrix, NULL);
        error = FT_Load_Glyph(face, glyph_index, load_flags);
        FT_Set_Transform(face, NULL, NULL); // reset transform
        if (error) {
            vlog("rasterize_glyph_index: Failed to reload scaled glyph %u: error %d\n", glyph_index, error);
            return NULL;
        }
    }

    if (ft_data->synthetic_bold)
        FT_GlyphSlot_Embolden(face->glyph);
    if (ft_data->synthetic_italic)
        FT_GlyphSlot_Oblique(face->glyph);

    // Render glyph to bitmap
    error = FT_Render_Glyph(face->glyph, FT_LOAD_TARGET_MODE(ft_data->ft_hint_target));
    if (error) {
        vlog("rasterize_glyph_index: Failed to render glyph index %u: error %d\n", glyph_index, error);
        return NULL;
    }

    FT_GlyphSlot slot = face->glyph;
    FT_Bitmap *bitmap = &slot->bitmap;

    if (bitmap->width == 0 || bitmap->rows == 0)
        return NULL;

    GlyphBitmap *glyph_bitmap = malloc(sizeof(GlyphBitmap));
    if (!glyph_bitmap)
        return NULL;

    glyph_bitmap->width = bitmap->width;
    glyph_bitmap->height = bitmap->rows;
    glyph_bitmap->x_offset = slot->bitmap_left;
    glyph_bitmap->y_offset = slot->bitmap_top;
    glyph_bitmap->advance = (int)(slot->advance.x >> 6);
    glyph_bitmap->glyph_id = glyph_index;
    glyph_bitmap->centered = false;

    // When the glyph was scaled down via FT_Set_Transform, baseline-relative
    // placement no longer makes sense (the scaled bitmap_top would anchor a
    // shrunken glyph against the original baseline, parking it low in the
    // cell). Mark it for cell-centered placement and let the renderer drop
    // the baseline arithmetic for this entry.
    if (glyph_scale > 0.0) {
        glyph_bitmap->x_offset = (target - (int)bitmap->width) / 2;
        glyph_bitmap->centered = true;
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

    vlog("rasterize_glyph_index: Rendered glyph %u: %dx%d\n", glyph_index, glyph_bitmap->width, glyph_bitmap->height);
    return glyph_bitmap;
}

// Render a single glyph by its font glyph index (for atlas cache misses)
static GlyphBitmap *ft_render_glyph_by_id(FontBackend *font, void *font_data,
                                          uint32_t glyph_id,
                                          uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    (void)font;
    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data || glyph_id == 0)
        return NULL;
    return rasterize_glyph_index(ft_data, (FT_UInt)glyph_id, fg_r, fg_g, fg_b);
}

// Shape a run with HarfBuzz, returning glyph IDs and positions without rasterizing.
// The caller is responsible for rendering individual glyphs (e.g. after checking an atlas cache).
static ShapedGlyphs *ft_render_shaped(FontBackend *font, void *font_data,
                                      uint32_t *codepoints, int codepoint_count,
                                      uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
    (void)font;
    (void)fg_r;
    (void)fg_g;
    (void)fg_b;
    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data || !ft_data->hb_font || !ft_data->hb_buf || !codepoints || codepoint_count <= 0)
        return NULL;

    hb_buffer_t *buf = ft_data->hb_buf;
    hb_buffer_clear_contents(buf);

    hb_buffer_add_utf32(buf, codepoints, codepoint_count, 0, codepoint_count);
    hb_buffer_guess_segment_properties(buf);

    hb_shape(ft_data->hb_font, buf, NULL, 0);

    unsigned int glyph_count = 0;
    hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
    hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buf, &glyph_count);

    if (!glyph_info || glyph_count == 0)
        return NULL;

    ShapedGlyphs *run = calloc(1, sizeof(ShapedGlyphs));
    if (!run)
        return NULL;

    run->num_glyphs = (int)glyph_count;
    run->glyph_ids = calloc(glyph_count, sizeof(uint32_t));
    run->bitmaps = NULL;
    run->x_positions = calloc(glyph_count, sizeof(int));
    run->y_positions = calloc(glyph_count, sizeof(int));
    run->x_advances = calloc(glyph_count, sizeof(int));
    run->total_advance = 0;

    int32_t cursor_x = 0, cursor_y = 0;
    for (unsigned int i = 0; i < glyph_count; i++) {
        run->glyph_ids[i] = (uint32_t)glyph_info[i].codepoint;

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

    return run;
}

// Set a single variation axis value
static bool ft_set_variation_axis(FontBackend *font, void *font_data, const char *axis_tag, float value)
{
    (void)font;
    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data || !ft_data->ft_face || !axis_tag || strlen(axis_tag) < 4)
        return false;

    cache_mm_var(ft_data);
    if (!ft_data->mm_var)
        return false;

    FT_MM_Var *mm_var = ft_data->mm_var;
    FT_Fixed *coords = ft_data->coords_scratch;
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

    if (err == 0) {
        if (ft_data->hb_font)
            hb_ft_font_changed(ft_data->hb_font);
        return true;
    }
    return false;
}

// Set multiple variation axis coordinates
static bool ft_set_variation_axes(FontBackend *font, void *font_data, float *coords_in, int num_coords)
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
    FT_Fixed *coords = ft_data->coords_scratch;
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

    if (err == 0) {
        if (ft_data->hb_font)
            hb_ft_font_changed(ft_data->hb_font);
        return true;
    }
    return false;
}

// Get glyph index for a codepoint (no rasterization)
static uint32_t ft_get_glyph_index(FontBackend *font, void *font_data, uint32_t codepoint)
{
    (void)font;
    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data)
        return 0;
    return FT_Get_Char_Index(ft_data->ft_face, codepoint);
}

// Get glyph info without rendering
static bool ft_get_glyph_info(FontBackend *font, void *font_data, uint32_t codepoint,
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
static bool ft_style_has_colr(FontBackend *font, void *font_data)
{
    (void)font;
    FtFontData *ft_data = (FtFontData *)font_data;
    if (!ft_data)
        return false;
    return ft_data->has_colr;
}

// Set target cell width on a single FtFontData (vtable callback)
static void ft_set_target_cell_width(void *font_data, int cell_width)
{
    FtFontData *ft_data = (FtFontData *)font_data;
    if (ft_data)
        ft_data->target_cell_width = cell_width;
}

// Set per-render presentation width (vtable callback)
static void ft_set_presentation_width(void *font_data, int width)
{
    FtFontData *ft_data = (FtFontData *)font_data;
    if (ft_data)
        ft_data->presentation_width = width;
}

FontBackend font_backend_ft = {
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
    .render_glyph_id = ft_render_glyph_by_id,
    .get_glyph_info = ft_get_glyph_info,
    .free_glyph_bitmap = ft_free_glyph_bitmap,
    .load_font = font_load_font,
    .get_style_metrics = font_get_metrics,
    .has_style = font_has_style,
    .style_has_colr = ft_style_has_colr,
    .get_glyph_index = ft_get_glyph_index,
    .set_target_cell_width = ft_set_target_cell_width,
    .set_presentation_width = ft_set_presentation_width
};
