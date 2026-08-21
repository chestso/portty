/*
 * portty — Test stubs for terminal backend functions
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * Stub translation unit for tests that don't link term.c.
 *
 * Provides minimal implementations of terminal functions that rend_common.c
 * and portty_debug_script.c call. These are hard (non-weak) definitions —
 * tests that DO link term.c must NOT also link this file.
 */

#include "portty_pty.h"
#include "term.h"
#include <stddef.h>
#include <string.h>

ssize_t pty_write(PtyContext *ctx, const char *data, size_t len)
{
    (void)ctx;
    (void)data;
    return (ssize_t)len;
}

int terminal_get_scrollback_lines(TerminalBackend *term)
{
    (void)term;
    return 0;
}

int terminal_get_cell(TerminalBackend *term, int row, int col, TerminalCell *cell)
{
    (void)term;
    (void)row;
    (void)col;
    if (cell)
        memset(cell, 0, sizeof(*cell));
    return -1;
}

int terminal_get_scrollback_cell(TerminalBackend *term, int sb_row, int col,
                                 TerminalCell *cell)
{
    (void)term;
    (void)sb_row;
    (void)col;
    if (cell)
        memset(cell, 0, sizeof(*cell));
    return -1;
}

int terminal_get_dimensions(TerminalBackend *term, int *rows, int *cols)
{
    (void)term;
    if (rows)
        *rows = 0;
    if (cols)
        *cols = 0;
    return 0;
}

uint16_t terminal_hovered_hyperlink(const TerminalBackend *term)
{
    (void)term;
    return 0;
}

const CfrSixel *terminal_get_sixels(TerminalBackend *term, int *count)
{
    (void)term;
    if (count)
        *count = 0;
    return NULL;
}
