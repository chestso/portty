/*
 * portty — Shell quoting of dropped file paths tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_shell_quote — shell quoting of dropped file paths.
 *
 * A dropped path is pasted as shell input, so quoting is the security
 * boundary: a filename must arrive as one literal word, never as
 * command syntax. Both quoting styles are exercised on every platform.
 */

#include "test_helpers.h"

#include "../src/shell_quote.h"

#include <stdlib.h>

static void check_posix(const char *in, const char *want)
{
    char *q = shell_quote_posix(in);
    ASSERT_NOT_NULL(q);
    ASSERT_STR_EQ(q, want);
    free(q);
}

static void check_win(const char *in, const char *want)
{
    char *q = shell_quote_win(in);
    ASSERT_NOT_NULL(q);
    ASSERT_STR_EQ(q, want);
    free(q);
}

/* Even a path with no special characters is quoted — callers never
 * decide whether quoting is needed. */
static void test_posix_always_quotes(void)
{
    check_posix("/tmp/file.txt", "'/tmp/file.txt'");
    check_posix("", "''");
}

static void test_posix_spaces(void)
{
    check_posix("/tmp/a b", "'/tmp/a b'");
}

/* An embedded single quote ends the quoted run, escapes one quote,
 * and reopens: ' → '\'' */
static void test_posix_single_quote(void)
{
    check_posix("it's", "'it'\\''s'");
    check_posix("a'b'c", "'a'\\''b'\\''c'");
    check_posix("'", "''\\'''");
}

/* Shell metacharacters stay literal inside single quotes. */
static void test_posix_metachars_literal(void)
{
    check_posix("x;echo PWNED", "'x;echo PWNED'");
    check_posix("$HOME `id` $(id)", "'$HOME `id` $(id)'");
    check_posix("a\\b\"c", "'a\\b\"c'");
    check_posix("a|b&c>d<e", "'a|b&c>d<e'");
}

/* A literal newline in a filename survives quoting untouched. */
static void test_posix_newline_preserved(void)
{
    check_posix("a\nb", "'a\nb'");
}

static void test_posix_utf8_passthrough(void)
{
    check_posix("/tmp/caf\u00e9", "'/tmp/caf\u00e9'");
}

static void test_win_style(void)
{
    check_win("plain", "\"plain\"");
    check_win("a b", "\"a b\"");
    check_win("", "\"\"");
    /* cmd.exe and PowerShell read "" inside quotes as one literal ". */
    check_win("a\"b", "\"a\"\"b\"");
    check_win("x;echo PWNED", "\"x;echo PWNED\"");
}

static void test_null_input(void)
{
    ASSERT_NULL(shell_quote_posix(NULL));
    ASSERT_NULL(shell_quote_win(NULL));
    ASSERT_NULL(shell_quote(NULL));
}

/* The platform dispatch picks the style portty's child shells use. */
static void test_platform_dispatch(void)
{
    char *q = shell_quote("a b");
    ASSERT_NOT_NULL(q);
#ifdef _WIN32
    ASSERT_STR_EQ(q, "\"a b\"");
#else
    ASSERT_STR_EQ(q, "'a b'");
#endif
    free(q);
}

/* A multi-file drop is one paste: quoted words joined by single spaces. */
static void test_join(void)
{
    const char *one[] = { "/tmp/a" };
    char *q = shell_quote_join(one, 1);
    ASSERT_NOT_NULL(q);
#ifdef _WIN32
    ASSERT_STR_EQ(q, "\"/tmp/a\"");
#else
    ASSERT_STR_EQ(q, "'/tmp/a'");
#endif
    free(q);

    const char *many[] = { "/tmp/a b", "/tmp/c", "x;echo PWNED" };
    q = shell_quote_join(many, 3);
    ASSERT_NOT_NULL(q);
#ifdef _WIN32
    ASSERT_STR_EQ(q, "\"/tmp/a b\" \"/tmp/c\" \"x;echo PWNED\"");
#else
    ASSERT_STR_EQ(q, "'/tmp/a b' '/tmp/c' 'x;echo PWNED'");
#endif
    free(q);
}

static void test_join_degenerate(void)
{
    ASSERT_NULL(shell_quote_join(NULL, 3));
    const char *paths[] = { "/tmp/a" };
    ASSERT_NULL(shell_quote_join(paths, 0));
    const char *with_null[] = { "/tmp/a", NULL };
    ASSERT_NULL(shell_quote_join(with_null, 2));
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_posix_always_quotes);
    RUN_TEST(test_posix_spaces);
    RUN_TEST(test_posix_single_quote);
    RUN_TEST(test_posix_metachars_literal);
    RUN_TEST(test_posix_newline_preserved);
    RUN_TEST(test_posix_utf8_passthrough);
    RUN_TEST(test_win_style);
    RUN_TEST(test_null_input);
    RUN_TEST(test_platform_dispatch);
    RUN_TEST(test_join);
    RUN_TEST(test_join_degenerate);

    TEST_SUMMARY();
}
