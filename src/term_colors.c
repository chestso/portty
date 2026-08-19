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

void term_colors_init(TermColors *c)
{
    uint32_t bg = cfr_default_bg_rgb();
    c->bg_r = rgb_r(bg);
    c->bg_g = rgb_g(bg);
    c->bg_b = rgb_b(bg);
    c->bg_a = 0xFF;

    uint32_t cursor = cfr_default_palette_rgb(4);
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
}
