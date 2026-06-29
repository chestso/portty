// General-purpose in-process pager — see pager.h.
//
// The document is fed into a dedicated, PTY-less coffer terminal (the
// "overlay"); the shared renderer draws that terminal full-screen when set, and
// its scrollback provides paging. Hyperlink hover/open reuse the same cell
// lookups as the main view, but target the overlay terminal.

#include "pager.h"
#include "common.h"
#include "term_cfr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Large delta to drive renderer_scroll to a clamped extreme (top/bottom).
#define PAGER_SCROLL_EXTREME (1 << 30)

struct Pager
{
    RendererBackend *rend;
    PlatformBackend *plat;
    TerminalBackend *term; // overlay terminal; NULL when closed
    char *text;            // retained source document (for rebuild on resize)
    int cols, rows;
    uint16_t hovered; // last hovered hyperlink id (debounces cursor changes)
    // Char-selection drag: a left press defers selection until first motion so
    // a plain click doesn't start one (mirrors the main view).
    bool drag_pending;
    int drag_row, drag_col; // unified row / vt col of the deferred start
};

static void copy_selection(Pager *p);

Pager *pager_create(RendererBackend *rend, PlatformBackend *plat)
{
    if (!rend || !plat)
        return NULL;
    Pager *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->rend = rend;
    p->plat = plat;
    return p;
}

void pager_destroy(Pager *p)
{
    if (!p)
        return;
    pager_close(p);
    free(p);
}

bool pager_active(const Pager *p)
{
    return p && p->term != NULL;
}

// Feed the document into the VT, translating bare LF to CRLF so lines start at
// column 0 (a VT treats LF as line-feed only).
static void feed_text(TerminalBackend *term, const char *s)
{
    const char *run = s;
    for (const char *c = s; *c; c++) {
        if (*c == '\n' && (c == s || c[-1] != '\r')) {
            if (c > run)
                terminal_process_input(term, run, (size_t)(c - run));
            terminal_process_input(term, "\r\n", 2);
            run = c + 1;
        }
    }
    size_t tail = strlen(run);
    if (tail)
        terminal_process_input(term, run, tail);
}

// Build the overlay terminal from `text` at the given size and hand it to the
// renderer (which positions the view at the top). Returns false on failure.
static bool overlay_build(Pager *p, const char *text, int cols, int rows)
{
    int cell_w = 10, cell_h = 20;
    renderer_get_cell_size(p->rend, &cell_w, &cell_h);
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.cols = cols;
    cfg.rows = rows;
    cfg.cell_w_px = cell_w;
    cfg.cell_h_px = cell_h;
    TerminalBackend *t = term_cfr_new(&cfg);
    if (!t)
        return false;

    // Scrollback must retain the whole document above the fold.
    int lines = 1;
    for (const char *c = text; *c; c++)
        if (*c == '\n')
            lines++;
    terminal_set_scrollback_size(t, lines + rows + 16);

    feed_text(t, text);

    p->term = t;
    p->cols = cols;
    p->rows = rows;
    p->hovered = 0;
    renderer_set_overlay(p->rend, t); // positions the shared view at the top
    return true;
}

static void overlay_teardown(Pager *p)
{
    if (!p->term)
        return;
    renderer_clear_overlay(p->rend); // restore host scroll view, stop drawing overlay
    terminal_destroy(p->term);
    free(p->term); // heap instance from term_cfr_new
    p->term = NULL;
}

bool pager_open(Pager *p, const char *ansi_text, int cols, int rows)
{
    if (!p || !ansi_text || cols <= 0 || rows <= 0)
        return false;

    bool was_open = (p->term != NULL);

    char *copy = strdup(ansi_text);
    if (!copy)
        return false;

    if (was_open)
        overlay_teardown(p); // replacing existing content; PTY stays paused

    if (!overlay_build(p, copy, cols, rows)) {
        free(copy);
        // If we tore down a prior session, it is now closed — release the PTY.
        if (was_open)
            platform_resume_pty(p->plat);
        return false;
    }

    free(p->text);
    p->text = copy;

    // Drop any link hint left over from the main terminal — the pager owns the
    // hint channel while it is open and re-resolves on the next hover.
    platform_set_link_hint(p->plat, NULL, 0);

    if (!was_open)
        platform_pause_pty(p->plat); // freeze background output behind the overlay
    return true;
}

