#ifndef REND_SDL3_H
#define REND_SDL3_H

#include "diag.h"
#include "font.h"
#include "portty_backend.h"
#include "rend_common.h"
#include "rend_sdl3_atlas.h"
#include "rend_sdl3_shader.h"
#include "term.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIXEL_CACHE_MAX  256
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

    bool notif_active;
    bool notif_close_hover;
    int notif_level;
    char *notif_title;
    char *notif_body;
    SDL_Texture *notif_texture;
    SDL_Texture *notif_close_tex;
    int notif_h;
    SDL_FRect notif_close_rect;

    bool hint_active;
    char *hint_text;
    SDL_Texture *hint_texture;
    int hint_h;
    int hint_anchor_py;

    struct
    {
        SDL_Texture *texture;
        uint64_t id;
        uint32_t version;
        int w, h;
    } sixel_cache[SIXEL_CACHE_MAX];
    int sixel_cache_count;

    struct
    {
        SDL_Texture *texture;
        uint64_t id;
        uint32_t version;
        int w, h;
    } lottie_cache[LOTTIE_CACHE_MAX];
    int lottie_cache_count;
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
void rend_sdl3_set_notification(RendererSdl3Data *data, const char *title, const char *body, int level);
void rend_sdl3_clear_notification(RendererSdl3Data *data);
bool rend_sdl3_set_notification_hover(RendererSdl3Data *data, bool hovered);
void rend_sdl3_set_link_hint(RendererSdl3Data *data, const char *url, int anchor_py);
int rend_sdl3_notification_hit(RendererSdl3Data *data, int px, int py);
int rend_sdl3_render_to_png(RendererSdl3Data *data, TerminalBackend *term, const char *output_path);
void rend_sdl3_set_content_scale(RendererSdl3Data *data, float scale);
void rend_sdl3_process_pty_data(RendererSdl3Data *data, TerminalBackend *term, const char *data_bytes, size_t len);

#endif // REND_SDL3_H
