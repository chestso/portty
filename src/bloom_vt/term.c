/*
 * bloom-vt — lifecycle + public API entry points.
 *
 * This file is the seam between bloom_vt.h and the internal subsystems.
 * Behavior for grid/parser/etc is implemented in their dedicated files;
 * here we own creation, destruction, allocator routing, and dispatch.
 */

#include "bloom_vt_internal.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Allocator                                                           */
/* ------------------------------------------------------------------ */

static void *stdlib_alloc(size_t size, void *user)
{
    (void)user;
    return malloc(size);
}
static void *stdlib_realloc(void *ptr, size_t size, void *user)
{
    (void)user;
    return realloc(ptr, size);
}
static void stdlib_free(void *ptr, void *user)
{
    (void)user;
    free(ptr);
}
static const BvtAllocator BVT_STDLIB_ALLOCATOR = {
    .alloc = stdlib_alloc,
    .realloc = stdlib_realloc,
    .free = stdlib_free,
    .user = NULL,
};

void *bvt_alloc(BvtTerm *vt, size_t size)
{
    return vt->alloc.alloc(size, vt->alloc.user);
}
void *bvt_realloc(BvtTerm *vt, void *ptr, size_t size)
{
    return vt->alloc.realloc(ptr, size, vt->alloc.user);
}
void bvt_dealloc(BvtTerm *vt, void *ptr)
{
    if (ptr)
        vt->alloc.free(ptr, vt->alloc.user);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

BvtTerm *bvt_new(int rows, int cols)
{
    return bvt_new_with_allocator(rows, cols, NULL);
}

BvtTerm *bvt_new_with_allocator(int rows, int cols, const BvtAllocator *alloc)
{
    if (rows <= 0 || cols <= 0)
        return NULL;

    const BvtAllocator *a = alloc ? alloc : &BVT_STDLIB_ALLOCATOR;
    BvtTerm *vt = a->alloc(sizeof(*vt), a->user);
    if (!vt)
        return NULL;

    memset(vt, 0, sizeof(*vt));
    vt->alloc = *a;
    vt->rows = rows;
    vt->cols = cols;
    vt->scroll_top = 0;
    vt->scroll_bottom = rows - 1;
    vt->sb_capacity = BVT_DEFAULT_SCROLLBACK;
    vt->cursor.visible = true;
    vt->cursor.blink = true;
    vt->cursor.pen.color_flags =
        BVT_COLOR_DEFAULT_FG | BVT_COLOR_DEFAULT_BG | BVT_COLOR_DEFAULT_UL;
    vt->modes[BVT_MODE_CURSOR_VISIBLE] = true;
    vt->modes[BVT_MODE_CURSOR_BLINK] = true;
    vt->charset[0] = vt->charset[1] = vt->charset[2] = vt->charset[3] = 'B';
    vt->charset_active = 0;

    bvt_parser_init(&vt->parser);

    /* Tab stops every 8 columns by default. */
    vt->tabstops = vt->alloc.alloc((size_t)cols, vt->alloc.user);
    if (!vt->tabstops) {
        vt->alloc.free(vt, vt->alloc.user);
        return NULL;
    }
    for (int i = 0; i < cols; ++i)
        vt->tabstops[i] = (i % 8 == 0) ? 1u : 0u;

    /* Grid + altgrid pages allocated lazily once grid.c is implemented. */
    vt->grid = NULL;
    vt->altgrid = NULL;

    return vt;
}

void bvt_free(BvtTerm *vt)
{
    if (!vt)
        return;

    /* Drop scrollback pages. */
    BvtPage *p = vt->sb_head;
    while (p) {
        BvtPage *next = p->next;
        bvt_page_free(vt, p);
        p = next;
    }

    if (vt->grid)
        bvt_page_free(vt, vt->grid);
    if (vt->altgrid)
        bvt_page_free(vt, vt->altgrid);

    bvt_dealloc(vt, vt->tabstops);
    bvt_dealloc(vt, vt->title);

    /* Use saved allocator (vt->alloc.free) — vt itself is freed last. */
    BvtAllocator a = vt->alloc;
    a.free(vt, a.user);
}

void bvt_set_callbacks(BvtTerm *vt, const BvtCallbacks *cb, void *user)
{
    if (!vt)
        return;
    if (cb)
        vt->callbacks = *cb;
    else
        memset(&vt->callbacks, 0, sizeof(vt->callbacks));
    vt->callback_user = user;
}

void bvt_resize(BvtTerm *vt, int rows, int cols)
{
    if (!vt || rows <= 0 || cols <= 0)
        return;
    if (rows == vt->rows && cols == vt->cols)
        return;
    bvt_reflow(vt, rows, cols);
}

void bvt_set_reflow(BvtTerm *vt, bool enabled)
{
    if (vt)
        vt->reflow_enabled = enabled;
}
void bvt_set_ambiguous_wide(BvtTerm *vt, bool wide)
{
    if (vt)
        vt->ambiguous_wide = wide;
}
void bvt_set_scrollback_size(BvtTerm *vt, int lines)
{
    if (vt && lines >= 0)
        vt->sb_capacity = lines;
}

/* ------------------------------------------------------------------ */
/* I/O                                                                 */
/* ------------------------------------------------------------------ */

size_t bvt_input_write(BvtTerm *vt, const uint8_t *bytes, size_t len)
{
    if (!vt || !bytes || len == 0)
        return 0;
    bvt_parser_feed(vt, bytes, len);
    /* Commit any in-flight grapheme cluster. Real PTY frames almost
     * never split clusters across writes; for the rare exception
     * callers can chain writes without intervening reads. */
    bvt_flush_cluster(vt);
    return len;
}

/* bvt_send_key, bvt_send_text, bvt_send_mouse, bvt_paste_begin,
 * bvt_paste_end are implemented in keys.c. */

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

const BvtCell *bvt_get_cell(const BvtTerm *vt, int row, int col)
{
    if (!vt || !vt->grid)
        return NULL;
    if (row < 0 || row >= vt->rows)
        return NULL;
    if (col < 0 || col >= vt->cols)
        return NULL;
    return &vt->grid->cells[(size_t)row * vt->cols + col];
}

void bvt_get_dimensions(const BvtTerm *vt, int *out_rows, int *out_cols)
{
    if (!vt)
        return;
    if (out_rows)
        *out_rows = vt->rows;
    if (out_cols)
        *out_cols = vt->cols;
}

int bvt_get_scrollback_lines(const BvtTerm *vt)
{
    return vt ? vt->sb_lines : 0;
}

/* bvt_get_scrollback_cell, bvt_get_scrollback_wrapline are implemented
 * in scrollback.c. */

BvtCursor bvt_get_cursor(const BvtTerm *vt)
{
    BvtCursor out = { 0 };
    if (!vt)
        return out;
    out.row = vt->cursor.row;
    out.col = vt->cursor.col;
    out.visible = vt->cursor.visible;
    out.blink = vt->cursor.blink;
    return out;
}

const char *bvt_get_title(const BvtTerm *vt)
{
    return vt ? vt->title : NULL;
}

bool bvt_is_altscreen(const BvtTerm *vt)
{
    return vt ? vt->in_altscreen : false;
}

bool bvt_get_mode(const BvtTerm *vt, BvtMode mode)
{
    if (!vt)
        return false;
    if ((unsigned)mode >= sizeof(vt->modes) / sizeof(vt->modes[0]))
        return false;
    return vt->modes[mode];
}

bool bvt_get_line_continuation(const BvtTerm *vt, int row)
{
    if (!vt || !vt->grid)
        return false;
    if (row < 0 || row >= vt->rows)
        return false;
    return (vt->grid->row_flags[row] & BVT_CELL_WRAPLINE) != 0u;
}

const BvtStyle *bvt_cell_style(const BvtTerm *vt, const BvtCell *cell)
{
    if (!vt || !cell)
        return NULL;
    const BvtPage *page = bvt_find_owner_page(vt, cell);
    if (!page)
        return NULL;
    return bvt_style_lookup(page, cell->style_id);
}

size_t bvt_cell_get_grapheme(const BvtTerm *vt, const BvtCell *cell,
                             uint32_t *out, size_t out_cap)
{
    if (!vt || !cell || !out || out_cap == 0)
        return 0;
    if (cell->grapheme_id == 0) {
        out[0] = cell->cp;
        return 1;
    }
    const BvtPage *page = bvt_find_owner_page(vt, cell);
    if (!page)
        return 0;
    return bvt_grapheme_read(page, cell->grapheme_id, out, out_cap);
}