void pager_close(Pager *p)
{
    if (!pager_active(p))
        return;
    overlay_teardown(p);
    free(p->text);
    p->text = NULL;
    p->hovered = 0;
    platform_set_link_hint(p->plat, NULL, 0);
    platform_set_cursor(p->plat, PLATFORM_CURSOR_TEXT);
    platform_resume_pty(p->plat);
}

void pager_resize(Pager *p, int cols, int rows)
{
    if (!pager_active(p) || cols <= 0 || rows <= 0)
        return;
    // Rebuild from the retained document at the new size (resets to the top).
    // The PTY stays paused across the rebuild.
    char *text = p->text;
    p->text = NULL;
    overlay_teardown(p);
    if (overlay_build(p, text, cols, rows))
        p->text = text;
    else {
        free(text);
        platform_resume_pty(p->plat); // build failed → session is closed
    }
}

bool pager_key(Pager *p, int key, int mod, uint32_t codepoint)
{
    if (!pager_active(p))
        return false;

    int page = p->rows - 2;
    if (page < 1)
        page = 1;

    // Ctrl+C / Ctrl+Shift+C — copy the active selection.
    if ((mod & TERM_MOD_CTRL) && (codepoint == 'c' || codepoint == 'C')) {
        copy_selection(p);
        return true;
    }

    // Close.
    if (key == TERM_KEY_ESCAPE || codepoint == 'q' || codepoint == 'Q') {
        pager_close(p);
        return true;
    }

    // Scroll. Positive delta moves toward the top (older), negative toward the
    // bottom (newer); renderer_scroll clamps to the overlay's scrollback.
    int delta = 0;
    switch (key) {
    case TERM_KEY_UP:
        delta = 1;
        break;
    case TERM_KEY_DOWN:
        delta = -1;
        break;
    case TERM_KEY_PAGEUP:
        delta = page;
        break;
    case TERM_KEY_PAGEDOWN:
        delta = -page;
        break;
    case TERM_KEY_HOME:
        delta = PAGER_SCROLL_EXTREME;
        break;
    case TERM_KEY_END:
        delta = -PAGER_SCROLL_EXTREME;
        break;
    default:
        switch (codepoint) {
        case 'k':
            delta = 1;
            break;
        case 'j':
            delta = -1;
            break;
        case 'b':
        case 'B':
            delta = page;
            break;
        case ' ':
            delta = -page;
            break;
        case 'g':
            delta = PAGER_SCROLL_EXTREME;
            break;
        case 'G':
            delta = -PAGER_SCROLL_EXTREME;
            break;
        default:
            break;
        }
        break;
    }
    if (delta != 0)
        renderer_scroll(p->rend, p->term, delta);

    // Modal: consume every key so nothing leaks to the shell.
    return true;
}

bool pager_scroll(Pager *p, int delta)
{
    if (!pager_active(p))
        return false;
    renderer_scroll(p->rend, p->term, delta);
    return true;
}

// Map a pixel position to the overlay's (unified_row, vt_col). Returns false if
// outside the grid. While the pager is active the renderer's scroll offset is
// the overlay's, so the main view's display-row -> unified-row math applies.
static bool cell_at(Pager *p, int px, int py, int *out_row, int *out_col)
{
    int cw, ch;
    if (!renderer_get_cell_size(p->rend, &cw, &ch) || cw <= 0 || ch <= 0)
        return false;
    int display_col = px / cw;
    int display_row = py / ch;

    int rows, cols;
    terminal_get_dimensions(p->term, &rows, &cols);
    if (display_row < 0 || display_row >= rows || display_col < 0 || display_col >= cols)
        return false;

    int scroll_offset = renderer_get_scroll_offset(p->rend);
    int scrollback_row = scroll_offset - 1 - display_row;
    int unified_row =
        (scrollback_row >= 0) ? -(scrollback_row + 1) : (display_row - scroll_offset);

    int vt_col = terminal_vis_col_to_vt_col(p->term, unified_row, display_col);
    if (vt_col < 0)
        vt_col = 0;
    if (vt_col >= cols)
        vt_col = cols - 1;

    *out_row = unified_row;
    *out_col = vt_col;
    return true;
}

// OSC 8 hyperlink id at an already-resolved overlay cell (0 = none).
static uint16_t link_at(Pager *p, int unified_row, int vt_col)
{
    TerminalCell cell;
    int rc = (unified_row >= 0)
                 ? terminal_get_cell(p->term, unified_row, vt_col, &cell)
                 : terminal_get_scrollback_cell(p->term, -(unified_row + 1), vt_col, &cell);
    if (rc < 0)
        return 0;
    return cell.hyperlink_id;
}

