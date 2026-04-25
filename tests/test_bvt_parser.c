/* tests/test_bvt_parser.c — bloom-vt parser smoke tests */

#include "test_helpers.h"
#include "../src/bloom_vt/bloom_vt.h"
#include "../src/bloom_vt/bloom_vt_internal.h"

#include <stdio.h>
#include <string.h>

static const char *g_title = NULL;
static int g_bell_count = 0;
static char g_output_buf[1024];
static size_t g_output_len = 0;

static void on_title(const char *s, void *u) { (void)u; g_title = s; }
static void on_bell(void *u) { (void)u; g_bell_count++; }
static void on_output(const uint8_t *bytes, size_t len, void *u) {
    (void)u;
    if (g_output_len + len >= sizeof(g_output_buf)) return;
    memcpy(g_output_buf + g_output_len, bytes, len);
    g_output_len += len;
    g_output_buf[g_output_len] = '\0';
}

static BvtTerm *make_term(int rows, int cols) {
    BvtTerm *vt = bvt_new(rows, cols);
    BvtCallbacks cb = { 0 };
    cb.set_title = on_title;
    cb.bell      = on_bell;
    cb.output    = on_output;
    bvt_set_callbacks(vt, &cb, NULL);
    g_output_len = 0;
    g_output_buf[0] = '\0';
    return vt;
}

static void feed(BvtTerm *vt, const char *s) {
    bvt_input_write(vt, (const uint8_t *)s, strlen(s));
}

/* ------------------------------------------------------------------ */

static void test_print_ascii(void) {
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "Hello");

    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'H');
    ASSERT_EQ(c->width, 1);

    c = bvt_get_cell(vt, 0, 4);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'o');

    BvtCursor cur = bvt_get_cursor(vt);
    ASSERT_EQ(cur.row, 0);
    ASSERT_EQ(cur.col, 5);
    bvt_free(vt);
}

static void test_lf_cr(void) {
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "ab\r\ncd");

    const BvtCell *a = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(a->cp, (uint32_t)'a');
    const BvtCell *c = bvt_get_cell(vt, 1, 0);
    ASSERT_EQ(c->cp, (uint32_t)'c');
    const BvtCell *d = bvt_get_cell(vt, 1, 1);
    ASSERT_EQ(d->cp, (uint32_t)'d');

    BvtCursor cur = bvt_get_cursor(vt);
    ASSERT_EQ(cur.row, 1);
    ASSERT_EQ(cur.col, 2);
    bvt_free(vt);
}

static void test_csi_cup(void) {
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[5;10HX");
    const BvtCell *c = bvt_get_cell(vt, 4, 9);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, (uint32_t)'X');
    bvt_free(vt);
}

static void test_csi_erase_line(void) {
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "abcde\x1b[3G\x1b[K");
    const BvtCell *a = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(a->cp, (uint32_t)'a');
    const BvtCell *b = bvt_get_cell(vt, 0, 1);
    ASSERT_EQ(b->cp, (uint32_t)'b');
    const BvtCell *c = bvt_get_cell(vt, 0, 2);
    ASSERT_EQ(c->cp, 0u); /* erased */
    const BvtCell *d = bvt_get_cell(vt, 0, 3);
    ASSERT_EQ(d->cp, 0u);
    bvt_free(vt);
}

static void test_osc_title_bel(void) {
    BvtTerm *vt = make_term(24, 80);
    g_title = NULL;
    feed(vt, "\x1b]0;hello world\x07");
    ASSERT_NOT_NULL(g_title);
    ASSERT_STR_EQ(g_title, "hello world");
    bvt_free(vt);
}

static void test_osc_title_st(void) {
    BvtTerm *vt = make_term(24, 80);
    g_title = NULL;
    feed(vt, "\x1b]2;st-form\x1b\\");
    ASSERT_NOT_NULL(g_title);
    ASSERT_STR_EQ(g_title, "st-form");
    bvt_free(vt);
}

static void test_utf8_basic(void) {
    BvtTerm *vt = make_term(24, 80);
    /* "héllo" — é is U+00E9 → 0xc3 0xa9 */
    feed(vt, "h\xc3\xa9llo");
    const BvtCell *c = bvt_get_cell(vt, 0, 1);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->cp, 0x00E9u);
    /* The 'l' that follows should be at col 2, not 3 — UTF-8 multi-byte
     * collapses to one cell. */
    c = bvt_get_cell(vt, 0, 2);
    ASSERT_EQ(c->cp, (uint32_t)'l');
    bvt_free(vt);
}

static void test_utf8_replacement(void) {
    BvtTerm *vt = make_term(24, 80);
    /* Bare 0x80 is invalid UTF-8 → should produce U+FFFD. */
    feed(vt, "\x80x");
    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(c->cp, 0xFFFDu);
    c = bvt_get_cell(vt, 0, 1);
    ASSERT_EQ(c->cp, (uint32_t)'x');
    bvt_free(vt);
}

