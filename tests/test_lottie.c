/*
 * test_lottie — Lottie animation end-to-end through the host bridge.
 *
 * The protocol/state logic is unit-tested in coffer itself.
 * This file checks the portty side: that an APC Lottie sequence
 * fed through terminal_process_input() reaches the engine and comes back
 * out through terminal_get_lotties() / terminal_get_lottie_placements()
 * with the right metadata, that playback state changes work, and that
 * clear/delete remove animations.
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

/* --- Base64 encoder for building APC payloads in tests --- */

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t in_len,
                         char *out, size_t out_cap)
{
    size_t out_len = 0;
    for (size_t i = 0; i < in_len && out_len + 4 < out_cap; i += 3) {
        uint32_t a = in[i];
        uint32_t b = (i + 1 < in_len) ? in[i + 1] : 0;
        uint32_t c = (i + 2 < in_len) ? in[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        out[out_len++] = b64_chars[(triple >> 18) & 0x3F];
        out[out_len++] = b64_chars[(triple >> 12) & 0x3F];
        out[out_len++] = (i + 1 < in_len) ? b64_chars[(triple >> 6) & 0x3F]
                                          : '=';
        out[out_len++] = (i + 2 < in_len) ? b64_chars[triple & 0x3F] : '=';
    }
    out[out_len] = '\0';
    return out_len;
}

/* Build an APC sequence: ESC _ <base64-json> ESC \ */
static void apc(TerminalBackend *t, const char *json)
{
    size_t json_len = strlen(json);
    char b64[4096];
    size_t b64_len = b64_encode((const uint8_t *)json, json_len, b64,
                                sizeof(b64));

    char seq[4224];
    size_t pos = 0;
    seq[pos++] = '\x1b';
    seq[pos++] = '_';
    memcpy(seq + pos, b64, b64_len);
    pos += b64_len;
    seq[pos++] = '\x1b';
    seq[pos++] = '\\';
    terminal_process_input(t, seq, pos);
}

/* Helper: create a heap-allocated terminal with standard test config */
static TerminalBackend *make_term(int cols, int rows)
{
    TerminalBackend *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    *t = terminal_backend_cfr;
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.cols = cols;
    cfg.rows = rows;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    if (!terminal_init(t, &cfg)) {
        free(t);
        return NULL;
    }
    return t;
}

static void destroy_term(TerminalBackend *t)
{
    terminal_destroy(t);
    free(t);
}

/* --- Tests --- */

/* A load command creates one animation with correct metadata. */
static void test_load_basic(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":90,"
           "\"w\":40,\"h\":40,\"layers\":[]},"
           "\"placement\":{\"row\":5,\"col\":10,\"rows\":4,\"cols\":8},"
           "\"layer\":\"foreground\"}");

    int n = -1;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_NOT_NULL(l);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(l[0].id, 1);
    ASSERT_EQ(l[0].canvas_w, 80);
    ASSERT_EQ(l[0].canvas_h, 80);
    ASSERT_EQ(l[0].current_frame, 0);
    ASSERT_EQ(l[0].frame_count, 90);
    ASSERT_TRUE(l[0].playing);
    ASSERT_TRUE(l[0].speed > 0.99 && l[0].speed < 1.01);
    ASSERT_TRUE(l[0].loop);
    ASSERT_EQ(l[0].placement_count, 1);

    int pn = -1;
    const CfrLottiePlacement *pl =
        terminal_get_lottie_placements(t, l[0].id, &pn);
    ASSERT_NOT_NULL(pl);
    ASSERT_EQ(pn, 1);
    ASSERT_EQ(pl[0].col, 10);
    ASSERT_EQ(pl[0].rows, 4);
    ASSERT_EQ(pl[0].cols, 8);
    ASSERT_EQ(pl[0].layer, 0);
    ASSERT_EQ(pl[0].opacity_x256, 255);

    destroy_term(t);
}

