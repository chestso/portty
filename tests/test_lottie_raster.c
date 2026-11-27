/*
 * portty — Lottie CPU rasterization tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "test_helpers.h"

#include "lottie_raster.h"
#include "term.h"
#include "term_cfr.h"

#include <stdlib.h>
#include <string.h>

extern TerminalBackend terminal_backend_cfr;

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t in_len,
                         char *out, size_t out_cap)
{
    size_t out_len = 0;
    for (size_t i = 0; i < in_len && out_len + 4 < out_cap; i += 3) {
        uint32_t a = in[i];
        uint32_t b = i + 1 < in_len ? in[i + 1] : 0;
        uint32_t c = i + 2 < in_len ? in[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[out_len++] = b64_chars[(triple >> 18) & 0x3f];
        out[out_len++] = b64_chars[(triple >> 12) & 0x3f];
        out[out_len++] = i + 1 < in_len ? b64_chars[(triple >> 6) & 0x3f] : '=';
        out[out_len++] = i + 2 < in_len ? b64_chars[triple & 0x3f] : '=';
    }
    out[out_len] = '\0';
    return out_len;
}

static void apc(TerminalBackend *t, const char *json)
{
    size_t len = strlen(json);
    size_t cap = 4 * ((len + 2) / 3) + 8;
    char *b64 = malloc(cap);
    ASSERT_NOT_NULL(b64);
    b64_encode((const uint8_t *)json, len, b64, cap);
    size_t seq_cap = strlen(b64) + 8;
    char *seq = malloc(seq_cap);
    ASSERT_NOT_NULL(seq);
    snprintf(seq, seq_cap, "\x1b_%s\x1b\\", b64);
    terminal_process_input(t, seq, strlen(seq));
    free(seq);
    free(b64);
}

static TerminalBackend *make_term(void)
{
    TerminalBackend *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    *t = terminal_backend_cfr;
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.cols = 80;
    cfg.rows = 24;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    if (!terminal_init(t, &cfg)) {
        free(t);
        return NULL;
    }
    return t;
}

static void load_color_animation(TerminalBackend *t, int fill_opacity)
{
    char json[4096];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"load\",\"id\":1,\"play\":{\"autostart\":false},"
             "\"lottie\":{\"v\":\"5.6.0\",\"fr\":30,\"ip\":0,\"op\":2,"
             "\"w\":20,\"h\":20,\"nm\":\"raster\",\"ddd\":0,\"assets\":[],"
             "\"layers\":[{\"ddd\":0,\"ind\":1,\"ty\":4,\"nm\":\"bg\",\"sr\":1,"
             "\"ks\":{\"o\":{\"a\":0,\"k\":100},\"r\":{\"a\":0,\"k\":0},"
             "\"p\":{\"a\":0,\"k\":[10,10,0]},\"a\":{\"a\":0,\"k\":[0,0,0]},"
             "\"s\":{\"a\":0,\"k\":[100,100,100]}},\"ao\":0,\"shapes\":[{"
             "\"ty\":\"gr\",\"it\":[{\"ty\":\"rc\",\"d\":1,"
             "\"s\":{\"a\":0,\"k\":[20,20]},\"p\":{\"a\":0,\"k\":[0,0]},"
             "\"r\":{\"a\":0,\"k\":0}},{\"ty\":\"fl\",\"c\":{\"a\":1,\"k\":["
             "{\"t\":0,\"s\":[1,0,0,1],\"h\":1},"
             "{\"t\":1,\"s\":[0,1,0,1],\"h\":1}]},\"o\":{\"a\":0,\"k\":%d},"
             "\"r\":1},{\"ty\":\"tr\",\"p\":{\"a\":0,\"k\":[0,0]},"
             "\"a\":{\"a\":0,\"k\":[0,0]},\"s\":{\"a\":0,\"k\":[100,100]},"
             "\"r\":{\"a\":0,\"k\":0},\"o\":{\"a\":0,\"k\":100}}]}],"
             "\"ip\":0,\"op\":2,\"st\":0,\"bm\":0}]}}}",
             fill_opacity);
    apc(t, json);
}

static void assert_pixel(const LottieFrame *frame, int x, int y,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const uint8_t *px = frame->rgba + ((size_t)y * (size_t)frame->w + x) * 4;
    ASSERT_EQ(px[0], r);
    ASSERT_EQ(px[1], g);
    ASSERT_EQ(px[2], b);
    ASSERT_EQ(px[3], a);
}

static void test_unavailable_stub(void)
{
    if (lottie_raster_available())
        return;
    LottieRaster *raster = lottie_raster_create();
    ASSERT_NULL(raster);
    lottie_raster_sync(raster, NULL, 0);
    LottieFrame frame;
    ASSERT_FALSE(lottie_raster_get(raster, 1, &frame));
    lottie_raster_destroy(raster);
}

static void test_raster_seek_resize_and_delete(void)
{
    if (!lottie_raster_available())
        return;

    TerminalBackend *t = make_term();
    ASSERT_NOT_NULL(t);
    LottieRaster *raster = lottie_raster_create();
    ASSERT_NOT_NULL(raster);
    load_color_animation(t, 100);

    int count = 0;
    const CfrLottie *anims = terminal_get_lotties(t, &count);
    ASSERT_EQ(count, 1);
    lottie_raster_sync(raster, anims, count);

    LottieFrame frame;
    ASSERT_TRUE(lottie_raster_get(raster, anims[0].handle, &frame));
    ASSERT_EQ(frame.w, 20);
    ASSERT_EQ(frame.h, 20);
    ASSERT_EQ(frame.raster_rev, anims[0].raster_rev);
    assert_pixel(&frame, 0, 0, 255, 0, 0, 255);
    assert_pixel(&frame, 10, 10, 255, 0, 0, 255);
    assert_pixel(&frame, 19, 19, 255, 0, 0, 255);

    /* An unchanged revision is a cache hit. */
    const uint8_t *pixels = frame.rgba;
    lottie_raster_sync(raster, anims, count);
    ASSERT_TRUE(lottie_raster_get(raster, anims[0].handle, &frame));
    ASSERT_EQ(frame.rgba, pixels);

    apc(t, "{\"cmd\":\"seek\",\"id\":1,\"frame\":1}");
    anims = terminal_get_lotties(t, &count);
    lottie_raster_sync(raster, anims, count);
    ASSERT_TRUE(lottie_raster_get(raster, anims[0].handle, &frame));
    assert_pixel(&frame, 10, 10, 0, 255, 0, 255);

    apc(t, "{\"cmd\":\"place\",\"id\":1,"
           "\"placement\":{\"row\":0,\"col\":0,\"width\":40,\"height\":40}}");
    anims = terminal_get_lotties(t, &count);
    lottie_raster_sync(raster, anims, count);
    ASSERT_TRUE(lottie_raster_get(raster, anims[0].handle, &frame));
    ASSERT_EQ(frame.w, 40);
    ASSERT_EQ(frame.h, 40);
    assert_pixel(&frame, 20, 20, 0, 255, 0, 255);

    uint64_t deleted_handle = anims[0].handle;
    apc(t, "{\"cmd\":\"delete\",\"id\":1}");
    anims = terminal_get_lotties(t, &count);
    ASSERT_EQ(count, 0);
    lottie_raster_sync(raster, anims, count);
    ASSERT_FALSE(lottie_raster_get(raster, deleted_handle, &frame));

    lottie_raster_destroy(raster);
    terminal_destroy(t);
    free(t);
}

