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
#include "unicode.h"
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
#define CURSOR_COLOR_A 0xFF

// Selection highlight color: blue-ish tint (RGBA) — matches backend_sokol.c
#define SELECTION_COLOR_R 0x5A
#define SELECTION_COLOR_G 0x60
#define SELECTION_COLOR_B 0x7A
#define SELECTION_COLOR_A 220

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

void rend_sokol_cell_color(TerminalColor tc, bool is_fg, bool reverse, uint8_t out[4])
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

// ── Cursor/Selection quad emission ────────────────────────────────────────

void rend_sokol_emit_cursor_quad(float x0, float y0, float x1, float y1)
{
    uint8_t cc[4] = { CURSOR_COLOR_R, CURSOR_COLOR_G, CURSOR_COLOR_B, CURSOR_COLOR_A };
    float cu0 = -(0.0f + 2.0f);
    float cu1 = -(1.0f + 2.0f);
    GlyphVertex *q = s_cursor_verts;
    rend_sokol_emit_glyph_quad(q, x0, y0, x1, y1, cu0, 0.0f, cu1, 1.0f, cc, cc);
    s_cursor_vert_count = 6;
}

void rend_sokol_emit_selection_quad(float x0, float y0, float x1, float y1,
                                    GlyphVertex *sel_verts, int *sel_vert_count)
{
    uint8_t sc[4] = { SELECTION_COLOR_R, SELECTION_COLOR_G, SELECTION_COLOR_B, SELECTION_COLOR_A };
    float bg_u = 2.0f;
    GlyphVertex *sq = &sel_verts[*sel_vert_count];
    rend_sokol_emit_glyph_quad(sq, x0, y0, x1, y1, bg_u, 0.0f, bg_u, 0.0f, sc, sc);
    *sel_vert_count += 6;
}

// ── Glyph quad emission helper ─────────────────────────────────────────────

