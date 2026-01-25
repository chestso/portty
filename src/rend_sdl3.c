#include "rend_sdl3.h"
#include "common.h"
#include "font.h"
#include "font_ft.h"
#include "font_resolver.h"
#include "rend.h"
#include "rend_sdl3_atlas.h"
#include "unicode.h"
#include <SDL3/SDL.h>
#include <stdint.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define EMOJI_FONT_SCALE 4.0f

// Box-filter downscale a glyph bitmap to fit within max_w x max_h.
// Returns a newly allocated GlyphBitmap, or NULL if no downscale is needed.
static GlyphBitmap *downscale_bitmap(GlyphBitmap *src, int max_w, int max_h)
{
    if (!src || !src->pixels || src->width <= 0 || src->height <= 0)
        return NULL;
    if (src->width <= max_w && src->height <= max_h)
        return NULL;

    float scale_x = (float)max_w / (float)src->width;
    float scale_y = (float)max_h / (float)src->height;
    float scale = fminf(scale_x, scale_y);

    int dst_w = (int)(src->width * scale + 0.5f);
    int dst_h = (int)(src->height * scale + 0.5f);
    if (dst_w <= 0)
        dst_w = 1;
    if (dst_h <= 0)
        dst_h = 1;

    uint8_t *dst_pixels = calloc((size_t)dst_w * dst_h, 4);
    if (!dst_pixels)
        return NULL;

    for (int dy = 0; dy < dst_h; dy++) {
        int sy0 = dy * src->height / dst_h;
        int sy1 = (dy + 1) * src->height / dst_h;
        if (sy1 > src->height)
            sy1 = src->height;
        if (sy0 == sy1)
            sy1 = sy0 + 1;

        for (int dx = 0; dx < dst_w; dx++) {
            int sx0 = dx * src->width / dst_w;
            int sx1 = (dx + 1) * src->width / dst_w;
            if (sx1 > src->width)
                sx1 = src->width;
            if (sx0 == sx1)
                sx1 = sx0 + 1;

            float pr_sum = 0, pg_sum = 0, pb_sum = 0, a_sum = 0;
            int count = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                for (int sx = sx0; sx < sx1; sx++) {
                    uint8_t *p = src->pixels + (sy * src->width + sx) * 4;
                    float a = p[3] / 255.0f;
                    pr_sum += p[0] * a;
                    pg_sum += p[1] * a;
                    pb_sum += p[2] * a;
                    a_sum += p[3];
                    count++;
                }
            }
            if (count > 0) {
                uint8_t *dp = dst_pixels + (dy * dst_w + dx) * 4;
                float avg_a = a_sum / count;
                if (avg_a > 0.5f) {
                    float inv = 255.0f / a_sum;
                    dp[0] = (uint8_t)fminf(pr_sum * inv + 0.5f, 255.0f);
                    dp[1] = (uint8_t)fminf(pg_sum * inv + 0.5f, 255.0f);
                    dp[2] = (uint8_t)fminf(pb_sum * inv + 0.5f, 255.0f);
                } else {
                    dp[0] = dp[1] = dp[2] = 0;
                }
                dp[3] = (uint8_t)(avg_a + 0.5f);
            }
        }
    }

    GlyphBitmap *result = malloc(sizeof(GlyphBitmap));
    if (!result) {
        free(dst_pixels);
        return NULL;
    }
    result->pixels = dst_pixels;
    result->width = dst_w;
    result->height = dst_h;
    result->x_offset = (int)(src->x_offset * scale + 0.5f);
    result->y_offset = (int)(src->y_offset * scale + 0.5f);
    result->advance = (int)(src->advance * scale + 0.5f);
    result->glyph_id = src->glyph_id;
    return result;
}

typedef struct RendererSdl3Data
{
    SDL_Renderer *renderer;
    SDL_Window *window;
    FontBackend *font;
    int cell_width;
    int cell_height;
    int char_width;
    int char_height;
    int font_ascent;
    int font_descent;
    int width;
    int height;
    int debug_grid;
    bool full_redraw;

    SDL_Texture *terminal_texture;
    RendSdl3Atlas atlas;
} RendererSdl3Data;

