#include "rend_sdl3_boxdraw.h"

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
    BDE(0,1,0,1), // U+256D ╭  [arc, drawn as corner]
    BDE(0,1,1,0), // U+256E ╮  [arc, drawn as corner]
    BDE(1,0,1,0), // U+256F ╯  [arc, drawn as corner]
    BDE(1,0,0,1), // U+2570 ╰  [arc, drawn as corner]
    BDE(1,1,1,1), // U+2571 ╱  [diagonal, drawn as cross]
    BDE(1,1,1,1), // U+2572 ╲  [diagonal, drawn as cross]
    BDE(1,1,1,1), // U+2573 ╳  [diagonal, drawn as cross]
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

bool rend_sdl3_boxdraw_is_supported(uint32_t cp)
{
    return (cp >= 0x2500 && cp <= 0x257F) || (cp >= 0x2580 && cp <= 0x259F);
}

// Draw single/heavy box lines (weights 1 and 2) from center to edges.
// All coordinates are intentionally integer-truncated for pixel-aligned rendering.
static void draw_single_heavy_lines(SDL_Renderer *renderer,
                                    int up, int down, int left, int right,
                                    int x, int y, int w, int h,
                                    int light, int heavy)
{
    int cx = x + w / 2;
    int cy = y + h / 2;
    int light_half = light / 2;
    int heavy_half = heavy / 2;

#define FILL(rx, ry, rw, rh)                                                   \
    do {                                                                       \
        SDL_FRect _r = { (float)(rx), (float)(ry), (float)(rw), (float)(rh) }; \
        SDL_RenderFillRect(renderer, &_r);                                     \
    } while (0)

    if (up == 1)
        FILL(cx - light_half, y, light, cy - y + light_half);
    else if (up == 2)
        FILL(cx - heavy_half, y, heavy, cy - y + heavy_half);

    if (down == 1)
        FILL(cx - light_half, cy - light_half, light, y + h - cy + light_half);
    else if (down == 2)
        FILL(cx - heavy_half, cy - heavy_half, heavy, y + h - cy + heavy_half);

    if (left == 1)
        FILL(x, cy - light_half, cx - x + light_half, light);
    else if (left == 2)
        FILL(x, cy - heavy_half, cx - x + heavy_half, heavy);

    if (right == 1)
        FILL(cx - light_half, cy - light_half, x + w - cx + light_half, light);
    else if (right == 2)
        FILL(cx - heavy_half, cy - heavy_half, x + w - cx + heavy_half, heavy);

#undef FILL
}

