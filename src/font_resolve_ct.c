#ifdef __APPLE__

#include "font_resolve_ct.h"
#include "common.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreText/CoreText.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- Pattern parsing (same format as Fontconfig/Win32) --- */

typedef struct
{
    char family[256];
    float size;
    int bold;
    int italic;
} ParsedPattern;

static void parse_pattern(const char *pattern, ParsedPattern *out)
{
    memset(out, 0, sizeof(*out));

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", pattern);

    /* Split on first ':' */
    char *props = strchr(buf, ':');
    if (props) {
        *props++ = '\0';

        char *tok = props;
        while (tok && *tok) {
            char *next = strchr(tok, ':');
            if (next)
                *next++ = '\0';

            if (strncmp(tok, "weight=", 7) == 0) {
                if (strcasecmp(tok + 7, "bold") == 0)
                    out->bold = 1;
            } else if (strncmp(tok, "slant=", 6) == 0) {
                if (strcasecmp(tok + 6, "italic") == 0 ||
                    strcasecmp(tok + 6, "oblique") == 0)
                    out->italic = 1;
            } else if (strncmp(tok, "size=", 5) == 0) {
                out->size = (float)atof(tok + 5);
            }
            tok = next;
        }
    }

    /* Parse family-size (e.g., "Menlo-14") */
    char *dash = strrchr(buf, '-');
    if (dash && dash > buf && dash[1] >= '0' && dash[1] <= '9') {
        out->size = (float)atof(dash + 1);
        *dash = '\0';
    }

    /* Map "monospace" to a macOS system font */
    if (strcasecmp(buf, "monospace") == 0) {
        static const char *defaults[] = {
            "SF Mono", "Menlo", "Monaco", "Courier", NULL
        };
        /* Try to find one that exists */
        for (int i = 0; defaults[i]; i++) {
            CFStringRef name =
                CFStringCreateWithCString(NULL, defaults[i],
                                          kCFStringEncodingUTF8);
            CTFontRef test = CTFontCreateWithName(name, 12.0, NULL);
            CFRelease(name);
            if (test) {
                /* Verify it actually matched the requested family */
                CFStringRef matched = CTFontCopyFamilyName(test);
                char matched_buf[256];
                CFStringGetCString(matched, matched_buf,
                                   sizeof(matched_buf),
                                   kCFStringEncodingUTF8);
                CFRelease(matched);
                CFRelease(test);
                if (strcasecmp(matched_buf, defaults[i]) == 0) {
                    snprintf(out->family, sizeof(out->family), "%s",
                             defaults[i]);
                    return;
                }
            }
        }
        snprintf(out->family, sizeof(out->family), "Courier");
    } else {
        snprintf(out->family, sizeof(out->family), "%s", buf);
    }
}

/* --- Core Text helpers --- */

/* Extract the file path from a CTFont, returning a malloc'd string or NULL */
static char *ct_font_get_path(CTFontRef font)
{
    CTFontDescriptorRef desc = CTFontCopyFontDescriptor(font);
    if (!desc)
        return NULL;

    CFURLRef url =
        CTFontDescriptorCopyAttribute(desc, kCTFontURLAttribute);
    CFRelease(desc);
    if (!url)
        return NULL;

    char path[1024];
    Boolean ok =
        CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path,
                                         sizeof(path));
    CFRelease(url);

    return ok ? strdup(path) : NULL;
}

/* Extract the family name from a CTFont, returning a malloc'd string or NULL */
static char *ct_font_get_family(CTFontRef font)
{
    CFStringRef name = CTFontCopyFamilyName(font);
    if (!name)
        return NULL;

    char buf[256];
    Boolean ok = CFStringGetCString(name, buf, sizeof(buf),
                                    kCFStringEncodingUTF8);
    CFRelease(name);

    return ok ? strdup(buf) : NULL;
}