// Function to combine emoji cells for multi-codepoint sequences
static int combine_cells_for_emoji(TerminalBackend *term, int row, int col,
                                   uint32_t *combined_codepoints,
                                   int max_combined,
                                   int *columns_consumed)
{
    int term_rows, term_cols;
    terminal_get_dimensions(term, &term_rows, &term_cols);

    // Read the first cell
    TerminalCell cell;
    if (terminal_get_cell(term, row, col, &cell) < 0 || cell.chars[0] == 0) {
        *columns_consumed = 0;
        return 0;
    }

    vlog("combine_cells_for_emoji: starting at (%d,%d), first char=U+%04X, width=%d, term_cols=%d\n",
         row, col, cell.chars[0], cell.width, term_cols);

    // Collect first cell's codepoints
    int cp_count = 0;
    for (int i = 0; i < TERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; i++) {
        if (cp_count >= max_combined - 1) {
            break;
        }
        combined_codepoints[cp_count++] = cell.chars[i];
        vlog("  Added U+%04X from first cell\n", cell.chars[i]);
    }

    int col_offset = cell.width;          // Track column offset (not cell offset!)
    int lookahead_col = col + cell.width; // Next column to check

    // Look ahead for combinable cells
    const int MAX_LOOKAHEAD = 10; // Maximum cells to look ahead
    int cells_checked = 1;        // Already checked first cell

    vlog("  Looking ahead starting from column %d\n", lookahead_col);

    while (lookahead_col < term_cols && cells_checked < MAX_LOOKAHEAD) {
        TerminalCell next_cell;
        if (terminal_get_cell(term, row, lookahead_col, &next_cell) < 0) {
            vlog("  Can't read cell at column %d\n", lookahead_col);
            break; // Can't read cell
        }

        if (next_cell.chars[0] == 0) {
            vlog("  Empty cell at column %d\n", lookahead_col);
            break; // Empty cell, end of content
        }

        // Check if we should combine this cell
        uint32_t first_cp = next_cell.chars[0];
        vlog("  Checking cell at column %d: U+%04X (width=%d)\n", lookahead_col, first_cp, next_cell.width);
        bool should_combine = false;

        // Check if we just collected a ZWJ - if so, continue to get the next emoji
        if (cp_count > 0 && is_zwj(combined_codepoints[cp_count - 1])) {
            // After ZWJ, combine next emoji
            if (is_emoji_base_range(first_cp)) {
                should_combine = true;
            }
        }
        // Check if next cell has a skin tone modifier
        else if (is_skin_tone_modifier(first_cp)) {
            vlog("  Next cell U+%04X is skin tone modifier\n", first_cp);
            // Skin tone can combine with base emoji
            if (cp_count > 0 && is_emoji_base_range(combined_codepoints[0])) {
                vlog("  And we have base emoji U+%04X - should combine!\n", combined_codepoints[0]);
                should_combine = true;
            } else {
                vlog("  But no base emoji (cp_count=%d, first=U+%04X)\n", cp_count,
                     cp_count > 0 ? combined_codepoints[0] : 0);
            }
        }
        // Check if we're collecting regional indicators for a flag
        else if (is_regional_indicator(first_cp)) {
            // Check if first codepoint is also RI
            if (cp_count > 0 && is_regional_indicator(combined_codepoints[0])) {
                // Only combine first two RIs (flag emoji)
                if (cp_count == 1) {
                    should_combine = true;
                }
            }
        }
        // Check if next cell has ZWJ - always combine ZWJ
        else if (is_zwj(first_cp)) {
            should_combine = true;
        }

        if (!should_combine) {
            vlog("  Not combining with U+%04X at col %d\n", first_cp, lookahead_col);
            break; // Not part of sequence
        }

        vlog("  Combining with U+%04X at col %d (width=%d)\n", first_cp, lookahead_col, next_cell.width);

        // Add this cell's codepoints
        for (int i = 0; i < TERM_MAX_CHARS_PER_CELL && next_cell.chars[i] != 0; i++) {
            if (cp_count >= max_combined - 1) {
                break;
            }
            combined_codepoints[cp_count++] = next_cell.chars[i];
        }

        col_offset += next_cell.width;
        lookahead_col += next_cell.width; // Advance by cell width, not 1
        cells_checked++;
    }

    // Ensure null termination for safety
    if (cp_count < max_combined) {
        combined_codepoints[cp_count] = 0; // null terminate
    }

    vlog("combine_cells_for_emoji: collected %d codepoints, %d columns consumed\n",
         cp_count, col_offset);

    // Return number of codepoints that were combined
    *columns_consumed = col_offset;
    return cp_count;
}