void rend_sokol_emit_glyph_quad(GlyphVertex *q,
                                float x0, float y0, float x1, float y1,
                                float u0, float v0, float u1, float v1,
                                const uint8_t fg[4], const uint8_t bg[4])
{
    q[0] = (GlyphVertex){ x0, y0, u0, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[1] = (GlyphVertex){ x1, y0, u1, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[2] = (GlyphVertex){ x1, y1, u1, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[3] = (GlyphVertex){ x0, y0, u0, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[4] = (GlyphVertex){ x1, y1, u1, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[5] = (GlyphVertex){ x0, y1, u0, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
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
    rend_sokol_emit_glyph_quad(q, x0, y0, x1, y1, u, 0.0f, u, 1.0f, color, color);
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

// ── Lottie/sixel cache management ──────────────────────────────────────────

void rend_sokol_lottie_cache_reconcile(RendererSokolData *data,
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
            if (data->lottie_cache[i].image.id != SG_INVALID_ID)
                sg_destroy_image(data->lottie_cache[i].image);
            if (data->lottie_cache[i].view.id != SG_INVALID_ID)
                sg_destroy_view(data->lottie_cache[i].view);
            data->lottie_cache[i] = data->lottie_cache[--data->lottie_cache_count];
        }
    }
}

int rend_sokol_lottie_get_texture(RendererSokolData *data, const CfrLottie *anim)
{
    for (int i = 0; i < data->lottie_cache_count; i++) {
        if (data->lottie_cache[i].id != anim->id)
            continue;
        if (data->lottie_cache[i].w != anim->canvas_w ||
            data->lottie_cache[i].h != anim->canvas_h) {
            // Size changed — recreate
            if (data->lottie_cache[i].image.id != SG_INVALID_ID)
                sg_destroy_image(data->lottie_cache[i].image);
            if (data->lottie_cache[i].view.id != SG_INVALID_ID)
                sg_destroy_view(data->lottie_cache[i].view);
            data->lottie_cache[i].image.id = SG_INVALID_ID;
            data->lottie_cache[i].view.id = SG_INVALID_ID;
        } else if (data->lottie_cache[i].version != anim->version) {
            // Version changed — update pixels
            sg_update_image(data->lottie_cache[i].image, &(sg_image_data){
                                                             .mip_levels[0] = {
                                                                 .ptr = (void *)anim->rgba,
                                                                 .size = (size_t)anim->canvas_w * anim->canvas_h * 4,
                                                             },
                                                         });
            data->lottie_cache[i].version = anim->version;
            return i;
        } else {
            return i;
        }
        // Re-create image (size changed or first creation)
        data->lottie_cache[i].image = sg_make_image(&(sg_image_desc){
            .width = anim->canvas_w,
            .height = anim->canvas_h,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .usage.dynamic_update = true,
            .label = "sokol-lottie",
        });
        data->lottie_cache[i].view = sg_make_view(&(sg_view_desc){
            .texture.image = data->lottie_cache[i].image,
            .label = "sokol-lottie-view",
        });
        sg_update_image(data->lottie_cache[i].image, &(sg_image_data){
                                                         .mip_levels[0] = {
                                                             .ptr = (void *)anim->rgba,
                                                             .size = (size_t)anim->canvas_w * anim->canvas_h * 4,
                                                         },
                                                     });
        data->lottie_cache[i].version = anim->version;
        data->lottie_cache[i].w = anim->canvas_w;
        data->lottie_cache[i].h = anim->canvas_h;
        return i;
    }

    // New cache entry
    if (data->lottie_cache_count >= SOKOL_LOTTIE_CACHE_MAX)
        return -1;

    sg_image img = sg_make_image(&(sg_image_desc){
        .width = anim->canvas_w,
        .height = anim->canvas_h,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage.dynamic_update = true,
        .label = "sokol-lottie",
    });
    sg_update_image(img, &(sg_image_data){
                             .mip_levels[0] = {
                                 .ptr = (void *)anim->rgba,
                                 .size = (size_t)anim->canvas_w * anim->canvas_h * 4,
                             },
                         });
    sg_view view = sg_make_view(&(sg_view_desc){
        .texture.image = img,
        .label = "sokol-lottie-view",
    });
    int n = data->lottie_cache_count++;
    data->lottie_cache[n].image = img;
    data->lottie_cache[n].view = view;
    data->lottie_cache[n].id = anim->id;
    data->lottie_cache[n].version = anim->version;
    data->lottie_cache[n].w = anim->canvas_w;
    data->lottie_cache[n].h = anim->canvas_h;
    return n;
}

void rend_sokol_sixel_cache_reconcile(RendererSokolData *data,
                                      const CfrSixel *imgs, int count)
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
            if (data->sixel_cache[i].image.id != SG_INVALID_ID)
                sg_destroy_image(data->sixel_cache[i].image);
            if (data->sixel_cache[i].view.id != SG_INVALID_ID)
                sg_destroy_view(data->sixel_cache[i].view);
            data->sixel_cache[i] = data->sixel_cache[--data->sixel_cache_count];
        }
    }
}

int rend_sokol_sixel_get_texture(RendererSokolData *data, const CfrSixel *img)
{
    for (int i = 0; i < data->sixel_cache_count; i++) {
        if (data->sixel_cache[i].id != img->id)
            continue;
        if (data->sixel_cache[i].w != img->width_px ||
            data->sixel_cache[i].h != img->height_px) {
            if (data->sixel_cache[i].image.id != SG_INVALID_ID)
                sg_destroy_image(data->sixel_cache[i].image);
            if (data->sixel_cache[i].view.id != SG_INVALID_ID)
                sg_destroy_view(data->sixel_cache[i].view);
            data->sixel_cache[i].image.id = SG_INVALID_ID;
            data->sixel_cache[i].view.id = SG_INVALID_ID;
        } else if (data->sixel_cache[i].version != img->version) {
            sg_update_image(data->sixel_cache[i].image, &(sg_image_data){
                                                            .mip_levels[0] = {
                                                                .ptr = (void *)img->rgba,
                                                                .size = (size_t)img->width_px * img->height_px * 4,
                                                            },
                                                        });
            data->sixel_cache[i].version = img->version;
            return i;
        } else {
            return i;
        }
        data->sixel_cache[i].image = sg_make_image(&(sg_image_desc){
            .width = img->width_px,
            .height = img->height_px,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .usage.dynamic_update = true,
            .label = "sokol-sixel",
        });
        data->sixel_cache[i].view = sg_make_view(&(sg_view_desc){
            .texture.image = data->sixel_cache[i].image,
            .label = "sokol-sixel-view",
        });
        sg_update_image(data->sixel_cache[i].image, &(sg_image_data){
                                                        .mip_levels[0] = {
                                                            .ptr = (void *)img->rgba,
                                                            .size = (size_t)img->width_px * img->height_px * 4,
                                                        },
                                                    });
        data->sixel_cache[i].version = img->version;
        data->sixel_cache[i].w = img->width_px;
        data->sixel_cache[i].h = img->height_px;
        return i;
    }

    if (data->sixel_cache_count >= SOKOL_SIXEL_CACHE_MAX)
        return -1;

    sg_image img_obj = sg_make_image(&(sg_image_desc){
        .width = img->width_px,
        .height = img->height_px,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage.dynamic_update = true,
        .label = "sokol-sixel",
    });
    sg_update_image(img_obj, &(sg_image_data){
                                 .mip_levels[0] = {
                                     .ptr = (void *)img->rgba,
                                     .size = (size_t)img->width_px * img->height_px * 4,
                                 },
                             });
    sg_view view = sg_make_view(&(sg_view_desc){
        .texture.image = img_obj,
        .label = "sokol-sixel-view",
    });
    int n = data->sixel_cache_count++;
    data->sixel_cache[n].image = img_obj;
    data->sixel_cache[n].view = view;
    data->sixel_cache[n].id = img->id;
    data->sixel_cache[n].version = img->version;
    data->sixel_cache[n].w = img->width_px;
    data->sixel_cache[n].h = img->height_px;
    return n;
}
void rend_sokol_render_terminal_cells(RendererSokolData *data, TerminalBackend *term,
                                      int origin_x, int origin_y,
                                      bool cursor_visible, int scroll_offset,
                                      int *vert_count, int *glyph_vert_count,
                                      int *sel_vert_count,
                                      GlyphVertex *sel_verts)
{
    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);
    if (rows <= 0 || cols <= 0)
        return;

    int cell_w = data->cell_w;
    int cell_h = data->cell_h;
    if (cell_w <= 0 || cell_h <= 0)
        return;

    float atlas_size = (float)REND_ATLAS_TEXTURE_SIZE;
    bool track_index = true; // Track atlas usage for main terminal

    for (int row = 0; row < rows && *vert_count + 12 <= SOKOL_MAX_VERTICES; row++) {
        int unified_row = rend_display_row_to_unified(scroll_offset, row);
        for (int col = 0; col < cols && *vert_count + 12 <= SOKOL_MAX_VERTICES; col++) {
            TerminalCell cell;
            if (unified_row < 0) {
                if (terminal_get_scrollback_cell(term, -unified_row - 1, col, &cell) != 0)
                    continue;
            } else {
                if (terminal_get_cell(term, unified_row, col, &cell) != 0)
                    continue;
            }
            if (cell.width == 0) {
                if (cursor_visible && scroll_offset == 0 &&
                    terminal_get_cursor_visible(term)) {
                    TerminalPos cp = terminal_get_cursor_pos(term);
                    if (cp.row == row && cp.col == col) {
                        float cx0 = (float)(origin_x + col * cell_w);
                        float cy0 = (float)(origin_y + row * cell_h);
                        float cx1 = cx0 + (float)cell_w;
                        float cy1 = cy0 + (float)cell_h;
                        rend_sokol_emit_cursor_quad(cx0, cy0, cx1, cy1);
                    }
                }
                continue;
            }

            uint8_t fg[4], bg[4];
            bool rev = cell.attrs.reverse;
            rend_sokol_cell_color(cell.fg, true, rev, fg);
            rend_sokol_cell_color(cell.bg, false, rev, bg);

            // Dim/faint (SGR 2): blend foreground toward background at 40% opacity
            if (cell.attrs.dim) {
                fg[0] = (uint8_t)(fg[0] * 0.4f + bg[0] * 0.6f);
                fg[1] = (uint8_t)(fg[1] * 0.4f + bg[1] * 0.6f);
                fg[2] = (uint8_t)(fg[2] * 0.4f + bg[2] * 0.6f);
            }

            bool in_sel = terminal_cell_in_selection(term, unified_row, col);

            bool is_cursor = false;
            if (cursor_visible && scroll_offset == 0 &&
                terminal_get_cursor_visible(term)) {
                TerminalPos cpos = terminal_get_cursor_pos(term);
                if (cpos.row == row && cpos.col == col) {
                    is_cursor = true;
                }
            }

            float cell_x0 = (float)(origin_x + col * cell_w);
            float cell_y0 = (float)(origin_y + row * cell_h);
            float cell_x1 = cell_x0 + (float)(cell.width * cell_w);
            float cell_y1 = cell_y0 + (float)cell_h;

            float bg_u = 2.0f;
            float bg_v = 0.0f;
            if (track_index)
                rend_sokol_get_vert_index()[row * SOKOL_MAX_COLS + col] = *vert_count;
            GlyphVertex *q = &rend_sokol_get_frame_verts()[*vert_count];
            rend_sokol_emit_glyph_quad(q, cell_x0, cell_y0, cell_x1, cell_y1,
                                       bg_u, bg_v, bg_u, bg_v, fg, bg);
            *vert_count += 6;

            if (is_cursor) {
                rend_sokol_emit_cursor_quad(cell_x0, cell_y0, cell_x1, cell_y1);
            }

            if (cell.cp != 0 && cell.cp != 0x20 && !cell.attrs.invis) {
                if (rend_boxdraw_is_supported(cell.cp)) {
                    uint32_t bd_cp = cell.cp;
                    uint32_t color_key = 0;

                    RendSokolAtlasEntry *bd_entry = rend_sokol_atlas_lookup(
                        &data->atlas, BOXDRAW_FONT_DATA, (int)bd_cp, color_key);

                    if (!bd_entry) {
                        GlyphBitmap *bmp = rend_boxdraw_render(
                            bd_cp, cell_w, cell_h, fg[0], fg[1], fg[2]);
                        if (bmp) {
                            bd_entry = rend_sokol_atlas_insert(
                                &data->atlas, BOXDRAW_FONT_DATA,
                                (int)bd_cp, color_key, bmp, false);
                            free(bmp->pixels);
                            free(bmp);
                        } else {
                            bd_entry = rend_sokol_atlas_insert_empty(
                                &data->atlas, BOXDRAW_FONT_DATA,
                                (int)bd_cp, color_key);
                        }
                    }

                    if (bd_entry && bd_entry->region.w > 0 &&
                        bd_entry->region.h > 0 &&
                        *glyph_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                        float gx0, gy0, gx1, gy1;
                        if (bd_entry->centered) {
                            int glyph_w = cell.width * cell_w;
                            // For padded bitmaps (diagonals with region > cell), extend
                            // beyond cell bounds so overhang fills row-boundary gaps.
                            // For normal centered glyphs (region <= cell), center within.
                            int pad_x = (bd_entry->region.w - glyph_w) / 2;
                            int pad_y = (bd_entry->region.h - cell_h) / 2;
                            if (pad_x > 0 || pad_y > 0) {
                                // Bitmap has padding - extend beyond cell bounds
                                gx0 = (float)cell_x0 - (float)pad_x;
                                gy0 = (float)cell_y0 - (float)pad_y;
                            } else {
                                // Normal centered glyph (emoji, symbol) - center within cell
                                gx0 = (float)cell_x0 +
                                      ((float)glyph_w - (float)bd_entry->region.w) * 0.5f;
                                gy0 = (float)cell_y0 +
                                      ((float)cell_h - (float)bd_entry->region.h) * 0.5f;
                            }
                        } else {
                            gx0 = (float)cell_x0;
                            gy0 = (float)cell_y0;
                        }
                        gx1 = gx0 + (float)bd_entry->region.w;
                        gy1 = gy0 + (float)bd_entry->region.h;

                        float u0 = (float)bd_entry->region.x / atlas_size;
                        float v0 = (float)bd_entry->region.y / atlas_size;
                        float u1 = (float)(bd_entry->region.x + bd_entry->region.w) / atlas_size;
                        float v1 = (float)(bd_entry->region.y + bd_entry->region.h) / atlas_size;

                        q = &rend_sokol_get_glyph_verts()[*glyph_vert_count];
                        rend_sokol_emit_glyph_quad(q, gx0, gy0, gx1, gy1,
                                                   u0, v0, u1, v1, fg, bg);
                        *glyph_vert_count += 6;
                    }
                    goto selection_check;
                }

                if (!data->font)
                    goto selection_check;

                FontStyle style = FONT_STYLE_NORMAL;
                if (cell.attrs.bold && cell.attrs.italic)
                    style = FONT_STYLE_BOLD_ITALIC;
                else if (cell.attrs.bold)
                    style = FONT_STYLE_BOLD;
                else if (cell.attrs.italic)
                    style = FONT_STYLE_ITALIC;

                if (!font_has_style(data->font, style))
                    style = FONT_STYLE_NORMAL;

                uint32_t cps[32];
                int cp_count;
                if (cell.grapheme_id == 0) {
                    cps[0] = cell.cp;
                    cp_count = 1;
                } else {
                    size_t n = terminal_cell_get_grapheme(term, unified_row, col,
                                                          cps, 32);
                    if (n == 0) {
                        cps[0] = cell.cp;
                        n = 1;
                    }
                    cp_count = (int)n;
                }
                bool emoji_available = font_has_style(data->font, FONT_STYLE_EMOJI);
                bool emoji_has_glyph = emoji_available &&
                                       font_get_glyph_index(data->font, FONT_STYLE_EMOJI, cell.cp) != 0;
                if (rend_should_use_emoji(cps, cp_count, emoji_available, emoji_has_glyph))
                    style = FONT_STYLE_EMOJI;

                int avail_w = cell.width * cell_w;
                int avail_h = cell_h;

                for (int s = 0; s < FONT_STYLE_COUNT; s++)
                    font_set_presentation_width(data->font, s, avail_w);

                if (style == FONT_STYLE_EMOJI && avail_h < avail_w)
                    avail_w = avail_h;

                bool color_baked = rend_is_color_font(data->font, style);
                uint8_t render_r = color_baked ? fg[0] : 255;
                uint8_t render_g = color_baked ? fg[1] : 255;
                uint8_t render_b = color_baked ? fg[2] : 255;
                uint32_t color_key = color_baked
                                         ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                         : 0xFFFFFF;

                bool emoji_render = (style == FONT_STYLE_EMOJI);
                bool symbol_cell = rend_is_symbol_cell_cp(cell.cp);
                bool downscale_glyph = (emoji_render && color_baked) || symbol_cell;
                bool height_only_fit = symbol_cell && !color_baked;

                int cache_w = avail_w;
                int cache_h = avail_h;
                bool is_regional = is_regional_indicator(cell.cp);
                if (is_regional) {
                    int side = avail_w < avail_h ? avail_w : avail_h;
                    cache_w = cache_h = side;
                }

                if (cp_count > 1 && data->font->render_shaped) {
                    void *sh_font_data = data->font->font_data[style];
                    ShapedGlyphs *shaped = font_render_shaped_text(
                        data->font, style, cps, cp_count,
                        render_r, render_g, render_b);

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

                    if (!shaped && style != FONT_STYLE_NORMAL) {
                        style = FONT_STYLE_NORMAL;
                        sh_font_data = data->font->font_data[style];
                        color_baked = rend_is_color_font(data->font, style);
                        render_r = color_baked ? fg[0] : 255;
                        render_g = color_baked ? fg[1] : 255;
                        render_b = color_baked ? fg[2] : 255;
                        color_key = color_baked
                                        ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                        : 0xFFFFFF;
                        shaped = font_render_shaped_text(
                            data->font, style, cps, cp_count,
                            render_r, render_g, render_b);
                    }

                    if (!shaped && cp_count > 0) {
                        const char *fb_path = rend_fallback_lookup(&data->fallback, data->resolve, cps[0]);
                        if (fb_path && rend_fallback_ensure(&data->fallback, data->font, fb_path,
                                                            data->font_size, &data->font_options, data->cell_w)) {
                            style = FONT_STYLE_FALLBACK;
                            sh_font_data = data->font->font_data[style];
                            font_set_presentation_width(data->font, style, avail_w);
                            color_baked = rend_is_color_font(data->font, style);
                            render_r = color_baked ? fg[0] : 255;
                            render_g = color_baked ? fg[1] : 255;
                            render_b = color_baked ? fg[2] : 255;
                            color_key = color_baked
                                            ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                            : 0xFFFFFF;
                            shaped = font_render_shaped_text(
                                data->font, style, cps, cp_count,
                                render_r, render_g, render_b);
                        }
                    }

                    if (shaped) {
                        for (int gi = 0; gi < shaped->num_glyphs; gi++) {
                            uint32_t gid = shaped->glyph_ids[gi];
                            if (gid == 0)
                                continue;
                            uint32_t atlas_gid = (cell.width >= 2) ? (gid | (1u << 29)) : gid;
                            RendSokolAtlasEntry *entry = rend_sokol_atlas_lookup(
                                &data->atlas, sh_font_data, (int)atlas_gid, color_key);
                            if (!entry) {
                                GlyphBitmap *gb = font_render_glyph_id(
                                    data->font, style, gid,
                                    render_r, render_g, render_b);
                                if (gb) {
                                    GlyphBitmap *scaled = NULL;
                                    if (downscale_glyph) {
                                        scaled = rend_downscale_bitmap(gb, cache_w, cache_h, height_only_fit);
                                        bool centered = !height_only_fit;
                                        gb->centered = centered;
                                        if (scaled)
                                            scaled->centered = centered;
                                        if (height_only_fit) {
                                            int eff_w = scaled ? scaled->width : gb->width;
                                            int x_off = (cache_w - eff_w) / 2;
                                            gb->x_offset = x_off;
                                            if (scaled)
                                                scaled->x_offset = x_off;
                                        }
                                    }
                                    uint32_t insert_id = atlas_gid ? atlas_gid
                                                                   : (uint32_t)gb->glyph_id;
                                    entry = rend_sokol_atlas_insert(
                                        &data->atlas, sh_font_data, (int)insert_id, color_key,
                                        scaled ? scaled : gb, color_baked);
                                    if (scaled) {
                                        free(scaled->pixels);
                                        free(scaled);
                                    }
                                    data->font->free_glyph_bitmap(data->font, gb);
                                } else if (atlas_gid != 0) {
                                    entry = rend_sokol_atlas_insert_empty(
                                        &data->atlas, sh_font_data, (int)atlas_gid, color_key);
                                }
                            }

                            if (entry && entry->region.w > 0 && entry->region.h > 0 &&
                                *glyph_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                                int gx = (int)cell_x0 + shaped->x_positions[gi] + entry->x_offset;
                                int gy = (int)cell_y0 + data->font_ascent - entry->y_offset;
                                if (entry->centered) {
                                    int glyph_w = cell.width * cell_w;
                                    gx = (int)floorf(cell_x0 + ((float)glyph_w - (float)entry->region.w) * 0.5f);
                                    gy = (int)floorf(cell_y0 + ((float)cell_h - (float)entry->region.h) * 0.5f);
                                }

                                float gx0 = (float)gx;
                                float gy0 = (float)gy;
                                float gx1 = gx0 + (float)entry->region.w;
                                float gy1 = gy0 + (float)entry->region.h;

                                float u0 = (float)entry->region.x / atlas_size;
                                float v0 = (float)entry->region.y / atlas_size;
                                float u1 = (float)(entry->region.x + entry->region.w) / atlas_size;
                                float v1 = (float)(entry->region.y + entry->region.h) / atlas_size;

                                float u_off = color_baked ? 3.0f : 0.0f;

                                q = &rend_sokol_get_glyph_verts()[*glyph_vert_count];
                                rend_sokol_emit_glyph_quad(q, gx0, gy0, gx1, gy1,
                                                           u0 + u_off, v0, u1 + u_off, v1, fg, bg);
                                *glyph_vert_count += 6;
                            }
                        }
                        free(shaped->glyph_ids);
                        free(shaped->x_positions);
                        free(shaped->y_positions);
                        free(shaped->x_advances);
                        free(shaped);
                        goto selection_check;
                    }
                }

                uint32_t glyph_id = font_get_glyph_index(data->font, style, cell.cp);

                void *font_data = data->font->font_data[style];
                if (glyph_id == 0 && style != FONT_STYLE_NORMAL) {
                    style = FONT_STYLE_NORMAL;
                    font_data = data->font->font_data[style];
                    color_baked = rend_is_color_font(data->font, style);
                    render_r = color_baked ? fg[0] : 255;
                    render_g = color_baked ? fg[1] : 255;
                    render_b = color_baked ? fg[2] : 255;
                    color_key = color_baked
                                    ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                    : 0xFFFFFF;
                    glyph_id = font_get_glyph_index(data->font, style, cell.cp);
                }

                if (glyph_id == 0) {
                    const char *fb_path = rend_fallback_lookup(&data->fallback, data->resolve, cell.cp);
                    if (fb_path && rend_fallback_ensure(&data->fallback, data->font, fb_path,
                                                        data->font_size, &data->font_options, data->cell_w)) {
                        style = FONT_STYLE_FALLBACK;
                        font_data = data->font->font_data[style];
                        font_set_presentation_width(data->font, style, avail_w);
                        color_baked = rend_is_color_font(data->font, style);
                        render_r = color_baked ? fg[0] : 255;
                        render_g = color_baked ? fg[1] : 255;
                        render_b = color_baked ? fg[2] : 255;
                        color_key = color_baked
                                        ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                        : 0xFFFFFF;
                        glyph_id = font_get_glyph_index(data->font, style, cell.cp);
                    }
                }

                uint32_t atlas_glyph_id = glyph_id;
                if (cell.width >= 2 && atlas_glyph_id != 0)
                    atlas_glyph_id |= (1u << 29);

                RendSokolAtlasEntry *entry = NULL;
                if (atlas_glyph_id != 0)
                    entry = rend_sokol_atlas_lookup(
                        &data->atlas, font_data, (int)atlas_glyph_id, color_key);

                if (!entry) {
                    GlyphBitmap *bmp = font_render_glyphs(
                        data->font, style, &cell.cp, 1, render_r, render_g, render_b);
                    if (bmp) {
                        GlyphBitmap *scaled = NULL;
                        if (downscale_glyph) {
                            scaled = rend_downscale_bitmap(bmp, cache_w, cache_h, height_only_fit);
                            bool centered = !height_only_fit;
                            bmp->centered = centered;
                            if (scaled)
                                scaled->centered = centered;
                            if (height_only_fit) {
                                int eff_w = scaled ? scaled->width : bmp->width;
                                int x_off = (cache_w - eff_w) / 2;
                                bmp->x_offset = x_off;
                                if (scaled)
                                    scaled->x_offset = x_off;
                            }
                        }
                        uint32_t insert_id = atlas_glyph_id ? atlas_glyph_id
                                                            : (uint32_t)bmp->glyph_id;
                        entry = rend_sokol_atlas_insert(
                            &data->atlas, font_data, (int)insert_id, color_key,
                            scaled ? scaled : bmp, color_baked);
                        if (scaled) {
                            free(scaled->pixels);
                            free(scaled);
                        }
                        data->font->free_glyph_bitmap(data->font, bmp);
                    } else if (atlas_glyph_id != 0) {
                        entry = rend_sokol_atlas_insert_empty(
                            &data->atlas, font_data, (int)atlas_glyph_id, color_key);
                    }
                }

                if (entry && entry->region.w > 0 && entry->region.h > 0 &&
                    *glyph_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                    int gx = (int)cell_x0 + entry->x_offset;
                    int gy = (int)cell_y0 + data->font_ascent - entry->y_offset;
                    if (entry->centered) {
                        int glyph_w = cell.width * cell_w;
                        gx = (int)floorf(cell_x0 + ((float)glyph_w - (float)entry->region.w) * 0.5f);
                        gy = (int)floorf(cell_y0 + ((float)cell_h - (float)entry->region.h) * 0.5f);
                    }

                    float gx0 = (float)gx;
                    float gy0 = (float)gy;
                    float gx1 = gx0 + (float)entry->region.w;
                    float gy1 = gy0 + (float)entry->region.h;

                    float u0 = (float)entry->region.x / atlas_size;
                    float v0 = (float)entry->region.y / atlas_size;
                    float u1 = (float)(entry->region.x + entry->region.w) / atlas_size;
                    float v1 = (float)(entry->region.y + entry->region.h) / atlas_size;

                    float u_off = color_baked ? 3.0f : 0.0f;

                    q = &rend_sokol_get_glyph_verts()[*glyph_vert_count];
                    rend_sokol_emit_glyph_quad(q, gx0, gy0, gx1, gy1,
                                               u0 + u_off, v0, u1 + u_off, v1, fg, bg);
                    *glyph_vert_count += 6;
                }
            }

        selection_check:
            if (in_sel && *sel_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                rend_sokol_emit_selection_quad(cell_x0, cell_y0, cell_x1, cell_y1,
                                               sel_verts, sel_vert_count);
            }
        }
    }
}

// ── Lottie rendering ───────────────────────────────────────────────────────

int rend_sokol_render_lottie_layer(RendererSokolData *data, TerminalBackend *term,
                                   int win_w, int win_h,
                                   uint8_t target_layer, int vert_offset)
{
    int anim_count = 0;
    const CfrLottie *anims = terminal_get_lotties(term, &anim_count);
    if (anim_count == 0)
        return 0;

    int cell_w = data->cell_w;
    int cell_h = data->cell_h;
    float scale = data->content_scale > 0.0f ? data->content_scale : 1.0f;
    int scroll_offset = data->scroll.scroll_offset;
    float uniforms[2] = { (float)win_w, (float)win_h };

    int vert_base = vert_offset;

    for (int i = 0; i < anim_count; i++) {
        const CfrLottie *anim = &anims[i];
        int pl_count = 0;
        const CfrLottiePlacement *pls =
            terminal_get_lottie_placements(term, anim->id, &pl_count);

        int scaled_canvas_w = logical_to_physical(anim->canvas_w, scale);
        int scaled_canvas_h = logical_to_physical(anim->canvas_h, scale);
        int anim_vert_count = 0;

        for (int j = 0; j < pl_count; j++) {
            const CfrLottiePlacement *pl = &pls[j];
            if (pl->layer != target_layer)
                continue;

            int screen_row = pl->row + scroll_offset;
            int px = pl->col * cell_w;
            int py = screen_row * cell_h;
            int box_w = pl->cols * cell_w;
            int box_h = pl->rows * cell_h;

            if (py + box_h <= 0 || py >= win_h)
                continue;
            if (px + box_w <= 0 || px >= win_w)
                continue;
            if (vert_base + anim_vert_count + 6 > SOKOL_MAX_LOTTIE_VERTICES)
                break;

            int off_x = (box_w - scaled_canvas_w) / 2;
            int off_y = (box_h - scaled_canvas_h) / 2;
            float x0 = (float)(px + off_x);
            float y0 = (float)(py + off_y);
            float x1 = x0 + (float)scaled_canvas_w;
            float y1 = y0 + (float)scaled_canvas_h;

            uint8_t op = (uint8_t)pl->opacity_x256;
            uint8_t op_color[4] = { op, op, op, op };
            uint8_t zero[4] = { 0, 0, 0, 0 };

            GlyphVertex *q = &rend_sokol_get_lottie_verts()[vert_base + anim_vert_count];
            rend_sokol_emit_glyph_quad(q, x0, y0, x1, y1,
                                       0.0f, 0.0f, 1.0f, 1.0f, op_color, zero);
            anim_vert_count += 6;
        }

        if (anim_vert_count > 0) {
            int cache_idx = rend_sokol_lottie_get_texture(data, anim);
            if (cache_idx >= 0) {
                sg_apply_pipeline(data->lottie_pip);
                sg_apply_bindings(&(sg_bindings){
                    .vertex_buffers[0] = data->lottie_vbuf,
                    .views[0] = data->lottie_cache[cache_idx].view,
                    .samplers[0] = data->lottie_sampler,
                });
                sg_apply_uniforms(0, &SG_RANGE(uniforms));
                sg_draw(vert_base, anim_vert_count, 1);
            }
            vert_base += anim_vert_count;
        }
    }

    return vert_base - vert_offset;
}

// ── Sixel rendering ─────────────────────────────────────────────────────────

static GlyphVertex s_sixel_verts[SOKOL_MAX_SIXEL_VERTICES];

void rend_sokol_ensure_sixel_vbuf(RendererSokolData *data)
{
    if (data->sixel_vbuf_created)
        return;
    data->sixel_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .size = SOKOL_MAX_SIXEL_VERTICES * sizeof(GlyphVertex),
        .usage.dynamic_update = true,
        .label = "sokol-sixel-vbuf",
    });
    data->sixel_vbuf_created = true;
}

