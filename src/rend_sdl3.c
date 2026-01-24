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

    // Destroy glyph atlas
    rend_sdl3_atlas_destroy(&data->atlas);

    if (data->font) {
        font_destroy(data->font);
    }

    free(data);
    backend->backend_data = NULL;
}

static int sdl3_load_fonts(RendererBackend *backend, float font_size)
{
    if (!backend || !backend->backend_data)
        return -1;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;

    vlog("Loading fonts with size %.1f\n", font_size);

    // Initialize font resolver
    if (font_resolver_init() != 0) {
        fprintf(stderr, "Failed to initialize font resolver\n");
        return -1;
    }

    // Setup DPI options
    FontOptions options = { 0 };
    options.antialias = true;
    options.hinting = 1;
    options.hint_style = 1;
    options.subpixel_order = 0;
    options.lcd_filter = 0;
    options.ft_load_flags = 0;
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

    // Load normal monospace font
    FontResolutionResult result;
    if (font_resolver_find_font(FONT_TYPE_NORMAL, &result) == 0) {
        if (font_load_font(data->font, FONT_STYLE_NORMAL, result.font_path, font_size, &options)) {
            vlog("Normal font loaded successfully from %s\n", result.font_path);
        } else {
            fprintf(stderr, "Failed to load normal font from %s\n", result.font_path);
            font_resolver_free_result(&result);
            font_resolver_cleanup();
            return -1;
        }
        font_resolver_free_result(&result);
    } else {
        fprintf(stderr, "Failed to find normal font\n");
        font_resolver_cleanup();
        return -1;
    }

    // Load bold font
    if (font_resolver_find_font(FONT_TYPE_BOLD, &result) == 0) {
        if (font_load_font(data->font, FONT_STYLE_BOLD, result.font_path, font_size, &options)) {
            vlog("Bold font loaded successfully from %s\n", result.font_path);
        } else {
            vlog("Failed to load bold font from %s\n", result.font_path);
        }
        font_resolver_free_result(&result);
    }

    // Load emoji font
    if (font_resolver_find_font(FONT_TYPE_EMOJI, &result) == 0) {
        if (font_load_font(data->font, FONT_STYLE_EMOJI, result.font_path, font_size * EMOJI_FONT_SCALE, &options)) {
            vlog("Emoji font loaded successfully from %s\n", result.font_path);
        } else {
            vlog("Failed to load emoji font from %s\n", result.font_path);
        }
        font_resolver_free_result(&result);
    }

    // Cleanup font resolver
    font_resolver_cleanup();

    // Calculate cell dimensions from font metrics using normal font
    const FontMetrics *metrics = font_get_metrics(data->font, FONT_STYLE_NORMAL);
    if (metrics) {
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
    } else {
        vlog("ERROR: No font available for metrics calculation\n");
        return -1;
    }

    return 0;
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

    vlog("Drawing terminal with dimensions %dx%d\n", data->width, data->height);
    vlog("Cell dimensions: %dx%d, Font ascent: %d\n",
         data->cell_width, data->cell_height, data->font_ascent);

    // Clear screen
    SDL_SetRenderDrawColor(data->renderer, 0, 0, 0, 255);
    SDL_RenderClear(data->renderer);

    // Begin new atlas frame
    rend_sdl3_atlas_begin_frame(&data->atlas);

    // Calculate number of rows and columns that fit in the window
    int term_rows, term_cols;
    terminal_get_dimensions(term, &term_rows, &term_cols);

    int display_rows = data->height / data->cell_height;
    int display_cols = data->width / data->cell_width;

    // Limit to actual terminal size
    if (display_rows > term_rows)
        display_rows = term_rows;
    if (display_cols > term_cols)
        display_cols = term_cols;

    // Get cursor position for rendering
    TerminalPos cursor_pos = terminal_get_cursor_pos(term);

    // Render each cell
    for (int row = 0; row < display_rows; row++) {
        int last_combined_col = -1; // Track cells consumed by emoji combining
        int last_rendered_col = -1; // Track cells that were rendered (for skipping continuation cells)
        for (int col = 0; col < display_cols; col++) {
            // Skip cells that were already consumed by emoji combining logic
            if (col <= last_combined_col) {
                continue;
            }

            // Also skip cells that have already been rendered as part of a multi-cell emoji sequence
            if (col <= last_rendered_col) {
                continue;
            }

            TerminalCell cell;
            if (terminal_get_cell(term, row, col, &cell) < 0) {
                continue;
            }

            // Font selection is now handled in the rendering code below
            // Add logging for font attributes
            if (cell.attrs.bold || cell.attrs.italic) {
                vlog("Cell (%d,%d) attributes: bold=%d, italic=%d\n",
                     row, col, cell.attrs.bold, cell.attrs.italic);
            }

            // Colors are already resolved to RGB by terminal_get_cell
            Uint8 r = cell.fg.r, g = cell.fg.g, b = cell.fg.b;
            Uint8 bg_r = cell.bg.r, bg_g = cell.bg.g, bg_b = cell.bg.b;

            if (!cell.bg.is_default) {
                SDL_FRect bg_rect = {
                    col * data->cell_width,
                    row * data->cell_height,
                    data->cell_width,
                    data->cell_height
                };
                SDL_SetRenderDrawColor(data->renderer, bg_r, bg_g, bg_b, 255);
                SDL_RenderFillRect(data->renderer, &bg_rect);
            }

            // Skip empty cells
            if (cell.chars[0] == 0) {
                continue;
            }

            // Convert character to UTF-8 string for rendering
            char text[32] = { 0 };
            int pos = 0;
            for (int i = 0; i < TERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0 && pos < (int)(sizeof(text) - 3); i++) {
                uint32_t ch = cell.chars[i];
                if (ch < 0x80) {
                    text[pos++] = (char)ch;
                } else if (ch < 0x800) {
                    text[pos++] = (char)(0xC0 | (ch >> 6));
                    text[pos++] = (char)(0x80 | (ch & 0x3F));
                } else if (ch < 0x10000) {
                    text[pos++] = (char)(0xE0 | (ch >> 12));
                    text[pos++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    text[pos++] = (char)(0x80 | (ch & 0x3F));
                } else if (ch < 0x110000) {
                    text[pos++] = (char)(0xF0 | (ch >> 18));
                    text[pos++] = (char)(0x80 | ((ch >> 12) & 0x3F));
                    text[pos++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    text[pos++] = (char)(0x80 | (ch & 0x3F));
                }
            }

            // Render character if it's printable
            if (pos > 0) {
                // Check if we have an emoji that might need combining with subsequent cells
                uint32_t cps[TERM_MAX_CHARS_PER_CELL];
                int cp_count = 0;
                uint32_t combined_cps[TERM_MAX_CHARS_PER_CELL * 2]; // Allow for combining
                int combined_cp_count = 0;

                // Track how many columns this rendering will consume (for skip tracking)
                int columns_to_consume = cell.width; // Default: this cell's width

                // If the first character looks like an emoji, check for combining
                if (cell.chars[0] != 0 && (is_emoji_presentation(cell.chars[0]) ||
                                           is_regional_indicator(cell.chars[0]) ||
                                           is_zwj(cell.chars[0]) ||
                                           is_skin_tone_modifier(cell.chars[0]))) {
                    // Try to combine cells for emoji sequences
                    int columns_consumed = 0;
                    combined_cp_count = combine_cells_for_emoji(term, row, col, combined_cps,
                                                                sizeof(combined_cps) / sizeof(combined_cps[0]),
                                                                &columns_consumed);

                    // If we found multiple codepoints that can be combined, use them
                    if (combined_cp_count > 1) {
                        // Update the last_combined_col to skip the cells we consumed
                        // Use columns_consumed instead of combined_cp_count for correct skip tracking
                        last_combined_col = col + columns_consumed - 1;
                        columns_to_consume = columns_consumed;

                        // Use the combined codepoints
                        cp_count = combined_cp_count;
                        for (int i = 0; i < cp_count; i++) {
                            cps[i] = combined_cps[i];
                        }
                    } else {
                        // Fall back to regular single-cell handling (simple emoji)
                        for (int i = 0; i < TERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; i++) {
                            cps[cp_count++] = cell.chars[i];
                        }
                    }
                } else {
                    // Regular single-cell handling (non-emoji)
                    for (int i = 0; i < TERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; i++) {
                        cps[cp_count++] = cell.chars[i];
                    }
                }

                // Select appropriate font style based on first codepoint and attributes
                FontStyle style = FONT_STYLE_NORMAL;
                const char *font_type = "normal";
                if (cell.attrs.bold && font_has_style(data->font, FONT_STYLE_BOLD)) {
                    style = FONT_STYLE_BOLD;
                    font_type = "bold";
                }
                if (cp_count > 0) {
                    uint32_t first_cp = cps[0];
                    bool use_emoji = is_emoji_presentation(first_cp) || is_regional_indicator(first_cp);
                    // VS16 (U+FE0F) forces emoji presentation for any base character
                    if (!use_emoji) {
                        for (int i = 1; i < cp_count; i++) {
                            if (cps[i] == 0xFE0F) {
                                use_emoji = true;
                                break;
                            }
                        }
                    }
                    if (use_emoji && font_has_style(data->font, FONT_STYLE_EMOJI)) {
                        style = FONT_STYLE_EMOJI;
                        font_type = "emoji";
                    }
                }

                bool selected_has_colr = font_style_has_colr(data->font, style);
                vlog("  Selected font: %s (style=%d) has_colr=%d\n", font_type, style, selected_has_colr);

                // If multiple codepoints, try shaped rendering (backend must support it)
                if (cp_count > 1 && data->font && data->font->render_shaped) {
                    ShapedGlyphs *shaped = font_render_shaped_text(data->font, style, cps, cp_count, r, g, b);
                    if (shaped) {
                        int cell_x = col * data->cell_width;
                        int cell_y = row * data->cell_height;

                        for (int gi = 0; gi < shaped->num_glyphs; gi++) {
                            GlyphBitmap *gb = shaped->bitmaps[gi];
                            if (!gb)
                                continue;

                            void *font_data = data->font ? data->font->font_data[style] : NULL;
                            uint32_t color_key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                            RendSdl3AtlasEntry *entry = rend_sdl3_atlas_lookup(&data->atlas, font_data, gb->glyph_id, color_key);
                            if (!entry) {
                                GlyphBitmap *scaled = NULL;
                                if (style == FONT_STYLE_EMOJI)
                                    scaled = downscale_bitmap(gb, columns_to_consume * data->cell_width, data->cell_height);
                                entry = rend_sdl3_atlas_insert(&data->atlas, font_data, gb->glyph_id, color_key, scaled ? scaled : gb);
                                if (scaled) {
                                    free(scaled->pixels);
                                    free(scaled);
                                }
                            }

                            if (entry) {
                                SDL_FRect src = { (float)entry->region.x, (float)entry->region.y,
                                                  (float)entry->region.w, (float)entry->region.h };
                                SDL_FRect dst;
                                if (style == FONT_STYLE_EMOJI) {
                                    float avail_w = (float)(columns_to_consume * data->cell_width);
                                    float avail_h = (float)data->cell_height;
                                    float glyph_w = (float)entry->region.w;
                                    float glyph_h = (float)entry->region.h;
                                    float scale = fminf(avail_w / glyph_w, avail_h / glyph_h);
                                    float scaled_w = glyph_w * scale;
                                    float scaled_h = glyph_h * scale;
                                    dst = (SDL_FRect){
                                        (float)cell_x,
                                        (float)cell_y + (avail_h - scaled_h) * 0.5f,
                                        scaled_w, scaled_h
                                    };
                                } else {
                                    int x = cell_x + shaped->x_positions[gi] + entry->x_offset;
                                    int y = cell_y + data->font_ascent - entry->y_offset;
                                    dst = (SDL_FRect){ (float)x, (float)y, (float)entry->region.w, (float)entry->region.h };
                                }
                                SDL_RenderTexture(data->renderer, data->atlas.pages[entry->page_index].texture, &src, &dst);
                            }

                            data->font->free_glyph_bitmap(data->font, gb);
                        }

                        // Free shaped arrays
                        free(shaped->bitmaps);
                        free(shaped->x_positions);
                        free(shaped->y_positions);
                        free(shaped->x_advances);
                        free(shaped);

                        // Update skip tracking to prevent re-rendering continuation cells
                        last_rendered_col = col + columns_to_consume - 1;
                        continue; // done with this cell
                    }
                }

                // Fallback: render single glyph (first codepoint)
                uint32_t codepoint = cps[0];
                void *font_data_single = data->font ? data->font->font_data[style] : NULL;
                uint32_t color_key_single = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                GlyphBitmap *glyph_bitmap = font_render_glyphs(data->font, style, &codepoint, 1, r, g, b);
                if (glyph_bitmap) {
                    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_lookup(&data->atlas, font_data_single, glyph_bitmap->glyph_id, color_key_single);
                    if (!entry) {
                        GlyphBitmap *scaled = NULL;
                        if (style == FONT_STYLE_EMOJI)
                            scaled = downscale_bitmap(glyph_bitmap, columns_to_consume * data->cell_width, data->cell_height);
                        entry = rend_sdl3_atlas_insert(&data->atlas, font_data_single, glyph_bitmap->glyph_id, color_key_single, scaled ? scaled : glyph_bitmap);
                        if (scaled) {
                            free(scaled->pixels);
                            free(scaled);
                        }
                    }
                    if (entry) {
                        int cell_x = col * data->cell_width;
                        int cell_y = row * data->cell_height;
                        SDL_FRect src = { (float)entry->region.x, (float)entry->region.y,
                                          (float)entry->region.w, (float)entry->region.h };
                        SDL_FRect dest_rect;
                        if (style == FONT_STYLE_EMOJI) {
                            float avail_w = (float)(columns_to_consume * data->cell_width);
                            float avail_h = (float)data->cell_height;
                            float glyph_w = (float)entry->region.w;
                            float glyph_h = (float)entry->region.h;
                            float scale = fminf(avail_w / glyph_w, avail_h / glyph_h);
                            float scaled_w = glyph_w * scale;
                            float scaled_h = glyph_h * scale;
                            dest_rect = (SDL_FRect){
                                (float)cell_x,
                                (float)cell_y + (avail_h - scaled_h) * 0.5f,
                                scaled_w, scaled_h
                            };
                        } else {
                            float text_x = (float)cell_x + entry->x_offset;
                            float text_y = (float)cell_y + data->font_ascent - entry->y_offset;
                            dest_rect = (SDL_FRect){ text_x, text_y, (float)entry->region.w, (float)entry->region.h };
                        }
                        SDL_RenderTexture(data->renderer, data->atlas.pages[entry->page_index].texture, &src, &dest_rect);
                    }
                    data->font->free_glyph_bitmap(data->font, glyph_bitmap);
                }

                // Update skip tracking to prevent re-rendering continuation cells
                last_rendered_col = col + columns_to_consume - 1;
            }

            // Render cursor if this is the cursor position
            // Note: This is a simplified cursor implementation
            // In a full implementation, you'd want to get the actual cursor position from the terminal
            if (row == cursor_pos.row && col == cursor_pos.col) {
                SDL_FRect cursor_rect = {
                    col * data->cell_width,
                    row * data->cell_height,
                    data->cell_width,
                    data->cell_height
                };
                SDL_SetRenderDrawColor(data->renderer, r, g, b, 255); // Invert cursor color
                SDL_RenderRect(data->renderer, &cursor_rect);
            }
        }
    }

    // Render debug grid if enabled
    if (data->debug_grid) {
        vlog("DEBUG GRID: *** GRID RENDERING STARTED ***\n");
        vlog("DEBUG GRID: Rendering debug grid with display_cols=%d, display_rows=%d\n", display_cols, display_rows);
        vlog("DEBUG GRID: Renderer dimensions: %dx%d\n", data->width, data->height);
        vlog("DEBUG GRID: Cell dimensions: %dx%d\n", data->cell_width, data->cell_height);
        vlog("DEBUG GRID: debug_grid flag is %d\n", data->debug_grid);
        // Draw a more visible grid by using a brighter color and slightly thicker lines
        SDL_SetRenderDrawColor(data->renderer, 255, 255, 255, 200); // Slightly more opaque
        SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_BLEND);
        // Draw vertical lines
        for (int col = 0; col <= display_cols; col++) {
            SDL_FRect vline = { (float)(col * data->cell_width), 0.0f, 2.0f, (float)data->height };
            SDL_RenderFillRect(data->renderer, &vline);
        }
        // Draw horizontal lines
        for (int row = 0; row <= display_rows; row++) {
            SDL_FRect hline = { 0.0f, (float)(row * data->cell_height), (float)data->width, 2.0f };
            SDL_RenderFillRect(data->renderer, &hline);
        }
        SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_NONE);
        vlog("DEBUG GRID: *** GRID RENDERING COMPLETED ***\n");
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
    .log_stats = sdl3_log_stats
};
