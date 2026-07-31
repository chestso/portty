#ifndef PORTTY_PANEL_H
#define PORTTY_PANEL_H

#include "portty_backend.h"
#include <stdbool.h>

#define PORTTY_PANEL_MAX 8

// Panel decoration flags (bitwise OR)
#define PANEL_FLAG_NO_ACCENT 0x01
#define PANEL_FLAG_NO_CLOSE  0x02

// Cell-based layout constants (all measurements in cells)
#define PANEL_CELL_PAD_LEFT   1
#define PANEL_CELL_PAD_RIGHT  1
#define PANEL_CELL_PAD_TOP    1
#define PANEL_CELL_PAD_BOTTOM 1
#define PANEL_CELL_ACCENT     1
#define PANEL_CELL_GAP        1

// Total decoration cells: accent + gap + right_pad = 3 cols
// Total decoration cells: top_pad + bottom_pad = 2 rows
#define PANEL_DECORATION_COLS (PANEL_CELL_ACCENT + PANEL_CELL_GAP + PANEL_CELL_PAD_RIGHT)
#define PANEL_DECORATION_ROWS (PANEL_CELL_PAD_TOP + PANEL_CELL_PAD_BOTTOM)

typedef struct
{
    bool active;
    bool dirty; // Texture needs rebuild
    int id;
    int col, row;
    int cols, rows;
    PorttyNotifyLevel level;
    unsigned int flags; // PANEL_FLAG_* bits
    char *title;
    char *body;
    bool close_hover;

    int px, py;
    int pw, ph;
    int close_px, close_py, close_size;
} PanelState;

typedef struct
{
    PanelState panels[PORTTY_PANEL_MAX];
    int cell_w, cell_h;
} PanelManager;

void panel_mgr_init(PanelManager *mgr, int cell_w, int cell_h);
void panel_mgr_set_cell_size(PanelManager *mgr, int cell_w, int cell_h);

PanelState *panel_mgr_show(PanelManager *mgr, int id,
                           int col, int row, int cols, int rows,
                           const char *title, const char *body,
                           PorttyNotifyLevel level, unsigned int flags);

void panel_mgr_hide(PanelManager *mgr, int id);
void panel_mgr_hide_all(PanelManager *mgr);

PanelState *panel_mgr_find(PanelManager *mgr, int id);

int panel_mgr_hit_test(PanelManager *mgr, int px, int py, bool *close_btn);

void panel_mgr_set_hover(PanelManager *mgr, int id, bool hovered);

int panel_mgr_active_count(PanelManager *mgr);

void panel_mgr_recompute_layout(PanelManager *mgr);

void panel_grid_to_pixel(int cell_w, int cell_h,
                         int col, int row, int *px, int *py);

void panel_pixel_to_grid(int cell_w, int cell_h,
                         int px, int py, int *col, int *row);

// Compute terminal size from panel size (in cells)
static inline int panel_term_cols(int panel_cols, bool show_accent)
{
    int decor = PANEL_CELL_GAP + PANEL_CELL_PAD_RIGHT;
    if (show_accent)
        decor += PANEL_CELL_ACCENT;
    return panel_cols - decor;
}

static inline int panel_term_rows(int panel_rows)
{
    return panel_rows - PANEL_DECORATION_ROWS;
}

// Compute terminal pixel position within panel
static inline int panel_term_px(int panel_px, int cell_w, bool show_accent)
{
    int offset = PANEL_CELL_GAP;
    if (show_accent)
        offset += PANEL_CELL_ACCENT;
    return panel_px + offset * cell_w;
}

static inline int panel_term_py(int panel_py, int cell_h)
{
    return panel_py + PANEL_CELL_PAD_TOP * cell_h;
}

// Compute accent stripe pixel geometry
static inline int panel_accent_px(int panel_px, int cell_w)
{
    (void)cell_w;
    return panel_px;
}

static inline int panel_accent_w(int cell_w)
{
    return cell_w / 2;
}

// Helper: check if accent stripe should be shown
static inline bool panel_show_accent(unsigned int flags)
{
    return (flags & PANEL_FLAG_NO_ACCENT) == 0;
}

// Helper: check if close button should be shown
static inline bool panel_show_close(unsigned int flags)
{
    return (flags & PANEL_FLAG_NO_CLOSE) == 0;
}

#endif
