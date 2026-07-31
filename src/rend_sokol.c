#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rend_sokol.h"
#include "common.h"
#include "font.h"
#include "font_resolve.h"
#include "rend_common.h"
#include "rend_sokol_atlas.h"
#include "term.h"
#include <coffer/coffer.h>
#include <math.h>
#include <sokol/sokol_gfx.h>
#include <sokol/sokol_glue.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Cursor color: Charm signature purple, opaque (RGBA)
#define CURSOR_COLOR_R 0x6B
#define CURSOR_COLOR_G 0x50
#define CURSOR_COLOR_B 0xFF
#define CURSOR_COLOR_A 255

// Selection highlight color: muted Dracula comment-style (RGBA)
#define SELECTION_COLOR_R 0x44
#define SELECTION_COLOR_G 0x47
#define SELECTION_COLOR_B 0x5A
#define SELECTION_COLOR_A 180

// Underline color: matches cursor color
#define UNDERLINE_COLOR_R CURSOR_COLOR_R
#define UNDERLINE_COLOR_G CURSOR_COLOR_G
#define UNDERLINE_COLOR_B CURSOR_COLOR_B
#define UNDERLINE_COLOR_A 255

// Default background color — used both as the render pass clear color and
// as the bg for cells with bg.is_default.
#define DEF_BG_R 0x00
#define DEF_BG_G 0x00
#define DEF_BG_B 0x00

// File-scope vertex arrays (moved from static-local so debug dump functions
// can access them after the render pass).
static GlyphVertex s_frame_verts[SOKOL_MAX_VERTICES];
static int s_frame_vert_count;
static int s_vert_index[SOKOL_MAX_ROWS][SOKOL_MAX_COLS];
static GlyphVertex s_glyph_verts[SOKOL_MAX_VERTICES];
static GlyphVertex s_cursor_verts[6];
static int s_cursor_vert_count;
static GlyphVertex s_deco_verts[SOKOL_MAX_DECO_VERTICES];
static int s_deco_vert_count;
static bool s_deco_overflow_warned;
static GlyphVertex s_lottie_verts[SOKOL_MAX_LOTTIE_VERTICES];

// ── Buffer accessors ─────────────────────────────────────────────────────────

GlyphVertex *rend_sokol_get_frame_verts(void) { return s_frame_verts; }
GlyphVertex *rend_sokol_get_glyph_verts(void) { return s_glyph_verts; }
GlyphVertex *rend_sokol_get_cursor_verts(void) { return s_cursor_verts; }
int *rend_sokol_get_vert_index(void) { return &s_vert_index[0][0]; }
int *rend_sokol_get_frame_vert_count_ptr(void) { return &s_frame_vert_count; }
int *rend_sokol_get_cursor_vert_count_ptr(void) { return &s_cursor_vert_count; }
GlyphVertex *rend_sokol_get_lottie_verts(void) { return s_lottie_verts; }

void rend_sokol_reset_frame_buffers(void)
{
    s_frame_vert_count = 0;
    s_cursor_vert_count = 0;
    memset(s_vert_index, -1, sizeof(s_vert_index));
}

// ── Color conversion ──────────────────────────────────────────────────────

static void cell_color(TerminalColor tc, bool is_fg, bool reverse, uint8_t out[4])
{
    const uint8_t def_bg[4] = { DEF_BG_R, DEF_BG_G, DEF_BG_B, 0xFF };
    const uint8_t def_fg[4] = { 0xD0, 0xD0, 0xD0, 0xFF };
    if (tc.is_default) {
        if (reverse && is_fg) {
            memcpy(out, def_bg, 4);
        } else {
            memcpy(out, is_fg ? def_fg : def_bg, 4);
        }
    } else {
        out[0] = tc.r;
        out[1] = tc.g;
        out[2] = tc.b;
        out[3] = 0xFF;
    }
}

// ── Pipeline creation ──────────────────────────────────────────────────────

