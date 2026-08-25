/*
 * portty — Coffer-backed VT terminal bridge (TerminalBackend)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * term_cfr.c — TerminalBackend bridge to coffer.
 *
 * Mirrors term_vt.c but routes everything through coffer instead of
 * libvterm. Cell conversion translates CfrCell + CfrStyle into the
 * legacy TerminalCell shape so the renderer is unchanged. Selected at
 * startup via PORTTY_VT=coffer; libvterm remains the default
 * during the parallel-development window.
 */

#include "term_cfr.h"
#include "base64.h"
#include "path_compat.h"
#include "common.h"
#include <coffer/coffer.h>
#include <ctype.h>
#include <limits.h>

#ifdef PORTTY_HARDEN_HEAP
#include "heap_harden.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    CfrTerm *vt;

    /* Back-pointer for callbacks that need to reach application-level
     * state stored on TerminalBackend (e.g. the OSC 52 clipboard hook). */
    TerminalBackend *term;

    /* Damage tracking — accumulated rectangle since last clear_redraw.
     * coffer provides its own damage callback; we union into this. */
    bool needs_redraw;
    int damage_top, damage_bottom, damage_left, damage_right;

    /* Latest property values (mirrored from callbacks). */
    char title[256];
    bool cursor_visible;
    bool cursor_blink;
    bool altscreen;
    int mouse_mode;

    TerminalOutputCallback output_cb;
    void *output_user;

    /* Rows pushed to scrollback since last consume. Renderer reads this
     * to keep a held scroll-lock view stable as new content arrives —
     * sb_lines saturates at sb_capacity so it can't be inferred from a
     * before/after diff once the ring is full. */
    int pushed_rows;

    /* Rows popped back from scrollback during scroll-down. Used to
     * adjust selection coordinates upward (positive delta). */
    int popped_rows;

    /* Set by cb_damage when a damage rect overlaps the active selection.
     * The caller (rend_sdl3_process_pty_data) checks this after processing
     * and clears the selection if set. Scroll damage (from cfr_scroll_up/
     * cfr_scroll_down via cfr_damage_all) does NOT set this — the pushed_rows
     * counter and cb_moverect handle selection coordinate tracking for scrolls. */
    bool selection_damaged;

    /* Current terminal dimensions (rows), tracked for full-screen
     * damage detection in cb_damage. */
    int term_rows;
} CfrBackendData;

/* Fire the CWD callback with the given path. On Windows, convert
 * MSYS2/Unix paths to native Windows paths using the exe path stored
 * on TerminalBackend. On failure (or non-Windows), pass the raw path. */
static void fire_cwd_cb(CfrBackendData *d, const char *path)
{
    if (!d->term->cwd_cb)
        return;
#ifdef _WIN32
    if (d->term->exe_path) {
        char native[PATH_MAX];
        if (path_compat_msys_to_win(path, d->term->exe_path, native,
                                    sizeof(native))) {
            d->term->cwd_cb(native, d->term->cwd_cb_data);
            return;
        }
    }
#endif
    d->term->cwd_cb(path, d->term->cwd_cb_data);
}

/* ------------------------------------------------------------------ */
/* Color conversion                                                    */
/* ------------------------------------------------------------------ */

