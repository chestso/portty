/* Test for --dpi-scale argument parsing (TDD) */
#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>

/* Inline implementation of DPI scale parsing (matches main.c logic) */
static int portty_parse_dpi_scale(const char *arg, float *out)
{
    if (!arg || !out)
        return -1;
    char *end = NULL;
    float scale = strtof(arg, &end);
    if (end == arg || *end != '\0' || scale <= 0.0f)
        return -1;
    *out = scale;
    return 0;
}

/* --- Tests --- */

static void test_dpi_scale_valid_float(void)
{
    float result = 0.0f;
    int ret = portty_parse_dpi_scale("2.0", &result);
    ASSERT_EQ(ret, 0);
    ASSERT_FLOAT_NEAR(result, 2.0f, 0.01f);
}

static void test_dpi_scale_default_one(void)
{
    float result = 0.0f;
    int ret = portty_parse_dpi_scale("1.0", &result);
    ASSERT_EQ(ret, 0);
    ASSERT_FLOAT_NEAR(result, 1.0f, 0.01f);
}

static void test_dpi_scale_half(void)
{
    float result = 0.0f;
    int ret = portty_parse_dpi_scale("0.5", &result);
    ASSERT_EQ(ret, 0);
    ASSERT_FLOAT_NEAR(result, 0.5f, 0.01f);
}

static void test_dpi_scale_large(void)
{
    float result = 0.0f;
    int ret = portty_parse_dpi_scale("4.0", &result);
    ASSERT_EQ(ret, 0);
    ASSERT_FLOAT_NEAR(result, 4.0f, 0.01f);
}

static void test_dpi_scale_invalid_negative(void)
{
    float result = 0.0f;
    int ret = portty_parse_dpi_scale("-1.0", &result);
    ASSERT_NEQ(ret, 0);
}

static void test_dpi_scale_invalid_text(void)
{
    float result = 0.0f;
    int ret = portty_parse_dpi_scale("abc", &result);
    ASSERT_NEQ(ret, 0);
}

static void test_dpi_scale_invalid_empty(void)
{
    float result = 0.0f;
    int ret = portty_parse_dpi_scale("", &result);
    ASSERT_NEQ(ret, 0);
}

static void test_dpi_scale_invalid_zero(void)
{
    float result = 0.0f;
    int ret = portty_parse_dpi_scale("0.0", &result);
    ASSERT_NEQ(ret, 0);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("Running --dpi-scale parsing tests...\n");

    RUN_TEST(test_dpi_scale_valid_float);
    RUN_TEST(test_dpi_scale_default_one);
    RUN_TEST(test_dpi_scale_half);
    RUN_TEST(test_dpi_scale_large);
    RUN_TEST(test_dpi_scale_invalid_negative);
    RUN_TEST(test_dpi_scale_invalid_text);
    RUN_TEST(test_dpi_scale_invalid_empty);
    RUN_TEST(test_dpi_scale_invalid_zero);

    TEST_SUMMARY();
}
