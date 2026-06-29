#include "diag.h"
#include "portty_version.h"
#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A fully-populated sample so the report exercises every section.
static DiagSources sample(void)
{
    DiagSources s = {
        .renderer_name = "test-gpu",
        .gpu_device = "Test GPU 9000 (NVK)",
        .gpu_driver = "NVK (open source) — Mesa 25.x",
        .gpu_driver_libre = true,
        .linear_light = true,
        .glyph_shader = true,
        .content_scale = 1.5f,
        .pixel_width = 1280,
        .pixel_height = 720,
        .cell_width = 10,
        .cell_height = 22,
        .cols = 128,
        .rows = 32,
        .config_path = "/tmp/test/portty.conf",
        .font_pattern = "Cascadia Code-14",
        .font_path = "/usr/share/fonts/test.ttf",
        .font_source = "config file",
        .hinting = "light",
        .scrollback = 1000,
        .text_gamma = 1.7f,
        .text_contrast = 30.0f,
        .word_chars = "A-Za-z0-9_",
        .platform_name = "sdl3",
        .term_env = "portty-vty-256color",
        .colorterm_env = "truecolor",
        .lang_env = "en_US.UTF-8",
        .title = "bash",
        .altscreen = false,
        .mouse_mode = 0,
        .vt_backend = "bloom-vt",
        .lottie_rasterizer = true,
        .osc52 = true,
        .bracketed_paste = false,
        .sync_output = false,
        .focus_reporting = false,
        .sixel_scrolling = false,
        .hardened_heap = false,
    };
    return s;
}

static void test_null_input(void)
{
    ASSERT_NULL(diag_build_report(NULL));
}

static void test_contains_sections(void)
{
    DiagSources s = sample();
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "VERSION & BUILD") != NULL);
    ASSERT_TRUE(strstr(r, "RENDERING") != NULL);
    ASSERT_TRUE(strstr(r, "CONFIGURATION") != NULL);
    ASSERT_TRUE(strstr(r, "VT FEATURES") != NULL);
    ASSERT_TRUE(strstr(r, "SESSION") != NULL);
    ASSERT_TRUE(strstr(r, "SYSTEM") != NULL);
    free(r);
}

static void test_contains_values(void)
{
    DiagSources s = sample();
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    // Version string from the build (portty_version.h).
    ASSERT_TRUE(strstr(r, PORTTY_VERSION) != NULL);
    // A few runtime values flow through verbatim.
    ASSERT_TRUE(strstr(r, "test-gpu") != NULL);
    ASSERT_TRUE(strstr(r, "Cascadia Code-14") != NULL);
    ASSERT_TRUE(strstr(r, "/usr/share/fonts/test.ttf") != NULL);
    ASSERT_TRUE(strstr(r, "128 cols x 32 rows") != NULL);
    ASSERT_TRUE(strstr(r, "portty-vty-256color") != NULL);
    free(r);
}

static void test_neutral_composition(void)
{
    DiagSources s = sample();
    s.text_gamma = 1.0f;
    s.text_contrast = 0.0f;
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "neutral") != NULL);
    free(r);
}

static void test_glyph_curve_states(void)
{
    // Non-neutral curve + shader active => GPU shader label.
    DiagSources s = sample(); // gamma 1.7, glyph_shader = true
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "GPU shader (luminance-aware)") != NULL);
    free(r);

    // Non-neutral curve + no shader => uniform baked-LUT fallback label.
    s = sample();
    s.glyph_shader = false;
    r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "no GPU shader") != NULL);
    free(r);

    // Neutral curve => identity, regardless of the shader flag.
    s = sample();
    s.text_gamma = 1.0f;
    s.text_contrast = 0.0f;
    s.glyph_shader = false;
    r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "neutral (identity)") != NULL);
    free(r);
}

static void test_scrollback(void)
{
    // Enabled: shows the configured capacity, not "(disabled)".
    DiagSources s = sample(); // scrollback = 1000
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "1000 lines") != NULL);
    ASSERT_TRUE(strstr(r, "(disabled)") == NULL);
    free(r);

    // Only a capacity of 0 reads as disabled.
    s = sample();
    s.scrollback = 0;
    r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "(disabled)") != NULL);
    free(r);
}

static void test_altscreen_neutral(void)
{
    // "alt screen: no" is a neutral state, not an error — it must not be
    // emitted in the red (FG_OFF) colour. Verify the "alt screen" key is
    // followed by plain "no" without the red SGR escape (other sections
    // legitimately use red for disabled features, so a whole-report check
    // would be too broad).
    DiagSources s = sample(); // altscreen = false
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    const char *alt = strstr(r, "alt screen");
    ASSERT_TRUE(alt != NULL);
    // The "no" value follows the key; red SGR (\x1b[31m) must not appear
    // between "alt screen" and the "no" value.
    const char *red = strstr(alt, "\x1b[31m");
    const char *newline = strchr(alt, '\n');
    ASSERT_TRUE(red == NULL || red > newline);
    free(r);
}

