#include "font_resolve_fc.h"
#include "common.h"
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// True when at least one installed font advertises this exact family name.
static bool family_is_installed(FcConfig *fc_config, const char *family)
{
    FcPattern *pat = FcPatternBuild(NULL, FC_FAMILY, FcTypeString, family, (char *)0);
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, (char *)0);
    FcFontSet *set = FcFontList(fc_config, pat, os);
    bool installed = set && set->nfont > 0;
    if (set)
        FcFontSetDestroy(set);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    return installed;
}

// True when fontconfig has an <alias> rule for this name. Alias rules with
// <prefer> bindings insert preferred families ahead of the requested name in
// the family list, so after FcMatchPattern substitution the requested family
// no longer occupies index 0. A typo or unrecognised name keeps its slot at
// index 0 (with generic sans-serif fallbacks merely appended after it).
static bool family_is_alias(FcConfig *fc_config, const char *family)
{
    FcPattern *pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    FcConfigSubstitute(fc_config, pat, FcMatchPattern);
    FcChar8 *first = NULL;
    bool aliased = false;
    if (FcPatternGetString(pat, FC_FAMILY, 0, &first) == FcResultMatch && first)
        aliased = strcasecmp((const char *)first, family) != 0;
    FcPatternDestroy(pat);
    return aliased;
}

// Helper function to find font using fontconfig
static char *find_font_with_pattern(FcConfig *fc_config, const char *pattern,
                                    char **family_name, float *out_size,
                                    bool *substituted)
{
    FcPattern *pat;
    char *font_path = NULL;

    if (substituted)
        *substituted = false;

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

    // Capture the requested family before substitution so we can cross-ref
    // it against fontconfig's installed-font list and alias rules.
    char *requested_family = NULL;
    {
        FcChar8 *fam = NULL;
        if (FcPatternGetString(pat, FC_FAMILY, 0, &fam) == FcResultMatch && fam)
            requested_family = strdup((char *)fam);
    }

    if (substituted && requested_family) {
        bool installed = family_is_installed(fc_config, requested_family);
        bool alias = !installed && family_is_alias(fc_config, requested_family);
        *substituted = !installed && !alias;
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

            FcChar8 *matched = NULL;
            if (FcPatternGetString(match, FC_FAMILY, 0, &matched) != FcResultMatch)
                matched = NULL;

            if (family_name) {
                if (matched) {
                    *family_name = strdup((const char *)matched);
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

    free(requested_family);
    FcPatternDestroy(pat);

    return font_path;
}

// Shared utility in font_resolve.c — declaration in font_resolve.h
// int font_resolve_is_fixed_width(const char *font_path);

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
    const char *fallback_pattern = NULL;
    char styled_pattern[256];
    switch (type) {
    case FONT_TYPE_NORMAL:
        pattern = family ? family : "monospace";
        fallback_pattern = "monospace";
        break;
    case FONT_TYPE_BOLD:
        if (family) {
            snprintf(styled_pattern, sizeof(styled_pattern), "%s:weight=bold", family);
            pattern = styled_pattern;
        } else {
            pattern = "monospace:weight=bold";
        }
        fallback_pattern = "monospace:weight=bold";
        break;
    case FONT_TYPE_ITALIC:
        if (family) {
            snprintf(styled_pattern, sizeof(styled_pattern), "%s:slant=italic", family);
            pattern = styled_pattern;
        } else {
            pattern = "monospace:slant=italic";
        }
        fallback_pattern = "monospace:slant=italic";
        break;
    case FONT_TYPE_BOLD_ITALIC:
        if (family) {
            snprintf(styled_pattern, sizeof(styled_pattern), "%s:weight=bold:slant=italic", family);
            pattern = styled_pattern;
        } else {
            pattern = "monospace:weight=bold:slant=italic";
        }
        fallback_pattern = "monospace:weight=bold:slant=italic";
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

    bool substituted = false;
    char *font_path = NULL;
    char *family_name = NULL;

    font_path = find_font_with_pattern(fc_config, pattern, &family_name,
                                       &result->size, &substituted);

    // The user named a family that fontconfig neither has installed nor
    // recognises as an alias — almost certainly a typo. Warn and retry against
    // the system monospace alias so the terminal still renders with sane metrics.
    if (font_path && family && substituted && fallback_pattern) {
        // Only emit the user-facing warning once (on the normal style); bold/
        // italic/bold-italic queries hit the same missing family and would
        // otherwise log the same warning up to four times.
        if (type == FONT_TYPE_NORMAL) {
            fprintf(stderr,
                    "WARNING: font family '%s' is not installed and not a known fontconfig alias; falling back to system default monospace\n",
                    family);
        }
        free(font_path);
        free(family_name);
        font_path = NULL;
        family_name = NULL;
        result->size = 0;
        font_path = find_font_with_pattern(fc_config, fallback_pattern,
                                           &family_name, &result->size, NULL);
    }

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
        if (!font_resolve_is_fixed_width((const char *)file))
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
