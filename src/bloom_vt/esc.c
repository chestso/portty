/*
 * bloom-vt — ESC dispatcher (non-CSI/non-OSC/non-DCS escapes).
 *
 * Handles ESC X commands like keypad mode, save/restore cursor,
 * charset designations (G0/G1/G2/G3), reverse index, and full reset.
 */

#include "bloom_vt_internal.h"

#include <string.h>

void bvt_esc_dispatch(BvtTerm *vt, uint8_t final)
{
    bvt_flush_cluster(vt);
    BvtParser *p = &vt->parser;

    /* Charset designation: ESC ( c | ) c | * c | + c — single-char
     * intermediate captured during ESCAPE_INTERMEDIATE. We track only
     * the designation here; the GL translation runs at print time. */
    if (p->intermediate_count == 1) {
        uint8_t inter = p->intermediates[0];
        if (inter == '(' || inter == ')' || inter == '*' || inter == '+') {
            int slot = (inter == '(') ? 0 : (inter == ')') ? 1
                                        : (inter == '*')   ? 2
                                                           : 3;
            vt->charset[slot] = final;
            return;
        }
    }

    switch (final) {
    case '7':
        vt->saved_cursor = vt->cursor;
        break; /* DECSC */
    case '8':  /* DECRC */
        vt->cursor = vt->saved_cursor;
        break;
    case 'n':
        vt->charset_active = 2;
        break; /* LS2 */
    case 'o':
        vt->charset_active = 3;
        break; /* LS3 */
    case '=':
        vt->deckpam = true;
        break; /* DECKPAM */
    case '>':
        vt->deckpam = false;
        break; /* DECKPNM */
    case 'D':  /* IND */
        if (vt->cursor.row == vt->scroll_bottom)
            bvt_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
        break;
    case 'E': /* NEL */
        if (vt->cursor.row == vt->scroll_bottom)
            bvt_scroll_up(vt, 1);
        else if (vt->cursor.row < vt->rows - 1)
            vt->cursor.row++;
        vt->cursor.col = 0;
        break;
    case 'M': /* RI */
        if (vt->cursor.row == vt->scroll_top)
            bvt_scroll_down(vt, 1);
        else if (vt->cursor.row > 0)
            vt->cursor.row--;
        break;
    case 'c': /* RIS — full reset */
        memset(&vt->cursor, 0, sizeof(vt->cursor));
        vt->cursor.visible = true;
        vt->cursor.blink = true;
        vt->scroll_top = 0;
        vt->scroll_bottom = vt->rows - 1;
        vt->in_altscreen = false;
        if (vt->grid) {
            memset(vt->grid->cells, 0,
                   (size_t)vt->rows * vt->cols * sizeof(BvtCell));
            memset(vt->grid->row_flags, 0, (size_t)vt->rows);
        }
        bvt_damage_all(vt);
        break;
    default:
        /* Many ESC dispatches are silently ignored. */
        break;
    }
}