void rend_sokol_ensure_lottie_pipeline(RendererSokolData *data)
{
    if (data->lottie_pip_created)
        return;

    static const char *vs_src =
        "#version 410\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(location=1) in vec2 uv;\n"
        "layout(location=2) in vec4 fg;\n"
        "out vec2 v_uv;\n"
        "out float v_opacity;\n"
        "uniform vec2 u_resolution;\n"
        "void main() {\n"
        "  vec2 clip = vec2(pos.x / u_resolution.x * 2.0 - 1.0,\n"
        "                   1.0 - pos.y / u_resolution.y * 2.0);\n"
        "  gl_Position = vec4(clip, 0.0, 1.0);\n"
        "  v_uv = uv;\n"
        "  v_opacity = fg.r;\n"
        "}\n";
    static const char *fs_src =
        "#version 410\n"
        "in vec2 v_uv;\n"
        "in float v_opacity;\n"
        "out vec4 frag_color;\n"
        "uniform sampler2D lottie_tex;\n"
        "vec3 srgb_to_linear(vec3 c) {\n"
        "  return mix(pow((c + vec3(0.055)) / vec3(1.055), vec3(2.4)),\n"
        "             c / 12.92,\n"
        "             lessThanEqual(c, vec3(0.04045)));\n"
        "}\n"
        "void main() {\n"
        "  vec4 texel = texture(lottie_tex, v_uv);\n"
        "  float alpha = texel.a * v_opacity;\n"
        "  frag_color = vec4(srgb_to_linear(texel.rgb), alpha);\n"
        "}\n";

    sg_shader shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func.source = vs_src,
        .fragment_func.source = fs_src,
        .attrs[0].glsl_name = "pos",
        .attrs[1].glsl_name = "uv",
        .attrs[2].glsl_name = "fg",
        .uniform_blocks[0] = {
            .stage = SG_SHADERSTAGE_VERTEX,
            .size = sizeof(float) * 2,
            .glsl_uniforms = {
                [0] = { .glsl_name = "u_resolution", .type = SG_UNIFORMTYPE_FLOAT2 },
            },
        },
        .views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT,
        .views[0].texture.image_type = SG_IMAGETYPE_2D,
        .views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT,
        .samplers[0].stage = SG_SHADERSTAGE_FRAGMENT,
        .samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING,
        .texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT,
        .texture_sampler_pairs[0].view_slot = 0,
        .texture_sampler_pairs[0].sampler_slot = 0,
        .texture_sampler_pairs[0].glsl_name = "lottie_tex",
        .label = "sokol-lottie-shader",
    });

    data->lottie_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .size = SOKOL_MAX_LOTTIE_VERTICES * sizeof(GlyphVertex),
        .usage.dynamic_update = true,
        .label = "sokol-lottie-vbuf",
    });

    data->lottie_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .layout = {
            .buffers[0].stride = sizeof(GlyphVertex),
            .attrs = {
                [0] = { .offset = offsetof(GlyphVertex, x), .format = SG_VERTEXFORMAT_FLOAT2 },
                [1] = { .offset = offsetof(GlyphVertex, u), .format = SG_VERTEXFORMAT_FLOAT2 },
                [2] = { .offset = offsetof(GlyphVertex, fg), .format = SG_VERTEXFORMAT_UBYTE4N },
            },
        },
        .colors[0] = {
            .pixel_format = data->linear_ok ? SG_PIXELFORMAT_SRGB8A8 : SG_PIXELFORMAT_RGBA8,
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .src_factor_alpha = SG_BLENDFACTOR_ONE,
                .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            },
        },
        .label = "sokol-lottie-pipeline",
    });

    data->lottie_sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .label = "sokol-lottie-sampler",
    });

    data->lottie_pip_created = true;
}

