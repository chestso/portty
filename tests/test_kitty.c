/*
 * portty — kitty graphics (APC G) end-to-end tests through the host bridge
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_kitty — kitty graphics end-to-end through the portty bridge.
 *
 * The decode/store logic is unit-tested in coffer (test_cfr_graphics.c).
 * This file checks the portty side: that a kitty APC sequence fed through
 * terminal_process_input() reaches the engine and comes back out through
 * terminal_get_images() (pixels) and terminal_get_image_placements()
 * (placements with z-index) with IMG_SRC_KITTY source.
 */

#include "test_helpers.h"

#include "term.h"
#include "term_cfr.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern TerminalBackend terminal_backend_cfr;

static void feed(TerminalBackend *t, const char *s)
{
    terminal_process_input(t, s, strlen(s));
}

static void b64_encode(const uint8_t *data, size_t len, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t oi = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len)
            v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len)
            v |= data[i + 2];
        out[oi++] = tbl[(v >> 18) & 63];
        out[oi++] = tbl[(v >> 12) & 63];
        out[oi++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[oi++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    out[oi] = '\0';
}

/* A kitty RGBA transmit reaches the host as an image with IMG_SRC_KITTY. */
static void test_bridge_transmit(void)
{
    TerminalBackend t = terminal_backend_cfr;
    {
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = 20;
        cfg.rows = 10;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 6;
        ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
    };
    terminal_set_cell_px(&t, 10, 6);

    uint8_t rgba[2 * 2 * 4] = { 255, 0, 0, 255, 255, 0, 0, 255,
                                255, 0, 0, 255, 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);

    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=2,v=2,i=1;%s\x1b\\", b64);
    feed(&t, seq);

    int n = 0;
    const CfrImage *s = terminal_get_images(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].source, IMG_SRC_KITTY);
    ASSERT_EQ(s[0].width_px, 2);
    ASSERT_EQ(s[0].height_px, 2);
    ASSERT_EQ(s[0].rgba[0], 255);
    ASSERT_EQ(s[0].rgba[1], 0);
    ASSERT_EQ(s[0].rgba[2], 0);
    ASSERT_EQ(s[0].rgba[3], 255);

    terminal_destroy(&t);
}

/* A kitty place reaches the host as a placement with z-index and row/col. */
static void test_bridge_place(void)
{
    TerminalBackend t = terminal_backend_cfr;
    {
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = 20;
        cfg.rows = 10;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 6;
        ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
    };
    terminal_set_cell_px(&t, 10, 6);

    uint8_t rgba[4] = { 0, 255, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=9;%s\x1b\\", b64);
    feed(&t, seq);

    feed(&t, "\x1b_Ga=p,i=9,p=77,z=-3,x=2,y=4,w=5,h=6\x1b\\");

    int n = 0;
    const CfrImagePlacement *pls =
        terminal_get_image_placements(&t, &n);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(n, 1);
    ASSERT_EQ((long long)pls[0].image_id, 9);
    ASSERT_EQ((long long)pls[0].id, 77);
    ASSERT_EQ(pls[0].z_index, -3);
    ASSERT_EQ(pls[0].col, 2);
    ASSERT_EQ(pls[0].row, 4);
    ASSERT_EQ(pls[0].cols, 5);
    ASSERT_EQ(pls[0].rows, 6);

    terminal_destroy(&t);
}

/* Kitty images and sixel images share the same pixel query. */
static void test_bridge_mixed_sources(void)
{
    TerminalBackend t = terminal_backend_cfr;
    {
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = 20;
        cfg.rows = 10;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 6;
        ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
    };
    terminal_set_cell_px(&t, 10, 6);

    feed(&t, "\x1bPq#1;2;100;0;0#1BB\x1b\\");

    uint8_t rgba[4] = { 0, 0, 255, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=1,v=1,i=5;%s\x1b\\", b64);
    feed(&t, seq);

    int n = 0;
    const CfrImage *s = terminal_get_images(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(s[0].source, IMG_SRC_SIXEL);
    ASSERT_EQ(s[1].source, IMG_SRC_KITTY);

    terminal_destroy(&t);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("Running kitty graphics bridge tests:\n");

    RUN_TEST(test_bridge_transmit);
    RUN_TEST(test_bridge_place);
    RUN_TEST(test_bridge_mixed_sources);

    TEST_SUMMARY();
}
