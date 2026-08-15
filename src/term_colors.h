/*
 * portty — Portty-specific terminal color defaults
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * Terminal color defaults — portty-specific, not ANSI palette.
 *
 * The ANSI 0-15 palette lives in coffer's palette.c (CharmTone-based).
 * These are portty-specific colors for cursor, selection, panels, etc.
 * All colors reference charmtones.h via CHARMTONE_R/G/B macros.
 *
 * Exceptions (not charmtones):
 *   - TERM_BG: true black (#000000)
 *   - TERM_FG: cream (#FFFDF5, matches coffer's ANSI 15)
 */

#ifndef TERM_COLORS_H
#define TERM_COLORS_H

#include "charmtones.h"

/* ------------------------------------------------------------------ */
/* Exceptions — not charmtones                                        */
/* ------------------------------------------------------------------ */

/* Default background — true black */
#define TERM_BG_R 0x00
#define TERM_BG_G 0x00
#define TERM_BG_B 0x00
#define TERM_BG_A 0xFF

/* Default foreground — cream (matches coffer's bright white/ANSI 15) */
#define TERM_FG_R 0xFF
#define TERM_FG_G 0xFD
#define TERM_FG_B 0xF5
#define TERM_FG_A 0xFF

/* ------------------------------------------------------------------ */
/* Terminal UI colors — derived from CharmTone                        */
/* ------------------------------------------------------------------ */

/* Cursor — Charple (ANSI blue) */
#define TERM_CURSOR_R CHARMTONE_R(CHARMTONE_CHARPLE)
#define TERM_CURSOR_G CHARMTONE_G(CHARMTONE_CHARPLE)
#define TERM_CURSOR_B CHARMTONE_B(CHARMTONE_CHARPLE)
#define TERM_CURSOR_A CHARMTONE_A(CHARMTONE_CHARPLE)

/* Default underline — Squid (neutral gray) */
#define TERM_UNDERLINE_R CHARMTONE_R(CHARMTONE_SQUID)
#define TERM_UNDERLINE_G CHARMTONE_G(CHARMTONE_SQUID)
#define TERM_UNDERLINE_B CHARMTONE_B(CHARMTONE_SQUID)
#define TERM_UNDERLINE_A CHARMTONE_A(CHARMTONE_SQUID)

/* Selection overlay — Iron (dark neutral) @ 86% opacity */
#define TERM_SELECTION_R CHARMTONE_R(CHARMTONE_IRON)
#define TERM_SELECTION_G CHARMTONE_G(CHARMTONE_IRON)
#define TERM_SELECTION_B CHARMTONE_B(CHARMTONE_IRON)
#define TERM_SELECTION_A 220

/* ------------------------------------------------------------------ */
/* Panel colors — derived from CharmTone                              */
/* ------------------------------------------------------------------ */

/* Panel background — BBQ (dark neutral) */
#define TERM_PANEL_BG_R CHARMTONE_R(CHARMTONE_BBQ)
#define TERM_PANEL_BG_G CHARMTONE_G(CHARMTONE_BBQ)
#define TERM_PANEL_BG_B CHARMTONE_B(CHARMTONE_BBQ)
#define TERM_PANEL_BG_A 0xFF

/* Error accent — Sriracha */
#define TERM_ACCENT_ERROR_R CHARMTONE_R(CHARMTONE_SRIRACHA)
#define TERM_ACCENT_ERROR_G CHARMTONE_G(CHARMTONE_SRIRACHA)
#define TERM_ACCENT_ERROR_B CHARMTONE_B(CHARMTONE_SRIRACHA)
#define TERM_ACCENT_ERROR_A 0xFF

/* Warning accent — Mustard */
#define TERM_ACCENT_WARNING_R CHARMTONE_R(CHARMTONE_MUSTARD)
#define TERM_ACCENT_WARNING_G CHARMTONE_G(CHARMTONE_MUSTARD)
#define TERM_ACCENT_WARNING_B CHARMTONE_B(CHARMTONE_MUSTARD)
#define TERM_ACCENT_WARNING_A 0xFF

/* Default accent — Thunder */
#define TERM_ACCENT_DEFAULT_R CHARMTONE_R(CHARMTONE_THUNDER)
#define TERM_ACCENT_DEFAULT_G CHARMTONE_G(CHARMTONE_THUNDER)
#define TERM_ACCENT_DEFAULT_B CHARMTONE_B(CHARMTONE_THUNDER)
#define TERM_ACCENT_DEFAULT_A 0xFF

/* Close button foreground — Steam (normal) */
#define TERM_CLOSE_FG_R CHARMTONE_R(CHARMTONE_STEAM)
#define TERM_CLOSE_FG_G CHARMTONE_G(CHARMTONE_STEAM)
#define TERM_CLOSE_FG_B CHARMTONE_B(CHARMTONE_STEAM)
#define TERM_CLOSE_FG_A 0xFF

/* Close button foreground — Salt (hover) */
#define TERM_CLOSE_FG_HOVER_R CHARMTONE_R(CHARMTONE_SALT)
#define TERM_CLOSE_FG_HOVER_G CHARMTONE_G(CHARMTONE_SALT)
#define TERM_CLOSE_FG_HOVER_B CHARMTONE_B(CHARMTONE_SALT)
#define TERM_CLOSE_FG_HOVER_A 0xFF

/* Windows caption — BBQ (matches panel bg) */
#define TERM_CAPTION_R CHARMTONE_R(CHARMTONE_BBQ)
#define TERM_CAPTION_G CHARMTONE_G(CHARMTONE_BBQ)
#define TERM_CAPTION_B CHARMTONE_B(CHARMTONE_BBQ)

#endif /* TERM_COLORS_H */
