/*
 * portty — Base64 encoder and decoder
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "base64.h"

#include <stdlib.h>

static const int8_t b64_lookup[256] = {
    /* 0x00 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x10 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x20 */ -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    62,
    -1,
    -1,
    -1,
    63,
    /* 0x30 */ 52,
    53,
    54,
    55,
    56,
    57,
    58,
    59,
    60,
    61,
    -1,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x40 */ -1,
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    /* 0x50 */ 15,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    23,
    24,
    25,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x60 */ -1,
    26,
    27,
    28,
    29,
    30,
    31,
    32,
    33,
    34,
    35,
    36,
    37,
    38,
    39,
    40,
    /* 0x70 */ 41,
    42,
    43,
    44,
    45,
    46,
    47,
    48,
    49,
    50,
    51,
    -1,
    -1,
    -1,
    -1,
    -1,
    /* 0x80..0xFF: -1 by default initializer */
};

static int is_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

uint8_t *base64_decode(const char *src, size_t len, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!src)
        return NULL;

    /* Worst case 3 bytes per 4 input chars; over-allocate a bit. */
    uint8_t *out = malloc(len + 3);
    if (!out)
        return NULL;
    size_t out_pos = 0;

    uint32_t accum = 0;
    int bits = 0;
    int pad = 0;

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (is_ws(c))
            continue;
        if (c == '=') {
            pad++;
            bits += 6;
            accum <<= 6;
            if (bits >= 8) {
                bits -= 8;
                /* discard pad bits, don't emit */
            }
            continue;
        }
        int8_t v = b64_lookup[c];
        if (v < 0) {
            free(out);
            return NULL;
        }
        if (pad) {
            /* Non-pad after pad → malformed. */
            free(out);
            return NULL;
        }
        accum = (accum << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[out_pos++] = (uint8_t)((accum >> bits) & 0xFF);
        }
    }

    if (out_len)
        *out_len = out_pos;
    return out;
}
