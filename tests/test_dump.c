/*
 * portty — Terminal state dump (JSON) tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_dump — the Ctrl+Shift+F7 / `dumpstate` JSON serializer.
 *
 * Creates a small coffer terminal, feeds ANSI, dumps to a tmpfile, and
 * asserts on the JSON. There is no JSON parser in the tree, so the checks
 * are substring-based but anchored on exact, load-bearing fragments
 * (arithmetic-free values). Covers the header, geometry, cell encoding
 * (codepoint/width/attrs/colors), wide cells, mode flags, hyperlink
 * interning, and scrollback capture.
 */

#include "test_helpers.h"

#include "dump.h"
#include "term.h"
#include "term_cfr.h"

#include <stdlib.h>
#include <string.h>

extern TerminalBackend terminal_backend_cfr;

static void feed(TerminalBackend *t, const char *s)
{
    terminal_process_input(t, s, strlen(s));
}

static TerminalBackend make_term(int cols, int rows)
{
    TerminalBackend t = terminal_backend_cfr;
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.cols = cols;
    cfg.rows = rows;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    terminal_init(&t, &cfg);
    return t;
}

static char *dump_to_string(TerminalBackend *t, const TerminalDumpMeta *meta)
{
    FILE *f = tmpfile();
    if (!f)
        return NULL;
    if (terminal_dump_json(f, t, meta) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)n, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static int count_substr(const char *hay, const char *needle)
{
    int n = 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nl)
        n++;
    return n;
}

static void test_header_and_geometry(void)
{
    TerminalBackend t = make_term(5, 2);
    feed(&t, "hi");
    char *j = dump_to_string(&t, NULL);
    ASSERT_NOT_NULL(j);
    ASSERT_TRUE(strstr(j, "\"schema\": \"portty.terminal-dump\"") != NULL);
    ASSERT_TRUE(strstr(j, "\"version\": 1") != NULL);
    ASSERT_TRUE(strstr(j, "\"name\": \"coffer\"") != NULL);
    ASSERT_TRUE(strstr(j, "\"cols\": 5") != NULL);
    ASSERT_TRUE(strstr(j, "\"rows\": 2") != NULL);
    ASSERT_TRUE(strstr(j, "\"unified_row_min\": 0") != NULL);
    ASSERT_TRUE(strstr(j, "\"unified_row_max\": 1") != NULL);
    free(j);
    terminal_destroy(&t);
}

static void test_cell_codepoint_width_attrs_colors(void)
{
    TerminalBackend t = make_term(4, 1);
    /* "X" in bold red on the default background. */
    feed(&t, "\x1b[1;31mX\x1b[0m");
    char *j = dump_to_string(&t, NULL);
    ASSERT_NOT_NULL(j);
    /* [cp, grapheme, width, attrs, fg, bg, ul, link] */
    ASSERT_TRUE(strstr(j, "[88,0,1,\"b\",\"#ff5555\",\"default\",\"default\",0]") != NULL);
    free(j);
    terminal_destroy(&t);
}

static void test_wide_cell(void)
{
    TerminalBackend t = make_term(4, 1);
    feed(&t, "\xe6\xbc\xa2"); /* U+6F22, East-Asian wide → 2 cells */
    char *j = dump_to_string(&t, NULL);
    ASSERT_NOT_NULL(j);
    ASSERT_TRUE(strstr(j, "[28450,0,2,") != NULL);
    free(j);
    terminal_destroy(&t);
}

static void test_mode_flags(void)
{
    TerminalBackend t = make_term(4, 1);
    feed(&t, "\x1b[?2004h"); /* bracketed paste on */
    char *j = dump_to_string(&t, NULL);
    ASSERT_NOT_NULL(j);
    ASSERT_TRUE(strstr(j, "\"bracketed_paste\": true") != NULL);
    ASSERT_TRUE(strstr(j, "\"altscreen\": false") != NULL);
    free(j);
    terminal_destroy(&t);
}

static void test_hyperlink_interned_once(void)
{
    TerminalBackend t = make_term(8, 1);
    feed(&t, "\x1b]8;;https://example.com\x1b\\L\x1b]8;;\x1b\\L");
    char *j = dump_to_string(&t, NULL);
    ASSERT_NOT_NULL(j);
    ASSERT_TRUE(strstr(j, "\"links\"") != NULL);
    /* URI stored once even though two cells point at it. */
    ASSERT_EQ(count_substr(j, "https://example.com"), 1);
    free(j);
    terminal_destroy(&t);
}

static void test_scrollback_captured(void)
{
    TerminalBackend t = make_term(4, 3);
    feed(&t, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n");
    char *j = dump_to_string(&t, NULL);
    ASSERT_NOT_NULL(j);
    /* Rows scrolled off ⇒ a negative unified row range is present. */
    ASSERT_TRUE(strstr(j, "\"unified_row_min\": -") != NULL);
    ASSERT_TRUE(strstr(j, "\"row\": -1") != NULL);
    free(j);
    terminal_destroy(&t);
}

static void test_selection_serialized(void)
{
    TerminalBackend t = make_term(8, 1);
    feed(&t, "hello");
    terminal_selection_start(&t, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(&t, 0, 4);
    char *j = dump_to_string(&t, NULL);
    ASSERT_NOT_NULL(j);
    ASSERT_TRUE(strstr(j, "\"selection\": {") != NULL);
    ASSERT_TRUE(strstr(j, "\"mode\": \"char\"") != NULL);
    free(j);
    terminal_destroy(&t);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("Running dump tests:\n");
    RUN_TEST(test_header_and_geometry);
    RUN_TEST(test_cell_codepoint_width_attrs_colors);
    RUN_TEST(test_wide_cell);
    RUN_TEST(test_mode_flags);
    RUN_TEST(test_hyperlink_interned_once);
    RUN_TEST(test_scrollback_captured);
    RUN_TEST(test_selection_serialized);
    TEST_SUMMARY();
    return test_fail_count == 0 ? 0 : 1;
}