static TerminalColor unpack_rgb(uint32_t rgb, bool is_default,
                                uint32_t fallback)
{
    TerminalColor out;
    if (is_default) {
        out.r = (uint8_t)((fallback >> 16) & 0xFF);
        out.g = (uint8_t)((fallback >> 8) & 0xFF);
        out.b = (uint8_t)(fallback & 0xFF);
        out.is_default = true;
    } else {
        out.r = (uint8_t)((rgb >> 16) & 0xFF);
        out.g = (uint8_t)((rgb >> 8) & 0xFF);
        out.b = (uint8_t)(rgb & 0xFF);
        out.is_default = false;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Damage                                                              */
/* ------------------------------------------------------------------ */

static void damage_init(CfrBackendData *d)
{
    d->needs_redraw = false;
    d->damage_top = d->damage_bottom = d->damage_left = d->damage_right = 0;
}

static void damage_union(CfrBackendData *d, int t, int l, int b, int r)
{
    if (!d->needs_redraw) {
        d->damage_top = t;
        d->damage_bottom = b;
        d->damage_left = l;
        d->damage_right = r;
        d->needs_redraw = true;
    } else {
        if (t < d->damage_top)
            d->damage_top = t;
        if (b > d->damage_bottom)
            d->damage_bottom = b;
        if (l < d->damage_left)
            d->damage_left = l;
        if (r > d->damage_right)
            d->damage_right = r;
    }
}

/* ------------------------------------------------------------------ */
/* coffer callbacks                                                  */
/* ------------------------------------------------------------------ */

static void cb_damage(CfrRect rect, void *user)
{
    CfrBackendData *d = user;
    damage_union(d, rect.start_row, rect.start_col, rect.end_row, rect.end_col);
    /* Skip the overlap check for full-screen damage. cfr_scroll_up/
     * cfr_scroll_down call cfr_damage_all, and apps like less -X
     * redraw the entire screen after scrolling. Both produce a
     * full-screen rect that would always overlap an active selection.
     * Real content overwrites (writes, erases) produce smaller rects. */
    bool full_screen = (rect.start_row == 0 &&
                        rect.start_col == 0 &&
                        rect.end_row >= d->term_rows - 1);
    if (full_screen)
        return;
    if (d->term && terminal_selection_overlaps_damage(d->term,
                                                      rect.start_row, rect.start_col, rect.end_row, rect.end_col))
        d->selection_damaged = true;
}

static void cb_moverect(CfrRect dst, CfrRect src, void *user)
{
    CfrBackendData *d = user;
    damage_union(d, dst.start_row, dst.start_col, dst.end_row, dst.end_col);
    damage_union(d, src.start_row, src.start_col, src.end_row, src.end_col);

    /* In-screen content moves (scroll regions, SU/SD sequences) that
     * don't go through scrollback. Adjust selection coordinates to
     * follow the moved content. If the selection spans the scroll
     * region boundary, flag for clearing. */
    if (!d->term || !terminal_selection_active(d->term))
        return;

    int delta = dst.start_row - src.start_row;
    if (delta == 0)
        return;

    TerminalSelection *sel = &d->term->selection;
    bool start_in = (sel->start.row >= src.start_row &&
                     sel->start.row <= src.end_row);
    bool end_in = (sel->end.row >= src.start_row &&
                   sel->end.row <= src.end_row);

    if (start_in && end_in) {
        sel->start.row += delta;
        sel->end.row += delta;
        sel->anchor.row += delta;
    } else if (start_in || end_in) {
        d->selection_damaged = true;
    }
}

static void cb_movecursor(CfrCursor cur, void *user)
{
    CfrBackendData *d = user;
    d->cursor_visible = cur.visible;
    d->cursor_blink = cur.blink;
    /* Cursor position is queried lazily via get_cursor_pos. */
}

static void cb_set_title(const char *utf8, void *user)
{
    CfrBackendData *d = user;
    if (!utf8) {
        d->title[0] = '\0';
        return;
    }
    size_t n = strlen(utf8);
    if (n >= sizeof(d->title)) {
        n = sizeof(d->title) - 1;
        /* Don't slice through a UTF-8 codepoint — truncation
         * on mid-codepoint bytes can cause assertion failures.
         * Walk the last `n` bytes back to a leading byte and
         * drop the codepoint if it doesn't fit. */
        if (n > 0) {
            size_t k = n - 1;
            while (k > 0 && ((unsigned char)utf8[k] & 0xC0) == 0x80)
                k--;
            unsigned char lead = (unsigned char)utf8[k];
            int needed =
                ((lead & 0x80) == 0x00) ? 1 : ((lead & 0xE0) == 0xC0) ? 2
                                          : ((lead & 0xF0) == 0xE0)   ? 3
                                          : ((lead & 0xF8) == 0xF0)   ? 4
                                                                      : 1;
            if (k + (size_t)needed > n)
                n = k;
        }
    }
    memcpy(d->title, utf8, n);
    d->title[n] = '\0';
}

static void cb_set_mode(CfrMode mode, bool on, void *user)
{
    CfrBackendData *d = user;
    switch (mode) {
    case CFR_MODE_CURSOR_VISIBLE:
        d->cursor_visible = on;
        break;
    case CFR_MODE_CURSOR_BLINK:
        d->cursor_blink = on;
        break;
    case CFR_MODE_ALTSCREEN:
        d->altscreen = on;
        break;
    case CFR_MODE_MOUSE_X10:
        d->mouse_mode = on ? 1 : 0;
        break;
    case CFR_MODE_MOUSE_BTN_EVENT:
        d->mouse_mode = on ? 1 : 0;
        break;
    case CFR_MODE_MOUSE_DRAG:
        d->mouse_mode = on ? 2 : 0;
        break;
    case CFR_MODE_MOUSE_ANY_EVENT:
        d->mouse_mode = on ? 3 : 0;
        break;
    default:
        break;
    }
}

static void cb_output(const uint8_t *bytes, size_t len, void *user)
{
    CfrBackendData *d = user;
    if (d->output_cb)
        d->output_cb((const char *)bytes, len, d->output_user);
}

static void cb_bell(void *user) { (void)user; /* TODO: visual bell hook */ }

static void cb_sb_push(const CfrCell *c, int n, bool w, void *u)
{
    (void)c;
    (void)n;
    (void)w;
    CfrBackendData *d = u;
    d->pushed_rows++;
}
static void cb_sb_pop(CfrCell *o, int n, void *u)
{
    (void)o;
    (void)n;
    CfrBackendData *d = u;
    d->popped_rows++;
}
static void cb_selection_changed(bool active, void *user)
{
    CfrBackendData *d = user;
    if (d && d->term && d->term->selection_change_cb)
        d->term->selection_change_cb(active, d->term->selection_change_data);
}

/* OSC 52 (set clipboard). Body format: <selection-chars> ';' <base64 | '?'>.
 * We accept any selection (c/p/s/...) and route to one OS clipboard. The
 * '?' query form is silently refused — see the rationale in term.h next to
 * TerminalClipboardSetFn. coffer only forwards non-special OSC codes here
 * (0/1/2 go through set_title; 8 is handled inside the engine), so the
 * code==52 guard is strictly a defense against future engine changes.
 *
 * OSC 7 (set working directory): body is a file:// URI. Used by shells
 * to inform the terminal of CWD changes so Ctrl+Shift+N can spawn a new
 * terminal in the same directory. This is the same approach Windows
 * Terminal uses — PEB-walking doesn't work for ConPTY child processes
 * (ReadProcessMemory returns ERROR_PARTIAL_COPY).
 *
 * OSC 9;9 (ConEmu CWD): body is ';' 9 ';' <path>. Same purpose as OSC 7
 * but using the ConEmu protocol variant. */
static void cb_osc(int code, const char *data, size_t len, void *user)
{
    CfrBackendData *d = user;
    if (!d || !d->term)
        return;
    if (!data || len == 0)
        return;

    if (code == 52 && d->term->clipboard_set_cb) {
        const char *semi = memchr(data, ';', len);
        if (!semi)
            return;
        const char *payload = semi + 1;
        size_t payload_len = len - (size_t)(payload - data);

        if (payload_len == 1 && payload[0] == '?')
            return; /* query refused */

        if (payload_len == 0) {
            d->term->clipboard_set_cb("", 0, d->term->clipboard_set_data);
            return;
        }

        size_t decoded_len = 0;
        uint8_t *decoded = base64_decode(payload, payload_len, &decoded_len);
        if (!decoded)
            return;
        /* 1 MiB cap mirrors xterm-style guard against runaway sequences. */
        if (decoded_len > 1024u * 1024u) {
            free(decoded);
            return;
        }
        d->term->clipboard_set_cb((const char *)decoded, decoded_len,
                                  d->term->clipboard_set_data);
        free(decoded);
        return;
    }

    /* OSC 7: file:// URI → local path. Shells emit this on every cd
     * (bash: PROMPT_COMMAND, fish: vcs_prompt, zsh: chpwd). */
    if (code == 7 && d->term->cwd_cb) {
        /* Make a NUL-terminated copy for parsing */
        char *uri = malloc(len + 1);
        if (!uri)
            return;
        memcpy(uri, data, len);
        uri[len] = '\0';

        /* Convert file:// URI to local path.
         * file:///C:/Users/foo → C:/Users/foo (Windows drive letter)
         * file:///home/foo    → /home/foo    (Unix)               */
        char *path = uri;
        if (strncmp(path, "file://", 7) == 0) {
            path += 7;
            /* Strip the leading / for Windows drive-letter paths
             * (e.g. file:///C:/... → C:/...).  This check runs on
             * all platforms — a file:///C:/ URI always refers to a
             * Windows drive, even when received on a Unix host. */
            if (*path == '/' && path[1] && path[2] == ':')
                path++;
        }

        /* URL-decode %XX sequences in-place */
        char *dst = path, *src = path;
        while (*src) {
            if (*src == '%' && src[1] && src[2]) {
                char hex[3] = { src[1], src[2], '\0' };
                *dst++ = (char)strtol(hex, NULL, 16);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';

        fire_cwd_cb(d, path);
        free(uri);
        return;
    }

    /* OSC 9;9: ConEmu working-directory protocol.
     * Body format: 9;"<path>" or 9;<path> (quotes optional per ConEmu). */
    if (code == 9 && d->term->cwd_cb) {
        const char *semi = memchr(data, ';', len);
        if (!semi)
            return;
        /* Check the sub-command is 9 (OSC 9;9;<path>) */
        int subcmd = 0;
        const char *p = data;
        while (p < semi) {
            if (*p >= '0' && *p <= '9')
                subcmd = subcmd * 10 + (*p - '0');
            else
                break;
            p++;
        }
        if (subcmd != 9)
            return;

        const char *payload = semi + 1;
        size_t payload_len = len - (size_t)(payload - data);

        /* Strip optional surrounding quotes */
        if (payload_len >= 2 && payload[0] == '"' && payload[payload_len - 1] == '"') {
            payload++;
            payload_len -= 2;
        }

        /* Make NUL-terminated copy */
        char *path = malloc(payload_len + 1);
        if (!path)
            return;
        memcpy(path, payload, payload_len);
        path[payload_len] = '\0';

        fire_cwd_cb(d, path);
        free(path);
        return;
    }
}

static void cb_log(CfrLogLevel level, const char *msg, void *user)
{
    (void)user;
    (void)level;
    vlog("coffer: %s", msg);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static bool cfr_back_init(TerminalBackend *term, const CfrConfig *cfg)
{
    CfrBackendData *d = calloc(1, sizeof(*d));
    if (!d)
        return false;
#ifdef PORTTY_HARDEN_HEAP
    heap_harden_init();
    d->vt = cfr_new_with_allocator(cfg, &cfr_hardened_allocator);
#else
    d->vt = cfr_new(cfg);
#endif
    if (!d->vt) {
        free(d);
        return false;
    }
    d->cursor_visible = true;
    d->cursor_blink = true;

    int rows, cols;
    cfr_get_dimensions(d->vt, &rows, &cols);
    d->term_rows = rows;

    CfrCallbacks cb = {
        .damage = cb_damage,
        .moverect = cb_moverect,
        .movecursor = cb_movecursor,
        .bell = cb_bell,
        .set_title = cb_set_title,
        .set_mode = cb_set_mode,
        .output = cb_output,
        .sb_pushline = cb_sb_push,
        .sb_popline = cb_sb_pop,
        .osc = cb_osc,
        .log = cb_log,
        .selection_changed = cb_selection_changed,
    };
    cfr_set_callbacks(d->vt, &cb, d);

    d->term = term;
    term->backend_data = d;
    return true;
}

static void cfr_back_destroy(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return;
    cfr_free(d->vt);
    free(d);
    term->backend_data = NULL;
}

static void cfr_back_resize(TerminalBackend *term, int width, int height)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return;
    cfr_resize(d->vt, height, width);
    damage_init(d);
    d->needs_redraw = true;
    int rows, cols;
    cfr_get_dimensions(d->vt, &rows, &cols);
    d->term_rows = rows;
    if (rows && cols)
        damage_union(d, 0, 0, rows - 1, cols - 1);
}

static int cfr_back_process_input(TerminalBackend *term, const char *input,
                                  size_t len)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return 0;
    /* Damage accumulates inside coffer; needs_redraw becomes true when the
     * caller drains it via terminal_flush_damage() (once per frame). */
    return (int)cfr_input_write(d->vt, (const uint8_t *)input, len);
}

/* ------------------------------------------------------------------ */
/* Cell conversion                                                     */
/* ------------------------------------------------------------------ */

static void convert_cell(CfrTerm *vt, const CfrCell *src, TerminalCell *dst)
{
    (void)vt;
    memset(dst, 0, sizeof(*dst));
    if (!src)
        return;
    /* Width: continuation cells (width=0) are passed through; the
     * renderer treats them as continuation. */
    dst->width = src->width;
    /* Primary codepoint + opaque cluster handle. The renderer fetches the
     * full multi-codepoint sequence (if any) via terminal_cell_get_grapheme,
     * which routes back through cfr_cell_get_grapheme — no truncation at
     * the renderer boundary. */
    dst->cp = src->cp;
    dst->grapheme_id = src->grapheme_id;
    dst->hyperlink_id = src->hyperlink_id;

    /* Style. */
    const CfrStyle *st = cfr_cell_style(vt, src);
    if (!st) {
        dst->fg = unpack_rgb(0, true, cfr_default_fg_rgb());
        dst->bg = unpack_rgb(0, true, cfr_default_bg_rgb());
        dst->ul_color = unpack_rgb(0, true, cfr_default_fg_rgb());
        return;
    }
    dst->attrs.bold = (st->attrs & CFR_ATTR_BOLD) ? 1 : 0;
    dst->attrs.italic = (st->attrs & CFR_ATTR_ITALIC) ? 1 : 0;
    dst->attrs.blink = (st->attrs & CFR_ATTR_BLINK) ? 1 : 0;
    dst->attrs.reverse = (st->attrs & CFR_ATTR_REVERSE) ? 1 : 0;
    dst->attrs.strikethrough = (st->attrs & CFR_ATTR_STRIKETHROUGH) ? 1 : 0;
    dst->attrs.dwl = (st->attrs & CFR_ATTR_DWL) ? 1 : 0;
    if (st->attrs & CFR_ATTR_DHL_TOP)
        dst->attrs.dhl = 1;
    else if (st->attrs & CFR_ATTR_DHL_BOTTOM)
        dst->attrs.dhl = 2;
    dst->attrs.dim = (st->attrs & CFR_ATTR_DIM) ? 1 : 0;
    dst->attrs.invis = (st->attrs & CFR_ATTR_INVIS) ? 1 : 0;
    dst->attrs.font = st->font & 0xF;
    dst->attrs.underline = st->underline & 0x7;

    dst->fg = unpack_rgb(st->fg_rgb,
                         (st->color_flags & CFR_COLOR_DEFAULT_FG) != 0,
                         cfr_default_fg_rgb());
    dst->bg = unpack_rgb(st->bg_rgb,
                         (st->color_flags & CFR_COLOR_DEFAULT_BG) != 0,
                         cfr_default_bg_rgb());
    dst->ul_color = unpack_rgb(st->ul_rgb,
                               (st->color_flags & CFR_COLOR_DEFAULT_UL) != 0,
                               cfr_default_fg_rgb());

    /* Match libvterm backend: pre-swap fg/bg for reverse video so the
     * renderer sees visual colors. */
    if (dst->attrs.reverse) {
        TerminalColor tmp = dst->fg;
        dst->fg = dst->bg;
        dst->bg = tmp;
        dst->bg.is_default = false;
    }
}

static int cfr_back_get_cell(TerminalBackend *term, int row, int col,
                             TerminalCell *cell)
{
    CfrBackendData *d = term->backend_data;
    if (!d || !cell)
        return -1;
    const CfrCell *src = cfr_get_cell(d->vt, row, col);
    convert_cell(d->vt, src, cell);
    return 0;
}

static int cfr_back_get_dimensions(TerminalBackend *term, int *rows, int *cols)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return -1;
    cfr_get_dimensions(d->vt, rows, cols);
    return 0;
}

static TerminalPos cfr_back_get_cursor_pos(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    CfrCursor c = cfr_get_cursor(d->vt);
    return (TerminalPos){ .row = c.row, .col = c.col };
}
static bool cfr_back_get_cursor_visible(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return cfr_get_cursor(d->vt).visible;
}
static const CfrImage *cfr_back_get_images(TerminalBackend *term, int *count)
{
    CfrBackendData *d = term->backend_data;
    return cfr_get_images(d->vt, count);
}
static const CfrImagePlacement *cfr_back_get_image_placements(
    TerminalBackend *term, int *count)
{
    CfrBackendData *d = term->backend_data;
    return cfr_get_image_placements(d->vt, count);
}
static const CfrLottie *cfr_back_get_lotties(TerminalBackend *term, int *count)
{
    CfrBackendData *d = term->backend_data;
    return cfr_get_lotties(d->vt, count);
}
static const CfrLottiePlacement *cfr_back_get_lottie_placements(
    TerminalBackend *term, uint64_t id, int *count)
{
    CfrBackendData *d = term->backend_data;
    return cfr_get_lottie_placements(d->vt, id, count);
}
static bool cfr_back_lottie_tick(TerminalBackend *term, uint64_t now_us)
{
    CfrBackendData *d = term->backend_data;
    return cfr_lottie_tick(d->vt, now_us);
}
static int cfr_back_lottie_count(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return cfr_lottie_active_count(d->vt);
}
static void cfr_back_set_cell_px(TerminalBackend *term, int cell_w, int cell_h)
{
    CfrBackendData *d = term->backend_data;
    cfr_set_cell_pixels(d->vt, cell_w, cell_h);
}
static void cfr_back_set_content_scale(TerminalBackend *term, float scale)
{
    CfrBackendData *d = term->backend_data;
    cfr_set_content_scale(d->vt, scale);
}
static bool cfr_back_get_cursor_blink(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return cfr_get_cursor(d->vt).blink;
}
static const char *cfr_back_get_title(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return d->title;
}

/* ------------------------------------------------------------------ */
/* Damage                                                              */
/* ------------------------------------------------------------------ */

static bool cfr_back_needs_redraw(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return d ? d->needs_redraw : false;
}
static void cfr_back_clear_redraw(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    if (d)
        damage_init(d);
}
/* Drain coffer's accumulated damage: cfr_damage_flush fires cb_damage (which
 * unions into our rect and sets needs_redraw) only when the grid actually
 * changed, and folds in cursor-only moves. Called once per frame. */
static void cfr_back_flush_damage(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    if (d)
        cfr_damage_flush(d->vt);
}
/* Flag a redraw for a change coffer can't see (cursor blink, selection,
 * scroll view, focus, resize). Full-grid repaint, so no precise rect needed. */
static void cfr_back_mark_dirty(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    if (d)
        d->needs_redraw = true;
}
static bool cfr_back_get_damage_rect(TerminalBackend *term,
                                     TerminalDamageRect *rect)
{
    CfrBackendData *d = term->backend_data;
    if (!d || !d->needs_redraw || !rect)
        return false;
    rect->start_row = d->damage_top;
    rect->start_col = d->damage_left;
    rect->end_row = d->damage_bottom;
    rect->end_col = d->damage_right;
    return true;
}

/* ------------------------------------------------------------------ */
/* Scrollback                                                          */
/* ------------------------------------------------------------------ */

static int cfr_back_get_scrollback_lines(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return d ? cfr_get_scrollback_lines(d->vt) : 0;
}
static int cfr_back_get_scrollback_capacity(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return d ? cfr_get_scrollback_capacity(d->vt) : 0;
}
static int cfr_back_consume_pushed_rows(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return 0;
    int n = d->pushed_rows;
    d->pushed_rows = 0;
    return n;
}
static int cfr_back_consume_popped_rows(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return 0;
    int n = d->popped_rows;
    d->popped_rows = 0;
    return n;
}
static bool cfr_back_consume_selection_damaged(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return false;
    bool v = d->selection_damaged;
    d->selection_damaged = false;
    return v;
}
static int cfr_back_get_scrollback_cell(TerminalBackend *term, int sb_row,
                                        int col, TerminalCell *cell)
{
    CfrBackendData *d = term->backend_data;
    if (!d || !cell)
        return -1;
    const CfrCell *src = cfr_get_scrollback_cell(d->vt, sb_row, col);
    convert_cell(d->vt, src, cell);
    return 0;
}

static size_t cfr_back_get_grapheme(TerminalBackend *term, int unified_row,
                                    int col, uint32_t *out, size_t cap)
{
    CfrBackendData *d = term->backend_data;
    if (!d || !out || cap == 0)
        return 0;
    const CfrCell *src = (unified_row >= 0)
                             ? cfr_get_cell(d->vt, unified_row, col)
                             : cfr_get_scrollback_cell(d->vt, -(unified_row + 1), col);
    if (!src)
        return 0;
    return cfr_cell_get_grapheme(d->vt, src, out, cap);
}

static size_t cfr_back_get_hyperlink(TerminalBackend *term, int unified_row,
                                     int col, char *out, size_t cap)
{
    CfrBackendData *d = term->backend_data;
    if (!d || !out || cap == 0)
        return 0;
    const CfrCell *src = (unified_row >= 0)
                             ? cfr_get_cell(d->vt, unified_row, col)
                             : cfr_get_scrollback_cell(d->vt, -(unified_row + 1), col);
    if (!src || src->hyperlink_id == 0)
        return 0;
    /* Reserve one byte for the trailing NUL. bvt returns raw bytes, so
     * we cap the write at cap-1 and add the terminator ourselves. */
    size_t n = cfr_cell_get_hyperlink(d->vt, src, (uint8_t *)out, cap - 1);
    if (n >= cap)
        n = cap - 1;
    out[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------ */
/* Modes / I/O                                                         */
/* ------------------------------------------------------------------ */

static bool cfr_back_is_altscreen(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return d ? d->altscreen : false;
}
static int cfr_back_get_mouse_mode(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    return d ? d->mouse_mode : 0;
}
static void cfr_back_send_mouse_event(TerminalBackend *term, int row, int col,
                                      int button, bool pressed, int mod)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return;
    CfrMods mods = 0;
    if (mod & TERM_MOD_SHIFT)
        mods |= CFR_MOD_SHIFT;
    if (mod & TERM_MOD_ALT)
        mods |= CFR_MOD_ALT;
    if (mod & TERM_MOD_CTRL)
        mods |= CFR_MOD_CTRL;
    CfrMouseButton b = CFR_MOUSE_NONE;
    switch (button) {
    case 1:
        b = CFR_MOUSE_LEFT;
        break;
    case 2:
        b = CFR_MOUSE_MIDDLE;
        break;
    case 3:
        b = CFR_MOUSE_RIGHT;
        break;
    case 4:
        b = CFR_MOUSE_WHEEL_UP;
        break;
    case 5:
        b = CFR_MOUSE_WHEEL_DOWN;
        break;
    default:
        break;
    }
    cfr_send_mouse(d->vt, row, col, b, pressed, mods);
}
static void cfr_back_set_output_callback(TerminalBackend *term,
                                         TerminalOutputCallback cb,
                                         void *user)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return;
    d->output_cb = cb;
    d->output_user = user;
}

static CfrKey map_key(int key)
{
    switch (key) {
    case TERM_KEY_ENTER:
        return CFR_KEY_ENTER;
    case TERM_KEY_TAB:
        return CFR_KEY_TAB;
    case TERM_KEY_BACKSPACE:
        return CFR_KEY_BACKSPACE;
    case TERM_KEY_ESCAPE:
        return CFR_KEY_ESCAPE;
    case TERM_KEY_UP:
        return CFR_KEY_UP;
    case TERM_KEY_DOWN:
        return CFR_KEY_DOWN;
    case TERM_KEY_LEFT:
        return CFR_KEY_LEFT;
    case TERM_KEY_RIGHT:
        return CFR_KEY_RIGHT;
    case TERM_KEY_INS:
        return CFR_KEY_INS;
    case TERM_KEY_DEL:
        return CFR_KEY_DEL;
    case TERM_KEY_HOME:
        return CFR_KEY_HOME;
    case TERM_KEY_END:
        return CFR_KEY_END;
    case TERM_KEY_PAGEUP:
        return CFR_KEY_PAGEUP;
    case TERM_KEY_PAGEDOWN:
        return CFR_KEY_PAGEDOWN;
    case TERM_KEY_F1:
        return CFR_KEY_F1;
    case TERM_KEY_F2:
        return CFR_KEY_F2;
    case TERM_KEY_F3:
        return CFR_KEY_F3;
    case TERM_KEY_F4:
        return CFR_KEY_F4;
    case TERM_KEY_F5:
        return CFR_KEY_F5;
    case TERM_KEY_F6:
        return CFR_KEY_F6;
    case TERM_KEY_F7:
        return CFR_KEY_F7;
    case TERM_KEY_F8:
        return CFR_KEY_F8;
    case TERM_KEY_F9:
        return CFR_KEY_F9;
    case TERM_KEY_F10:
        return CFR_KEY_F10;
    case TERM_KEY_F11:
        return CFR_KEY_F11;
    case TERM_KEY_F12:
        return CFR_KEY_F12;
    default:
        return CFR_KEY_NONE;
    }
}

static void cfr_back_send_key(TerminalBackend *term, int key, int mod)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return;
    CfrMods mods = 0;
    if (mod & TERM_MOD_SHIFT)
        mods |= CFR_MOD_SHIFT;
    if (mod & TERM_MOD_ALT)
        mods |= CFR_MOD_ALT;
    if (mod & TERM_MOD_CTRL)
        mods |= CFR_MOD_CTRL;
    cfr_send_key(d->vt, map_key(key), mods);
}

static void cfr_back_send_char(TerminalBackend *term, uint32_t cp, int mod)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return;
    CfrMods mods = 0;
    if (mod & TERM_MOD_SHIFT)
        mods |= CFR_MOD_SHIFT;
    if (mod & TERM_MOD_ALT)
        mods |= CFR_MOD_ALT;
    if (mod & TERM_MOD_CTRL)
        mods |= CFR_MOD_CTRL;

    /* Encode the codepoint as UTF-8 and send as text. */
    char buf[4];
    int n = 0;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    cfr_send_text(d->vt, buf, n, mods);
}

