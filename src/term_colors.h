/*
 * portty — Portty-specific terminal color defaults sourced from coffer
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * Terminal color defaults — portty-specific, not ANSI palette.
 *
 * All palette colors are sourced from coffer at runtime via
 * term_colors_init(). coffer owns the Dracula palette (ANSI 0-15,
 * default bg/fg). The only portty-local color is DRACULA_SELECTION
 * (#44475A), a Dracula spec color not exposed by coffer's API.
 *
 * Alpha values are portty UI choices, not palette colors.
 */

#ifndef TERM_COLORS_H
#define TERM_COLORS_H

#include <stdint.h>

/* Dracula Selection color (#44475A) — not in coffer's ANSI palette. */
#define DRACULA_SELECTION 0x44475Au

typedef struct
{
    /* Background — cfr_default_bg_rgb() */
    uint8_t bg_r, bg_g, bg_b, bg_a;

    /* Cursor — cfr_default_palette_rgb(4) (Dracula purple) */
    uint8_t cursor_r, cursor_g, cursor_b;

    /* Default underline — cfr_default_palette_rgb(8) (Dracula comment) */
    uint8_t underline_r, underline_g, underline_b, underline_a;

    /* Selection overlay — DRACULA_SELECTION, portty-only */
    uint8_t selection_r, selection_g, selection_b, selection_a;

    /* Windows caption — DRACULA_SELECTION */
    uint8_t caption_r, caption_g, caption_b;
} TermColors;

/* Initialise all colors from coffer's API. Call once at startup. */
void term_colors_init(TermColors *c);

#endif /* TERM_COLORS_H */