void rend_sokol_ensure_glyph_pipeline(RendererSokolData *data)
{
    if (data->glyph_pip_created)
        return;

    static const char *vs_src =
        "#version 410\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(location=1) in vec2 uv;\n"
        "layout(location=2) in vec4 fg;\n"
        "layout(location=3) in vec4 bg;\n"
        "out vec2 v_uv;\n"
        "out vec4 v_fg;\n"
        "out vec4 v_bg;\n"
        "out vec2 v_cell_size;\n"
        "uniform vec2 u_resolution;\n"
        "uniform vec2 u_cell_size;\n"
        "void main() {\n"
        "  vec2 clip = vec2(pos.x / u_resolution.x * 2.0 - 1.0,\n"
        "                   1.0 - pos.y / u_resolution.y * 2.0);\n"
        "  gl_Position = vec4(clip, 0.0, 1.0);\n"
        "  v_uv = uv;\n"
        "  v_fg = fg;\n"
        "  v_bg = bg;\n"
        "  v_cell_size = u_cell_size;\n"
        "}\n";
    static const char *fs_src =
        "#version 410\n"
        "in vec2 v_uv;\n"
        "in vec4 v_fg;\n"
        "in vec4 v_bg;\n"
        "in vec2 v_cell_size;\n"
        "out vec4 frag_color;\n"
        "uniform sampler2D atlas;\n"
        "vec3 srgb_to_linear(vec3 c) {\n"
        "  return mix(pow((c + vec3(0.055)) / vec3(1.055), vec3(2.4)),\n"
        "             c / 12.92,\n"
        "             lessThanEqual(c, vec3(0.04045)));\n"
        "}\n"
        "void main() {\n"
        "  if (v_uv.x < 0.0) {\n"
        "    float lx = -v_uv.x - 2.0;\n"
        "    float ly = v_uv.y;\n"
        "    float px = lx * v_cell_size.x;\n"
        "    float py = ly * v_cell_size.y;\n"
        "    float r = 0.09 * v_cell_size.y;\n"
        "    float qx = min(px, v_cell_size.x - px);\n"
        "    float qy = min(py, v_cell_size.y - py);\n"
        "    if (qx < r && qy < r) {\n"
        "      if (length(vec2(r - qx, r - qy)) > r) discard;\n"
        "    }\n"
        "    frag_color = vec4(srgb_to_linear(v_fg.rgb), v_fg.a);\n"
        "    return;\n"
        "  }\n"
        "  if (v_uv.x >= 3.0) {\n"
        "    vec2 uv = vec2(v_uv.x - 3.0, v_uv.y);\n"
        "    vec4 texel = texture(atlas, uv);\n"
        "    vec3 bg_lin = srgb_to_linear(v_bg.rgb);\n"
        "    vec3 composited = mix(bg_lin, texel.rgb, texel.a);\n"
        "    frag_color = vec4(composited, v_bg.a);\n"
        "    return;\n"
        "  }\n"
        "  if (v_uv.x >= 2.0) {\n"
        "    vec3 bg_lin = srgb_to_linear(v_bg.rgb);\n"
        "    frag_color = vec4(bg_lin, v_bg.a);\n"
        "    return;\n"
        "  }\n"
        "  vec4 texel = texture(atlas, v_uv);\n"
        "  float coverage = texel.a;\n"
        "  if (coverage <= 0.0) discard;\n"
        "  vec3 fg_lin = srgb_to_linear(v_fg.rgb);\n"
        "  vec3 bg_lin = srgb_to_linear(v_bg.rgb);\n"
        "  vec3 color = mix(bg_lin, fg_lin, coverage);\n"
        "  frag_color = vec4(color, v_bg.a);\n"
        "}\n";

    sg_shader shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func.source = vs_src,
        .fragment_func.source = fs_src,
        .attrs[0].glsl_name = "pos",
        .attrs[1].glsl_name = "uv",
        .attrs[2].glsl_name = "fg",
        .attrs[3].glsl_name = "bg",
        .uniform_blocks[0] = {
            .stage = SG_SHADERSTAGE_VERTEX,
            .size = sizeof(float) * 4,
            .glsl_uniforms = {
                [0] = { .glsl_name = "u_resolution", .type = SG_UNIFORMTYPE_FLOAT2 },
                [1] = { .glsl_name = "u_cell_size", .type = SG_UNIFORMTYPE_FLOAT2 },
            },
        },
        .views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT,
        .views[0].texture.image_type = SG_IMAGETYPE_2D,
        .views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT,
        .samplers[0].stage = SG_SHADERSTAGE_FRAGMENT,
        .samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING,
        .texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT,
        .texture_sampler_pairs[0].view_slot = 0,
        .texture_sampler_pairs[0].sampler_slot = 0,
        .texture_sampler_pairs[0].glsl_name = "atlas",
        .label = "sokol-glyph-shader",
    });

    data->glyph_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .size = SOKOL_MAX_VERTICES * sizeof(GlyphVertex),
        .usage.dynamic_update = true,
        .label = "sokol-glyph-vbuf",
    });

    data->glyph_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .layout = {
            .buffers[0].stride = sizeof(GlyphVertex),
            .attrs = {
                [0] = { .offset = offsetof(GlyphVertex, x), .format = SG_VERTEXFORMAT_FLOAT2 },
                [1] = { .offset = offsetof(GlyphVertex, u), .format = SG_VERTEXFORMAT_FLOAT2 },
                [2] = { .offset = offsetof(GlyphVertex, fg), .format = SG_VERTEXFORMAT_UBYTE4N },
                [3] = { .offset = offsetof(GlyphVertex, bg), .format = SG_VERTEXFORMAT_UBYTE4N },
            },
        },
        .colors[0] = { .pixel_format = data->linear_ok ? SG_PIXELFORMAT_SRGB8A8 : SG_PIXELFORMAT_RGBA8 },
        .label = "sokol-glyph-pipeline",
    });
    data->glyph_pip_created = true;

    // Selection overlay pipeline: same shader, alpha-blended on top.
    data->sel_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .layout = {
            .buffers[0].stride = sizeof(GlyphVertex),
            .attrs = {
                [0] = { .offset = offsetof(GlyphVertex, x), .format = SG_VERTEXFORMAT_FLOAT2 },
                [1] = { .offset = offsetof(GlyphVertex, u), .format = SG_VERTEXFORMAT_FLOAT2 },
                [2] = { .offset = offsetof(GlyphVertex, fg), .format = SG_VERTEXFORMAT_UBYTE4N },
                [3] = { .offset = offsetof(GlyphVertex, bg), .format = SG_VERTEXFORMAT_UBYTE4N },
            },
        },
        .colors[0] = {
            .pixel_format = data->linear_ok ? SG_PIXELFORMAT_SRGB8A8 : SG_PIXELFORMAT_RGBA8,
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .src_factor_alpha = SG_BLENDFACTOR_ONE,
                .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            },
        },
        .label = "sokol-selection-pipeline",
    });
    data->sel_pip_created = true;
}

