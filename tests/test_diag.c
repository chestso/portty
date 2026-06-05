#include "bloom_version.h"
#include "diag.h"
#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A fully-populated sample so the report exercises every section.
static DiagSources sample(void)
{
    DiagSources s = {
        .renderer_name = "test-gpu",
        .linear_light = true,
        .glyph_shader = true,
        .content_scale = 1.5f,
        .pixel_width = 1280,
        .pixel_height = 720,
        .cell_width = 10,
        .cell_height = 22,
        .cols = 128,
        .rows = 32,
        .config_path = "/tmp/test/bloom.conf",
        .font_pattern = "Cascadia Code-14",
        .font_path = "/usr/share/fonts/test.ttf",
        .hinting = "light",
        .scrollback = 1000,
        .text_gamma = 1.7f,
        .text_contrast = 30.0f,
        .word_chars = "A-Za-z0-9_",
        .platform_name = "sdl3",
        .term_env = "bloom-terminal-vty-256color",
        .colorterm_env = "truecolor",
        .pager_env = "less",
        .lang_env = "en_US.UTF-8",
        .title = "bash",
        .altscreen = false,
        .mouse_mode = 0,
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
    ASSERT_TRUE(strstr(r, "SESSION") != NULL);
    ASSERT_TRUE(strstr(r, "SYSTEM") != NULL);
    free(r);
}

static void test_contains_values(void)
{
    DiagSources s = sample();
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    // Version string from the build (bloom_version.h).
    ASSERT_TRUE(strstr(r, BLOOM_TERMINAL_VERSION) != NULL);
    // A few runtime values flow through verbatim.
    ASSERT_TRUE(strstr(r, "test-gpu") != NULL);
    ASSERT_TRUE(strstr(r, "Cascadia Code-14") != NULL);
    ASSERT_TRUE(strstr(r, "/usr/share/fonts/test.ttf") != NULL);
    ASSERT_TRUE(strstr(r, "128 cols x 32 rows") != NULL);
    ASSERT_TRUE(strstr(r, "bloom-terminal-vty-256color") != NULL);
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

static void test_unset_values(void)
{
    // NULL string fields must not crash and should render as "(unset)".
    DiagSources s = sample();
    s.font_pattern = NULL;
    s.word_chars = NULL;
    s.title = NULL;
    char *r = diag_build_report(&s);
    ASSERT_NOT_NULL(r);
    ASSERT_TRUE(strstr(r, "(unset)") != NULL);
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
    RUN_TEST(test_unset_values);

    TEST_SUMMARY();
}
