/*
 * bloom-vt — DEC private mode handlers that need real state changes
 * beyond the boolean flag flip in csi.c (altscreen save/restore,
 * cursor key application mode, etc.).
 */

#include "bloom_vt_internal.h"

#include <stdlib.h>
#include <string.h>

/* Altscreen toggle. With save_restore_cursor, mimics DECSET 1049 (the
 * "smcup/rmcup" variant): save cursor on entry, restore on exit. With
 * save_restore_cursor=false this matches DECSET 47 / 1047 (raw alt). */
void bvt_set_altscreen(BvtTerm *vt, bool on, bool save_restore_cursor)
{
    if (!vt)
        return;
    if (on == vt->in_altscreen)
        return;

    /* Flush any pending cluster before swapping grids. */
    bvt_flush_cluster(vt);

    if (on) {
        if (save_restore_cursor)
            vt->saved_cursor = vt->cursor;

        /* Lazily allocate or resize the alt grid to current geometry. */
        if (!vt->altgrid ||
            vt->altgrid->cols != vt->cols ||
            vt->altgrid->row_capacity != vt->rows) {
            if (vt->altgrid) {
                bvt_page_free(vt, vt->altgrid);
                vt->altgrid = NULL;
            }
            vt->altgrid = bvt_page_new(vt, vt->rows, vt->cols);
            if (!vt->altgrid)
                return; /* OOM — leave altscreen flag off */
        }

        /* Swap. The current grid is preserved in vt->altgrid; the
         * alt grid (which is zeroed by bvt_page_new) becomes active. */
        BvtPage *tmp = vt->grid;
        vt->grid = vt->altgrid;
        vt->altgrid = tmp;

        vt->in_altscreen = true;
        vt->modes[BVT_MODE_ALTSCREEN] = true;
        /* DECSET 1049 also moves cursor to home. */
        if (save_restore_cursor) {
            vt->cursor.row = 0;
            vt->cursor.col = 0;
            vt->cursor.pending_wrap = false;
        }
    } else {
        BvtPage *tmp = vt->grid;
        vt->grid = vt->altgrid;
        vt->altgrid = tmp;

        vt->in_altscreen = false;
        vt->modes[BVT_MODE_ALTSCREEN] = false;
        if (save_restore_cursor)
            vt->cursor = vt->saved_cursor;
    }

    bvt_damage_all(vt);
    if (vt->callbacks.set_mode)
        vt->callbacks.set_mode(BVT_MODE_ALTSCREEN, on, vt->callback_user);
}
