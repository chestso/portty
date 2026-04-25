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

void bvt_emit_bytes(BvtTerm *vt, const uint8_t *bytes, size_t len) {
    if (!vt || !bytes || len == 0) return;
    if (vt->callbacks.output)
        vt->callbacks.output(bytes, len, vt->callback_user);
}

static void emit_str(BvtTerm *vt, const char *s) {
    bvt_emit_bytes(vt, (const uint8_t *)s, strlen(s));
}

static int mod_value(BvtMods m) {
    int v = 0;
    if (m & BVT_MOD_SHIFT) v |= 1;
    if (m & BVT_MOD_ALT)   v |= 2;
    if (m & BVT_MOD_CTRL)  v |= 4;
    return v;
}

/* Arrow / cursor keys. Honors DECCKM when no modifiers. */
static void emit_cursor_key(BvtTerm *vt, char letter, BvtMods mods) {
    int mv = mod_value(mods);
    char buf[16];
    int n;
    if (mv == 0) {
        if (vt->decckm) n = snprintf(buf, sizeof(buf), "\x1bO%c", letter);
        else            n = snprintf(buf, sizeof(buf), "\x1b[%c", letter);
    } else {
        n = snprintf(buf, sizeof(buf), "\x1b[1;%d%c", mv + 1, letter);
    }
    bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

/* Tilde-form keys: PageUp/Down, Home/End (alternative form), F5+, etc. */
static void emit_tilde_key(BvtTerm *vt, int code, BvtMods mods) {
    int mv = mod_value(mods);
    char buf[16];
    int n;
    if (mv == 0) n = snprintf(buf, sizeof(buf), "\x1b[%d~", code);
    else         n = snprintf(buf, sizeof(buf), "\x1b[%d;%d~", code, mv + 1);
    bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

/* SS3-form function keys (F1-F4 and PF1-PF4). */
static void emit_ss3_key(BvtTerm *vt, char letter, BvtMods mods) {
    int mv = mod_value(mods);
    char buf[16];
    int n;
    if (mv == 0) n = snprintf(buf, sizeof(buf), "\x1bO%c", letter);
    else         n = snprintf(buf, sizeof(buf), "\x1b[1;%dP", mv + 1); /* xterm style */
    (void)letter; /* For simplicity we route bare to SS3 P/Q/R/S directly. */
    bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

void bvt_send_key(BvtTerm *vt, BvtKey key, BvtMods mods) {
    if (!vt) return;
    switch (key) {
        case BVT_KEY_NONE: return;
        case BVT_KEY_ENTER:     bvt_emit_bytes(vt, (const uint8_t*)"\r", 1); break;
        case BVT_KEY_TAB:
            if (mods & BVT_MOD_SHIFT) emit_str(vt, "\x1b[Z");
            else bvt_emit_bytes(vt, (const uint8_t*)"\t", 1);
            break;
        case BVT_KEY_BACKSPACE: bvt_emit_bytes(vt, (const uint8_t*)"\x7f", 1); break;
        case BVT_KEY_ESCAPE:    bvt_emit_bytes(vt, (const uint8_t*)"\x1b", 1); break;
        case BVT_KEY_UP:    emit_cursor_key(vt, 'A', mods); break;
        case BVT_KEY_DOWN:  emit_cursor_key(vt, 'B', mods); break;
        case BVT_KEY_RIGHT: emit_cursor_key(vt, 'C', mods); break;
        case BVT_KEY_LEFT:  emit_cursor_key(vt, 'D', mods); break;
        case BVT_KEY_HOME:  emit_cursor_key(vt, 'H', mods); break;
        case BVT_KEY_END:   emit_cursor_key(vt, 'F', mods); break;
        case BVT_KEY_INS:       emit_tilde_key(vt, 2, mods);  break;
        case BVT_KEY_DEL:       emit_tilde_key(vt, 3, mods);  break;
        case BVT_KEY_PAGEUP:    emit_tilde_key(vt, 5, mods);  break;
        case BVT_KEY_PAGEDOWN:  emit_tilde_key(vt, 6, mods);  break;
        case BVT_KEY_F1: emit_ss3_key(vt, 'P', mods); break;
        case BVT_KEY_F2: emit_ss3_key(vt, 'Q', mods); break;
        case BVT_KEY_F3: emit_ss3_key(vt, 'R', mods); break;
        case BVT_KEY_F4: emit_ss3_key(vt, 'S', mods); break;
        case BVT_KEY_F5:  emit_tilde_key(vt, 15, mods); break;
        case BVT_KEY_F6:  emit_tilde_key(vt, 17, mods); break;
        case BVT_KEY_F7:  emit_tilde_key(vt, 18, mods); break;
        case BVT_KEY_F8:  emit_tilde_key(vt, 19, mods); break;
        case BVT_KEY_F9:  emit_tilde_key(vt, 20, mods); break;
        case BVT_KEY_F10: emit_tilde_key(vt, 21, mods); break;
        case BVT_KEY_F11: emit_tilde_key(vt, 23, mods); break;
        case BVT_KEY_F12: emit_tilde_key(vt, 24, mods); break;
        /* Keypad: not yet differentiated from text. */
        default: break;
    }
}

void bvt_send_text(BvtTerm *vt, const char *utf8, size_t len, BvtMods mods) {
    if (!vt || !utf8 || len == 0) return;
    /* Alt-prefix: emit ESC, then the bytes. Standard xterm meta-sends-esc. */
    if (mods & BVT_MOD_ALT)
        bvt_emit_bytes(vt, (const uint8_t *)"\x1b", 1);
    bvt_emit_bytes(vt, (const uint8_t *)utf8, len);
}

/* ------------------------------------------------------------------ */
/* Mouse                                                               */
/* ------------------------------------------------------------------ */

static int mouse_button_code(BvtMouseButton b, bool pressed, BvtMods mods,
                             bool motion) {
    int code;
    switch (b) {
        case BVT_MOUSE_LEFT:       code = 0; break;
        case BVT_MOUSE_MIDDLE:     code = 1; break;
        case BVT_MOUSE_RIGHT:      code = 2; break;
        case BVT_MOUSE_WHEEL_UP:   code = 64; break;
        case BVT_MOUSE_WHEEL_DOWN: code = 65; break;
        default:                    code = 3; break; /* release for X10 */
    }
    if (motion) code |= 32;
    if (mods & BVT_MOD_SHIFT) code |= 4;
    if (mods & BVT_MOD_ALT)   code |= 8;
    if (mods & BVT_MOD_CTRL)  code |= 16;
    /* In X10 mode, button release uses code 3. SGR mode keeps the
     * button code and signals release with `m` instead of `M`. */
    (void)pressed;
    return code;
}

void bvt_send_mouse(BvtTerm *vt, int row, int col, BvtMouseButton b,
                    bool pressed, BvtMods mods) {
    if (!vt) return;

    bool any_mode =
        vt->modes[BVT_MODE_MOUSE_BTN_EVENT] ||
        vt->modes[BVT_MODE_MOUSE_DRAG] ||
        vt->modes[BVT_MODE_MOUSE_ANY_EVENT] ||
        vt->modes[BVT_MODE_MOUSE_X10];
    if (!any_mode) return;

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
        if (cx > 223) cx = 223;
        if (cy > 223) cy = 223;
        int byte_code = (pressed ? code : 3) + 32;
        if (byte_code > 255) byte_code = 255;
        n = snprintf(buf, sizeof(buf), "\x1b[M%c%c%c",
                     (char)byte_code, (char)(cx + 32), (char)(cy + 32));
    }
    bvt_emit_bytes(vt, (const uint8_t *)buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Bracketed paste                                                     */
/* ------------------------------------------------------------------ */

void bvt_paste_begin(BvtTerm *vt) {
    if (!vt) return;
    if (!vt->modes[BVT_MODE_BRACKETED_PASTE]) return;
    emit_str(vt, "\x1b[200~");
}
void bvt_paste_end(BvtTerm *vt) {
    if (!vt) return;
    if (!vt->modes[BVT_MODE_BRACKETED_PASTE]) return;
    emit_str(vt, "\x1b[201~");
}