static bool sdl3_init(RendererBackend *backend, void *window_handle, void *renderer_handle)
{
    // Allocate SDL3-specific data
    RendererSdl3Data *data = malloc(sizeof(RendererSdl3Data));
    if (!data)
        return false;

    // Cast handles back to SDL types
    data->window = (SDL_Window *)window_handle;
    data->renderer = (SDL_Renderer *)renderer_handle;

    // Initialize fields
    data->font = NULL;
    data->cell_width = 0;
    data->cell_height = 0;
    data->char_width = 0;
    data->char_height = 0;
    data->font_ascent = 0;
    data->font_descent = 0;
    data->width = 0;
    data->height = 0;
    data->debug_grid = 0;
    data->full_redraw = true;
    data->terminal_texture = NULL;

    // Initialize glyph atlas
    if (!rend_sdl3_atlas_init(&data->atlas, data->renderer)) {
        vlog("Failed to initialize glyph atlas\n");
        free(data);
        return false;
    }

    // Initialize font backend with FreeType backend
    data->font = &font_backend_ft;
    if (!font_init(data->font)) {
        vlog("Failed to initialize font backend\n");
        rend_sdl3_atlas_destroy(&data->atlas);
        free(data);
        return false;
    }

    // Store in backend
    backend->backend_data = data;

    return true;
}

static void sdl3_destroy(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;

    if (data->terminal_texture) {
        SDL_DestroyTexture(data->terminal_texture);
        data->terminal_texture = NULL;
    }

    // Destroy glyph atlas
    rend_sdl3_atlas_destroy(&data->atlas);

    if (data->font) {
        font_destroy(data->font);
    }

    free(data);
    backend->backend_data = NULL;
}

static bool load_font_style(FontBackend *font, FontType type, FontStyle style,
                            const char *font_name, float font_size,
                            const FontOptions *options, const char *label)
{
    FontResolutionResult result;
    if (font_resolver_find_font(type, font_name, &result) != 0)
        return false;

    bool ok = font_load_font(font, style, result.font_path, font_size, options);
    if (ok)
        vlog("%s font loaded successfully from %s\n", label, result.font_path);
    else
        vlog("Failed to load %s font from %s\n", label, result.font_path);

    font_resolver_free_result(&result);
    return ok;
}

