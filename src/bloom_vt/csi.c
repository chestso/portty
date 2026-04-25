/*
 * bloom-vt — CSI dispatcher.
 *
 * Initial cut handles cursor moves, erase-in-line / erase-in-display,
 * and SGR reset. The full SGR repertoire (colors, 4:N underlines,
 * 38/48/58 8-bit and 24-bit colors) lands in a follow-up step.
 */

#include "bloom_vt_internal.h"

#include <stdio.h>
#include <string.h>

static int param_or(BvtTerm *vt, int idx, int defval)
{
    BvtParser *p = &vt->parser;
    if (idx >= p->param_count)
        return defval;
    if (p->params[idx] == 0 && !p->param_present && idx == p->param_count - 1)
        return defval;
    return (p->params[idx] == 0) ? defval : (int)p->params[idx];
}

static bool has_intermediate(BvtTerm *vt, uint8_t b)
{
    BvtParser *p = &vt->parser;
    for (int i = 0; i < p->intermediate_count; ++i)
        if (p->intermediates[i] == b)
            return true;
    return false;
}

static void cursor_to(BvtTerm *vt, int row, int col)
{
    if (row < 0)
        row = 0;
    if (col < 0)
        col = 0;
    if (row >= vt->rows)
        row = vt->rows - 1;
    if (col >= vt->cols)
        col = vt->cols - 1;
    vt->cursor.row = row;
    vt->cursor.col = col;
    vt->cursor.pending_wrap = false;
}

/* CUP/HVP/VPA destination — applies DECOM (origin mode): when on, the
 * `row` argument is relative to the scroll region, and the cursor is
 * clamped to the scroll region. When off, behaves like cursor_to. */
static void cursor_to_origin(BvtTerm *vt, int row, int col)
{
    if (vt->decom) {
        int top = vt->scroll_top;
        int bot = vt->scroll_bottom;
        if (row < 0)
            row = 0;
        row += top;
        if (row > bot)
            row = bot;
        if (col < 0)
            col = 0;
        if (col >= vt->cols)
            col = vt->cols - 1;
        vt->cursor.row = row;
        vt->cursor.col = col;
        vt->cursor.pending_wrap = false;
        return;
    }
    cursor_to(vt, row, col);
}

static void reset_pen(BvtTerm *vt)
{
    memset(&vt->cursor.pen, 0, sizeof(vt->cursor.pen));
    vt->cursor.pen.color_flags =
        BVT_COLOR_DEFAULT_FG | BVT_COLOR_DEFAULT_BG | BVT_COLOR_DEFAULT_UL;
}

/* Consume an SGR color sub-sequence starting at index `i`. Sets
 * *out_rgb / *out_default per the parsed mode. Returns the new index
 * after the sub-sequence. Both 38;5;N and 38:5:N and 38;2;R;G;B and
 * 38:2:R:G:B forms are accepted (xterm and ITU/ISO variants). */
static int parse_color_arg(BvtTerm *vt, int i, uint32_t *out_rgb, bool *out_default)
{
    BvtParser *p = &vt->parser;
    if (i >= p->param_count) {
        *out_default = true;
        return i;
    }
    uint32_t mode = p->params[i++];
    if (mode == 5) {
        if (i < p->param_count) {
            *out_rgb = bvt_palette_lookup(vt, (uint8_t)p->params[i++]);
            *out_default = false;
        }
    } else if (mode == 2) {
        /* CSI 38;2;R;G;B  or  CSI 38:2::R:G:B — ITU form has an empty
         * "colourspace ID" slot. We accept either. */
        uint32_t r = (i < p->param_count) ? p->params[i++] : 0;
        uint32_t g = (i < p->param_count) ? p->params[i++] : 0;
        uint32_t b = (i < p->param_count) ? p->params[i++] : 0;
        *out_rgb = ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
        *out_default = false;
    }
    return i;
}

