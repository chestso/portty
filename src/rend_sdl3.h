/*
 * portty — SDL3 GPU renderer interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef REND_SDL3_H
#define REND_SDL3_H

#include "diag.h"
#include "font.h"
#include "portty_backend.h"
#include "portty_panel.h"
#include "rend_common.h"
#include "rend_sdl3_atlas.h"
#include "rend_sdl3_shader.h"
#include "term.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "term_colors.h"

#define IMAGE_CACHE_MAX  256
#define LOTTIE_CACHE_MAX 64

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
    int font_cap_height;
    int width;
    int height;
    RendScrollState scroll;

    RendSdl3Atlas atlas;

    FontResolveBackend *resolve;

    RendFallbackState fallback;
    float font_size;
    FontOptions font_options;

    float content_scale;

    char *font_path;
    const char *hint_name;

    char gpu_name[128];
    char gpu_driver[160];
    GpuDriverLibre gpu_driver_libre;

    SDL_Texture *linear_target;
    int linear_w, linear_h;
    bool linear_ok;
    bool linear_selfcheck_done;

    RendShaderState *glyph_shader;

    PanelManager panels;
    TerminalBackend *panel_terms[PORTTY_PANEL_MAX];
    SDL_Texture *panel_textures[PORTTY_PANEL_MAX];

    struct
    {
        SDL_Texture *texture;
        uint64_t id;
        uint32_t version;
        int w, h;
    } image_cache[IMAGE_CACHE_MAX];
    int image_cache_count;

    struct
    {
        SDL_Texture *texture;
        uint64_t id;
        uint32_t version;
        int w, h;
    } lottie_cache[LOTTIE_CACHE_MAX];
    int lottie_cache_count;

    TermColors colors;
} RendererSdl3Data;

bool rend_sdl3_init(RendererSdl3Data *data, SDL_Window *window, SDL_Renderer *renderer);
void rend_sdl3_destroy(RendererSdl3Data *data);
int rend_sdl3_load_fonts(RendererSdl3Data *data, float font_size, const char *font_name, int ft_hint_target);
void rend_sdl3_draw_terminal(RendererSdl3Data *data, TerminalBackend *term, bool cursor_visible);
void rend_sdl3_present(RendererSdl3Data *data);
void rend_sdl3_resize(RendererSdl3Data *data, int width, int height);
void rend_sdl3_log_stats(RendererSdl3Data *data);
bool rend_sdl3_get_cell_size(RendererSdl3Data *data, int *cell_width, int *cell_height);
bool rend_sdl3_get_diag(RendererSdl3Data *data, PorttyDiag *out);
void rend_sdl3_scroll(RendererSdl3Data *data, TerminalBackend *term, int delta);
void rend_sdl3_reset_scroll(RendererSdl3Data *data);
int rend_sdl3_get_scroll_offset(RendererSdl3Data *data);
void rend_sdl3_set_overlay(RendererSdl3Data *data, TerminalBackend *overlay);
void rend_sdl3_clear_overlay(RendererSdl3Data *data);
bool rend_sdl3_has_overlay(RendererSdl3Data *data);
void rend_sdl3_panel_show(RendererSdl3Data *data, int id, int col, int row, int cols, int rows,
                          const char *title, const char *body, PorttyNotifyLevel level,
                          unsigned int flags);
void rend_sdl3_panel_hide(RendererSdl3Data *data, int id);
int rend_sdl3_panel_hit_test(RendererSdl3Data *data, int px, int py, bool *close_btn);
void rend_sdl3_panel_set_hover(RendererSdl3Data *data, int id, bool hovered);
int rend_sdl3_render_to_png(RendererSdl3Data *data, TerminalBackend *term, const char *output_path);
void rend_sdl3_set_content_scale(RendererSdl3Data *data, float scale);
int rend_sdl3_process_pty_data(RendererSdl3Data *data, TerminalBackend *term, const char *data_bytes, size_t len);

#endif // REND_SDL3_H
