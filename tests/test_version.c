/*
 * portty — Generated version header contract tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * test_version — the generated version header (portty_version.h)
 *
 * The header is the single version source: build-aux/git-version.sh's string,
 * plus the numeric quad the Windows resource script (src/portty.rc) builds
 * FILEVERSION/PRODUCTVERSION from. Splitting the string into that quad is a
 * contract, not a cosmetic: a field that drifts from the string would put a
 * version in the .exe's Properties tab that the product does not have.
 *
 * Compiling this file is itself half the pin: a field the generator left empty
 * (a tag with fewer components than X.Y.Z) or filled with something that is
 * not a number stops expanding to a number here, instead of silently mangling
 * a Windows resource.
 *
 * Pure C, no SDL/coffer — the header is all it needs.
 */

#include <stddef.h>
#include <string.h>

#include "portty_version.h"
#include "test_helpers.h"

/* One component of the version's BASE (everything before the first '-', where
 * git describe's distance/sha and the -dirty marker live), by 1-based index.
 * The rule mirrors src/Makefile.am's generator: a component's digits are its
 * value, anything else in it is dropped, and a component the string never had
 * (or one with no digits at all) reads as 0 — never as an unset macro. */
static unsigned base_field(const char *version, int index)
{
    const char *p = version;

    for (int i = 1; i < index; i++) {
        const char *dot = strchr(p, '.');
        if (!dot)
            return 0;
        p = dot + 1;
    }

    unsigned value = 0;
    int digits = 0;
    for (; *p && *p != '.' && *p != '-'; p++) {
        if (*p < '0' || *p > '9')
            continue;
        value = value * 10 + (unsigned)(*p - '0');
        digits++;
    }
    return digits ? value : 0;
}

/* The fields the .rc embeds are the STRING's own numbers: same base, same
 * components, and the git suffix never leaks into one. */
static void test_numeric_fields_agree_with_the_version_string(void)
{
    ASSERT_NOT_NULL(PORTTY_VERSION);

    /* The split's precondition: the version starts with its numeric base, so
     * the first field is never a fallback zero. */
    ASSERT_TRUE(PORTTY_VERSION[0] >= '0' && PORTTY_VERSION[0] <= '9');

    ASSERT_EQ((unsigned)PORTTY_VERSION_MAJOR, base_field(PORTTY_VERSION, 1));
    ASSERT_EQ((unsigned)PORTTY_VERSION_MINOR, base_field(PORTTY_VERSION, 2));
    ASSERT_EQ((unsigned)PORTTY_VERSION_PATCH, base_field(PORTTY_VERSION, 3));

    /* And the base is what the string says it is: X.Y.Z, so the quad the .rc
     * writes (X, Y, Z, 0) is the product's version. */
    const char *base_end = strchr(PORTTY_VERSION, '-');
    size_t base_len = base_end ? (size_t)(base_end - PORTTY_VERSION)
                               : strlen(PORTTY_VERSION);
    ASSERT_TRUE(base_len > 0);
    ASSERT_TRUE(memchr(PORTTY_VERSION, '.', base_len) != NULL);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_numeric_fields_agree_with_the_version_string);

    TEST_SUMMARY();
}
