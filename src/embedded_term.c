/*
 * portty — Embedded terminal substrate for PTY-less coffer terminals
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

// EmbeddedTerm — see embedded_term.h. A thin owner of a heap coffer-backed
// TerminalBackend (term_cfr_new), giving the pager overlay and the
// notification panels one correct create/resize/destroy/feed routine instead
// of two hand-rolled (and drifting) copies.

#include "embedded_term.h"

#include "term_cfr.h"

#include <coffer/coffer.h>
#include <stdlib.h>
#include <string.h>

// Feed the document into the VT, translating bare LF to CRLF so lines start at
// column 0 (a VT treats LF as line-feed only). Mirrors pager.c's feed_text.
static void feed_text(TerminalBackend *term, const char *s)
{
    const char *run = s;
    for (const char *c = s; *c; c++) {
        if (*c == '\n' && (c == s || c[-1] != '\r')) {
            if (c > run)
                terminal_process_input(term, run, (size_t)(c - run));
            terminal_process_input(term, "\r\n", 2);
            run = c + 1;
        }
    }
    size_t tail = strlen(run);
    if (tail)
        terminal_process_input(term, run, tail);
}

bool embedded_term_init(EmbeddedTerm *et, int cols, int rows, int cell_w, int cell_h)
{
    if (!et || cols <= 0 || rows <= 0 || cell_w <= 0 || cell_h <= 0)
        return false;

    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.cols = cols;
    cfg.rows = rows;
    cfg.cell_w_px = cell_w;
    cfg.cell_h_px = cell_h;
    TerminalBackend *t = term_cfr_new(&cfg);
    if (!t)
        return false;

    et->term = t;
    et->cols = cols;
    et->rows = rows;
    return true;
}

bool embedded_term_ensure(EmbeddedTerm *et, int cols, int rows, int cell_w, int cell_h)
{
    if (!et)
        return false;
    if (et->term && et->cols == cols && et->rows == rows)
        return true;
    embedded_term_destroy(et);
    return embedded_term_init(et, cols, rows, cell_w, cell_h);
}

void embedded_term_destroy(EmbeddedTerm *et)
{
    if (!et || !et->term)
        return;
    terminal_destroy(et->term);
    free(et->term); // heap instance from term_cfr_new
    et->term = NULL;
    et->cols = 0;
    et->rows = 0;
}

void embedded_term_feed(EmbeddedTerm *et, const char *ansi)
{
    if (!et || !et->term || !ansi)
        return;
    feed_text(et->term, ansi);
    terminal_flush_damage(et->term);
}

bool embedded_term_active(const EmbeddedTerm *et)
{
    return et && et->term != NULL;
}