/* Create a CTFont matching a family name and symbolic traits */
static CTFontRef ct_create_font(const char *family, float size,
                                CTFontSymbolicTraits traits)
{
    if (size <= 0)
        size = 12.0;

    CFStringRef fam_str =
        CFStringCreateWithCString(NULL, family, kCFStringEncodingUTF8);

    CTFontRef base = CTFontCreateWithName(fam_str, size, NULL);
    CFRelease(fam_str);
    if (!base)
        return NULL;

    if (traits == 0)
        return base;

    /* Request bold/italic variant */
    CTFontRef styled =
        CTFontCreateCopyWithSymbolicTraits(base, size, NULL, traits,
                                           traits);
    CFRelease(base);

    return styled; /* may be NULL if no styled variant exists */
}

/* --- Backend implementation --- */

static bool ct_init(FontResolveBackend *resolve)
{
    (void)resolve;
    return true;
}

static void ct_destroy(FontResolveBackend *resolve)
{
    (void)resolve;
}

static int ct_find_font(FontResolveBackend *resolve, FontType type,
                        const char *pattern,
                        FontResolutionResult *result)
{
    (void)resolve;

    result->font_path = NULL;
    result->family_name = NULL;
    result->size = 0;

    /* Handle emoji directly */
    if (type == FONT_TYPE_EMOJI) {
        CTFontRef emoji =
            CTFontCreateWithName(CFSTR("Apple Color Emoji"), 12.0,
                                 NULL);
        if (!emoji)
            return -1;
        result->font_path = ct_font_get_path(emoji);
        result->family_name = ct_font_get_family(emoji);
        CFRelease(emoji);
        return result->font_path ? 0 : -1;
    }

    if (type == FONT_TYPE_FALLBACK)
        return -1;

    /* Parse the pattern */
    ParsedPattern pp;
    parse_pattern(pattern ? pattern : "monospace", &pp);

    /* Map FontType to symbolic traits */
    CTFontSymbolicTraits traits = 0;
    switch (type) {
    case FONT_TYPE_BOLD:
        traits = kCTFontBoldTrait;
        break;
    case FONT_TYPE_ITALIC:
        traits = kCTFontItalicTrait;
        break;
    case FONT_TYPE_BOLD_ITALIC:
        traits = kCTFontBoldTrait | kCTFontItalicTrait;
        break;
    default:
        if (pp.bold && pp.italic)
            traits = kCTFontBoldTrait | kCTFontItalicTrait;
        else if (pp.bold)
            traits = kCTFontBoldTrait;
        else if (pp.italic)
            traits = kCTFontItalicTrait;
        break;
    }

    CTFontRef font = ct_create_font(pp.family, pp.size, traits);
    if (!font)
        return -1;

    result->font_path = ct_font_get_path(font);
    result->family_name = ct_font_get_family(font);
    result->size = pp.size;
    CFRelease(font);

    if (!result->font_path) {
        free(result->family_name);
        result->family_name = NULL;
        return -1;
    }

    vlog("Core Text font resolver: %s traits=0x%x -> %s\n",
         pp.family, (unsigned)traits, result->font_path);

    return 0;
}

static int ct_find_font_for_codepoint(FontResolveBackend *resolve,
                                      uint32_t codepoint,
                                      FontResolutionResult *result)
{
    (void)resolve;

    result->font_path = NULL;
    result->family_name = NULL;
    result->size = 0;

    /* Create a base font for fallback lookup */
    CTFontRef base = CTFontCreateWithName(CFSTR("Menlo"), 16.0, NULL);
    if (!base)
        return -1;

    /* Encode codepoint as UTF-16 */
    UniChar chars[2];
    CFIndex char_count;
    if (codepoint <= 0xFFFF) {
        chars[0] = (UniChar)codepoint;
        char_count = 1;
    } else {
        /* Surrogate pair for supplementary plane */
        codepoint -= 0x10000;
        chars[0] = (UniChar)(0xD800 + (codepoint >> 10));
        chars[1] = (UniChar)(0xDC00 + (codepoint & 0x3FF));
        char_count = 2;
    }

    CFStringRef str =
        CFStringCreateWithCharacters(NULL, chars, char_count);
    if (!str) {
        CFRelease(base);
        return -1;
    }

    /* Ask Core Text for the best font for this string */
    CTFontRef fallback =
        CTFontCreateForString(base, str, CFRangeMake(0, char_count));
    CFRelease(str);
    CFRelease(base);

    if (!fallback)
        return -1;

    result->font_path = ct_font_get_path(fallback);
    result->family_name = ct_font_get_family(fallback);
    CFRelease(fallback);

    if (!result->font_path) {
        free(result->family_name);
        result->family_name = NULL;
        return -1;
    }

    vlog("Core Text fallback for U+%04X: %s (%s)\n", codepoint,
         result->family_name ? result->family_name : "?",
         result->font_path);

    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcasecmp(*(const char **)a, *(const char **)b);
}