static void sgr_dispatch(BvtTerm *vt)
{
    BvtParser *p = &vt->parser;
    if (p->param_count == 0) {
        reset_pen(vt);
        return;
    }
    for (int i = 0; i < p->param_count; /* increment per-case */) {
        uint32_t s = p->params[i++];
        switch (s) {
        case 0:
            reset_pen(vt);
            break;
        case 1:
            vt->cursor.pen.attrs |= BVT_ATTR_BOLD;
            break;
        case 3:
            vt->cursor.pen.attrs |= BVT_ATTR_ITALIC;
            break;
        case 4:
        {
            /* CSI 4:N m — extended underline subparameter form.
             * We don't currently surface sub-params separately, so
             * map: SGR 4 → single. SGR 4:N is parsed as SGR 4
             * followed by parameter N if `:` was used, but our
             * parser folds `:` and `;` together, so 4:3 looks like
             * (4)(3). Detect and consume. */
            uint8_t style = BVT_UL_SINGLE;
            if (i < p->param_count) {
                /* Heuristic: if the next param is in 0..5 it's
                 * the underline style. */
                uint32_t n = p->params[i];
                if (n <= 5) {
                    style = (uint8_t)n;
                    i++;
                }
            }
            vt->cursor.pen.underline = style;
            break;
        }
        case 5:
            vt->cursor.pen.attrs |= BVT_ATTR_BLINK;
            break;
        case 7:
            vt->cursor.pen.attrs |= BVT_ATTR_REVERSE;
            break;
        case 9:
            vt->cursor.pen.attrs |= BVT_ATTR_STRIKETHROUGH;
            break;
        case 22:
            vt->cursor.pen.attrs &= (uint16_t)~BVT_ATTR_BOLD;
            break;
        case 23:
            vt->cursor.pen.attrs &= (uint16_t)~BVT_ATTR_ITALIC;
            break;
        case 24:
            vt->cursor.pen.underline = BVT_UL_NONE;
            break;
        case 25:
            vt->cursor.pen.attrs &= (uint16_t)~BVT_ATTR_BLINK;
            break;
        case 27:
            vt->cursor.pen.attrs &= (uint16_t)~BVT_ATTR_REVERSE;
            break;
        case 29:
            vt->cursor.pen.attrs &= (uint16_t)~BVT_ATTR_STRIKETHROUGH;
            break;
        /* Foreground 16-color (30-37 normal, 90-97 bright). */
        case 30:
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
        {
            vt->cursor.pen.fg_rgb = bvt_palette_lookup(vt, (uint8_t)(s - 30));
            vt->cursor.pen.color_flags &= (uint16_t)~BVT_COLOR_DEFAULT_FG;
            break;
        }
        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
        {
            vt->cursor.pen.fg_rgb = bvt_palette_lookup(vt, (uint8_t)(s - 90 + 8));
            vt->cursor.pen.color_flags &= (uint16_t)~BVT_COLOR_DEFAULT_FG;
            break;
        }
        case 38:
        {
            bool def = false;
            uint32_t rgb = 0;
            i = parse_color_arg(vt, i, &rgb, &def);
            if (def) {
                vt->cursor.pen.color_flags |= BVT_COLOR_DEFAULT_FG;
            } else {
                vt->cursor.pen.fg_rgb = rgb;
                vt->cursor.pen.color_flags &= (uint16_t)~BVT_COLOR_DEFAULT_FG;
            }
            break;
        }
        case 39:
            vt->cursor.pen.color_flags |= BVT_COLOR_DEFAULT_FG;
            break;
        /* Background 16-color (40-47, 100-107). */
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
        {
            vt->cursor.pen.bg_rgb = bvt_palette_lookup(vt, (uint8_t)(s - 40));
            vt->cursor.pen.color_flags &= (uint16_t)~BVT_COLOR_DEFAULT_BG;
            break;
        }
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
        {
            vt->cursor.pen.bg_rgb = bvt_palette_lookup(vt, (uint8_t)(s - 100 + 8));
            vt->cursor.pen.color_flags &= (uint16_t)~BVT_COLOR_DEFAULT_BG;
            break;
        }
        case 48:
        {
            bool def = false;
            uint32_t rgb = 0;
            i = parse_color_arg(vt, i, &rgb, &def);
            if (def) {
                vt->cursor.pen.color_flags |= BVT_COLOR_DEFAULT_BG;
            } else {
                vt->cursor.pen.bg_rgb = rgb;
                vt->cursor.pen.color_flags &= (uint16_t)~BVT_COLOR_DEFAULT_BG;
            }
            break;
        }
        case 49:
            vt->cursor.pen.color_flags |= BVT_COLOR_DEFAULT_BG;
            break;
        /* Underline color (58/59). */
        case 58:
        {
            bool def = false;
            uint32_t rgb = 0;
            i = parse_color_arg(vt, i, &rgb, &def);
            if (def) {
                vt->cursor.pen.color_flags |= BVT_COLOR_DEFAULT_UL;
            } else {
                vt->cursor.pen.ul_rgb = rgb;
                vt->cursor.pen.color_flags &= (uint16_t)~BVT_COLOR_DEFAULT_UL;
            }
            break;
        }
        case 59:
            vt->cursor.pen.color_flags |= BVT_COLOR_DEFAULT_UL;
            break;
        default:
            /* SGR 2 (faint), 6 (rapid blink), 8 (conceal), 21
             * (double-underline), 28 (reveal), 53 (overline) etc.
             * are accepted-but-ignored for now. */
            break;
        }
    }
}