/* Pause freezes the animation; play resumes it. */
static void test_play_pause(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
           "\"w\":20,\"h\":20,\"layers\":[]}}");

    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_TRUE(l[0].playing);

    apc(t, "{\"cmd\":\"pause\",\"id\":1}");
    l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_FALSE(l[0].playing);

    apc(t, "{\"cmd\":\"play\",\"id\":1,\"speed\":2.0}");
    l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_TRUE(l[0].playing);
    ASSERT_TRUE(l[0].speed > 1.99 && l[0].speed < 2.01);

    destroy_term(t);
}

/* Stop resets to frame_ip and pauses. */
static void test_stop(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":5,\"op\":30,"
           "\"w\":20,\"h\":20,\"layers\":[]}}");

    apc(t, "{\"cmd\":\"seek\",\"id\":1,\"frame\":15}");

    apc(t, "{\"cmd\":\"stop\",\"id\":1}");
    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_FALSE(l[0].playing);
    ASSERT_EQ(l[0].current_frame, 5);

    destroy_term(t);
}

/* Seek jumps to the requested frame (clamped). */
static void test_seek(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
           "\"w\":20,\"h\":20,\"layers\":[]}}");

    apc(t, "{\"cmd\":\"pause\",\"id\":1}");

    apc(t, "{\"cmd\":\"seek\",\"id\":1,\"frame\":15}");
    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(l[0].current_frame, 15);

    /* Seek beyond op → clamped to op-1 */
    apc(t, "{\"cmd\":\"seek\",\"id\":1,\"frame\":999}");
    l = terminal_get_lotties(t, &n);
    ASSERT_EQ(l[0].current_frame, 29);

    /* Seek below ip → clamped to ip */
    apc(t, "{\"cmd\":\"seek\",\"id\":1,\"frame\":-5}");
    l = terminal_get_lotties(t, &n);
    ASSERT_EQ(l[0].current_frame, 0);

    destroy_term(t);
}

/* Delete removes the animation entirely. */
static void test_delete(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
           "\"w\":20,\"h\":20,\"layers\":[]}}");

    int n;
    terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);

    apc(t, "{\"cmd\":\"delete\",\"id\":1}");
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_NULL(l);
    ASSERT_EQ(n, 0);

    destroy_term(t);
}

/* Background layer has layer=1, custom opacity. */
static void test_background_layer(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":2,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
           "\"w\":40,\"h\":40,\"layers\":[]},"
           "\"layer\":\"background\",\"opacity\":0.3}");

    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);

    int pn;
    const CfrLottiePlacement *pl =
        terminal_get_lottie_placements(t, l[0].id, &pn);
    ASSERT_NOT_NULL(pl);
    ASSERT_EQ(pn, 1);
    ASSERT_EQ(pl[0].layer, 1);
    ASSERT_EQ(pl[0].opacity_x256, 77);

    destroy_term(t);
}

/* Place adds additional placements to an existing animation. */
static void test_place(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
           "\"w\":20,\"h\":20,\"layers\":[]}}");

    apc(t, "{\"cmd\":\"place\",\"id\":1,"
           "\"placement\":{\"row\":0,\"col\":78,\"rows\":2,\"cols\":2},"
           "\"layer\":\"background\",\"opacity\":0.5}");

    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(l[0].placement_count, 2);

    int pn;
    const CfrLottiePlacement *pl =
        terminal_get_lottie_placements(t, l[0].id, &pn);
    ASSERT_NOT_NULL(pl);
    ASSERT_EQ(pn, 2);
    ASSERT_EQ(pl[0].layer, 0);
    ASSERT_EQ(pl[1].layer, 1);
    ASSERT_EQ(pl[1].col, 78);
    ASSERT_EQ(pl[1].rows, 2);
    ASSERT_EQ(pl[1].opacity_x256, 128);

    destroy_term(t);
}

/* Version bumps on state changes. */
static void test_version_bump(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
           "\"w\":20,\"h\":20,\"layers\":[]}}");

    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    uint32_t v0 = l[0].version;
    ASSERT_TRUE(v0 > 0);

    apc(t, "{\"cmd\":\"pause\",\"id\":1}");
    l = terminal_get_lotties(t, &n);
    ASSERT_TRUE(l[0].version > v0);

    uint32_t v1 = l[0].version;
    apc(t, "{\"cmd\":\"play\",\"id\":1}");
    l = terminal_get_lotties(t, &n);
    ASSERT_TRUE(l[0].version > v1);

    destroy_term(t);
}