static void ct_list_monospace(FontResolveBackend *resolve)
{
    (void)resolve;

    /* Create a descriptor matching monospace fonts */
    CTFontSymbolicTraits mono_trait = kCTFontMonoSpaceTrait;
    CFNumberRef trait_val =
        CFNumberCreate(NULL, kCFNumberSInt32Type, &mono_trait);
    CFStringRef trait_key = kCTFontSymbolicTrait;
    CFDictionaryRef traits =
        CFDictionaryCreate(NULL, (const void **)&trait_key,
                           (const void **)&trait_val, 1,
                           &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CFRelease(trait_val);

    CFStringRef traits_attr = kCTFontTraitsAttribute;
    CFDictionaryRef attrs =
        CFDictionaryCreate(NULL, (const void **)&traits_attr,
                           (const void **)&traits, 1,
                           &kCFTypeDictionaryKeyCallBacks,
                           &kCFTypeDictionaryValueCallBacks);
    CFRelease(traits);

    CTFontDescriptorRef desc =
        CTFontDescriptorCreateWithAttributes(attrs);
    CFRelease(attrs);

    CFArrayRef descs = CFArrayCreate(NULL, (const void **)&desc, 1,
                                     &kCFTypeArrayCallBacks);
    CFRelease(desc);

    CTFontCollectionRef collection =
        CTFontCollectionCreateWithFontDescriptors(descs, NULL);
    CFRelease(descs);

    CFArrayRef matches =
        CTFontCollectionCreateMatchingFontDescriptors(collection);
    CFRelease(collection);

    if (!matches)
        return;

    /* Collect unique family names */
    char **families = NULL;
    int n_families = 0;
    CFIndex count = CFArrayGetCount(matches);

    for (CFIndex i = 0; i < count; i++) {
        CTFontDescriptorRef match =
            (CTFontDescriptorRef)CFArrayGetValueAtIndex(matches, i);

        CFStringRef fam =
            CTFontDescriptorCopyAttribute(match, kCTFontFamilyNameAttribute);
        if (!fam)
            continue;

        char fam_buf[256];
        if (!CFStringGetCString(fam, fam_buf, sizeof(fam_buf),
                                kCFStringEncodingUTF8)) {
            CFRelease(fam);
            continue;
        }
        CFRelease(fam);

        /* Deduplicate */
        int dup = 0;
        for (int j = 0; j < n_families; j++) {
            if (strcasecmp(families[j], fam_buf) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;

        families =
            realloc(families, (n_families + 1) * sizeof(char *));
        families[n_families++] = strdup(fam_buf);
    }

    CFRelease(matches);

    /* Sort and print */
    if (n_families > 0) {
        qsort(families, n_families, sizeof(char *), cmp_str);
        for (int i = 0; i < n_families; i++) {
            fprintf(stdout, "%s\n", families[i]);
            free(families[i]);
        }
        fflush(stdout);
    }
    free(families);
}

FontResolveBackend font_resolve_backend_ct = {
    .name = "coretext",
    .backend_data = NULL,
    .init = ct_init,
    .destroy = ct_destroy,
    .find_font = ct_find_font,
    .find_font_for_codepoint = ct_find_font_for_codepoint,
    .list_monospace = ct_list_monospace,
};

#endif /* __APPLE__ */
