/*
 * test_term_bvt — covers the TerminalBackend bridge for bloom-vt.
 *
 * Two regressions motivated this file:
 *   1. Wrap-aware selection (double-click on a soft-wrapped word) reads
 *      `terminal_get_line_continuation` in unified row coordinates, so the
 *      backend has to translate negative rows to scrollback lookups AND
 *      flip bvt's "this row wraps into the next" flag into libvterm's
 *      "this row continues from the previous" semantic.
 *   2. Resize triggers reflow only when reflow_enabled is true. bvt's
 *      reflow path is stable, so the bvt backend now turns it on by
 *      default — verify the resize actually re-wraps content.
 */

#include "test_helpers.h"

#include "term.h"
#include "term_bvt.h"

#include <string.h>

extern TerminalBackend terminal_backend_bvt;

static void feed(TerminalBackend *t, const char *s) {
    terminal_process_input(t, s, strlen(s));
}

/* Soft-wrap in visible space: typing "abcdef" into 5 cols puts "abcde" on
 * row 0 and "f" on row 1. Selection scanning right from row 0 col 4 must
 * cross into row 1 — that requires `get_line_continuation(row=1)` to
 * return true (libvterm semantics: "row 1 continues from row 0"). */
static void test_visible_wrap_continuation(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 5, 2) != NULL);
    feed(&t, "abcdef");

    ASSERT_FALSE(terminal_get_line_continuation(&t, 0));
    ASSERT_TRUE(terminal_get_line_continuation(&t, 1));

    terminal_destroy(&t);
}

/* When a wrapped line scrolls off, the wrap relationship has to survive.
 * In unified coords the boundary is row 0 (visible) ↔ row -1 (most recent
 * scrollback). Scrolling "abcde\n...content..." until "abcde" is gone:
 * the row that used to be row 0 is now sb_row=k-1, and the wrap from it
 * into the next row must still be reachable. */
static void test_scrollback_wrap_continuation(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 5, 2) != NULL);
    /* "abcdef" wraps row 0 → row 1. Then push lots of newlines so both
     * rows scroll into history. */
    feed(&t, "abcdef");
    for (int i = 0; i < 5; ++i)
        feed(&t, "\r\nx");

    /* Walk scrollback from most recent (-1) backwards until we find the
     * wrap. Selection logic does the same when scanning across the
     * visible/scrollback boundary. */
    bool found_wrap = false;
    for (int row = -1; row >= -10; --row) {
        if (terminal_get_line_continuation(&t, row)) {
            found_wrap = true;
            break;
        }
    }
    ASSERT_TRUE(found_wrap);
    terminal_destroy(&t);
}

/* Resize wider → bvt should re-wrap "abcdef" from two rows into one. */
static void test_resize_grows_and_reflows(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 5, 2) != NULL);
    feed(&t, "abcdef");
    /* Confirm initial wrap. */
    ASSERT_TRUE(terminal_get_line_continuation(&t, 1));

    terminal_resize(&t, 10, 2);

    /* "abcdef" should now sit on a single row 0, no wrap. */
    TerminalCell c0, c5;
    ASSERT_EQ(terminal_get_cell(&t, 0, 0, &c0), 0);
    ASSERT_EQ(terminal_get_cell(&t, 0, 5, &c5), 0);
    ASSERT_EQ(c0.chars[0], (uint32_t)'a');
    ASSERT_EQ(c5.chars[0], (uint32_t)'f');
    ASSERT_FALSE(terminal_get_line_continuation(&t, 1));

    terminal_destroy(&t);
}

/* Resize narrower → a single-row "abcdefghij" wraps into two rows. */
static void test_resize_shrinks_and_reflows(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 10, 2) != NULL);
    feed(&t, "abcdefghij");
    ASSERT_FALSE(terminal_get_line_continuation(&t, 1));

    terminal_resize(&t, 5, 2);

    TerminalCell c5;
    ASSERT_EQ(terminal_get_cell(&t, 1, 0, &c5), 0);
    ASSERT_EQ(c5.chars[0], (uint32_t)'f');
    ASSERT_TRUE(terminal_get_line_continuation(&t, 1));

    terminal_destroy(&t);
}

int main(int argc, char *argv[]) {
    test_parse_args(argc, argv);
    printf("Running term_bvt tests:\n");
    RUN_TEST(test_visible_wrap_continuation);
    RUN_TEST(test_scrollback_wrap_continuation);
    RUN_TEST(test_resize_grows_and_reflows);
    RUN_TEST(test_resize_shrinks_and_reflows);
    TEST_SUMMARY();
    return test_fail_count == 0 ? 0 : 1;
}
