/*
 * test_paste_normalize — regression tests for clipboard line-ending
 * normalization (the fix for blank lines appearing after every pasted
 * line on Windows CRLF clipboards).
 *
 * The PTY line discipline treats both '\r' and '\n' as Enter, so a
 * Windows clipboard ("\r\n" per line) pasted verbatim produces two
 * Enters per line — a blank line after each line. The normalization
 * collapses CRLF and bare LF to a single CR before writing to the PTY.
 *
 * The logic under test lives in terminal_paste_normalize() in term.c.
 * It is a pure byte transform with no terminal/PTY dependencies, so we
 * replicate it inline here (same approach as test_clipboard_deferred)
 * to keep this test link-light and free of a coffer dependency.
 */

#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Replicate terminal_paste_normalize() from src/term.c                */
/* ------------------------------------------------------------------ */

static char *paste_normalize(const char *text, size_t len, size_t *out_len)
{
    if (!text || len == 0) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c == '\r') {
            buf[j++] = '\r';
            if (i + 1 < len && text[i + 1] == '\n')
                i++;
        } else if (c == '\n') {
            buf[j++] = '\r';
        } else {
            buf[j++] = c;
        }
    }
    buf[j] = '\0';
    if (out_len)
        *out_len = j;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* The original bug: a Windows CRLF clipboard must collapse each \r\n
 * into a single \r so the PTY sees one Enter per line, not two. */
static void test_crlf_collapses_to_single_cr(void)
{
    const char *in = "line1\r\nline2\r\nline3\r\n";
    size_t n = 0;
    char *out = paste_normalize(in, strlen(in), &n);
    ASSERT_NOT_NULL(out);
    /* "line1\rline2\rline3\r" — 18 bytes, no LFs remain */
    ASSERT_STR_EQ(out, "line1\rline2\rline3\r");
    ASSERT_EQ((long long)n, 18LL);
    free(out);
}

/* A bare-LF (Unix) clipboard also maps each line ending to one CR. */
static void test_bare_lf_becomes_cr(void)
{
    const char *in = "a\nb\nc\n";
    size_t n = 0;
    char *out = paste_normalize(in, strlen(in), &n);
    ASSERT_NOT_NULL(out);
    ASSERT_STR_EQ(out, "a\rb\rc\r");
    ASSERT_EQ((long long)n, 6LL);
    free(out);
}

/* Already-normalized CR-only text must pass through unchanged. */
static void test_cr_only_passthrough(void)
{
    const char *in = "x\ry\rz\r";
    size_t n = 0;
    char *out = paste_normalize(in, strlen(in), &n);
    ASSERT_NOT_NULL(out);
    ASSERT_STR_EQ(out, "x\ry\rz\r");
    ASSERT_EQ((long long)n, (long long)strlen(in));
    free(out);
}

/* A single-line paste with no line endings is untouched. */
static void test_no_line_endings_untouched(void)
{
    const char *in = "just some text";
    size_t n = 0;
    char *out = paste_normalize(in, strlen(in), &n);
    ASSERT_NOT_NULL(out);
    ASSERT_STR_EQ(out, "just some text");
    ASSERT_EQ((long long)n, (long long)strlen(in));
    free(out);
}

/* Empty / NULL input must return NULL and zero length, not crash. */
static void test_empty_and_null_safe(void)
{
    size_t n = 42;
    char *out = paste_normalize("", 0, &n);
    ASSERT_NULL(out);
    ASSERT_EQ((long long)n, 0LL);

    out = paste_normalize(NULL, 5, &n);
    ASSERT_NULL(out);
    ASSERT_EQ((long long)n, 0LL);

    /* out_len may be NULL. */
    out = paste_normalize(NULL, 5, NULL);
    ASSERT_NULL(out);
}

/* A trailing lone CR (no following LF) must be preserved, not dropped. */
static void test_trailing_lone_cr_preserved(void)
{
    const char *in = "text\r";
    size_t n = 0;
    char *out = paste_normalize(in, strlen(in), &n);
    ASSERT_NOT_NULL(out);
    ASSERT_STR_EQ(out, "text\r");
    ASSERT_EQ((long long)n, 5LL);
    free(out);
}

/* Mixed endings: CRLF, bare LF, and lone CR in one buffer. */
static void test_mixed_endings(void)
{
    const char *in = "a\r\nb\nc\rd";
    size_t n = 0;
    char *out = paste_normalize(in, strlen(in), &n);
    ASSERT_NOT_NULL(out);
    /* a<CR> b<CR> c<CR> d */
    ASSERT_STR_EQ(out, "a\rb\rc\rd");
    ASSERT_EQ((long long)n, 7LL);
    free(out);
}

/* A CRLF split so the CR is the very last byte (no trailing LF) is
 * still just a single CR — the lookahead must not read past the end. */
static void test_crlf_at_end(void)
{
    const char *in = "abc\r\n";
    size_t n = 0;
    char *out = paste_normalize(in, strlen(in), &n);
    ASSERT_NOT_NULL(out);
    ASSERT_STR_EQ(out, "abc\r");
    ASSERT_EQ((long long)n, 4LL);
    free(out);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_crlf_collapses_to_single_cr);
    RUN_TEST(test_bare_lf_becomes_cr);
    RUN_TEST(test_cr_only_passthrough);
    RUN_TEST(test_no_line_endings_untouched);
    RUN_TEST(test_empty_and_null_safe);
    RUN_TEST(test_trailing_lone_cr_preserved);
    RUN_TEST(test_mixed_endings);
    RUN_TEST(test_crlf_at_end);

    TEST_SUMMARY();
}
