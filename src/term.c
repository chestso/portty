#include "term.h"
#include "sixel.h"
#include <stdlib.h>
#include <string.h>

// Default set of characters considered part of a "word" for double-click selection
static const char *default_word_chars =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";

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

// --- Selection API ---

void terminal_selection_clear(TerminalBackend *term)
{
    if (!term)
        return;
    term->selection.active = false;
    term->selection.mode = TERM_SELECT_NONE;
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

    // Scan left
    int left = col;
    while (left > 0) {
        TerminalCell c;
        if (read_cell_unified(term, row, left - 1, &c) < 0)
            break;
        if (char_class(c.chars[0], wchars) != cls)
            break;
        left--;
    }

    // Scan right
    int right = col;
    while (right < cols - 1) {
        TerminalCell c;
        if (read_cell_unified(term, row, right + 1, &c) < 0)
            break;
        if (char_class(c.chars[0], wchars) != cls)
            break;
        right++;
    }

    *out_start = (TerminalPos){ row, left };
    *out_end = (TerminalPos){ row, right };
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

        // Strip trailing whitespace from this row
        pos = last_nonspace_pos;

        // Add newline between rows (not after last)
        if (row < sel->end.row && pos < buf_size - 1)
            buf[pos++] = '\n';
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