static void cfr_back_start_paste(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    if (d)
        cfr_paste_begin(d->vt);
}
static void cfr_back_end_paste(TerminalBackend *term)
{
    CfrBackendData *d = term->backend_data;
    if (d)
        cfr_paste_end(d->vt);
}
static bool cfr_back_get_line_continuation(TerminalBackend *term, int row)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return false;
    /* Unified coordinate space: visible rows are >= 0, scrollback rows are
     * negative (-1 = most recent). The selection layer (`term.c`) wants
     * libvterm semantics: "row N is a continuation of N-1" — true when
     * the previous logical row ended in a soft wrap.
     *
     * bvt's WRAPLINE flag sits on the row that *wrapped into* the next,
     * the inverse direction. So is_continuation(row) = wrapline(row - 1).
     * We have to walk across the visible/scrollback boundary too. */
    int prev = row - 1;
    if (prev >= 0) {
        return cfr_get_line_continuation(d->vt, prev);
    }
    int sb_row = -(prev + 1);
    return cfr_get_scrollback_wrapline(d->vt, sb_row);
}

static void cfr_back_set_scrollback_size(TerminalBackend *term, int lines)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return;
    cfr_set_scrollback_size(d->vt, lines);
}

static bool cfr_back_get_mode(TerminalBackend *term, CfrMode mode)
{
    CfrBackendData *d = term->backend_data;
    if (!d)
        return false;
    return cfr_get_mode(d->vt, mode);
}

