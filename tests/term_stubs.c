/*
 * Placeholder translation unit.
 *
 * term.c used to call sixel_image_free(), which this file stubbed for
 * unit tests. Sixel decoding now lives entirely in coffer, so term.c
 * has no such dependency and no stub is needed. Kept (with additions)
 * so existing test targets that list it keep building without a
 * Makefile change.
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

// Stub for terminal_get_scrollback_lines — only linked when term.c
// is not compiled into the test. Use weak symbol to avoid multiple
// definition errors when term.c is also linked.
__attribute__((weak)) int terminal_get_scrollback_lines(TerminalBackend *term)
{
    (void)term;
    return 0;
}

// Weak stubs for terminal functions used by portty_debug_script.c.
// Only active when term.c is not linked into the test binary.
__attribute__((weak)) int terminal_get_cell(TerminalBackend *term, int row,
                                            int col, TerminalCell *cell)
{
    (void)term;
    (void)row;
    (void)col;
    if (cell)
        memset(cell, 0, sizeof(*cell));
    return -1;
}

__attribute__((weak)) int terminal_get_scrollback_cell(TerminalBackend *term,
                                                       int sb_row, int col,
                                                       TerminalCell *cell)
{
    (void)term;
    (void)sb_row;
    (void)col;
    if (cell)
        memset(cell, 0, sizeof(*cell));
    return -1;
}

__attribute__((weak)) int terminal_get_dimensions(TerminalBackend *term,
                                                  int *rows, int *cols)
{
    (void)term;
    if (rows)
        *rows = 0;
    if (cols)
        *cols = 0;
    return 0;
}
