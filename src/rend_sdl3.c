#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rend_sdl3.h"
#include "common.h"
#include "font.h"
#include "font_ft.h"
#include "font_resolve.h"
#include "term_cfr.h"
#include "term_colors.h"
#ifdef _WIN32
#include "font_resolve_w32.h"
#define FONT_RESOLVE_BACKEND font_resolve_backend_w32
#elif defined(__APPLE__)
#include "font_resolve_ct.h"
#define FONT_RESOLVE_BACKEND font_resolve_backend_ct
#else
#include "font_resolve_fc.h"
#define FONT_RESOLVE_BACKEND font_resolve_backend_fc
#endif
#include "png_writer.h"
#include "display_info.h"

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

// Box-filter downscale a glyph bitmap to fit within max_w x max_h.
// Returns a newly allocated GlyphBitmap, or NULL if no downscale is needed.
// When height_only_fit is true, only the height is constrained: a glyph that
// fits vertically passes through unchanged regardless of width (overhang is
// handled by the renderer's two-pass row draw). When the glyph overflows
// vertically, scale uniformly by the height ratio so aspect is preserved.
// (Implementation moved to rend_common.c as rend_downscale_bitmap)

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

// ---------------------------------------------------------------------------
// Underline drawing helpers (DPI-aware)
// ---------------------------------------------------------------------------

static void draw_underline_single(SDL_Renderer *renderer, int x, int y, int width,
                                  float pixel_density)
{
    int thickness = (int)roundf(1.0f * pixel_density);
    if (thickness < 1)
        thickness = 1;
    SDL_FRect rect = { (float)x, (float)y, (float)width, (float)thickness };
    SDL_RenderFillRect(renderer, &rect);
}

static void draw_underline_double(SDL_Renderer *renderer, int x, int y, int width,
                                  float pixel_density)
{
    int thickness = (int)roundf(1.0f * pixel_density);
    if (thickness < 1)
        thickness = 1;
    int gap = (int)roundf(1.0f * pixel_density);
    if (gap < 1)
        gap = 1;
    SDL_FRect top = { (float)x, (float)y, (float)width, (float)thickness };
    SDL_RenderFillRect(renderer, &top);
    SDL_FRect bot = { (float)x, (float)(y + thickness + gap), (float)width, (float)thickness };
    SDL_RenderFillRect(renderer, &bot);
}