// ── Initialization/Cleanup ────────────────────────────────────────────────

void rend_sokol_init(RendererSokolData *data)
{
    memset(data, 0, sizeof(*data));
    data->cell_w = 10;
    data->cell_h = 20;
    data->content_scale = 1.0f;
    data->linear_ok = true;
    rend_fallback_init(&data->fallback);
}

void rend_sokol_destroy(RendererSokolData *data)
{
    // Free panel terminals
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        if (data->panel_terms[i]) {
            terminal_destroy(data->panel_terms[i]);
            free(data->panel_terms[i]);
            data->panel_terms[i] = NULL;
        }
    }

    // Free font resources
    if (data->font) {
        font_destroy(data->font);
        data->font = NULL;
    }
    if (data->resolve) {
        font_resolve_destroy(data->resolve);
        data->resolve = NULL;
    }
    free(data->font_path);
    data->font_path = NULL;

    // Free atlas
    rend_sokol_atlas_destroy(&data->atlas);

    // Free pipelines and buffers
    if (data->glyph_pip.id != SG_INVALID_ID)
        sg_destroy_pipeline(data->glyph_pip);
    if (data->glyph_vbuf.id != SG_INVALID_ID)
        sg_destroy_buffer(data->glyph_vbuf);
    if (data->sel_pip.id != SG_INVALID_ID)
        sg_destroy_pipeline(data->sel_pip);
    if (data->lottie_pip.id != SG_INVALID_ID)
        sg_destroy_pipeline(data->lottie_pip);
    if (data->lottie_vbuf.id != SG_INVALID_ID)
        sg_destroy_buffer(data->lottie_vbuf);
    if (data->lottie_sampler.id != SG_INVALID_ID)
        sg_destroy_sampler(data->lottie_sampler);
    if (data->sixel_vbuf.id != SG_INVALID_ID)
        sg_destroy_buffer(data->sixel_vbuf);

    // Free lottie/sixel caches
    for (int i = 0; i < data->lottie_cache_count; i++) {
        if (data->lottie_cache[i].image.id != SG_INVALID_ID)
            sg_destroy_image(data->lottie_cache[i].image);
        if (data->lottie_cache[i].view.id != SG_INVALID_ID)
            sg_destroy_view(data->lottie_cache[i].view);
    }
    for (int i = 0; i < data->sixel_cache_count; i++) {
        if (data->sixel_cache[i].image.id != SG_INVALID_ID)
            sg_destroy_image(data->sixel_cache[i].image);
        if (data->sixel_cache[i].view.id != SG_INVALID_ID)
            sg_destroy_view(data->sixel_cache[i].view);
    }
}

