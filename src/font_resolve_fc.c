#include "font_resolve_fc.h"
#include "common.h"
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Forward declarations
static char *find_font_with_pattern(FcConfig *fc_config, const char *pattern,
                                    char **family_name, float *out_size);
static int is_fixed_width_font(const char *font_path);

// Helper function to find font using fontconfig
static char *find_font_with_pattern(FcConfig *fc_config, const char *pattern,
                                    char **family_name, float *out_size)
{
    FcPattern *pat;
    char *font_path = NULL;

    vlog("Attempting to find font with pattern: '%s'\n", pattern);

    pat = FcNameParse((FcChar8 *)pattern);
    if (!pat) {
        vlog("Failed to parse font pattern: '%s'\n", pattern);
        return NULL;
    }

    // Extract size from parsed pattern before substitution overwrites it
    if (out_size) {
        double parsed_size = 0;
        if (FcPatternGetDouble(pat, FC_SIZE, 0, &parsed_size) == FcResultMatch && parsed_size > 0)
            *out_size = (float)parsed_size;
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

static bool fc_init(FontResolveBackend *resolve)
{
    FcBool result = FcInit();
    if (!result) {
        vlog("Failed to initialize fontconfig\n");
        return false;
    }

    FcConfig *fc_config = FcConfigGetCurrent();
    if (!fc_config) {
        vlog("Failed to get fontconfig current config\n");
        return false;
    }

    resolve->backend_data = fc_config;
    return true;
}

static void fc_destroy(FontResolveBackend *resolve)
{
    if (resolve->backend_data) {
        FcFini();
        resolve->backend_data = NULL;
    }
}

static int fc_find_font(FontResolveBackend *resolve, FontType type,
                        const char *family, FontResolutionResult *result)
{
    if (!result)
        return -1;

    FcConfig *fc_config = (FcConfig *)resolve->backend_data;

    // Initialize result
    result->font_path = NULL;
    result->family_name = NULL;
    result->size = 0; // 0 = not specified in pattern

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
    case FONT_TYPE_FALLBACK:
        vlog("FONT_TYPE_FALLBACK should use font_resolve_find_font_for_codepoint()\n");
        return -1;
    default:
        vlog("Invalid font type requested\n");
        return -1;
    }

    char *font_path = NULL;
    char *family_name = NULL;

    font_path = find_font_with_pattern(fc_config, pattern, &family_name, &result->size);
    if (!font_path) {
        vlog("Failed to find font for pattern: %s\n", pattern);
        return -1;
    }

    result->font_path = font_path;
    result->family_name = family_name;
    vlog("font_resolve_find_font: resolved type=%d to path='%s' family='%s'\n", type, result->font_path, result->family_name ? result->family_name : "(null)");
    return 0;
}

static int fc_find_font_for_codepoint(FontResolveBackend *resolve,
                                      uint32_t codepoint, FontResolutionResult *result)
{
    if (!result || !resolve->backend_data)
        return -1;

    FcConfig *fc_config = (FcConfig *)resolve->backend_data;

    result->font_path = NULL;
    result->family_name = NULL;
    result->size = 0;

    FcCharSet *cs = FcCharSetCreate();
    if (!cs)
        return -1;
    FcCharSetAddChar(cs, codepoint);

    FcPattern *pat = FcPatternCreate();
    if (!pat) {
        FcCharSetDestroy(cs);
        return -1;
    }
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);

    FcConfigSubstitute(fc_config, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult fc_result;
    FcPattern *match = FcFontMatch(fc_config, pat, &fc_result);
    int ret = -1;

    if (match) {
        FcChar8 *file = NULL;
        FcChar8 *family = NULL;

        // Verify the matched font actually contains the codepoint
        FcCharSet *match_cs = NULL;
        int has_char = 0;
        if (FcPatternGetCharSet(match, FC_CHARSET, 0, &match_cs) == FcResultMatch && match_cs)
            has_char = FcCharSetHasChar(match_cs, codepoint);

        if (has_char &&
            FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
            result->font_path = strdup((char *)file);
            if (FcPatternGetString(match, FC_FAMILY, 0, &family) == FcResultMatch)
                result->family_name = strdup((char *)family);
            vlog("Fallback font for U+%04X: %s (%s)\n", codepoint,
                 result->font_path, result->family_name ? result->family_name : "unknown");
            ret = 0;
        } else {
            vlog("No font found containing U+%04X\n", codepoint);
        }
        FcPatternDestroy(match);
    }

    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);
    return ret;
}

static void fc_list_monospace(FontResolveBackend *resolve)
{
    FcConfig *fc_config = (FcConfig *)resolve->backend_data;

    // List all installed fonts, then filter to monospace
    FcPattern *pat = FcPatternCreate();
    if (!pat)
        return;

    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, FC_FILE, FC_CHARSET, NULL);
    if (!os) {
        FcPatternDestroy(pat);
        return;
    }

    FcFontSet *fs = FcFontList(fc_config, pat, os);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    if (!fs)
        return;

    int capacity = 64;
    char **names = malloc(sizeof(char *) * capacity);
    int count = 0;
    if (!names) {
        FcFontSetDestroy(fs);
        return;
    }

    for (int i = 0; i < fs->nfont; i++) {
        FcChar8 *family = NULL;
        FcChar8 *file = NULL;
        FcCharSet *cs = NULL;

        if (FcPatternGetString(fs->fonts[i], FC_FAMILY, 0, &family) != FcResultMatch)
            continue;
        if (FcPatternGetString(fs->fonts[i], FC_FILE, 0, &file) != FcResultMatch)
            continue;

        // Must contain Latin 'A' (U+0041)
        if (FcPatternGetCharSet(fs->fonts[i], FC_CHARSET, 0, &cs) != FcResultMatch || !cs)
            continue;
        if (!FcCharSetHasChar(cs, 0x0041))
            continue;

        // Deduplicate by family name before expensive FreeType check
        int dup = 0;
        for (int j = 0; j < count; j++) {
            if (strcasecmp(names[j], (char *)family) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;

        // Verify the font is actually fixed-width
        if (!is_fixed_width_font((const char *)file))
            continue;

        if (count >= capacity) {
            capacity *= 2;
            char **tmp = realloc(names, sizeof(char *) * capacity);
            if (!tmp) {
                for (int j = 0; j < count; j++)
                    free(names[j]);
                free(names);
                FcFontSetDestroy(fs);
                return;
            }
            names = tmp;
        }
        names[count++] = strdup((char *)family);
    }
    FcFontSetDestroy(fs);

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

FontResolveBackend font_resolve_backend_fc = {
    .name = "fontconfig",
    .backend_data = NULL,
    .init = fc_init,
    .destroy = fc_destroy,
    .find_font = fc_find_font,
    .find_font_for_codepoint = fc_find_font_for_codepoint,
    .list_monospace = fc_list_monospace,
};
