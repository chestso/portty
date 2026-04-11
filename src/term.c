#include "term.h"
#include "sixel.h"
#include "unicode.h"
#include <stdlib.h>
#include <string.h>

// Default set of characters considered part of a "word" for double-click selection
static const char *default_word_chars =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-/";

// Compare two positions: -1 if a < b, 0 if equal, 1 if a > b
static int selpos_cmp(TerminalPos a, TerminalPos b)
{
    if (a.row < b.row)
        return -1;
    if (a.row > b.row)
        return 1;
    if (a.col < b.col)
        return -1;
    if (a.col > b.col)
        return 1;
    return 0;
}

// Classify a character: 0=whitespace/empty, 1=word char, 2=other
static int char_class(uint32_t ch, const char *word_chars)
{
    if (ch == 0 || ch == ' ' || ch == '\t')
        return 0;
    // Check if character is in word_chars set
    if (ch < 128) {
        for (const char *p = word_chars; *p; p++) {
            if ((uint32_t)*p == ch)
                return 1;
        }
    }
    // Non-ASCII characters are treated as word characters
    if (ch >= 128)
        return 1;
    return 2;
}

// Read a cell at a unified row coordinate (row >= 0 = visible, row < 0 = scrollback)
static int read_cell_unified(TerminalBackend *term, int row, int col, TerminalCell *cell)
{
    if (row >= 0) {
        return terminal_get_cell(term, row, col, cell);
    } else {
        int scrollback_index = -(row + 1);
        return terminal_get_scrollback_cell(term, scrollback_index, col, cell);
    }
}

// Encode a single Unicode codepoint as UTF-8, return number of bytes written
static int codepoint_to_utf8(uint32_t cp, char *buf)
{
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp < 0x110000) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

TerminalBackend *terminal_init(TerminalBackend *backend, int width, int height)
{
    if (!backend || !backend->init)
        return NULL;

    if (!backend->init(backend, width, height))
        return NULL;

    return backend;
}

void terminal_destroy(TerminalBackend *term)
{
    if (!term || !term->destroy)
        return;
    term->destroy(term);
}

void terminal_resize(TerminalBackend *term, int width, int height)
{
    if (!term || !term->resize)
        return;
    term->resize(term, width, height);
}

int terminal_process_input(TerminalBackend *term, const char *input, size_t len)
{
    if (!term || !term->process_input)
        return -1;
    terminal_selection_clear(term);
    return term->process_input(term, input, len);
}

int terminal_get_cell(TerminalBackend *term, int row, int col, TerminalCell *cell)
{
    if (!term || !term->get_cell)
        return -1;
    return term->get_cell(term, row, col, cell);
}

int terminal_get_dimensions(TerminalBackend *term, int *rows, int *cols)
{
    if (!term || !term->get_dimensions)
        return -1;
    return term->get_dimensions(term, rows, cols);
}

TerminalPos terminal_get_cursor_pos(TerminalBackend *term)
{
    if (!term || !term->get_cursor_pos)
        return (TerminalPos){ 0, 0 };
    return term->get_cursor_pos(term);
}

bool terminal_get_cursor_visible(TerminalBackend *term)
{
    if (!term || !term->get_cursor_visible)
        return true;
    return term->get_cursor_visible(term);
}

bool terminal_get_cursor_blink(TerminalBackend *term)
{
    if (!term || !term->get_cursor_blink)
        return true;
    return term->get_cursor_blink(term);
}

const char *terminal_get_title(TerminalBackend *term)
{
    if (!term || !term->get_title)
        return NULL;
    return term->get_title(term);
}

bool terminal_needs_redraw(TerminalBackend *term)
{
    if (!term || !term->needs_redraw)
        return false;
    return term->needs_redraw(term);
}

void terminal_clear_redraw(TerminalBackend *term)
{
    if (!term || !term->clear_redraw)
        return;
    term->clear_redraw(term);
}

bool terminal_get_damage_rect(TerminalBackend *term, TerminalDamageRect *rect)
{
    if (!term || !term->get_damage_rect || !rect)
        return false;
    return term->get_damage_rect(term, rect);
}

int terminal_get_scrollback_lines(TerminalBackend *term)
{
    if (!term || !term->get_scrollback_lines)
        return 0;
    return term->get_scrollback_lines(term);
}

int terminal_get_scrollback_cell(TerminalBackend *term, int scrollback_row, int col,
                                 TerminalCell *cell)
{
    if (!term || !term->get_scrollback_cell || !cell)
        return -1;
    return term->get_scrollback_cell(term, scrollback_row, col, cell);
}

bool terminal_is_altscreen(TerminalBackend *term)
{
    if (!term || !term->is_altscreen)
        return false;
    return term->is_altscreen(term);
}

