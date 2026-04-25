/*
 * term_bvt.c — TerminalBackend bridge to bloom-vt.
 *
 * Mirrors term_vt.c but routes everything through bloom_vt instead of
 * libvterm. Cell conversion translates BvtCell + BvtStyle into the
 * legacy TerminalCell shape so the renderer is unchanged. Selected at
 * startup via BLOOM_TERMINAL_VT=bloomvt; libvterm remains the default
 * during the parallel-development window.
 */

#include "term_bvt.h"
#include "bloom_vt/bloom_vt.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    BvtTerm *vt;

    /* Damage tracking — accumulated rectangle since last clear_redraw.
     * bloom-vt provides its own damage callback; we union into this. */
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
} BvtBackendData;

/* ------------------------------------------------------------------ */
/* Color conversion                                                    */
/* ------------------------------------------------------------------ */

static const uint8_t default_fg[3] = { 0xff, 0xfd, 0xf5 };
static const uint8_t default_bg[3] = { 0x00, 0x00, 0x00 };

static TerminalColor unpack_rgb(uint32_t rgb, bool is_default,
                                const uint8_t fallback[3])
{
    TerminalColor out;
    if (is_default) {
        out.r = fallback[0];
        out.g = fallback[1];
        out.b = fallback[2];
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

static void damage_init(BvtBackendData *d)
{
    d->needs_redraw = false;
    d->damage_top = d->damage_bottom = d->damage_left = d->damage_right = 0;
}

static void damage_union(BvtBackendData *d, int t, int l, int b, int r)
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
/* bloom-vt callbacks                                                  */
/* ------------------------------------------------------------------ */

static void cb_damage(BvtRect rect, void *user)
{
    BvtBackendData *d = user;
    damage_union(d, rect.start_row, rect.start_col, rect.end_row, rect.end_col);
}

static void cb_moverect(BvtRect dst, BvtRect src, void *user)
{
    BvtBackendData *d = user;
    damage_union(d, dst.start_row, dst.start_col, dst.end_row, dst.end_col);
    damage_union(d, src.start_row, src.start_col, src.end_row, src.end_col);
}

static void cb_movecursor(BvtCursor cur, void *user)
{
    BvtBackendData *d = user;
    d->cursor_visible = cur.visible;
    d->cursor_blink = cur.blink;
    /* Cursor position is queried lazily via get_cursor_pos. */
}

static void cb_set_title(const char *utf8, void *user)
{
    BvtBackendData *d = user;
    if (!utf8) {
        d->title[0] = '\0';
        return;
    }
    size_t n = strlen(utf8);
    if (n >= sizeof(d->title)) {
        n = sizeof(d->title) - 1;
        /* Don't slice through a UTF-8 codepoint — GTK/Pango/Gdk abort
         * hard on mid-codepoint truncation. Walk the last `n` bytes back
         * to a leading byte and drop the codepoint if it doesn't fit. */
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

static void cb_set_mode(BvtMode mode, bool on, void *user)
{
    BvtBackendData *d = user;
    switch (mode) {
    case BVT_MODE_CURSOR_VISIBLE:
        d->cursor_visible = on;
        break;
    case BVT_MODE_CURSOR_BLINK:
        d->cursor_blink = on;
        break;
    case BVT_MODE_ALTSCREEN:
        d->altscreen = on;
        break;
    case BVT_MODE_MOUSE_X10:
        d->mouse_mode = on ? 1 : 0;
        break;
    case BVT_MODE_MOUSE_BTN_EVENT:
        d->mouse_mode = on ? 1 : 0;
        break;
    case BVT_MODE_MOUSE_DRAG:
        d->mouse_mode = on ? 2 : 0;
        break;
    case BVT_MODE_MOUSE_ANY_EVENT:
        d->mouse_mode = on ? 3 : 0;
        break;
    default:
        break;
    }
}

static void cb_output(const uint8_t *bytes, size_t len, void *user)
{
    BvtBackendData *d = user;
    if (d->output_cb)
        d->output_cb((const char *)bytes, len, d->output_user);
}

static void cb_bell(void *user) { (void)user; /* TODO: visual bell hook */ }

/* sb_pushline: bvt owns the scrollback so we're a no-op observer. */
static void cb_sb_push(const BvtCell *c, int n, bool w, void *u)
{
    (void)c;
    (void)n;
    (void)w;
    (void)u;
}
static void cb_sb_pop(BvtCell *o, int n, void *u)
{
    (void)o;
    (void)n;
    (void)u;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static bool bvt_back_init(TerminalBackend *term, int width, int height)
{
    BvtBackendData *d = calloc(1, sizeof(*d));
    if (!d)
        return false;
    d->vt = bvt_new(height, width); /* rows, cols */
    if (!d->vt) {
        free(d);
        return false;
    }
    d->cursor_visible = true;
    d->cursor_blink = true;
    /* bloom-vt reflow is the stable WRAPLINE-tagged path (reflow.c). The
     * libvterm-era "UNSTABLE" warning does not apply, so default it on. */
    bvt_set_reflow(d->vt, true);

    BvtCallbacks cb = {
        .damage = cb_damage,
        .moverect = cb_moverect,
        .movecursor = cb_movecursor,
        .bell = cb_bell,
        .set_title = cb_set_title,
        .set_mode = cb_set_mode,
        .output = cb_output,
        .sb_pushline = cb_sb_push,
        .sb_popline = cb_sb_pop,
    };
    bvt_set_callbacks(d->vt, &cb, d);

    term->backend_data = d;
    return true;
}

static void bvt_back_destroy(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    if (!d)
        return;
    bvt_free(d->vt);
    free(d);
    term->backend_data = NULL;
}

static void bvt_back_resize(TerminalBackend *term, int width, int height)
{
    BvtBackendData *d = term->backend_data;
    if (!d)
        return;
    bvt_resize(d->vt, height, width);
    damage_init(d);
    d->needs_redraw = true;
    int rows, cols;
    bvt_get_dimensions(d->vt, &rows, &cols);
    if (rows && cols)
        damage_union(d, 0, 0, rows - 1, cols - 1);
}

static int bvt_back_process_input(TerminalBackend *term, const char *input,
                                  size_t len)
{
    BvtBackendData *d = term->backend_data;
    if (!d)
        return 0;
    return (int)bvt_input_write(d->vt, (const uint8_t *)input, len);
}

/* ------------------------------------------------------------------ */
/* Cell conversion                                                     */
/* ------------------------------------------------------------------ */

static void convert_cell(BvtTerm *vt, const BvtCell *src, TerminalCell *dst)
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
     * which routes back through bvt_cell_get_grapheme — no truncation at
     * the renderer boundary. */
    dst->cp = src->cp;
    dst->grapheme_id = src->grapheme_id;

    /* Style. */
    const BvtStyle *st = bvt_cell_style(vt, src);
    if (!st) {
        dst->fg = unpack_rgb(0, true, default_fg);
        dst->bg = unpack_rgb(0, true, default_bg);
        dst->ul_color = unpack_rgb(0, true, default_fg);
        return;
    }
    dst->attrs.bold = (st->attrs & BVT_ATTR_BOLD) ? 1 : 0;
    dst->attrs.italic = (st->attrs & BVT_ATTR_ITALIC) ? 1 : 0;
    dst->attrs.blink = (st->attrs & BVT_ATTR_BLINK) ? 1 : 0;
    dst->attrs.reverse = (st->attrs & BVT_ATTR_REVERSE) ? 1 : 0;
    dst->attrs.strikethrough = (st->attrs & BVT_ATTR_STRIKETHROUGH) ? 1 : 0;
    dst->attrs.dwl = (st->attrs & BVT_ATTR_DWL) ? 1 : 0;
    if (st->attrs & BVT_ATTR_DHL_TOP)
        dst->attrs.dhl = 1;
    else if (st->attrs & BVT_ATTR_DHL_BOTTOM)
        dst->attrs.dhl = 2;
    dst->attrs.font = st->font & 0xF;
    dst->attrs.underline = st->underline & 0x7;

    dst->fg = unpack_rgb(st->fg_rgb,
                         (st->color_flags & BVT_COLOR_DEFAULT_FG) != 0,
                         default_fg);
    dst->bg = unpack_rgb(st->bg_rgb,
                         (st->color_flags & BVT_COLOR_DEFAULT_BG) != 0,
                         default_bg);
    dst->ul_color = unpack_rgb(st->ul_rgb,
                               (st->color_flags & BVT_COLOR_DEFAULT_UL) != 0,
                               default_fg);

    /* Match libvterm backend: pre-swap fg/bg for reverse video so the
     * renderer sees visual colors. */
    if (dst->attrs.reverse) {
        TerminalColor tmp = dst->fg;
        dst->fg = dst->bg;
        dst->bg = tmp;
        dst->bg.is_default = false;
    }
}

static int bvt_back_get_cell(TerminalBackend *term, int row, int col,
                             TerminalCell *cell)
{
    BvtBackendData *d = term->backend_data;
    if (!d || !cell)
        return -1;
    const BvtCell *src = bvt_get_cell(d->vt, row, col);
    convert_cell(d->vt, src, cell);
    return 0;
}

static int bvt_back_get_dimensions(TerminalBackend *term, int *rows, int *cols)
{
    BvtBackendData *d = term->backend_data;
    if (!d)
        return -1;
    bvt_get_dimensions(d->vt, rows, cols);
    return 0;
}

static TerminalPos bvt_back_get_cursor_pos(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    BvtCursor c = bvt_get_cursor(d->vt);
    return (TerminalPos){ .row = c.row, .col = c.col };
}
static bool bvt_back_get_cursor_visible(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    return bvt_get_cursor(d->vt).visible;
}
static bool bvt_back_get_cursor_blink(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    return bvt_get_cursor(d->vt).blink;
}
static const char *bvt_back_get_title(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    return d->title;
}

/* ------------------------------------------------------------------ */
/* Damage                                                              */
/* ------------------------------------------------------------------ */

static bool bvt_back_needs_redraw(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    return d ? d->needs_redraw : false;
}
static void bvt_back_clear_redraw(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    if (d)
        damage_init(d);
}
static bool bvt_back_get_damage_rect(TerminalBackend *term,
                                     TerminalDamageRect *rect)
{
    BvtBackendData *d = term->backend_data;
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

static int bvt_back_get_scrollback_lines(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    return d ? bvt_get_scrollback_lines(d->vt) : 0;
}
static int bvt_back_get_scrollback_cell(TerminalBackend *term, int sb_row,
                                        int col, TerminalCell *cell)
{
    BvtBackendData *d = term->backend_data;
    if (!d || !cell)
        return -1;
    const BvtCell *src = bvt_get_scrollback_cell(d->vt, sb_row, col);
    convert_cell(d->vt, src, cell);
    return 0;
}

static size_t bvt_back_get_grapheme(TerminalBackend *term, int unified_row,
                                    int col, uint32_t *out, size_t cap)
{
    BvtBackendData *d = term->backend_data;
    if (!d || !out || cap == 0)
        return 0;
    const BvtCell *src = (unified_row >= 0)
                             ? bvt_get_cell(d->vt, unified_row, col)
                             : bvt_get_scrollback_cell(d->vt, -(unified_row + 1), col);
    if (!src)
        return 0;
    return bvt_cell_get_grapheme(d->vt, src, out, cap);
}

/* ------------------------------------------------------------------ */
/* Modes / I/O                                                         */
/* ------------------------------------------------------------------ */

static bool bvt_back_is_altscreen(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    return d ? d->altscreen : false;
}
static int bvt_back_get_mouse_mode(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    return d ? d->mouse_mode : 0;
}
static void bvt_back_send_mouse_event(TerminalBackend *term, int row, int col,
                                      int button, bool pressed, int mod)
{
    BvtBackendData *d = term->backend_data;
    if (!d)
        return;
    BvtMods mods = 0;
    if (mod & TERM_MOD_SHIFT)
        mods |= BVT_MOD_SHIFT;
    if (mod & TERM_MOD_ALT)
        mods |= BVT_MOD_ALT;
    if (mod & TERM_MOD_CTRL)
        mods |= BVT_MOD_CTRL;
    BvtMouseButton b = BVT_MOUSE_NONE;
    switch (button) {
    case 1:
        b = BVT_MOUSE_LEFT;
        break;
    case 2:
        b = BVT_MOUSE_MIDDLE;
        break;
    case 3:
        b = BVT_MOUSE_RIGHT;
        break;
    case 4:
        b = BVT_MOUSE_WHEEL_UP;
        break;
    case 5:
        b = BVT_MOUSE_WHEEL_DOWN;
        break;
    default:
        break;
    }
    bvt_send_mouse(d->vt, row, col, b, pressed, mods);
}
static void bvt_back_set_output_callback(TerminalBackend *term,
                                         TerminalOutputCallback cb,
                                         void *user)
{
    BvtBackendData *d = term->backend_data;
    if (!d)
        return;
    d->output_cb = cb;
    d->output_user = user;
}

static BvtKey map_key(int key)
{
    switch (key) {
    case TERM_KEY_ENTER:
        return BVT_KEY_ENTER;
    case TERM_KEY_TAB:
        return BVT_KEY_TAB;
    case TERM_KEY_BACKSPACE:
        return BVT_KEY_BACKSPACE;
    case TERM_KEY_ESCAPE:
        return BVT_KEY_ESCAPE;
    case TERM_KEY_UP:
        return BVT_KEY_UP;
    case TERM_KEY_DOWN:
        return BVT_KEY_DOWN;
    case TERM_KEY_LEFT:
        return BVT_KEY_LEFT;
    case TERM_KEY_RIGHT:
        return BVT_KEY_RIGHT;
    case TERM_KEY_INS:
        return BVT_KEY_INS;
    case TERM_KEY_DEL:
        return BVT_KEY_DEL;
    case TERM_KEY_HOME:
        return BVT_KEY_HOME;
    case TERM_KEY_END:
        return BVT_KEY_END;
    case TERM_KEY_PAGEUP:
        return BVT_KEY_PAGEUP;
    case TERM_KEY_PAGEDOWN:
        return BVT_KEY_PAGEDOWN;
    case TERM_KEY_F1:
        return BVT_KEY_F1;
    case TERM_KEY_F2:
        return BVT_KEY_F2;
    case TERM_KEY_F3:
        return BVT_KEY_F3;
    case TERM_KEY_F4:
        return BVT_KEY_F4;
    case TERM_KEY_F5:
        return BVT_KEY_F5;
    case TERM_KEY_F6:
        return BVT_KEY_F6;
    case TERM_KEY_F7:
        return BVT_KEY_F7;
    case TERM_KEY_F8:
        return BVT_KEY_F8;
    case TERM_KEY_F9:
        return BVT_KEY_F9;
    case TERM_KEY_F10:
        return BVT_KEY_F10;
    case TERM_KEY_F11:
        return BVT_KEY_F11;
    case TERM_KEY_F12:
        return BVT_KEY_F12;
    default:
        return BVT_KEY_NONE;
    }
}

static void bvt_back_send_key(TerminalBackend *term, int key, int mod)
{
    BvtBackendData *d = term->backend_data;
    if (!d)
        return;
    BvtMods mods = 0;
    if (mod & TERM_MOD_SHIFT)
        mods |= BVT_MOD_SHIFT;
    if (mod & TERM_MOD_ALT)
        mods |= BVT_MOD_ALT;
    if (mod & TERM_MOD_CTRL)
        mods |= BVT_MOD_CTRL;
    bvt_send_key(d->vt, map_key(key), mods);
}

static void bvt_back_send_char(TerminalBackend *term, uint32_t cp, int mod)
{
    BvtBackendData *d = term->backend_data;
    if (!d)
        return;
    BvtMods mods = 0;
    if (mod & TERM_MOD_SHIFT)
        mods |= BVT_MOD_SHIFT;
    if (mod & TERM_MOD_ALT)
        mods |= BVT_MOD_ALT;
    if (mod & TERM_MOD_CTRL)
        mods |= BVT_MOD_CTRL;

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
    bvt_send_text(d->vt, buf, n, mods);
}

static void bvt_back_start_paste(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    if (d)
        bvt_paste_begin(d->vt);
}
static void bvt_back_end_paste(TerminalBackend *term)
{
    BvtBackendData *d = term->backend_data;
    if (d)
        bvt_paste_end(d->vt);
}
static void bvt_back_set_reflow(TerminalBackend *term, bool enabled)
{
    BvtBackendData *d = term->backend_data;
    if (d)
        bvt_set_reflow(d->vt, enabled);
}
static bool bvt_back_get_line_continuation(TerminalBackend *term, int row)
{
    BvtBackendData *d = term->backend_data;
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
        return bvt_get_line_continuation(d->vt, prev);
    }
    int sb_row = -(prev + 1);
    return bvt_get_scrollback_wrapline(d->vt, sb_row);
}

/* ------------------------------------------------------------------ */
/* vtable                                                              */
/* ------------------------------------------------------------------ */

TerminalBackend terminal_backend_bvt = {
    .name = "bloom-vt",
    .backend_data = NULL,
    .init = bvt_back_init,
    .destroy = bvt_back_destroy,
    .resize = bvt_back_resize,
    .process_input = bvt_back_process_input,
    .get_cell = bvt_back_get_cell,
    .get_dimensions = bvt_back_get_dimensions,
    .get_cursor_pos = bvt_back_get_cursor_pos,
    .get_cursor_visible = bvt_back_get_cursor_visible,
    .get_cursor_blink = bvt_back_get_cursor_blink,
    .get_title = bvt_back_get_title,
    .needs_redraw = bvt_back_needs_redraw,
    .clear_redraw = bvt_back_clear_redraw,
    .get_damage_rect = bvt_back_get_damage_rect,
    .get_scrollback_lines = bvt_back_get_scrollback_lines,
    .get_scrollback_cell = bvt_back_get_scrollback_cell,
    .get_grapheme = bvt_back_get_grapheme,
    .is_altscreen = bvt_back_is_altscreen,
    .get_mouse_mode = bvt_back_get_mouse_mode,
    .send_mouse_event = bvt_back_send_mouse_event,
    .set_output_callback = bvt_back_set_output_callback,
    .send_key = bvt_back_send_key,
    .send_char = bvt_back_send_char,
    .start_paste = bvt_back_start_paste,
    .end_paste = bvt_back_end_paste,
    .set_reflow = bvt_back_set_reflow,
    .get_line_continuation = bvt_back_get_line_continuation,
};
