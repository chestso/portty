/*
 * portty — Portty-specific terminal color defaults sourced from coffer
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "term_colors.h"
#include <coffer/coffer.h>

static uint8_t rgb_r(uint32_t c) { return (uint8_t)((c >> 16) & 0xFF); }
static uint8_t rgb_g(uint32_t c) { return (uint8_t)((c >> 8) & 0xFF); }
static uint8_t rgb_b(uint32_t c) { return (uint8_t)(c & 0xFF); }

uint32_t term_cursor_default_rgb(void)
{
    return cfr_default_palette_rgb(4); /* Dracula purple */
}

void term_colors_init(TermColors *c, const PorttyConf *conf)
{
    uint32_t bg = cfr_default_bg_rgb();
    c->bg_r = rgb_r(bg);
    c->bg_g = rgb_g(bg);
    c->bg_b = rgb_b(bg);
    c->bg_a = 0xFF;

    uint32_t cursor = term_cursor_default_rgb();
    c->cursor_r = rgb_r(cursor);
    c->cursor_g = rgb_g(cursor);
    c->cursor_b = rgb_b(cursor);

    uint32_t underline = cfr_default_palette_rgb(8);
    c->underline_r = rgb_r(underline);
    c->underline_g = rgb_g(underline);
    c->underline_b = rgb_b(underline);
    c->underline_a = 0xFF;

    c->selection_r = rgb_r(DRACULA_SELECTION);
    c->selection_g = rgb_g(DRACULA_SELECTION);
    c->selection_b = rgb_b(DRACULA_SELECTION);
    c->selection_a = 220;

    c->caption_r = rgb_r(DRACULA_SELECTION);
    c->caption_g = rgb_g(DRACULA_SELECTION);
    c->caption_b = rgb_b(DRACULA_SELECTION);

    c->cursor_text_r = c->cursor_text_g = c->cursor_text_b = 0;
    c->cursor_text_set = false;

    /* Config overrides (background / cursor_color / cursor_text_color). */
    if (conf) {
        if (conf->bg_color.set) {
            c->bg_r = rgb_r(conf->bg_color.rgb);
            c->bg_g = rgb_g(conf->bg_color.rgb);
            c->bg_b = rgb_b(conf->bg_color.rgb);
        }
        if (conf->cursor_color.set) {
            c->cursor_r = rgb_r(conf->cursor_color.rgb);
            c->cursor_g = rgb_g(conf->cursor_color.rgb);
            c->cursor_b = rgb_b(conf->cursor_color.rgb);
        }
        if (conf->cursor_text_color.set) {
            c->cursor_text_r = rgb_r(conf->cursor_text_color.rgb);
            c->cursor_text_g = rgb_g(conf->cursor_text_color.rgb);
            c->cursor_text_b = rgb_b(conf->cursor_text_color.rgb);
            c->cursor_text_set = true;
        }
    }
}
