/*
 * bloom-vt — 256-color palette resolution.
 *
 * Layout:
 *   0-15:   ANSI base colors (Charm dark style by default; matches term_vt.c)
 *   16-231: 6×6×6 cube
 *   232-255: greyscale ramp
 */

#include "bloom_vt_internal.h"

static const uint8_t base16[16][3] = {
    { 0x1a, 0x1a, 0x1a },
    { 0xed, 0x56, 0x7a },
    { 0x02, 0xbf, 0x87 },
    { 0xec, 0xcc, 0x68 },
    { 0x75, 0x71, 0xf9 },
    { 0xf7, 0x80, 0xe2 },
    { 0x6e, 0xef, 0xc0 },
    { 0xd0, 0xd0, 0xd0 },
    { 0x67, 0x67, 0x67 },
    { 0xff, 0x8d, 0xa1 },
    { 0x5a, 0xee, 0xad },
    { 0xf5, 0xdf, 0xa0 },
    { 0x9b, 0x98, 0xff },
    { 0xff, 0x9c, 0xe8 },
    { 0xa5, 0xf5, 0xd4 },
    { 0xff, 0xfd, 0xf5 },
};

uint32_t bvt_palette_lookup(BvtTerm *vt, uint8_t idx)
{
    (void)vt; /* OSC 4 palette overrides will hook in here later. */
    if (idx < 16) {
        const uint8_t *c = base16[idx];
        return ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
    }
    if (idx < 232) {
        /* 6×6×6 cube. Each step: {0, 95, 135, 175, 215, 255}. */
        static const uint8_t levels[6] = { 0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff };
        uint8_t v = idx - 16;
        uint8_t r = levels[(v / 36) % 6];
        uint8_t g = levels[(v / 6) % 6];
        uint8_t b = levels[v % 6];
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    /* 24-step grey: 8 + 10*(idx-232). */
    uint8_t v = (uint8_t)(8 + 10 * (idx - 232));
    return ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
}
