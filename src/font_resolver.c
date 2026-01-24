#include "font_resolver.h"
#include "common.h"
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Forward declarations
static char *find_font_with_pattern(const char *pattern, char **family_name);
static int is_fixed_width_font(const char *font_path);

// Global fontconfig configuration
static FcConfig *fc_config = NULL;

// Helper function to find font using fontconfig
static char *find_font_with_pattern(const char *pattern, char **family_name)
{
    FcPattern *pat;
    char *font_path = NULL;

    vlog("Attempting to find font with pattern: '%s'\n", pattern);

    pat = FcNameParse((FcChar8 *)pattern);
    if (!pat) {
        vlog("Failed to parse font pattern: '%s'\n", pattern);
        return NULL;
    }

    FcConfigSubstitute(fc_config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(fc_config, pat, &result);
    if (match) {
        FcChar8 *file;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            font_path = strdup((char *)file);
            vlog("Found font file: '%s'\n", font_path);

            // Get family name if requested
            if (family_name) {
                FcChar8 *family;
                if (FcPatternGetString(match, FC_FAMILY, 0, &family) == FcResultMatch) {
                    *family_name = strdup((char *)family);
                } else {
                    *family_name = strdup(pattern);
                }
            }
        } else {
            vlog("No font file found for pattern: '%s'\n", pattern);
        }
        FcPatternDestroy(match);
    } else {
        vlog("No matching font found for pattern: '%s'\n", pattern);
    }

    FcPatternDestroy(pat);

    return font_path;
}

// Check if a font file is actually monospace by comparing glyph advances
static int is_fixed_width_font(const char *font_path)
{
    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0)
        return 0;

    FT_Face face;
    if (FT_New_Face(lib, font_path, 0, &face) != 0) {
        FT_Done_FreeType(lib);
        return 0;
    }

    FT_Set_Char_Size(face, 0, 16 * 64, 72, 72);

    FT_UInt gi_i = FT_Get_Char_Index(face, 'i');
    FT_UInt gi_M = FT_Get_Char_Index(face, 'M');
    int fixed = 0;

    if (gi_i && gi_M &&
        FT_Load_Glyph(face, gi_i, FT_LOAD_NO_HINTING) == 0) {
        FT_Fixed adv_i = face->glyph->linearHoriAdvance;
        if (FT_Load_Glyph(face, gi_M, FT_LOAD_NO_HINTING) == 0) {
            FT_Fixed adv_M = face->glyph->linearHoriAdvance;
            fixed = (adv_i == adv_M);
        }
    }

    FT_Done_Face(face);
    FT_Done_FreeType(lib);
    return fixed;
}

int font_resolver_init()
{
    // Initialize fontconfig
    FcBool result = FcInit();
    if (!result) {
        vlog("Failed to initialize fontconfig\n");
        return -1;
    }

    // Get the default config
    fc_config = FcConfigGetCurrent();
    if (!fc_config) {
        vlog("Failed to get fontconfig current config\n");
        return -1;
    }

    return 0;
}

int font_resolver_find_font(FontType type, const char *family, FontResolutionResult *result)
{
    if (!result) {
        return -1;
    }

    // Initialize result
    result->font_path = NULL;
    result->family_name = NULL;
    result->size = 24.0f; // Default size

    const char *pattern = NULL;
    char bold_pattern[256];
    switch (type) {
    case FONT_TYPE_NORMAL:
        pattern = family ? family : "monospace";
        break;
    case FONT_TYPE_BOLD:
        if (family) {
            snprintf(bold_pattern, sizeof(bold_pattern), "%s:weight=bold", family);
            pattern = bold_pattern;
        } else {
            pattern = "monospace:weight=bold";
        }
        break;
    case FONT_TYPE_EMOJI:
        pattern = "emoji";
        break;
    default:
        vlog("Invalid font type requested\n");
        return -1;
    }

    char *font_path = NULL;
    char *family_name = NULL;

    font_path = find_font_with_pattern(pattern, &family_name);
    if (!font_path) {
        vlog("Failed to find font for pattern: %s\n", pattern);
        return -1;
    }

    result->font_path = font_path;
    result->family_name = family_name;
    vlog("font_resolver_find_font: resolved type=%d to path='%s' family='%s'\n", type, result->font_path, result->family_name ? result->family_name : "(null)");
    return 0;
}

void font_resolver_list_monospace()
{
    // Substitute "monospace" to expand into all aliased families
    FcPattern *pat = FcNameParse((FcChar8 *)"monospace");
    if (!pat)
        return;

    FcConfigSubstitute(fc_config, pat, FcMatchPattern);

    // Extract all family names from the substituted pattern
    int capacity = 64;
    char **names = malloc(sizeof(char *) * capacity);
    int count = 0;
    if (!names) {
        FcPatternDestroy(pat);
        return;
    }

    FcChar8 *family = NULL;
    for (int i = 0; FcPatternGetString(pat, FC_FAMILY, i, &family) == FcResultMatch; i++) {
        // Skip the generic "monospace" alias itself
        if (strcasecmp((char *)family, "monospace") == 0)
            continue;

        // Verify the font is actually installed (resolves to itself)
        FcPattern *check = FcNameParse(family);
        if (!check)
            continue;
        FcConfigSubstitute(fc_config, check, FcMatchPattern);
        FcDefaultSubstitute(check);

        FcResult res;
        FcPattern *match = FcFontMatch(fc_config, check, &res);
        FcPatternDestroy(check);
        if (!match)
            continue;

        // Check that resolved family matches what we asked for
        FcChar8 *matched_family = NULL;
        int family_matches = 0;
        if (FcPatternGetString(match, FC_FAMILY, 0, &matched_family) == FcResultMatch) {
            family_matches = (strcasecmp((char *)family, (char *)matched_family) == 0);
        }

        // Check charset for Latin 'A'
        FcCharSet *cs = NULL;
        int has_latin = 0;
        if (family_matches && FcPatternGetCharSet(match, FC_CHARSET, 0, &cs) == FcResultMatch && cs) {
            has_latin = FcCharSetHasChar(cs, 0x0041);
        }

        // Verify the font is actually fixed-width
        int is_mono = 0;
        FcChar8 *file = NULL;
        if (family_matches && has_latin &&
            FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            is_mono = is_fixed_width_font((const char *)file);
        }
        FcPatternDestroy(match);

        if (!family_matches || !has_latin || !is_mono)
            continue;

        // Deduplicate
        int dup = 0;
        for (int j = 0; j < count; j++) {
            if (strcasecmp(names[j], (char *)family) == 0) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            if (count >= capacity) {
                capacity *= 2;
                names = realloc(names, sizeof(char *) * capacity);
                if (!names) {
                    FcPatternDestroy(pat);
                    return;
                }
            }
            names[count++] = strdup((char *)family);
        }
    }
    FcPatternDestroy(pat);

    // Sort alphabetically
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcasecmp(names[i], names[j]) > 0) {
                char *tmp = names[i];
                names[i] = names[j];
                names[j] = tmp;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        printf("%s\n", names[i]);
        free(names[i]);
    }
    free(names);
}

void font_resolver_free_result(FontResolutionResult *result)
{
    if (result) {
        if (result->font_path) {
            free(result->font_path);
            result->font_path = NULL;
        }
        if (result->family_name) {
            free(result->family_name);
            result->family_name = NULL;
        }
    }
}

void font_resolver_cleanup()
{
    if (fc_config) {
        FcFini();
        fc_config = NULL;
    }
}