// ── Accessors ──────────────────────────────────────────────────────────────

bool rend_sokol_get_cell_size(RendererSokolData *data, int *cell_w, int *cell_h)
{
    if (cell_w)
        *cell_w = data->cell_w;
    if (cell_h)
        *cell_h = data->cell_h;
    return true;
}

void rend_sokol_set_content_scale(RendererSokolData *data, float scale)
{
    data->content_scale = scale;
}

void rend_sokol_scroll(RendererSokolData *data, TerminalBackend *term, int delta)
{
    (void)term;
    data->scroll.scroll_offset += delta;
    if (data->scroll.scroll_offset < 0)
        data->scroll.scroll_offset = 0;
}

void rend_sokol_reset_scroll(RendererSokolData *data)
{
    data->scroll.scroll_offset = 0;
}

int rend_sokol_get_scroll_offset(RendererSokolData *data)
{
    return data->scroll.scroll_offset;
}

void rend_sokol_set_overlay(RendererSokolData *data, TerminalBackend *overlay)
{
    data->scroll.overlay = overlay;
}

void rend_sokol_clear_overlay(RendererSokolData *data)
{
    data->scroll.overlay = NULL;
}

bool rend_sokol_has_overlay(RendererSokolData *data)
{
    return data->scroll.overlay != NULL;
}

// ── Debug utilities ────────────────────────────────────────────────────────

void rend_sokol_debug_dumpverts(int row, int col_start, int col_end)
{
    if (row < 0 || row >= SOKOL_MAX_ROWS)
        return;
    if (col_start < 0)
        col_start = 0;
    if (col_end < 0 || col_end >= SOKOL_MAX_COLS)
        col_end = SOKOL_MAX_COLS - 1;

    printf("=== dumpverts row=%d cols=%d..%d ===\n", row, col_start, col_end);
    for (int col = col_start; col <= col_end; col++) {
        int idx = s_vert_index[row][col];
        if (idx < 0 || idx + 5 >= SOKOL_MAX_VERTICES)
            continue;
        GlyphVertex *q = &s_frame_verts[idx];
        printf("col %d: pos=(%.1f,%.1f) uv=(%.2f,%.2f) fg=(%d,%d,%d,%d) bg=(%d,%d,%d,%d)\n",
               col, q->x, q->y, q->u, q->v,
               q->fg[0], q->fg[1], q->fg[2], q->fg[3],
               q->bg[0], q->bg[1], q->bg[2], q->bg[3]);
    }
}

