/*
 * portty — PTY winsize pixel-size reporting tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/* The PTY reports the character grid's size in pixels through TIOCGWINSZ
 * (ws_xpixel/ws_ypixel), and a child that sizes images — chafa, img2sixel,
 * a file previewer — reads the cell's worth in pixels from there. Those
 * figures are LOGICAL pixels: everything a client sends back is scaled by
 * the content scale once, so a physical figure makes the client pack twice
 * the pixels per cell and the picture renders at twice its size. A zero
 * figure is worse: the client falls back to guessing the cell aspect.
 *
 * The child never has to be asked anything — TIOCGWINSZ on the master fd
 * reports the same winsize the child sees.
 */

#include "portty_pty.h"
#include "test_helpers.h"
#include <stdbool.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static bool read_winsize(PtyContext *ctx, struct winsize *ws)
{
    if (!ctx)
        return false;
    return ioctl(pty_get_master_fd(ctx), TIOCGWINSZ, ws) == 0;
}

/* A child that stays alive so the winsize can be read and changed. */
static PtyContext *spawn_child(int rows, int cols, int w_px, int h_px)
{
    char *argv[] = { "/bin/sh", "-c", "sleep 5", NULL };
    return pty_create(rows, cols, w_px, h_px, argv);
}

static void test_create_reports_grid_pixels(void)
{
    PtyContext *ctx = spawn_child(24, 80, 800, 480);
    ASSERT_NOT_NULL(ctx);

    struct winsize ws = { 0 };
    ASSERT_TRUE(read_winsize(ctx, &ws));
    ASSERT_EQ(ws.ws_row, 24);
    ASSERT_EQ(ws.ws_col, 80);
    ASSERT_EQ(ws.ws_xpixel, 800);
    ASSERT_EQ(ws.ws_ypixel, 480);

    pty_destroy(ctx);
}

/* 0 means "not measured yet" — it must stay 0 rather than become a guess. */
static void test_create_without_pixels_reports_zero(void)
{
    PtyContext *ctx = spawn_child(24, 80, 0, 0);
    ASSERT_NOT_NULL(ctx);

    struct winsize ws = { 0 };
    ASSERT_TRUE(read_winsize(ctx, &ws));
    ASSERT_EQ(ws.ws_xpixel, 0);
    ASSERT_EQ(ws.ws_ypixel, 0);

    pty_destroy(ctx);
}

static void test_set_pixel_size_reaches_the_child(void)
{
    PtyContext *ctx = spawn_child(24, 80, 800, 480);
    ASSERT_NOT_NULL(ctx);

    pty_set_pixel_size(ctx, 640, 384);

    struct winsize ws = { 0 };
    ASSERT_TRUE(read_winsize(ctx, &ws));
    ASSERT_EQ(ws.ws_xpixel, 640);
    ASSERT_EQ(ws.ws_ypixel, 384);
    /* The grid did not move. */
    ASSERT_EQ(ws.ws_row, 24);
    ASSERT_EQ(ws.ws_col, 80);

    pty_destroy(ctx);
}

/* A grid resize pushes the pixel size along: the two are one winsize. */
static void test_grid_resize_keeps_pixel_size(void)
{
    PtyContext *ctx = spawn_child(24, 80, 800, 480);
    ASSERT_NOT_NULL(ctx);

    pty_set_pixel_size(ctx, 640, 384);
    ASSERT_EQ(pty_resize(ctx, 30, 100), 0);

    struct winsize ws = { 0 };
    ASSERT_TRUE(read_winsize(ctx, &ws));
    ASSERT_EQ(ws.ws_row, 30);
    ASSERT_EQ(ws.ws_col, 100);
    ASSERT_EQ(ws.ws_xpixel, 640);
    ASSERT_EQ(ws.ws_ypixel, 384);

    pty_destroy(ctx);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    if (pty_signal_init() != 0) {
        fprintf(stderr, "pty_signal_init failed\n");
        return 1;
    }

    printf("test_pty_winsize\n");
    RUN_TEST(test_create_reports_grid_pixels);
    RUN_TEST(test_create_without_pixels_reports_zero);
    RUN_TEST(test_set_pixel_size_reaches_the_child);
    RUN_TEST(test_grid_resize_keeps_pixel_size);

    pty_signal_cleanup();
    TEST_SUMMARY();
}
