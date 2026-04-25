/*
 * bloom-vt — OSC dispatcher.
 *
 * Parses the leading numeric code and routes title-setting (0/1/2) to
 * the set_title callback, hands everything else to the generic osc
 * callback. OSC 8 (hyperlink) parsing arrives in a follow-up.
 */

#include "bloom_vt_internal.h"

#include <stdlib.h>
#include <string.h>

static int parse_code(const uint8_t *data, size_t len, size_t *out_offset)
{
    size_t i = 0;
    int code = -1;
    while (i < len && data[i] >= '0' && data[i] <= '9') {
        if (code < 0)
            code = 0;
        code = code * 10 + (data[i] - '0');
        i++;
    }
    if (i < len && data[i] == ';')
        i++;
    *out_offset = i;
    return code;
}

static void set_title(BvtTerm *vt, const uint8_t *data, size_t len)
{
    bvt_dealloc(vt, vt->title);
    vt->title = bvt_alloc(vt, len + 1);
    if (!vt->title)
        return;
    memcpy(vt->title, data, len);
    vt->title[len] = '\0';
    if (vt->callbacks.set_title)
        vt->callbacks.set_title(vt->title, vt->callback_user);
}

void bvt_osc_dispatch(BvtTerm *vt, const uint8_t *data, size_t len)
{
    size_t off = 0;
    int code = parse_code(data, len, &off);

    const uint8_t *body = data + off;
    size_t body_len = (off > len) ? 0 : len - off;

    switch (code) {
    case 0:
    case 1:
    case 2:
        set_title(vt, body, body_len);
        break;
    default:
        if (vt->callbacks.osc) {
            /* The data is in osc_buf which is uint8_t[]; the
             * callback signature uses const char* for convenience.
             * NUL-termination is not guaranteed; len is authoritative. */
            vt->callbacks.osc(code, (const char *)body, body_len, vt->callback_user);
        }
        break;
    }
}