static int sdl3_load_fonts(RendererBackend *backend, float font_size, const char *font_name, int ft_hint_target)
{
    if (!backend || !backend->backend_data)
        return -1;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;

    const char *hint_name = "none";
    if (ft_hint_target == FT_LOAD_TARGET_LIGHT)
        hint_name = "light";
    else if (ft_hint_target == FT_LOAD_TARGET_NORMAL)
        hint_name = "normal";
    else if (ft_hint_target == FT_LOAD_TARGET_MONO)
        hint_name = "mono";
    vlog("Loading fonts with size %.1f, hinting=%s\n", font_size, hint_name);

    // Initialize font resolver
    if (font_resolver_init() != 0) {
        fprintf(stderr, "Failed to initialize font resolver\n");
        return -1;
    }

    // Setup DPI options
    FontOptions options = { 0 };
    options.ft_hint_target = ft_hint_target;
    options.subpixel_order = 0;
    options.lcd_filter = 0;
    options.dpi_x = 96;
    options.dpi_y = 96;

    // Get DPI from window if available
    if (data->window) {
        float pixel_density = SDL_GetWindowPixelDensity(data->window);
        if (pixel_density > 0.0f) {
            // Calculate DPI based on pixel density (assuming 96 DPI as base)
            // This is a reasonable approximation for HiDPI displays
            int dpi = (int)(96.0f * pixel_density);
            options.dpi_x = dpi;
            options.dpi_y = dpi;
            vlog("SDL Pixel Density: %.2f (calculated DPI: %d)\n", pixel_density, dpi);
        } else {
            // Fallback to default DPI
            vlog("Failed to get window pixel density: %s\n", SDL_GetError());
        }
    }

    // Load normal monospace font (required)
    if (!load_font_style(data->font, FONT_TYPE_NORMAL, FONT_STYLE_NORMAL,
                         font_name, font_size, &options, "Normal")) {
        fprintf(stderr, "Failed to load or find normal font\n");
        font_resolver_cleanup();
        return -1;
    }

    // Load bold font (optional)
    load_font_style(data->font, FONT_TYPE_BOLD, FONT_STYLE_BOLD,
                    font_name, font_size, &options, "Bold");

    // Load emoji font (optional)
    load_font_style(data->font, FONT_TYPE_EMOJI, FONT_STYLE_EMOJI,
                    NULL, font_size * EMOJI_FONT_SCALE, &options, "Emoji");

    // Cleanup font resolver
    font_resolver_cleanup();

    // Calculate cell dimensions from font metrics using normal font
    const FontMetrics *metrics = font_get_metrics(data->font, FONT_STYLE_NORMAL);
    if (!metrics) {
        vlog("ERROR: No font available for metrics calculation\n");
        return -1;
    }

    data->font_ascent = metrics->ascent;
    data->font_descent = metrics->descent;
    data->char_width = metrics->glyph_width;
    data->char_height = metrics->glyph_height;
    data->cell_width = metrics->cell_width;
    data->cell_height = metrics->cell_height;

    vlog("Font metrics - ascent: %d, descent: %d\n",
         data->font_ascent, data->font_descent);
    vlog("Character dimensions - width: %d, height: %d\n",
         data->char_width, data->char_height);
    vlog("Cell dimensions - width: %d, height: %d\n",
         data->cell_width, data->cell_height);

    // Log which fonts were successfully loaded
    vlog("Font loading summary:\n");
    vlog("  Normal font: %s\n", font_has_style(data->font, FONT_STYLE_NORMAL) ? "Loaded" : "Not loaded");
    vlog("  Bold font: %s\n", font_has_style(data->font, FONT_STYLE_BOLD) ? "Loaded" : "Not loaded");
    vlog("  Emoji font: %s\n", font_has_style(data->font, FONT_STYLE_EMOJI) ? "Loaded" : "Not loaded");

    return 0;
}

static RendSdl3AtlasEntry *cache_glyph(RendSdl3Atlas *atlas, void *font_data,
                                       uint32_t glyph_id, uint32_t color_key,
                                       GlyphBitmap *bitmap, FontStyle style,
                                       int max_w, int max_h)
{
    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_lookup(atlas, font_data, glyph_id, color_key);
    if (entry)
        return entry;

    GlyphBitmap *scaled = NULL;
    if (style == FONT_STYLE_EMOJI)
        scaled = downscale_bitmap(bitmap, max_w, max_h);
    entry = rend_sdl3_atlas_insert(atlas, font_data, glyph_id, color_key,
                                   scaled ? scaled : bitmap);
    if (scaled) {
        free(scaled->pixels);
        free(scaled);
    }
    return entry;
}

