/*
 * portty — Configuration parser tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "portty_conf.h"
#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Write content to a temp file and return the path (caller must free) */
static char *write_tmp_conf(const char *content)
{
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmpdir) == 0)
        return NULL;
    char path[MAX_PATH];
    if (GetTempFileNameA(tmpdir, "portty", 0, path) == 0)
        return NULL;
    FILE *fp = fopen(path, "w");
#else
    char path[] = "/tmp/portty_test_conf_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    FILE *fp = fdopen(fd, "w");
#endif
    if (!fp) {
#ifndef _WIN32
        close(fd);
#endif
        return NULL;
    }
    fputs(content, fp);
    fclose(fp);
    return strdup(path);
}

static void cleanup_tmp(char *path)
{
    if (path) {
        remove(path);
        free(path);
    }
}

/* --- Tests --- */

static void test_init_defaults(void)
{
    PorttyConf conf;
    portty_conf_init(&conf);

    ASSERT_NULL(conf.font);
    ASSERT_EQ(conf.cols, 0);
    ASSERT_EQ(conf.rows, 0);
    ASSERT_EQ(conf.hinting, PORTTY_HINT_UNSET);
    ASSERT_EQ(conf.verbose, -1);
    ASSERT_NULL(conf.word_chars);
    ASSERT_EQ(conf.scrollback, -1);
    ASSERT_EQ(conf.borderless, -1);
    ASSERT_TRUE(conf.dim_opacity < 0.0f);
    ASSERT_FALSE(conf.fg_color.set);
    ASSERT_FALSE(conf.bg_color.set);
    ASSERT_FALSE(conf.cursor_color.set);
    ASSERT_FALSE(conf.cursor_text_color.set);
    ASSERT_EQ(conf.cursor_shape, PORTTY_CURSOR_UNSET);
    ASSERT_EQ(conf.cursor_blink, -1);

    portty_conf_free(&conf);
}