static void draw_underline_curly(SDL_Renderer *renderer, int x, int y, int width,
                                 float pixel_density, Uint8 cr, Uint8 cg, Uint8 cb)
{
    float amplitude = 1.5f * pixel_density;
    if (amplitude < 1.0f)
        amplitude = 1.0f;
    float wavelength = 8.0f * pixel_density;
    if (wavelength < 4.0f)
        wavelength = 4.0f;
    float thickness = 0.5f * pixel_density;
    if (thickness < 0.5f)
        thickness = 0.5f;
    float center_y = (float)y + amplitude;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (int px = 0; px < width; px++) {
        float sine_y = center_y + amplitude * sinf((float)px / wavelength * 2.0f * (float)M_PI);
        int y_min = (int)floorf(sine_y - thickness);
        int y_max = (int)ceilf(sine_y + thickness);
        for (int iy = y_min; iy <= y_max; iy++) {
            float dist = fabsf((float)iy + 0.5f - sine_y);
            float alpha;
            if (dist <= thickness)
                alpha = 1.0f;
            else if (dist <= thickness + 1.0f)
                alpha = 1.0f - (dist - thickness);
            else
                continue;
            Uint8 a = (Uint8)(alpha * 255.0f);
            SDL_SetRenderDrawColor(renderer, cr, cg, cb, a);
            SDL_RenderPoint(renderer, (float)(x + px), (float)iy);
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

static void draw_underline_dotted(SDL_Renderer *renderer, int x, int y, int width,
                                  float pixel_density)
{
    float radius = 0.5f * pixel_density;
    if (radius < 0.5f)
        radius = 0.5f;
    float gap = roundf(2.0f * pixel_density);
    if (gap < 2.0f)
        gap = 2.0f;
    float stride = radius * 2.0f + gap;
    float cy = (float)y + radius;

    if (radius < 1.0f) {
        // Low DPI: single-pixel dots
        for (float cx = (float)x; cx < (float)(x + width); cx += stride)
            SDL_RenderPoint(renderer, cx, (float)y);
    } else {
        // HiDPI: filled circles via scanlines
        float r2 = radius * radius;
        for (float cx = (float)x + radius; cx < (float)(x + width); cx += stride) {
            for (float dy = -radius; dy <= radius; dy += 1.0f) {
                float half_w = sqrtf(r2 - dy * dy);
                SDL_FRect span = { cx - half_w, cy + dy, half_w * 2.0f, 1.0f };
                SDL_RenderFillRect(renderer, &span);
            }
        }
    }
}

static void draw_underline_dashed(SDL_Renderer *renderer, int x, int y, int width,
                                  float pixel_density)
{
    int thickness = (int)roundf(1.0f * pixel_density);
    if (thickness < 1)
        thickness = 1;
    int dash_w = (int)roundf(3.0f * pixel_density);
    if (dash_w < 1)
        dash_w = 1;
    int gap = (int)roundf(2.0f * pixel_density);
    if (gap < 1)
        gap = 1;
    int stride = dash_w + gap;
    for (int px = x; px < x + width; px += stride) {
        int w = dash_w;
        if (px + w > x + width)
            w = x + width - px;
        SDL_FRect rect = { (float)px, (float)y, (float)w, (float)thickness };
        SDL_RenderFillRect(renderer, &rect);
    }
}

// ---------------------------------------------------------------------------
// Strikethrough drawing helper (DPI-aware)
// ---------------------------------------------------------------------------

static void draw_strikethrough(SDL_Renderer *renderer, int x, int y, int width,
                               float pixel_density)
{
    int thickness = (int)roundf(1.0f * pixel_density);
    if (thickness < 1)
        thickness = 1;
    SDL_FRect rect = { (float)x, (float)y, (float)width, (float)thickness };
    SDL_RenderFillRect(renderer, &rect);
}

// Forward declaration for sixel cache cleanup (used in rend_sdl3_destroy)
static void sixel_cache_clear(RendererSdl3Data *data);
static void lottie_cache_clear(RendererSdl3Data *data);

// Linearize RGBA data for upload to the linear render target.
// On the linear-light path (linear_ok), SDL samples textures as-is without
// sRGB decode, so we must pre-linearize sixel/lottie RGBA to match how
// color glyphs are linearized in the atlas. Returns a heap-allocated copy
// that the caller must free, or NULL if linearization isn't needed.
static uint8_t *linearize_for_upload(RendererSdl3Data *data, const uint8_t *src,
                                     int w, int h)
{
    if (!data->linear_ok)
        return NULL;
    size_t n = (size_t)w * h * 4;
    uint8_t *copy = malloc(n);
    if (!copy)
        return NULL;
    memcpy(copy, src, n);
    rend_linearize_rgba_in_place(copy, w, h);
    return copy;
}

// Forward declaration for panel terminal building
static void build_panel_terminal(RendererSdl3Data *data, int slot);

// Free panel terminal and texture for a specific slot
static void panel_free_slot(RendererSdl3Data *data, int slot)
{
    if (slot < 0 || slot >= PORTTY_PANEL_MAX)
        return;
    if (data->panel_textures[slot]) {
        SDL_DestroyTexture(data->panel_textures[slot]);
        data->panel_textures[slot] = NULL;
    }
    if (data->panel_terms[slot]) {
        terminal_destroy(data->panel_terms[slot]);
        free(data->panel_terms[slot]);
        data->panel_terms[slot] = NULL;
    }
}

// Free all panel terminals and textures
static void panels_free_all(RendererSdl3Data *data)
{
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        panel_free_slot(data, i);
    }
}

// Capture the GPU model + driver from SDL's GPU renderer device for the
// diagnostics report. SDL GPU owns the device here.
static void capture_sdl_gpu_info(RendererSdl3Data *data)
{
    data->gpu_name[0] = '\0';
    data->gpu_driver[0] = '\0';
    data->gpu_driver_libre = GPU_DRIVER_LIBRE_NO;
    SDL_PropertiesID rp = SDL_GetRendererProperties(data->renderer);
    if (!rp)
        return;
    SDL_GPUDevice *gpu = SDL_GetPointerProperty(rp, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, NULL);
    if (!gpu)
        return; // not a GPU render driver
    SDL_PropertiesID dp = SDL_GetGPUDeviceProperties(gpu);
    if (!dp)
        return;

    const char *name = SDL_GetStringProperty(dp, SDL_PROP_GPU_DEVICE_NAME_STRING, NULL);
    const char *dname = SDL_GetStringProperty(dp, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, NULL);
    const char *dinfo = SDL_GetStringProperty(dp, SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING, NULL);
    const char *dver = SDL_GetStringProperty(dp, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, NULL);

    if (name && *name)
        snprintf(data->gpu_name, sizeof(data->gpu_name), "%s", name);

    data->gpu_driver_libre = rend_classify_gpu_driver_libre(dname, dinfo)
                                 ? GPU_DRIVER_LIBRE_YES
                                 : GPU_DRIVER_LIBRE_NO;
    rend_format_gpu_driver(data->gpu_driver, sizeof(data->gpu_driver),
                           dname, dinfo, dver);
}

bool rend_sdl3_init(RendererSdl3Data *data, SDL_Window *window_handle, SDL_Renderer *renderer_handle)
{
    data->window = window_handle;
    data->renderer = renderer_handle;

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
    memset(&data->scroll, 0, sizeof(data->scroll));
    data->hint_name = NULL;
    data->gpu_name[0] = '\0';
    data->gpu_driver[0] = '\0';
    data->gpu_driver_libre = GPU_DRIVER_LIBRE_NO;
    data->resolve = NULL;
    rend_fallback_init(&data->fallback);
    data->font_size = 0;
    memset(&data->font_options, 0, sizeof(data->font_options));
    data->content_scale = 1.0f;
    data->font_path = NULL;
    memset(&data->panels, 0, sizeof(data->panels));
    memset(data->panel_terms, 0, sizeof(data->panel_terms));
    memset(data->panel_textures, 0, sizeof(data->panel_textures));
    data->linear_target = NULL;
    data->linear_w = 0;
    data->linear_h = 0;
    // Linear-light compositing requires SDL's Vulkan-based renderers ("gpu" or
    // "vulkan"), which honor the SRGB_LINEAR colorspace on float render targets
    // (linearize -> blend -> re-encode). The OpenGL/software renderers blend in
    // sRGB regardless, so there we skip the linear pass and draw straight
    // (legacy behavior).
    const char *rname = SDL_GetRendererName(data->renderer);
    data->linear_ok = (rname && (strcmp(rname, "gpu") == 0 || strcmp(rname, "vulkan") == 0));
    data->linear_selfcheck_done = false;
    vlog("Renderer '%s': linear-light compositing %s\n", rname ? rname : "(null)",
         data->linear_ok ? "enabled" : "disabled (sRGB blending)");

    // GPU model + driver for the diagnostics report (portable; "gpu" driver only).
    capture_sdl_gpu_info(data);
    if (data->gpu_name[0])
        vlog("GPU '%s' driver '%s'%s\n", data->gpu_name, data->gpu_driver,
             data->gpu_driver_libre == GPU_DRIVER_LIBRE_YES ? " (open source)" : "");

    // GPU glyph-coverage shader. Only attempt on the linear-light backends
    // (gpu/vulkan) — the shader rides on the same float target. rend_shader_create
    // returns NULL for a neutral curve, a missing GPU device, or no SPIR-V, in
    // which case we keep the atlas-baked curve. font_ft_set_shader_curve_active
    // MUST run before the first glyph is rasterized (which happens on the first
    // draw, after this init), so rasterization bakes raw coverage iff the shader
    // is active — avoiding a double-applied curve.
    data->glyph_shader = NULL;
    if (data->linear_ok)
        data->glyph_shader =
            rend_shader_create(data->renderer, portty_text_gamma, portty_text_contrast);
    font_ft_set_shader_curve_active(data->glyph_shader != NULL);
    vlog("Glyph curve: %s\n",
         data->glyph_shader ? "GPU shader (luminance-scaled)" : "baked LUT (uniform)");

    // Initialize glyph atlas
    if (!rend_sdl3_atlas_init(&data->atlas, data->renderer)) {
        vlog("Failed to initialize glyph atlas\n");
        return false;
    }
    // On the linear-light path, color-glyph texels are sRGB->linear decoded on
    // insert so the blit-out re-encode round-trips them (SDL never decodes
    // sampled texels). See rend_sdl3_atlas.h.
    data->atlas.linearize_color = data->linear_ok;

    // Initialize font backend with FreeType backend
    data->font = &font_backend_ft;
    if (!font_init(data->font)) {
        vlog("Failed to initialize font backend\n");
        rend_sdl3_atlas_destroy(&data->atlas);
        return false;
    }

    return true;
}

void rend_sdl3_destroy(RendererSdl3Data *data)
{
    if (!data)
        return;

    // Destroy sixel texture cache
    sixel_cache_clear(data);

    // Destroy lottie texture cache
    lottie_cache_clear(data);

    // Destroy linear-light render target
    if (data->linear_target) {
        SDL_DestroyTexture(data->linear_target);
        data->linear_target = NULL;
    }

    // Destroy GPU glyph-coverage shader (no-op if it was never created)
    rend_shader_destroy(data->glyph_shader);
    data->glyph_shader = NULL;

    // Destroy panel textures
    panels_free_all(data);

    free(data->font_path);
    data->font_path = NULL;

    // Destroy glyph atlas
    rend_sdl3_atlas_destroy(&data->atlas);

    // Destroy fallback fonts and cache
    rend_fallback_destroy(&data->fallback, data->font);

    if (data->font) {
        font_destroy(data->font);
    }

    // Cleanup font resolver (deferred from rend_sdl3_load_fonts)
    if (data->resolve) {
        font_resolve_destroy(data->resolve);
        data->resolve = NULL;
    }
}

int rend_sdl3_load_fonts(RendererSdl3Data *data, float font_size, const char *font_name, int ft_hint_target)
{
    if (!data)
        return -1;

    data->resolve = font_resolve_init(&FONT_RESOLVE_BACKEND);
    if (!data->resolve) {
        fprintf(stderr, "Failed to initialize font resolver\n");
        return -1;
    }

    // Determine content scale from SDL window if available
    float content_scale = data->content_scale;
    if (data->window && content_scale <= 1.0f) {
        float sdl_scale = SDL_GetWindowDisplayScale(data->window);
        if (sdl_scale > 1.0f)
            content_scale = sdl_scale;
    }
    data->content_scale = content_scale;

    const char *hint_name = "none";
    if (ft_hint_target == FT_LOAD_TARGET_LIGHT)
        hint_name = "light";
    else if (ft_hint_target == FT_LOAD_TARGET_NORMAL)
        hint_name = "normal";
    else if (ft_hint_target == FT_LOAD_TARGET_MONO)
        hint_name = "mono";

    RendFontLoadResult r = { 0 };
    if (rend_load_fonts(&r, data->font, data->resolve, font_size, font_name,
                        ft_hint_target, content_scale, hint_name) != 0) {
        font_resolve_destroy(data->resolve);
        data->resolve = NULL;
        return -1;
    }

    data->hint_name = r.hint_name;
    data->font_ascent = r.font_ascent;
    data->font_descent = r.font_descent;
    data->font_cap_height = r.font_cap_height;
    data->char_width = r.char_width;
    data->char_height = r.char_height;
    data->cell_width = r.cell_width;
    data->cell_height = r.cell_height;
    data->font_size = r.font_size;
    data->font_options = r.font_options;
    free(data->font_path);
    data->font_path = r.font_path;
    r.font_path = NULL;

    // Cell metrics changed — panel textures will rebuild on next draw pass
    panel_mgr_set_cell_size(&data->panels, data->cell_width, data->cell_height);

    return 0;
}

static RendSdl3AtlasEntry *cache_glyph(RendSdl3Atlas *atlas, void *font_data,
                                       uint32_t glyph_id, uint32_t color_key,
                                       GlyphBitmap *bitmap, bool downscale,
                                       int max_w, int max_h, bool height_only_fit,
                                       bool is_color)
{
    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_lookup(atlas, font_data, glyph_id, color_key);
    if (entry)
        return entry;

    GlyphBitmap *scaled = NULL;
    if (downscale) {
        vlog("Cache glyph %u: bitmap=%dx%d max=%dx%d%s\n",
             glyph_id, bitmap->width, bitmap->height, max_w, max_h,
             height_only_fit ? " (height-only)" : "");
        scaled = rend_downscale_bitmap(bitmap, max_w, max_h, height_only_fit);
        // Color emoji are placed by cell-center, not baseline. Symbol-class
        // glyphs from a text font (height_only_fit) use the baseline branch
        // of blit_glyph but with x_offset overridden: FreeType's bitmap_left
        // is calibrated against the font's natural advance, which for many
        // mono fonts is wider than our 1-cell allocation (Noto Sans Mono ✶
        // U+2736: advance 1200/1000 em). Honoring it directly drops the ink
        // into the right half of the cell with the rest overhanging into
        // the next cell. Centering the bitmap horizontally restores a
        // symmetric placement while bitmap_top still anchors the glyph to
        // the typographic baseline.
        bool centered = !height_only_fit;
        bitmap->centered = centered;
        if (scaled)
            scaled->centered = centered;
        if (height_only_fit) {
            int eff_w = scaled ? scaled->width : bitmap->width;
            int x_off = (max_w - eff_w) / 2;
            bitmap->x_offset = x_off;
            if (scaled)
                scaled->x_offset = x_off;
        }
    }
    entry = rend_sdl3_atlas_insert(atlas, font_data, glyph_id, color_key,
                                   scaled ? scaled : bitmap, is_color);
    if (scaled) {
        free(scaled->pixels);
        free(scaled);
    }
    return entry;
}

// (rend_is_color_font moved to rend_common.c as rend_is_color_font)

// (rend_is_symbol_cell_cp moved to rend_common.c as rend_is_symbol_cell_cp)

static void blit_glyph(SDL_Renderer *renderer, RendSdl3Atlas *atlas,
                       RendSdl3AtlasEntry *entry,
                       int cell_x, int cell_y, int glyph_x_offset, int glyph_y_offset,
                       int avail_w, int avail_h, int font_ascent,
                       bool color_baked, uint8_t mod_r, uint8_t mod_g, uint8_t mod_b,
                       RendShaderState *shader, float bg_luma)
{
    if (!entry || entry->region.w <= 0)
        return;

    SDL_FRect src = { (float)entry->region.x, (float)entry->region.y,
                      (float)entry->region.w, (float)entry->region.h };
    SDL_FRect dst;
    if (entry->centered) {
        // Cell-center placement. The atlas bitmap is already at its final
        // display size: cache_glyph runs the box-filter when the rasterized
        // glyph overflows the cell, and small glyphs stay on their native
        // pixel grid. Blit is always 1:1 — no GPU scaling.
        //
        // For padded bitmaps (diagonals with region > cell), use integer
        // division for padding calculation to match Sokol's exact placement
        // and ensure seamless tiling. Normal centered glyphs use floorf.
        int pad_x = (entry->region.w - avail_w) / 2;
        int pad_y = (entry->region.h - avail_h) / 2;
        if (pad_x > 0 || pad_y > 0) {
            // Padded bitmap - extend beyond cell bounds for seamless overhang
            dst = (SDL_FRect){
                (float)(cell_x - pad_x),
                (float)(cell_y - pad_y),
                (float)entry->region.w, (float)entry->region.h
            };
        } else {
            // Normal centered glyph - center within cell
            dst = (SDL_FRect){
                floorf((float)cell_x + ((float)avail_w - (float)entry->region.w) * 0.5f),
                floorf((float)cell_y + ((float)avail_h - (float)entry->region.h) * 0.5f),
                (float)entry->region.w, (float)entry->region.h
            };
        }
    } else {
        // Trust FreeType's bitmap bounds: anchor at cell_x + bitmap_left and
        // let the glyph overhang the cell. Row draw is two-pass — all cell
        // backgrounds in pass 1 before any glyph in pass 2 — so a small
        // overhang lands on top of an already-painted neighbor background.
        dst = (SDL_FRect){
            (float)cell_x + glyph_x_offset,
            (float)cell_y + font_ascent - glyph_y_offset,
            (float)entry->region.w, (float)entry->region.h
        };
    }
    if (!color_baked)
        SDL_SetTextureColorMod(atlas->texture, mod_r, mod_g, mod_b);

    // Non-color glyphs carry coverage in alpha and get the luminance-scaled
    // coverage curve from the GPU shader (when active). Color glyphs blend with
    // SDL's default shader. The shader is bound only around this one draw so
    // cursor/selection fills keep the default pipeline.
    bool use_shader = shader && !color_baked;
    if (use_shader) {
        rend_shader_set_bg_luma(shader, bg_luma);
        rend_shader_bind(shader);
    }
    SDL_RenderTexture(renderer, atlas->texture, &src, &dst);
    if (use_shader)
        rend_shader_unbind(shader);

    if (!color_baked)
        SDL_SetTextureColorMod(atlas->texture, 255, 255, 255);
}

// Look up or query fontconfig for a fallback font covering the given codepoint.
// Returns the cached font_path (may be NULL if no font was found).

// Draw cell background only. Cell must be pre-fetched by the caller.
// row:     display row (for draw position).
// vis_col: visual column for draw position (may exceed vt_col on rows
//          with VS16-widened emoji).
// pres_w:  presentation width from the iterator (for bg rect width).
static void render_cell_bg(RendererSdl3Data *data, int row, int vis_col,
                           int pres_w, const TerminalCell *cell)
{
    if (cell->bg.is_default)
        return;
    if (pres_w <= 0)
        pres_w = 1;
    SDL_FRect bg_rect = {
        (float)(vis_col * data->cell_width),
        (float)(row * data->cell_height),
        (float)(pres_w * data->cell_width),
        (float)data->cell_height
    };
    SDL_SetRenderDrawColor(data->renderer, cell->bg.r, cell->bg.g, cell->bg.b, 255);
    SDL_RenderFillRect(data->renderer, &bg_rect);
}

// Render a single cell (glyphs only; cursor is drawn separately).
// Cell must be pre-fetched by the caller (typically via TerminalRowIter).
// row:     display row (for draw position).
// vt_col:  libvterm column (for selection check).
// vis_col: visual column for draw position (may differ from vt_col on rows
//          with VS16-widened emoji).
// pres_w:  presentation width in cells (from iterator).
static void render_cell(RendererSdl3Data *data, TerminalBackend *term,
                        int row, int vt_col, int vis_col, int pres_w,
                        const TerminalCell *cell_in, bool populate_only)
{
    TerminalCell cell = *cell_in;
    Uint8 r = cell.fg.r, g = cell.fg.g, b = cell.fg.b;

    // Dim/faint (SGR 2): blend foreground toward background at 40% opacity,
    // matching kitty's dim_opacity default. Works for both color-baked
    // (emoji) and non-color-baked (text) glyph paths since the blended
    // color flows into both the atlas color_key and SDL_SetTextureColorMod.
    if (cell.attrs.dim) {
        uint8_t bg_r = cell.bg.is_default ? 0 : cell.bg.r;
        uint8_t bg_g = cell.bg.is_default ? 0 : cell.bg.g;
        uint8_t bg_b = cell.bg.is_default ? 0 : cell.bg.b;
        r = (Uint8)(r * 0.4f + bg_r * 0.6f);
        g = (Uint8)(g * 0.4f + bg_g * 0.6f);
        b = (Uint8)(b * 0.4f + bg_b * 0.6f);
    }

    // Background perceptual luma (0..1) for the glyph-coverage shader's fg/bg
    // direction. Default-bg cells render over the black-cleared linear target,
    // so treat them as 0 — that keeps normal (light-on-dark) text neutral while
    // reverse video (explicit light bg) thickens. Rec.709 weights match the
    // shader's fg_luma.
    float bg_luma = cell.bg.is_default
                        ? 0.0f
                        : (0.2126f * cell.bg.r + 0.7152f * cell.bg.g +
                           0.0722f * cell.bg.b) /
                              255.0f;

    int columns_to_consume = pres_w > 0 ? pres_w : 1;

    if (cell.cp == 0) {
        // Empty cell - nothing to render
        return;
    }

    // Collect the cell's codepoint sequence. Single-codepoint cells —
    // ~99% of the grid — take the fast path with zero backend calls.
    // Multi-codepoint clusters (emoji ZWJ chains, flags, long combining
    // runs) come through `terminal_cell_get_grapheme` which routes to
    // bvt's grapheme arena and returns the full sequence regardless of
    // length. The 32-element local cap matches CFR_CLUSTER_MAX*2 — well
    // above any cluster the parser will commit.
    uint32_t cps[32];
    int cp_count;
    if (cell.grapheme_id == 0) {
        cps[0] = cell.cp;
        cp_count = 1;
    } else {
        int scroll_offset = data->scroll.scroll_offset;
        int scrollback_row = scroll_offset - 1 - row;
        int unified_row = (scrollback_row >= 0)
                              ? -(scrollback_row + 1)
                              : (row - scroll_offset);
        size_t n = terminal_cell_get_grapheme(term, unified_row, vt_col,
                                              cps, sizeof(cps) / sizeof(cps[0]));
        if (n == 0) {
            cps[0] = cell.cp;
            n = 1;
        }
        cp_count = (int)n;
    }

    // NERD FONTS HACK: Translate obsolete v2 codepoints to v3 equivalents
    for (int i = 0; i < cp_count; i++)
        cps[i] = rend_nf_translate_codepoint(cps[i]);

    // Procedural box drawing / block elements — render to pixel buffer
    // and cache in the atlas like a regular font glyph.
    if (cp_count == 1 && rend_boxdraw_is_supported(cps[0])) {
        uint32_t bd_cp = cps[0];
        int cell_x = vis_col * data->cell_width;
        int cell_y = row * data->cell_height;
        int avail_w = columns_to_consume * data->cell_width;
        int avail_h = data->cell_height;

        // Atlas key: sentinel font_data + codepoint as glyph_id.
        // Color is baked into the bitmap (like color emoji), so use
        // the fg color as the color_key to get per-color caching.
        uint32_t color_key = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        RendSdl3AtlasEntry *entry =
            rend_sdl3_atlas_lookup(&data->atlas, BOXDRAW_FONT_DATA,
                                   (int)bd_cp, color_key);

        if (!entry) {
            GlyphBitmap *bmp = rend_boxdraw_render(bd_cp,
                                                   data->cell_width,
                                                   data->cell_height,
                                                   r, g, b);
            if (bmp) {
                entry = rend_sdl3_atlas_insert(&data->atlas, BOXDRAW_FONT_DATA,
                                               (int)bd_cp, color_key, bmp, false);
                free(bmp->pixels);
                free(bmp);
            }
        }

        if (!populate_only && entry) {
            // Box-drawing bitmaps carry coverage in alpha (like text glyphs),
            // not baked color. Use the glyph-coverage shader for proper blending.
            blit_glyph(data->renderer, &data->atlas, entry,
                       cell_x, cell_y, 0, 0,
                       avail_w, avail_h, data->font_ascent,
                       false, r, g, b, data->glyph_shader, bg_luma);
        }
        return;
    }

    // Select font style
    FontStyle style = FONT_STYLE_NORMAL;
    if (cell.attrs.bold && cell.attrs.italic) {
        if (font_has_style(data->font, FONT_STYLE_BOLD_ITALIC))
            style = FONT_STYLE_BOLD_ITALIC;
        else if (font_has_style(data->font, FONT_STYLE_BOLD))
            style = FONT_STYLE_BOLD;
        else if (font_has_style(data->font, FONT_STYLE_ITALIC))
            style = FONT_STYLE_ITALIC;
    } else if (cell.attrs.bold) {
        if (font_has_style(data->font, FONT_STYLE_BOLD))
            style = FONT_STYLE_BOLD;
    } else if (cell.attrs.italic) {
        if (font_has_style(data->font, FONT_STYLE_ITALIC))
            style = FONT_STYLE_ITALIC;
    }
    if (cp_count > 0) {
        bool emoji_available = font_has_style(data->font, FONT_STYLE_EMOJI);
        bool emoji_has_glyph = emoji_available &&
                               font_get_glyph_index(data->font, FONT_STYLE_EMOJI, cps[0]) != 0;
        if (rend_should_use_emoji(cps, cp_count, emoji_available, emoji_has_glyph))
            style = FONT_STYLE_EMOJI;
    }

    bool emoji_render = (style == FONT_STYLE_EMOJI);

    // Width is authoritative from cell.width (the term backend enforces the
    // "VS16 → 2 cells" rule in convert_vterm_screen_cell). No render-time
    // width override needed here. See README.md "Emoji Width Paradigm".

    if (cps[0] > 0x7F)
        vlog("render_cell: U+%04X style=%d emoji=%d cols=%d cell.w=%d\n",
             cps[0], style, emoji_render, columns_to_consume, cell.width);

    int cell_x = vis_col * data->cell_width;
    int cell_y = row * data->cell_height;
    int avail_w = columns_to_consume * data->cell_width;
    int avail_h = data->cell_height;

    // Tell the font backend the pixel budget for this glyph so oversized
    // glyphs (e.g. double-advance symbols, CJK via fallback) get scaled.
    for (int s = 0; s < FONT_STYLE_COUNT; s++)
        font_set_presentation_width(data->font, s, avail_w);

    // Emoji: prefer square aspect ratio (avail_h) but never exceed
    // the allocated cell space (columns_to_consume * cell_width).
    if (style == FONT_STYLE_EMOJI && avail_h < avail_w) {
        avail_w = avail_h;
    }

    void *font_data = data->font->font_data[style];
    bool color_baked = rend_is_color_font(data->font, style);
    uint8_t render_r = color_baked ? r : 255;
    uint8_t render_g = color_baked ? g : 255;
    uint8_t render_b = color_baked ? b : 255;
    uint32_t color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                     : 0xFFFFFF;
    bool is_regional = (cp_count > 0 && is_regional_indicator(cps[0]));
    bool symbol_cell = (cp_count > 0) && rend_is_symbol_cell_cp(cps[0]);
    bool downscale_glyph = (emoji_render && color_baked) || symbol_cell;
    // Symbol-class glyphs rendered through a text/fallback font keep their
    // natural design width — only height drives the (rare) downscale, and
    // they are placed on the typographic baseline instead of cell-center.
    // Color-baked emoji-font output keeps the existing centered min-fit
    // policy because emoji fonts are designed for a square cell.
    bool height_only_fit = symbol_cell && !color_baked;

    // For regional indicators, cache at square size for consistent high-quality scaling
    int cache_w = avail_w;
    int cache_h = avail_h;
    if (is_regional) {
        int side = avail_w < avail_h ? avail_w : avail_h;
        cache_w = cache_h = side;
    }

    // Shaped rendering path (multiple codepoints)
    if (cp_count > 1 && data->font->render_shaped) {
        ShapedGlyphs *shaped = font_render_shaped_text(data->font, style, cps, cp_count,
                                                       render_r, render_g, render_b);

        // If shaped returned all .notdef glyphs, treat as failure so the
        // single-glyph fallback (with fontconfig) gets a chance to run.
        if (shaped) {
            bool all_notdef = true;
            for (int i = 0; i < shaped->num_glyphs; i++) {
                if (shaped->glyph_ids[i] != 0) {
                    all_notdef = false;
                    break;
                }
            }
            if (all_notdef) {
                free(shaped->glyph_ids);
                free(shaped->x_positions);
                free(shaped->y_positions);
                free(shaped->x_advances);
                free(shaped);
                shaped = NULL;
            }
        }

        // Fallback: if shaped rendering fails with selected style, try NORMAL
        if (!shaped && style != FONT_STYLE_NORMAL) {
            style = FONT_STYLE_NORMAL;
            font_data = data->font->font_data[style];
            color_baked = rend_is_color_font(data->font, style);
            render_r = color_baked ? r : 255;
            render_g = color_baked ? g : 255;
            render_b = color_baked ? b : 255;
            color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                    : 0xFFFFFF;
            shaped = font_render_shaped_text(data->font, style, cps, cp_count,
                                             render_r, render_g, render_b);
        }

        // Dynamic fallback: try fontconfig-resolved font for the first codepoint
        if (!shaped && cp_count > 0) {
            const char *fb_path = rend_fallback_lookup(&data->fallback, data->resolve, cps[0]);
            if (fb_path && rend_fallback_ensure(&data->fallback, data->font, fb_path, data->font_size, &data->font_options, data->cell_width)) {
                style = FONT_STYLE_FALLBACK;
                font_data = data->font->font_data[style];
                // Lazily loaded — set presentation_width now
                font_set_presentation_width(data->font, style, avail_w);
                color_baked = rend_is_color_font(data->font, style);
                render_r = color_baked ? r : 255;
                render_g = color_baked ? g : 255;
                render_b = color_baked ? b : 255;
                color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                        : 0xFFFFFF;
                shaped = font_render_shaped_text(data->font, style, cps, cp_count,
                                                 render_r, render_g, render_b);
            }
        }

        if (shaped) {
            for (int gi = 0; gi < shaped->num_glyphs; gi++) {
                uint32_t gid = shaped->glyph_ids[gi];
                if (gid == 0)
                    continue;
                // Tag the gid so 1-cell and 2-cell rasters of the same glyph
                // get separate atlas entries (matches single-glyph path bit 29).
                uint32_t atlas_gid = (columns_to_consume >= 2) ? (gid | (1u << 29)) : gid;
                RendSdl3AtlasEntry *entry =
                    rend_sdl3_atlas_lookup(&data->atlas, font_data, atlas_gid, color_key);
                if (!entry) {
                    GlyphBitmap *gb = font_render_glyph_id(data->font, style, gid,
                                                           render_r, render_g, render_b);
                    if (gb) {
                        entry = cache_glyph(&data->atlas, font_data, atlas_gid, color_key,
                                            gb, downscale_glyph,
                                            cache_w, cache_h, height_only_fit, color_baked);
                        data->font->free_glyph_bitmap(data->font, gb);
                    } else {
                        rend_sdl3_atlas_insert_empty(&data->atlas, font_data, atlas_gid, color_key);
                    }
                }
                if (!populate_only) {
                    int x_off = shaped->x_positions[gi] + (entry ? entry->x_offset : 0);
                    int y_off = entry ? entry->y_offset : 0;
                    blit_glyph(data->renderer, &data->atlas, entry,
                               cell_x, cell_y, x_off,
                               y_off, avail_w, avail_h, data->font_ascent,
                               color_baked, r, g, b, data->glyph_shader, bg_luma);
                }
            }
            free(shaped->glyph_ids);
            free(shaped->x_positions);
            free(shaped->y_positions);
            free(shaped->x_advances);
            free(shaped);
            return;
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
            color_baked = rend_is_color_font(data->font, style);
            render_r = color_baked ? r : 255;
            render_g = color_baked ? g : 255;
            render_b = color_baked ? b : 255;
            color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                    : 0xFFFFFF;
            glyph_index = font_get_glyph_index(data->font, style, codepoint);
        }

        // Dynamic fallback: if still missing, query fontconfig for a covering font
        if (glyph_index == 0) {
            const char *fb_path = rend_fallback_lookup(&data->fallback, data->resolve, codepoint);
            if (fb_path && rend_fallback_ensure(&data->fallback, data->font, fb_path, data->font_size, &data->font_options, data->cell_width)) {
                style = FONT_STYLE_FALLBACK;
                font_data = data->font->font_data[style];
                // Lazily loaded — set presentation_width now
                font_set_presentation_width(data->font, style, avail_w);
                color_baked = rend_is_color_font(data->font, style);
                render_r = color_baked ? r : 255;
                render_g = color_baked ? g : 255;
                render_b = color_baked ? b : 255;
                color_key = color_baked ? ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b
                                        : 0xFFFFFF;
                glyph_index = font_get_glyph_index(data->font, style, codepoint);
            }
        }

        // Tag glyph_index so glyphs at different presentation widths
        // get separate atlas entries (different rasterization sizes).
        uint32_t atlas_glyph_id = glyph_index;
        if (columns_to_consume >= 2 && atlas_glyph_id != 0)
            atlas_glyph_id |= (1u << 29);

        if (atlas_glyph_id != 0)
            entry = rend_sdl3_atlas_lookup(&data->atlas, font_data, atlas_glyph_id, color_key);
        if (!entry) {
            GlyphBitmap *glyph_bitmap = font_render_glyphs(data->font, style, &codepoint, 1,
                                                           render_r, render_g, render_b);
            if (glyph_bitmap) {
                uint32_t insert_id = atlas_glyph_id ? atlas_glyph_id
                                                    : (uint32_t)glyph_bitmap->glyph_id;
                entry = cache_glyph(&data->atlas, font_data, insert_id, color_key,
                                    glyph_bitmap, downscale_glyph,
                                    cache_w, cache_h, height_only_fit, color_baked);
                data->font->free_glyph_bitmap(data->font, glyph_bitmap);
            } else if (atlas_glyph_id != 0) {
                rend_sdl3_atlas_insert_empty(&data->atlas, font_data, atlas_glyph_id, color_key);
            }
        }
        if (!populate_only)
            blit_glyph(data->renderer, &data->atlas, entry,
                       cell_x, cell_y,
                       entry ? entry->x_offset : 0, entry ? entry->y_offset : 0,
                       avail_w, avail_h, data->font_ascent,
                       color_baked, r, g, b, data->glyph_shader, bg_luma);
    }

    // Selection highlight
    if (!populate_only) {
        int scroll_offset = data->scroll.scroll_offset;
        int scrollback_row = scroll_offset - 1 - row;
        int unified_row = (scrollback_row >= 0) ? -(scrollback_row + 1) : (row - scroll_offset);
        if (terminal_cell_in_selection(term, unified_row, vt_col)) {
            float sx = (float)(vis_col * data->cell_width);
            float sy = (float)(row * data->cell_height);
            float sw = (float)(columns_to_consume * data->cell_width);
            float sh = (float)data->cell_height;

            SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(data->renderer, TERM_SELECTION_R, TERM_SELECTION_G,
                                   TERM_SELECTION_B, TERM_SELECTION_A);
            SDL_FRect sel_rect = { sx, sy, sw, sh };
            SDL_RenderFillRect(data->renderer, &sel_rect);
            SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_NONE);
        }
    }
}

// Flush one coalesced SGR-underline run [vis_start, vis_end) on `row`, in the
// given style (1=single … 5=dashed) and color. Shared by the mid-row and
// end-of-row flushes in render_visible_cells.
static void flush_underline_run(RendererSdl3Data *data, int row, int vis_start,
                                int vis_end, unsigned int style, Uint8 r,
                                Uint8 g, Uint8 b)
{
    float pd = data->content_scale;
    int thickness = (int)roundf(1.0f * pd);
    if (thickness < 1)
        thickness = 1;
    int cell_y = row * data->cell_height;
    int underline_y = cell_y + data->font_ascent + (int)roundf(2.0f * pd);
    if (underline_y + thickness > cell_y + data->cell_height)
        underline_y = cell_y + data->cell_height - thickness;
    int run_x = vis_start * data->cell_width;
    int run_w = (vis_end - vis_start) * data->cell_width;
    SDL_SetRenderDrawColor(data->renderer, r, g, b, TERM_UNDERLINE_A);
    switch (style) {
    case UNDERLINE_SINGLE:
        draw_underline_single(data->renderer, run_x, underline_y, run_w, pd);
        break;
    case UNDERLINE_DOUBLE:
        draw_underline_double(data->renderer, run_x, underline_y, run_w, pd);
        break;
    case UNDERLINE_CURLY:
        draw_underline_curly(data->renderer, run_x, underline_y, run_w, pd, r, g, b);
        break;
    case UNDERLINE_DOTTED:
        draw_underline_dotted(data->renderer, run_x, underline_y, run_w, pd);
        break;
    case UNDERLINE_DASHED:
        draw_underline_dashed(data->renderer, run_x, underline_y, run_w, pd);
        break;
    }
}

// Flush one coalesced strikethrough run [vis_start, vis_end) on `row`.
static void flush_strike_run(RendererSdl3Data *data, int row, int vis_start,
                             int vis_end, Uint8 r, Uint8 g, Uint8 b)
{
    float pd = data->content_scale;
    int cell_y = row * data->cell_height;
    int strike_y = cell_y + data->font_ascent - data->font_cap_height / 2;
    int run_x = vis_start * data->cell_width;
    int run_w = (vis_end - vis_start) * data->cell_width;
    SDL_SetRenderDrawColor(data->renderer, r, g, b, 255);
    draw_strikethrough(data->renderer, run_x, strike_y, run_w, pd);
}

static void render_visible_cells(RendererSdl3Data *data, TerminalBackend *term,
                                 int display_rows, int display_cols,
                                 bool cursor_visible, bool populate_only)
{
    TerminalPos cursor_pos = terminal_get_cursor_pos(term);
    // Hide cursor when scrolled back, when terminal says it's not visible, when cursor_visible is false,
    // or when cursor is outside visible bounds (can happen during resize before shell repositions cursor)
    bool cursor_in_bounds = cursor_pos.row >= 0 && cursor_pos.row < display_rows &&
                            cursor_pos.col >= 0 && cursor_pos.col < display_cols;
    bool show_cursor = cursor_visible && cursor_in_bounds &&
                       (data->scroll.scroll_offset == 0) && terminal_get_cursor_visible(term);

    for (int row = 0; row < display_rows; row++) {
        int unified_row = row - data->scroll.scroll_offset;
        TerminalRowIter it;

        // Pass 1: draw all cell backgrounds for this row.
        if (!populate_only) {
            terminal_row_iter_init(&it, term, unified_row, display_cols);
            while (terminal_row_iter_next(&it)) {
                render_cell_bg(data, row, it.vis_col, it.pres_w, &it.cell);
            }
        }
        // Pass 1.5: draw cursor (under glyphs)
        if (!populate_only && show_cursor && cursor_pos.row == row) {
            terminal_row_iter_init(&it, term, unified_row, display_cols);
            while (terminal_row_iter_next(&it)) {
                if (it.vt_col == cursor_pos.col) {
                    float cx = (float)(it.vis_col * data->cell_width);
                    float cy = (float)(row * data->cell_height);
                    float cw = (float)(it.pres_w * data->cell_width);
                    float ch = (float)data->cell_height;
                    SDL_SetRenderDrawColor(data->renderer, TERM_CURSOR_R, TERM_CURSOR_G,
                                           TERM_CURSOR_B, 255);
                    draw_rounded_rect(data->renderer, cx, cy, cw, ch, 2.0f);
                    break;
                }
            }
        }
        // Pass 2: draw glyphs and selection overlays
        terminal_row_iter_init(&it, term, unified_row, display_cols);
        while (terminal_row_iter_next(&it)) {
            // Skip foreground rendering for invisible text (SGR 8)
            if (!it.cell.attrs.invis) {
                render_cell(data, term, row, it.vt_col, it.vis_col, it.pres_w,
                            &it.cell, populate_only);
            }
        }
        // Pass 3: draw underlines as continuous spans across consecutive cells
        if (!populate_only) {
            terminal_row_iter_init(&it, term, unified_row, display_cols);
            int vis_run_start = -1;
            int vis_run_end = 0;
            unsigned int run_style = 0;
            Uint8 run_r = 0, run_g = 0, run_b = 0;
            while (terminal_row_iter_next(&it)) {
                unsigned int cs = it.cell.attrs.underline;
                Uint8 cr = it.cell.ul_color.is_default ? TERM_UNDERLINE_R : it.cell.ul_color.r;
                Uint8 cg = it.cell.ul_color.is_default ? TERM_UNDERLINE_G : it.cell.ul_color.g;
                Uint8 cb = it.cell.ul_color.is_default ? TERM_UNDERLINE_B : it.cell.ul_color.b;
                bool same_run = (run_style != 0 && cs == run_style && cr == run_r &&
                                 cg == run_g && cb == run_b);
                if (run_style != 0 && !same_run) {
                    flush_underline_run(data, row, vis_run_start, vis_run_end,
                                        run_style, run_r, run_g, run_b);
                    run_style = 0;
                }
                if (cs != 0 && run_style == 0) {
                    vis_run_start = it.vis_col;
                    run_style = cs;
                    run_r = cr;
                    run_g = cg;
                    run_b = cb;
                }
                vis_run_end = it.vis_col + it.pres_w;
            }
            if (run_style != 0)
                flush_underline_run(data, row, vis_run_start, vis_run_end,
                                    run_style, run_r, run_g, run_b);
        }
        // Pass 4: draw strikethroughs as continuous spans across consecutive cells
        if (!populate_only) {
            terminal_row_iter_init(&it, term, unified_row, display_cols);
            int vis_run_start = -1;
            int vis_run_end = 0;
            bool in_run = false;
            Uint8 run_r = 0, run_g = 0, run_b = 0;
            while (terminal_row_iter_next(&it)) {
                bool cs = it.cell.attrs.strikethrough;
                Uint8 cr = it.cell.fg.r, cg = it.cell.fg.g, cb = it.cell.fg.b;
                bool same_run = in_run && cs && cr == run_r && cg == run_g && cb == run_b;
                if (in_run && !same_run) {
                    flush_strike_run(data, row, vis_run_start, vis_run_end, run_r,
                                     run_g, run_b);
                    in_run = false;
                }
                if (cs && !in_run) {
                    vis_run_start = it.vis_col;
                    in_run = true;
                    run_r = cr;
                    run_g = cg;
                    run_b = cb;
                }
                vis_run_end = it.vis_col + it.pres_w;
            }
            if (in_run)
                flush_strike_run(data, row, vis_run_start, vis_run_end, run_r,
                                 run_g, run_b);
        }
    }
}

// Destroy all cached sixel textures
static void sixel_cache_clear(RendererSdl3Data *data)
{
    for (int i = 0; i < data->sixel_cache_count; i++) {
        if (data->sixel_cache[i].texture)
            SDL_DestroyTexture(data->sixel_cache[i].texture);
        data->sixel_cache[i].texture = NULL;
    }
    data->sixel_cache_count = 0;
}

// Drop cached textures whose image id is no longer live, so the cache
// tracks the engine's current image set (and frees textures for evicted /
// scrolled-off / cleared images). O(cache * live) but both are small.
static void sixel_cache_reconcile(RendererSdl3Data *data, const CfrSixel *imgs, int count)
{
    for (int i = 0; i < data->sixel_cache_count;) {
        bool live = false;
        for (int j = 0; j < count; j++) {
            if (imgs[j].id == data->sixel_cache[i].id) {
                live = true;
                break;
            }
        }
        if (live) {
            i++;
        } else {
            if (data->sixel_cache[i].texture)
                SDL_DestroyTexture(data->sixel_cache[i].texture);
            data->sixel_cache[i] = data->sixel_cache[--data->sixel_cache_count];
        }
    }
}

// Find or create an SDL_Texture for a sixel image, keyed by its stable id.
// On a version change (animation) the texture is re-uploaded in place; a
// dimension change forces a recreate.
static SDL_Texture *sixel_get_texture(RendererSdl3Data *data, const CfrSixel *img)
{
    uint8_t *linear = linearize_for_upload(data, img->rgba, img->width_px, img->height_px);
    const uint8_t *pixels = linear ? linear : img->rgba;

    for (int i = 0; i < data->sixel_cache_count; i++) {
        if (data->sixel_cache[i].id != img->id)
            continue;
        if (data->sixel_cache[i].w != img->width_px ||
            data->sixel_cache[i].h != img->height_px) {
            // Dimensions changed — recreate the texture object.
            if (data->sixel_cache[i].texture)
                SDL_DestroyTexture(data->sixel_cache[i].texture);
            data->sixel_cache[i].texture = NULL;
        } else if (data->sixel_cache[i].version != img->version) {
            // Same size, new pixels — re-upload in place (no churn).
            SDL_UpdateTexture(data->sixel_cache[i].texture, NULL, pixels,
                              img->width_px * 4);
            data->sixel_cache[i].version = img->version;
            free(linear);
            return data->sixel_cache[i].texture;
        } else {
            free(linear);
            return data->sixel_cache[i].texture;
        }
        // Fall through to recreate into this slot.
        SDL_Texture *t = SDL_CreateTexture(data->renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC, img->width_px,
                                           img->height_px);
        if (!t) {
            free(linear);
            return NULL;
        }
        SDL_UpdateTexture(t, NULL, pixels, img->width_px * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        data->sixel_cache[i].texture = t;
        data->sixel_cache[i].version = img->version;
        data->sixel_cache[i].w = img->width_px;
        data->sixel_cache[i].h = img->height_px;
        free(linear);
        return t;
    }

    // Not cached — create and insert.
    SDL_Texture *tex = SDL_CreateTexture(data->renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, img->width_px,
                                         img->height_px);
    if (!tex) {
        free(linear);
        return NULL;
    }
    SDL_UpdateTexture(tex, NULL, pixels, img->width_px * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    free(linear);

    if (data->sixel_cache_count >= SIXEL_CACHE_MAX) {
        // Cache full (>256 simultaneous images — effectively never). Skip
        // rather than leak an uncached texture.
        SDL_DestroyTexture(tex);
        return NULL;
    }
    int n = data->sixel_cache_count++;
    data->sixel_cache[n].texture = tex;
    data->sixel_cache[n].id = img->id;
    data->sixel_cache[n].version = img->version;
    data->sixel_cache[n].w = img->width_px;
    data->sixel_cache[n].h = img->height_px;
    return tex;
}

// Render sixel images on top of the terminal. The engine returns each
// image's anchor as a unified row (>= 0 visible, < 0 scrollback); the
// display row is unified_row + scroll_offset, identical to how cells map.
static void render_sixel_images(RendererSdl3Data *data, TerminalBackend *term)
{
    int count = 0;
    const CfrSixel *imgs = terminal_get_sixels(term, &count);
    sixel_cache_reconcile(data, imgs, count);
    if (count == 0)
        return;

    float scale = data->content_scale > 0.0f ? data->content_scale : 1.0f;

    for (int i = 0; i < count; i++) {
        const CfrSixel *img = &imgs[i];

        int screen_row = img->row + data->scroll.scroll_offset;
        int px = img->col * data->cell_width;
        int py = screen_row * data->cell_height;

        int scaled_w = logical_to_physical(img->width_px, scale);
        int scaled_h = logical_to_physical(img->height_px, scale);

        if (py + scaled_h <= 0 || py >= data->height)
            continue;
        if (px + scaled_w <= 0 || px >= data->width)
            continue;

        SDL_Texture *tex = sixel_get_texture(data, img);
        if (!tex)
            continue;

        SDL_FRect dst = { (float)px, (float)py, (float)scaled_w,
                          (float)scaled_h };
        SDL_RenderTexture(data->renderer, tex, NULL, &dst);
    }
}

// ---------------------------------------------------------------------------
// Lottie animation cache and rendering
// ---------------------------------------------------------------------------

static void lottie_cache_clear(RendererSdl3Data *data)
{
    for (int i = 0; i < data->lottie_cache_count; i++) {
        if (data->lottie_cache[i].texture)
            SDL_DestroyTexture(data->lottie_cache[i].texture);
    }
    data->lottie_cache_count = 0;
}

static void lottie_cache_reconcile(RendererSdl3Data *data,
                                   const CfrLottie *anims, int count)
{
    for (int i = 0; i < data->lottie_cache_count;) {
        bool live = false;
        for (int j = 0; j < count; j++) {
            if (anims[j].id == data->lottie_cache[i].id) {
                live = true;
                break;
            }
        }
        if (live) {
            i++;
        } else {
            if (data->lottie_cache[i].texture)
                SDL_DestroyTexture(data->lottie_cache[i].texture);
            data->lottie_cache[i] =
                data->lottie_cache[--data->lottie_cache_count];
        }
    }
}

static SDL_Texture *lottie_get_texture(RendererSdl3Data *data,
                                       const CfrLottie *anim)
{
    uint8_t *linear = linearize_for_upload(data, anim->rgba, anim->canvas_w, anim->canvas_h);
    const uint8_t *pixels = linear ? linear : anim->rgba;

    for (int i = 0; i < data->lottie_cache_count; i++) {
        if (data->lottie_cache[i].id != anim->id)
            continue;
        if (data->lottie_cache[i].w != anim->canvas_w ||
            data->lottie_cache[i].h != anim->canvas_h) {
            if (data->lottie_cache[i].texture)
                SDL_DestroyTexture(data->lottie_cache[i].texture);
            data->lottie_cache[i].texture = NULL;
        } else if (data->lottie_cache[i].version != anim->version) {
            SDL_UpdateTexture(data->lottie_cache[i].texture, NULL, pixels,
                              anim->canvas_w * 4);
            data->lottie_cache[i].version = anim->version;
            free(linear);
            return data->lottie_cache[i].texture;
        } else {
            free(linear);
            return data->lottie_cache[i].texture;
        }
        SDL_Texture *t = SDL_CreateTexture(
            data->renderer, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STATIC, anim->canvas_w, anim->canvas_h);
        if (!t) {
            free(linear);
            return NULL;
        }
        SDL_UpdateTexture(t, NULL, pixels, anim->canvas_w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        data->lottie_cache[i].texture = t;
        data->lottie_cache[i].version = anim->version;
        data->lottie_cache[i].w = anim->canvas_w;
        data->lottie_cache[i].h = anim->canvas_h;
        free(linear);
        return t;
    }

    SDL_Texture *tex = SDL_CreateTexture(
        data->renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STATIC, anim->canvas_w, anim->canvas_h);
    if (!tex) {
        free(linear);
        return NULL;
    }
    SDL_UpdateTexture(tex, NULL, pixels, anim->canvas_w * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    free(linear);

    if (data->lottie_cache_count >= LOTTIE_CACHE_MAX) {
        SDL_DestroyTexture(tex);
        return NULL;
    }
    int n = data->lottie_cache_count++;
    data->lottie_cache[n].texture = tex;
    data->lottie_cache[n].id = anim->id;
    data->lottie_cache[n].version = anim->version;
    data->lottie_cache[n].w = anim->canvas_w;
    data->lottie_cache[n].h = anim->canvas_h;
    return tex;
}

static void render_lottie_layer(RendererSdl3Data *data, TerminalBackend *term,
                                uint8_t target_layer)
{
    int count = 0;
    const CfrLottie *anims = terminal_get_lotties(term, &count);
    lottie_cache_reconcile(data, anims, count);
    if (count == 0)
        return;

    float scale = data->content_scale > 0.0f ? data->content_scale : 1.0f;

    for (int i = 0; i < count; i++) {
        const CfrLottie *anim = &anims[i];
        int pl_count = 0;
        const CfrLottiePlacement *pls =
            terminal_get_lottie_placements(term, anim->id, &pl_count);

        SDL_Texture *tex = lottie_get_texture(data, anim);
        if (!tex)
            continue;

        int scaled_canvas_w = logical_to_physical(anim->canvas_w, scale);
        int scaled_canvas_h = logical_to_physical(anim->canvas_h, scale);

        for (int j = 0; j < pl_count; j++) {
            const CfrLottiePlacement *pl = &pls[j];
            if (pl->layer != target_layer)
                continue;

            int screen_row = pl->row + data->scroll.scroll_offset;
            int px = pl->col * data->cell_width;
            int py = screen_row * data->cell_height;
            int box_w = pl->cols * data->cell_width;
            int box_h = pl->rows * data->cell_height;

            if (py + box_h <= 0 || py >= data->height)
                continue;
            if (px + box_w <= 0 || px >= data->width)
                continue;

            if (pl->opacity_x256 < 255)
                SDL_SetTextureAlphaModFloat(tex,
                                            (float)pl->opacity_x256 / 255.0f);

            int off_x = (box_w - scaled_canvas_w) / 2;
            int off_y = (box_h - scaled_canvas_h) / 2;
            SDL_FRect dst = {
                (float)(px + off_x), (float)(py + off_y),
                (float)scaled_canvas_w, (float)scaled_canvas_h
            };
            SDL_RenderTexture(data->renderer, tex, NULL, &dst);

            if (pl->opacity_x256 < 255)
                SDL_SetTextureAlphaModFloat(tex, 1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Linear-light compositing
// ---------------------------------------------------------------------------
//
// portty blends antialiased glyph coverage in *linear* light (like kitty),
// rather than gamma-incorrectly in sRGB space. We do this by drawing the whole
// frame into an RGBA64_FLOAT render target tagged SDL_COLORSPACE_SRGB_LINEAR:
// SDL linearizes sRGB *draw/vertex colors* (FillRect colors, colormod fg) on
// read, blends in linear, and re-encodes linear->sRGB when we blit the float
// target back onto the active sRGB target. Coverage (alpha) blends correctly
// and solid/colormod colors round-trip exactly.
//
// CAVEAT: SDL does NOT decode *sampled texture texels* on this path -- only
// draw colors. The glyph atlas is white-coverage for text (gamma-invariant) so
// text is unaffected, but color emoji carry real RGB in the texel and would be
// double-encoded (washed out) by the blit-out. We compensate by sRGB->linear
// decoding color-glyph texels when they enter the atlas (rend_sdl3_atlas.c,
// gated on RendSdl3Atlas.linearize_color), so they round-trip exactly too.

// Ensure a linear float render target large enough for the currently bound
// target's pixel size, writing that size to *out_w/*out_h (the top-left region
// the caller must render into and blit back out). The target is GROW-ONLY:
// linear_w/linear_h track the *allocated* size, which is always >= the output,
// and it is only ever reallocated to grow — never to shrink. Recreating this
// large RGBA64F target on every resize (especially a fullscreen<->windowed
// toggle) churns GPU allocations and forces SDL's GPU backend to rebuild
// pipelines tied to the target format; that path has crashed on NVK (a
// zeroed/freed VkPipeline bound during the command-queue flush). A stable
// grow-only target removes that per-resize churn. draw_scene_linear
// additionally calls SDL_FlushRenderer before each SDL_SetRenderTarget
// switch to invalidate any cached pipeline state that a swapchain rebuild
// may have freed. Returns false (and disables the linear path permanently)
// if the GPU can't allocate the float target.
static bool ensure_linear_target(RendererSdl3Data *data, int *out_w, int *out_h)
{
    if (out_w)
        *out_w = 0;
    if (out_h)
        *out_h = 0;

    if (!data->linear_ok)
        return false;

    int w = 0, h = 0;
    if (!SDL_GetCurrentRenderOutputSize(data->renderer, &w, &h) || w <= 0 || h <= 0)
        return false;

    if (out_w)
        *out_w = w;
    if (out_h)
        *out_h = h;

    // Reuse whenever the existing target already covers the output (grow-only).
    if (data->linear_target && data->linear_w >= w && data->linear_h >= h)
        return true;

    // Need a bigger target. Grow each dimension independently so the allocation
    // only ever expands (and so a later resize back up reuses it).
    int alloc_w = data->linear_w > w ? data->linear_w : w;
    int alloc_h = data->linear_h > h ? data->linear_h : h;

    if (data->linear_target) {
        SDL_DestroyTexture(data->linear_target);
        data->linear_target = NULL;
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props)
        return false;
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                          SDL_PIXELFORMAT_RGBA64_FLOAT);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                          SDL_COLORSPACE_SRGB_LINEAR);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                          SDL_TEXTUREACCESS_TARGET);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, alloc_w);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, alloc_h);
    SDL_Texture *tex = SDL_CreateTextureWithProperties(data->renderer, props);
    SDL_DestroyProperties(props);

    if (!tex) {
        vlog("Linear render target %dx%d unavailable (%s); falling back to "
             "legacy sRGB blending\n",
             alloc_w, alloc_h, SDL_GetError());
        data->linear_ok = false;
        return false;
    }

    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    data->linear_target = tex;
    data->linear_w = alloc_w;
    data->linear_h = alloc_h;
    vlog("Linear render target grown to %dx%d RGBA64F / SRGB_LINEAR (output %dx%d)\n",
         alloc_w, alloc_h, w, h);
    return true;
}

// One-shot canary: composite 50%-coverage white over black through the linear
// path and read back the midtone. A linear-correct blend yields ~188; a
// gamma-incorrect sRGB blend yields ~128. This confirms SDL's renderer really
// linearizes on this backend (and is a permanent regression tripwire under -v).
static void linear_blend_selfcheck(RendererSdl3Data *data)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props)
        return;
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                          SDL_PIXELFORMAT_RGBA64_FLOAT);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                          SDL_COLORSPACE_SRGB_LINEAR);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                          SDL_TEXTUREACCESS_TARGET);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, 4);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, 4);
    SDL_Texture *lin = SDL_CreateTextureWithProperties(data->renderer, props);
    SDL_DestroyProperties(props);
    SDL_Texture *srgb = SDL_CreateTexture(data->renderer, SDL_PIXELFORMAT_RGBA32,
                                          SDL_TEXTUREACCESS_TARGET, 4, 4);
    if (!lin || !srgb) {
        if (lin)
            SDL_DestroyTexture(lin);
        if (srgb)
            SDL_DestroyTexture(srgb);
        return;
    }

    // A white texture whose coverage we drive via alpha-mod, mirroring how real
    // glyphs are drawn (textured blit, not a solid fill).
    SDL_Texture *white = SDL_CreateTexture(data->renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC, 1, 1);
    if (white) {
        Uint8 wpx[4] = { 255, 255, 255, 255 };
        SDL_UpdateTexture(white, NULL, wpx, 4);
        SDL_SetTextureBlendMode(white, SDL_BLENDMODE_BLEND);
    }

    SDL_Texture *prev = SDL_GetRenderTarget(data->renderer);

    int fill_mid = -1, tex_mid = -1;

    // Variant 1: 50% white via solid FillRect (draw color) over black.
    SDL_SetRenderTarget(data->renderer, lin);
    SDL_SetRenderDrawColor(data->renderer, 0, 0, 0, 255);
    SDL_RenderClear(data->renderer);
    SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(data->renderer, 255, 255, 255, 128);
    SDL_RenderFillRect(data->renderer, NULL);
    SDL_SetRenderDrawBlendMode(data->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(data->renderer, srgb);
    SDL_SetTextureBlendMode(lin, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(data->renderer, lin, NULL, NULL);
    SDL_Surface *s1 = SDL_RenderReadPixels(data->renderer, NULL);
    if (s1) {
        Uint8 r, g, b, a;
        if (SDL_ReadSurfacePixel(s1, 0, 0, &r, &g, &b, &a))
            fill_mid = r;
        SDL_DestroySurface(s1);
    }

    // Variant 2: 50% white via textured blit with alpha-mod (the glyph path).
    if (white) {
        SDL_SetRenderTarget(data->renderer, lin);
        SDL_SetRenderDrawColor(data->renderer, 0, 0, 0, 255);
        SDL_RenderClear(data->renderer);
        SDL_SetTextureAlphaMod(white, 128);
        SDL_SetTextureColorMod(white, 255, 255, 255);
        SDL_RenderTexture(data->renderer, white, NULL, NULL);
        SDL_SetRenderTarget(data->renderer, srgb);
        SDL_RenderTexture(data->renderer, lin, NULL, NULL);
        SDL_Surface *s2 = SDL_RenderReadPixels(data->renderer, NULL);
        if (s2) {
            Uint8 r, g, b, a;
            if (SDL_ReadSurfacePixel(s2, 0, 0, &r, &g, &b, &a))
                tex_mid = r;
            SDL_DestroySurface(s2);
        }
    }

    SDL_SetRenderTarget(data->renderer, prev);

    vlog("Linear-blend self-check (50%% white over black): fill=%d texture=%d "
         "(expect ~188 linear-correct, ~128 if gamma-incorrect)\n",
         fill_mid, tex_mid);

    if (white)
        SDL_DestroyTexture(white);
    SDL_DestroyTexture(lin);
    SDL_DestroyTexture(srgb);
}

// Draw the already-populated scene into the linear-light target and encode it
// onto whatever render target is currently bound (SDL window or PNG export target). Falls back to drawing straight into the bound
// target (legacy sRGB) when the float target is unavailable.
static void draw_scene_linear(RendererSdl3Data *data, TerminalBackend *term,
                              int display_rows, int display_cols,
                              bool cursor_visible, bool with_sixel)
{
    SDL_Texture *dst = SDL_GetRenderTarget(data->renderer);
    int out_w = 0, out_h = 0;
    bool linear = ensure_linear_target(data, &out_w, &out_h);

    if (linear) {
        // Flush any pending commands from the previous frame and invalidate
        // cached pipeline state.  After a swapchain rebuild (e.g. fullscreen
        // toggle), NVK frees the old VkPipeline objects.  If the SDL3 GPU
        // backend still holds a cached pipeline reference from the previous
        // frame's render pass, the BindGraphicsPipeline inside
        // SDL_SetRenderTarget's internal command-queue flush will dereference
        // a freed handle and SIGSEGV.  SDL_FlushRenderer submits the pending
        // command queue and invalidates all cached state so SDL will prepare
        // fresh state for the new render pass, avoiding the stale-pipeline
        // bind.
        SDL_FlushRenderer(data->renderer);
        SDL_SetRenderTarget(data->renderer, data->linear_target);
    }

    SDL_SetRenderDrawColor(data->renderer, TERM_BG_R, TERM_BG_G, TERM_BG_B, TERM_BG_A);
    SDL_RenderClear(data->renderer);
    render_visible_cells(data, term, display_rows, display_cols, cursor_visible, false);
    render_lottie_layer(data, term, 1); /* background lottie */
    render_lottie_layer(data, term, 0); /* foreground lottie */
    if (with_sixel)
        render_sixel_images(data, term);

    if (linear) {
        SDL_FlushRenderer(data->renderer);
        SDL_SetRenderTarget(data->renderer, dst);
        SDL_SetTextureBlendMode(data->linear_target, SDL_BLENDMODE_NONE);
        // The target is grow-only and may be larger than the output: copy only
        // the used top-left out_w x out_h region 1:1 (same size + NEAREST scale
        // = exact copy, so the linear->sRGB re-encode is byte-identical). A
        // NULL/NULL blit would scale the whole oversized target into dst.
        SDL_FRect used = { 0.0f, 0.0f, (float)out_w, (float)out_h };
        SDL_RenderTexture(data->renderer, data->linear_target, &used, &used);
    }
}

// --- Top notification panel (pure-SDL3 path) ---------------------------------
//
// The renderer draws the panel; panel_active defaults false and the
// panel draw is a no-op when inactive.

// Build ANSI content for panel terminal with background color, title (bold),
// and body text.
static char *rend_sdl3_panel_build_ansi(const char *title, const char *body)
{
    // Simple string builder using static buffer (panels are small)
    static char buf[2048];
    int len = 0;

    const char *panel_bg = "\x1b[48;2;38;38;44m";

    // Clear screen first in case terminal is being reused
    len += snprintf(buf + len, sizeof(buf) - len, "\x1b[2J\x1b[H");
    if (len < 0 || len >= (int)sizeof(buf))
        return NULL;

    if (title) {
        len += snprintf(buf + len, sizeof(buf) - len,
                        "%s\x1b[1m\x1b[38;2;236;236;241m%s\x1b[22m\x1b[39m",
                        panel_bg, title);
        if (len < 0 || len >= (int)sizeof(buf))
            return NULL;
    }
    if (body) {
        if (title) {
            len += snprintf(buf + len, sizeof(buf) - len, "\r\n%s", panel_bg);
            if (len < 0 || len >= (int)sizeof(buf))
                return NULL;
        } else {
            len += snprintf(buf + len, sizeof(buf) - len, "%s", panel_bg);
            if (len < 0 || len >= (int)sizeof(buf))
                return NULL;
        }
        len += snprintf(buf + len, sizeof(buf) - len,
                        "\x1b[38;2;190;190;198m%s\x1b[39m", body);
        if (len < 0 || len >= (int)sizeof(buf))
            return NULL;
    }

    return len > 0 ? strdup(buf) : NULL;
}

// Build terminal for a panel slot by rendering to texture

// Close "×" bitmap is now shared via rend_make_close_x_bitmap() in rend_common.c.
// Build a `size`×`size` white texture holding an anti-aliased "×". SDL's 2D
// renderer can't antialias geometry, so we bake coverage into the alpha channel
// (analytic distance-to-segment) and blit the texture — the SDL-recommended way
// to get smooth edges. The glyph is white; callers tint it via SDL_SetTextureColorMod.
static SDL_Texture *make_close_x_texture(SDL_Renderer *r, int size)
{
    if (size <= 0)
        return NULL;
    uint8_t *buf = calloc((size_t)size * size, 4);
    if (!buf)
        return NULL;

    rend_make_close_x_bitmap(buf, size);

    SDL_Texture *tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, size, size);
    if (tex) {
        SDL_UpdateTexture(tex, NULL, buf, size * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    }
    free(buf);
    return tex;
}

// Forward declaration
static void populate_atlas(RendererSdl3Data *data, TerminalBackend *term,
                           int rows, int cols, bool cursor_visible);

// Build terminal and texture for a panel slot based on PanelState
static void build_panel_terminal(RendererSdl3Data *data, int slot)
{
    if (slot < 0 || slot >= PORTTY_PANEL_MAX)
        return;

    PanelState *ps = &data->panels.panels[slot];
    if (!ps->active || !data->font || data->cell_width <= 0 || data->cell_height <= 0 ||
        !font_has_style(data->font, FONT_STYLE_NORMAL))
        return;

    // Terminal size = panel size minus decoration cells
    int term_cols = panel_term_cols(ps->cols, panel_show_accent(ps->flags));
    int term_rows = panel_term_rows(ps->rows);
    if (term_cols <= 0 || term_rows <= 0)
        return;

    // Terminal pixel size
    int term_w = term_cols * data->cell_width;
    int term_h = term_rows * data->cell_height;

    // Check if terminal needs recreation (wrong size or doesn't exist)
    int existing_cols = 0, existing_rows = 0;
    if (data->panel_terms[slot]) {
        terminal_get_dimensions(data->panel_terms[slot], &existing_rows, &existing_cols);
    }

    if (!data->panel_terms[slot] || existing_cols != term_cols || existing_rows != term_rows) {
        // Destroy old terminal if it exists
        if (data->panel_terms[slot]) {
            terminal_destroy(data->panel_terms[slot]);
            free(data->panel_terms[slot]);
            data->panel_terms[slot] = NULL;
        }
        // Create new terminal with correct dimensions
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = term_cols;
        cfg.rows = term_rows;
        cfg.cell_w_px = data->cell_width;
        cfg.cell_h_px = data->cell_height;
        data->panel_terms[slot] = term_cfr_new(&cfg);
    }
    if (!data->panel_terms[slot])
        return;

    // Build ANSI content and feed to terminal
    char *ansi = rend_sdl3_panel_build_ansi(ps->title, ps->body);
    if (ansi) {
        terminal_process_input(data->panel_terms[slot], ansi, strlen(ansi));
        terminal_flush_damage(data->panel_terms[slot]);
        free(ansi);
    }

    // Create or reuse texture
    if (!data->panel_textures[slot] ||
        (data->panel_textures[slot] &&
         (SDL_GetTextureSize(data->panel_textures[slot], NULL, NULL), false))) {
        if (data->panel_textures[slot])
            SDL_DestroyTexture(data->panel_textures[slot]);
        data->panel_textures[slot] = SDL_CreateTexture(
            data->renderer, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_TARGET, term_w, term_h);
    }
    if (!data->panel_textures[slot])
        return;

    SDL_SetTextureBlendMode(data->panel_textures[slot], SDL_BLENDMODE_BLEND);

    // First, populate atlas with panel glyphs (may be different from main terminal)
    int term_r, term_c;
    terminal_get_dimensions(data->panel_terms[slot], &term_r, &term_c);
    populate_atlas(data, data->panel_terms[slot], term_r, term_c, false);

    // Save current render target
    SDL_Texture *prev_target = SDL_GetRenderTarget(data->renderer);

    // Render panel terminal to texture
    SDL_SetRenderTarget(data->renderer, data->panel_textures[slot]);
    SDL_SetRenderDrawColor(data->renderer, 38, 38, 44, 255);
    SDL_RenderClear(data->renderer);

    // Render terminal cells to the texture
    render_visible_cells(data, data->panel_terms[slot], term_r, term_c, false, false);

    // Restore render target
    SDL_SetRenderTarget(data->renderer, prev_target);
}

// Two-phase atlas populate (shared by sdl3_draw_terminal and sdl3_render_to_png):
// insert glyphs with no draw calls, and if eviction occurred mid-pass, flush the
// partial staging and re-populate so destroyed glyphs are re-rasterized; then
// flush staging to the GPU. The caller must have called rend_sdl3_atlas_begin_frame
// first; on return the atlas is ready for the draw pass. Doing the upload while
// the render queue is empty avoids the implicit flush inside SDL_UpdateTexture
// interfering with in-flight draw commands.
static void populate_atlas(RendererSdl3Data *data, TerminalBackend *term,
                           int rows, int cols, bool cursor_visible)
{
    data->atlas.packing.eviction_occurred = false;
    render_visible_cells(data, term, rows, cols, cursor_visible, true);
    if (data->atlas.packing.eviction_occurred) {
        rend_sdl3_atlas_flush(&data->atlas);
        data->atlas.packing.eviction_occurred = false;
        render_visible_cells(data, term, rows, cols, cursor_visible, true);
    }
    rend_sdl3_atlas_flush(&data->atlas);
}

void rend_sdl3_draw_terminal(RendererSdl3Data *data, TerminalBackend *term,
                             bool cursor_visible)
{
    if (!data || !term)
        return;

    // Internal pager overlay: when set, draw it full-screen in place of the
    // host terminal. scroll_offset already holds the overlay's view position
    // (stashed/restored by rend_sdl3_set_overlay / rend_sdl3_clear_overlay), and
    // the overlay has no cursor of its own.
    if (data->scroll.overlay) {
        term = data->scroll.overlay;
        cursor_visible = false;
    }

    if (!font_has_style(data->font, FONT_STYLE_NORMAL)) {
        vlog("Renderer draw terminal failed: invalid parameters\n");
        return;
    }

    if (verbose && !data->linear_selfcheck_done) {
        data->linear_selfcheck_done = true;
        if (data->linear_ok)
            linear_blend_selfcheck(data);
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

    populate_atlas(data, term, display_rows, display_cols, cursor_visible);

    // Draw the scene gamma-correct (linear-light) and draw sixel images.
    draw_scene_linear(data, term, display_rows, display_cols, cursor_visible, true);

    // Build textures for dirty panels (after main terminal atlas work is complete)
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        PanelState *ps = &data->panels.panels[i];
        if (ps->active && ps->dirty) {
            build_panel_terminal(data, i);
            ps->dirty = false;
        }
    }

    // Draw active panels (sRGB UI chrome over terminal frame)
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        PanelState *ps = &data->panels.panels[i];
        if (!ps->active || !data->panel_textures[i])
            continue;

        // Draw panel background rect
        SDL_FRect bg_rect = { (float)ps->px, (float)ps->py, (float)ps->pw, (float)ps->ph };
        SDL_SetRenderDrawColor(data->renderer, 38, 38, 44, 255);
        SDL_RenderFillRect(data->renderer, &bg_rect);

        // Draw accent stripe (if enabled)
        if (panel_show_accent(ps->flags)) {
            uint8_t ar, ag, ab;
            switch (ps->level) {
            case PORTTY_NOTIFY_ERROR:
                ar = 235; /* Sriracha */
                ag = 66;
                ab = 104;
                break;
            case PORTTY_NOTIFY_WARNING:
                ar = 245; /* Mustard */
                ag = 239;
                ab = 52;
                break;
            default:
                ar = 71; /* Thunder */
                ag = 118;
                ab = 255;
                break;
            }
            int accent_px = panel_accent_px(ps->px, data->cell_width);
            int accent_w = panel_accent_w(data->cell_width);
            SDL_FRect accent_rect = { (float)accent_px, (float)ps->py,
                                      (float)accent_w, (float)ps->ph };
            SDL_SetRenderDrawColor(data->renderer, ar, ag, ab, 255);
            SDL_RenderFillRect(data->renderer, &accent_rect);
        }

        // Draw panel text texture at offset position
        int text_x = panel_term_px(ps->px, data->cell_width, panel_show_accent(ps->flags));
        int text_y = panel_term_py(ps->py, data->cell_height);
        float term_w, term_h;
        SDL_GetTextureSize(data->panel_textures[i], &term_w, &term_h);
        SDL_FRect tex_dst = { (float)text_x, (float)text_y, term_w, term_h };
        SDL_RenderTexture(data->renderer, data->panel_textures[i], NULL, &tex_dst);

        // Close button (if enabled)
        if (panel_show_close(ps->flags) && ps->close_size > 0) {
            SDL_Texture *close_tex = make_close_x_texture(data->renderer, ps->close_size);
            if (close_tex) {
                uint8_t lum = ps->close_hover ? 245 : 170;
                SDL_SetTextureColorMod(close_tex, lum, lum, lum);
                SDL_FRect close_dst = { (float)ps->close_px,
                                        (float)ps->close_py,
                                        (float)ps->close_size, (float)ps->close_size };
                SDL_RenderTexture(data->renderer, close_tex, NULL, &close_dst);
                SDL_DestroyTexture(close_tex);
            }
        }
    }
}

