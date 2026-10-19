/*
 * portty — Shared macros, logging, and common utilities
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdarg.h>
#include <stdbool.h>

/* Global verbose flag - defined in main.c */
extern int verbose;

/* kitty-style text_composition_strategy knob (defined in main.c, set from
 * config). Applied as a uniform coverage curve to grayscale glyph alpha in
 * font_ft.c, on top of the renderer's linear-light blending. Neutral defaults
 * (gamma 1.0, contrast 0) leave coverage unchanged. */
extern float portty_text_gamma;
extern float portty_text_contrast;

/* Opacity of SGR 2 (dim/faint) foreground text, 0..1 (defined in main.c, set
 * from config). 1 renders dim text at full intensity (no dimming); lower
 * values blend the foreground toward the cell background. */
extern float portty_dim_opacity;

/* Verbose logging implementation - use vlog() macro instead */
void vlog_impl(const char *file, const char *func, int line, const char *format, ...);

/* Verbose logging macro - captures file, function, and line number */
#define vlog(fmt, ...) vlog_impl(__FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)

/* Shared constants */
#define CURSOR_BLINK_INTERVAL_MS 1000
#define SCROLL_LINES_PER_TICK    1
#define AUTOSCROLL_INTERVAL_MS   33

/* The cell-width contract this engine implements, declared to child
 * processes as PORTTY_WIDTH alongside the identity marker PORTTY=1 (see
 * pty.c / pty_w32.c). "uts51" means VS15 takes one cell where UTS #51
 * defines a text glyph for the base (coffer's TEXT_PRES rule) and VS16
 * forces two — the rules pinned by tests/test_term_cfr.c. A TUI can read
 * the variable instead of maintaining a terminal-name list; if the width
 * engine ever changes, change this one constant so the declaration and
 * the behavior move together. */
#define PORTTY_WIDTH_CONTRACT "uts51"

#endif /* COMMON_H */