static void test_parse_font(void)
{
    char *path = write_tmp_conf("font = monospace-14\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.font, "monospace-14");

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_geometry(void)
{
    char *path = write_tmp_conf("geometry = 120x40\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.cols, 120);
    ASSERT_EQ(conf.rows, 40);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_hinting(void)
{
    char *path = write_tmp_conf("hinting = mono\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.hinting, PORTTY_HINT_MONO);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_booleans(void)
{
    char *path = write_tmp_conf("verbose = yes\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.verbose, 1);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_borderless(void)
{
    /* borderless = true */
    char *path = write_tmp_conf("borderless = true\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.borderless, 1);

    portty_conf_free(&conf);
    cleanup_tmp(path);

    /* borderless = false */
    path = write_tmp_conf("borderless = false\n");
    ASSERT_NOT_NULL(path);

    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.borderless, 0);

    portty_conf_free(&conf);
    cleanup_tmp(path);

    /* invalid value: stays unset, load still succeeds */
    path = write_tmp_conf("borderless = maybe\n");
    ASSERT_NOT_NULL(path);

    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.borderless, -1);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_dim_opacity(void)
{
    /* A fractional value in range is stored. */
    char *path = write_tmp_conf("dim_opacity = 0.75\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_TRUE(conf.dim_opacity > 0.74f && conf.dim_opacity < 0.76f);
    portty_conf_free(&conf);
    cleanup_tmp(path);

    /* 0 is a valid value (fully dimmed). */
    path = write_tmp_conf("dim_opacity = 0\n");
    ASSERT_NOT_NULL(path);
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_TRUE(conf.dim_opacity == 0.0f);
    portty_conf_free(&conf);
    cleanup_tmp(path);

    /* Out of range stays unset, load still succeeds. */
    path = write_tmp_conf("dim_opacity = 1.5\n");
    ASSERT_NOT_NULL(path);
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_TRUE(conf.dim_opacity < 0.0f);
    portty_conf_free(&conf);
    cleanup_tmp(path);

    /* Non-numeric stays unset. */
    path = write_tmp_conf("dim_opacity = banana\n");
    ASSERT_NOT_NULL(path);
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_TRUE(conf.dim_opacity < 0.0f);
    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_word_chars(void)
{
    char *path = write_tmp_conf("word_chars = -_./~\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.word_chars, "-_./~");

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_scrollback(void)
{
    char *path = write_tmp_conf("scrollback = 5000\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.scrollback, 5000);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_scrollback_zero(void)
{
    char *path = write_tmp_conf("scrollback = 0\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.scrollback, 0);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_scrollback_invalid(void)
{
    char *path = write_tmp_conf("scrollback = -5\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.scrollback, -1);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_comments_and_blank_lines(void)
{
    char *path = write_tmp_conf(
        "# Top comment\n"
        "\n"
        "; semicolon comment\n"
        "font = test-font\n"
        "\n"
        "# inline comment line\n"
        "verbose = true\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.font, "test-font");
    ASSERT_EQ(conf.verbose, 1);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_parse_shell(void)
{
    char *path = write_tmp_conf("shell = /usr/bin/bash --norc\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.shell, "/usr/bin/bash --norc");

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_unknown_keys_ignored(void)
{
    char *path = write_tmp_conf("unknown_key = whatever\nfont = ok\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.font, "ok");

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_section_headers_tolerated(void)
{
    char *path = write_tmp_conf("[other]\nfont = ignored\nfont = kept\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_STR_EQ(conf.font, "kept");

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_nonexistent_file(void)
{
    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_FALSE(portty_conf_load_path(&conf, "/tmp/portty_nonexistent_12345.conf"));
    portty_conf_free(&conf);
}

static void test_all_hinting_modes(void)
{
    const char *modes[] = { "none", "light", "normal", "mono" };
    PorttyHintMode expected[] = { PORTTY_HINT_NONE, PORTTY_HINT_LIGHT, PORTTY_HINT_NORMAL,
                                  PORTTY_HINT_MONO };

    for (int i = 0; i < 4; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "hinting = %s\n", modes[i]);
        char *path = write_tmp_conf(buf);
        ASSERT_NOT_NULL(path);

        PorttyConf conf;
        portty_conf_init(&conf);
        ASSERT_TRUE(portty_conf_load_path(&conf, path));
        ASSERT_EQ(conf.hinting, expected[i]);

        portty_conf_free(&conf);
        cleanup_tmp(path);
    }
}

/* BEL presentation. Unset means `always`; an unknown value leaves the
 * previous setting alone. */
static void test_bell_modes(void)
{
    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_EQ(conf.bell, PORTTY_BELL_ALWAYS);
    portty_conf_free(&conf);

    const char *modes[] = { "always", "unfocused", "none" };
    PorttyBellMode expected[] = { PORTTY_BELL_ALWAYS, PORTTY_BELL_UNFOCUSED,
                                  PORTTY_BELL_NONE };
    const char *names[] = { "always", "unfocused", "none" };

    for (int i = 0; i < 3; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "bell = %s\n", modes[i]);
        char *path = write_tmp_conf(buf);
        ASSERT_NOT_NULL(path);

        portty_conf_init(&conf);
        ASSERT_TRUE(portty_conf_load_path(&conf, path));
        ASSERT_EQ(conf.bell, expected[i]);
        ASSERT_STR_EQ(portty_bell_mode_name(expected[i]), names[i]);

        portty_conf_free(&conf);
        cleanup_tmp(path);
    }

    /* An invalid value is ignored, not applied. */
    char *path = write_tmp_conf("bell = speaker\n");
    ASSERT_NOT_NULL(path);
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.bell, PORTTY_BELL_ALWAYS);
    portty_conf_free(&conf);
    cleanup_tmp(path);
}

/* The four color keys land in their slots. */
static void test_color_keys(void)
{
    char *path = write_tmp_conf(
        "foreground = #f8f8f2\n"
        "background = rgb:10/20/30\n"
        "cursor_color = #bd93f9\n"
        "cursor_text_color = #282a36\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_TRUE(conf.fg_color.set);
    ASSERT_EQ(conf.fg_color.rgb, 0xF8F8F2u);
    ASSERT_TRUE(conf.bg_color.set);
    ASSERT_EQ(conf.bg_color.rgb, 0x102030u);
    ASSERT_TRUE(conf.cursor_color.set);
    ASSERT_EQ(conf.cursor_color.rgb, 0xBD93F9u);
    ASSERT_TRUE(conf.cursor_text_color.set);
    ASSERT_EQ(conf.cursor_text_color.rgb, 0x282A36u);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

/* All the XParseColor RGB forms land on the same value. */
static void test_color_forms(void)
{
    const char *forms[] = {
        "#f00",
        "#ff0000",
        "#ffff00000000",
        "rgb:f/0/0",
        "rgb:ff/0/0",
    };
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "foreground = %s\n", forms[i]);
        char *path = write_tmp_conf(buf);
        ASSERT_NOT_NULL(path);

        PorttyConf conf;
        portty_conf_init(&conf);
        ASSERT_TRUE(portty_conf_load_path(&conf, path));
        ASSERT_TRUE(conf.fg_color.set);
        ASSERT_EQ(conf.fg_color.rgb, 0xFF0000u);

        portty_conf_free(&conf);
        cleanup_tmp(path);
    }
}

/* XParseColor scaling: a 1-digit component covers 0..15 and scales up
 * — rgb:8 is #88, not #08. */
static void test_color_component_scaling(void)
{
    char *path = write_tmp_conf("background = rgb:8/8/8\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_TRUE(conf.bg_color.set);
    ASSERT_EQ(conf.bg_color.rgb, 0x888888u);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

/* Invalid colors warn and leave the previous value alone. */
static void test_color_invalid(void)
{
    char *path = write_tmp_conf(
        "foreground = red\n"
        "foreground = #12345\n"
        "background = rgb:zz/0/0\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_FALSE(conf.fg_color.set);
    ASSERT_FALSE(conf.bg_color.set);

    portty_conf_free(&conf);
    cleanup_tmp(path);

    /* A bad value after a good one does not clobber it. */
    path = write_tmp_conf(
        "foreground = #123456\n"
        "foreground = not-a-color\n");
    ASSERT_NOT_NULL(path);
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_TRUE(conf.fg_color.set);
    ASSERT_EQ(conf.fg_color.rgb, 0x123456u);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

/* Cursor shape: block/underline/bar with the hbar/vbar aliases. */
static void test_cursor_shape(void)
{
    const char *values[] = { "block", "underline", "hbar", "bar", "vbar" };
    PorttyCursorShape expected[] = {
        PORTTY_CURSOR_BLOCK,
        PORTTY_CURSOR_UNDERLINE,
        PORTTY_CURSOR_UNDERLINE,
        PORTTY_CURSOR_BAR,
        PORTTY_CURSOR_BAR,
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "cursor = %s\n", values[i]);
        char *path = write_tmp_conf(buf);
        ASSERT_NOT_NULL(path);

        PorttyConf conf;
        portty_conf_init(&conf);
        ASSERT_TRUE(portty_conf_load_path(&conf, path));
        ASSERT_EQ(conf.cursor_shape, expected[i]);

        portty_conf_free(&conf);
        cleanup_tmp(path);
    }

    /* Invalid: warn and leave the previous value alone. */
    char *path = write_tmp_conf(
        "cursor = bar\n"
        "cursor = underline-me\n");
    ASSERT_NOT_NULL(path);
    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.cursor_shape, PORTTY_CURSOR_BAR);
    portty_conf_free(&conf);
    cleanup_tmp(path);
}

static void test_cursor_blink(void)
{
    char *path = write_tmp_conf("cursor_blink = false\n");
    ASSERT_NOT_NULL(path);

    PorttyConf conf;
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.cursor_blink, 0);

    portty_conf_free(&conf);
    cleanup_tmp(path);

    path = write_tmp_conf("cursor_blink = true\n");
    ASSERT_NOT_NULL(path);
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.cursor_blink, 1);

    portty_conf_free(&conf);
    cleanup_tmp(path);

    path = write_tmp_conf("cursor_blink = sometimes\n");
    ASSERT_NOT_NULL(path);
    portty_conf_init(&conf);
    ASSERT_TRUE(portty_conf_load_path(&conf, path));
    ASSERT_EQ(conf.cursor_blink, -1);

    portty_conf_free(&conf);
    cleanup_tmp(path);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_conf\n");

    RUN_TEST(test_init_defaults);
    RUN_TEST(test_parse_font);
    RUN_TEST(test_parse_geometry);
    RUN_TEST(test_parse_hinting);
    RUN_TEST(test_parse_booleans);
    RUN_TEST(test_parse_borderless);
    RUN_TEST(test_parse_dim_opacity);
    RUN_TEST(test_parse_word_chars);
    RUN_TEST(test_parse_scrollback);
    RUN_TEST(test_parse_scrollback_zero);
    RUN_TEST(test_parse_scrollback_invalid);
    RUN_TEST(test_comments_and_blank_lines);
    RUN_TEST(test_parse_shell);
    RUN_TEST(test_unknown_keys_ignored);
    RUN_TEST(test_section_headers_tolerated);
    RUN_TEST(test_nonexistent_file);
    RUN_TEST(test_all_hinting_modes);
    RUN_TEST(test_bell_modes);
    RUN_TEST(test_color_keys);
    RUN_TEST(test_color_forms);
    RUN_TEST(test_color_component_scaling);
    RUN_TEST(test_color_invalid);
    RUN_TEST(test_cursor_shape);
    RUN_TEST(test_cursor_blink);

    TEST_SUMMARY();
}
