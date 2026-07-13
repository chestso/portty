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
#include <stddef.h>

ssize_t pty_write(PtyContext *ctx, const char *data, size_t len)
{
    (void)ctx;
    (void)data;
    return (ssize_t)len;
}

// Stub for terminal_get_scrollback_lines — only linked when term.c
// is not compiled into the test. Use weak symbol to avoid multiple
// definition errors when term.c is also linked.
__attribute__((weak)) int terminal_get_scrollback_lines(void *term)
{
    (void)term;
    return 0;
}
