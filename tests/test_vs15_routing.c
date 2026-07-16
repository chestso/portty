#include "test_helpers.h"
#include "rend_common.h"
#include "unicode.h"
#include <stdlib.h>

/* --- VS15 detection --- */

static void test_unicode_cell_has_vs15(void)
{
    /* Emoji-default + VS15: ⚡︎ */
    uint32_t bolt_vs15[] = { 0x26A1, 0xFE0E, 0 };
    ASSERT_TRUE(unicode_cell_has_vs15(bolt_vs15, 8));

    /* Emoji-default + VS16 (no VS15): ⚡️ */
    uint32_t bolt_vs16[] = { 0x26A1, 0xFE0F, 0 };
    ASSERT_FALSE(unicode_cell_has_vs15(bolt_vs16, 8));

    /* Bare codepoint, no selector */
    uint32_t bare[] = { 0x26A1, 0 };
    ASSERT_FALSE(unicode_cell_has_vs15(bare, 8));

    /* Both VS15 and VS16 present */
    uint32_t both[] = { 0x26A1, 0xFE0E, 0xFE0F, 0 };
    ASSERT_TRUE(unicode_cell_has_vs15(both, 8));

    /* Empty cell */
    uint32_t empty[] = { 0 };
    ASSERT_FALSE(unicode_cell_has_vs15(empty, 8));

    /* NULL safety */
    ASSERT_FALSE(unicode_cell_has_vs15(NULL, 8));
}

/* --- Emoji routing: VS15 should NOT block color emoji --- */

static void test_vs15_routes_to_emoji_when_glyph_available(void)
{
    /* ⚡ U+26A1 is emoji-default. With VS15, the cell is 1-column wide,
     * but the color emoji font should still be used because it has the glyph. */
    uint32_t bolt_vs15[] = { 0x26A1, 0xFE0E };

    ASSERT_TRUE(rend_should_use_emoji(bolt_vs15, 2, true, true));
}

static void test_vs15_routes_to_emoji_for_emoji_default(void)
{
    /* ⭐ U+2B50 is emoji-presentation. With VS15, still route to emoji font. */
    uint32_t star_vs15[] = { 0x2B50, 0xFE0E };

    ASSERT_TRUE(rend_should_use_emoji(star_vs15, 2, true, true));
}

static void test_vs15_does_not_force_emoji_via_vs16(void)
{
    /* When both VS15 and VS16 are present, VS15 takes precedence for width.
     * The emoji font is used only if it actually has the glyph. */
    uint32_t both_vs[] = { 0x26A1, 0xFE0E, 0xFE0F };

    /* Glyph available: use emoji */
    ASSERT_TRUE(rend_should_use_emoji(both_vs, 3, true, true));

    /* Glyph not available: don't use emoji */
    ASSERT_FALSE(rend_should_use_emoji(both_vs, 3, true, false));
}

static void test_vs16_forces_emoji(void)
{
    /* ⚠ U+26A0 is ambiguous (text-default). VS16 forces emoji presentation. */
    uint32_t warn_vs16[] = { 0x26A0, 0xFE0F };

    ASSERT_TRUE(rend_should_use_emoji(warn_vs16, 2, true, false));
}

static void test_regional_indicator_routes_to_emoji(void)
{
    /* Regional indicators are always emoji, regardless of VS15. */
    uint32_t ri[] = { 0x1F1E6 };
    ASSERT_TRUE(rend_should_use_emoji(ri, 1, true, false));

    /* VS15 on a regional indicator should not block emoji (regional indicators
     * are definitionally emoji, but they use VS15-less sequences in practice) */
    uint32_t ri_vs15[] = { 0x1F1E6, 0xFE0E };
    ASSERT_FALSE(rend_should_use_emoji(ri_vs15, 2, true, false));
}

static void test_emoji_default_without_vs15(void)
{
    /* ⚡ without any selector: emoji-default, route to emoji if glyph exists */
    uint32_t bolt[] = { 0x26A1 };
    ASSERT_TRUE(rend_should_use_emoji(bolt, 1, true, true));

    /* No glyph in emoji font: don't route */
    ASSERT_FALSE(rend_should_use_emoji(bolt, 1, true, false));
}

static void test_text_default_without_vs15_stays_text(void)
{
    /* 'A' is not emoji-presentation. Without VS16, it stays on the text font
     * even if the emoji font claims to have a glyph for it. */
    uint32_t letter[] = { 0x0041 };
    ASSERT_FALSE(rend_should_use_emoji(letter, 1, true, true));
}

static void test_no_emoji_font_available(void)
{
    uint32_t bolt[] = { 0x26A1 };
    ASSERT_FALSE(rend_should_use_emoji(bolt, 1, false, false));

    uint32_t bolt_vs16[] = { 0x26A1, 0xFE0F };
    ASSERT_FALSE(rend_should_use_emoji(bolt_vs16, 2, false, false));
}

static void test_empty_or_invalid_input(void)
{
    uint32_t empty[] = { 0 };
    ASSERT_FALSE(rend_should_use_emoji(empty, 0, true, true));

    ASSERT_FALSE(rend_should_use_emoji(NULL, 0, true, true));
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_vs15_routing\n");

    printf("\nVS15 detection\n");
    RUN_TEST(test_unicode_cell_has_vs15);

    printf("\nEmoji routing with VS15\n");
    RUN_TEST(test_vs15_routes_to_emoji_when_glyph_available);
    RUN_TEST(test_vs15_routes_to_emoji_for_emoji_default);
    RUN_TEST(test_vs15_does_not_force_emoji_via_vs16);

    printf("\nEmoji routing without VS15\n");
    RUN_TEST(test_vs16_forces_emoji);
    RUN_TEST(test_regional_indicator_routes_to_emoji);
    RUN_TEST(test_emoji_default_without_vs15);
    RUN_TEST(test_text_default_without_vs15_stays_text);

    printf("\nEdge cases\n");
    RUN_TEST(test_no_emoji_font_available);
    RUN_TEST(test_empty_or_invalid_input);

    TEST_SUMMARY();
}