/* ------------------------------------------------------------------ */
/* vtable                                                              */
/* ------------------------------------------------------------------ */

TerminalBackend terminal_backend_cfr = {
    .name = "coffer",
    .backend_data = NULL,
    .init = cfr_back_init,
    .destroy = cfr_back_destroy,
    .resize = cfr_back_resize,
    .process_input = cfr_back_process_input,
    .get_cell = cfr_back_get_cell,
    .get_dimensions = cfr_back_get_dimensions,
    .get_cursor_pos = cfr_back_get_cursor_pos,
    .get_cursor_visible = cfr_back_get_cursor_visible,
    .get_cursor_blink = cfr_back_get_cursor_blink,
    .get_title = cfr_back_get_title,
    .needs_redraw = cfr_back_needs_redraw,
    .clear_redraw = cfr_back_clear_redraw,
    .flush_damage = cfr_back_flush_damage,
    .mark_dirty = cfr_back_mark_dirty,
    .get_damage_rect = cfr_back_get_damage_rect,
    .get_scrollback_lines = cfr_back_get_scrollback_lines,
    .get_scrollback_capacity = cfr_back_get_scrollback_capacity,
    .consume_pushed_rows = cfr_back_consume_pushed_rows,
    .consume_popped_rows = cfr_back_consume_popped_rows,
    .consume_selection_damaged = cfr_back_consume_selection_damaged,
    .get_scrollback_cell = cfr_back_get_scrollback_cell,
    .get_grapheme = cfr_back_get_grapheme,
    .get_hyperlink = cfr_back_get_hyperlink,
    .is_altscreen = cfr_back_is_altscreen,
    .get_mouse_mode = cfr_back_get_mouse_mode,
    .send_mouse_event = cfr_back_send_mouse_event,
    .set_output_callback = cfr_back_set_output_callback,
    .send_key = cfr_back_send_key,
    .send_char = cfr_back_send_char,
    .start_paste = cfr_back_start_paste,
    .end_paste = cfr_back_end_paste,
    .get_line_continuation = cfr_back_get_line_continuation,
    .set_scrollback_size = cfr_back_set_scrollback_size,
    .get_images = cfr_back_get_images,
    .get_image_placements = cfr_back_get_image_placements,
    .set_cell_px = cfr_back_set_cell_px,
    .set_content_scale = cfr_back_set_content_scale,
    .get_lotties = cfr_back_get_lotties,
    .get_lottie_placements = cfr_back_get_lottie_placements,
    .lottie_tick = cfr_back_lottie_tick,
    .lottie_count = cfr_back_lottie_count,
    .get_mode = cfr_back_get_mode,
};

TerminalBackend *term_cfr_new(const CfrConfig *cfg)
{
    TerminalBackend *t = malloc(sizeof(*t));
    if (!t)
        return NULL;
    /* Copy the vtable/template, then clear all per-instance state so this is a
     * clean terminal independent of the shared global (whose backend_data,
     * selection, callbacks, etc. belong to the host terminal once it is up). */
    *t = terminal_backend_cfr;
    t->backend_data = NULL;
    memset(&t->selection, 0, sizeof(t->selection));
    t->selection_change_cb = NULL;
    t->selection_change_data = NULL;
    t->clipboard_set_cb = NULL;
    t->clipboard_set_data = NULL;
    t->hovered_hyperlink_id = 0;
    if (!terminal_init(t, cfg)) {
        free(t);
        return NULL;
    }
    return t;
}

CfrTerm *term_cfr_get_cfr_term(TerminalBackend *term)
{
    if (!term || !term->backend_data)
        return NULL;
    CfrBackendData *d = term->backend_data;
    return d->vt;
}
