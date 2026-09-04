/*
 * portty — Embedded terminal substrate interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef EMBEDDED_TERM_H
#define EMBEDDED_TERM_H

#include "term.h"

#include <stdbool.h>

/* A TerminalBackend instance created via term_cfr_new, owned as one unit:
 * create/resize/destroy plus ANSI feeding. This is the shared substrate for
 * portty's embedded terminals — the pager overlay and notification panels
 * both build on it; each adds its own policy (modal input vs. UI chrome) on
 * top. An EmbeddedTerm with term == NULL is torn down / never built. */
typedef struct EmbeddedTerm
{
    TerminalBackend *term; /* heap instance from term_cfr_new; NULL when down */
    int cols, rows;        /* current grid size (single source of truth) */
} EmbeddedTerm;

/* Create a fresh terminal at (cols x rows) using the host cell metrics.
 * Returns false on failure (et is left torn down). */
bool embedded_term_init(EmbeddedTerm *et, int cols, int rows, int cell_w, int cell_h);

/* Ensure the terminal exists at exactly (cols x rows), recreating it when
 * the dimensions differ from the current ones. On a size change the old
 * terminal is destroyed and replaced with a fresh one — callers that need
 * content preserved across the resize must re-feed it afterwards. Returns
 * false on failure; the prior terminal is already gone in that case. */
bool embedded_term_ensure(EmbeddedTerm *et, int cols, int rows, int cell_w, int cell_h);

/* Destroy the terminal and clear the size (no-op when already down). */
void embedded_term_destroy(EmbeddedTerm *et);

/* Feed ANSI text to the terminal. Bare LF is translated to CRLF so lines
 * start at column 0 (a VT treats LF as line-feed only) — the same
 * normalisation pager.c has always done. No-op when torn down. */
void embedded_term_feed(EmbeddedTerm *et, const char *ansi);

/* True while a terminal is alive. */
bool embedded_term_active(const EmbeddedTerm *et);

#endif /* EMBEDDED_TERM_H */
