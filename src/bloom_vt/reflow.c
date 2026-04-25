/*
 * bloom-vt — WRAPLINE-tagged reflow on resize.
 *
 * Algorithm (Alacritty / foot lineage):
 *   1. Walk the active grid top-to-bottom, collecting cells into a flat
 *      buffer. Continuation cells (width=0) and uninitialized cells are
 *      skipped — the rewrapper rebuilds continuations and trailing
 *      blanks from cell width.
 *   2. Logical lines are sequences of grid rows linked by the WRAPLINE
 *      flag; ends accumulate into `line_ends`. Each entry is the
 *      cell index where the next logical line begins.
 *   3. Cursor position is captured as `cursor_offset` — the index of
 *      the cell at-or-after which the cursor sits in the flat buffer.
 *   4. Re-wrap: for each logical line, lay cells into rows of width
 *      new_cols. Width-2 clusters that don't fit terminate the row
 *      early (logical line continues). Cursor placement falls out of
 *      tracking the row that contains `cursor_offset`.
 *   5. Distribute: the oldest rows beyond `new_rows` are pushed to
 *      scrollback; the last new_rows land in the new active grid.
 *
 * Scope: this first cut reflows only the active grid. Pre-existing
 * scrollback stays at its original column widths (mixed-cols sb pages
 * are well-supported by the iterator). A future revision can join sb
 * + grid into one logical stream and re-emit both.
 *
 * Altscreen and "reflow disabled" both fall through to a clamp-only
 * resize that preserves the existing top-left corner of content.
 */

#include "bloom_vt_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
    BvtCell cell;
    const BvtPage *src_page;
} CellRef;

typedef struct
{
    uint32_t start; /* inclusive index into cells[] */
    uint32_t end;   /* exclusive */
    bool wraps;
} NewRow;

typedef struct
{
    CellRef *cells;
    uint32_t cells_count, cells_cap;
    uint32_t *line_ends; /* exclusive cell-index where each logical line ends */
    uint32_t lines_count, lines_cap;
} Collected;

static bool grow_cells(BvtTerm *vt, Collected *c, uint32_t need)
{
    if (c->cells_count + need <= c->cells_cap)
        return true;
    uint32_t nc = c->cells_cap ? c->cells_cap : 256;
    while (c->cells_count + need > nc)
        nc *= 2;
    CellRef *p = bvt_realloc(vt, c->cells, (size_t)nc * sizeof(CellRef));
    if (!p)
        return false;
    c->cells = p;
    c->cells_cap = nc;
    return true;
}

static bool grow_lines(BvtTerm *vt, Collected *c)
{
    if (c->lines_count < c->lines_cap)
        return true;
    uint32_t nc = c->lines_cap ? c->lines_cap * 2 : 32;
    uint32_t *p = bvt_realloc(vt, c->line_ends, (size_t)nc * sizeof(uint32_t));
    if (!p)
        return false;
    c->line_ends = p;
    c->lines_cap = nc;
    return true;
}

/* ------------------------------------------------------------------ */
/* Clamp-only resize (altscreen / reflow disabled).                    */
/* ------------------------------------------------------------------ */

void bvt_resize_clamp(BvtTerm *vt, int new_rows, int new_cols)
{
    if (new_rows <= 0 || new_cols <= 0)
        return;
    BvtPage *old_grid = vt->grid;

    BvtPage *new_grid = bvt_page_new(vt, new_rows, new_cols);
    if (!new_grid)
        return;

    if (old_grid) {
        int copy_rows = (new_rows < vt->rows) ? new_rows : vt->rows;
        int copy_cols = (new_cols < vt->cols) ? new_cols : vt->cols;
        for (int r = 0; r < copy_rows; ++r) {
            for (int c = 0; c < copy_cols; ++c) {
                BvtCell src = old_grid->cells[(size_t)r * vt->cols + c];
                /* Re-intern style + grapheme refs into the new page. */
                const BvtStyle *st = bvt_style_lookup(old_grid, src.style_id);
                src.style_id =
                    st ? bvt_style_intern(vt, new_grid, st) : 0u;
                if (src.grapheme_id != 0u) {
                    uint32_t cps[BVT_CLUSTER_MAX];
                    size_t n = bvt_grapheme_read(old_grid, src.grapheme_id,
                                                 cps, BVT_CLUSTER_MAX);
                    src.grapheme_id =
                        (n >= 2) ? bvt_grapheme_intern(vt, new_grid, cps,
                                                       (uint32_t)n)
                                 : 0u;
                }
                new_grid->cells[(size_t)r * new_cols + c] = src;
            }
            new_grid->row_flags[r] = old_grid->row_flags[r];
        }
        bvt_page_free(vt, old_grid);
    }

    vt->grid = new_grid;
    vt->rows = new_rows;
    vt->cols = new_cols;
    vt->scroll_top = 0;
    vt->scroll_bottom = new_rows - 1;
    if (vt->cursor.row >= new_rows)
        vt->cursor.row = new_rows - 1;
    if (vt->cursor.col >= new_cols)
        vt->cursor.col = new_cols - 1;
    if (vt->cursor.row < 0)
        vt->cursor.row = 0;
    if (vt->cursor.col < 0)
        vt->cursor.col = 0;
    vt->cursor.pending_wrap = false;

    /* Resize tabstops. */
    uint8_t *new_tabs = bvt_alloc(vt, (size_t)new_cols);
    if (new_tabs) {
        for (int i = 0; i < new_cols; ++i)
            new_tabs[i] = (i % 8 == 0) ? 1u : 0u;
        bvt_dealloc(vt, vt->tabstops);
        vt->tabstops = new_tabs;
    }

    bvt_damage_all(vt);
}