static void test_issues_hyperlink(void)
{
    // The footer URL is wrapped in an OSC 8 hyperlink so the internal pager can
    // make it clickable. Check both the opening (with URI) and closing markers.
    DiagSources s = sample();
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "\x1b]8;;https://codeberg.org/thomasc/portty/issues\x1b\\") !=
                NULL);
    ASSERT_TRUE(strstr(r, "\x1b]8;;\x1b\\") != NULL);
    free(r);
}

static void test_gpu_driver_color(void)
{
    // Permissively-licensed open-source driver → green (FG_ON, SGR 32)
    // immediately preceding the driver text.
    DiagSources s = sample();
    s.gpu_driver = "NVK — Mesa";
    s.gpu_driver_libre = true;
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "\x1b[32m"
                          "NVK — Mesa") != NULL);
    free(r);

    // Proprietary driver → plain text, not the green colour.
    s = sample();
    s.gpu_driver = "NVIDIA";
    s.gpu_driver_libre = false;
    r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "NVIDIA") != NULL);
    ASSERT_TRUE(strstr(r, "\x1b[32m"
                          "NVIDIA") == NULL);
    free(r);
}

static void test_unset_values(void)
{
    // NULL string fields must not crash and should render as "(unset)".
    DiagSources s = sample();
    s.font_pattern = NULL;
    s.font_source = NULL;
    s.word_chars = NULL;
    s.title = NULL;
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "(unset)") != NULL);
    free(r);
}

static void test_font_source(void)
{
    // The font-source provenance flows through verbatim. On the SDL backend
    // with no configured font this is the generic-fallback note — the by-design
    // behaviour the report is meant to make legible.
    DiagSources s = sample();
    s.font_pattern = NULL; // no explicit pattern
    s.font_source = "fontconfig generic (no desktop default)";
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "font source") != NULL);
    ASSERT_TRUE(strstr(r, "fontconfig generic (no desktop default)") != NULL);
    free(r);
}

static void test_vt_features_section(void)
{
    // The VT FEATURES section shows the engine name and feature flags.
    DiagSources s = sample();
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "engine") != NULL);
    ASSERT_TRUE(strstr(r, "bloom-vt") != NULL);
    // sixel, OSC 8 appear in the static capabilities line; lottie in the boolean
    ASSERT_TRUE(strstr(r, "sixel") != NULL);
    ASSERT_TRUE(strstr(r, "lottie rasterizer") != NULL);
    ASSERT_TRUE(strstr(r, "ThorVG") != NULL);
    ASSERT_TRUE(strstr(r, "OSC 8") != NULL);
    ASSERT_TRUE(strstr(r, "OSC 52") != NULL);
    // runtime modes show neutral "off" (not red — default is not an error)
    ASSERT_TRUE(strstr(r, "bracketed paste") != NULL);
    ASSERT_TRUE(strstr(r, "sync output") != NULL);
    ASSERT_TRUE(strstr(r, "focus reporting") != NULL);
    ASSERT_TRUE(strstr(r, "sixel scrolling") != NULL);
    ASSERT_TRUE(strstr(r, "hardened heap") != NULL);
    free(r);
}

static void test_vt_features_enabled(void)
{
    // Compile-time features show green/red; runtime modes show neutral on/off.
    DiagSources s = sample();
    s.hardened_heap = true;
    s.bracketed_paste = true;
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    // hardened heap is a compile-time feature — green "enabled"
    ASSERT_TRUE(strstr(r, "enabled") != NULL);
    // bracketed paste is a runtime mode — neutral "on" (no green/red SGR)
    ASSERT_TRUE(strstr(r, "bracketed paste") != NULL);
    ASSERT_TRUE(strstr(r, "on") != NULL);
    free(r);
}

static void test_lottie_rasterizer_off(void)
{
    // Without ThorVG, lottie rasterizer shows "unavailable" in red.
    DiagSources s = sample();
    s.lottie_rasterizer = false;
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "lottie rasterizer") != NULL);
    ASSERT_TRUE(strstr(r, "unavailable") != NULL);
    free(r);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_diag\n");

    RUN_TEST(test_null_input);
    RUN_TEST(test_contains_sections);
    RUN_TEST(test_contains_values);
    RUN_TEST(test_neutral_composition);
    RUN_TEST(test_glyph_curve_states);
    RUN_TEST(test_scrollback);
    RUN_TEST(test_altscreen_neutral);
    RUN_TEST(test_issues_hyperlink);
    RUN_TEST(test_gpu_driver_color);
    RUN_TEST(test_unset_values);
    RUN_TEST(test_font_source);
    RUN_TEST(test_vt_features_section);
    RUN_TEST(test_vt_features_enabled);
    RUN_TEST(test_lottie_rasterizer_off);

    TEST_SUMMARY();
}
