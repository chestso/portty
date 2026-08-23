/*
 * portty — iTerm2 inline image (OSC 1337) end-to-end tests through the host bridge
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_osc1337 — OSC 1337 end-to-end through the portty bridge.
 *
 * The decode/store logic is unit-tested in coffer (test_cfr_osc_1337.c).
 * This file checks the portty side: that an OSC 1337 sequence fed through
 * terminal_process_input() reaches the engine and comes back out through
 * terminal_get_images() with the right pixels and IMG_SRC_ITERM source.
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

/* A 1x1 red RGBA PNG, base64-encoded. */
static const char *PNG_1X1_RED_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";

/* A 2x2 RGBA PNG with semi-transparent pixels (r=128, g=64, b=32, a=128). */
static const char *PNG_2X2_SEMI_ALPHA_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR4nGNocFBoAGEGGAMAMdIFgRA3teIAAAAASUVORK5CYII=";

/* An inline PNG image reaches the host as a decoded image with IMG_SRC_ITERM. */
static void test_bridge_inline_png(void)
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
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1:%s\x07",
             PNG_1X1_RED_B64);
    feed(&t, seq);

    int n = 0;
    const CfrImage *s = terminal_get_images(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].source, IMG_SRC_ITERM);
    ASSERT_EQ(s[0].width_px, 1);
    ASSERT_EQ(s[0].height_px, 1);
    ASSERT_EQ(s[0].rgba[0], 255);
    ASSERT_EQ(s[0].rgba[1], 0);
    ASSERT_EQ(s[0].rgba[2], 0);
    ASSERT_EQ(s[0].rgba[3], 255);

    terminal_destroy(&t);
}

/* Intermediate alpha is preserved through the bridge. */
static void test_bridge_alpha(void)
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
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1:%s\x07",
             PNG_2X2_SEMI_ALPHA_B64);
    feed(&t, seq);

    int n = 0;
    const CfrImage *s = terminal_get_images(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].source, IMG_SRC_ITERM);
    ASSERT_EQ(s[0].width_px, 2);
    ASSERT_EQ(s[0].height_px, 2);
    /* Semi-transparent pixel: r=128, g=64, b=32, a=128 */
    ASSERT_EQ(s[0].rgba[0], 128);
    ASSERT_EQ(s[0].rgba[1], 64);
    ASSERT_EQ(s[0].rgba[2], 32);
    ASSERT_EQ(s[0].rgba[3], 128);

    terminal_destroy(&t);
}

/* Download mode (inline=0) is ignored — no image appears. */
static void test_bridge_download_ignored(void)
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
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=0:%s\x07",
             PNG_1X1_RED_B64);
    feed(&t, seq);

    int n = 0;
    const CfrImage *s = terminal_get_images(&t, &n);
    ASSERT_EQ(n, 0);
    ASSERT_NULL(s);

    terminal_destroy(&t);
}

/* Both sixel and iTerm2 images appear through the same query. */
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

    /* Place a sixel image */
    feed(&t, "\x1bPq#1;2;100;0;0#1BB\x1b\\");
    /* Place an iTerm2 image (moves cursor down first from sixel) */
    char seq[256];
    snprintf(seq, sizeof(seq),
             "\x1b]1337;File=inline=1:%s\x07",
             PNG_1X1_RED_B64);
    feed(&t, seq);

    int n = 0;
    const CfrImage *s = terminal_get_images(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 2);
    /* First image is sixel, second is iTerm2 */
    ASSERT_EQ(s[0].source, IMG_SRC_SIXEL);
    ASSERT_EQ(s[1].source, IMG_SRC_ITERM);

    terminal_destroy(&t);
}

/* A 4x4 green PNG, base64-encoded (matches coffer test fixture). */
static const char *PNG_4X4_GREEN_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAD0lEQVR4nGNg+I8GSRcAACxQH+H81I9XAAAAAElFTkSuQmCC";

/* Multipart transfer (MultipartFile/FilePart/FileEnd) assembles the
 * image across multiple OSC 1337 sequences through the bridge. */
static void test_bridge_multipart(void)
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

    const char *b64 = PNG_4X4_GREEN_B64;
    size_t b64_len = strlen(b64);
    size_t chunk_size = 32;

    feed(&t, "\x1b]1337;MultipartFile=inline=1\x07");

    for (size_t off = 0; off < b64_len; off += chunk_size) {
        char seq[128];
        size_t this_len = b64_len - off;
        if (this_len > chunk_size)
            this_len = chunk_size;
        snprintf(seq, sizeof(seq), "\x1b]1337;FilePart=%.*s\x07",
                 (int)this_len, b64 + off);
        feed(&t, seq);
    }

    feed(&t, "\x1b]1337;FileEnd\x07");

    int n = 0;
    const CfrImage *s = terminal_get_images(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].source, IMG_SRC_ITERM);
    ASSERT_EQ(s[0].width_px, 4);
    ASSERT_EQ(s[0].height_px, 4);
    ASSERT_EQ(s[0].rgba[1], 255);
    ASSERT_EQ(s[0].rgba[3], 255);

    terminal_destroy(&t);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("Running OSC 1337 bridge tests:\n");

    RUN_TEST(test_bridge_inline_png);
    RUN_TEST(test_bridge_alpha);
    RUN_TEST(test_bridge_download_ignored);
    RUN_TEST(test_bridge_mixed_sources);
    RUN_TEST(test_bridge_multipart);

    TEST_SUMMARY();
}
