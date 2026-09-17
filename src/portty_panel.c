/*
 * portty — Panel layout and state management of the notification panel
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_panel.h"
#include <stdlib.h>
#include <string.h>

static char *safe_strdup(const char *s)
{
    if (!s)
        return NULL;
    return strdup(s);
}

static bool str_equal(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

// Replace *dst with a copy of src, but only when the text actually differs.
// Panels are re-shown on every resize event and on link-hint changes; keeping
// the existing allocation when the text is unchanged avoids a free/strdup
// per redundant show.
static void panel_set_string(char **dst, const char *src)
{
    if (str_equal(*dst, src))
        return;
    free(*dst);
    *dst = safe_strdup(src);
}

static void panel_free_strings(PanelState *p)
{
    free(p->title);
    free(p->body);
    p->title = NULL;
    p->body = NULL;
}

static void panel_compute_layout(PanelState *p, int cell_w, int cell_h)
{
    p->px = p->col * cell_w;
    p->py = p->row * cell_h;
    p->pw = p->cols * cell_w;
    p->ph = p->rows * cell_h;

    p->close_size = cell_h;
    p->close_px = p->px + p->pw - p->close_size;
    p->close_py = p->py;
}

void panel_mgr_init(PanelManager *mgr, int cell_w, int cell_h)
{
    memset(mgr, 0, sizeof(*mgr));
    mgr->cell_w = cell_w;
    mgr->cell_h = cell_h;
}

void panel_mgr_set_cell_size(PanelManager *mgr, int cell_w, int cell_h)
{
    // Cell metrics change with font size/DPI, not per frame; the renderer
    // calls this on every show. Skipping the no-op case keeps a repeated
    // show from marking every active panel dirty (and rebuilding its
    // texture) when nothing about the metrics moved.
    if (mgr->cell_w == cell_w && mgr->cell_h == cell_h)
        return;
    mgr->cell_w = cell_w;
    mgr->cell_h = cell_h;
    panel_mgr_recompute_layout(mgr);
}

PanelState *panel_mgr_show(PanelManager *mgr, int id,
                           int col, int row, int cols, int rows,
                           const char *title, const char *body,
                           PorttyNotifyLevel level, unsigned int flags)
{
    PanelState *slot = panel_mgr_find(mgr, id);
    bool reused = slot != NULL;
    if (!slot) {
        for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
            if (!mgr->panels[i].active) {
                slot = &mgr->panels[i];
                break;
            }
        }
    }
    if (!slot)
        return NULL;

    // A repeat show with identical content is a no-op: leave the dirty flag
    // (and close-button hover) untouched so a redundant configure event or
    // link re-resolve does not rebuild the panel texture. Any changed field
    // still marks the slot for rebuild below.
    if (reused &&
        slot->col == col && slot->row == row &&
        slot->cols == cols && slot->rows == rows &&
        slot->level == level && slot->flags == flags &&
        str_equal(slot->title, title) && str_equal(slot->body, body))
        return slot;

    slot->active = true;
    slot->id = id;
    slot->col = col;
    slot->row = row;
    slot->cols = cols;
    slot->rows = rows;
    slot->level = level;
    slot->flags = flags;
    panel_set_string(&slot->title, title);
    panel_set_string(&slot->body, body);
    slot->close_hover = false;
    slot->dirty = true;

    panel_compute_layout(slot, mgr->cell_w, mgr->cell_h);
    return slot;
}

void panel_mgr_hide(PanelManager *mgr, int id)
{
    PanelState *p = panel_mgr_find(mgr, id);
    if (!p)
        return;
    panel_free_strings(p);
    p->active = false;
    p->id = 0;
    p->close_hover = false;
}

void panel_mgr_hide_all(PanelManager *mgr)
{
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        if (mgr->panels[i].active)
            panel_mgr_hide(mgr, mgr->panels[i].id);
    }
}

PanelState *panel_mgr_find(PanelManager *mgr, int id)
{
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        if (mgr->panels[i].active && mgr->panels[i].id == id)
            return &mgr->panels[i];
    }
    return NULL;
}

int panel_mgr_hit_test(PanelManager *mgr, int px, int py, bool *close_btn)
{
    if (close_btn)
        *close_btn = false;

    for (int i = PORTTY_PANEL_MAX - 1; i >= 0; i--) {
        PanelState *p = &mgr->panels[i];
        if (!p->active)
            continue;
        if (px < p->px || px >= p->px + p->pw ||
            py < p->py || py >= p->py + p->ph)
            continue;

        if (close_btn && panel_show_close(p->flags) &&
            px >= p->close_px && px < p->close_px + p->close_size &&
            py >= p->close_py && py < p->close_py + p->close_size) {
            *close_btn = true;
        }
        return p->id;
    }
    return 0;
}

void panel_mgr_set_hover(PanelManager *mgr, int id, bool hovered)
{
    PanelState *p = panel_mgr_find(mgr, id);
    if (p)
        p->close_hover = hovered;
}

int panel_mgr_active_count(PanelManager *mgr)
{
    int count = 0;
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        if (mgr->panels[i].active)
            count++;
    }
    return count;
}

void panel_mgr_recompute_layout(PanelManager *mgr)
{
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        if (mgr->panels[i].active) {
            panel_compute_layout(&mgr->panels[i], mgr->cell_w, mgr->cell_h);
            mgr->panels[i].dirty = true;
        }
    }
}

void panel_grid_to_pixel(int cell_w, int cell_h,
                         int col, int row, int *px, int *py)
{
    if (px)
        *px = col * cell_w;
    if (py)
        *py = row * cell_h;
}

void panel_pixel_to_grid(int cell_w, int cell_h,
                         int px, int py, int *col, int *row)
{
    if (col)
        *col = (cell_w > 0) ? (px / cell_w) : 0;
    if (row)
        *row = (cell_h > 0) ? (py / cell_h) : 0;
}

void panel_center_in_grid(int term_cols, int term_rows,
                          int panel_cols, int panel_rows,
                          int *out_col, int *out_row)
{
    int col = (term_cols - panel_cols) / 2;
    int row = (term_rows - panel_rows) / 2;
    if (col < 0)
        col = 0;
    if (row < 0)
        row = 0;
    if (out_col)
        *out_col = col;
    if (out_row)
        *out_row = row;
}

PanelRect panel_link_hint_rect(int term_cols, int term_rows, int link_row)
{
    PanelRect r = { 0, 0, term_cols, 1 + PANEL_DECORATION_ROWS };

    if (r.cols < 0)
        r.cols = 0;
    if (term_rows < 1)
        term_rows = 1;
    if (link_row < 0)
        link_row = 0;
    if (link_row >= term_rows)
        link_row = term_rows - 1;
    if (r.rows > term_rows)
        r.rows = term_rows;

    if (link_row - r.rows >= 0)
        r.row = link_row - r.rows; // the row just above the link
    else if (link_row + 1 + r.rows <= term_rows)
        r.row = link_row + 1; // the row just below it
    else
        r.row = term_rows - r.rows; // neither fits: bottom-align
    return r;
}
