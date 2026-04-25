/*
 * bloom-vt — keyboard, text, mouse, and paste emit.
 *
 * All emit paths run through bvt_emit_bytes which forwards to the
 * registered output callback (the embedder writes those bytes to the
 * PTY).
 *
 * Modifier encoding follows xterm's convention:
 *   mod_value = 1 + shift + (alt<<1) + (ctrl<<2)
 * Arrow keys with modifiers emit "ESC [ 1 ; <mod> <X>", tilde keys
 * emit "ESC [ <code> ; <mod> ~". Bare keys honor DECCKM for arrows
 * (ESC O <X> when set, ESC [ <X> otherwise).
 *
 * Mouse encoding emits SGR (1006) form when that mode is active,
 * X10/normal (1000) otherwise. Wheel events use button codes 64/65
 * per xterm.
 */

#include "bloom_vt_internal.h"

#include <stdio.h>
#include <string.h>

void bvt_emit_bytes(BvtTerm *vt, const uint8_t *bytes, size_t len)
{
    if (!vt || !bytes || len == 0)
        return;
    if (vt->callbacks.output)
        vt->callbacks.output(bytes, len, vt->callback_user);
}

static void emit_str(BvtTerm *vt, const char *s)
{
    bvt_emit_bytes(vt, (const uint8_t *)s, strlen(s));
}

static int mod_value(BvtMods m)
{
    int v = 0;
    if (m & BVT_MOD_SHIFT)
        v |= 1;
    if (m & BVT_MOD_ALT)
        v |= 2;
    if (m & BVT_MOD_CTRL)
        v |= 4;
    return v;
}

/* Kitty keyboard protocol flag bits we honour. */
#define KITTY_FLAG_DISAMBIGUATE 0x1u
#define KITTY_FLAG_REPORT_ALL   0x8u

static bool kitty_active(BvtTerm *vt, uint32_t bit)
{
    return (vt->kitty_kb_stack[vt->kitty_kb_depth] & bit) != 0;
}

/* Emit a kitty `CSI <code>;<mod>u` sequence. The mod parameter is
 * omitted when no modifier is held (`CSI <code>u`), matching the
 * kitty spec. */
