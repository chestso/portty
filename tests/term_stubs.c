/*
 * Placeholder translation unit.
 *
 * term.c used to call sixel_image_free(), which this file stubbed for
 * unit tests. Sixel decoding now lives entirely in bloom-vt, so term.c
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