int terminal_get_mouse_mode(TerminalBackend *term)
{
    if (!term || !term->get_mouse_mode)
        return 0;
    return term->get_mouse_mode(term);
}

void terminal_send_mouse_event(TerminalBackend *term, int row, int col, int button, bool pressed,
                               int mod)
{
    if (!term || !term->send_mouse_event)
        return;
    term->send_mouse_event(term, row, col, button, pressed, mod);
}

void terminal_set_output_callback(TerminalBackend *term, TerminalOutputCallback cb, void *user)
{
    if (!term || !term->set_output_callback)
        return;
    term->set_output_callback(term, cb, user);
}

void terminal_send_key(TerminalBackend *term, int key, int mod)
{
    if (!term || !term->send_key)
        return;
    term->send_key(term, key, mod);
}

void terminal_send_char(TerminalBackend *term, uint32_t codepoint, int mod)
{
    if (!term || !term->send_char)
        return;
    term->send_char(term, codepoint, mod);
}

void terminal_start_paste(TerminalBackend *term)
{
    if (!term || !term->start_paste)
        return;
    term->start_paste(term);
}

void terminal_end_paste(TerminalBackend *term)
{
    if (!term || !term->end_paste)
        return;
    term->end_paste(term);
}

void terminal_set_reflow(TerminalBackend *term, bool enabled)
{
    if (!term || !term->set_reflow)
        return;
    term->set_reflow(term, enabled);
}

bool terminal_get_line_continuation(TerminalBackend *term, int row)
{
    if (!term || !term->get_line_continuation)
        return false;
    return term->get_line_continuation(term, row);
}

// --- Selection API ---

void terminal_selection_clear(TerminalBackend *term)
{
    if (!term || !term->selection.active)
        return;
    term->selection.active = false;
    term->selection.mode = TERM_SELECT_NONE;
    if (term->selection_change_cb)
        term->selection_change_cb(false, term->selection_change_data);
}

bool terminal_selection_active(TerminalBackend *term)
{
    if (!term)
        return false;
    return term->selection.active;
}

void terminal_selection_set_word_chars(TerminalBackend *term, const char *chars)
{
    if (!term)
        return;
    free(term->selection.word_chars);
    term->selection.word_chars = chars ? strdup(chars) : NULL;
}

void terminal_set_selection_callback(TerminalBackend *term, TerminalSelectionChangeFn cb,
                                     void *user_data)
{
    if (!term)
        return;
    term->selection_change_cb = cb;
    term->selection_change_data = user_data;
}

// Expand start/end to cover the word at (row, col)
static void expand_word(TerminalBackend *term, int row, int col, TerminalPos *out_start,
                        TerminalPos *out_end)
{
    const char *wchars =
        term->selection.word_chars ? term->selection.word_chars : default_word_chars;
    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);

    TerminalCell cell;
    if (read_cell_unified(term, row, col, &cell) < 0) {
        *out_start = (TerminalPos){ row, col };
        *out_end = (TerminalPos){ row, col };
        return;
    }
    int cls = char_class(cell.chars[0], wchars);

    // Scan left — cross soft-wrapped line boundaries
    int scan_row = row, left = col;
    while (left > 0 || terminal_get_line_continuation(term, scan_row)) {
        if (left > 0) {
            TerminalCell c;
            if (read_cell_unified(term, scan_row, left - 1, &c) < 0)
                break;
            if (char_class(c.chars[0], wchars) != cls)
                break;
            left--;
        } else {
            // At col 0 on a continuation line — move to end of previous row
            if (!terminal_get_line_continuation(term, scan_row))
                break;
            scan_row--;
            left = cols - 1;
            // Verify the character class still matches
            TerminalCell c;
            if (read_cell_unified(term, scan_row, left, &c) < 0)
                break;
            if (char_class(c.chars[0], wchars) != cls)
                break;
        }
    }

    // Scan right — cross soft-wrapped line boundaries
    int scan_row_r = row, right = col;
    while (right < cols - 1 || terminal_get_line_continuation(term, scan_row_r + 1)) {
        if (right < cols - 1) {
            TerminalCell c;
            if (read_cell_unified(term, scan_row_r, right + 1, &c) < 0)
                break;
            if (char_class(c.chars[0], wchars) != cls)
                break;
            right++;
        } else {
            // At last col — check if next row continues
            if (!terminal_get_line_continuation(term, scan_row_r + 1))
                break;
            scan_row_r++;
            right = 0;
            TerminalCell c;
            if (read_cell_unified(term, scan_row_r, right, &c) < 0)
                break;
            if (char_class(c.chars[0], wchars) != cls)
                break;
        }
    }

    *out_start = (TerminalPos){ scan_row, left };
    *out_end = (TerminalPos){ scan_row_r, right };
}