static void emit_csi_u(BvtTerm *vt, uint32_t code, BvtMods mods)
{
    int mv = mod_value(mods);
    char buf[32];
    int n;
    if (mv == 0)
        n = snprintf(buf, sizeof(buf), "\x1b[%uu", code);
    else
        n = snprintf(buf, sizeof(buf), "\x1b[%u;%du", code, mv + 1);
    bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

/* Decide whether ENTER / TAB / BACKSPACE / ESCAPE should be emitted in
 * kitty CSI-u form. Disambiguate (0x1) only kicks in when modifiers
 * are present (its purpose is to distinguish e.g. Shift+Enter from
 * Enter); Report-all (0x8) sends CSI-u even for the bare key. */
static bool kitty_route_special(BvtTerm *vt, BvtMods mods)
{
    if (kitty_active(vt, KITTY_FLAG_REPORT_ALL))
        return true;
    if (kitty_active(vt, KITTY_FLAG_DISAMBIGUATE) && mod_value(mods) != 0)
        return true;
    return false;
}

/* Arrow / cursor keys. Honors DECCKM when no modifiers. */
static void emit_cursor_key(BvtTerm *vt, char letter, BvtMods mods)
{
    int mv = mod_value(mods);
    char buf[16];
    int n;
    if (mv == 0) {
        if (vt->decckm)
            n = snprintf(buf, sizeof(buf), "\x1bO%c", letter);
        else
            n = snprintf(buf, sizeof(buf), "\x1b[%c", letter);
    } else {
        n = snprintf(buf, sizeof(buf), "\x1b[1;%d%c", mv + 1, letter);
    }
    bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

/* Tilde-form keys: PageUp/Down, Home/End (alternative form), F5+, etc. */
static void emit_tilde_key(BvtTerm *vt, int code, BvtMods mods)
{
    int mv = mod_value(mods);
    char buf[16];
    int n;
    if (mv == 0)
        n = snprintf(buf, sizeof(buf), "\x1b[%d~", code);
    else
        n = snprintf(buf, sizeof(buf), "\x1b[%d;%d~", code, mv + 1);
    bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

/* SS3-form function keys (F1-F4 and PF1-PF4). */
static void emit_ss3_key(BvtTerm *vt, char letter, BvtMods mods)
{
    int mv = mod_value(mods);
    char buf[16];
    int n;
    if (mv == 0)
        n = snprintf(buf, sizeof(buf), "\x1bO%c", letter);
    else
        n = snprintf(buf, sizeof(buf), "\x1b[1;%dP", mv + 1); /* xterm style */
    (void)letter;                                             /* For simplicity we route bare to SS3 P/Q/R/S directly. */
    bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

void bvt_send_key(BvtTerm *vt, BvtKey key, BvtMods mods)
{
    if (!vt)
        return;
    switch (key) {
    case BVT_KEY_NONE:
        return;
    case BVT_KEY_ENTER:
        if (kitty_route_special(vt, mods))
            emit_csi_u(vt, 13, mods);
        else
            bvt_emit_bytes(vt, (const uint8_t *)"\r", 1);
        break;
    case BVT_KEY_TAB:
        if (kitty_route_special(vt, mods))
            emit_csi_u(vt, 9, mods);
        else if (mods & BVT_MOD_SHIFT)
            emit_str(vt, "\x1b[Z");
        else
            bvt_emit_bytes(vt, (const uint8_t *)"\t", 1);
        break;
    case BVT_KEY_BACKSPACE:
        if (kitty_route_special(vt, mods))
            emit_csi_u(vt, 127, mods);
        else
            bvt_emit_bytes(vt, (const uint8_t *)"\x7f", 1);
        break;
    case BVT_KEY_ESCAPE:
        if (kitty_route_special(vt, mods))
            emit_csi_u(vt, 27, mods);
        else
            bvt_emit_bytes(vt, (const uint8_t *)"\x1b", 1);
        break;
    case BVT_KEY_UP:
        emit_cursor_key(vt, 'A', mods);
        break;
    case BVT_KEY_DOWN:
        emit_cursor_key(vt, 'B', mods);
        break;
    case BVT_KEY_RIGHT:
        emit_cursor_key(vt, 'C', mods);
        break;
    case BVT_KEY_LEFT:
        emit_cursor_key(vt, 'D', mods);
        break;
    case BVT_KEY_HOME:
        emit_cursor_key(vt, 'H', mods);
        break;
    case BVT_KEY_END:
        emit_cursor_key(vt, 'F', mods);
        break;
    case BVT_KEY_INS:
        emit_tilde_key(vt, 2, mods);
        break;
    case BVT_KEY_DEL:
        emit_tilde_key(vt, 3, mods);
        break;
    case BVT_KEY_PAGEUP:
        emit_tilde_key(vt, 5, mods);
        break;
    case BVT_KEY_PAGEDOWN:
        emit_tilde_key(vt, 6, mods);
        break;
    case BVT_KEY_F1:
        emit_ss3_key(vt, 'P', mods);
        break;
    case BVT_KEY_F2:
        emit_ss3_key(vt, 'Q', mods);
        break;
    case BVT_KEY_F3:
        emit_ss3_key(vt, 'R', mods);
        break;
    case BVT_KEY_F4:
        emit_ss3_key(vt, 'S', mods);
        break;
    case BVT_KEY_F5:
        emit_tilde_key(vt, 15, mods);
        break;
    case BVT_KEY_F6:
        emit_tilde_key(vt, 17, mods);
        break;
    case BVT_KEY_F7:
        emit_tilde_key(vt, 18, mods);
        break;
    case BVT_KEY_F8:
        emit_tilde_key(vt, 19, mods);
        break;
    case BVT_KEY_F9:
        emit_tilde_key(vt, 20, mods);
        break;
    case BVT_KEY_F10:
        emit_tilde_key(vt, 21, mods);
        break;
    case BVT_KEY_F11:
        emit_tilde_key(vt, 23, mods);
        break;
    case BVT_KEY_F12:
        emit_tilde_key(vt, 24, mods);
        break;
    /* Keypad: not yet differentiated from text. */
    default:
        break;
    }
}

void bvt_send_text(BvtTerm *vt, const char *utf8, size_t len, BvtMods mods)
{
    if (!vt || !utf8 || len == 0)
        return;

    /* Kitty Disambiguate / Report-all: when active, route Ctrl+ASCII (and
     * Alt+ASCII) through CSI-u so the application can tell e.g. Ctrl+A
     * apart from a literal 0x01 byte and recover the Shift modifier. The
     * codepoint is lowercased per the kitty spec — Shift travels in the
     * modifier bitmask, not in the codepoint. Take this branch before
     * emitting the legacy meta-sends-esc prefix so Alt is encoded in the
     * mod parameter, not as a stray ESC. */
    if (len == 1 && (mods & (BVT_MOD_CTRL | BVT_MOD_ALT)) &&
        (kitty_active(vt, KITTY_FLAG_DISAMBIGUATE) ||
         kitty_active(vt, KITTY_FLAG_REPORT_ALL))) {
        uint8_t b = (uint8_t)utf8[0];
        uint32_t cp = (b >= 'A' && b <= 'Z') ? (uint32_t)(b + 0x20)
                                             : (uint32_t)b;
        emit_csi_u(vt, cp, mods);
        return;
    }

    /* Alt-prefix: emit ESC, then the bytes. Standard xterm meta-sends-esc. */
    if (mods & BVT_MOD_ALT)
        bvt_emit_bytes(vt, (const uint8_t *)"\x1b", 1);

    /* Ctrl+<key>: transform single-byte ASCII to its control code so the
     * PTY's line discipline produces SIGINT for Ctrl+C, NUL for Ctrl+Space
     * etc. Multi-byte input is passed through unchanged. */
    if ((mods & BVT_MOD_CTRL) && len == 1) {
        uint8_t b = (uint8_t)utf8[0];
        uint8_t out = b;
        if (b == ' ' || b == '@')
            out = 0x00;
        else if (b == '?')
            out = 0x7F;
        else if (b >= 'A' && b <= 'Z')
            out = (uint8_t)(b - 0x40);
        else if (b >= 'a' && b <= 'z')
            out = (uint8_t)(b - 0x60);
        else if (b >= '[' && b <= '_')
            out = (uint8_t)(b - 0x40);
        bvt_emit_bytes(vt, &out, 1);
        return;
    }

    bvt_emit_bytes(vt, (const uint8_t *)utf8, len);
}

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

static int mouse_button_code(BvtMouseButton b, bool pressed, BvtMods mods,
                             bool motion)
{
    int code;
    switch (b) {
    case BVT_MOUSE_LEFT:
        code = 0;
        break;
    case BVT_MOUSE_MIDDLE:
        code = 1;
        break;
    case BVT_MOUSE_RIGHT:
        code = 2;
        break;
    case BVT_MOUSE_WHEEL_UP:
        code = 64;
        break;
    case BVT_MOUSE_WHEEL_DOWN:
        code = 65;
        break;
    default:
        code = 3;
        break; /* release for X10 */
    }
    if (motion)
        code |= 32;
    if (mods & BVT_MOD_SHIFT)
        code |= 4;
    if (mods & BVT_MOD_ALT)
        code |= 8;
    if (mods & BVT_MOD_CTRL)
        code |= 16;
    /* In X10 mode, button release uses code 3. SGR mode keeps the
     * button code and signals release with `m` instead of `M`. */
    (void)pressed;
    return code;
}

void bvt_send_mouse(BvtTerm *vt, int row, int col, BvtMouseButton b,
                    bool pressed, BvtMods mods)
{
    if (!vt)
        return;

    bool any_mode =
        vt->modes[BVT_MODE_MOUSE_BTN_EVENT] ||
        vt->modes[BVT_MODE_MOUSE_DRAG] ||
        vt->modes[BVT_MODE_MOUSE_ANY_EVENT] ||
        vt->modes[BVT_MODE_MOUSE_X10];
    if (!any_mode)
        return;

    bool sgr = vt->modes[BVT_MODE_MOUSE_SGR];
    bool motion = (b == BVT_MOUSE_NONE);
    int code = mouse_button_code(b, pressed, mods, motion);

    char buf[32];
    int n;
    if (sgr) {
        n = snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c",
                     code, col + 1, row + 1, pressed ? 'M' : 'm');
    } else {
        /* X10: ESC [ M Cb Cx Cy with values offset by 32. Coordinates
         * cap at 223. */
        int cx = col + 1, cy = row + 1;
        if (cx > 223)
            cx = 223;
        if (cy > 223)
            cy = 223;
        int byte_code = (pressed ? code : 3) + 32;
        if (byte_code > 255)
            byte_code = 255;
        n = snprintf(buf, sizeof(buf), "\x1b[M%c%c%c",
                     (char)byte_code, (char)(cx + 32), (char)(cy + 32));
    }
    bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Bracketed paste                                                     */
/* ------------------------------------------------------------------ */

void bvt_paste_begin(BvtTerm *vt)
{
    if (!vt)
        return;
    if (!vt->modes[BVT_MODE_BRACKETED_PASTE])
        return;
    emit_str(vt, "\x1b[200~");
}
void bvt_paste_end(BvtTerm *vt)
{
    if (!vt)
        return;
    if (!vt->modes[BVT_MODE_BRACKETED_PASTE])
        return;
    emit_str(vt, "\x1b[201~");
}
