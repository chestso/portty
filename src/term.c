/*
 * portty — Terminal abstraction: wrappers, selection, and shared logic
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "term.h"
#include "term_cfr.h"
#include <stdlib.h>
#include <string.h>

TerminalBackend *terminal_init(TerminalBackend *backend, const CfrConfig *cfg)
{
    if (!backend || !backend->init)
        return NULL;

    if (!backend->init(backend, cfg))
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
    term->hovered_hyperlink_id = 0;
    term->resize(term, width, height);
}

void terminal_set_scrollback_size(TerminalBackend *term, int lines)
{
    if (!term || !term->set_scrollback_size)
        return;
    term->set_scrollback_size(term, lines);
}

int terminal_process_input(TerminalBackend *term, const char *input, size_t len)
{
    if (!term || !term->process_input)
        return -1;
    /* PTY output may rewrite the cell under the mouse. The next motion
     * event will re-resolve. */
    term->hovered_hyperlink_id = 0;
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

void terminal_flush_damage(TerminalBackend *term)
{
    if (!term || !term->flush_damage)
        return;
    term->flush_damage(term);
}

void terminal_mark_dirty(TerminalBackend *term)
{
    if (!term || !term->mark_dirty)
        return;
    term->mark_dirty(term);
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

int terminal_get_scrollback_capacity(TerminalBackend *term)
{
    if (!term || !term->get_scrollback_capacity)
        return 0;
    return term->get_scrollback_capacity(term);
}

int terminal_consume_pushed_rows(TerminalBackend *term)
{
    if (!term || !term->consume_pushed_rows)
        return 0;
    return term->consume_pushed_rows(term);
}

int terminal_consume_popped_rows(TerminalBackend *term)
{
    if (!term || !term->consume_popped_rows)
        return 0;
    return term->consume_popped_rows(term);
}

bool terminal_consume_selection_damaged(TerminalBackend *term)
{
    if (!term || !term->consume_selection_damaged)
        return false;
    return term->consume_selection_damaged(term);
}

int terminal_get_scrollback_cell(TerminalBackend *term, int scrollback_row, int col,
                                 TerminalCell *cell)
{
    if (!term || !term->get_scrollback_cell || !cell)
        return -1;
    return term->get_scrollback_cell(term, scrollback_row, col, cell);
}

size_t terminal_cell_get_grapheme(TerminalBackend *term, int unified_row, int col,
                                  uint32_t *out, size_t cap)
{
    if (!term || !term->get_grapheme || !out || cap == 0)
        return 0;
    return term->get_grapheme(term, unified_row, col, out, cap);
}

size_t terminal_cell_get_hyperlink(TerminalBackend *term, int unified_row, int col,
                                   char *out, size_t cap)
{
    if (!term || !term->get_hyperlink || !out || cap == 0)
        return 0;
    return term->get_hyperlink(term, unified_row, col, out, cap);
}

bool terminal_hyperlink_is_safe(const char *uri)
{
    if (!uri)
        return false;
    /* Allow-list: only the schemes a desktop user expects to invoke from a
     * terminal click. javascript:, data:, vbscript:, etc. stay refused —
     * pasted/escaped link smuggling is the realistic threat. file:// is
     * allowed: tools like Claude Code, compilers, and grep emit file:///abs
     * links to project files, and the required Ctrl-click is the
     * human-in-the-loop safeguard against a smuggled target. */
    static const char *const allowed[] = {
        "http://", "https://", "ftp://", "ftps://", "mailto:", "file://", NULL
    };
    for (int i = 0; allowed[i]; i++) {
        size_t n = strlen(allowed[i]);
        /* Case-insensitive scheme compare. */
        bool match = true;
        for (size_t k = 0; k < n; k++) {
            char a = uri[k];
            char b = allowed[i][k];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + ('a' - 'A'));
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

void terminal_set_hovered_hyperlink(TerminalBackend *term, uint16_t id)
{
    if (!term)
        return;
    if (term->hovered_hyperlink_id == id)
        return;
    term->hovered_hyperlink_id = id;
}

uint16_t terminal_hovered_hyperlink(const TerminalBackend *term)
{
    return term ? term->hovered_hyperlink_id : 0;
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

/* Normalize paste text so each logical line produces a single Enter.
 *
 * The PTY line discipline treats both '\r' and '\n' as a carriage
 * return / Enter. Windows clipboards store lines as CRLF, so pasting
 * them verbatim sends two Enters per line — one for '\r' and one for
 * '\n' — which shows up as a blank line after every pasted line. Bare
 * LF (X11/Wayland clipboards) is also accepted and mapped to CR.
 *
 * The result is NUL-terminated and returned in a malloc'd buffer the
 * caller must free(); NULL on allocation failure. *out_len receives
 * the byte length excluding the NUL. */
char *terminal_paste_normalize(const char *text, size_t len, size_t *out_len)
{
    if (!text || len == 0) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    /* Worst case: every byte becomes '\r' (only happens for a run of
     * bare LFs), so the buffer is never larger than len. */
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\r') {
            buf[j++] = '\r';
            /* Skip a following LF so CRLF collapses to one CR. */
            if (i + 1 < len && text[i + 1] == '\n')
                i++;
        } else if (c == '\n') {
            /* Bare LF → CR (Enter). */
            buf[j++] = '\r';
        } else {
            buf[j++] = c;
        }
    }
    buf[j] = '\0';
    if (out_len)
        *out_len = j;
    return buf;
}

bool terminal_get_line_continuation(TerminalBackend *term, int row)
{
    if (!term || !term->get_line_continuation)
        return false;
    return term->get_line_continuation(term, row);
}

bool terminal_get_mode(TerminalBackend *term, CfrMode mode)
{
    if (!term || !term->get_mode)
        return false;
    return term->get_mode(term, mode);
}

// --- Selection API (thin wrappers over coffer) ---

static CfrTerm *get_cfr(TerminalBackend *term)
{
    return term_cfr_get_cfr_term(term);
}

static CfrSelectionMode map_mode(TerminalSelectMode mode)
{
    switch (mode) {
    case TERM_SELECT_CHAR:
        return CFR_SEL_CHAR;
    case TERM_SELECT_WORD:
        return CFR_SEL_WORD;
    case TERM_SELECT_LINE:
        return CFR_SEL_LINE;
    default:
        return CFR_SEL_NONE;
    }
}

void terminal_selection_clear(TerminalBackend *term)
{
    CfrTerm *vt = get_cfr(term);
    if (vt)
        cfr_selection_clear(vt);
}

bool terminal_selection_active(TerminalBackend *term)
{
    CfrTerm *vt = get_cfr(term);
    return vt ? cfr_selection_active(vt) : false;
}

void terminal_selection_adjust_scroll(TerminalBackend *term, int pushed_rows)
{
    /* No-op: coffer handles scroll adjustment inline during VT processing. */
    (void)term;
    (void)pushed_rows;
}

bool terminal_selection_overlaps_damage(TerminalBackend *term,
                                        int start_row, int start_col,
                                        int end_row, int end_col)
{
    /* No-op: coffer handles draw-clear inline during VT processing. */
    (void)term;
    (void)start_row;
    (void)start_col;
    (void)end_row;
    (void)end_col;
    return false;
}

void terminal_selection_set_word_chars(TerminalBackend *term, const char *chars)
{
    CfrTerm *vt = get_cfr(term);
    if (vt)
        cfr_selection_set_word_chars(vt, chars);
}

void terminal_set_selection_callback(TerminalBackend *term, TerminalSelectionChangeFn cb,
                                     void *user_data)
{
    if (!term)
        return;
    term->selection_change_cb = cb;
    term->selection_change_data = user_data;
}

void terminal_set_clipboard_set_callback(TerminalBackend *term, TerminalClipboardSetFn cb,
                                         void *user_data)
{
    if (!term)
        return;
    term->clipboard_set_cb = cb;
    term->clipboard_set_data = user_data;
}

void terminal_set_cwd_callback(TerminalBackend *term, TerminalCwdFn cb,
                               void *user_data)
{
    if (!term)
        return;
    term->cwd_cb = cb;
    term->cwd_cb_data = user_data;
}

void terminal_selection_start(TerminalBackend *term, int row, int col, TerminalSelectMode mode)
{
    CfrTerm *vt = get_cfr(term);
    if (vt)
        cfr_selection_start(vt, row, col, map_mode(mode));
}

void terminal_selection_update(TerminalBackend *term, int row, int col)
{
    CfrTerm *vt = get_cfr(term);
    if (vt)
        cfr_selection_update(vt, row, col);
}

void terminal_selection_extend(TerminalBackend *term, int row, int col)
{
    CfrTerm *vt = get_cfr(term);
    if (vt)
        cfr_selection_extend(vt, row, col);
}

bool terminal_cell_in_selection(TerminalBackend *term, int row, int col)
{
    CfrTerm *vt = get_cfr(term);
    return vt ? cfr_selection_in_cell(vt, row, col) : false;
}

char *terminal_selection_get_text(TerminalBackend *term)
{
    CfrTerm *vt = get_cfr(term);
    return vt ? cfr_selection_get_text(vt) : NULL;
}

// --- Sixel Image API ---
//
// Decode, storage, scrolling, and clearing all live in the VT engine
// (coffer); these are thin pass-throughs to the backend.

const CfrImage *terminal_get_images(TerminalBackend *term, int *count)
{
    if (count)
        *count = 0;
    if (!term || !term->get_images)
        return NULL;
    return term->get_images(term, count);
}

const CfrImagePlacement *terminal_get_image_placements(TerminalBackend *term,
                                                       int *count)
{
    if (count)
        *count = 0;
    if (!term || !term->get_image_placements)
        return NULL;
    return term->get_image_placements(term, count);
}

void terminal_set_cell_px(TerminalBackend *term, int cell_w, int cell_h)
{
    if (term && term->set_cell_px)
        term->set_cell_px(term, cell_w, cell_h);
}

void terminal_set_content_scale(TerminalBackend *term, float scale)
{
    if (term && term->set_content_scale)
        term->set_content_scale(term, scale);
}

const CfrLottie *terminal_get_lotties(TerminalBackend *term, int *count)
{
    if (count)
        *count = 0;
    if (!term || !term->get_lotties)
        return NULL;
    return term->get_lotties(term, count);
}

const CfrLottiePlacement *terminal_get_lottie_placements(TerminalBackend *term,
                                                         uint64_t id, int *count)
{
    if (count)
        *count = 0;
    if (!term || !term->get_lottie_placements)
        return NULL;
    return term->get_lottie_placements(term, id, count);
}

bool terminal_lottie_tick(TerminalBackend *term, uint64_t now_us)
{
    if (!term || !term->lottie_tick)
        return false;
    return term->lottie_tick(term, now_us);
}

int terminal_lottie_count(TerminalBackend *term)
{
    if (!term || !term->lottie_count)
        return 0;
    return term->lottie_count(term);
}

// Emoji width paradigm.
//
// coffer computes UAX #11 + #29 cluster widths at insertion time and
// stores them on the cell, so VS16 emoji come through with width=2 and
// continuation cells with width=0. The renderer walks the row in plain
// vt-column order and increments by `cell.width` — no peek-ahead, no
// dual coordinate spaces. The iterator is kept for renderer-side
// scrollback-aware lookups.

void terminal_row_iter_init(TerminalRowIter *it, TerminalBackend *term,
                            int unified_row, int max_vt_cols)
{
    if (!it)
        return;
    it->term = term;
    it->unified_row = unified_row;
    it->max_vt_cols = max_vt_cols;
    it->next_vt_col = 0;
    it->next_vis_col = 0;
    it->vt_col = 0;
    it->vis_col = 0;
    it->pres_w = 0;
    memset(&it->cell, 0, sizeof(it->cell));
}

bool terminal_row_iter_next(TerminalRowIter *it)
{
    if (!it || !it->term || it->next_vt_col >= it->max_vt_cols)
        return false;

    it->vt_col = it->next_vt_col;
    it->vis_col = it->next_vis_col;

    // Read cell from visible area or scrollback
    int rc;
    if (it->unified_row >= 0) {
        rc = terminal_get_cell(it->term, it->unified_row, it->vt_col, &it->cell);
    } else {
        int scrollback_index = -(it->unified_row + 1);
        rc = terminal_get_scrollback_cell(it->term, scrollback_index, it->vt_col, &it->cell);
    }

    if (rc < 0) {
        memset(&it->cell, 0, sizeof(it->cell));
        it->pres_w = 1;
        it->next_vt_col = it->vt_col + 1;
        it->next_vis_col = it->vis_col + 1;
        return true;
    }

    int advance = it->cell.width > 0 ? it->cell.width : 1;
    it->pres_w = advance;
    it->next_vt_col = it->vt_col + advance;
    it->next_vis_col = it->vis_col + advance;
    return true;
}

// vt and visual column space are now identical (bvt stores width on the
// cell). These wrappers stay as identity so existing callers compile;
// they will be deleted along with their callsites in a follow-up.
int terminal_vt_col_to_vis_col(TerminalBackend *term, int unified_row, int vt_col)
{
    (void)term;
    (void)unified_row;
    return vt_col < 0 ? 0 : vt_col;
}

int terminal_vis_col_to_vt_col(TerminalBackend *term, int unified_row, int vis_col)
{
    (void)term;
    (void)unified_row;
    return vis_col < 0 ? 0 : vis_col;
}