static void test_straight_alpha_output(void)
{
    if (!lottie_raster_available())
        return;
    TerminalBackend *t = make_term();
    ASSERT_NOT_NULL(t);
    LottieRaster *raster = lottie_raster_create();
    ASSERT_NOT_NULL(raster);
    load_color_animation(t, 50);

    int count = 0;
    const CfrLottie *anims = terminal_get_lotties(t, &count);
    lottie_raster_sync(raster, anims, count);
    LottieFrame frame;
    ASSERT_TRUE(lottie_raster_get(raster, anims[0].handle, &frame));
    assert_pixel(&frame, 10, 10, 255, 0, 0, 127);

    lottie_raster_destroy(raster);
    terminal_destroy(t);
    free(t);
}

static void test_invalid_definition_has_no_frame(void)
{
    if (!lottie_raster_available())
        return;
    LottieRaster *raster = lottie_raster_create();
    ASSERT_NOT_NULL(raster);
    const uint8_t json[] = "not a Lottie document";
    CfrLottie anim = {
        .handle = 77,
        .definition_rev = 1,
        .raster_rev = 1,
        .json = json,
        .json_len = sizeof(json) - 1,
        .px_w = 20,
        .px_h = 20,
    };
    lottie_raster_sync(raster, &anim, 1);
    LottieFrame frame;
    ASSERT_FALSE(lottie_raster_get(raster, 77, &frame));
    lottie_raster_destroy(raster);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_lottie_raster\n");
    RUN_TEST(test_unavailable_stub);
    RUN_TEST(test_raster_seek_resize_and_delete);
    RUN_TEST(test_straight_alpha_output);
    RUN_TEST(test_invalid_definition_has_no_frame);
    TEST_SUMMARY();
}