static void mode_set(BvtTerm *vt, bool on)
{
    BvtParser *p = &vt->parser;
    bool dec = has_intermediate(vt, '?');
    for (int i = 0; i < p->param_count; ++i) {
        uint32_t m = p->params[i];
        if (!dec) {
            /* ANSI modes — minimal handling. */
            continue;
        }
        switch (m) {
        case 1: /* DECCKM */
            vt->decckm = on;
            break;
        case 6: /* DECOM origin mode */
            vt->decom = on;
            /* Setting DECOM also homes the cursor: with origin mode
             * on, home is the top-left of the scroll region. */
            vt->cursor.row = on ? vt->scroll_top : 0;
            vt->cursor.col = 0;
            vt->cursor.pending_wrap = false;
            break;
        case 25: /* DECTCEM cursor visible */
            vt->modes[BVT_MODE_CURSOR_VISIBLE] = on;
            vt->cursor.visible = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(BVT_MODE_CURSOR_VISIBLE, on, vt->callback_user);
            break;
        case 12: /* AT&T cursor blink */
            vt->modes[BVT_MODE_CURSOR_BLINK] = on;
            vt->cursor.blink = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(BVT_MODE_CURSOR_BLINK, on, vt->callback_user);
            break;
        case 1049: /* altscreen + save/restore cursor */
            bvt_set_altscreen(vt, on, true);
            break;
        case 47:
        case 1047: /* raw altscreen */
            bvt_set_altscreen(vt, on, false);
            break;
        case 1000:
            vt->modes[BVT_MODE_MOUSE_BTN_EVENT] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(BVT_MODE_MOUSE_BTN_EVENT, on, vt->callback_user);
            break;
        case 1002:
            vt->modes[BVT_MODE_MOUSE_DRAG] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(BVT_MODE_MOUSE_DRAG, on, vt->callback_user);
            break;
        case 1003:
            vt->modes[BVT_MODE_MOUSE_ANY_EVENT] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(BVT_MODE_MOUSE_ANY_EVENT, on, vt->callback_user);
            break;
        case 1006:
            vt->modes[BVT_MODE_MOUSE_SGR] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(BVT_MODE_MOUSE_SGR, on, vt->callback_user);
            break;
        case 2004:
            vt->modes[BVT_MODE_BRACKETED_PASTE] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(BVT_MODE_BRACKETED_PASTE, on, vt->callback_user);
            break;
        case 2027:
            vt->modes[BVT_MODE_GRAPHEME_CLUSTERS] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(BVT_MODE_GRAPHEME_CLUSTERS, on, vt->callback_user);
            break;
        case 2026:
            vt->modes[BVT_MODE_SYNC_OUTPUT] = on;
            if (vt->callbacks.set_mode)
                vt->callbacks.set_mode(BVT_MODE_SYNC_OUTPUT, on, vt->callback_user);
            break;
        default:
            break;
        }
    }
}

