/*
 * test_osc52 — covers the OSC 52 (clipboard set) wiring through coffer
 * into TerminalBackend.clipboard_set_cb, plus the base64 decoder it sits
 * on top of. Query form ('?') must be silently refused; malformed payloads
 * must not crash or fire the callback.
 */

#include "test_helpers.h"

#include "base64.h"
#include "term.h"
#include "term_cfr.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern TerminalBackend terminal_backend_cfr;

typedef struct
{
    int call_count;
    char last[256];
    size_t last_len;
} ClipboardCapture;

static void capture_set(const char *text, size_t len, void *user)
{
    ClipboardCapture *c = user;
    c->call_count++;
    c->last_len = len < sizeof(c->last) - 1 ? len : sizeof(c->last) - 1;
    if (c->last_len)
        memcpy(c->last, text, c->last_len);
    c->last[c->last_len] = '\0';
}

static void feed(TerminalBackend *t, const void *bytes, size_t n)
{
    terminal_process_input(t, (const char *)bytes, n);
}

/* ------------------------------------------------------------------ */
/* base64 decoder                                                      */
/* ------------------------------------------------------------------ */

static void test_base64_rfc4648_vectors(void)
{
    struct
    {
        const char *in;
        const char *out;
    } cases[] = {
        { "", "" },
        { "Zg==", "f" },
        { "Zm8=", "fo" },
        { "Zm9v", "foo" },
        { "Zm9vYg==", "foob" },
        { "Zm9vYmE=", "fooba" },
        { "Zm9vYmFy", "foobar" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t out_len = 0;
        uint8_t *out = base64_decode(cases[i].in, strlen(cases[i].in), &out_len);
        ASSERT_NOT_NULL(out);
        ASSERT_EQ(out_len, strlen(cases[i].out));
        ASSERT_TRUE(memcmp(out, cases[i].out, out_len) == 0);
        free(out);
    }
}

static void test_base64_missing_padding(void)
{
    /* Some OSC 52 producers omit '=' padding; we accept it. */
    size_t n = 0;
    uint8_t *out = base64_decode("Zm9vYg", 6, &n);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ(n, (size_t)4);
    ASSERT_TRUE(memcmp(out, "foob", 4) == 0);
    free(out);
}

static void test_base64_whitespace_tolerated(void)
{
    size_t n = 0;
    uint8_t *out = base64_decode("Zm9v\nYmFy\n", 10, &n);
    ASSERT_NOT_NULL(out);
    ASSERT_EQ(n, (size_t)6);
    ASSERT_TRUE(memcmp(out, "foobar", 6) == 0);
    free(out);
}

static void test_base64_invalid_char_rejected(void)
{
    size_t n = 0;
    uint8_t *out = base64_decode("Zm9v!Ymar", 9, &n);
    ASSERT_NULL(out);
}

/* ------------------------------------------------------------------ */
/* OSC 52 dispatch                                                     */
/* ------------------------------------------------------------------ */

static void install(TerminalBackend *t, ClipboardCapture *c)
{
    memset(c, 0, sizeof(*c));
    terminal_set_clipboard_set_callback(t, capture_set, c);
}

/* "Hello" base64-encoded, BEL-terminated. */
static void test_osc52_set_clipboard_bel(void)
{
    TerminalBackend t = terminal_backend_cfr;
    {
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = 20;
        cfg.rows = 4;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 20;
        ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
    };
    ClipboardCapture cap;
    install(&t, &cap);

    const char seq[] = "\x1b]52;c;SGVsbG8=\x07";
    feed(&t, seq, sizeof(seq) - 1);

    ASSERT_EQ(cap.call_count, 1);
    ASSERT_EQ(cap.last_len, (size_t)5);
    ASSERT_STR_EQ(cap.last, "Hello");

    terminal_destroy(&t);
}

/* ESC \ (ST) terminator should work the same as BEL. */
static void test_osc52_set_clipboard_st(void)
{
    TerminalBackend t = terminal_backend_cfr;
    {
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = 20;
        cfg.rows = 4;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 20;
        ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
    };
    ClipboardCapture cap;
    install(&t, &cap);

    const char seq[] = "\x1b]52;c;d29ybGQ=\x1b\\";
    feed(&t, seq, sizeof(seq) - 1);

    ASSERT_EQ(cap.call_count, 1);
    ASSERT_STR_EQ(cap.last, "world");

    terminal_destroy(&t);
}

/* All selection variants route to the same one OS clipboard. */
static void test_osc52_selection_variants(void)
{
    const char *prefixes[] = { "c", "p", "s", "cs", "" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        TerminalBackend t = terminal_backend_cfr;
        {
            CfrConfig cfg = CFR_CONFIG_DEFAULTS;
            cfg.cols = 20;
            cfg.rows = 4;
            cfg.cell_w_px = 10;
            cfg.cell_h_px = 20;
            ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
        };
        ClipboardCapture cap;
        install(&t, &cap);

        char seq[64];
        int n = snprintf(seq, sizeof(seq), "\x1b]52;%s;aGk=\x07", prefixes[i]);
        feed(&t, seq, (size_t)n);

        ASSERT_EQ(cap.call_count, 1);
        ASSERT_STR_EQ(cap.last, "hi");

        terminal_destroy(&t);
    }
}

/* '?' query form must NOT fire the callback (privacy default). */
static void test_osc52_query_refused(void)
{
    TerminalBackend t = terminal_backend_cfr;
    {
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = 20;
        cfg.rows = 4;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 20;
        ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
    };
    ClipboardCapture cap;
    install(&t, &cap);

    const char seq[] = "\x1b]52;c;?\x07";
    feed(&t, seq, sizeof(seq) - 1);

    ASSERT_EQ(cap.call_count, 0);

    terminal_destroy(&t);
}

/* Malformed base64: callback must not fire and we must not crash. */
static void test_osc52_malformed_b64(void)
{
    TerminalBackend t = terminal_backend_cfr;
    {
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = 20;
        cfg.rows = 4;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 20;
        ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
    };
    ClipboardCapture cap;
    install(&t, &cap);

    /* '!' is not in the base64 alphabet. */
    const char seq[] = "\x1b]52;c;SGVs!G8=\x07";
    feed(&t, seq, sizeof(seq) - 1);

    ASSERT_EQ(cap.call_count, 0);

    terminal_destroy(&t);
}

/* Missing semicolon between selection and payload — bvt may still hand the
 * body to us; the dispatcher must skip it without firing the callback. */
static void test_osc52_no_semicolon(void)
{
    TerminalBackend t = terminal_backend_cfr;
    {
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = 20;
        cfg.rows = 4;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 20;
        ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
    };
    ClipboardCapture cap;
    install(&t, &cap);

    const char seq[] = "\x1b]52;c\x07";
    feed(&t, seq, sizeof(seq) - 1);

    ASSERT_EQ(cap.call_count, 0);

    terminal_destroy(&t);
}

/* Empty payload after the selection-list semicolon: clear clipboard. */
static void test_osc52_empty_payload_clears(void)
{
    TerminalBackend t = terminal_backend_cfr;
    {
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = 20;
        cfg.rows = 4;
        cfg.cell_w_px = 10;
        cfg.cell_h_px = 20;
        ASSERT_TRUE(terminal_init(&t, &cfg) != NULL);
    };
    ClipboardCapture cap;
    install(&t, &cap);

    const char seq[] = "\x1b]52;c;\x07";
    feed(&t, seq, sizeof(seq) - 1);

    ASSERT_EQ(cap.call_count, 1);
    ASSERT_EQ(cap.last_len, (size_t)0);

    terminal_destroy(&t);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_base64_rfc4648_vectors);
    RUN_TEST(test_base64_missing_padding);
    RUN_TEST(test_base64_whitespace_tolerated);
    RUN_TEST(test_base64_invalid_char_rejected);

    RUN_TEST(test_osc52_set_clipboard_bel);
    RUN_TEST(test_osc52_set_clipboard_st);
    RUN_TEST(test_osc52_selection_variants);
    RUN_TEST(test_osc52_query_refused);
    RUN_TEST(test_osc52_malformed_b64);
    RUN_TEST(test_osc52_no_semicolon);
    RUN_TEST(test_osc52_empty_payload_clears);

    TEST_SUMMARY();
}