void rend_sdl3_present(RendererSdl3Data *data)
{
    if (!data)
        return;

    SDL_RenderPresent(data->renderer);
}

void rend_sdl3_log_stats(RendererSdl3Data *data)
{
    if (!data)
        return;

    rend_sdl3_atlas_log_stats(&data->atlas);
}

void rend_sdl3_resize(RendererSdl3Data *data, int width, int height)
{
    if (!data)
        return;

    data->width = width;
    data->height = height;
    // Rebuild active panels on resize (dirty flag set by recompute_layout)
    panel_mgr_recompute_layout(&data->panels);
}

bool rend_sdl3_get_cell_size(RendererSdl3Data *data, int *cell_width, int *cell_height)
{
    if (!data)
        return false;
    if (data->cell_width <= 0 || data->cell_height <= 0)
        return false;
    *cell_width = data->cell_width;
    *cell_height = data->cell_height;
    return true;
}

bool rend_sdl3_get_diag(RendererSdl3Data *data, PorttyDiag *out)
{
    if (!data || !out)
        return false;
    out->backend_name = SDL_GetRendererName(data->renderer);
    out->linear_light = data->linear_ok;
    out->glyph_shader = (data->glyph_shader != NULL);
    out->content_scale = data->content_scale;
    out->pixel_width = data->width;
    out->pixel_height = data->height;
    out->cell_width = data->cell_width;
    out->cell_height = data->cell_height;
    out->font_path = data->font_path;
    out->hinting = data->hint_name;
    out->gpu_device = data->gpu_name[0] ? data->gpu_name : NULL;
    out->gpu_driver = data->gpu_driver[0] ? data->gpu_driver : NULL;
    out->gpu_driver_libre = data->gpu_driver_libre;
    out->display_session = NULL;
    out->display_xwayland = NULL;
    out->display_screen = NULL;
    out->display_dpi = NULL;
    out->display_scale = NULL;
    out->display_physical = display_info_get_physical();

    // Display scaling context — uses SDL3 display APIs (cross-platform)
    {
        static char session_str[32];
        static char scale_str[128];
        static char screen_str[128];
        static char dpi_str[128];
        static char xwayland_str[8];

        const char *video_driver = SDL_GetCurrentVideoDriver();
        const char *xdg_session = getenv("XDG_SESSION_TYPE");

        if (xdg_session && *xdg_session) {
            snprintf(session_str, sizeof(session_str), "%s", xdg_session);
        } else if (video_driver) {
            snprintf(session_str, sizeof(session_str), "%s", video_driver);
        }
        if (session_str[0])
            out->display_session = session_str;

        // XWayland detection (Linux only)
        if (xdg_session && strcmp(xdg_session, "wayland") == 0 && video_driver &&
            strcmp(video_driver, "x11") == 0) {
            snprintf(xwayland_str, sizeof(xwayland_str), "yes");
            out->display_xwayland = xwayland_str;
        }

        if (data->window) {
            SDL_DisplayID display_id = SDL_GetDisplayForWindow(data->window);
            if (!display_id)
                display_id = SDL_GetPrimaryDisplay();
            if (display_id) {
                SDL_Rect bounds;
                if (SDL_GetDisplayBounds(display_id, &bounds)) {
                    const char *display_name = SDL_GetDisplayName(display_id);
                    if (display_name)
                        snprintf(screen_str, sizeof(screen_str), "%dx%d px (%s)",
                                 bounds.w, bounds.h, display_name);
                    else
                        snprintf(screen_str, sizeof(screen_str), "%dx%d px",
                                 bounds.w, bounds.h);
                    out->display_screen = screen_str;
                }

                float content_scale = SDL_GetDisplayContentScale(display_id);
                float window_scale = SDL_GetWindowDisplayScale(data->window);
                snprintf(scale_str, sizeof(scale_str),
                         "content %.2f, window %.2f",
                         (double)content_scale, (double)window_scale);
                out->display_scale = scale_str;

                // DPI estimate: SDL3 content scale is relative to 96 DPI
                if (content_scale > 0.0f) {
                    float dpi = content_scale * 96.0f;
                    snprintf(dpi_str, sizeof(dpi_str), "%.1f (from content scale)", dpi);
                    out->display_dpi = dpi_str;
                }
            }
        }
    }

    return true;
}

