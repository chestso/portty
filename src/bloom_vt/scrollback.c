/*
 * bloom-vt — scrollback as a doubly-linked page ring.
 *
 * Each page holds up to BVT_SB_PAGE_ROWS chronological rows. New
 * lines are appended to the head page (newest); when the head page
 * fills, a new page is allocated at the front. When the configured
 * line cap is exceeded, the tail page (oldest) is freed wholesale —
 * one `free` returns up to BVT_SB_PAGE_ROWS lines plus their style
 * intern table and grapheme arena. No fragmentation accumulates.
 *
 * Cells are re-interned into the receiving page's tables: style_id
 * and grapheme_id are page-scoped, so a cell pulled out of the active
 * grid must have its intern references reseated for the scrollback
 * page. This keeps page lifetimes independent.
 *
 * Layout convention:
 *   vt->sb_head  = newest page (push target)
 *   vt->sb_tail  = oldest page (eviction target)
 *   page->next   = older neighbor (toward tail)
 *   page->prev   = newer neighbor (toward head)
 *
 * Reading: sb_row 0 = newest. Within a page, rows are stored
 * chronologically; the newest in each page is at index `row_count - 1`.
 */

#include "bloom_vt_internal.h"

#include <string.h>

static BvtPage *new_sb_page(BvtTerm *vt, int cols)
{
    BvtPage *p = bvt_page_new(vt, BVT_SB_PAGE_ROWS, cols);
    if (!p)
        return NULL;
    /* bvt_page_new sets row_count to row_capacity (full page); for
     * scrollback we treat row_count as "rows-used-so-far" and grow it
     * up to row_capacity. Reset to empty. */
    p->row_count = 0;
    p->next = NULL;
    p->prev = NULL;
    return p;
}

static bool ensure_head(BvtTerm *vt, int cols)
{
    if (vt->sb_head && vt->sb_head->row_count < vt->sb_head->row_capacity && vt->sb_head->cols == cols) {
        return true;
    }
    BvtPage *page = new_sb_page(vt, cols);
    if (!page)
        return false;
    /* Insert as new head. */
    page->next = vt->sb_head;
    if (vt->sb_head)
        vt->sb_head->prev = page;
    vt->sb_head = page;
    if (!vt->sb_tail)
        vt->sb_tail = page;
    return true;
}

static void evict_to_capacity(BvtTerm *vt)
{
    while (vt->sb_lines > vt->sb_capacity && vt->sb_tail) {
        BvtPage *tail = vt->sb_tail;
        vt->sb_tail = tail->prev; /* newer than the evicted page */
        if (vt->sb_tail)
            vt->sb_tail->next = NULL;
        else
            vt->sb_head = NULL; /* the only page */
        vt->sb_lines -= tail->row_count;
        bvt_page_free(vt, tail);
    }
}

void bvt_scrollback_push(BvtTerm *vt, const BvtCell *src_cells, int cols,
                         bool wrapline)
{
    if (!vt || vt->sb_capacity <= 0 || cols <= 0 || !src_cells)
        return;
    if (!ensure_head(vt, cols))
        return;

    BvtPage *head = vt->sb_head;
    int row = head->row_count;
    BvtCell *dst = &head->cells[(size_t)row * head->cols];
    head->row_flags[row] = wrapline ? (uint8_t)BVT_CELL_WRAPLINE : 0u;

    /* Re-intern style and grapheme references into the head page. */
    for (int c = 0; c < cols && c < head->cols; ++c) {
        BvtCell src = src_cells[c];
        const BvtStyle *src_style =
            bvt_style_lookup(vt->grid, src.style_id);
        uint32_t new_style_id = src_style
                                    ? bvt_style_intern(vt, head, src_style)
                                    : 0u;
        uint32_t new_grapheme_id = 0u;
        if (src.grapheme_id != 0u) {
            uint32_t cps[BVT_CLUSTER_MAX];
            size_t n = bvt_grapheme_read(vt->grid, src.grapheme_id,
                                         cps, BVT_CLUSTER_MAX);
            if (n >= 2)
                new_grapheme_id = bvt_grapheme_intern(
                    vt, head, cps, (uint32_t)n);
        }
        dst[c] = src;
        dst[c].style_id = new_style_id;
        dst[c].grapheme_id = new_grapheme_id;
    }
    head->row_count++;
    vt->sb_lines++;
    evict_to_capacity(vt);
}

void bvt_scrollback_clear(BvtTerm *vt)
{
    if (!vt)
        return;
    BvtPage *p = vt->sb_head;
    while (p) {
        BvtPage *next = p->next;
        bvt_page_free(vt, p);
        p = next;
    }
    vt->sb_head = NULL;
    vt->sb_tail = NULL;
    vt->sb_lines = 0;
}

static const BvtPage *find_sb_row(const BvtTerm *vt, int sb_row,
                                  int *out_row_in_page)
{
    if (!vt || sb_row < 0)
        return NULL;
    int remaining = sb_row;
    const BvtPage *p = vt->sb_head;
    while (p) {
        if (remaining < p->row_count) {
            *out_row_in_page = p->row_count - 1 - remaining;
            return p;
        }
        remaining -= p->row_count;
        p = p->next; /* toward older */
    }
    return NULL;
}

const BvtCell *bvt_get_scrollback_cell(const BvtTerm *vt, int sb_row, int col)
{
    int row_in_page = 0;
    const BvtPage *p = find_sb_row(vt, sb_row, &row_in_page);
    if (!p)
        return NULL;
    if (col < 0 || col >= p->cols)
        return NULL;
    return &p->cells[(size_t)row_in_page * p->cols + col];
}

bool bvt_get_scrollback_wrapline(const BvtTerm *vt, int sb_row)
{
    int row_in_page = 0;
    const BvtPage *p = find_sb_row(vt, sb_row, &row_in_page);
    if (!p)
        return false;
    return (p->row_flags[row_in_page] & BVT_CELL_WRAPLINE) != 0u;
}

const BvtPage *bvt_find_owner_page(const BvtTerm *vt, const BvtCell *cell)
{
    if (!vt || !cell)
        return NULL;
    if (vt->grid) {
        size_t n = (size_t)vt->grid->row_capacity * vt->grid->cols;
        if (cell >= vt->grid->cells && cell < vt->grid->cells + n)
            return vt->grid;
    }
    if (vt->altgrid) {
        size_t n = (size_t)vt->altgrid->row_capacity * vt->altgrid->cols;
        if (cell >= vt->altgrid->cells && cell < vt->altgrid->cells + n)
            return vt->altgrid;
    }
    for (const BvtPage *p = vt->sb_head; p; p = p->next) {
        size_t n = (size_t)p->row_capacity * p->cols;
        if (cell >= p->cells && cell < p->cells + n)
            return p;
    }
    return NULL;
}
