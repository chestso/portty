/*
 * portty — PTY winsize pixel-size reporting tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/* The PTY reports the character grid's size in pixels through TIOCGWINSZ
 * (ws_xpixel/ws_ypixel), and a child that sizes images — chafa, img2sixel,
 * a file previewer — reads the cell's worth in pixels from there. Those
 * figures are DEVICE pixels: the cell-size queries (CSI 16t) report the
 * same unit and a client's raster draws 1:1, so a canvas sized from the
 * winsize lands sharp on a scaled display instead of upscaled. A zero
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
#if !defined(_WIN32)
#include <paths.h>
#endif

static bool read_winsize(PtyContext *ctx, struct winsize *ws)
{
    if (!ctx)
        return false;
    return ioctl(pty_get_master_fd(ctx), TIOCGWINSZ, ws) == 0;
}

/* A child that stays alive so the winsize can be read and changed. */
static PtyContext *spawn_child(int rows, int cols, int w_px, int h_px)
{
#ifdef _PATH_BSHELL
    char *argv[] = { _PATH_BSHELL, "-c", "sleep 5", NULL };
#else
    char *argv[] = { "/bin/sh", "-c", "sleep 5", NULL };
#endif
    return pty_create(rows, cols, w_px, h_px, argv);
}

/* A 24x80 grid on a 20x12 device-pixel cell reports 1600x288. */
static void test_create_reports_grid_pixels(void)
{
    PtyContext *ctx = spawn_child(24, 80, 1600, 288);
    ASSERT_NOT_NULL(ctx);

    struct winsize ws = { 0 };
    ASSERT_TRUE(read_winsize(ctx, &ws));
    ASSERT_EQ(ws.ws_row, 24);
    ASSERT_EQ(ws.ws_col, 80);
    ASSERT_EQ(ws.ws_xpixel, 1600);
    ASSERT_EQ(ws.ws_ypixel, 288);

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

/* A resize pushes the grid and its pixel extent as one winsize: a 30x100
 * grid on a 20x12 device-pixel cell reports 2000x360. */
static void test_resize_sets_grid_and_pixels(void)
{
    PtyContext *ctx = spawn_child(24, 80, 1600, 288);
    ASSERT_NOT_NULL(ctx);

    ASSERT_EQ(pty_resize(ctx, 30, 100, 2000, 360), 0);

    struct winsize ws = { 0 };
    ASSERT_TRUE(read_winsize(ctx, &ws));
    ASSERT_EQ(ws.ws_row, 30);
    ASSERT_EQ(ws.ws_col, 100);
    ASSERT_EQ(ws.ws_xpixel, 2000);
    ASSERT_EQ(ws.ws_ypixel, 360);

    pty_destroy(ctx);
}

/* 0 means "unreported": a resize with no pixels clears the fields. */
static void test_resize_without_pixels_reports_zero(void)
{
    PtyContext *ctx = spawn_child(24, 80, 1600, 288);
    ASSERT_NOT_NULL(ctx);

    ASSERT_EQ(pty_resize(ctx, 24, 80, 0, 0), 0);

    struct winsize ws = { 0 };
    ASSERT_TRUE(read_winsize(ctx, &ws));
    ASSERT_EQ(ws.ws_xpixel, 0);
    ASSERT_EQ(ws.ws_ypixel, 0);

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
    RUN_TEST(test_resize_sets_grid_and_pixels);
    RUN_TEST(test_resize_without_pixels_reports_zero);

    pty_signal_cleanup();
    TEST_SUMMARY();
}
