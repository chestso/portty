/*
 * test_sixel — sixel end-to-end through the host bridge.
 *
 * The decode/store/placement logic is unit-tested in coffer itself
 * (tests/test_cfr_sixel.c). This file checks the portty side:
 * that a DCS sixel sequence fed through terminal_process_input() reaches
 * the engine and comes back out through terminal_get_sixels() with the
 * right pixels, that the cell-pixel size plumbing drives row placement,
 * and that animation coalesces.
 */

#include "test_helpers.h"

#include "term.h"
#include "term_cfr.h"

#include <stdlib.h>
#include <string.h>

extern TerminalBackend terminal_backend_cfr;

static void feed(TerminalBackend *t, const char *s)
{
    terminal_process_input(t, s, strlen(s));
}

static const uint8_t *px(const CfrSixel *s, int x, int y)
{
    return s->rgba + ((size_t)y * s->width_px + x) * 4;
}

/* A DCS red square reaches the host as a decoded image. */
static void test_bridge_decode(void)
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

    int n = -1;
    const CfrSixel *s = terminal_get_sixels(&t, &n);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].width_px, 2);
    ASSERT_EQ(s[0].height_px, 6);
    ASSERT_EQ(s[0].row, 0);
    ASSERT_EQ(s[0].col, 0);
    ASSERT_EQ(px(&s[0], 0, 0)[0], 255); /* red */
    ASSERT_EQ(px(&s[0], 0, 0)[3], 255); /* opaque */

    /* Cell pixel size (10x6) → a 6px image is one row tall; cursor moved
     * to the line below. */
    TerminalPos cur = terminal_get_cursor_pos(&t);
    ASSERT_EQ(cur.row, 1);

    terminal_destroy(&t);
}

/* Without a cell size the engine still decodes but can't advance rows. */
static void test_bridge_no_cell_size(void)
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
    /* deliberately no terminal_set_cell_px */
    feed(&t, "\x1bPq#1;2;0;100;0#1~\x1b\\");

    int n = -1;
    const CfrSixel *s = terminal_get_sixels(&t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].width_px, 1);
    terminal_destroy(&t);
}

/* Animation: in-place frames coalesce into one image, version bumps. */
static void test_bridge_animation(void)
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
    feed(&t, "\x1b[?80h"); /* in-place */

    feed(&t, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    const CfrSixel *s = terminal_get_sixels(&t, &n);
    ASSERT_EQ(n, 1);
    uint64_t id0 = s[0].id;
    uint32_t v0 = s[0].version;

    feed(&t, "\x1bPq#1;2;0;100;0#1~\x1b\\");
    s = terminal_get_sixels(&t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(s[0].id, id0);
    ASSERT_EQ(s[0].version, v0 + 1);

    terminal_destroy(&t);
}

/* Clearing the screen removes images. */
static void test_bridge_clear(void)
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
    feed(&t, "\x1bPq#1;2;100;0;0#1~\x1b\\");
    int n = -1;
    terminal_get_sixels(&t, &n);
    ASSERT_EQ(n, 1);

    feed(&t, "\x1b[2J");
    terminal_get_sixels(&t, &n);
    ASSERT_EQ(n, 0);
    terminal_destroy(&t);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_sixel\n");
    RUN_TEST(test_bridge_decode);
    RUN_TEST(test_bridge_no_cell_size);
    RUN_TEST(test_bridge_animation);
    RUN_TEST(test_bridge_clear);
    TEST_SUMMARY();
}
