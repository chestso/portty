/*
 * portty — Content scale calculation tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/* Test for content scale calculation (TDD) */
#include "test_helpers.h"
#include <stdio.h>

/* Inline implementation matching portty_app.c */
static float portty_compute_content_scale(float system_scale, float user_scale)
{
    float scale = system_scale;
    if (user_scale != 1.0f && scale > 0.0f)
        scale *= user_scale;
    return scale > 0.0f ? scale : 1.0f;
}

/* --- Tests --- */

static void test_content_scale_system_only(void)
{
    float result = portty_compute_content_scale(1.5f, 1.0f);
    ASSERT_FLOAT_NEAR(result, 1.5f, 0.01f);
}

static void test_content_scale_user_only(void)
{
    float result = portty_compute_content_scale(1.0f, 2.0f);
    ASSERT_FLOAT_NEAR(result, 2.0f, 0.01f);
}

static void test_content_scale_both(void)
{
    float result = portty_compute_content_scale(1.5f, 2.0f);
    ASSERT_FLOAT_NEAR(result, 3.0f, 0.01f);
}

static void test_content_scale_no_scaling(void)
{
    float result = portty_compute_content_scale(1.0f, 1.0f);
    ASSERT_FLOAT_NEAR(result, 1.0f, 0.01f);
}

static void test_content_scale_fractional(void)
{
    float result = portty_compute_content_scale(1.25f, 1.5f);
    ASSERT_FLOAT_NEAR(result, 1.875f, 0.01f);
}

static void test_content_scale_zero_system(void)
{
    /* System returning 0 should default to 1.0 */
    float result = portty_compute_content_scale(0.0f, 2.0f);
    ASSERT_FLOAT_NEAR(result, 1.0f, 0.01f);
}

static void test_content_scale_negative_system(void)
{
    /* System returning negative should default to 1.0 */
    float result = portty_compute_content_scale(-1.0f, 2.0f);
    ASSERT_FLOAT_NEAR(result, 1.0f, 0.01f);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("Running content scale calculation tests...\n");

    RUN_TEST(test_content_scale_system_only);
    RUN_TEST(test_content_scale_user_only);
    RUN_TEST(test_content_scale_both);
    RUN_TEST(test_content_scale_no_scaling);
    RUN_TEST(test_content_scale_fractional);
    RUN_TEST(test_content_scale_zero_system);
    RUN_TEST(test_content_scale_negative_system);

    TEST_SUMMARY();
}