static void copy_selection(Pager *p)
{
    if (!terminal_selection_active(p->term))
        return;
    char *text = terminal_selection_get_text(p->term);
    if (text) {
        platform_clipboard_set(p->plat, text);
        free(text);
    }
    // Clear the highlight after copying, matching the main view.
    terminal_selection_clear(p->term);
}

bool pager_mouse(Pager *p, int pixel_x, int pixel_y, int button, bool pressed, int clicks,
                 int mod)
{
    if (!pager_active(p))
        return false;

    // Mouse wheel arrives as button 4 (up, toward the top) / 5 (down, toward the
    // bottom) before the platform's on_scroll fallback. Consume it here so it
    // pages the overlay rather than being swallowed silently.
    if (button == 4 || button == 5) {
        renderer_scroll(p->rend, p->term,
                        button == 4 ? SCROLL_LINES_PER_TICK : -SCROLL_LINES_PER_TICK);
        return true;
    }

    int row = 0, col = 0;
    bool in_grid = cell_at(p, pixel_x, pixel_y, &row, &col);
    uint16_t hid = in_grid ? link_at(p, row, col) : 0;

    // Hover feedback (link pointer vs text cursor) + the real-URI hint pill,
    // positioned via the pixel-Y anchor like the main terminal.
    if (hid != p->hovered) {
        terminal_set_hovered_hyperlink(p->term, hid);
        platform_set_cursor(p->plat,
                            hid != 0 ? PLATFORM_CURSOR_POINTER : PLATFORM_CURSOR_TEXT);
        if (hid != 0) {
            char url[4096];
            size_t n = terminal_cell_get_hyperlink(p->term, row, col, url, sizeof(url));
            platform_set_link_hint(p->plat, n > 0 ? url : NULL, pixel_y);
        } else {
            platform_set_link_hint(p->plat, NULL, pixel_y);
        }
        p->hovered = hid;
    }

    // Ctrl + left-click on a link opens the URL (takes precedence over selection).
    if (hid != 0 && button == 1 && pressed && (mod & TERM_MOD_CTRL)) {
        char url[4096];
        size_t n = terminal_cell_get_hyperlink(p->term, row, col, url, sizeof(url));
        if (n > 0 && terminal_hyperlink_is_safe(url)) {
            char err[256];
            if (!platform_open_url(p->plat, url, err, sizeof(err)))
                fprintf(stderr, "ERROR: failed to open URL: %s\n", err);
        } else if (n > 0) {
            fprintf(stderr, "WARNING: refusing to open URL with disallowed scheme: %s\n",
                    url);
        }
        return true;
    }

    // Left press: word (double) / line (triple) selection, clear an existing
    // one, or defer char selection until the first drag motion.
    if (button == 1 && pressed && in_grid) {
        if (clicks >= 3) {
            p->drag_pending = false;
            terminal_selection_start(p->term, row, col, TERM_SELECT_LINE);
        } else if (clicks == 2) {
            p->drag_pending = false;
            terminal_selection_start(p->term, row, col, TERM_SELECT_WORD);
        } else if (terminal_selection_active(p->term)) {
            p->drag_pending = false;
            terminal_selection_clear(p->term);
        } else {
            p->drag_pending = true;
            p->drag_row = row;
            p->drag_col = col;
        }
        return true;
    }

    // Left release: end any deferred drag (copy is explicit: right-click or
    // Ctrl+Shift+C, matching the main view).
    if (button == 1 && !pressed) {
        p->drag_pending = false;
        return true;
    }

    // Motion with a button held: begin (from the deferred start) or extend the
    // char selection.
    if (button == 0 && pressed && in_grid) {
        if (p->drag_pending) {
            p->drag_pending = false;
            terminal_selection_start(p->term, p->drag_row, p->drag_col, TERM_SELECT_CHAR);
            terminal_selection_update(p->term, row, col);
        } else if (terminal_selection_active(p->term)) {
            terminal_selection_update(p->term, row, col);
        }
        return true;
    }

    // Right-click: copy the active selection.
    if (button == 3 && pressed)
        copy_selection(p);

    // Modal: consume all mouse events while open.
    return true;
}
