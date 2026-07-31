#ifndef REND_SOKOL_H
#define REND_SOKOL_H

#include "diag.h"
#include "font.h"
#include "portty_backend.h"
#include "portty_panel.h"
#include "rend_common.h"
#include "rend_sokol_atlas.h"
#include "term.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sokol/sokol_gfx.h>

#define SOKOL_MAX_COLS            400
#define SOKOL_MAX_ROWS            120
#define SOKOL_MAX_VERTICES        (SOKOL_MAX_COLS * SOKOL_MAX_ROWS * 12)
#define SOKOL_MAX_DECO_VERTICES   200000
#define SOKOL_MAX_LOTTIE_VERTICES 4096
#define SOKOL_MAX_SIXEL_VERTICES  4096

#define SOKOL_LOTTIE_CACHE_MAX 64
#define SOKOL_SIXEL_CACHE_MAX  256

// Vertex format: position (xy) + texcoord (uv) + fg color (rgba) + bg color (rgba)
// 2 floats + 2 floats + 4 ubytes + 4 ubytes = 20 bytes per vertex
typedef struct
{
    float x, y;    // pixel position
    float u, v;    // atlas texcoord
    uint8_t fg[4]; // foreground color (RGBA)
    uint8_t bg[4]; // background color (RGBA)
} GlyphVertex;

typedef struct
{
    sg_image image;
    sg_view view;
    uint64_t id;
    uint32_t version;
    int w, h;
} SokolLottieCacheEntry;

typedef struct
{
    sg_image image;
    sg_view view;
    uint64_t id;
    uint32_t version;
    int w, h;
} SokolSixelCacheEntry;

typedef struct RendererSokolData
{
    FontBackend *font;
    int cell_w;
    int cell_h;
    int width;
    int height;
    int font_ascent;
    int font_descent;
    int font_cap_height;
    float content_scale;
    RendScrollState scroll;

    RendSokolAtlas atlas;
    sg_pipeline glyph_pip;
    sg_buffer glyph_vbuf;
    bool glyph_pip_created;
    sg_pipeline sel_pip;
    bool sel_pip_created;

    FontResolveBackend *resolve;
    RendFallbackState fallback;
    float font_size;
    FontOptions font_options;

    char *font_path;
    const char *hint_name;

    bool linear_ok;

    PanelManager panels;
    TerminalBackend *panel_terms[PORTTY_PANEL_MAX];

    SokolLottieCacheEntry lottie_cache[SOKOL_LOTTIE_CACHE_MAX];
    int lottie_cache_count;
    sg_pipeline lottie_pip;
    sg_buffer lottie_vbuf;
    sg_sampler lottie_sampler;
    bool lottie_pip_created;

    SokolSixelCacheEntry sixel_cache[SOKOL_SIXEL_CACHE_MAX];
    int sixel_cache_count;
    sg_buffer sixel_vbuf;
    bool sixel_vbuf_created;
} RendererSokolData;

// Public rendering API
void rend_sokol_init(RendererSokolData *data);
void rend_sokol_destroy(RendererSokolData *data);
bool rend_sokol_get_cell_size(RendererSokolData *data, int *cell_w, int *cell_h);
void rend_sokol_scroll(RendererSokolData *data, TerminalBackend *term, int delta);
void rend_sokol_reset_scroll(RendererSokolData *data);
int rend_sokol_get_scroll_offset(RendererSokolData *data);
void rend_sokol_set_overlay(RendererSokolData *data, TerminalBackend *overlay);
void rend_sokol_clear_overlay(RendererSokolData *data);
bool rend_sokol_has_overlay(RendererSokolData *data);
void rend_sokol_set_content_scale(RendererSokolData *data, float scale);

// Pipeline creation
void rend_sokol_ensure_glyph_pipeline(RendererSokolData *data);
void rend_sokol_ensure_lottie_pipeline(RendererSokolData *data);

// Static buffer accessors (for backend_sokol.c)
GlyphVertex *rend_sokol_get_frame_verts(void);
GlyphVertex *rend_sokol_get_glyph_verts(void);
GlyphVertex *rend_sokol_get_cursor_verts(void);
int *rend_sokol_get_vert_index(void);
int *rend_sokol_get_frame_vert_count_ptr(void);
int *rend_sokol_get_cursor_vert_count_ptr(void);
GlyphVertex *rend_sokol_get_lottie_verts(void);

// Buffer operations
void rend_sokol_reset_frame_buffers(void);

// Debug utilities (used by script commands)
void rend_sokol_deco_reset(void);
int rend_sokol_deco_get_count(void);
GlyphVertex *rend_sokol_deco_get_verts(void);

void rend_sokol_deco_emit_quad(float x0, float y0, float x1, float y1,
                               const uint8_t color[4]);

typedef struct
{
    int y;
    float x_start;
    float x_end;
    uint8_t alpha;
} DecoStrip;

void rend_sokol_deco_strip_emit(float x0, float x1, int y, uint8_t alpha,
                                const uint8_t color[4]);
void rend_sokol_deco_coalesce_update(float x, float y_min, float y_max,
                                     const uint8_t *alphas, DecoStrip *strips,
                                     int *strip_count, int max_strips,
                                     const uint8_t color[4]);
void rend_sokol_deco_coalesce_flush(DecoStrip *strips, int *strip_count,
                                    const uint8_t color[4]);

int rend_sokol_underline_position(RendererSokolData *data, int row);
void rend_sokol_draw_underline_single(RendererSokolData *data, int row,
                                      int vis_start, int vis_end,
                                      const uint8_t color[4]);
void rend_sokol_draw_underline_double(RendererSokolData *data, int row,
                                      int vis_start, int vis_end,
                                      const uint8_t color[4]);
void rend_sokol_draw_underline_curly(RendererSokolData *data, int row,
                                     int vis_start, int vis_end,
                                     const uint8_t color[4]);
void rend_sokol_draw_underline_dotted(RendererSokolData *data, int row,
                                      int vis_start, int vis_end,
                                      const uint8_t color[4]);
void rend_sokol_draw_underline_dashed(RendererSokolData *data, int row,
                                      int vis_start, int vis_end,
                                      const uint8_t color[4]);
void rend_sokol_draw_strikethrough(RendererSokolData *data, int row,
                                   int vis_start, int vis_end,
                                   const uint8_t color[4]);

// Color conversion
void rend_sokol_cell_color(TerminalColor tc, bool is_fg, bool reverse,
                           uint8_t out[4]);

// Lottie/sixel cache management
void rend_sokol_lottie_cache_reconcile(RendererSokolData *data,
                                        const CfrLottie *anims, int count);
int rend_sokol_lottie_get_texture(RendererSokolData *data, const CfrLottie *anim);
void rend_sokol_sixel_cache_reconcile(RendererSokolData *data,
                                       const CfrSixel *imgs, int count);
int rend_sokol_sixel_get_texture(RendererSokolData *data, const CfrSixel *img);

#endif // REND_SOKOL_H
