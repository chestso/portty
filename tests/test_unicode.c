#include "test_helpers.h"
#include "unicode.h"
#include <stdlib.h>

/* --- Emoji range detection --- */

static void test_emoji_base_range(void)
{
    /* Inside ranges */
    ASSERT_TRUE(is_emoji_base_range(0x1F600));  /* grinning face */
    ASSERT_TRUE(is_emoji_base_range(0x1F680));  /* rocket */
    ASSERT_TRUE(is_emoji_base_range(0x1F300));  /* cyclone */
    ASSERT_TRUE(is_emoji_base_range(0x1F9D1));  /* person */
    ASSERT_TRUE(is_emoji_base_range(0x1FA70));  /* ballet shoes */

    /* Outside ranges */
    ASSERT_FALSE(is_emoji_base_range(0x0041));  /* 'A' */
    ASSERT_FALSE(is_emoji_base_range(0x2600));  /* sun — ambiguous, not base */
    ASSERT_FALSE(is_emoji_base_range(0x1F1E6)); /* regional indicator */
}

static void test_ambiguous_emoji(void)
{
    ASSERT_TRUE(is_ambiguous_emoji(0x2600));   /* sun */
    ASSERT_TRUE(is_ambiguous_emoji(0x231A));   /* watch */
    ASSERT_TRUE(is_ambiguous_emoji(0x2328));   /* keyboard */
    ASSERT_TRUE(is_ambiguous_emoji(0x23E9));   /* fast forward */

    ASSERT_FALSE(is_ambiguous_emoji(0x0041));  /* 'A' */
    ASSERT_FALSE(is_ambiguous_emoji(0x1F1E6)); /* regional indicator */

    /* Emoji_Presentation=Yes ranges belong in is_emoji_base_range, not here */
    ASSERT_FALSE(is_ambiguous_emoji(0x1F600)); /* grinning face */
    ASSERT_FALSE(is_ambiguous_emoji(0x1F300)); /* cyclone */
    ASSERT_FALSE(is_ambiguous_emoji(0x1F680)); /* rocket */
}

static void test_emoji_presentation(void)
{
    /* Should include both base range and ambiguous */
    ASSERT_TRUE(is_emoji_presentation(0x1F600));  /* base range */
    ASSERT_TRUE(is_emoji_presentation(0x2600));    /* ambiguous */

    ASSERT_FALSE(is_emoji_presentation(0x0041));   /* 'A' */
}

/* --- Codepoint classification --- */

static void test_zwj(void)
{
    ASSERT_TRUE(is_zwj(0x200D));
    ASSERT_FALSE(is_zwj(0x200C));
    ASSERT_FALSE(is_zwj(0x200E));
    ASSERT_FALSE(is_zwj(0x0000));
}

static void test_skin_tone_modifier(void)
{
    ASSERT_TRUE(is_skin_tone_modifier(0x1F3FB));   /* light skin */
    ASSERT_TRUE(is_skin_tone_modifier(0x1F3FF));   /* dark skin */
    ASSERT_TRUE(is_skin_tone_modifier(0x1F3FD));   /* medium */

    ASSERT_FALSE(is_skin_tone_modifier(0x1F3FA));  /* just below */
    ASSERT_FALSE(is_skin_tone_modifier(0x1F400));  /* just above */
}

static void test_regional_indicator(void)
{
    ASSERT_TRUE(is_regional_indicator(0x1F1E6));   /* A */
    ASSERT_TRUE(is_regional_indicator(0x1F1FF));   /* Z */

    ASSERT_FALSE(is_regional_indicator(0x1F1E5));  /* below range */
    ASSERT_FALSE(is_regional_indicator(0x1F200));  /* above range */
}

/* --- UTF-8 decoding --- */

static void test_utf8_ascii(void)
{
    uint32_t out[16];
    int n = utf8_to_codepoints("ABC", out, 16);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(out[0], 'A');
    ASSERT_EQ(out[1], 'B');
    ASSERT_EQ(out[2], 'C');
}

static void test_utf8_multibyte(void)
{
    uint32_t out[16];
    /* U+00E9 = é = 0xC3 0xA9 (2-byte) */
    int n = utf8_to_codepoints("\xC3\xA9", out, 16);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(out[0], 0x00E9);
}

static void test_utf8_3byte(void)
{
    uint32_t out[16];
    /* U+2603 = snowman = 0xE2 0x98 0x83 */
    int n = utf8_to_codepoints("\xE2\x98\x83", out, 16);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(out[0], 0x2603);
}

static void test_utf8_4byte_emoji(void)
{
    uint32_t out[16];
    /* U+1F600 = grinning face = 0xF0 0x9F 0x98 0x80 */
    int n = utf8_to_codepoints("\xF0\x9F\x98\x80", out, 16);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(out[0], 0x1F600);
}

static void test_utf8_empty(void)
{
    uint32_t out[16];
    int n = utf8_to_codepoints("", out, 16);
    ASSERT_EQ(n, 0);
}

static void test_utf8_invalid(void)
{
    uint32_t out[16];
    /* Invalid continuation byte */
    int n = utf8_to_codepoints("\xFF", out, 16);
    ASSERT_EQ(n, -1);
}

static void test_utf8_max_out_truncation(void)
{
    uint32_t out[2];
    int n = utf8_to_codepoints("ABCDE", out, 2);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(out[0], 'A');
    ASSERT_EQ(out[1], 'B');
}

static void test_utf8_mixed(void)
{
    uint32_t out[16];
    /* "Aé😀" = 'A' + U+00E9 + U+1F600 */
    int n = utf8_to_codepoints("A\xC3\xA9\xF0\x9F\x98\x80", out, 16);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(out[0], 'A');
    ASSERT_EQ(out[1], 0x00E9);
    ASSERT_EQ(out[2], 0x1F600);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_unicode\n");

    RUN_TEST(test_emoji_base_range);
    RUN_TEST(test_ambiguous_emoji);
    RUN_TEST(test_emoji_presentation);
    RUN_TEST(test_zwj);
    RUN_TEST(test_skin_tone_modifier);
    RUN_TEST(test_regional_indicator);

    printf("\nUTF-8 decoding\n");

    RUN_TEST(test_utf8_ascii);
    RUN_TEST(test_utf8_multibyte);
    RUN_TEST(test_utf8_3byte);
    RUN_TEST(test_utf8_4byte_emoji);
    RUN_TEST(test_utf8_empty);
    RUN_TEST(test_utf8_invalid);
    RUN_TEST(test_utf8_max_out_truncation);
    RUN_TEST(test_utf8_mixed);

    TEST_SUMMARY();
}
