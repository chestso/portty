/*
 * portty — Procedural box-drawing glyph rasterization
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "rend_common.h"
#include "font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── Pixel buffer rendering context ──────────────────────────────────────
// Replaces SDL_Renderer with a simple RGBA pixel buffer so box-drawing
// glyphs can be rendered to a GlyphBitmap and cached in the texture atlas.

typedef struct
{
    uint8_t *pixels; // RGBA, 4 bytes/pixel
    int width, height;
    uint8_t r, g, b; // current draw color
    uint8_t alpha;   // current alpha (255 = opaque)
    bool blend;      // blend mode (true = alpha-composite, false = overwrite)
} BoxDrawCtx;

static void ctx_fill_rect(BoxDrawCtx *ctx, int x, int y, int w, int h)
{
    if (!ctx->pixels || w <= 0 || h <= 0)
        return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > ctx->width ? ctx->width : x + w;
    int y1 = y + h > ctx->height ? ctx->height : y + h;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            int idx = (py * ctx->width + px) * 4;
            if (ctx->blend) {
                float a = (float)ctx->alpha / 255.0f;
                ctx->pixels[idx + 0] = (uint8_t)(ctx->r * a + ctx->pixels[idx + 0] * (1.0f - a));
                ctx->pixels[idx + 1] = (uint8_t)(ctx->g * a + ctx->pixels[idx + 1] * (1.0f - a));
                ctx->pixels[idx + 2] = (uint8_t)(ctx->b * a + ctx->pixels[idx + 2] * (1.0f - a));
                ctx->pixels[idx + 3] = (uint8_t)(255 * a + ctx->pixels[idx + 3] * (1.0f - a));
            } else {
                ctx->pixels[idx + 0] = ctx->r;
                ctx->pixels[idx + 1] = ctx->g;
                ctx->pixels[idx + 2] = ctx->b;
                ctx->pixels[idx + 3] = ctx->alpha;
            }
        }
    }
}

// Clear a rectangle to fully transparent. Used to open a double tube's wall
// where a perpendicular tube crosses it.
static void ctx_clear_rect(BoxDrawCtx *ctx, int x, int y, int w, int h)
{
    if (!ctx->pixels || w <= 0 || h <= 0)
        return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > ctx->width ? ctx->width : x + w;
    int y1 = y + h > ctx->height ? ctx->height : y + h;
    for (int py = y0; py < y1; py++)
        memset(ctx->pixels + (size_t)(py * ctx->width + x0) * 4, 0,
               (size_t)(x1 - x0) * 4);
}

static void ctx_draw_point(BoxDrawCtx *ctx, float x, float y)
{
    if (!ctx->pixels)
        return;
    int px = (int)roundf(x);
    int py = (int)roundf(y);
    if (px < 0 || px >= ctx->width || py < 0 || py >= ctx->height)
        return;
    int idx = (py * ctx->width + px) * 4;
    if (ctx->blend) {
        float a = (float)ctx->alpha / 255.0f;
        ctx->pixels[idx + 0] = (uint8_t)(ctx->r * a + ctx->pixels[idx + 0] * (1.0f - a));
        ctx->pixels[idx + 1] = (uint8_t)(ctx->g * a + ctx->pixels[idx + 1] * (1.0f - a));
        ctx->pixels[idx + 2] = (uint8_t)(ctx->b * a + ctx->pixels[idx + 2] * (1.0f - a));
        ctx->pixels[idx + 3] = (uint8_t)(255 * a + ctx->pixels[idx + 3] * (1.0f - a));
    } else {
        ctx->pixels[idx + 0] = ctx->r;
        ctx->pixels[idx + 1] = ctx->g;
        ctx->pixels[idx + 2] = ctx->b;
        ctx->pixels[idx + 3] = ctx->alpha;
    }
}

static inline void ctx_set_color(BoxDrawCtx *ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    ctx->r = r;
    ctx->g = g;
    ctx->b = b;
    ctx->alpha = a;
}

static inline void ctx_set_blend(BoxDrawCtx *ctx, bool blend)
{
    ctx->blend = blend;
}

// ── End pixel buffer context ────────────────────────────────────────────

// Encode direction weights into a byte: (up << 6) | (down << 4) | (left << 2) | right
// Weight values: 0=none, 1=light, 2=heavy, 3=double
#define BDE(u, d, l, r) (uint8_t)(((u) << 6) | ((d) << 4) | ((l) << 2) | (r))

// Box drawing lookup table (U+2500-U+254F, 80 entries)
// clang-format off
static const uint8_t box_table_main[80] = {
    // U+2500-U+250F
    BDE(0,0,1,1), // U+2500 ─
    BDE(0,0,2,2), // U+2501 ━
    BDE(1,1,0,0), // U+2502 │
    BDE(2,2,0,0), // U+2503 ┃
    BDE(0,0,1,1), // U+2504 ┄  [dashed, drawn solid]
    BDE(0,0,2,2), // U+2505 ┅  [dashed, drawn solid]
    BDE(1,1,0,0), // U+2506 ┆  [dashed, drawn solid]
    BDE(2,2,0,0), // U+2507 ┇  [dashed, drawn solid]
    BDE(0,0,1,1), // U+2508 ┈  [dashed, drawn solid]
    BDE(0,0,2,2), // U+2509 ┉  [dashed, drawn solid]
    BDE(1,1,0,0), // U+250A ┊  [dashed, drawn solid]
    BDE(2,2,0,0), // U+250B ┋  [dashed, drawn solid]
    BDE(0,1,0,1), // U+250C ┌
    BDE(0,1,0,2), // U+250D ┍
    BDE(0,2,0,1), // U+250E ┎
    BDE(0,2,0,2), // U+250F ┏
    // U+2510-U+251F
    BDE(0,1,1,0), // U+2510 ┐
    BDE(0,1,2,0), // U+2511 ┑
    BDE(0,2,1,0), // U+2512 ┒
    BDE(0,2,2,0), // U+2513 ┓
    BDE(1,0,0,1), // U+2514 └
    BDE(1,0,0,2), // U+2515 ┕
    BDE(2,0,0,1), // U+2516 ┖
    BDE(2,0,0,2), // U+2517 ┗
    BDE(1,0,1,0), // U+2518 ┘
    BDE(1,0,2,0), // U+2519 ┙
    BDE(2,0,1,0), // U+251A ┚
    BDE(2,0,2,0), // U+251B ┛
    BDE(1,1,0,1), // U+251C ├
    BDE(1,1,0,2), // U+251D ┝
    BDE(2,1,0,1), // U+251E ┞
    BDE(1,2,0,1), // U+251F ┟
    // U+2520-U+252F
    BDE(2,2,0,1), // U+2520 ┠
    BDE(2,1,0,2), // U+2521 ┡
    BDE(1,2,0,2), // U+2522 ┢
    BDE(2,2,0,2), // U+2523 ┣
    BDE(1,1,1,0), // U+2524 ┤
    BDE(1,1,2,0), // U+2525 ┥
    BDE(2,1,1,0), // U+2526 ┦
    BDE(1,2,1,0), // U+2527 ┧
    BDE(2,2,1,0), // U+2528 ┨
    BDE(2,1,2,0), // U+2529 ┩
    BDE(1,2,2,0), // U+252A ┪
    BDE(2,2,2,0), // U+252B ┫
    BDE(0,1,1,1), // U+252C ┬
    BDE(0,1,2,1), // U+252D ┭
    BDE(0,1,1,2), // U+252E ┮
    BDE(0,1,2,2), // U+252F ┯
    // U+2530-U+253F
    BDE(0,2,1,1), // U+2530 ┰
    BDE(0,2,2,1), // U+2531 ┱
    BDE(0,2,1,2), // U+2532 ┲
    BDE(0,2,2,2), // U+2533 ┳
    BDE(1,0,1,1), // U+2534 ┴
    BDE(1,0,2,1), // U+2535 ┵
    BDE(1,0,1,2), // U+2536 ┶
    BDE(1,0,2,2), // U+2537 ┷
    BDE(2,0,1,1), // U+2538 ┸
    BDE(2,0,2,1), // U+2539 ┹
    BDE(2,0,1,2), // U+253A ┺
    BDE(2,0,2,2), // U+253B ┻
    BDE(1,1,1,1), // U+253C ┼
    BDE(1,1,2,1), // U+253D ┽
    BDE(1,1,1,2), // U+253E ┾
    BDE(1,1,2,2), // U+253F ┿
    // U+2540-U+254F
    BDE(2,1,1,1), // U+2540 ╀
    BDE(1,2,1,1), // U+2541 ╁
    BDE(2,2,1,1), // U+2542 ╂
    BDE(2,1,2,1), // U+2543 ╃
    BDE(2,1,1,2), // U+2544 ╄
    BDE(1,2,2,1), // U+2545 ╅
    BDE(1,2,1,2), // U+2546 ╆
    BDE(2,1,2,2), // U+2547 ╇
    BDE(1,2,2,2), // U+2548 ╈
    BDE(2,2,1,2), // U+2549 ╉
    BDE(2,2,2,1), // U+254A ╊
    BDE(2,2,2,2), // U+254B ╋
    BDE(0,0,1,1), // U+254C ╌  [dashed, drawn solid]
    BDE(0,0,2,2), // U+254D ╍  [dashed, drawn solid]
    BDE(1,1,0,0), // U+254E ╎  [dashed, drawn solid]
    BDE(2,2,0,0), // U+254F ╏  [dashed, drawn solid]
};

// U+2550-U+257F (double-line, arcs, half-lines)
static const uint8_t box_table_ext[48] = {
    BDE(0,0,3,3), // U+2550 ═
    BDE(3,3,0,0), // U+2551 ║
    BDE(0,1,0,3), // U+2552 ╒
    BDE(0,3,0,1), // U+2553 ╓
    BDE(0,3,0,3), // U+2554 ╔
    BDE(0,1,3,0), // U+2555 ╕
    BDE(0,3,1,0), // U+2556 ╖
    BDE(0,3,3,0), // U+2557 ╗
    BDE(1,0,0,3), // U+2558 ╘
    BDE(3,0,0,1), // U+2559 ╙
    BDE(3,0,0,3), // U+255A ╚
    BDE(1,0,3,0), // U+255B ╛
    BDE(3,0,1,0), // U+255C ╜
    BDE(3,0,3,0), // U+255D ╝
    BDE(1,1,0,3), // U+255E ╞
    BDE(3,3,0,1), // U+255F ╟
    BDE(3,3,0,3), // U+2560 ╠
    BDE(1,1,3,0), // U+2561 ╡
    BDE(3,3,1,0), // U+2562 ╢
    BDE(3,3,3,0), // U+2563 ╣
    BDE(0,1,3,3), // U+2564 ╤
    BDE(0,3,1,1), // U+2565 ╥
    BDE(0,3,3,3), // U+2566 ╦
    BDE(1,0,3,3), // U+2567 ╧
    BDE(3,0,1,1), // U+2568 ╨
    BDE(3,0,3,3), // U+2569 ╩
    BDE(1,1,3,3), // U+256A ╪
    BDE(3,3,1,1), // U+256B ╫
    BDE(3,3,3,3), // U+256C ╬
    BDE(0,0,0,0), // U+256D ╭  [arc, handled separately]
    BDE(0,0,0,0), // U+256E ╮  [arc, handled separately]
    BDE(0,0,0,0), // U+256F ╯  [arc, handled separately]
    BDE(0,0,0,0), // U+2570 ╰  [arc, handled separately]
    BDE(0,0,0,0), // U+2571 ╱  [diagonal, handled separately]
    BDE(0,0,0,0), // U+2572 ╲  [diagonal, handled separately]
    BDE(0,0,0,0), // U+2573 ╳  [diagonal, handled separately]
    BDE(0,0,1,0), // U+2574 ╴  light left
    BDE(1,0,0,0), // U+2575 ╵  light up
    BDE(0,0,0,1), // U+2576 ╶  light right
    BDE(0,1,0,0), // U+2577 ╷  light down
    BDE(0,0,2,0), // U+2578 ╸  heavy left
    BDE(2,0,0,0), // U+2579 ╹  heavy up
    BDE(0,0,0,2), // U+257A ╺  heavy right
    BDE(0,2,0,0), // U+257B ╻  heavy down
    BDE(0,0,1,2), // U+257C ╼  light left, heavy right
    BDE(1,2,0,0), // U+257D ╽  light up, heavy down
    BDE(0,0,2,1), // U+257E ╾  heavy left, light right
    BDE(2,1,0,0), // U+257F ╿  heavy up, light down
};
// clang-format on

#undef BDE

static uint8_t get_box_encoding(uint32_t cp)
{
    if (cp >= 0x2500 && cp <= 0x254F)
        return box_table_main[cp - 0x2500];
    if (cp >= 0x2550 && cp <= 0x257F)
        return box_table_ext[cp - 0x2550];
    return 0;
}

bool rend_boxdraw_is_supported(uint32_t cp)
{
    return (cp >= 0x2500 && cp <= 0x257F) || (cp >= 0x2580 && cp <= 0x259F);
}

// Resolved pixel geometry for one box-drawing cell.
//
// A double line is a hollow "tube": two parallel sub-lines with the space
// between them left as background.  The sub-lines are V1/V2 (left/right
// vertical) and H1/H2 (top/bottom horizontal); a tube's interior is the band
// between its two sub-lines.  All edges are pixel coordinates relative to the
// cell origin.
typedef struct
{
    int x, y, w, h; // cell rect
    int cx, cy;     // cell center
    int light;      // stroke thickness for weight 1
    int heavy;      // stroke thickness for weight 2
    int light_half;
    int heavy_half;
    int lv_left, lv_right; // left vertical sub-line (V1)
    int rv_left, rv_right; // right vertical sub-line (V2)
    int th_top, th_bot;    // top horizontal sub-line (H1)
    int bh_top, bh_bot;    // bottom horizontal sub-line (H2)
} BoxGeom;

static BoxGeom box_geom(int x, int y, int w, int h)
{
    BoxGeom g;

    g.x = x;
    g.y = y;
    g.w = w;
    g.h = h;
    g.cx = x + w / 2;
    g.cy = y + h / 2;

    // Uniform line thickness based on cell width (narrower dimension)
    g.light = w / 5;
    if (g.light < 1)
        g.light = 1;
    g.heavy = g.light * 3;
    if (g.heavy < g.light + 2)
        g.heavy = g.light + 2;
    g.light_half = g.light / 2;
    g.heavy_half = g.heavy / 2;

    // Sub-line rect edges.  Horizontal and vertical sub-lines connect at
    // these edges (not centers) so corners meet without gaps.
    int off = g.light + (g.light + 1) / 2; // sub-line offset from center
    g.lv_left = g.cx - off - g.light_half;
    g.lv_right = g.lv_left + g.light;
    g.rv_left = g.cx + off - g.light_half;
    g.rv_right = g.rv_left + g.light;
    g.th_top = g.cy - off - g.light_half;
    g.th_bot = g.th_top + g.light;
    g.bh_top = g.cy + off - g.light_half;
    g.bh_bot = g.bh_top + g.light;
    return g;
}

// Draw single/heavy box lines (weights 1 and 2).
//
// A stub running into a double tube stops at the tube's wall rather than at
// the cell center: at the near wall when the stub branches off a through
// tube, or at the outer wall when the stub is the other arm of a corner.
// Where both halves of an axis are single/heavy the two stubs meet in the
// middle and span the cell, so no adjustment is needed.
// All coordinates are intentionally integer-truncated for pixel-aligned rendering.
static void draw_single_heavy_lines(BoxDrawCtx *ctx, const BoxGeom *g,
                                    int up, int down, int left, int right)
{
    bool v_double = (up == 3 || down == 3);
    bool v_through = (up == 3 && down == 3);
    bool h_double = (left == 3 || right == 3);
    bool h_through = (left == 3 && right == 3);

    int up_end = g->cy + (up == 2 ? g->heavy_half : g->light_half);
    int down_start = g->cy - (down == 2 ? g->heavy_half : g->light_half);
    int left_end = g->cx + (left == 2 ? g->heavy_half : g->light_half);
    int right_start = g->cx - (right == 2 ? g->heavy_half : g->light_half);

    if (h_double && !(up && down)) {
        up_end = h_through ? g->th_bot : g->bh_bot;
        down_start = h_through ? g->bh_top : g->th_top;
    }
    if (v_double && !(left && right)) {
        left_end = v_through ? g->lv_right : g->rv_right;
        right_start = v_through ? g->rv_left : g->lv_left;
    }

#define FILL(rx, ry, rw, rh) ctx_fill_rect(ctx, (rx), (ry), (rw), (rh))

    if (up == 1)
        FILL(g->cx - g->light_half, g->y, g->light, up_end - g->y);
    else if (up == 2)
        FILL(g->cx - g->heavy_half, g->y, g->heavy, up_end - g->y);

    if (down == 1)
        FILL(g->cx - g->light_half, down_start, g->light, g->y + g->h - down_start);
    else if (down == 2)
        FILL(g->cx - g->heavy_half, down_start, g->heavy, g->y + g->h - down_start);

    if (left == 1)
        FILL(g->x, g->cy - g->light_half, left_end - g->x, g->light);
    else if (left == 2)
        FILL(g->x, g->cy - g->heavy_half, left_end - g->x, g->heavy);

    if (right == 1)
        FILL(right_start, g->cy - g->light_half, g->x + g->w - right_start, g->light);
    else if (right == 2)
        FILL(right_start, g->cy - g->heavy_half, g->x + g->w - right_start, g->heavy);

#undef FILL
}

// Draw double box lines (weight 3) using 4 sub-lines with proper corner
// connections.  The 4 sub-lines are: left-v (V1), right-v (V2),
// top-h (H1) and bot-h (H2).
//
// At a corner the outer sub-lines connect to each other and the inner
// sub-lines connect to each other, forming two nested L-shapes.  At a
// T-junction or a cross, the through tube's wall is opened over the crossing
// tube's interior so the two tube interiors form one connected region.
// All coordinates are intentionally integer-truncated for pixel-aligned rendering.
static void draw_double_lines(BoxDrawCtx *ctx, const BoxGeom *g,
                              int up, int down, int left, int right)
{
    int lw = g->light;

    bool du = (up == 3), dd = (down == 3), dl = (left == 3), dr = (right == 3);
    bool has_dv = du || dd;
    bool has_dh = dl || dr;

#define FILL(rx, ry, rw, rh) ctx_fill_rect(ctx, (rx), (ry), (rw), (rh))

    // --- Vertical sub-lines ---
    if (has_dv) {
        int lv_y1, lv_y2, rv_y1, rv_y2;

        // Top endpoints
        if (du) {
            lv_y1 = g->y;
            rv_y1 = g->y;
        } else if (has_dh) {
            // Only going down. Determine corner pairing:
            // down+right (╔-like): outer = left-v/top-h, inner = right-v/bot-h
            // down+left  (╗-like): outer = right-v/top-h, inner = left-v/bot-h
            // down+both  (╦-like): both verticals start at bot-h
            if (dr && !dl) {
                lv_y1 = g->th_top;
                rv_y1 = g->bh_top;
            } else if (dl && !dr) {
                rv_y1 = g->th_top;
                lv_y1 = g->bh_top;
            } else {
                lv_y1 = g->bh_top;
                rv_y1 = g->bh_top;
            }
        } else {
            lv_y1 = g->cy - g->light_half;
            rv_y1 = g->cy - g->light_half;
        }

        // Bottom endpoints
        if (dd) {
            lv_y2 = g->y + g->h;
            rv_y2 = g->y + g->h;
        } else if (has_dh) {
            // Only going up.
            // up+right (╚-like): outer = left-v/bot-h, inner = right-v/top-h
            // up+left  (╝-like): outer = right-v/bot-h, inner = left-v/top-h
            // up+both  (╩-like): both verticals end at top-h
            if (dr && !dl) {
                lv_y2 = g->bh_bot;
                rv_y2 = g->th_bot;
            } else if (dl && !dr) {
                rv_y2 = g->bh_bot;
                lv_y2 = g->th_bot;
            } else {
                lv_y2 = g->th_bot;
                rv_y2 = g->th_bot;
            }
        } else {
            lv_y2 = g->cy + g->light_half;
            rv_y2 = g->cy + g->light_half;
        }

        if (lv_y2 > lv_y1)
            FILL(g->lv_left, lv_y1, lw, lv_y2 - lv_y1);
        if (rv_y2 > rv_y1)
            FILL(g->rv_left, rv_y1, lw, rv_y2 - rv_y1);
    }

    // --- Horizontal sub-lines ---
    if (has_dh) {
        int th_x1, th_x2, bh_x1, bh_x2;

        // Left endpoints
        if (dl) {
            th_x1 = g->x;
            bh_x1 = g->x;
        } else if (has_dv) {
            // Only going right.
            // down+right (╔-like): outer = top-h/left-v, inner = bot-h/right-v
            // up+right   (╚-like): outer = bot-h/left-v, inner = top-h/right-v
            // both+right (╠-like): both horizontals start at right-v
            if (dd && !du) {
                th_x1 = g->lv_left;
                bh_x1 = g->rv_left;
            } else if (du && !dd) {
                bh_x1 = g->lv_left;
                th_x1 = g->rv_left;
            } else {
                th_x1 = g->rv_left;
                bh_x1 = g->rv_left;
            }
        } else {
            th_x1 = g->cx - g->light_half;
            bh_x1 = g->cx - g->light_half;
        }

        // Right endpoints
        if (dr) {
            th_x2 = g->x + g->w;
            bh_x2 = g->x + g->w;
        } else if (has_dv) {
            // Only going left.
            // down+left (╗-like): outer = top-h/right-v, inner = bot-h/left-v
            // up+left   (╝-like): outer = bot-h/right-v, inner = top-h/left-v
            // both+left (╣-like): both horizontals end at left-v
            if (dd && !du) {
                th_x2 = g->rv_right;
                bh_x2 = g->lv_right;
            } else if (du && !dd) {
                bh_x2 = g->rv_right;
                th_x2 = g->lv_right;
            } else {
                th_x2 = g->lv_right;
                bh_x2 = g->lv_right;
            }
        } else {
            th_x2 = g->cx + g->light_half;
            bh_x2 = g->cx + g->light_half;
        }

        if (th_x2 > th_x1)
            FILL(th_x1, g->th_top, th_x2 - th_x1, lw);
        if (bh_x2 > bh_x1)
            FILL(bh_x1, g->bh_top, bh_x2 - bh_x1, lw);
    }

    // --- Open the junctions ---
    // A through tube's wall is cut where a perpendicular double tube crosses
    // it, so the two tube interiors join into one open region.
    if (has_dv && has_dh) {
        if (du && dd) {
            if (dr)
                ctx_clear_rect(ctx, g->rv_left, g->th_bot, lw, g->bh_top - g->th_bot);
            if (dl)
                ctx_clear_rect(ctx, g->lv_left, g->th_bot, lw, g->bh_top - g->th_bot);
        }
        if (dl && dr) {
            if (dd)
                ctx_clear_rect(ctx, g->lv_right, g->bh_top, g->rv_left - g->lv_right, lw);
            if (du)
                ctx_clear_rect(ctx, g->lv_right, g->th_top, g->rv_left - g->lv_right, lw);
        }
    }

#undef FILL
}

static void draw_box_lines(BoxDrawCtx *ctx, uint8_t enc,
                           int x, int y, int w, int h)
{
    int up = (enc >> 6) & 3;
    int down = (enc >> 4) & 3;
    int left = (enc >> 2) & 3;
    int right = (enc >> 0) & 3;

    BoxGeom g = box_geom(x, y, w, h);

    // Draw single/heavy lines (weights 1 and 2)
    draw_single_heavy_lines(ctx, &g, up, down, left, right);

    // Draw double lines (weight 3)
    if (up == 3 || down == 3 || left == 3 || right == 3)
        draw_double_lines(ctx, &g, up, down, left, right);
}

static void draw_block_element(BoxDrawCtx *ctx, uint32_t cp,
                               int x, int y, int w, int h)
{
    if (cp == 0x2580) {
        // Upper half block
        int half_h = h / 2;
        ctx_fill_rect(ctx, x, y, w, half_h);
        return;
    }

    if (cp >= 0x2581 && cp <= 0x2588) {
        // Lower N/8 blocks (1/8 through full)
        int n = cp - 0x2580;
        int block_h = (h * n + 4) / 8;
        ctx_fill_rect(ctx, x, y + h - block_h, w, block_h);
        return;
    }

    if (cp >= 0x2589 && cp <= 0x258F) {
        // Left N/8 blocks (7/8 down to 1/8)
        int n = 0x2590 - cp;
        int block_w = (w * n + 4) / 8;
        ctx_fill_rect(ctx, x, y, block_w, h);
        return;
    }

    if (cp == 0x2590) {
        // Right half block
        int half = w / 2;
        ctx_fill_rect(ctx, x + w - half, y, half, h);
        return;
    }

    if (cp >= 0x2591 && cp <= 0x2593) {
        // Shade characters (light=64, medium=128, dark=192)
        uint8_t alpha;
        if (cp == 0x2591)
            alpha = 64;
        else if (cp == 0x2592)
            alpha = 128;
        else
            alpha = 192;

        uint8_t saved_alpha = ctx->alpha;
        bool saved_blend = ctx->blend;
        ctx_set_blend(ctx, true);
        ctx_set_color(ctx, ctx->r, ctx->g, ctx->b, alpha);
        ctx_fill_rect(ctx, x, y, w, h);
        ctx_set_color(ctx, ctx->r, ctx->g, ctx->b, saved_alpha);
        ctx_set_blend(ctx, saved_blend);
        return;
    }

    if (cp == 0x2594) {
        // Upper 1/8 block
        int block_h = (h + 4) / 8;
        if (block_h < 1)
            block_h = 1;
        ctx_fill_rect(ctx, x, y, w, block_h);
        return;
    }

    if (cp == 0x2595) {
        // Right 1/8 block
        int block_w = (w + 4) / 8;
        if (block_w < 1)
            block_w = 1;
        ctx_fill_rect(ctx, x + w - block_w, y, block_w, h);
        return;
    }

    if (cp >= 0x2596 && cp <= 0x259F) {
        // Quadrant block characters
        // Bits: 0=lower-left, 1=lower-right, 2=upper-left, 3=upper-right
        int half_w = w / 2;
        int half_h = h / 2;
        int right_w = w - half_w;
        int bottom_h = h - half_h;

        uint8_t bits;
        switch (cp) {
        case 0x2596:
            bits = 0x01;
            break; // lower left
        case 0x2597:
            bits = 0x02;
            break; // lower right
        case 0x2598:
            bits = 0x04;
            break; // upper left
        case 0x2599:
            bits = 0x07;
            break; // upper left + lower left + lower right
        case 0x259A:
            bits = 0x06;
            break; // upper left + lower right
        case 0x259B:
            bits = 0x0D;
            break; // upper left + upper right + lower left
        case 0x259C:
            bits = 0x0E;
            break; // upper left + upper right + lower right
        case 0x259D:
            bits = 0x08;
            break; // upper right
        case 0x259E:
            bits = 0x09;
            break; // upper right + lower left
        case 0x259F:
            bits = 0x0B;
            break; // upper right + lower left + lower right
        default:
            return;
        }

        if (bits & 0x01)
            ctx_fill_rect(ctx, x, y + half_h, half_w, bottom_h);
        if (bits & 0x02)
            ctx_fill_rect(ctx, x + half_w, y + half_h, right_w, bottom_h);
        if (bits & 0x04)
            ctx_fill_rect(ctx, x, y, half_w, half_h);
        if (bits & 0x08)
            ctx_fill_rect(ctx, x + half_w, y, right_w, half_h);
    }
}

// Clamp x to [0, 1]
static float clampf01(float x)
{
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

// Hermite smoothstep: 0 below edge0, 1 above edge1, smooth in between.
static float smoothstepf(float edge0, float edge1, float x)
{
    if (edge0 == edge1)
        return x < edge0 ? 0.0f : 1.0f;
    float t = clampf01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// Draw a rounded corner arc for U+256D-U+2570 (╭╮╯╰).
//
// Uses a signed distance field (SDF) approach inspired by kitty's
// decorations.c.  The corner is modelled as the outline of a rounded
// rectangle whose bounding box spans from the cell centre (cx, cy) to
// just past the outer cell edges.  The corner radius equals min(Hx, Hy)
// — i.e. the maximum possible radius — so the arc is a true quarter
// circle that smoothly transitions into straight stubs on both sides.
//
// For each pixel, we compute the signed distance to the rounded-
// rectangle boundary, then use smoothstep to produce anti-aliased
// coverage.  The result is a stroke of uniform thickness that is
// smooth on both the inner and outer edges, with no seams between
// the arc and the straight stubs.
static void draw_rounded_corner(BoxDrawCtx *ctx, uint32_t cp,
                                int x, int y, int w, int h,
                                uint8_t r, uint8_t g, uint8_t b)
{
    int light = w / 5;
    if (light < 1)
        light = 1;

    int cx = x + w / 2;
    int cy = y + h / 2;

    // Following kitty's approach: the rounded rectangle is centred at the
    // cell centre (cx, cy) with half-extents matching the line centers.
    // adjusted_Hx/Hy are the distances from the cell edges to the center
    // of the vertical/horizontal line stroke (accounting for pixel centering).
    float light_f = (float)light;
    int light_half = light / 2;
    float adjusted_Hx = (float)(cx - x - light_half) + light_f * 0.5f;
    float adjusted_Hy = (float)(cy - y - light_half) + light_f * 0.5f;
    float x_shift, y_shift;

    switch (cp) {
    case 0x256D: // ╭ down+right — arc at (cx,cy), outer corner at (x,y)
        x_shift = -adjusted_Hx;
        y_shift = -adjusted_Hy;
        break;
    case 0x256E: // ╮ down+left — arc at (cx,cy), outer corner at (x+w,y)
        x_shift = adjusted_Hx;
        y_shift = -adjusted_Hy;
        break;
    case 0x256F: // ╯ up+left — arc at (cx,cy), outer corner at (x+w,y+h)
        x_shift = adjusted_Hx;
        y_shift = adjusted_Hy;
        break;
    case 0x2570: // ╰ up+right — arc at (cx,cy), outer corner at (x,y+h)
        x_shift = -adjusted_Hx;
        y_shift = adjusted_Hy;
        break;
    default:
        return;
    }

    float stroke = (float)light;
    float half_stroke = stroke * 0.5f;
    float corner_radius = adjusted_Hx < adjusted_Hy ? adjusted_Hx : adjusted_Hy;
    float bx = adjusted_Hx - corner_radius;
    float by = adjusted_Hy - corner_radius;
    float aa = 0.5f; // anti-aliasing band width

    ctx_set_blend(ctx, true);

    for (int py = y; py < y + h; py++) {
        float sample_y = (float)py + y_shift + 0.5f;
        float pos_y = sample_y - adjusted_Hy;

        for (int px = x; px < x + w; px++) {
            float sample_x = (float)px + x_shift + 0.5f;
            float pos_x = sample_x - adjusted_Hx;

            // SDF of a rounded rectangle centred at origin with half-extents
            // (Hx, Hy) and corner radius.  The distance to the nearest point
            // on the rounded rectangle boundary:
            //   qx = |pos_x| - (Hx - corner_radius)
            //   qy = |pos_y| - (Hy - corner_radius)
            //   dist = hypot(max(qx,0), max(qy,0)) + min(max(qx,qy),0) - corner_radius
            float qx = fabsf(pos_x) - bx;
            float qy = fabsf(pos_y) - by;
            float dx = qx > 0.0f ? qx : 0.0f;
            float dy = qy > 0.0f ? qy : 0.0f;
            float dist = sqrtf(dx * dx + dy * dy) +
                         (qx > qy ? (qx < 0.0f ? qx : 0.0f)
                                  : (qy < 0.0f ? qy : 0.0f)) -
                         corner_radius;

            // Stroke = area between dist = +half_stroke (outer) and
            // dist = -half_stroke (inner).  Anti-alias both edges.
            float outer = half_stroke - dist;
            float inner = -half_stroke - dist;

            // smoothstep(-aa, aa, x) gives 0 below -aa, 1 above +aa,
            // with a smooth transition in between.
            float alpha = smoothstepf(-aa, aa, outer) -
                          smoothstepf(-aa, aa, inner);

            if (alpha <= 0.0f)
                continue;

            uint8_t a = (uint8_t)(alpha * 255.0f + 0.5f);
            if (a > 0) {
                ctx_set_color(ctx, r, g, b, a);
                ctx_draw_point(ctx, (float)px, (float)py);
            }
        }
    }

    ctx_set_blend(ctx, false);
}

// Draw diagonal lines for U+2571 (╱), U+2572 (╲), U+2573 (╳).
// Uses SDF-based rendering for uniform thickness matching normal box lines.
//
// The bitmap has 10% proportional margins on all sides, creating overhang
// that fills gaps when cells are stacked.  The atlas stores the oversized
// bitmap and blits it with negative offsets so overhang overlaps adjacent
// cells.
//
// Line direction uses the exact (unrounded) cell dimensions, not the rounded
// bitmap dimensions.  The rounded margins would shift the bitmap's aspect
// ratio away from the cell's, causing a slight slope error that breaks
// seamless tiling.  By using the exact cell aspect ratio for the line
// equation and positioning through the bitmap center, the line slope
// matches the true cell geometry regardless of margin rounding.
static void draw_diagonal_lines(BoxDrawCtx *ctx, uint32_t cp,
                                int cell_w, int cell_h,
                                int pad_x, int pad_y,
                                uint8_t r, uint8_t g, uint8_t b)
{
    float stroke = (float)cell_w / 5.0f;
    if (stroke < 1.0f)
        stroke = 1.0f;

    int bmp_w = cell_w + pad_x * 2;
    int bmp_h = cell_h + pad_y * 2;

    float half_stroke = stroke * 0.5f;
    float aa = 0.5f;

    ctx_set_blend(ctx, true);

    float len;
    float nx, ny, c;

    // Draw bottom-left to top-right diagonal (╱)
    // Direction from exact cell dimensions (not rounded bitmap dimensions)
    // so the slope matches the true cell aspect ratio for seamless stacking.
    // Line passes through the bitmap center (bmp_w/2, bmp_h/2).
    if (cp == 0x2571 || cp == 0x2573) {
        float dx = (float)cell_w;
        float dy = (float)(-cell_h);
        len = sqrtf(dx * dx + dy * dy);
        if (len > 0.001f) {
            nx = -dy / len;
            ny = dx / len;
            float cx = (float)bmp_w * 0.5f;
            float cy = (float)bmp_h * 0.5f;
            c = -(nx * cx + ny * cy);

            for (int py = 0; py < bmp_h; py++) {
                for (int px = 0; px < bmp_w; px++) {
                    float dist = nx * (float)px + ny * (float)py + c;
                    float outer = half_stroke - fabsf(dist);
                    float alpha = smoothstepf(-aa, aa, outer);
                    if (alpha > 0.0f) {
                        uint8_t a = (uint8_t)(alpha * 255.0f + 0.5f);
                        ctx_set_color(ctx, r, g, b, a);
                        ctx_draw_point(ctx, (float)px, (float)py);
                    }
                }
            }
        }
    }

    // Draw top-left to bottom-right diagonal (╲)
    // Direction from exact cell dimensions (not rounded bitmap dimensions)
    // so the slope matches the true cell aspect ratio for seamless stacking.
    // Line passes through the bitmap center (bmp_w/2, bmp_h/2).
    if (cp == 0x2572 || cp == 0x2573) {
        float dx = (float)cell_w;
        float dy = (float)cell_h;
        len = sqrtf(dx * dx + dy * dy);
        if (len > 0.001f) {
            nx = -dy / len;
            ny = dx / len;
            float cx = (float)bmp_w * 0.5f;
            float cy = (float)bmp_h * 0.5f;
            c = -(nx * cx + ny * cy);

            for (int py = 0; py < bmp_h; py++) {
                for (int px = 0; px < bmp_w; px++) {
                    float dist = nx * (float)px + ny * (float)py + c;
                    float outer = half_stroke - fabsf(dist);
                    float alpha = smoothstepf(-aa, aa, outer);
                    if (alpha > 0.0f) {
                        uint8_t a = (uint8_t)(alpha * 255.0f + 0.5f);
                        ctx_set_color(ctx, r, g, b, a);
                        ctx_draw_point(ctx, (float)px, (float)py);
                    }
                }
            }
        }
    }

    ctx_set_blend(ctx, false);
}

// Internal: draw into a BoxDrawCtx at the given cell-local coordinates.
// For diagonals, the line direction uses the exact cell aspect ratio
// while the bitmap is clipped to the 10% padded bounds, ensuring the
// slope matches the true cell geometry for seamless tiling.
static void boxdraw_render_to_ctx(BoxDrawCtx *ctx, uint32_t cp,
                                  int cell_w, int cell_h,
                                  int pad_x, int pad_y,
                                  uint8_t r, uint8_t g, uint8_t b)
{
    ctx_set_color(ctx, r, g, b, 255);

    if (cp >= 0x256D && cp <= 0x2570) {
        draw_rounded_corner(ctx, cp, 0, 0, cell_w, cell_h, r, g, b);
    } else if (cp >= 0x2571 && cp <= 0x2573) {
        draw_diagonal_lines(ctx, cp, cell_w, cell_h, pad_x, pad_y, r, g, b);
    } else if (cp >= 0x2500 && cp <= 0x257F) {
        uint8_t enc = get_box_encoding(cp);
        if (enc != 0)
            draw_box_lines(ctx, enc, 0, 0, cell_w, cell_h);
    } else if (cp >= 0x2580 && cp <= 0x259F) {
        draw_block_element(ctx, cp, 0, 0, cell_w, cell_h);
    }
}

// Public API: render a box-drawing character to a GlyphBitmap.
// The bitmap is cell-sized (w×h) with centered=true, RGBA pixels,
// and is intended to be inserted into the texture atlas like a font glyph.
//
// Diagonal characters (U+2571-U+2573) get 10% proportional margins on all
// sides.  The line direction uses the exact cell aspect ratio rather than
// the rounded bitmap dimensions, so the slope is not distorted by margin
// rounding and stacked cells produce seamless diagonals.
GlyphBitmap *rend_boxdraw_render(uint32_t cp, int cell_w, int cell_h,
                                 uint8_t r, uint8_t g, uint8_t b)
{
    bool is_diagonal = (cp >= 0x2571 && cp <= 0x2573);
    int margin_x = is_diagonal ? (int)roundf((float)cell_w * 0.10f) : 0;
    int margin_y = is_diagonal ? (int)roundf((float)cell_h * 0.10f) : 0;
    int bmp_w = cell_w + margin_x * 2;
    int bmp_h = cell_h + margin_y * 2;

    if (is_diagonal) {
        // Debug: log diagonal bitmap dimensions
        // fprintf(stderr, "BOXDRAW: cp=U+%04X cell=%dx%d margin=%d,%d bmp=%dx%d\n",
        //         cp, cell_w, cell_h, margin_x, margin_y, bmp_w, bmp_h);
    }

    GlyphBitmap *bmp = malloc(sizeof(GlyphBitmap));
    if (!bmp)
        return NULL;
    bmp->width = bmp_w;
    bmp->height = bmp_h;
    bmp->x_offset = 0;
    bmp->y_offset = 0;
    bmp->advance = cell_w;
    bmp->glyph_id = (int)cp;
    bmp->centered = true;
    bmp->pixels = calloc((size_t)bmp_w * bmp_h * 4, 1);
    if (!bmp->pixels) {
        free(bmp);
        return NULL;
    }

    BoxDrawCtx ctx = {
        .pixels = bmp->pixels,
        .width = bmp_w,
        .height = bmp_h,
        .r = r,
        .g = g,
        .b = b,
        .alpha = 255,
        .blend = false,
    };

    boxdraw_render_to_ctx(&ctx, cp, cell_w, cell_h, margin_x, margin_y, r, g, b);
    return bmp;
}