/* Load with same id replaces the animation. */
static void test_load_replace(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
           "\"w\":20,\"h\":20,\"layers\":[]}}");

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":60,\"ip\":0,\"op\":120,"
           "\"w\":80,\"h\":80,\"layers\":[]}}");

    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(l[0].id, 1);
    ASSERT_EQ(l[0].canvas_w, 80);
    ASSERT_EQ(l[0].canvas_h, 80);
    ASSERT_EQ(l[0].frame_count, 120);

    destroy_term(t);
}

/* Tick advances playing animations. */
static void test_tick_advance(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":90,"
           "\"w\":20,\"h\":20,\"layers\":[]},"
           "\"play\":{\"autostart\":true}}");

    /* First tick establishes the baseline timestamp */
    bool advanced = terminal_lottie_tick(t, 1000000);
    ASSERT_FALSE(advanced);

    /* Advance 1 second → at 30fps should advance 30 frames */
    advanced = terminal_lottie_tick(t, 2000000);
    ASSERT_TRUE(advanced);

    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);
    ASSERT_TRUE(l[0].current_frame > 0);

    destroy_term(t);
}

/* Tick on paused animation does nothing. */
static void test_tick_paused(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":90,"
           "\"w\":20,\"h\":20,\"layers\":[]},"
           "\"play\":{\"autostart\":false}}");

    terminal_lottie_tick(t, 1000000);
    bool advanced = terminal_lottie_tick(t, 2000000);
    ASSERT_FALSE(advanced);

    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_EQ(l[0].current_frame, 0);

    destroy_term(t);
}

/* No lottie animations returns NULL/0. */
static void test_no_animations(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    int n = -1;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_NULL(l);
    ASSERT_EQ(n, 0);

    destroy_term(t);
}

/* Clearing the screen removes foreground animations. */
static void test_clear(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
           "\"w\":20,\"h\":20,\"layers\":[]},"
           "\"layer\":\"foreground\"}");

    int n;
    terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);

    feed(t, "\x1b[2J");
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_NULL(l);
    ASSERT_EQ(n, 0);

    destroy_term(t);
}

/* Load without explicit placement uses cursor position. */
static void test_load_default_placement(void)
{
    TerminalBackend *t = make_term(80, 24);
    ASSERT_TRUE(t != NULL);

    apc(t, "{\"cmd\":\"load\",\"id\":1,"
           "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":30,"
           "\"w\":40,\"h\":40,\"layers\":[]}}");

    int n;
    const CfrLottie *l = terminal_get_lotties(t, &n);
    ASSERT_EQ(n, 1);

    int pn;
    const CfrLottiePlacement *pl =
        terminal_get_lottie_placements(t, l[0].id, &pn);
    ASSERT_NOT_NULL(pl);
    ASSERT_EQ(pn, 1);
    /* Default cursor position is row 0, col 0 */
    ASSERT_EQ(pl[0].col, 0);
    /* Canvas 40x40 at cell 10x20 → cols=4, rows=2 */
    ASSERT_EQ(pl[0].cols, 4);
    ASSERT_EQ(pl[0].rows, 2);

    destroy_term(t);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_lottie\n");
    RUN_TEST(test_load_basic);
    RUN_TEST(test_play_pause);
    RUN_TEST(test_stop);
    RUN_TEST(test_seek);
    RUN_TEST(test_delete);
    RUN_TEST(test_background_layer);
    RUN_TEST(test_place);
    RUN_TEST(test_version_bump);
    RUN_TEST(test_load_replace);
    RUN_TEST(test_tick_advance);
    RUN_TEST(test_tick_paused);
    RUN_TEST(test_no_animations);
    RUN_TEST(test_clear);
    RUN_TEST(test_load_default_placement);
    TEST_SUMMARY();
}