void terminal_selection_start(TerminalBackend *term, int row, int col, TerminalSelectMode mode)
{
    if (!term)
        return;

    TerminalSelection *sel = &term->selection;
    sel->active = true;
    sel->mode = mode;
    sel->anchor = (TerminalPos){ row, col };

    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);

    switch (mode) {
    case TERM_SELECT_CHAR:
        sel->start = sel->end = (TerminalPos){ row, col };
        break;
    case TERM_SELECT_WORD:
        expand_word(term, row, col, &sel->start, &sel->end);
        break;
    case TERM_SELECT_LINE:
        sel->start = (TerminalPos){ row, 0 };
        sel->end = (TerminalPos){ row, cols - 1 };
        break;
    default:
        sel->active = false;
        break;
    }

    if (sel->active && term->selection_change_cb)
        term->selection_change_cb(true, term->selection_change_data);
}

void terminal_selection_update(TerminalBackend *term, int row, int col)
{
    if (!term || !term->selection.active)
        return;

    TerminalSelection *sel = &term->selection;
    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);

    switch (sel->mode) {
    case TERM_SELECT_CHAR:
    {
        TerminalPos cursor = { row, col };
        if (selpos_cmp(cursor, sel->anchor) < 0) {
            sel->start = cursor;
            sel->end = sel->anchor;
        } else {
            sel->start = sel->anchor;
            sel->end = cursor;
        }
        break;
    }
    case TERM_SELECT_WORD:
    {
        // Expand anchor word
        TerminalPos anchor_start, anchor_end;
        expand_word(term, sel->anchor.row, sel->anchor.col, &anchor_start, &anchor_end);
        // Expand cursor word
        TerminalPos cursor_start, cursor_end;
        expand_word(term, row, col, &cursor_start, &cursor_end);
        // Union: min of starts, max of ends
        sel->start = selpos_cmp(anchor_start, cursor_start) < 0 ? anchor_start : cursor_start;
        sel->end = selpos_cmp(anchor_end, cursor_end) > 0 ? anchor_end : cursor_end;
        break;
    }
    case TERM_SELECT_LINE:
    {
        if (row < sel->anchor.row) {
            sel->start = (TerminalPos){ row, 0 };
            sel->end = (TerminalPos){ sel->anchor.row, cols - 1 };
        } else {
            sel->start = (TerminalPos){ sel->anchor.row, 0 };
            sel->end = (TerminalPos){ row, cols - 1 };
        }
        break;
    }
    default:
        break;
    }
}

bool terminal_cell_in_selection(TerminalBackend *term, int row, int col)
{
    if (!term || !term->selection.active)
        return false;

    TerminalSelection *sel = &term->selection;
    TerminalPos pos = { row, col };
    return selpos_cmp(pos, sel->start) >= 0 && selpos_cmp(pos, sel->end) <= 0;
}

char *terminal_selection_get_text(TerminalBackend *term)
{
    if (!term || !term->selection.active)
        return NULL;

    TerminalSelection *sel = &term->selection;
    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);

    // Allocate buffer (worst case: 4 bytes per cell + newlines)
    int row_count = sel->end.row - sel->start.row + 1;
    size_t buf_size = (size_t)row_count * (size_t)(cols * 4 + 1) + 1;
    char *buf = malloc(buf_size);
    if (!buf)
        return NULL;

    size_t pos = 0;

    for (int row = sel->start.row; row <= sel->end.row; row++) {
        int col_start = (row == sel->start.row) ? sel->start.col : 0;
        int col_end = (row == sel->end.row) ? sel->end.col : cols - 1;

        // Track last non-whitespace position for trailing whitespace stripping
        size_t row_start_pos = pos;
        size_t last_nonspace_pos = row_start_pos;

        for (int col = col_start; col <= col_end; col++) {
            TerminalCell cell;
            if (read_cell_unified(term, row, col, &cell) < 0)
                continue;

            // Skip continuation cells (width == 0 means right-half of wide char)
            if (cell.width == 0)
                continue;

            if (cell.chars[0] == 0) {
                // Empty cell → space
                if (pos < buf_size - 1)
                    buf[pos++] = ' ';
            } else {
                for (int i = 0; i < TERM_MAX_CHARS_PER_CELL && cell.chars[i] != 0; i++) {
                    char utf8[4];
                    int n = codepoint_to_utf8(cell.chars[i], utf8);
                    if (pos + n < buf_size) {
                        memcpy(buf + pos, utf8, n);
                        pos += n;
                    }
                }
                last_nonspace_pos = pos;
            }
        }

        // Strip trailing whitespace only at hard line boundaries (not soft-wraps,
        // where trailing spaces are real content that continues on the next row)
        bool next_is_continuation =
            row < sel->end.row && terminal_get_line_continuation(term, row + 1);
        if (!next_is_continuation)
            pos = last_nonspace_pos;

        // Add newline between rows only if next row is NOT a soft-wrap continuation
        if (row < sel->end.row && pos < buf_size - 1) {
            if (!next_is_continuation)
                buf[pos++] = '\n';
        }
    }

    buf[pos] = '\0';
    return buf;
}