static void blit_glyph(SDL_Renderer *renderer, RendSdl3Atlas *atlas,
                       RendSdl3AtlasEntry *entry, FontStyle style,
                       int cell_x, int cell_y, int glyph_x_offset, int glyph_y_offset,
                       int avail_w, int avail_h, int font_ascent)
{
    if (!entry || entry->region.w <= 0)
        return;

    SDL_FRect src = { (float)entry->region.x, (float)entry->region.y,
                      (float)entry->region.w, (float)entry->region.h };
    SDL_FRect dst;
    if (style == FONT_STYLE_EMOJI) {
        float glyph_w = (float)entry->region.w;
        float glyph_h = (float)entry->region.h;
        float scale = fminf((float)avail_w / glyph_w, (float)avail_h / glyph_h);
        float scaled_w = glyph_w * scale;
        float scaled_h = glyph_h * scale;
        dst = (SDL_FRect){
            (float)cell_x,
            (float)cell_y + ((float)avail_h - scaled_h) * 0.5f,
            scaled_w, scaled_h
        };
    } else {
        dst = (SDL_FRect){
            (float)cell_x + glyph_x_offset,
            (float)cell_y + font_ascent - glyph_y_offset,
            (float)entry->region.w, (float)entry->region.h
        };
    }
    SDL_RenderTexture(renderer, atlas->pages[entry->page_index].texture, &src, &dst);
}

static int render_cell(RendererSdl3Data *data, TerminalBackend *term,
                       int row, int col, TerminalPos cursor_pos)
{
    TerminalCell cell;
    if (terminal_get_cell(term, row, col, &cell) < 0)
        return 1;

    // Draw background if non-default
    Uint8 r = cell.fg.r, g = cell.fg.g, b = cell.fg.b;
    if (!cell.bg.is_default) {
        SDL_FRect bg_rect = {
            (float)(col * data->cell_width),
            (float)(row * data->cell_height),
            (float)data->cell_width,
            (float)data->cell_height
        };
        SDL_SetRenderDrawColor(data->renderer, cell.bg.r, cell.bg.g, cell.bg.b, 255);
        SDL_RenderFillRect(data->renderer, &bg_rect);
    }

    if (cell.chars[0] == 0)
        return 1;

    // Collect codepoints (emoji combining logic)
    uint32_t cps[TERM_MAX_CHARS_PER_CELL];
    int cp_count = 0;
    int columns_to_consume = cell.width;

    if (is_emoji_presentation(cell.chars[0]) || is_regional_indicator(cell.chars[0]) ||
        is_zwj(cell.chars[0]) || is_skin_tone_modifier(cell.chars[0])) {
        uint32_t combined_cps[TERM_MAX_CHARS_PER_CELL * 2];
        int columns_consumed = 0;
        int combined_cp_count = combine_cells_for_emoji(term, row, col, combined_cps,
                                                        sizeof(combined_cps) / sizeof(combined_cps[0]),
                                                        &columns_consumed);
        if (combined_cp_count > 1) {
            columns_to_consume = columns_consumed;
            cp_count = combined_cp_count;
            for (int i = 0; i < cp_count; i++)
                cps[i] = combined_cps[i];
        } else {
            for (int i = 0; i < TERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; i++)
                cps[cp_count++] = cell.chars[i];
        }
    } else {
        for (int i = 0; i < TERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; i++)
            cps[cp_count++] = cell.chars[i];
    }

    // Select font style
    FontStyle style = FONT_STYLE_NORMAL;
    if (cell.attrs.bold && font_has_style(data->font, FONT_STYLE_BOLD))
        style = FONT_STYLE_BOLD;
    if (cp_count > 0) {
        bool use_emoji = is_emoji_presentation(cps[0]) || is_regional_indicator(cps[0]);
        if (!use_emoji) {
            for (int i = 1; i < cp_count; i++) {
                if (cps[i] == 0xFE0F) {
                    use_emoji = true;
                    break;
                }
            }
        }
        if (use_emoji && font_has_style(data->font, FONT_STYLE_EMOJI))
            style = FONT_STYLE_EMOJI;
    }

    int cell_x = col * data->cell_width;
    int cell_y = row * data->cell_height;
    int avail_w = columns_to_consume * data->cell_width;
    int avail_h = data->cell_height;
    void *font_data = data->font->font_data[style];
    uint32_t color_key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

    // Shaped rendering path (multiple codepoints)
    if (cp_count > 1 && data->font->render_shaped) {
        ShapedGlyphs *shaped = font_render_shaped_text(data->font, style, cps, cp_count, r, g, b);
        if (shaped) {
            for (int gi = 0; gi < shaped->num_glyphs; gi++) {
                GlyphBitmap *gb = shaped->bitmaps[gi];
                if (!gb)
                    continue;
                RendSdl3AtlasEntry *entry = cache_glyph(&data->atlas, font_data,
                                                        gb->glyph_id, color_key,
                                                        gb, style, avail_w, avail_h);
                int x_off = shaped->x_positions[gi] + (entry ? entry->x_offset : 0);
                int y_off = entry ? entry->y_offset : 0;
                blit_glyph(data->renderer, &data->atlas, entry, style,
                           cell_x, cell_y, x_off, y_off, avail_w, avail_h, data->font_ascent);
                data->font->free_glyph_bitmap(data->font, gb);
            }
            free(shaped->bitmaps);
            free(shaped->x_positions);
            free(shaped->y_positions);
            free(shaped->x_advances);
            free(shaped);
            goto render_cursor;
        }
    }

    // Single glyph fallback
    {
        uint32_t codepoint = cps[0];
        uint32_t glyph_index = font_get_glyph_index(data->font, style, codepoint);
        RendSdl3AtlasEntry *entry = NULL;
        if (glyph_index != 0)
            entry = rend_sdl3_atlas_lookup(&data->atlas, font_data, glyph_index, color_key);
        if (!entry) {
            GlyphBitmap *glyph_bitmap = font_render_glyphs(data->font, style, &codepoint, 1, r, g, b);
            if (glyph_bitmap) {
                uint32_t insert_id = glyph_index ? glyph_index : (uint32_t)glyph_bitmap->glyph_id;
                entry = cache_glyph(&data->atlas, font_data, insert_id, color_key,
                                    glyph_bitmap, style, avail_w, avail_h);
                data->font->free_glyph_bitmap(data->font, glyph_bitmap);
            } else if (glyph_index != 0) {
                rend_sdl3_atlas_insert_empty(&data->atlas, font_data, glyph_index, color_key);
            }
        }
        blit_glyph(data->renderer, &data->atlas, entry, style,
                   cell_x, cell_y, entry ? entry->x_offset : 0, entry ? entry->y_offset : 0,
                   avail_w, avail_h, data->font_ascent);
    }

render_cursor:
    if (row == cursor_pos.row && col == cursor_pos.col) {
        SDL_FRect cursor_rect = {
            (float)(col * data->cell_width),
            (float)(row * data->cell_height),
            (float)data->cell_width,
            (float)data->cell_height
        };
        SDL_SetRenderDrawColor(data->renderer, r, g, b, 255);
        SDL_RenderRect(data->renderer, &cursor_rect);
    }

    return columns_to_consume;
}

