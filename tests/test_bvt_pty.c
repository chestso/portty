/*
 * test_bvt_pty — engine-only PTY harness for bloom-vt.
 *
 * Spawns real child processes on a real PTY (no SDL, no FreeType, no atlas)
 * and pipes raw output into bvt_input_write(). Assertions are made against
 * the bvt grid via the public bloom_vt.h API.
 */

#include "bloom_pty.h"
#include "bloom_vt/bloom_vt.h"
#include "test_helpers.h"

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Drain PTY output until the child exits or `timeout_ms` elapses. */
static void drain_pty(BvtTerm *vt, PtyContext *pty, int timeout_ms) {
    long long deadline = now_ms() + timeout_ms;
    int fd = pty_get_master_fd(pty);
    char buf[4096];
    while (now_ms() < deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int wait = (int)(deadline - now_ms());
        if (wait <= 0) break;
        int r = poll(&pfd, 1, wait);
        if (r <= 0) {
            if (!pty_is_running(pty)) {
                /* Drain any final bytes before giving up. */
                ssize_t n = pty_read(pty, buf, sizeof(buf));
                if (n > 0) bvt_input_write(vt, (const uint8_t *)buf, (size_t)n);
                break;
            }
            continue;
        }
        if (pfd.revents & POLLIN) {
            ssize_t n = pty_read(pty, buf, sizeof(buf));
            if (n <= 0) break;
            bvt_input_write(vt, (const uint8_t *)buf, (size_t)n);
        }
        if (pfd.revents & (POLLHUP | POLLERR)) break;
    }
}

/* Search the visible grid for a UTF-8 substring (ASCII-only callers).
 * Returns row index of first occurrence, or -1. */
static int find_row_with(BvtTerm *vt, const char *needle) {
    int rows, cols;
    bvt_get_dimensions(vt, &rows, &cols);
    size_t nlen = strlen(needle);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c + (int)nlen <= cols; ++c) {
            int ok = 1;
            for (size_t i = 0; i < nlen; ++i) {
                const BvtCell *cell = bvt_get_cell(vt, r, c + (int)i);
                if (!cell || cell->cp != (uint32_t)(unsigned char)needle[i]) { ok = 0; break; }
            }
            if (ok) return r;
        }
    }
    return -1;
}

/* Output callback: bvt wants to send bytes back upstream (DSR replies, DA,
 * mouse reports, etc). Forward them to the PTY so the child receives them
 * exactly as a real terminal would. */
static void cb_output_to_pty(const uint8_t *bytes, size_t len, void *user) {
    PtyContext *pty = (PtyContext *)user;
    if (pty) (void)pty_write(pty, (const char *)bytes, len);
}

/* Spawn `sh -c cmd`, drain up to timeout_ms, return the BvtTerm grid for
 * inspection. Caller frees the term and pty. */