static void test_csi_sgr_reset(void) {
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[1mX\x1b[0mY");
    /* Both cells should exist; once SGR is fully wired we'll assert
     * pen attrs. For now: cells render, parser doesn't crash. */
    ASSERT_NOT_NULL(bvt_get_cell(vt, 0, 0));
    ASSERT_NOT_NULL(bvt_get_cell(vt, 0, 1));
    bvt_free(vt);
}

static void test_bel(void) {
    BvtTerm *vt = make_term(24, 80);
    g_bell_count = 0;
    feed(vt, "\x07\x07X");
    ASSERT_EQ(g_bell_count, 2);
    const BvtCell *c = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(c->cp, (uint32_t)'X');
    bvt_free(vt);
}

static void test_style_intern_dedup(void) {
    BvtTerm *vt = make_term(24, 80);
    /* Two identical bold runs should share a style id. Then a non-bold
     * "Y" picks up the default. */
    feed(vt, "\x1b[1mA\x1b[0mB\x1b[1mC");

    const BvtCell *a = bvt_get_cell(vt, 0, 0);
    const BvtCell *b = bvt_get_cell(vt, 0, 1);
    const BvtCell *c = bvt_get_cell(vt, 0, 2);
    ASSERT_EQ(a->cp, (uint32_t)'A');
    ASSERT_EQ(b->cp, (uint32_t)'B');
    ASSERT_EQ(c->cp, (uint32_t)'C');

    /* A and C share a style id (both bold). */
    ASSERT_EQ(a->style_id, c->style_id);
    /* A and B differ (bold vs default). */
    ASSERT_NEQ(a->style_id, b->style_id);
    /* B is the default style → id 0. */
    ASSERT_EQ(b->style_id, 0u);

    const BvtStyle *as = bvt_cell_style(vt, a);
    ASSERT_NOT_NULL(as);
    ASSERT_TRUE((as->attrs & BVT_ATTR_BOLD) != 0);
    bvt_free(vt);
}

static void test_grapheme_arena_dedup(void) {
    /* Direct-call the arena rather than going through the parser —
     * proper grapheme assembly arrives once UAX #29 tables ship. */
    BvtTerm *vt = make_term(24, 80);
    /* Force grid creation by writing one char, then poke the arena
     * directly via the internal header. */
    feed(vt, "x");
    BvtPage *page = vt->grid;
    ASSERT_NOT_NULL(page);

    uint32_t flag[] = { 0x1F1E9, 0x1F1F0 }; /* DK flag */
    uint32_t id1 = bvt_grapheme_intern(vt, page, flag, 2);
    uint32_t id2 = bvt_grapheme_intern(vt, page, flag, 2);
    ASSERT_NEQ(id1, 0u);
    ASSERT_EQ(id1, id2);

    uint32_t fam[] = { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466 };
    uint32_t id3 = bvt_grapheme_intern(vt, page, fam, 7);
    ASSERT_NEQ(id3, 0u);
    ASSERT_NEQ(id1, id3);

    uint32_t out[8] = { 0 };
    size_t n = bvt_grapheme_read(page, id3, out, 8);
    ASSERT_EQ(n, 7u);
    ASSERT_EQ(out[0], 0x1F468u);
    ASSERT_EQ(out[2], 0x1F469u);
    ASSERT_EQ(out[6], 0x1F466u);

    bvt_free(vt);
}

static void test_width_cjk(void) {
    BvtTerm *vt = make_term(24, 80);
    /* "あ" (U+3042) is UAX #11 Wide → width 2 */
    feed(vt, "\xE3\x81\x82" "x");
    const BvtCell *a = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(a->cp, 0x3042u);
    ASSERT_EQ(a->width, 2);
    /* Continuation cell at col 1 has width 0. */
    const BvtCell *cont = bvt_get_cell(vt, 0, 1);
    ASSERT_EQ(cont->width, 0);
    /* The 'x' lands at col 2, not col 1. */
    const BvtCell *x = bvt_get_cell(vt, 0, 2);
    ASSERT_EQ(x->cp, (uint32_t)'x');
    bvt_free(vt);
}

static void test_width_emoji(void) {
    BvtTerm *vt = make_term(24, 80);
    /* "🎉" U+1F389 — emoji presentation, width 2. */
    feed(vt, "\xF0\x9F\x8E\x89" "y");
    const BvtCell *e = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(e->cp, 0x1F389u);
    ASSERT_EQ(e->width, 2);
    const BvtCell *y = bvt_get_cell(vt, 0, 2);
    ASSERT_EQ(y->cp, (uint32_t)'y');
    bvt_free(vt);
}

static void test_vs16_widens_ambiguous(void) {
    BvtTerm *vt = make_term(24, 80);
    /* Warning sign: U+26A0 alone is width 1 (Ambiguous, default narrow);
     * U+26A0 + U+FE0F (VS16) becomes width 2 — this is the bug
     * libvterm cannot solve without our shift hack. */
    feed(vt, "\xE2\x9A\xA0" "x");
    const BvtCell *plain = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(plain->cp, 0x26A0u);
    ASSERT_EQ(plain->width, 1);
    const BvtCell *next = bvt_get_cell(vt, 0, 1);
    ASSERT_EQ(next->cp, (uint32_t)'x');

    /* Reset and try with VS16. */
    bvt_free(vt);
    vt = make_term(24, 80);
    feed(vt, "\xE2\x9A\xA0\xEF\xB8\x8F" "x"); /* ⚠ + VS16 + x */
    const BvtCell *vs = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(vs->cp, 0x26A0u);
    ASSERT_EQ(vs->width, 2);                   /* widened */
    ASSERT_NEQ(vs->grapheme_id, 0u);           /* multi-cp cluster */
    const BvtCell *cont = bvt_get_cell(vt, 0, 1);
    ASSERT_EQ(cont->width, 0);                 /* continuation */
    const BvtCell *xc = bvt_get_cell(vt, 0, 2);
    ASSERT_EQ(xc->cp, (uint32_t)'x');
    /* Verify the cluster stored both codepoints. */
    uint32_t cps[8] = { 0 };
    size_t n = bvt_cell_get_grapheme(vt, vs, cps, 8);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(cps[0], 0x26A0u);
    ASSERT_EQ(cps[1], 0xFE0Fu);
    bvt_free(vt);
}

static void test_combining_mark(void) {
    BvtTerm *vt = make_term(24, 80);
    /* "é" as e + combining acute (U+0301) → one cell, width 1, with
     * cluster of 2 codepoints. */
    feed(vt, "e\xCC\x81" "f");
    const BvtCell *eacute = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(eacute->cp, (uint32_t)'e');
    ASSERT_EQ(eacute->width, 1);
    ASSERT_NEQ(eacute->grapheme_id, 0u);
    /* 'f' lands at col 1 (combining marks don't advance the cursor). */
    const BvtCell *f = bvt_get_cell(vt, 0, 1);
    ASSERT_EQ(f->cp, (uint32_t)'f');

    uint32_t cps[4] = { 0 };
    size_t n = bvt_cell_get_grapheme(vt, eacute, cps, 4);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(cps[0], (uint32_t)'e');
    ASSERT_EQ(cps[1], 0x0301u);
    bvt_free(vt);
}

static void test_regional_indicator_pair(void) {
    BvtTerm *vt = make_term(24, 80);
    /* 🇩🇰 = U+1F1E9 + U+1F1F0 — Danish flag, width 2. */
    feed(vt, "\xF0\x9F\x87\xA9\xF0\x9F\x87\xB0" "z");
    const BvtCell *flag = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(flag->cp, 0x1F1E9u);
    ASSERT_EQ(flag->width, 2);
    ASSERT_NEQ(flag->grapheme_id, 0u);
    const BvtCell *cont = bvt_get_cell(vt, 0, 1);
    ASSERT_EQ(cont->width, 0);
    const BvtCell *z = bvt_get_cell(vt, 0, 2);
    ASSERT_EQ(z->cp, (uint32_t)'z');
    bvt_free(vt);
}

static void test_zwj_family(void) {
    BvtTerm *vt = make_term(24, 80);
    /* 👨‍👩‍👧‍👦 — man + ZWJ + woman + ZWJ + girl + ZWJ + boy. Single
     * grapheme cluster of 7 codepoints, width 2. This is the case
     * libvterm can't represent: chars[6] caps at 6 codepoints. */
    feed(vt, "\xF0\x9F\x91\xA8" "\xE2\x80\x8D" "\xF0\x9F\x91\xA9"
             "\xE2\x80\x8D" "\xF0\x9F\x91\xA7" "\xE2\x80\x8D"
             "\xF0\x9F\x91\xA6" "Q");
    const BvtCell *fam = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(fam->cp, 0x1F468u);
    ASSERT_EQ(fam->width, 2);
    uint32_t cps[16] = { 0 };
    size_t n = bvt_cell_get_grapheme(vt, fam, cps, 16);
    ASSERT_EQ(n, 7u);
    ASSERT_EQ(cps[0], 0x1F468u);
    ASSERT_EQ(cps[1], 0x200Du);
    ASSERT_EQ(cps[6], 0x1F466u);
    /* 'Q' lands at col 2. */
    const BvtCell *q = bvt_get_cell(vt, 0, 2);
    ASSERT_EQ(q->cp, (uint32_t)'Q');
    bvt_free(vt);
}

static void test_skin_tone(void) {
    BvtTerm *vt = make_term(24, 80);
    /* 👋🏽 = waving hand + medium skin tone modifier. */
    feed(vt, "\xF0\x9F\x91\x8B" "\xF0\x9F\x8F\xBD" "k");
    const BvtCell *wave = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(wave->cp, 0x1F44Bu);
    ASSERT_EQ(wave->width, 2);
    uint32_t cps[4] = { 0 };
    size_t n = bvt_cell_get_grapheme(vt, wave, cps, 4);
    ASSERT_EQ(n, 2u);
    ASSERT_EQ(cps[1], 0x1F3FDu);
    const BvtCell *k = bvt_get_cell(vt, 0, 2);
    ASSERT_EQ(k->cp, (uint32_t)'k');
    bvt_free(vt);
}

static void test_scrollback_push_read(void) {
    BvtTerm *vt = make_term(3, 5);
    /* Fill 3 rows then scroll one more line — the top line should
     * land in scrollback. */
    feed(vt, "AAAAA\r\nBBBBB\r\nCCCCC\r\nDDDDD");
    /* "DDDDD" pushed off "AAAAA" via LF on the third '\n'. */
    int sb = bvt_get_scrollback_lines(vt);
    ASSERT_EQ(sb, 1);

    const BvtCell *first = bvt_get_scrollback_cell(vt, 0, 0);
    ASSERT_NOT_NULL(first);
    ASSERT_EQ(first->cp, (uint32_t)'A');
    const BvtCell *fifth = bvt_get_scrollback_cell(vt, 0, 4);
    ASSERT_EQ(fifth->cp, (uint32_t)'A');

    /* Active grid row 0 is now BBBBB, row 1 is CCCCC, row 2 is DDDDD. */
    const BvtCell *row0 = bvt_get_cell(vt, 0, 0);
    const BvtCell *row1 = bvt_get_cell(vt, 1, 0);
    const BvtCell *row2 = bvt_get_cell(vt, 2, 0);
    ASSERT_EQ(row0->cp, (uint32_t)'B');
    ASSERT_EQ(row1->cp, (uint32_t)'C');
    ASSERT_EQ(row2->cp, (uint32_t)'D');
    bvt_free(vt);
}

static void test_scrollback_grapheme_reintern(void) {
    /* When a row containing a multi-codepoint cluster scrolls off,
     * the cluster must be re-interned into the scrollback page so
     * the cell continues to resolve correctly even after the active
     * grid's arena evolves. */
    BvtTerm *vt = make_term(2, 5);
    feed(vt, "\xF0\x9F\x91\xA8" "\xE2\x80\x8D" "\xF0\x9F\x91\xA9");
    /* Row 0: family man+ZWJ+woman cluster. */
    feed(vt, "\r\nXXXXX\r\nYYYYY"); /* scroll the family off */
    int sb = bvt_get_scrollback_lines(vt);
    ASSERT_EQ(sb, 1);

    const BvtCell *fam = bvt_get_scrollback_cell(vt, 0, 0);
    ASSERT_NOT_NULL(fam);
    ASSERT_EQ(fam->cp, 0x1F468u);
    ASSERT_EQ(fam->width, 2);
    ASSERT_NEQ(fam->grapheme_id, 0u);

    uint32_t cps[8] = { 0 };
    size_t n = bvt_cell_get_grapheme(vt, fam, cps, 8);
    ASSERT_EQ(n, 3u);
    ASSERT_EQ(cps[0], 0x1F468u);
    ASSERT_EQ(cps[1], 0x200Du);
    ASSERT_EQ(cps[2], 0x1F469u);
    bvt_free(vt);
}

static void test_send_key_arrow(void) {
    BvtTerm *vt = make_term(24, 80);
    g_output_len = 0;
    bvt_send_key(vt, BVT_KEY_UP, BVT_MOD_NONE);
    ASSERT_STR_EQ(g_output_buf, "\x1b[A");
    g_output_len = 0; g_output_buf[0] = '\0';
    bvt_send_key(vt, BVT_KEY_LEFT, BVT_MOD_CTRL);
    ASSERT_STR_EQ(g_output_buf, "\x1b[1;5D");
    bvt_free(vt);
}

static void test_decckm_arrow(void) {
    BvtTerm *vt = make_term(24, 80);
    /* DECSET 1 enables cursor key application mode. */
    feed(vt, "\x1b[?1h");
    g_output_len = 0;
    bvt_send_key(vt, BVT_KEY_UP, BVT_MOD_NONE);
    ASSERT_STR_EQ(g_output_buf, "\x1bOA");
    bvt_free(vt);
}

static void test_send_text_alt(void) {
    BvtTerm *vt = make_term(24, 80);
    g_output_len = 0;
    bvt_send_text(vt, "x", 1, BVT_MOD_ALT);
    ASSERT_STR_EQ(g_output_buf, "\x1b" "x");
    bvt_free(vt);
}

static void test_send_text_ctrl(void) {
    /* Ctrl+letter must produce the corresponding control byte so the PTY
     * line discipline can deliver SIGINT for Ctrl+C, etc. */
    BvtTerm *vt = make_term(24, 80);
    struct { char in; uint8_t out; } cases[] = {
        {'c', 0x03}, {'C', 0x03},
        {'a', 0x01}, {'z', 0x1A},
        {'@', 0x00}, {' ', 0x00},  /* Ctrl+Space */
        {'?', 0x7F},               /* Ctrl+?  → DEL */
        {'[', 0x1B},               /* Ctrl+[  → ESC */
        {'\\', 0x1C}, {']', 0x1D}, {'^', 0x1E}, {'_', 0x1F},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        g_output_len = 0;
        char in = cases[i].in;
        bvt_send_text(vt, &in, 1, BVT_MOD_CTRL);
        ASSERT_EQ((int)g_output_len, 1);
        ASSERT_EQ((int)(uint8_t)g_output_buf[0], (int)cases[i].out);
    }
    /* Ctrl+Alt+C → ESC then 0x03 */
    g_output_len = 0;
    bvt_send_text(vt, "c", 1, BVT_MOD_CTRL | BVT_MOD_ALT);
    ASSERT_EQ((int)g_output_len, 2);
    ASSERT_EQ((int)(uint8_t)g_output_buf[0], 0x1B);
    ASSERT_EQ((int)(uint8_t)g_output_buf[1], 0x03);
    bvt_free(vt);
}

static void test_dsr_cpr(void) {
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[5;7H");
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    ASSERT_STR_EQ(g_output_buf, "\x1b[5;7R");
    bvt_free(vt);
}

/* DECOM origin mode: CUP coordinates are relative to the scroll region. */
static void test_decom_cup(void) {
    BvtTerm *vt = make_term(40, 80);
    feed(vt, "\x1b[17;39r");  /* scroll region rows 17..39 (1-indexed) */
    /* Origin mode off — CUP 1;1 → absolute (0,0) */
    feed(vt, "\x1b[1;1H");
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    ASSERT_STR_EQ(g_output_buf, "\x1b[1;1R");
    /* Origin mode on — CUP 1;1 → (16,0) (top of scroll region) */
    feed(vt, "\x1b[?6h");
    feed(vt, "\x1b[1;1H");
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    ASSERT_STR_EQ(g_output_buf, "\x1b[17;1R");
    /* CUP 7;1 in origin mode → (16+6, 0) = (22, 0) → "\x1b[23;1R" */
    feed(vt, "\x1b[7;1H");
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    ASSERT_STR_EQ(g_output_buf, "\x1b[23;1R");
    /* Origin mode off resets cursor to home (per spec) */
    feed(vt, "\x1b[?6l");
    feed(vt, "\x1b[1;1H");
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    ASSERT_STR_EQ(g_output_buf, "\x1b[1;1R");
    bvt_free(vt);
}

/* DECSTBM with invalid margins (top >= bottom) must be rejected. */
static void test_decstbm_invalid_rejected(void) {
    BvtTerm *vt = make_term(24, 80);
    /* Set a real scroll region first. */
    feed(vt, "\x1b[5;15r");
    feed(vt, "\x1b[10;1H");
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    ASSERT_STR_EQ(g_output_buf, "\x1b[10;1R");
    /* Try CSI 1;1 r — invalid (top == bottom). Must NOT change anything. */
    feed(vt, "\x1b[1;1r");
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    /* Cursor should stay at row 10 — the rejected DECSTBM must not home it. */
    ASSERT_STR_EQ(g_output_buf, "\x1b[10;1R");
    bvt_free(vt);
}

/* Faithful replay of the bytes cf sends (captured from the integration
 * test). After processing, the cursor must land at scroll_top, not at
 * (0,0), so that subsequent menu drawing lands at the right place. */
static void test_cf_byte_replay(void) {
    BvtTerm *vt = make_term(40, 120);
    /* 16 prompts as if printed by the shell. */
    for (int i = 0; i < 16; ++i) {
        char line[32]; snprintf(line, sizeof line, "prompt %d\r\n", i + 1);
        feed(vt, line);
    }
    /* cf sends DSR + DECSTBM + DECOM + CUP. After this, cursor should be
     * at the top of the scroll region (row 16 0-indexed, 17 1-indexed). */
    feed(vt, "\x1b[6n");
    g_output_len = 0;
    feed(vt, "\x1b[17;39r");   /* DECSTBM 17;39 → top=16, bot=38 */
    feed(vt, "\x1b[?6h");      /* DECOM on */
    feed(vt, "\x1b[?25l");     /* hide cursor */
    feed(vt, "\x1b[1;1H");     /* CUP 1;1 — origin mode → (16, 0) */
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    ASSERT_STR_EQ(g_output_buf, "\x1b[17;1R");
    bvt_free(vt);
}

/* Reproduces the cf-launcher (brick) inline-render setup:
 * 1. Print 16 lines (cursor advances to row 16)
 * 2. DSR 6 → expect row 17 (1-indexed)
 * 3. Print 16 \r\n (brick reserves vertical space)
 * 4. DECSTBM with full scroll region, then scroll-region narrow
 * 5. Cursor positioning to draw menu at original row
 *
 * If this passes but the cf integration test fails, the bug is in how
 * we handle a sequence we have not yet covered with a unit test. */
static void test_brick_inline_setup(void) {
    BvtTerm *vt = make_term(40, 120);
    /* Fill 16 lines, cursor should land at row 16 col 0. */
    for (int i = 0; i < 16; ++i) {
        char line[32]; snprintf(line, sizeof line, "prompt %d\r\n", i + 1);
        feed(vt, line);
    }
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    ASSERT_STR_EQ(g_output_buf, "\x1b[17;1R");

    /* Brick now prints \r\n × N and DECSTBM. The cursor row after the
     * setup determines where it starts drawing. After 17 \r\n's from
     * row 16, the cursor would land at row 33 (still on screen). */
    for (int i = 0; i < 17; ++i) feed(vt, "\r\n");
    g_output_len = 0;
    feed(vt, "\x1b[6n");
    /* Expected: row 34 (16 + 17 + 1 1-indexed). */
    ASSERT_STR_EQ(g_output_buf, "\x1b[34;1R");

    /* Step through brick's setup in isolation. After each emission,
     * dump the current cursor row+col so we can see exactly which
     * sequence shifts the cursor away from row 33. */
    struct { const char *seq; const char *desc; } steps[] = {
        {"\x1b[0;0r",     "DECSTBM default"},
        {"\x1b[?6h",      "origin mode on"},
        {"\x1b[?25l",     "hide cursor"},
        {"\x1b[?1049l",   "exit altscreen"},
        {"\x1b[23;0;0t",  "XTWINOPS restore"},
        {"\x1b[?12l",     "blink off"},
        {"\x1b[?25h",     "show cursor"},
        {"\x1b(B",        "G0 = ASCII"},
        {"\x1b[m",        "SGR reset"},
        {"\x1b[?12l",     "blink off"},
        {"\x1b[?25h",     "show cursor"},
        {"\x1b[?6l",      "origin mode off"},
        {"\x1b[1;1r",     "DECSTBM 1;1"},
        {"\x1b[0;1H",     "CUP 0;1"},
        {"\r\n",          "CR LF"},
    };
    for (size_t i = 0; i < sizeof(steps)/sizeof(steps[0]); ++i) {
        feed(vt, steps[i].seq);
        g_output_len = 0;
        feed(vt, "\x1b[6n");
        fprintf(stderr, "  after [%s]: DSR=%s\n", steps[i].desc, g_output_buf);
    }

    bvt_free(vt);
}

static void test_da1(void) {
    BvtTerm *vt = make_term(24, 80);
    g_output_len = 0;
    feed(vt, "\x1b[c");
    ASSERT_STR_EQ(g_output_buf, "\x1b[?62;22c");
    bvt_free(vt);
}

static void test_altscreen_save_restore(void) {
    BvtTerm *vt = make_term(5, 5);
    feed(vt, "AAAAA\r\nBBBBB");                /* row 0 = AAAAA, row 1 = BBBBB */
    feed(vt, "\x1b[?1049h");                   /* enter altscreen */
    ASSERT_TRUE(bvt_is_altscreen(vt));
    /* Alt grid is fresh — row 0 should be empty. */
    const BvtCell *alt = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(alt->cp, 0u);
    feed(vt, "ZZZZZ");                          /* write to altscreen */
    feed(vt, "\x1b[?1049l");                   /* exit altscreen */
    ASSERT_FALSE(bvt_is_altscreen(vt));
    /* Original screen restored. */
    const BvtCell *a = bvt_get_cell(vt, 0, 0);
    ASSERT_EQ(a->cp, (uint32_t)'A');
    const BvtCell *b = bvt_get_cell(vt, 1, 0);
    ASSERT_EQ(b->cp, (uint32_t)'B');
    bvt_free(vt);
}

static void test_ich_dch(void) {
    BvtTerm *vt = make_term(2, 6);
    feed(vt, "ABCDEF\r");                      /* row 0 = ABCDEF, cursor col 0 */
    feed(vt, "\x1b[3C");                       /* CUF 3 → col 3 */
    feed(vt, "\x1b[2@");                        /* ICH 2 → insert 2 blanks at col 3 */
    /* Now: A B C _ _ D — last char shifted off. */
    ASSERT_EQ(bvt_get_cell(vt, 0, 0)->cp, (uint32_t)'A');
    ASSERT_EQ(bvt_get_cell(vt, 0, 2)->cp, (uint32_t)'C');
    ASSERT_EQ(bvt_get_cell(vt, 0, 3)->cp, 0u);
    ASSERT_EQ(bvt_get_cell(vt, 0, 4)->cp, 0u);
    ASSERT_EQ(bvt_get_cell(vt, 0, 5)->cp, (uint32_t)'D');
    /* Now DCH 2 at col 3 → cells D shift left, last 2 are blank. */
    feed(vt, "\x1b[2P");
    ASSERT_EQ(bvt_get_cell(vt, 0, 3)->cp, (uint32_t)'D');
    ASSERT_EQ(bvt_get_cell(vt, 0, 4)->cp, 0u);
    ASSERT_EQ(bvt_get_cell(vt, 0, 5)->cp, 0u);
    bvt_free(vt);
}

static void test_il_dl(void) {
    BvtTerm *vt = make_term(4, 3);
    feed(vt, "AAA\r\nBBB\r\nCCC\r\nDDD");
    feed(vt, "\x1b[2;1H"); /* CUP row 2 */
    feed(vt, "\x1b[L");     /* IL 1 — insert blank line at row 2 */
    /* Result: AAA, _, BBB, CCC. (DDD shifted off bottom.) */
    ASSERT_EQ(bvt_get_cell(vt, 0, 0)->cp, (uint32_t)'A');
    ASSERT_EQ(bvt_get_cell(vt, 1, 0)->cp, 0u);
    ASSERT_EQ(bvt_get_cell(vt, 2, 0)->cp, (uint32_t)'B');
    ASSERT_EQ(bvt_get_cell(vt, 3, 0)->cp, (uint32_t)'C');
    /* DL 1 at row 2 (the blank) — undo. */
    feed(vt, "\x1b[M");
    ASSERT_EQ(bvt_get_cell(vt, 1, 0)->cp, (uint32_t)'B');
    ASSERT_EQ(bvt_get_cell(vt, 2, 0)->cp, (uint32_t)'C');
    bvt_free(vt);
}

static void test_sgr_truecolor(void) {
    BvtTerm *vt = make_term(24, 80);
    /* SGR 38;2;255;128;0 — RGB orange foreground. */
    feed(vt, "\x1b[38;2;255;128;0mX");
    const BvtCell *x = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, x);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(s->fg_rgb, 0xFF8000u);
    ASSERT_TRUE((s->color_flags & BVT_COLOR_DEFAULT_FG) == 0);
    bvt_free(vt);
}

static void test_sgr_indexed(void) {
    BvtTerm *vt = make_term(24, 80);
    feed(vt, "\x1b[31mY"); /* red 30+1 */
    const BvtCell *y = bvt_get_cell(vt, 0, 0);
    const BvtStyle *s = bvt_cell_style(vt, y);
    ASSERT_NOT_NULL(s);
    /* Charm red is 0xed567a per palette.c. */
    ASSERT_EQ(s->fg_rgb, 0xED567Au);
    bvt_free(vt);
}

static void test_reflow_grow(void) {
    BvtTerm *vt = make_term(2, 5);
    bvt_set_reflow(vt, true);
    /* "abcdef" wraps at col 5: row 0 "abcde" with WRAPLINE, row 1 "f". */
    feed(vt, "abcdef");
    ASSERT_TRUE(bvt_get_line_continuation(vt, 0));
    /* Grow to 10 cols: should unwrap to a single row "abcdef". */
    bvt_resize(vt, 2, 10);
    int rows, cols;
    bvt_get_dimensions(vt, &rows, &cols);
    ASSERT_EQ(rows, 2);
    ASSERT_EQ(cols, 10);
    ASSERT_EQ(bvt_get_cell(vt, 0, 0)->cp, (uint32_t)'a');
    ASSERT_EQ(bvt_get_cell(vt, 0, 5)->cp, (uint32_t)'f');
    ASSERT_FALSE(bvt_get_line_continuation(vt, 0));
    bvt_free(vt);
}

static void test_reflow_shrink(void) {
    BvtTerm *vt = make_term(2, 10);
    bvt_set_reflow(vt, true);
    feed(vt, "abcdefghij");
    /* Shrink to 5 cols: "abcde" with WRAPLINE, row 1 "fghij". */
    bvt_resize(vt, 2, 5);
    ASSERT_EQ(bvt_get_cell(vt, 0, 0)->cp, (uint32_t)'a');
    ASSERT_EQ(bvt_get_cell(vt, 0, 4)->cp, (uint32_t)'e');
    ASSERT_TRUE(bvt_get_line_continuation(vt, 0));
    ASSERT_EQ(bvt_get_cell(vt, 1, 0)->cp, (uint32_t)'f');
    ASSERT_EQ(bvt_get_cell(vt, 1, 4)->cp, (uint32_t)'j');
    bvt_free(vt);
}

static void test_reflow_overflow_to_scrollback(void) {
    BvtTerm *vt = make_term(2, 10);
    bvt_set_reflow(vt, true);
    feed(vt, "abcdefghij");        /* fills row 0 with WRAPLINE */
    feed(vt, "klmnopqrst");        /* fills row 1 (no WRAPLINE — separate logical line because we wrote 10 chars then wrapped from row 0 to row 1) */
    /* Actually after writing 10 chars, the cursor is at col 9 with
     * pending_wrap. Then writing more wraps. Let's just verify the
     * shrink behavior. */
    bvt_resize(vt, 2, 5);
    /* The first logical line "abcdefghij…klmnopqrst" if all wrapped,
     * or just "abcdefghij" then "klmnopqrst" as two logical lines.
     * Either way, after shrinking the older content goes to scrollback. */
    ASSERT_TRUE(bvt_get_scrollback_lines(vt) >= 1);
    bvt_free(vt);
}

static void test_reflow_preserves_styles(void) {
    BvtTerm *vt = make_term(2, 5);
    bvt_set_reflow(vt, true);
    feed(vt, "\x1b[31mAAAAA\x1b[0mBBBBB"); /* row 0 red, row 1 default */
    bvt_resize(vt, 2, 10);
    /* After reflow, "AAAAABBBBB" might be one row. Verify A is red,
     * B is default. */
    const BvtCell *a = bvt_get_cell(vt, 0, 0);
    const BvtCell *b = bvt_get_cell(vt, 0, 5);
    ASSERT_EQ(a->cp, (uint32_t)'A');
    ASSERT_EQ(b->cp, (uint32_t)'B');
    const BvtStyle *as = bvt_cell_style(vt, a);
    const BvtStyle *bs = bvt_cell_style(vt, b);
    ASSERT_NOT_NULL(as);
    ASSERT_NOT_NULL(bs);
    ASSERT_EQ(as->fg_rgb, 0xED567Au); /* red */
    ASSERT_TRUE((bs->color_flags & BVT_COLOR_DEFAULT_FG) != 0);
    bvt_free(vt);
}

static void test_reflow_disabled(void) {
    BvtTerm *vt = make_term(2, 5);
    /* reflow disabled: clamp behavior, content stays put. */
    feed(vt, "abcdef");           /* "abcde" + wrap to "f" */
    bvt_resize(vt, 2, 10);
    /* After clamp resize content should still occupy its original cells
     * (now in a wider grid). Row 0 still has "abcde" at cols 0-4; the
     * 'f' from row 1 stays at row 1 col 0. */
    ASSERT_EQ(bvt_get_cell(vt, 0, 0)->cp, (uint32_t)'a');
    ASSERT_EQ(bvt_get_cell(vt, 0, 4)->cp, (uint32_t)'e');
    ASSERT_EQ(bvt_get_cell(vt, 1, 0)->cp, (uint32_t)'f');
    bvt_free(vt);
}

/* DEC special graphics charset — `tput smacs` / `\033(0` selects a line-
 * drawing alphabet on G0. Apps like tmux and ncurses rely on it for
 * borders. While selected, ASCII bytes 0x5F..0x7E translate to box-
 * drawing codepoints. SI/SO toggle between G0 and G1. */
/* Regression: OSC titles with UTF-8 codepoints whose trailing continuation
 * byte is 0x9C (e.g. U+201C "left double quote" = E2 80 9C) used to abort
 * the OSC because 0x9C was treated as a bare C1 ST. In UTF-8 mode the
 * only OSC terminators are ESC\ and BEL. */
static void test_osc_title_utf8_with_9c_byte(void) {
    BvtTerm *vt = make_term(2, 10);
    g_title = NULL;
    /* OSC 2 ; "Claude" U+201C "x" BEL — note the U+201C byte sequence
     * E2 80 9C lives inside the title body. The parser must not split
     * on the 9C and ship a partial codepoint. */
    feed(vt, "\x1b]2;Claude\xe2\x80\x9cx\x07");
    /* The title we kept should be the full sequence — a fully-formed
     * UTF-8 string. Validate by walking it through a small decoder. */
    ASSERT_NOT_NULL(g_title);
    ASSERT_STR_EQ(g_title, "Claude\xe2\x80\x9cx");
    bvt_free(vt);
}

static void test_dec_graphics_g0(void) {
    BvtTerm *vt = make_term(2, 10);
    feed(vt, "\x1b(0lqk\x1b(B");
    /* l→┌ (0x250C), q→─ (0x2500), k→┐ (0x2510). */
    ASSERT_EQ(bvt_get_cell(vt, 0, 0)->cp, 0x250Cu);
    ASSERT_EQ(bvt_get_cell(vt, 0, 1)->cp, 0x2500u);
    ASSERT_EQ(bvt_get_cell(vt, 0, 2)->cp, 0x2510u);
    bvt_free(vt);
}
static void test_dec_graphics_si_so(void) {
    BvtTerm *vt = make_term(2, 10);
    /* Designate G1 as DEC graphics, then SO (0x0E) shifts G1 into GL. */
    feed(vt, "\x1b)0Aa\x0e" "ax\x0f" "Z");
    /* G0 stays ASCII: 'A','a' pass through. After SO: 'a' → ▒
     * (0x2592), 'x' → │ (0x2502). After SI: 'Z' is plain ASCII. */
    ASSERT_EQ(bvt_get_cell(vt, 0, 0)->cp, (uint32_t)'A');
    ASSERT_EQ(bvt_get_cell(vt, 0, 1)->cp, (uint32_t)'a');
    ASSERT_EQ(bvt_get_cell(vt, 0, 2)->cp, 0x2592u);
    ASSERT_EQ(bvt_get_cell(vt, 0, 3)->cp, 0x2502u);
    ASSERT_EQ(bvt_get_cell(vt, 0, 4)->cp, (uint32_t)'Z');
    bvt_free(vt);
}

static void test_wrap(void) {
    BvtTerm *vt = make_term(2, 5);
    feed(vt, "abcdef"); /* 6 chars in 5-col grid → wrap */
    const BvtCell *c0 = bvt_get_cell(vt, 0, 0);
    const BvtCell *c4 = bvt_get_cell(vt, 0, 4);
    const BvtCell *c5 = bvt_get_cell(vt, 1, 0);
    ASSERT_EQ(c0->cp, (uint32_t)'a');
    ASSERT_EQ(c4->cp, (uint32_t)'e');
    ASSERT_EQ(c5->cp, (uint32_t)'f');
    /* Row 0 should be marked WRAPLINE. */
    ASSERT_TRUE(bvt_get_line_continuation(vt, 0));
    ASSERT_FALSE(bvt_get_line_continuation(vt, 1));
    bvt_free(vt);
}

int main(int argc, char *argv[]) {
    test_parse_args(argc, argv);
    printf("Running bloom-vt parser tests:\n");
    RUN_TEST(test_print_ascii);
    RUN_TEST(test_lf_cr);
    RUN_TEST(test_csi_cup);
    RUN_TEST(test_csi_erase_line);
    RUN_TEST(test_osc_title_bel);
    RUN_TEST(test_osc_title_st);
    RUN_TEST(test_utf8_basic);
    RUN_TEST(test_utf8_replacement);
    RUN_TEST(test_csi_sgr_reset);
    RUN_TEST(test_bel);
    RUN_TEST(test_style_intern_dedup);
    RUN_TEST(test_grapheme_arena_dedup);
    RUN_TEST(test_width_cjk);
    RUN_TEST(test_width_emoji);
    RUN_TEST(test_vs16_widens_ambiguous);
    RUN_TEST(test_combining_mark);
    RUN_TEST(test_regional_indicator_pair);
    RUN_TEST(test_zwj_family);
    RUN_TEST(test_skin_tone);
    RUN_TEST(test_scrollback_push_read);
    RUN_TEST(test_scrollback_grapheme_reintern);
    RUN_TEST(test_send_key_arrow);
    RUN_TEST(test_decckm_arrow);
    RUN_TEST(test_send_text_alt);
    RUN_TEST(test_send_text_ctrl);
    RUN_TEST(test_dsr_cpr);
    RUN_TEST(test_decom_cup);
    RUN_TEST(test_decstbm_invalid_rejected);
    RUN_TEST(test_osc_title_utf8_with_9c_byte);
    RUN_TEST(test_dec_graphics_g0);
    RUN_TEST(test_dec_graphics_si_so);
    RUN_TEST(test_cf_byte_replay);
    RUN_TEST(test_brick_inline_setup);
    RUN_TEST(test_da1);
    RUN_TEST(test_altscreen_save_restore);
    RUN_TEST(test_ich_dch);
    RUN_TEST(test_il_dl);
    RUN_TEST(test_sgr_truecolor);
    RUN_TEST(test_sgr_indexed);
    RUN_TEST(test_reflow_grow);
    RUN_TEST(test_reflow_shrink);
    RUN_TEST(test_reflow_overflow_to_scrollback);
    RUN_TEST(test_reflow_preserves_styles);
    RUN_TEST(test_reflow_disabled);
    RUN_TEST(test_wrap);
    TEST_SUMMARY();
}