// ── Decoration rendering ───────────────────────────────────────────────────

void rend_sokol_deco_reset(void)
{
    s_deco_vert_count = 0;
    s_deco_overflow_warned = false;
}

int rend_sokol_deco_get_count(void)
{
    return s_deco_vert_count;
}

GlyphVertex *rend_sokol_deco_get_verts(void)
{
    return s_deco_verts;
}

void rend_sokol_deco_emit_quad(float x0, float y0, float x1, float y1,
                               const uint8_t color[4])
{
    if (s_deco_vert_count + 6 > SOKOL_MAX_DECO_VERTICES) {
        if (!s_deco_overflow_warned) {
            fprintf(stderr, "decoration vertex buffer exhausted; skipping remaining decorations\n");
            s_deco_overflow_warned = true;
        }
        return;
    }
    GlyphVertex *q = &s_deco_verts[s_deco_vert_count];
    float u = 2.0f;
    q[0] = (GlyphVertex){ x0, y0, u, 0.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[1] = (GlyphVertex){ x1, y0, u, 0.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[2] = (GlyphVertex){ x1, y1, u, 1.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[3] = (GlyphVertex){ x0, y0, u, 0.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[4] = (GlyphVertex){ x1, y1, u, 1.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[5] = (GlyphVertex){ x0, y1, u, 1.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    s_deco_vert_count += 6;
}

void rend_sokol_deco_strip_emit(float x0, float x1, int y, uint8_t alpha,
                                const uint8_t color[4])
{
    if (alpha == 0)
        return;
    uint8_t c[4] = { color[0], color[1], color[2], alpha };
    rend_sokol_deco_emit_quad(x0, (float)y, x1, (float)(y + 1), c);
}

void rend_sokol_deco_coalesce_update(float x, float y_min, float y_max,
                                     const uint8_t *alphas, DecoStrip *strips,
                                     int *strip_count, int max_strips,
                                     const uint8_t color[4])
{
    int new_count = 0;
    int new_y[8];
    uint8_t new_alpha[8];
    for (int y = (int)y_min; y <= (int)y_max && new_count < 8; y++) {
        uint8_t a = alphas[y - (int)y_min];
        if (a > 0) {
            new_y[new_count] = y;
            new_alpha[new_count] = a;
            new_count++;
        }
    }

    for (int i = 0; i < *strip_count; i++) {
        DecoStrip *s = &strips[i];
        bool found = false;
        uint8_t alpha = 0;
        for (int j = 0; j < new_count; j++) {
            if (new_y[j] == s->y) {
                found = true;
                alpha = new_alpha[j];
                break;
            }
        }
        if (!found || alpha != s->alpha) {
            rend_sokol_deco_strip_emit(s->x_start, x, s->y, s->alpha, color);
            memmove(&strips[i], &strips[i + 1],
                    (size_t)(*strip_count - i - 1) * sizeof(DecoStrip));
            (*strip_count)--;
            i--;
        }
    }

    for (int i = 0; i < new_count; i++) {
        bool found = false;
        for (int j = 0; j < *strip_count; j++) {
            if (strips[j].y == new_y[i] &&
                strips[j].alpha == new_alpha[i]) {
                strips[j].x_end = x;
                found = true;
                break;
            }
        }
        if (!found && *strip_count < max_strips) {
            int idx = *strip_count;
            strips[idx].y = new_y[i];
            strips[idx].x_start = x;
            strips[idx].x_end = x + 1.0f;
            strips[idx].alpha = new_alpha[i];
            (*strip_count)++;
        }
    }
}

void rend_sokol_deco_coalesce_flush(DecoStrip *strips, int *strip_count,
                                    const uint8_t color[4])
{
    for (int i = 0; i < *strip_count; i++) {
        DecoStrip *s = &strips[i];
        rend_sokol_deco_strip_emit(s->x_start, s->x_end, s->y, s->alpha, color);
    }
    *strip_count = 0;
}

int rend_sokol_underline_position(RendererSokolData *data, int row)
{
    int cell_y = row * data->cell_h;
    int underline_y = cell_y + data->font_ascent +
                      (int)roundf(2.0f * data->content_scale);
    int thickness = (int)roundf(1.0f * data->content_scale);
    if (thickness < 1)
        thickness = 1;
    if (underline_y + thickness > cell_y + data->cell_h)
        underline_y = cell_y + data->cell_h - thickness;
    return underline_y;
}

void rend_sokol_draw_underline_single(RendererSokolData *data, int row,
                                      int vis_start, int vis_end,
                                      const uint8_t color[4])
{
    int thickness = (int)roundf(1.0f * data->content_scale);
    if (thickness < 1)
        thickness = 1;
    int y = rend_sokol_underline_position(data, row);
    float x0 = (float)(vis_start * data->cell_w);
    float x1 = (float)(vis_end * data->cell_w);
    rend_sokol_deco_emit_quad(x0, (float)y, x1, (float)(y + thickness), color);
}

void rend_sokol_draw_underline_double(RendererSokolData *data, int row,
                                      int vis_start, int vis_end,
                                      const uint8_t color[4])
{
    int thickness = (int)roundf(1.0f * data->content_scale);
    if (thickness < 1)
        thickness = 1;
    int gap = (int)roundf(1.0f * data->content_scale);
    if (gap < 1)
        gap = 1;
    int y1 = rend_sokol_underline_position(data, row);
    int y2 = y1 + thickness + gap;
    float x0 = (float)(vis_start * data->cell_w);
    float x1 = (float)(vis_end * data->cell_w);
    rend_sokol_deco_emit_quad(x0, (float)y1, x1, (float)(y1 + thickness), color);
    rend_sokol_deco_emit_quad(x0, (float)y2, x1, (float)(y2 + thickness), color);
}

void rend_sokol_draw_underline_curly(RendererSokolData *data, int row,
                                     int vis_start, int vis_end,
                                     const uint8_t color[4])
{
    float pd = data->content_scale;
    float amplitude = 1.5f * pd;
    if (amplitude < 1.0f)
        amplitude = 1.0f;
    float wavelength = 8.0f * pd;
    if (wavelength < 4.0f)
        wavelength = 4.0f;
    float thickness = 0.5f * pd;
    if (thickness < 0.5f)
        thickness = 0.5f;
    int underline_y = rend_sokol_underline_position(data, row);
    float center_y = (float)underline_y + amplitude;
    int run_x = vis_start * data->cell_w;
    int run_w = (vis_end - vis_start) * data->cell_w;

    DecoStrip strips[16];
    int strip_count = 0;
    for (int px = 0; px < run_w; px++) {
        float x = (float)(run_x + px);
        float sine_y = center_y +
                       amplitude *
                           sinf((float)px / wavelength * 2.0f * (float)M_PI);
        int y_min = (int)floorf(sine_y - thickness - 1.0f);
        int y_max = (int)ceilf(sine_y + thickness + 1.0f);
        uint8_t alphas[8] = { 0 };
        for (int y = y_min, n = 0; y <= y_max && n < 8; y++, n++) {
            float dist = fabsf((float)y + 0.5f - sine_y);
            if (dist <= thickness) {
                alphas[n] = 255;
            } else if (dist <= thickness + 1.0f) {
                alphas[n] = (uint8_t)roundf(255.0f * (1.0f - (dist - thickness)));
            } else {
                alphas[n] = 0;
            }
        }
        rend_sokol_deco_coalesce_update(x, (float)y_min, (float)y_max, alphas,
                                        strips, &strip_count, 16, color);
    }
    rend_sokol_deco_coalesce_flush(strips, &strip_count, color);
}

void rend_sokol_draw_underline_dotted(RendererSokolData *data, int row,
                                      int vis_start, int vis_end,
                                      const uint8_t color[4])
{
    float pd = data->content_scale;
    float radius = 0.5f * pd;
    if (radius < 0.5f)
        radius = 0.5f;
    float gap = roundf(2.0f * pd);
    if (gap < 2.0f)
        gap = 2.0f;
    float stride = radius * 2.0f + gap;
    int underline_y = rend_sokol_underline_position(data, row);
    int run_x = vis_start * data->cell_w;
    int run_w = (vis_end - vis_start) * data->cell_w;

    for (float cx = (float)run_x; cx < (float)(run_x + run_w); cx += stride) {
        float cy = (float)underline_y + radius;
        int x_min = (int)floorf(cx - radius - 1.0f);
        int x_max = (int)ceilf(cx + radius + 1.0f);
        int y_min = (int)floorf(cy - radius - 1.0f);
        int y_max = (int)ceilf(cy + radius + 1.0f);
        for (int y = y_min; y <= y_max; y++) {
            for (int x = x_min; x <= x_max; x++) {
                float dx = (float)x + 0.5f - cx;
                float dy = (float)y + 0.5f - cy;
                float dist = sqrtf(dx * dx + dy * dy);
                uint8_t alpha;
                if (dist <= radius)
                    alpha = 255;
                else if (dist <= radius + 1.0f)
                    alpha = (uint8_t)roundf(255.0f * (1.0f - (dist - radius)));
                else
                    alpha = 0;
                if (alpha > 0)
                    rend_sokol_deco_strip_emit((float)x, (float)(x + 1), y, alpha,
                                               color);
            }
        }
    }
}

void rend_sokol_draw_underline_dashed(RendererSokolData *data, int row,
                                      int vis_start, int vis_end,
                                      const uint8_t color[4])
{
    float pd = data->content_scale;
    int thickness = (int)roundf(1.0f * pd);
    if (thickness < 1)
        thickness = 1;
    int dash_w = (int)roundf(3.0f * pd);
    if (dash_w < 1)
        dash_w = 1;
    int gap = (int)roundf(2.0f * pd);
    if (gap < 1)
        gap = 1;
    int stride = dash_w + gap;
    int y = rend_sokol_underline_position(data, row);
    int run_x = vis_start * data->cell_w;
    int run_w = (vis_end - vis_start) * data->cell_w;
    float y0 = (float)y;
    float y1 = (float)(y + thickness);
    for (int px = 0; px < run_w; px += stride) {
        int x0 = run_x + px;
        int w = dash_w;
        if (x0 + w > run_x + run_w)
            w = run_x + run_w - x0;
        if (w > 0)
            rend_sokol_deco_emit_quad((float)x0, y0, (float)(x0 + w), y1, color);
    }
}

void rend_sokol_draw_strikethrough(RendererSokolData *data, int row,
                                   int vis_start, int vis_end,
                                   const uint8_t color[4])
{
    float pd = data->content_scale;
    int thickness = (int)roundf(1.0f * pd);
    if (thickness < 1)
        thickness = 1;
    int cell_y = row * data->cell_h;
    int strike_y = cell_y + data->font_ascent - data->font_cap_height / 2;
    float x0 = (float)(vis_start * data->cell_w);
    float x1 = (float)(vis_end * data->cell_w);
    rend_sokol_deco_emit_quad(x0, (float)strike_y, x1,
                              (float)(strike_y + thickness), color);
}