// Draw double box lines (weight 3) using 4 sub-lines with proper corner connections.
// The 4 sub-lines are: left-v (at cx-off), right-v (at cx+off),
// top-h (at cy-off), bot-h (at cy+off).
// At corners, outer sub-lines connect to outer, inner to inner, forming L-shapes.
// All coordinates are intentionally integer-truncated for pixel-aligned rendering.
static void draw_double_lines(SDL_Renderer *renderer,
                              int up, int down, int left, int right,
                              int x, int y, int w, int h,
                              int light)
{
    int cx = x + w / 2;
    int cy = y + h / 2;
    int off = light + (light + 1) / 2; // offset from center
    int lw = light;                    // sub-line width
    int lw_half = lw / 2;

    // Sub-line center positions
    int lv_x = cx - off;
    int rv_x = cx + off;
    int th_y = cy - off;
    int bh_y = cy + off;

    // Pixel edges of each sub-line's drawn rect. Horizontal and vertical
    // sub-lines must connect at these edges (not centers) to form seamless
    // corners without gaps.
    int lv_left = lv_x - lw_half;
    int lv_right = lv_left + lw;
    int rv_left = rv_x - lw_half;
    int rv_right = rv_left + lw;
    int th_top = th_y - lw_half;
    int th_bot = th_top + lw;
    int bh_top = bh_y - lw_half;
    int bh_bot = bh_top + lw;

    bool du = (up == 3), dd = (down == 3), dl = (left == 3), dr = (right == 3);
    bool has_dv = du || dd;
    bool has_dh = dl || dr;

#define FILL(rx, ry, rw, rh)                                                   \
    do {                                                                       \
        SDL_FRect _r = { (float)(rx), (float)(ry), (float)(rw), (float)(rh) }; \
        SDL_RenderFillRect(renderer, &_r);                                     \
    } while (0)

    // --- Vertical sub-lines ---
    if (has_dv) {
        int lv_y1, lv_y2, rv_y1, rv_y2;

        // Top endpoints
        if (du) {
            lv_y1 = y;
            rv_y1 = y;
        } else if (has_dh) {
            // Only going down. Determine corner pairing:
            // down+right (╔-like): outer = left-v/top-h, inner = right-v/bot-h
            // down+left  (╗-like): outer = right-v/top-h, inner = left-v/bot-h
            // down+both  (╦-like): both verticals start at bot-h
            if (dr && !dl) {
                lv_y1 = th_top;
                rv_y1 = bh_top;
            } else if (dl && !dr) {
                rv_y1 = th_top;
                lv_y1 = bh_top;
            } else {
                lv_y1 = bh_top;
                rv_y1 = bh_top;
            }
        } else {
            lv_y1 = cy - lw_half;
            rv_y1 = cy - lw_half;
        }

        // Bottom endpoints
        if (dd) {
            lv_y2 = y + h;
            rv_y2 = y + h;
        } else if (has_dh) {
            // Only going up.
            // up+right (╚-like): outer = left-v/bot-h, inner = right-v/top-h
            // up+left  (╝-like): outer = right-v/bot-h, inner = left-v/top-h
            // up+both  (╩-like): both verticals end at top-h
            if (dr && !dl) {
                lv_y2 = bh_bot;
                rv_y2 = th_bot;
            } else if (dl && !dr) {
                rv_y2 = bh_bot;
                lv_y2 = th_bot;
            } else {
                lv_y2 = th_bot;
                rv_y2 = th_bot;
            }
        } else {
            lv_y2 = cy + lw_half;
            rv_y2 = cy + lw_half;
        }

        if (lv_y2 > lv_y1)
            FILL(lv_left, lv_y1, lw, lv_y2 - lv_y1);
        if (rv_y2 > rv_y1)
            FILL(rv_left, rv_y1, lw, rv_y2 - rv_y1);
    }

    // --- Horizontal sub-lines ---
    if (has_dh) {
        int th_x1, th_x2, bh_x1, bh_x2;

        // Left endpoints
        if (dl) {
            th_x1 = x;
            bh_x1 = x;
        } else if (has_dv) {
            // Only going right.
            // down+right (╔-like): outer = top-h/left-v, inner = bot-h/right-v
            // up+right   (╚-like): outer = bot-h/left-v, inner = top-h/right-v
            // both+right (╠-like): both horizontals start at right-v
            if (dd && !du) {
                th_x1 = lv_left;
                bh_x1 = rv_left;
            } else if (du && !dd) {
                bh_x1 = lv_left;
                th_x1 = rv_left;
            } else {
                th_x1 = rv_left;
                bh_x1 = rv_left;
            }
        } else {
            th_x1 = cx - lw_half;
            bh_x1 = cx - lw_half;
        }

        // Right endpoints
        if (dr) {
            th_x2 = x + w;
            bh_x2 = x + w;
        } else if (has_dv) {
            // Only going left.
            // down+left (╗-like): outer = top-h/right-v, inner = bot-h/left-v
            // up+left   (╝-like): outer = bot-h/right-v, inner = top-h/left-v
            // both+left (╣-like): both horizontals end at left-v
            if (dd && !du) {
                th_x2 = rv_right;
                bh_x2 = lv_right;
            } else if (du && !dd) {
                bh_x2 = rv_right;
                th_x2 = lv_right;
            } else {
                th_x2 = lv_right;
                bh_x2 = lv_right;
            }
        } else {
            th_x2 = cx + lw_half;
            bh_x2 = cx + lw_half;
        }

        if (th_x2 > th_x1)
            FILL(th_x1, th_top, th_x2 - th_x1, lw);
        if (bh_x2 > bh_x1)
            FILL(bh_x1, bh_top, bh_x2 - bh_x1, lw);
    }

#undef FILL
}