void bvt_csi_dispatch(BvtTerm *vt, uint8_t final)
{
    bvt_flush_cluster(vt); /* commit any pending cluster before state-changing op */
    BvtParser *p = &vt->parser;
    int p1 = param_or(vt, 0, 1);
    int p2 = param_or(vt, 1, 1);

    switch (final) {
    /* Cursor moves */
    case 'A':
        cursor_to(vt, vt->cursor.row - p1, vt->cursor.col);
        break; /* CUU */
    case 'B':
        cursor_to(vt, vt->cursor.row + p1, vt->cursor.col);
        break; /* CUD */
    case 'C':
        cursor_to(vt, vt->cursor.row, vt->cursor.col + p1);
        break; /* CUF */
    case 'D':
        cursor_to(vt, vt->cursor.row, vt->cursor.col - p1);
        break; /* CUB */
    case 'E':
        cursor_to(vt, vt->cursor.row + p1, 0);
        break; /* CNL */
    case 'F':
        cursor_to(vt, vt->cursor.row - p1, 0);
        break; /* CPL */
    case 'G':
        cursor_to(vt, vt->cursor.row, p1 - 1);
        break; /* CHA */
    case 'H':
    case 'f': /* CUP / HVP */
        cursor_to_origin(vt, p1 - 1, p2 - 1);
        break;
    case 'd':
        cursor_to_origin(vt, p1 - 1, vt->cursor.col);
        break; /* VPA */

    case 'J':
        bvt_erase_in_display(vt, param_or(vt, 0, 0));
        break;
    case 'K':
        bvt_erase_in_line(vt, param_or(vt, 0, 0));
        break;

    case 'S':
        bvt_scroll_up(vt, p1);
        break;
    case 'T':
        bvt_scroll_down(vt, p1);
        break;

    case 'r':
    { /* DECSTBM — Set Top and Bottom Margins */
        int top = param_or(vt, 0, 1) - 1;
        int bottom = param_or(vt, 1, vt->rows) - 1;
        if (top < 0)
            top = 0;
        if (bottom >= vt->rows)
            bottom = vt->rows - 1;
        /* Per VT100/xterm: an invalid region (top >= bottom) is
         * rejected — leave the previous scroll region and cursor
         * unchanged. brick (Haskell vty) emits `CSI 1;1 r` as part
         * of its inline-render setup; if we accepted that as a
         * one-row region, every subsequent `\n` would be pinned to
         * that single row and inline UIs would draw at row 0,
         * clobbering the prompts above. */
        if (top >= bottom)
            break;
        vt->scroll_top = top;
        vt->scroll_bottom = bottom;
        /* DECSTBM homes the cursor: with origin mode on, home is
         * the top-left of the new scroll region. */
        cursor_to(vt, vt->decom ? top : 0, 0);
        break;
    }

    case 'h':
        mode_set(vt, true);
        break;
    case 'l':
        mode_set(vt, false);
        break;

    case 'm':
        if (has_intermediate(vt, '>')) {
            /* CSI > Pp ; Pv m — xterm `modifyOtherKeys` and friends
             * (key-resource set/reset). We provide the same coverage
             * via the kitty keyboard protocol, so this is a no-op.
             * Critical: do NOT fall through to sgr_dispatch — that
             * would interpret "4;2" as SGR underline + faint and
             * paint Claude Code's hyperlink-tagged words underlined. */
            break;
        }
        sgr_dispatch(vt);
        break;

    case '@':
        bvt_insert_chars(vt, p1);
        break; /* ICH */
    case 'P':
        bvt_delete_chars(vt, p1);
        break; /* DCH */
    case 'X':
        bvt_erase_chars(vt, p1);
        break; /* ECH */
    case 'L':
        bvt_insert_lines(vt, p1);
        break; /* IL  */
    case 'M':
        bvt_delete_lines(vt, p1);
        break; /* DL  */

    case 'g':
    { /* TBC — Tab Clear */
        int mode = param_or(vt, 0, 0);
        if (!vt->tabstops)
            break;
        if (mode == 0) {
            if (vt->cursor.col >= 0 && vt->cursor.col < vt->cols)
                vt->tabstops[vt->cursor.col] = 0;
        } else if (mode == 3) {
            memset(vt->tabstops, 0, (size_t)vt->cols);
        }
        break;
    }

    case 's': /* CSI s — Save Cursor (ANSI.SYS form). Only the bare
               * variant; `CSI ? s` is XTSAVE for DEC private modes
               * and `CSI < s` / `CSI > s` are vendor extensions we
               * do not implement. */
        if (p->intermediate_count == 0)
            vt->saved_cursor = vt->cursor;
        break;
    case 'u':
        if (p->intermediate_count == 0) {
            /* CSI u — ANSI.SYS Restore Cursor. */
            vt->cursor = vt->saved_cursor;
        } else if (has_intermediate(vt, '?')) {
            /* CSI ? u — kitty keyboard query. Reply with the active
             * flags at the top of the stack as `CSI ? <flags> u`. The
             * `u` is the final byte; no space separator. */
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "\x1b[?%uu",
                             (unsigned)vt->kitty_kb_stack[vt->kitty_kb_depth]);
            bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
        } else if (has_intermediate(vt, '>')) {
            /* CSI > flags u — kitty keyboard push. The new top of the
             * stack is `flags`. If the stack is full, overwrite the
             * top in place rather than dropping the request. */
            uint32_t flags = (uint32_t)param_or(vt, 0, 0);
            if (vt->kitty_kb_depth + 1 < 16)
                vt->kitty_kb_depth++;
            vt->kitty_kb_stack[vt->kitty_kb_depth] = flags;
        } else if (has_intermediate(vt, '<')) {
            /* CSI < n u — kitty keyboard pop n levels (default 1).
             * Never goes below depth 0; the baseline mask at index 0
             * is left at zero so popping past the bottom restores
             * legacy emit behaviour. */
            int n = param_or(vt, 0, 1);
            if (n < 1)
                n = 1;
            while (n-- > 0 && vt->kitty_kb_depth > 0) {
                vt->kitty_kb_stack[vt->kitty_kb_depth] = 0;
                vt->kitty_kb_depth--;
            }
        } else if (has_intermediate(vt, '=')) {
            /* CSI = flags ; mode u — set/clear/replace the active
             * flags. mode 1 = OR in, 2 = mask out, 3 = replace. */
            uint32_t flags = (uint32_t)param_or(vt, 0, 0);
            int mode = param_or(vt, 1, 1);
            uint32_t *top = &vt->kitty_kb_stack[vt->kitty_kb_depth];
            if (mode == 1)
                *top |= flags;
            else if (mode == 2)
                *top &= ~flags;
            else if (mode == 3)
                *top = flags;
        }
        break;

    case 'n': /* DSR */
        if (param_or(vt, 0, 0) == 6) {
            /* Cursor position report. */
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dR",
                             vt->cursor.row + 1, vt->cursor.col + 1);
            bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
        } else if (param_or(vt, 0, 0) == 5) {
            bvt_emit_bytes(vt, (const uint8_t *)"\x1b[0n", 4);
        }
        break;

    case 'c': /* Primary Device Attributes. */
        if (!has_intermediate(vt, '>')) {
            /* Respond as VT220 with ANSI colors. */
            bvt_emit_bytes(vt, (const uint8_t *)"\x1b[?62;22c", 9);
        } else {
            /* Secondary DA: identify as bloom-vt with version 0. */
            bvt_emit_bytes(vt, (const uint8_t *)"\x1b[>1;0;0c", 9);
        }
        break;

    default:
        /* TODO: REP, DA3, DECRQM, DECSCUSR cursor shape, etc. */
        break;
    }
    (void)p;
}