/* ------------------------------------------------------------------ */
/* Reflow                                                              */
/* ------------------------------------------------------------------ */

void bvt_reflow(BvtTerm *vt, int new_rows, int new_cols)
{
    if (!vt)
        return;
    if (new_rows <= 0 || new_cols <= 0)
        return;
    if (new_rows == vt->rows && new_cols == vt->cols)
        return;

    /* Altscreen and reflow-disabled fall through to clamp resize. */
    if (vt->in_altscreen || !vt->reflow_enabled) {
        bvt_resize_clamp(vt, new_rows, new_cols);
        return;
    }

    bvt_flush_cluster(vt);

    BvtPage *old_grid = vt->grid;
    if (!old_grid) {
        bvt_resize_clamp(vt, new_rows, new_cols);
        return;
    }

    int old_rows = vt->rows;
    int old_cols = vt->cols;
    int cursor_row = vt->cursor.row;
    int cursor_col = vt->cursor.col;

    /* ---------- Phase 1: collect ---------- */
    Collected c = (Collected){ 0 };
    int32_t cursor_offset = -1;
    int32_t cursor_line = -1;
    bool cursor_line_pending = false;

    for (int r = 0; r < old_rows; ++r) {
        uint32_t row_start_idx = c.cells_count;
        int32_t cursor_in_row = -1;

        if (!grow_cells(vt, &c, (uint32_t)old_cols))
            goto fail;

        for (int col = 0; col < old_cols; ++col) {
            if (r == cursor_row && col == cursor_col)
                cursor_in_row = (int32_t)(c.cells_count - row_start_idx);
            const BvtCell *cell = &old_grid->cells[(size_t)r * old_cols + col];
            if (cell->width == 0)
                continue;
            c.cells[c.cells_count].cell = *cell;
            c.cells[c.cells_count].src_page = old_grid;
            c.cells_count++;
        }
        if (r == cursor_row) {
            if (cursor_in_row < 0)
                cursor_in_row = (int32_t)(c.cells_count - row_start_idx);
            cursor_offset = (int32_t)(row_start_idx + (uint32_t)cursor_in_row);
            cursor_line_pending = true;
        }

        bool wrap = (old_grid->row_flags[r] & BVT_CELL_WRAPLINE) != 0u;
        if (!wrap) {
            if (!grow_lines(vt, &c))
                goto fail;
            c.line_ends[c.lines_count++] = c.cells_count;
            if (cursor_line_pending) {
                cursor_line = (int32_t)(c.lines_count - 1);
                cursor_line_pending = false;
            }
        }
    }
    /* Force-close any unclosed final line. */
    if (c.lines_count == 0 ||
        c.line_ends[c.lines_count - 1] != c.cells_count) {
        if (!grow_lines(vt, &c))
            goto fail;
        c.line_ends[c.lines_count++] = c.cells_count;
        if (cursor_line_pending) {
            cursor_line = (int32_t)(c.lines_count - 1);
            cursor_line_pending = false;
        }
    }
    if (cursor_offset < 0)
        cursor_offset = (int32_t)c.cells_count;
    if (cursor_line < 0)
        cursor_line = (int32_t)c.lines_count - 1;

    /* Trim trailing empty logical lines that don't host the cursor.
     * The original grid often has uninitialized rows below content;
     * propagating them through reflow would push real content into
     * scrollback unnecessarily. */
    while (c.lines_count > 0) {
        uint32_t end = c.line_ends[c.lines_count - 1];
        uint32_t start = (c.lines_count > 1)
                             ? c.line_ends[c.lines_count - 2]
                             : 0u;
        if (start != end)
            break;
        if (cursor_line == (int32_t)(c.lines_count - 1))
            break;
        c.lines_count--;
    }

    /* ---------- Phase 2: rewrap ---------- */
    NewRow *new_rows_arr = NULL;
    uint32_t new_rows_count = 0, new_rows_cap = 0;
    int32_t new_cursor_row = 0;
    int new_cursor_col = 0;
    bool cursor_placed = false;

    uint32_t line_start = 0;
    for (uint32_t li = 0; li < c.lines_count; ++li) {
        uint32_t line_end = c.line_ends[li];
        bool is_cursor_line = ((int32_t)li == cursor_line);
        uint32_t i = line_start;

        bool emitted_any = false;
        while (i < line_end) {
            uint32_t row_start = i;
            int row_used = 0;

            while (i < line_end) {
                int w = c.cells[i].cell.width;
                if (w <= 0) {
                    ++i;
                    continue;
                }
                if (row_used + w > new_cols)
                    break;
                row_used += w;
                ++i;
            }
            if (i == row_start && i < line_end)
                ++i;

            if (new_rows_count == new_rows_cap) {
                uint32_t nc = new_rows_cap ? new_rows_cap * 2 : 32;
                NewRow *np = bvt_realloc(vt, new_rows_arr,
                                         (size_t)nc * sizeof(NewRow));
                if (!np)
                    goto fail2;
                new_rows_arr = np;
                new_rows_cap = nc;
            }
            new_rows_arr[new_rows_count].start = row_start;
            new_rows_arr[new_rows_count].end = i;
            new_rows_arr[new_rows_count].wraps = (i < line_end);

            if (!cursor_placed && is_cursor_line) {
                if ((uint32_t)cursor_offset >= row_start &&
                    (uint32_t)cursor_offset < i) {
                    int w = 0;
                    for (uint32_t k = row_start;
                         k < (uint32_t)cursor_offset; ++k) {
                        w += c.cells[k].cell.width;
                    }
                    new_cursor_row = (int32_t)new_rows_count;
                    new_cursor_col = w;
                    cursor_placed = true;
                } else if ((uint32_t)cursor_offset == i) {
                    new_cursor_row = (int32_t)new_rows_count;
                    new_cursor_col = row_used;
                    if (new_cursor_col >= new_cols)
                        new_cursor_col = new_cols - 1;
                    cursor_placed = true;
                }
            }
            new_rows_count++;
            emitted_any = true;
        }
        if (!emitted_any) {
            if (new_rows_count == new_rows_cap) {
                uint32_t nc = new_rows_cap ? new_rows_cap * 2 : 32;
                NewRow *np = bvt_realloc(vt, new_rows_arr,
                                         (size_t)nc * sizeof(NewRow));
                if (!np)
                    goto fail2;
                new_rows_arr = np;
                new_rows_cap = nc;
            }
            new_rows_arr[new_rows_count].start = line_start;
            new_rows_arr[new_rows_count].end = line_start;
            new_rows_arr[new_rows_count].wraps = false;
            if (!cursor_placed && is_cursor_line) {
                new_cursor_row = (int32_t)new_rows_count;
                new_cursor_col = 0;
                cursor_placed = true;
            }
            new_rows_count++;
        }
        line_start = line_end;
    }
    if (!cursor_placed) {
        new_cursor_row = (new_rows_count > 0)
                             ? (int32_t)(new_rows_count - 1)
                             : 0;
        new_cursor_col = 0;
    }

    /* ---------- Phase 3: distribute ---------- */
    int total = (int)new_rows_count;
    int sb_count = total - new_rows;
    if (sb_count < 0)
        sb_count = 0;
    int visible_count = total - sb_count;
    if (visible_count > new_rows)
        visible_count = new_rows;

    BvtPage *new_grid = bvt_page_new(vt, new_rows, new_cols);
    if (!new_grid)
        goto fail2;

    /* Push sb_count oldest rows to scrollback. We need a temporary
     * row buffer at new_cols width and route through bvt_scrollback_push.
     * The push reads source style/grapheme from vt->grid which is still
     * the OLD grid until we install new_grid below — so it interns
     * correctly. */
    BvtCell *row_buf = bvt_alloc(vt, (size_t)new_cols * sizeof(BvtCell));
    if (!row_buf) {
        bvt_page_free(vt, new_grid);
        goto fail2;
    }

    for (int i = 0; i < sb_count; ++i) {
        memset(row_buf, 0, (size_t)new_cols * sizeof(BvtCell));
        NewRow nr = new_rows_arr[i];
        int col = 0;
        for (uint32_t k = nr.start; k < nr.end && col < new_cols; ++k) {
            CellRef *ref = &c.cells[k];
            int w = ref->cell.width;
            if (col + w > new_cols)
                break;
            row_buf[col] = ref->cell;
            if (w == 2 && col + 1 < new_cols) {
                BvtCell cont = (BvtCell){ 0 };
                cont.width = 0;
                cont.style_id = ref->cell.style_id;
                row_buf[col + 1] = cont;
            }
            col += w;
        }
        bvt_scrollback_push(vt, row_buf, new_cols, nr.wraps);
    }
    bvt_dealloc(vt, row_buf);

    /* Place visible rows into new_grid. */
    for (int i = 0; i < visible_count; ++i) {
        NewRow nr = new_rows_arr[sb_count + i];
        int col = 0;
        for (uint32_t k = nr.start; k < nr.end && col < new_cols; ++k) {
            CellRef *ref = &c.cells[k];
            int w = ref->cell.width;
            if (col + w > new_cols)
                break;

            const BvtStyle *st =
                bvt_style_lookup(ref->src_page, ref->cell.style_id);
            uint32_t new_style_id =
                st ? bvt_style_intern(vt, new_grid, st) : 0u;
            uint32_t new_grapheme_id = 0u;
            if (ref->cell.grapheme_id != 0u) {
                uint32_t cps[BVT_CLUSTER_MAX];
                size_t n = bvt_grapheme_read(ref->src_page, ref->cell.grapheme_id,
                                             cps, BVT_CLUSTER_MAX);
                if (n >= 2)
                    new_grapheme_id = bvt_grapheme_intern(vt, new_grid, cps,
                                                          (uint32_t)n);
            }
            BvtCell new_cell = ref->cell;
            new_cell.style_id = new_style_id;
            new_cell.grapheme_id = new_grapheme_id;
            new_grid->cells[(size_t)i * new_cols + col] = new_cell;
            if (w == 2 && col + 1 < new_cols) {
                BvtCell cont = (BvtCell){ 0 };
                cont.width = 0;
                cont.style_id = new_style_id;
                new_grid->cells[(size_t)i * new_cols + col + 1] = cont;
            }
            col += w;
        }
        if (nr.wraps)
            new_grid->row_flags[i] |= BVT_CELL_WRAPLINE;
    }

    /* Install new grid + free old. */
    bvt_page_free(vt, vt->grid);
    vt->grid = new_grid;

    vt->rows = new_rows;
    vt->cols = new_cols;
    vt->scroll_top = 0;
    vt->scroll_bottom = new_rows - 1;

    /* Cursor mapping. */
    int target_visible_row = (int)new_cursor_row - sb_count;
    if (target_visible_row < 0) {
        vt->cursor.row = 0;
        vt->cursor.col = 0;
    } else {
        if (target_visible_row >= new_rows)
            target_visible_row = new_rows - 1;
        vt->cursor.row = target_visible_row;
        vt->cursor.col =
            (new_cursor_col >= new_cols) ? new_cols - 1 : new_cursor_col;
    }
    vt->cursor.pending_wrap = false;

    /* Tabstops. */
    uint8_t *new_tabs = bvt_alloc(vt, (size_t)new_cols);
    if (new_tabs) {
        for (int i = 0; i < new_cols; ++i)
            new_tabs[i] = (i % 8 == 0) ? 1u : 0u;
        bvt_dealloc(vt, vt->tabstops);
        vt->tabstops = new_tabs;
    }

    bvt_damage_all(vt);

    bvt_dealloc(vt, c.cells);
    bvt_dealloc(vt, c.line_ends);
    bvt_dealloc(vt, new_rows_arr);
    return;

fail2:
    bvt_dealloc(vt, new_rows_arr);
fail:
    bvt_dealloc(vt, c.cells);
    bvt_dealloc(vt, c.line_ends);
    /* On failure we fall through to clamp resize so the user isn't
     * left with a broken geometry. */
    bvt_resize_clamp(vt, new_rows, new_cols);
}