static void draw_box_lines(SDL_Renderer *renderer, uint8_t enc,
                           int x, int y, int w, int h)
{
    int up = (enc >> 6) & 3;
    int down = (enc >> 4) & 3;
    int left = (enc >> 2) & 3;
    int right = (enc >> 0) & 3;

    // Uniform line thickness based on cell width (narrower dimension)
    int light = w / 5;
    if (light < 1)
        light = 1;
    int heavy = light * 3;
    if (heavy < light + 2)
        heavy = light + 2;

    // Draw single/heavy lines (weights 1 and 2)
    draw_single_heavy_lines(renderer, up, down, left, right,
                            x, y, w, h, light, heavy);

    // Draw double lines (weight 3)
    if (up == 3 || down == 3 || left == 3 || right == 3)
        draw_double_lines(renderer, up, down, left, right,
                          x, y, w, h, light);
}

static void draw_block_element(SDL_Renderer *renderer, uint32_t cp,
                               int x, int y, int w, int h)
{
    SDL_FRect rect;

    if (cp == 0x2580) {
        // Upper half block
        int half_h = h / 2;
        rect = (SDL_FRect){ (float)x, (float)y, (float)w, (float)half_h };
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    if (cp >= 0x2581 && cp <= 0x2588) {
        // Lower N/8 blocks (1/8 through full)
        int n = cp - 0x2580;
        int block_h = (h * n + 4) / 8;
        rect = (SDL_FRect){ (float)x, (float)(y + h - block_h), (float)w, (float)block_h };
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    if (cp >= 0x2589 && cp <= 0x258F) {
        // Left N/8 blocks (7/8 down to 1/8)
        int n = 0x2590 - cp;
        int block_w = (w * n + 4) / 8;
        rect = (SDL_FRect){ (float)x, (float)y, (float)block_w, (float)h };
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    if (cp == 0x2590) {
        // Right half block
        int half = w / 2;
        rect = (SDL_FRect){ (float)(x + w - half), (float)y, (float)half, (float)h };
        SDL_RenderFillRect(renderer, &rect);
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

        uint8_t cr, cg, cb, ca;
        SDL_GetRenderDrawColor(renderer, &cr, &cg, &cb, &ca);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, cr, cg, cb, alpha);
        rect = (SDL_FRect){ (float)x, (float)y, (float)w, (float)h };
        SDL_RenderFillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, cr, cg, cb, ca);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        return;
    }

    if (cp == 0x2594) {
        // Upper 1/8 block
        int block_h = (h + 4) / 8;
        if (block_h < 1)
            block_h = 1;
        rect = (SDL_FRect){ (float)x, (float)y, (float)w, (float)block_h };
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    if (cp == 0x2595) {
        // Right 1/8 block
        int block_w = (w + 4) / 8;
        if (block_w < 1)
            block_w = 1;
        rect = (SDL_FRect){ (float)(x + w - block_w), (float)y, (float)block_w, (float)h };
        SDL_RenderFillRect(renderer, &rect);
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

        if (bits & 0x01) {
            rect = (SDL_FRect){ (float)x, (float)(y + half_h), (float)half_w, (float)bottom_h };
            SDL_RenderFillRect(renderer, &rect);
        }
        if (bits & 0x02) {
            rect = (SDL_FRect){ (float)(x + half_w), (float)(y + half_h), (float)right_w, (float)bottom_h };
            SDL_RenderFillRect(renderer, &rect);
        }
        if (bits & 0x04) {
            rect = (SDL_FRect){ (float)x, (float)y, (float)half_w, (float)half_h };
            SDL_RenderFillRect(renderer, &rect);
        }
        if (bits & 0x08) {
            rect = (SDL_FRect){ (float)(x + half_w), (float)y, (float)right_w, (float)half_h };
            SDL_RenderFillRect(renderer, &rect);
        }
    }
}

void rend_sdl3_boxdraw_draw(SDL_Renderer *renderer, uint32_t cp,
                            int cell_x, int cell_y, int cell_w, int cell_h,
                            uint8_t r, uint8_t g, uint8_t b)
{
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);

    if (cp >= 0x2500 && cp <= 0x257F) {
        uint8_t enc = get_box_encoding(cp);
        if (enc != 0)
            draw_box_lines(renderer, enc, cell_x, cell_y, cell_w, cell_h);
    } else if (cp >= 0x2580 && cp <= 0x259F) {
        draw_block_element(renderer, cp, cell_x, cell_y, cell_w, cell_h);
    }
}