// --- Sixel Image API ---

void terminal_add_sixel_image(TerminalBackend *term, SixelImage *image)
{
    if (!term || !image)
        return;

    // Drop oldest if at capacity
    if (term->sixel_image_count >= TERM_MAX_SIXEL_IMAGES) {
        sixel_image_free(term->sixel_images[0]);
        memmove(&term->sixel_images[0], &term->sixel_images[1],
                (TERM_MAX_SIXEL_IMAGES - 1) * sizeof(SixelImage *));
        term->sixel_image_count = TERM_MAX_SIXEL_IMAGES - 1;
    }

    term->sixel_images[term->sixel_image_count++] = image;
}

void terminal_clear_sixel_images(TerminalBackend *term)
{
    if (!term)
        return;

    for (int i = 0; i < term->sixel_image_count; i++) {
        sixel_image_free(term->sixel_images[i]);
        term->sixel_images[i] = NULL;
    }
    term->sixel_image_count = 0;
}

void terminal_scroll_sixel_images(TerminalBackend *term, int delta)
{
    if (!term)
        return;

    for (int i = 0; i < term->sixel_image_count; i++) {
        term->sixel_images[i]->cursor_row -= delta;
    }
}

// VS16 widening shift logic.
//
// A row's libvterm columns map to visual columns with a per-row shift
// offset. The offset increments by 1 every time we walk past a
// VS16-marked emoji-presentation cell whose immediate next libvterm cell
// is non-empty (i.e. the application emitted naive output without a
// cursor advance — cat / glow / etc.). When the next cell is empty
// (chars[0] == 0), the application already inserted padding (emacs /
// Claude Code style), so we "absorb" the empty cell into the emoji's
// 2-cell visual extent and don't shift.
//
// Both helpers walk the row in vt space, applying the shift logic.
// Cost: O(target_col) per call. Selection / cursor / mouse events are
// infrequent, so this is acceptable.

// Returns true if (cell at vt_col, peek of vt_col+1) implies we should
// emit a shift after rendering vt_col. Caller fetches the cell.
static bool vs16_shift_after(TerminalBackend *term, int unified_row, int vt_col,
                             const TerminalCell *cell, int rows_unused)
{
    (void)rows_unused;
    if (!unicode_cell_is_vs16_emoji(cell->chars, TERM_MAX_CHARS_PER_CELL))
        return false;
    TerminalCell next;
    if (read_cell_unified(term, unified_row, vt_col + 1, &next) < 0)
        return false;
    // Empty cell at vt+1 → emacs/Claude already shifted, no extra shift
    return next.chars[0] != 0;
}

int terminal_vt_col_to_vis_col(TerminalBackend *term, int unified_row, int vt_col)
{
    if (!term || vt_col <= 0)
        return vt_col < 0 ? 0 : vt_col;

    int vis = 0;
    for (int c = 0; c < vt_col; c++) {
        TerminalCell cell;
        if (read_cell_unified(term, unified_row, c, &cell) < 0) {
            vis++;
            continue;
        }
        // Visual span of this libvterm cell
        int span = cell.width > 0 ? cell.width : 1;
        // VS16 widening adds an extra visual cell (the +1 shift)
        if (vs16_shift_after(term, unified_row, c, &cell, 0))
            span++;
        vis += span;
    }
    return vis;
}

int terminal_vis_col_to_vt_col(TerminalBackend *term, int unified_row, int vis_col)
{
    if (!term || vis_col <= 0)
        return vis_col < 0 ? 0 : vis_col;

    int vis = 0;
    int vt = 0;
    while (vis < vis_col) {
        TerminalCell cell;
        if (read_cell_unified(term, unified_row, vt, &cell) < 0) {
            vis++;
            vt++;
            continue;
        }
        int span = cell.width > 0 ? cell.width : 1;
        if (vs16_shift_after(term, unified_row, vt, &cell, 0))
            span++;
        if (vis + span > vis_col) {
            // The target visual column lands inside this cell — return
            // the cell's vt col (the visible cell the user pointed at).
            return vt;
        }
        vis += span;
        vt++;
    }
    return vt;
}
