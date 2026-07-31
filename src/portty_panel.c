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

    panel_free_strings(slot);
    slot->active = true;
    slot->id = id;
    slot->col = col;
    slot->row = row;
    slot->cols = cols;
    slot->rows = rows;
    slot->level = level;
    slot->flags = flags;
    slot->title = safe_strdup(title);
    slot->body = safe_strdup(body);
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