void rend_sdl3_scroll(RendererSdl3Data *data, TerminalBackend *term, int delta)
{
    if (!data)
        return;
    rend_scroll(&data->scroll, term, delta);
}

void rend_sdl3_reset_scroll(RendererSdl3Data *data)
{
    if (!data)
        return;
    rend_reset_scroll(&data->scroll);
}

int rend_sdl3_get_scroll_offset(RendererSdl3Data *data)
{
    if (!data)
        return 0;
    return rend_get_scroll_offset(&data->scroll);
}

void rend_sdl3_set_overlay(RendererSdl3Data *data, TerminalBackend *overlay)
{
    if (!data || !overlay)
        return;
    rend_set_overlay(&data->scroll, overlay);
}

void rend_sdl3_clear_overlay(RendererSdl3Data *data)
{
    if (!data)
        return;
    rend_clear_overlay(&data->scroll);
}

bool rend_sdl3_has_overlay(RendererSdl3Data *data)
{
    if (!data)
        return false;
    return rend_has_overlay(&data->scroll);
}

void rend_sdl3_panel_show(RendererSdl3Data *data, int id, int col, int row, int cols, int rows,
                          const char *title, const char *body, PorttyNotifyLevel level,
                          unsigned int flags)
{
    if (!data)
        return;

    // Find slot
    int slot = -1;
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        if (data->panels.panels[i].active && data->panels.panels[i].id == id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
            if (!data->panels.panels[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0)
        return;

    panel_mgr_set_cell_size(&data->panels, data->cell_width, data->cell_height);
    PanelState *ps = panel_mgr_show(&data->panels, id, col, row, cols, rows, title, body, level, flags);
    if (!ps)
        return;

    // Panel texture will be built during draw pass when dirty flag is checked
    (void)slot; // Slot index available if needed
}

void rend_sdl3_panel_hide(RendererSdl3Data *data, int id)
{
    if (!data)
        return;

    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        if (data->panels.panels[i].active && data->panels.panels[i].id == id) {
            panel_free_slot(data, i);
            panel_mgr_hide(&data->panels, id);
            break;
        }
    }
}

int rend_sdl3_panel_hit_test(RendererSdl3Data *data, int px, int py, bool *close_btn)
{
    if (!data)
        return 0;
    return panel_mgr_hit_test(&data->panels, px, py, close_btn);
}

void rend_sdl3_panel_set_hover(RendererSdl3Data *data, int id, bool hovered)
{
    if (!data)
        return;
    panel_mgr_set_hover(&data->panels, id, hovered);
}

int rend_sdl3_render_to_png(RendererSdl3Data *data, TerminalBackend *term,
                            const char *output_path)
{
    if (!data || !term || !output_path)
        return -1;

    if (!font_has_style(data->font, FONT_STYLE_NORMAL)) {
        fprintf(stderr, "ERROR: No font loaded for PNG render\n");
        return -1;
    }

    if (verbose && !data->linear_selfcheck_done) {
        data->linear_selfcheck_done = true;
        if (data->linear_ok)
            linear_blend_selfcheck(data);
    }

    // Get terminal dimensions
    int term_rows, term_cols;
    terminal_get_dimensions(term, &term_rows, &term_cols);

    // Find the rightmost non-empty column across all rows so multi-row
    // outputs (e.g. -P --exec) aren't trimmed to row 0's width. Symbol-class
    // codepoints rendered through a text font are anchored on the baseline
    // at natural width and may overhang the cell — bump the canvas by one
    // column whenever the rightmost glyph is symbol-class so the overhang
    // is preserved in the snapshot.
    int last_col = 0;
    bool last_is_symbol_overhang = false;
    for (int row = 0; row < term_rows; row++) {
        for (int col = 0; col < term_cols; col++) {
            TerminalCell cell;
            if (terminal_get_cell(term, row, col, &cell) == 0 && cell.cp != 0) {
                int end = col + (cell.width > 0 ? cell.width : 1);
                if (end > last_col) {
                    last_col = end;
                    last_is_symbol_overhang = rend_is_symbol_cell_cp(cell.cp);
                } else if (end == last_col && rend_is_symbol_cell_cp(cell.cp)) {
                    last_is_symbol_overhang = true;
                }
            }
        }
    }
    if (last_col <= 0)
        last_col = 1;

    int render_cols = last_col + (last_is_symbol_overhang ? 1 : 0);
    int render_rows = term_rows;

    int img_w = render_cols * data->cell_width;
    int img_h = render_rows * data->cell_height;

    // Expand the canvas to cover any sixel images so they aren't cropped
    // away by the text-content bounding box (a sixel-only output has no
    // text to size from).
    {
        int sc = 0;
        const CfrSixel *si = terminal_get_sixels(term, &sc);
        for (int i = 0; i < sc; i++) {
            if (si[i].row < 0)
                continue; // anchored in scrollback, above the snapshot
            int right = si[i].col * data->cell_width + si[i].width_px;
            int bottom = si[i].row * data->cell_height + si[i].height_px;
            if (right > img_w)
                img_w = right;
            if (bottom > img_h)
                img_h = bottom;
        }
    }

    vlog("PNG render: %d cols x %d rows = %dx%d pixels\n",
         render_cols, render_rows, img_w, img_h);

    // render_sixel_images() culls against data->width/height (the 1x1 hidden
    // window in PNG mode). Point them at the render canvas for the snapshot.
    int saved_w = data->width, saved_h = data->height;
    int saved_scroll = data->scroll.scroll_offset;
    data->width = img_w;
    data->height = img_h;
    data->scroll.scroll_offset = 0;

    // Create offscreen render target texture
    SDL_Texture *target = SDL_CreateTexture(data->renderer,
                                            SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_TARGET,
                                            img_w, img_h);
    if (!target) {
        fprintf(stderr, "ERROR: Failed to create offscreen texture: %s\n", SDL_GetError());
        return -1;
    }

    // Redirect rendering to offscreen texture
    if (!SDL_SetRenderTarget(data->renderer, target)) {
        fprintf(stderr, "ERROR: Failed to set render target: %s\n", SDL_GetError());
        SDL_DestroyTexture(target);
        return -1;
    }

    rend_sdl3_atlas_begin_frame(&data->atlas);
    populate_atlas(data, term, render_rows, render_cols, false);

    // Draw the scene gamma-correct (linear-light) into `target`
    // so the PNG matches on-screen output, including sixel images.
    draw_scene_linear(data, term, render_rows, render_cols, false, true);

    data->width = saved_w;
    data->height = saved_h;
    data->scroll.scroll_offset = saved_scroll;

    // Read pixels back from the render target
    SDL_Surface *surface = SDL_RenderReadPixels(data->renderer, NULL);

    // Restore default render target
    SDL_SetRenderTarget(data->renderer, NULL);

    if (!surface) {
        fprintf(stderr, "ERROR: Failed to read pixels: %s\n", SDL_GetError());
        SDL_DestroyTexture(target);
        return -1;
    }

    // Convert surface to RGBA32 if needed
    SDL_Surface *rgba_surface = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);

    if (!rgba_surface) {
        fprintf(stderr, "ERROR: Failed to convert surface to RGBA: %s\n", SDL_GetError());
        SDL_DestroyTexture(target);
        return -1;
    }

    // Write PNG
    int rc = png_write_rgba(output_path, (const uint8_t *)rgba_surface->pixels,
                            rgba_surface->w, rgba_surface->h);

    SDL_DestroySurface(rgba_surface);
    SDL_DestroyTexture(target);

    if (rc == 0) {
        fprintf(stderr, "STATUS: png_output=%s (%dx%d)\n", output_path, img_w, img_h);
    } else {
        fprintf(stderr, "ERROR: Failed to write PNG to %s\n", output_path);
    }

    return rc;
}

void rend_sdl3_set_content_scale(RendererSdl3Data *data, float scale)
{
    if (!data)
        return;
    if (scale > 0.0f) {
        data->content_scale = scale;
        vlog("Content scale set to %.2f\n", scale);
    }
}

void rend_sdl3_process_pty_data(RendererSdl3Data *data, TerminalBackend *term,
                                const char *data_bytes, size_t len)
{
    terminal_consume_pushed_rows(term);
    terminal_process_input(term, data_bytes, len);
    int pushed = terminal_consume_pushed_rows(term);
    if (pushed > 0 && rend_sdl3_get_scroll_offset(data) > 0)
        rend_sdl3_scroll(data, term, pushed);
}
