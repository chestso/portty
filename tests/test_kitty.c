/*
 * portty — Kitty graphics (APC G) end-to-end tests through the host bridge
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

#include "config.h"

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

    /* A 10x10 green image. */
    uint8_t rgba[10 * 10 * 4];
    for (int i = 0; i < 10 * 10; i++) {
        rgba[i * 4 + 0] = 0;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3] = 255;
    }
    char b64[1024];
    b64_encode(rgba, sizeof(rgba), b64);
    char seq[2048];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=t,f=32,s=10,v=10,i=9;%s\x1b\\", b64);
    feed(&t, seq);

    /* The placement sits at the cursor; x=/y=/w=/h= are the source rect
     * in pixels, c=/r= the display box in cells, X=/Y= the offset inside
     * the first cell. */
    feed(&t, "\x1b_Ga=p,i=9,p=77,z=-3,x=2,y=4,w=5,h=6,c=3,r=2,X=1,Y=2\x1b\\");

    int n = 0;
    const CfrImagePlacement *pls =
        terminal_get_image_placements(&t, &n);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(n, 1);
    ASSERT_EQ((long long)pls[0].image_id, 9);
    ASSERT_EQ((long long)pls[0].id, 77);
    ASSERT_EQ(pls[0].z_index, -3);
    ASSERT_EQ(pls[0].col, 0);
    ASSERT_EQ(pls[0].row, 0);
    ASSERT_EQ(pls[0].cols, 3);
    ASSERT_EQ(pls[0].rows, 2);
    ASSERT_EQ(pls[0].src_x, 2);
    ASSERT_EQ(pls[0].src_y, 4);
    ASSERT_EQ(pls[0].src_w, 5);
    ASSERT_EQ(pls[0].src_h, 6);
    ASSERT_EQ(pls[0].cell_off_x, 1);
    ASSERT_EQ(pls[0].cell_off_y, 2);

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

/* Chunked transmit (m=1/m=0) with a=T (transmit-and-place) where
 * continuation chunks omit the action key and all metadata — the exact
 * form chafa emits. The image must be stored AND placed at the cursor. */
static void test_bridge_chunked_transmit_no_action(void)
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

    uint8_t rgba[16] = { 255, 0, 0, 255, 255, 0, 0, 255,
                         255, 0, 0, 255, 255, 0, 0, 255 };
    char b64[64];
    b64_encode(rgba, sizeof(rgba), b64);
    size_t half = strlen(b64) / 2;
    char first[64], second[64];
    memcpy(first, b64, half);
    first[half] = '\0';
    strcpy(second, b64 + half);

    char seq[512];
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=2,v=2,i=1,m=1;%s\x1b\\", first);
    feed(&t, seq);
    snprintf(seq, sizeof(seq),
             "\x1b_Gm=1;%s\x1b\\", second);
    feed(&t, seq);
    snprintf(seq, sizeof(seq), "\x1b_Gm=0\x1b\\");
    feed(&t, seq);

    int n = 0;
    const CfrImage *s = terminal_get_images(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].source, IMG_SRC_KITTY);
    ASSERT_EQ(s[0].width_px, 2);
    ASSERT_EQ(s[0].height_px, 2);
    ASSERT_EQ(s[0].rgba[0], 255);
    ASSERT_EQ(s[0].rgba[3], 255);

    int pn = 0;
    const CfrImagePlacement *pls =
        terminal_get_image_placements(&t, &pn);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(pn, 1);
    ASSERT_EQ((long long)pls[0].image_id, 1);

    terminal_destroy(&t);
}

/* Clients that stream successive images under one fixed id (a=T,i=N
 * each time) rely on the spec's re-transmit rule: the old placement is
 * deleted and the new placement shows the NEW pixels. The renderer's
 * texture cache keys on (id, version), so the version must differ
 * across the re-transmit or the previous image's pixels stay cached. */
static void test_bridge_retransmit_same_id(void)
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

    char seq[256];
    char b64[64];

    uint8_t red[4] = { 255, 0, 0, 255 };
    b64_encode(red, sizeof(red), b64);
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,i=1,q=2,C=1;%s\x1b\\", b64);
    feed(&t, seq);

    int n = 0;
    const CfrImage *s = terminal_get_images(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 1);
    uint32_t v0 = s[0].version;

    /* Same id, same dimensions, new pixels. */
    uint8_t green[4] = { 0, 255, 0, 255 };
    b64_encode(green, sizeof(green), b64);
    snprintf(seq, sizeof(seq),
             "\x1b_Ga=T,f=32,s=1,v=1,i=1,q=2,C=1;%s\x1b\\", b64);
    feed(&t, seq);

    s = terminal_get_images(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 1);
    ASSERT_TRUE(s[0].version != v0); /* texture cache must re-upload */
    ASSERT_EQ(s[0].rgba[0], 0);
    ASSERT_EQ(s[0].rgba[1], 255);

    int pn = 0;
    const CfrImagePlacement *pls =
        terminal_get_image_placements(&t, &pn);
    ASSERT_NOT_NULL(pls);
    ASSERT_EQ(pn, 1); /* one live placement, not two */
    ASSERT_EQ((long long)pls[0].image_id, 1);

    terminal_destroy(&t);
}

/* The host uploads the image's PIXEL BUFFER: buf_w * buf_h * 4 bytes
 * taken from the engine's rgba. The buffer holds the decoded (native)
 * pixels, so buf_w/buf_h are the buffer's real extent — the display
 * scale lives in width_px/height_px. Copying the scaled size out of
 * the native buffer reads past the end of the allocation, so the test
 * copies exactly what the renderer copies: under ASan an overread
 * fails the test.
 *
 * Gated on the engine that reports the buffer's real extent (the
 * configure check for the image handle detects it). */
#ifdef HAVE_COFFER_IMAGE_HANDLE
static void test_texture_upload_at_content_scale(void)
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
    terminal_set_content_scale(&t, 2.0f);

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

    /* The DISPLAY is scaled: the 2x2 image is drawn 4x4 physical. */
    ASSERT_EQ(s[0].width_px, 4);
    ASSERT_EQ(s[0].height_px, 4);
    /* The BUFFER is not: it holds the 2x2 decoded pixels. */
    ASSERT_EQ(s[0].buf_w, 2);
    ASSERT_EQ(s[0].buf_h, 2);

    /* What the renderer does: copy buf_w * buf_h * 4 bytes out of the
     * engine's buffer. Under ASan an overread fails the test. */
    size_t bytes = (size_t)s[0].buf_w * (size_t)s[0].buf_h * 4;
    ASSERT_EQ(bytes, (size_t)(2 * 2 * 4));
    uint8_t *copy = malloc(bytes);
    ASSERT_NOT_NULL(copy);
    memcpy(copy, s[0].rgba, bytes);
    ASSERT_EQ(copy[0], 255);
    ASSERT_EQ(copy[3], 255);
    free(copy);

    terminal_destroy(&t);
}
#endif /* HAVE_COFFER_IMAGE_HANDLE */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("Running kitty graphics bridge tests:\n");

    RUN_TEST(test_bridge_transmit);
    RUN_TEST(test_bridge_place);
    RUN_TEST(test_bridge_mixed_sources);
    RUN_TEST(test_bridge_chunked_transmit_no_action);
    RUN_TEST(test_bridge_retransmit_same_id);
#ifdef HAVE_COFFER_IMAGE_HANDLE
    RUN_TEST(test_texture_upload_at_content_scale);
#endif

    TEST_SUMMARY();
}
