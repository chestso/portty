/*
 * bloom-vt — page allocation.
 *
 * A page is allocated as a single block: the BvtPage header followed
 * by a flexible-array storage region holding the cell grid and
 * per-row flag bytes. Style intern entries and grapheme arena buffers
 * live separately and grow lazily; a page that never sees a non-default
 * style or multi-codepoint cluster never allocates them.
 *
 * `bvt_page_free` returns the cells+flags block in one free, plus up
 * to four optional frees for the dynamic sub-tables.
 */

#include "bloom_vt_internal.h"

#include <stdint.h>
#include <string.h>

/* Round `n` up to a multiple of `align` (must be power of two). */
static size_t align_up(size_t n, size_t align)
{
    return (n + (align - 1)) & ~(align - 1);
}

BvtPage *bvt_page_new(BvtTerm *vt, int rows, int cols)
{
    if (rows <= 0 || cols <= 0)
        return NULL;

    size_t header_bytes = sizeof(BvtPage);
    size_t cells_bytes = (size_t)rows * (size_t)cols * sizeof(BvtCell);
    size_t flags_bytes = align_up((size_t)rows, 4u);

    /* Cells must start on a 4-byte boundary; the BvtPage header is
     * already aligned by the allocator (>= 8 on every supported
     * target), and we lay them out cells-then-flags. */
    size_t total = header_bytes + cells_bytes + flags_bytes;

    BvtPage *p = bvt_alloc(vt, total);
    if (!p)
        return NULL;

    memset(p, 0, sizeof(*p));
    p->cols = (uint16_t)cols;
    p->row_capacity = (uint16_t)rows;
    p->row_count = (uint16_t)rows;

    /* `storage` is the trailing flexible array. */
    uint8_t *base = (uint8_t *)p + header_bytes;
    p->cells = (BvtCell *)base;
    p->row_flags = base + cells_bytes;

    /* Cells and flags are zero-initialized via the allocation memset
     * above? No — bvt_alloc returns uninitialized memory. We zero the
     * trailing region explicitly. */
    memset(p->cells, 0, cells_bytes + flags_bytes);

    return p;
}

void bvt_page_free(BvtTerm *vt, BvtPage *page)
{
    if (!page)
        return;
    /* Dynamic sub-tables (may be NULL if the page never used them). */
    bvt_dealloc(vt, page->styles.entries);
    bvt_dealloc(vt, page->styles.index);
    bvt_dealloc(vt, page->graphemes.codepoints);
    bvt_dealloc(vt, page->graphemes.dedup_index);
    /* The page header + cells + row_flags are one allocation. */
    bvt_dealloc(vt, page);
}
