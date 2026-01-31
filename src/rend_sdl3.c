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
#include <string.h>

#define EMOJI_FONT_SCALE     4.0f
#define FALLBACK_CACHE_SIZE  64
#define MAX_LOADED_FALLBACKS 8

typedef struct
{
    uint32_t codepoint;
    char *font_path; // NULL = no font found for this codepoint
} FallbackCacheEntry;

typedef struct
{
    char *font_path;
    void *font_data; // FtFontData*, kept alive for pointer stability
} LoadedFallbackFont;

// Cursor color: Charm signature purple with slight transparency (RGBA)
#define CURSOR_COLOR_R 0x7D
#define CURSOR_COLOR_G 0x56
#define CURSOR_COLOR_B 0xF4
#define CURSOR_COLOR_A 220

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

    vlog("Downscale: src=%dx%d max=%dx%d scale=%.3f dst=%dx%d\n",
         src->width, src->height, max_w, max_h, scale, dst_w, dst_h);

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

// Draw a filled rounded rectangle
static void draw_rounded_rect(SDL_Renderer *renderer, float x, float y,
                              float w, float h, float radius)
{
    if (radius <= 0) {
        SDL_FRect rect = { x, y, w, h };
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    // Clamp radius to half of smallest dimension
    if (radius > w / 2)
        radius = w / 2;
    if (radius > h / 2)
        radius = h / 2;

    // Draw center rectangle (full width, excluding corner rows)
    SDL_FRect center = { x, y + radius, w, h - 2 * radius };
    SDL_RenderFillRect(renderer, &center);

    // Draw top and bottom rectangles (excluding corners)
    SDL_FRect top = { x + radius, y, w - 2 * radius, radius };
    SDL_FRect bottom = { x + radius, y + h - radius, w - 2 * radius, radius };
    SDL_RenderFillRect(renderer, &top);
    SDL_RenderFillRect(renderer, &bottom);

    // Draw corner circles using filled points
    float r2 = radius * radius;
    for (int dy = 0; dy < (int)radius; dy++) {
        for (int dx = 0; dx < (int)radius; dx++) {
            float dist2 = (radius - dx - 0.5f) * (radius - dx - 0.5f) +
                          (radius - dy - 0.5f) * (radius - dy - 0.5f);
            if (dist2 <= r2) {
                // Top-left
                SDL_RenderPoint(renderer, x + dx, y + dy);
                // Top-right
                SDL_RenderPoint(renderer, x + w - 1 - dx, y + dy);
                // Bottom-left
                SDL_RenderPoint(renderer, x + dx, y + h - 1 - dy);
                // Bottom-right
                SDL_RenderPoint(renderer, x + w - 1 - dx, y + h - 1 - dy);
            }
        }
    }
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
    int scroll_offset;
    char *last_title;

    RendSdl3Atlas atlas;

    // Dynamic font fallback cache
    FallbackCacheEntry fallback_cache[FALLBACK_CACHE_SIZE];
    int fallback_cache_count;
    float font_size;          // saved for loading fallback at same size
    FontOptions font_options; // saved for loading fallback with same options

    // Loaded fallback font cache — keeps multiple fallback fonts alive
    // so their font_data pointers remain stable for atlas cache keys
    LoadedFallbackFont loaded_fallbacks[MAX_LOADED_FALLBACKS];
    int loaded_fallback_count;
} RendererSdl3Data;

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
    data->scroll_offset = 0;
    data->last_title = NULL;
    data->fallback_cache_count = 0;
    data->font_size = 0;
    memset(&data->font_options, 0, sizeof(data->font_options));
    memset(data->fallback_cache, 0, sizeof(data->fallback_cache));
    data->loaded_fallback_count = 0;
    memset(data->loaded_fallbacks, 0, sizeof(data->loaded_fallbacks));

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

    // Destroy all cached loaded fallback fonts
    for (int i = 0; i < data->loaded_fallback_count; i++) {
        if (data->loaded_fallbacks[i].font_data) {
            data->font->destroy_font(data->font, data->loaded_fallbacks[i].font_data);
        }
        free(data->loaded_fallbacks[i].font_path);
    }
    data->loaded_fallback_count = 0;

    // Clear the fallback slot pointer so font_destroy() doesn't double-free it
    // (the actual font_data was already destroyed above from the cache)
    data->font->font_data[FONT_STYLE_FALLBACK] = NULL;
    data->font->loaded_styles &= ~(1u << FONT_STYLE_FALLBACK);

    if (data->font) {
        font_destroy(data->font);
    }

    // Free fallback cache entries
    for (int i = 0; i < data->fallback_cache_count; i++) {
        free(data->fallback_cache[i].font_path);
    }

    // Cleanup font resolver (deferred from sdl3_load_fonts)
    font_resolver_cleanup();

    free(data->last_title);
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

    // Save font size and options for dynamic fallback loading later
    data->font_size = font_size;
    data->font_options = options;

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

    // NOTE: font_resolver_cleanup() is deferred to sdl3_destroy() so the
    // resolver remains available for runtime dynamic fallback queries.

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
    vlog("  Fallback font: (loaded on demand)\n");

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
    if (style == FONT_STYLE_EMOJI) {
        vlog("Cache emoji glyph %u: bitmap=%dx%d max=%dx%d\n",
             glyph_id, bitmap->width, bitmap->height, max_w, max_h);
        scaled = downscale_bitmap(bitmap, max_w, max_h);
    }
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
                       int avail_w, int avail_h, int font_ascent, bool is_regional)
{
    if (!entry || entry->region.w <= 0)
        return;

    SDL_FRect src = { (float)entry->region.x, (float)entry->region.y,
                      (float)entry->region.w, (float)entry->region.h };
    SDL_FRect dst;
    if (style == FONT_STYLE_EMOJI) {
        float glyph_w = (float)entry->region.w;
        float glyph_h = (float)entry->region.h;
        float scaled_w, scaled_h;

        if (is_regional) {
            // Regional indicators: scale uniformly to fit within a square,
            // preserving aspect ratio and centering within the cell
            float side = fminf((float)avail_w, (float)avail_h);
            float scale = fminf(side / glyph_w, side / glyph_h);
            scaled_w = fminf(glyph_w * scale, side);
            scaled_h = fminf(glyph_h * scale, side);
        } else {
            float scale = fminf((float)avail_w / glyph_w, (float)avail_h / glyph_h);
            scaled_w = glyph_w * scale;
            scaled_h = glyph_h * scale;
        }
        dst = (SDL_FRect){
            floorf((float)cell_x + ((float)avail_w - scaled_w) * 0.5f),
            floorf((float)cell_y + ((float)avail_h - scaled_h) * 0.5f),
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

// Helper to get a cell considering scroll offset
static int get_cell_with_scroll(RendererSdl3Data *data, TerminalBackend *term, int display_row,
                                int col, TerminalCell *cell)
{
    int scroll_offset = data->scroll_offset;
    int scrollback_row = scroll_offset - 1 - display_row;

    if (scrollback_row >= 0) {
        // Fetch from scrollback buffer
        return terminal_get_scrollback_cell(term, scrollback_row, col, cell);
    } else {
        // Fetch from visible terminal
        int terminal_row = display_row - scroll_offset;
        return terminal_get_cell(term, terminal_row, col, cell);
    }
}

// Look up or query fontconfig for a fallback font covering the given codepoint.
// Returns the cached font_path (may be NULL if no font was found).
static const char *fallback_cache_lookup(RendererSdl3Data *data, uint32_t codepoint)
{
    // Search existing cache
    for (int i = 0; i < data->fallback_cache_count; i++) {
        if (data->fallback_cache[i].codepoint == codepoint)
            return data->fallback_cache[i].font_path;
    }

    // Query fontconfig
    FontResolutionResult result;
    char *path = NULL;
    if (font_resolver_find_font_for_codepoint(codepoint, &result) == 0) {
        path = result.font_path;
        result.font_path = NULL; // take ownership
        font_resolver_free_result(&result);
    }

    // Store in cache (evict oldest if full)
    if (data->fallback_cache_count >= FALLBACK_CACHE_SIZE) {
        free(data->fallback_cache[0].font_path);
        memmove(&data->fallback_cache[0], &data->fallback_cache[1],
                (FALLBACK_CACHE_SIZE - 1) * sizeof(FallbackCacheEntry));
        data->fallback_cache_count = FALLBACK_CACHE_SIZE - 1;
    }
    data->fallback_cache[data->fallback_cache_count].codepoint = codepoint;
    data->fallback_cache[data->fallback_cache_count].font_path = path;
    data->fallback_cache_count++;

    return path;
}

// Ensure the FONT_STYLE_FALLBACK slot is loaded with the font at the given path.
// Uses a cache of loaded fallback fonts to keep pointers stable (important for
// atlas cache keys). Never destroys/reloads a font that's already cached.
static bool ensure_fallback_font(RendererSdl3Data *data, const char *font_path)
{
    if (!font_path)
        return false;

    // Search the loaded fallback cache for a match
    for (int i = 0; i < data->loaded_fallback_count; i++) {
        if (strcmp(data->loaded_fallbacks[i].font_path, font_path) == 0) {
            // Found — swap into the fallback slot without destroying anything
            data->font->font_data[FONT_STYLE_FALLBACK] = data->loaded_fallbacks[i].font_data;
            data->font->loaded_styles |= (1u << FONT_STYLE_FALLBACK);
            return true;
        }
    }

    // Not cached — need to load. Evict oldest entry if cache is full.
    if (data->loaded_fallback_count >= MAX_LOADED_FALLBACKS) {
        LoadedFallbackFont *victim = &data->loaded_fallbacks[0];
        vlog("Fallback cache full, evicting: %s\n", victim->font_path);
        // If the evicted font is currently in the fallback slot, clear it
        if (data->font->font_data[FONT_STYLE_FALLBACK] == victim->font_data) {
            data->font->font_data[FONT_STYLE_FALLBACK] = NULL;
            data->font->loaded_styles &= ~(1u << FONT_STYLE_FALLBACK);
        }
        data->font->destroy_font(data->font, victim->font_data);
        free(victim->font_path);
        memmove(&data->loaded_fallbacks[0], &data->loaded_fallbacks[1],
                (MAX_LOADED_FALLBACKS - 1) * sizeof(LoadedFallbackFont));
        data->loaded_fallback_count--;
    }

    // Load directly via init_font (bypass font_load_font which auto-destroys
    // whatever is in the slot — we manage the slot ourselves)
    void *new_font_data = data->font->init_font(data->font, font_path,
                                                data->font_size, FONT_STYLE_FALLBACK,
                                                &data->font_options);
    if (!new_font_data) {
        vlog("Failed to load fallback font: %s\n", font_path);
        return false;
    }

    // Get metrics for the new font
    if (!data->font->get_metrics(data->font, new_font_data,
                                 &data->font->metrics[FONT_STYLE_FALLBACK])) {
        data->font->destroy_font(data->font, new_font_data);
        vlog("Failed to get metrics for fallback font: %s\n", font_path);
        return false;
    }

    // Store in cache
    LoadedFallbackFont *entry = &data->loaded_fallbacks[data->loaded_fallback_count];
    entry->font_path = strdup(font_path);
    entry->font_data = new_font_data;
    data->loaded_fallback_count++;

    // Set the fallback slot
    data->font->font_data[FONT_STYLE_FALLBACK] = new_font_data;
    data->font->loaded_styles |= (1u << FONT_STYLE_FALLBACK);

    vlog("Fallback font loaded and cached (%d/%d): %s\n",
         data->loaded_fallback_count, MAX_LOADED_FALLBACKS, font_path);
    return true;
}

static int render_cell(RendererSdl3Data *data, TerminalBackend *term,
                       int row, int col, TerminalPos cursor_pos, bool show_cursor,
                       bool populate_only)
{
    TerminalCell cell;
    if (get_cell_with_scroll(data, term, row, col, &cell) < 0)
        return 1;

    // Draw background if non-default
    Uint8 r = cell.fg.r, g = cell.fg.g, b = cell.fg.b;
    if (!populate_only && !cell.bg.is_default) {
        SDL_FRect bg_rect = {
            (float)(col * data->cell_width),
            (float)(row * data->cell_height),
            (float)data->cell_width,
            (float)data->cell_height
        };
        SDL_SetRenderDrawColor(data->renderer, cell.bg.r, cell.bg.g, cell.bg.b, 255);
        SDL_RenderFillRect(data->renderer, &bg_rect);
    }

    // Default to consuming 1 column; updated for wide characters below
    int columns_to_consume = 1;

    if (cell.chars[0] == 0) {
        // Empty cell - jump to cursor drawing, then return
        goto render_cursor;
    }

    // Collect codepoints from cell
    uint32_t cps[TERM_MAX_CHARS_PER_CELL];
    int cp_count = 0;
    columns_to_consume = cell.width;

    for (int i = 0; i < TERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; i++)
        cps[cp_count++] = cell.chars[i];

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
    bool is_regional = (cp_count > 0 && is_regional_indicator(cps[0]));

    // For regional indicators, cache at square size for consistent high-quality scaling
    int cache_w = avail_w;
    int cache_h = avail_h;
    if (is_regional) {
        int side = avail_w < avail_h ? avail_w : avail_h;
        cache_w = cache_h = side;
    }

    // Shaped rendering path (multiple codepoints)
    if (cp_count > 1 && data->font->render_shaped) {
        ShapedGlyphs *shaped = font_render_shaped_text(data->font, style, cps, cp_count, r, g, b);

        // Fallback: if shaped rendering fails with selected style, try NORMAL
        if (!shaped && style != FONT_STYLE_NORMAL) {
            style = FONT_STYLE_NORMAL;
            font_data = data->font->font_data[style];
            shaped = font_render_shaped_text(data->font, style, cps, cp_count, r, g, b);
        }

        // Dynamic fallback: try fontconfig-resolved font for the first codepoint
        if (!shaped && cp_count > 0) {
            const char *fb_path = fallback_cache_lookup(data, cps[0]);
            if (fb_path && ensure_fallback_font(data, fb_path)) {
                style = FONT_STYLE_FALLBACK;
                font_data = data->font->font_data[style];
                shaped = font_render_shaped_text(data->font, style, cps, cp_count, r, g, b);
            }
        }

        if (shaped) {
            for (int gi = 0; gi < shaped->num_glyphs; gi++) {
                uint32_t gid = shaped->glyph_ids[gi];
                if (gid == 0)
                    continue;
                RendSdl3AtlasEntry *entry = rend_sdl3_atlas_lookup(&data->atlas, font_data, gid, color_key);
                if (!entry) {
                    GlyphBitmap *gb = font_render_glyph_id(data->font, style, gid, r, g, b);
                    if (gb) {
                        entry = cache_glyph(&data->atlas, font_data, gid, color_key,
                                            gb, style, cache_w, cache_h);
                        data->font->free_glyph_bitmap(data->font, gb);
                    } else {
                        rend_sdl3_atlas_insert_empty(&data->atlas, font_data, gid, color_key);
                    }
                }
                if (!populate_only) {
                    int x_off = shaped->x_positions[gi] + (entry ? entry->x_offset : 0);
                    int y_off = entry ? entry->y_offset : 0;
                    blit_glyph(data->renderer, &data->atlas, entry, style,
                               cell_x, cell_y, x_off, y_off, avail_w, avail_h, data->font_ascent,
                               is_regional);
                }
            }
            free(shaped->glyph_ids);
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

        // Fallback: if glyph not found in selected style, try NORMAL
        if (glyph_index == 0 && style != FONT_STYLE_NORMAL) {
            style = FONT_STYLE_NORMAL;
            font_data = data->font->font_data[style];
            glyph_index = font_get_glyph_index(data->font, style, codepoint);
        }

        // Dynamic fallback: if still missing, query fontconfig for a covering font
        if (glyph_index == 0) {
            const char *fb_path = fallback_cache_lookup(data, codepoint);
            if (fb_path && ensure_fallback_font(data, fb_path)) {
                style = FONT_STYLE_FALLBACK;
                font_data = data->font->font_data[style];
                glyph_index = font_get_glyph_index(data->font, style, codepoint);
            }
        }

        if (glyph_index != 0)
            entry = rend_sdl3_atlas_lookup(&data->atlas, font_data, glyph_index, color_key);
        if (!entry) {
            GlyphBitmap *glyph_bitmap = font_render_glyphs(data->font, style, &codepoint, 1, r, g, b);
            if (glyph_bitmap) {
                uint32_t insert_id = glyph_index ? glyph_index : (uint32_t)glyph_bitmap->glyph_id;
                entry = cache_glyph(&data->atlas, font_data, insert_id, color_key,
                                    glyph_bitmap, style, cache_w, cache_h);
                data->font->free_glyph_bitmap(data->font, glyph_bitmap);
            } else if (glyph_index != 0) {
                rend_sdl3_atlas_insert_empty(&data->atlas, font_data, glyph_index, color_key);
            }
        }
        if (!populate_only)
            blit_glyph(data->renderer, &data->atlas, entry, style,
                       cell_x, cell_y, entry ? entry->x_offset : 0, entry ? entry->y_offset : 0,
                       avail_w, avail_h, data->font_ascent, is_regional);
    }

render_cursor:
    if (!populate_only && show_cursor && row == cursor_pos.row && col == cursor_pos.col) {
        float cx = (float)(col * data->cell_width);
        float cy = (float)(row * data->cell_height);
        float cw = (float)data->cell_width;
        float ch = (float)data->cell_height;

        SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(data->renderer, CURSOR_COLOR_R, CURSOR_COLOR_G,
                               CURSOR_COLOR_B, CURSOR_COLOR_A);
        draw_rounded_rect(data->renderer, cx, cy, cw, ch, 2.0f);
        SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_NONE);
    }

    return columns_to_consume;
}

static void render_visible_cells(RendererSdl3Data *data, TerminalBackend *term,
                                 int display_rows, int display_cols,
                                 bool cursor_visible, bool populate_only)
{
    TerminalPos cursor_pos = terminal_get_cursor_pos(term);
    // Hide cursor when scrolled back, when terminal says it's not visible, or when cursor_visible is false
    bool show_cursor = cursor_visible && (data->scroll_offset == 0) && terminal_get_cursor_visible(term);

    for (int row = 0; row < display_rows; row++) {
        for (int col = 0; col < display_cols;) {
            col += render_cell(data, term, row, col, cursor_pos, show_cursor, populate_only);
        }
    }
}

static void sdl3_draw_terminal(RendererBackend *backend, TerminalBackend *term,
                               bool cursor_visible)
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

    // Two-phase render: populate atlas first (no draw calls), then flush
    // staging buffers to GPU while the render queue is empty, then draw.
    // This avoids the implicit render queue flush inside SDL_UpdateTexture
    // interfering with in-flight draw commands.

    // Phase 1: Populate atlas (insert missing glyphs, no draws)
    render_visible_cells(data, term, display_rows, display_cols, cursor_visible, true);

    // Phase 2: Flush staging buffers to GPU (render queue is empty)
    rend_sdl3_atlas_flush(&data->atlas);

    // Phase 3: Draw (all glyphs cached, texture data is current)
    SDL_SetRenderDrawColor(data->renderer, 0x00, 0x00, 0x00, 255);
    SDL_RenderClear(data->renderer);
    render_visible_cells(data, term, display_rows, display_cols, cursor_visible, false);

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

static void sdl3_scroll(RendererBackend *backend, TerminalBackend *term, int delta)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    int scrollback_lines = terminal_get_scrollback_lines(term);

    int new_offset = data->scroll_offset + delta;
    if (new_offset < 0)
        new_offset = 0;
    if (new_offset > scrollback_lines)
        new_offset = scrollback_lines;

    if (new_offset != data->scroll_offset) {
        data->scroll_offset = new_offset;
        vlog("Scroll offset changed to %d (max: %d)\n", data->scroll_offset, scrollback_lines);
    }
}

static void sdl3_reset_scroll(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    if (data->scroll_offset != 0) {
        data->scroll_offset = 0;
        vlog("Scroll offset reset to 0\n");
    }
}

static int sdl3_get_scroll_offset(RendererBackend *backend)
{
    if (!backend || !backend->backend_data)
        return 0;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;
    return data->scroll_offset;
}

static void sdl3_set_title(RendererBackend *backend, const char *title)
{
    if (!backend || !backend->backend_data)
        return;

    RendererSdl3Data *data = (RendererSdl3Data *)backend->backend_data;

    // Skip if title unchanged
    if (data->last_title && title && strcmp(data->last_title, title) == 0)
        return;
    if (!data->last_title && !title)
        return;

    // Update stored title
    free(data->last_title);
    data->last_title = title ? strdup(title) : NULL;

    // Update window title
    SDL_SetWindowTitle(data->window, title ? title : "bloom-terminal");
    vlog("Window title set to: %s\n", title ? title : "(default)");
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
    .get_cell_size = sdl3_get_cell_size,
    .scroll = sdl3_scroll,
    .reset_scroll = sdl3_reset_scroll,
    .get_scroll_offset = sdl3_get_scroll_offset,
    .set_title = sdl3_set_title
};