static void render_damage_region(RendererSdl3Data *data, TerminalBackend *term,
                                 TerminalDamageRect damage)
{
    SDL_SetRenderTarget(data->renderer, data->terminal_texture);

    // Clear damaged region backgrounds
    for (int row = damage.start_row; row < damage.end_row; row++) {
        SDL_FRect row_rect = {
            (float)(damage.start_col * data->cell_width),
            (float)(row * data->cell_height),
            (float)((damage.end_col - damage.start_col) * data->cell_width),
            (float)data->cell_height
        };
        SDL_SetRenderDrawColor(data->renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(data->renderer, &row_rect);
    }

    TerminalPos cursor_pos = terminal_get_cursor_pos(term);

    for (int row = damage.start_row; row < damage.end_row; row++) {
        for (int col = damage.start_col; col < damage.end_col;) {
            col += render_cell(data, term, row, col, cursor_pos);
        }
    }

    SDL_SetRenderTarget(data->renderer, NULL);
}

static void sdl3_draw_terminal(RendererBackend *backend, TerminalBackend *term)
{
    if (!backend || !backend->backend_data || !term)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;

    if (!font_has_style(data->font, FONT_STYLE_NORMAL)) {
        vlog("Renderer draw terminal failed: invalid parameters\n");
        return;
    }

    rend_sdl3_atlas_begin_frame(&data->atlas);

    int term_rows, term_cols;
    terminal_get_dimensions(term, &term_rows, &term_cols);
    int display_rows = data->height / data->cell_height;
    int display_cols = data->width / data->cell_width;
    if (display_rows > term_rows)
        display_rows = term_rows;
    if (display_cols > term_cols)
        display_cols = term_cols;

    // Determine damage rect
    bool has_damage = true;
    TerminalDamageRect damage = { 0, 0, display_rows, display_cols };
    if (!data->full_redraw && data->terminal_texture) {
        TerminalDamageRect term_damage;
        if (terminal_get_damage_rect(term, &term_damage)) {
            damage = term_damage;
            if (damage.start_row < 0)
                damage.start_row = 0;
            if (damage.start_col < 0)
                damage.start_col = 0;
            if (damage.end_row > display_rows)
                damage.end_row = display_rows;
            if (damage.end_col > display_cols)
                damage.end_col = display_cols;
        } else {
            has_damage = false;
        }
    }
    data->full_redraw = false;

    if (!data->terminal_texture) {
        data->terminal_texture = SDL_CreateTexture(data->renderer,
                                                   SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                                   data->width, data->height);
        damage = (TerminalDamageRect){ 0, 0, display_rows, display_cols };
        has_damage = true;
    }

    if (has_damage) {
        vlog("Damage rect: rows [%d,%d) cols [%d,%d)\n",
             damage.start_row, damage.end_row, damage.start_col, damage.end_col);
        render_damage_region(data, term, damage);
    }

    // Blit terminal texture to screen
    SDL_SetRenderDrawColor(data->renderer, 0, 0, 0, 255);
    SDL_RenderClear(data->renderer);
    SDL_RenderTexture(data->renderer, data->terminal_texture, NULL, NULL);

    // Debug grid overlay
    if (data->debug_grid) {
        SDL_SetRenderDrawColor(data->renderer, 255, 255, 255, 200);
        SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_BLEND);
        for (int col = 0; col <= display_cols; col++) {
            SDL_FRect vline = { (float)(col * data->cell_width), 0.0f, 2.0f, (float)data->height };
            SDL_RenderFillRect(data->renderer, &vline);
        }
        for (int row = 0; row <= display_rows; row++) {
            SDL_FRect hline = { 0.0f, (float)(row * data->cell_height), (float)data->width, 2.0f };
            SDL_RenderFillRect(data->renderer, &hline);
        }
        SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_NONE);
    }
}

