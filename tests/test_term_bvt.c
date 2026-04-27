/*
 * test_term_bvt — covers the TerminalBackend bridge for bloom-vt.
 *
 * Two regressions motivated this file:
 *   1. Wrap-aware selection (double-click on a soft-wrapped word) reads
 *      `terminal_get_line_continuation` in unified row coordinates, so the
 *      backend has to translate negative rows to scrollback lookups AND
 *      flip bvt's "this row wraps into the next" flag into libvterm's
 *      "this row continues from the previous" semantic.
 *   2. Resize must re-wrap content — bvt's reflow path is always on.
 */

#include "test_helpers.h"

#include "term.h"
#include "term_bvt.h"

#include <stdlib.h>
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
    ASSERT_EQ(c0.cp, (uint32_t)'a');
    ASSERT_EQ(c5.cp, (uint32_t)'f');
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
    ASSERT_EQ(c5.cp, (uint32_t)'f');
    ASSERT_TRUE(terminal_get_line_continuation(&t, 1));

    terminal_destroy(&t);
}

/* The 7-codepoint ZWJ family 👨‍👩‍👧‍👦 used to truncate at the renderer
 * boundary because TerminalCell stored at most 6 codepoints inline. After
 * step 17, the cell carries (cp, grapheme_id) and the caller fetches the
 * full sequence via terminal_cell_get_grapheme. Verify all 7 cps survive. */
static void test_long_cluster_survives_accessor(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 20, 2) != NULL);
    /* U+1F468 ZWJ U+1F469 ZWJ U+1F467 ZWJ U+1F466 — 7 codepoints. */
    feed(&t,
         "\xf0\x9f\x91\xa8\xe2\x80\x8d"
         "\xf0\x9f\x91\xa9\xe2\x80\x8d"
         "\xf0\x9f\x91\xa7\xe2\x80\x8d"
         "\xf0\x9f\x91\xa6");

    TerminalCell cell;
    ASSERT_EQ(terminal_get_cell(&t, 0, 0, &cell), 0);
    ASSERT_EQ(cell.cp, (uint32_t)0x1F468);
    ASSERT_TRUE(cell.grapheme_id != 0);

    uint32_t cps[16];
    size_t n = terminal_cell_get_grapheme(&t, 0, 0, cps, 16);
    ASSERT_EQ(n, (size_t)7);
    ASSERT_EQ(cps[0], (uint32_t)0x1F468);
    ASSERT_EQ(cps[1], (uint32_t)0x200D);
    ASSERT_EQ(cps[2], (uint32_t)0x1F469);
    ASSERT_EQ(cps[3], (uint32_t)0x200D);
    ASSERT_EQ(cps[4], (uint32_t)0x1F467);
    ASSERT_EQ(cps[5], (uint32_t)0x200D);
    ASSERT_EQ(cps[6], (uint32_t)0x1F466);

    terminal_destroy(&t);
}

/* Spaces between words must round-trip through the clipboard. Typed spaces
 * land as cp=0x20, width=1 cells and were always preserved — this guards
 * the simple case while the harder cases (TAB, CUF) sit below. */
static void test_selection_preserves_spaces(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 20, 2) != NULL);
    feed(&t, "hello world");

    terminal_selection_start(&t, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(&t, 0, 10);

    char *text = terminal_selection_get_text(&t);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "hello world");
    free(text);

    terminal_destroy(&t);
}

static void test_selection_across_rows(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 20, 4) != NULL);
    feed(&t, "hello world\r\nfoo bar\r\nbaz");

    terminal_selection_start(&t, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(&t, 2, 2);
    char *text = terminal_selection_get_text(&t);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "hello world\nfoo bar\nbaz");
    free(text);

    terminal_destroy(&t);
}

/* TAB advances the cursor by 8 columns without writing anything; the cells
 * between the cursor's old and new positions stay cleared (cp=0, width=0).
 * The selection-extraction logic must treat those as spaces, not skip them. */
static void test_selection_with_tab(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 40, 2) != NULL);
    feed(&t, "foo\tbar");

    terminal_selection_start(&t, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(&t, 0, 10);
    char *text = terminal_selection_get_text(&t);
    ASSERT_NOT_NULL(text);
    /* TAB stops at column 8 → "foo" + 5 spaces + "bar". */
    ASSERT_STR_EQ(text, "foo     bar");
    free(text);

    terminal_destroy(&t);
}

/* CSI <n> C (CUF) is a common way for shells/applications to position
 * content with gaps; the gap cells must round-trip as spaces. */
static void test_selection_after_cursor_movement(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 20, 2) != NULL);
    feed(&t, "foo\x1b[5Cbar");

    terminal_selection_start(&t, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(&t, 0, 10);
    char *text = terminal_selection_get_text(&t);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "foo     bar");
    free(text);

    terminal_destroy(&t);
}

/* Wide CJK chars must not have their right-half continuation cell turn into
 * a space — the width-2 advance should skip past it. */
static void test_selection_skips_wide_continuation(void) {
    TerminalBackend t = terminal_backend_bvt;
    ASSERT_TRUE(terminal_init(&t, 20, 2) != NULL);
    /* "中x" — '中' is a 2-cell CJK char (U+4E2D, UTF-8 e4 b8 ad). */
    feed(&t, "\xe4\xb8\xad" "x");

    terminal_selection_start(&t, 0, 0, TERM_SELECT_CHAR);
    terminal_selection_update(&t, 0, 5);
    char *text = terminal_selection_get_text(&t);
    ASSERT_NOT_NULL(text);
    ASSERT_STR_EQ(text, "\xe4\xb8\xad" "x");
    free(text);

    terminal_destroy(&t);
}

int main(int argc, char *argv[]) {
    test_parse_args(argc, argv);
    printf("Running term_bvt tests:\n");
    RUN_TEST(test_visible_wrap_continuation);
    RUN_TEST(test_scrollback_wrap_continuation);
    RUN_TEST(test_resize_grows_and_reflows);
    RUN_TEST(test_resize_shrinks_and_reflows);
    RUN_TEST(test_long_cluster_survives_accessor);
    RUN_TEST(test_selection_preserves_spaces);
    RUN_TEST(test_selection_across_rows);
    RUN_TEST(test_selection_with_tab);
    RUN_TEST(test_selection_after_cursor_movement);
    RUN_TEST(test_selection_skips_wide_continuation);
    TEST_SUMMARY();
    return test_fail_count == 0 ? 0 : 1;
}