void rend_sokol_render_sixel_images(RendererSokolData *data, TerminalBackend *term,
                                    int win_w, int win_h)
{
    int count = 0;
    const CfrSixel *imgs = terminal_get_sixels(term, &count);
    rend_sokol_sixel_cache_reconcile(data, imgs, count);
    if (count == 0)
        return;

    int cell_w = data->cell_w;
    int cell_h = data->cell_h;
    float scale = data->content_scale > 0.0f ? data->content_scale : 1.0f;
    int scroll_offset = data->scroll.scroll_offset;
    float uniforms[2] = { (float)win_w, (float)win_h };
    uint8_t full_op[4] = { 255, 255, 255, 255 };

    int vert_count = 0;

    // Pass 1: build all vertices and ensure textures are cached
    for (int i = 0; i < count; i++) {
        const CfrSixel *img = &imgs[i];

        int screen_row = img->row + scroll_offset;
        int px = img->col * cell_w;
        int py = screen_row * cell_h;
        int scaled_w = logical_to_physical(img->width_px, scale);
        int scaled_h = logical_to_physical(img->height_px, scale);

        if (py + scaled_h <= 0 || py >= win_h)
            continue;
        if (px + scaled_w <= 0 || px >= win_w)
            continue;
        if (vert_count + 6 > SOKOL_MAX_SIXEL_VERTICES)
            break;

        float x0 = (float)px;
        float y0 = (float)py;
        float x1 = x0 + (float)scaled_w;
        float y1 = y0 + (float)scaled_h;

        uint8_t zero[4] = { 0, 0, 0, 0 };
        GlyphVertex *q = &s_sixel_verts[vert_count];
        rend_sokol_emit_glyph_quad(q, x0, y0, x1, y1,
                                   0.0f, 0.0f, 1.0f, 1.0f, full_op, zero);

        vert_count += 6;
    }

    if (vert_count == 0)
        return;

    // Upload vertex buffer once
    sg_update_buffer(data->sixel_vbuf, &(sg_range){
                                           .ptr = s_sixel_verts,
                                           .size = (size_t)vert_count * sizeof(GlyphVertex),
                                       });

    // Pass 2: draw each image's 6 verts with its own texture
    int vert_offset = 0;
    for (int i = 0; i < count; i++) {
        const CfrSixel *img = &imgs[i];

        int screen_row = img->row + scroll_offset;
        int px = img->col * cell_w;
        int py = screen_row * cell_h;
        int scaled_w = logical_to_physical(img->width_px, scale);
        int scaled_h = logical_to_physical(img->height_px, scale);

        if (py + scaled_h <= 0 || py >= win_h)
            continue;
        if (px + scaled_w <= 0 || px >= win_w)
            continue;
        if (vert_offset + 6 > vert_count)
            break;

        int cache_idx = rend_sokol_sixel_get_texture(data, img);
        if (cache_idx >= 0 && data->lottie_pip_created) {
            sg_apply_pipeline(data->lottie_pip);
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = data->sixel_vbuf,
                .views[0] = data->sixel_cache[cache_idx].view,
                .samplers[0] = data->lottie_sampler,
            });
            sg_apply_uniforms(0, &SG_RANGE(uniforms));
            sg_draw(vert_offset, 6, 1);
        }
        vert_offset += 6;
    }
}