static void sdl3_present(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    SDL_RenderPresent(data->renderer);
}

static void sdl3_toggle_debug_grid(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    data->debug_grid = !data->debug_grid;
    vlog("Debug grid toggled to %s\n", data->debug_grid ? "ON" : "OFF");
    vlog("Current debug_grid value: %d\n", data->debug_grid);
}

static void sdl3_log_stats(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    rend_sdl3_atlas_log_stats(&data->atlas);
}

static void sdl3_resize(RendererBackend *backend, int width, int height)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    data->width = width;
    data->height = height;

    // Recreate persistent render target at new size
    if (data->terminal_texture)
        SDL_DestroyTexture(data->terminal_texture);
    data->terminal_texture = SDL_CreateTexture(data->renderer,
                                               SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
    data->full_redraw = true;
}

static bool sdl3_get_cell_size(RendererBackend *backend, int *cell_width, int *cell_height)
{
    if (!backend || !backend->backend_data)
        return false;
    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    if (data->cell_width <= 0 || data->cell_height <= 0)
        return false;
    *cell_width = data->cell_width;
    *cell_height = data->cell_height;
    return true;
}

// SDL3 renderer backend instance
RendererBackend renderer_backend_sdl3 = {
    .name = "sdl3",
    .backend_data = NULL,
    .init = sdl3_init,
    .destroy = sdl3_destroy,
    .load_fonts = sdl3_load_fonts,
    .draw_terminal = sdl3_draw_terminal,
    .present = sdl3_present,
    .resize = sdl3_resize,
    .toggle_debug_grid = sdl3_toggle_debug_grid,
    .log_stats = sdl3_log_stats,
    .get_cell_size = sdl3_get_cell_size
};