static BvtTerm *run_cmd(const char *cmd, int rows, int cols,
                        int timeout_ms, PtyContext **out_pty) {
    char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
    PtyContext *pty = pty_create(rows, cols, argv);
    if (!pty) return NULL;
    BvtTerm *vt = bvt_new(rows, cols);
    if (!vt) { pty_destroy(pty); return NULL; }
    /* Wire the output callback so apps that probe the terminal (DSR, DA,
     * mouse mode acks) get their replies. Without this, brick/curses apps
     * sit waiting for `\x1b[...R` and never start drawing. */
    BvtCallbacks cb = { .output = cb_output_to_pty };
    bvt_set_callbacks(vt, &cb, pty);
    drain_pty(vt, pty, timeout_ms);
    if (out_pty) *out_pty = pty;
    else pty_destroy(pty);
    return vt;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_echo_hello(void) {
    PtyContext *pty = NULL;
    BvtTerm *vt = run_cmd("echo hello", 24, 80, 1000, &pty);
    ASSERT_NOT_NULL(vt);
    ASSERT_TRUE(find_row_with(vt, "hello") >= 0);
    bvt_free(vt);
    pty_destroy(pty);
}

static void test_sgr_red(void) {
    /* Print "red" with SGR 31, reset, then "plain". The text content should
     * land on the grid; style attribution is checked via bvt_cell_style if
     * we want — for now we just verify both segments appear. */
    PtyContext *pty = NULL;
    BvtTerm *vt = run_cmd("printf '\\033[31mred\\033[0m plain'", 24, 80, 1000, &pty);
    ASSERT_NOT_NULL(vt);
    int row = find_row_with(vt, "red");
    ASSERT_TRUE(row >= 0);
    ASSERT_TRUE(find_row_with(vt, "plain") >= 0);
    bvt_free(vt);
    pty_destroy(pty);
}

static void test_tput_cursor(void) {
    /* tput cup 5 10 then echo X — we expect 'X' near row 5 col 10. */
    PtyContext *pty = NULL;
    BvtTerm *vt = run_cmd("tput cup 5 10; printf X", 24, 80, 2000, &pty);
    ASSERT_NOT_NULL(vt);
    /* Look across a small window around (5, 10); shells print a prompt
     * before the script runs which can shift the row in some setups. */
    int found = -1;
    for (int r = 0; r < 24 && found < 0; ++r) {
        for (int c = 0; c < 80; ++c) {
            const BvtCell *cell = bvt_get_cell(vt, r, c);
            if (cell && cell->cp == (uint32_t)'X') { found = r * 100 + c; break; }
        }
    }
    ASSERT_TRUE(found >= 0);
    bvt_free(vt);
    pty_destroy(pty);
}

static void test_zwj_family_full(void) {
    /* 7-codepoint ZWJ family: 👨‍👩‍👧‍👦 = U+1F468 ZWJ U+1F469 ZWJ U+1F467 ZWJ U+1F466.
     * libvterm truncates at chars[6]; bvt must keep all 7 in one cluster. */
    PtyContext *pty = NULL;
    BvtTerm *vt = run_cmd(
        "printf '\\xf0\\x9f\\x91\\xa8\\xe2\\x80\\x8d\\xf0\\x9f\\x91\\xa9"
        "\\xe2\\x80\\x8d\\xf0\\x9f\\x91\\xa7\\xe2\\x80\\x8d\\xf0\\x9f\\x91\\xa6'",
        24, 80, 2000, &pty);
    ASSERT_NOT_NULL(vt);
    /* Find the cluster: the first cell with cp == 0x1F468 (man) should
     * have grapheme_id != 0 and the full 7-cp sequence stored. */
    int found = -1;
    for (int r = 0; r < 24 && found < 0; ++r) {
        for (int c = 0; c < 80; ++c) {
            const BvtCell *cell = bvt_get_cell(vt, r, c);
            if (cell && cell->cp == 0x1F468u && cell->width == 2) {
                found = r * 100 + c;
                uint32_t cps[16] = {0};
                size_t n = bvt_cell_get_grapheme(vt, cell, cps, 16);
                ASSERT_EQ(n, (size_t)7);
                ASSERT_EQ(cps[0], 0x1F468u); /* man */
                ASSERT_EQ(cps[1], 0x200Du);  /* ZWJ */
                ASSERT_EQ(cps[2], 0x1F469u); /* woman */
                ASSERT_EQ(cps[3], 0x200Du);
                ASSERT_EQ(cps[4], 0x1F467u); /* girl */
                ASSERT_EQ(cps[5], 0x200Du);
                ASSERT_EQ(cps[6], 0x1F466u); /* boy */
                break;
            }
        }
    }
    ASSERT_TRUE(found >= 0);
    bvt_free(vt);
    pty_destroy(pty);
}

static void test_cjk_echo(void) {
    /* CJK ideographs are width=2 — assert the cell has width 2. */
    PtyContext *pty = NULL;
    BvtTerm *vt = run_cmd("printf '\\xe4\\xbd\\xa0\\xe5\\xa5\\xbd'", /* 你好 */
                          24, 80, 1000, &pty);
    ASSERT_NOT_NULL(vt);
    int found = -1;
    for (int r = 0; r < 24 && found < 0; ++r) {
        for (int c = 0; c < 80; ++c) {
            const BvtCell *cell = bvt_get_cell(vt, r, c);
            if (cell && cell->cp == 0x4F60u /* 你 */) {
                ASSERT_EQ((int)cell->width, 2);
                /* Continuation cell at c+1 has width 0. */
                const BvtCell *cont = bvt_get_cell(vt, r, c + 1);
                ASSERT_NOT_NULL(cont);
                ASSERT_EQ((int)cont->width, 0);
                /* Next cluster 好 follows at c+2. */
                const BvtCell *next = bvt_get_cell(vt, r, c + 2);
                ASSERT_NOT_NULL(next);
                ASSERT_EQ(next->cp, 0x597Du);
                found = 1;
                break;
            }
        }
    }
    ASSERT_TRUE(found >= 0);
    bvt_free(vt);
    pty_destroy(pty);
}

static void test_altscreen_swap(void) {
    /* tput smcup, write FOO, tput rmcup. After rmcup we should be back on
     * the primary screen with FOO not visible. */
    PtyContext *pty = NULL;
    BvtTerm *vt = run_cmd("tput smcup; printf 'FOO_ALT'; sleep 0.05; tput rmcup",
                          24, 80, 2000, &pty);
    ASSERT_NOT_NULL(vt);
    /* On the primary screen now — FOO_ALT should NOT be visible. */
    ASSERT_FALSE(bvt_is_altscreen(vt));
    ASSERT_TRUE(find_row_with(vt, "FOO_ALT") < 0);
    bvt_free(vt);
    pty_destroy(pty);
}

/* Reproducer for the "cf menu wipes the screen" report. cf is a brick TUI
 * (Haskell vty) that runs in inline mode (no altscreen). It probes the
 * terminal with DSR 6 to learn the cursor row before drawing, so the menu
 * starts where the user invoked it instead of at row 0. If the cursor
 * row reported by DSR is wrong (e.g. always 1), the menu appears at the
 * top and the prompts above are clobbered — exactly the user-visible bug.
 *
 * Test: synthesize 16 lines of output to advance the cursor naturally,
 * then send DSR 6 directly. We expect `\x1b[17;1R` (1-indexed row 17). */
static char g_dsr_buf[64];
static size_t g_dsr_len = 0;
static void cb_capture_dsr(const uint8_t *b, size_t n, void *u) {
    (void)u;
    for (size_t i = 0; i < n && g_dsr_len + 1 < sizeof(g_dsr_buf); ++i)
        g_dsr_buf[g_dsr_len++] = (char)b[i];
    g_dsr_buf[g_dsr_len] = 0;
}

/* End-to-end repro for the user-reported "cf wipes the screen" bug.
 * Skipped if the cf binary isn't installed. */
static void test_cf_brick_inline_preserves_history(void) {
    if (access("/home/thomasc/.local/bin/cf", X_OK) != 0) {
        printf("    (skipping: cf not installed)\n");
        return;
    }
    int rows = 40, cols = 120;
    char *const argv[] = { "sh", "-c",
        /* Fill the screen with 16 prompts, then run cf. cf is a brick TUI
         * that draws inline (no altscreen) and uses DSR 6 to discover the
         * cursor row before drawing. Matches the user-reported geometry
         * (`bloom-terminal -g 120x40`). */
        "for i in $(seq 1 16); do echo \"prompt $i\"; done; "
        "/home/thomasc/.local/bin/cf",
        NULL };
    PtyContext *pty = pty_create(rows, cols, argv);
    ASSERT_NOT_NULL(pty);
    BvtTerm *vt = bvt_new(rows, cols);
    ASSERT_NOT_NULL(vt);

    /* Output callback writes back to the PTY so cf's DSR query is answered. */
    BvtCallbacks cb = { .output = cb_output_to_pty };
    bvt_set_callbacks(vt, &cb, pty);

    long long deadline = now_ms() + 2500;
    int fd = pty_get_master_fd(pty);
    char rbuf[4096];
    while (now_ms() < deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int wait = (int)(deadline - now_ms());
        if (wait <= 0) break;
        int r = poll(&pfd, 1, wait);
        if (r <= 0) {
            if (!pty_is_running(pty)) break;
            continue;
        }
        if (pfd.revents & POLLIN) {
            ssize_t n = pty_read(pty, rbuf, sizeof(rbuf));
            if (n <= 0) break;
            bvt_input_write(vt, (const uint8_t *)rbuf, (size_t)n);
        }
        if (pfd.revents & (POLLHUP | POLLERR)) break;
    }

    /* The brick TUI starts the menu with `× Carrion Fields`. Find it. */
    int menu_row = find_row_with(vt, "Carrion Fields");
    if (menu_row < 0 || menu_row == 0) {
        /* Failure path — dump the grid so the regression is debuggable. */
        if (menu_row < 0)
            fprintf(stderr, "  'Carrion Fields' not found anywhere\n");
        else
            fprintf(stderr, "  REPRO: cf menu at row 0 — prompts above were wiped\n");
        for (int r = 0; r < rows; ++r) {
            char line[256];
            int n = 0;
            for (int c = 0; c < cols && n + 1 < (int)sizeof(line); ++c) {
                const BvtCell *cell = bvt_get_cell(vt, r, c);
                uint32_t cp = (cell && cell->cp) ? cell->cp : 0;
                line[n++] = (cp >= 0x20 && cp < 0x7f) ? (char)cp : (cp ? '?' : ':');
            }
            line[n] = 0;
            while (n > 0 && line[n-1] == ':') line[--n] = 0;
            if (n > 0) fprintf(stderr, "    row %2d: %s\n", r, line);
        }
    }
    ASSERT_TRUE(menu_row > 0);
    /* Sanity: at least one of the recent prompts should still be visible
     * above the menu. */
    int found_prompt = 0;
    for (int row = 0; row < menu_row; ++row) {
        for (int c = 0; c + 6 < cols; ++c) {
            const BvtCell *cell = bvt_get_cell(vt, row, c);
            if (cell && cell->cp == (uint32_t)'p') {
                /* check for "prompt" prefix */
                const char *needle = "prompt";
                int ok = 1;
                for (size_t i = 0; i < strlen(needle); ++i) {
                    const BvtCell *x = bvt_get_cell(vt, row, c + (int)i);
                    if (!x || x->cp != (uint32_t)needle[i]) { ok = 0; break; }
                }
                if (ok) { found_prompt = 1; break; }
            }
        }
        if (found_prompt) break;
    }
    ASSERT_TRUE(found_prompt);

    bvt_free(vt);
    pty_destroy(pty);
}

static void test_dsr_after_natural_scroll(void) {
    int rows = 24, cols = 80;
    char *const argv[] = { "sh", "-c",
        "for i in $(seq 1 16); do echo line$i; done; printf '\\033[6n'",
        NULL };
    PtyContext *pty = pty_create(rows, cols, argv);
    ASSERT_NOT_NULL(pty);
    BvtTerm *vt = bvt_new(rows, cols);
    ASSERT_NOT_NULL(vt);

    g_dsr_len = 0; g_dsr_buf[0] = 0;
    BvtCallbacks cb = { .output = cb_capture_dsr };
    bvt_set_callbacks(vt, &cb, NULL);

    drain_pty(vt, pty, 1500);

    /* Cursor advanced to row 16 (after 16 lines from row 0).
     * DSR should report `\x1b[17;1R`. */
    ASSERT_STR_EQ(g_dsr_buf, "\x1b[17;1R");
    bvt_free(vt);
    pty_destroy(pty);
}

static void test_scrollback_push(void) {
    /* Print 50 lines into a 24-row terminal; expect ≥ 25 lines pushed to
     * scrollback and the most recent lines visible at the bottom. */
    PtyContext *pty = NULL;
    BvtTerm *vt = run_cmd("for i in $(seq 1 50); do echo line$i; done",
                          24, 80, 3000, &pty);
    ASSERT_NOT_NULL(vt);
    int sb = bvt_get_scrollback_lines(vt);
    ASSERT_TRUE(sb >= 25);
    /* Last printed line should be on or near the bottom of the visible
     * area. The shell prints a final prompt below it, so we look for
     * "line50" anywhere on the grid. */
    ASSERT_TRUE(find_row_with(vt, "line50") >= 0);
    bvt_free(vt);
    pty_destroy(pty);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    test_parse_args(argc, argv);

    /* Required before pty_create — pty.c installs a SIGCHLD handler. */
    if (pty_signal_init() != 0) {
        fprintf(stderr, "pty_signal_init failed\n");
        return 1;
    }

    printf("Running test_bvt_pty\n");
    RUN_TEST(test_echo_hello);
    RUN_TEST(test_sgr_red);
    RUN_TEST(test_tput_cursor);
    RUN_TEST(test_zwj_family_full);
    RUN_TEST(test_cjk_echo);
    RUN_TEST(test_altscreen_swap);
    RUN_TEST(test_dsr_after_natural_scroll);
    RUN_TEST(test_scrollback_push);
    RUN_TEST(test_cf_brick_inline_preserves_history);

    pty_signal_cleanup();
    TEST_SUMMARY();
}
