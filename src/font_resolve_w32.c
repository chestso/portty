#ifdef _WIN32

#include "font_resolve_w32.h"
#include "common.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* --- Font registry entry --- */

typedef struct
{
    char *family;
    char *style; /* "Regular", "Bold", "Italic", "Bold Italic" */
    char *path;  /* Absolute file path */
} FontEntry;

typedef struct
{
    FontEntry *entries;
    int count;
    int capacity;
    char fonts_dir[MAX_PATH]; /* e.g. "C:\\Windows\\Fonts\\" */
} W32FontData;

/* --- Helpers --- */

static char *wide_to_utf8(const WCHAR *wide)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0,
                                  NULL, NULL);
    if (len <= 0)
        return NULL;
    char *buf = malloc(len);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, len, NULL, NULL);
    return buf;
}

/* Parse a registry display name like "Consolas Bold (TrueType)" into
 * family="Consolas" and style="Bold". Modifies the input buffer. */
static void parse_display_name(char *name, char **out_family,
                               char **out_style)
{
    /* Strip trailing " (TrueType)" or " (OpenType)" */
    char *paren = strrchr(name, '(');
    if (paren && paren > name && *(paren - 1) == ' ')
        *(paren - 1) = '\0';

    /* Trim trailing whitespace */
    size_t len = strlen(name);
    while (len > 0 && name[len - 1] == ' ')
        name[--len] = '\0';

    /* Check for style suffixes */
    static const struct
    {
        const char *suffix;
        const char *style;
    } styles[] = {
        { " Bold Italic", "Bold Italic" },
        { " Bold", "Bold" },
        { " Italic", "Italic" },
    };

    *out_style = "Regular";
    for (int i = 0; i < 3; i++) {
        size_t slen = strlen(styles[i].suffix);
        if (len > slen &&
            _stricmp(name + len - slen, styles[i].suffix) == 0) {
            name[len - slen] = '\0';
            *out_style = (char *)styles[i].style;
            break;
        }
    }

    *out_family = name;
}

static void add_entry(W32FontData *data, const char *family,
                      const char *style, const char *path)
{
    if (data->count >= data->capacity) {
        data->capacity = data->capacity ? data->capacity * 2 : 256;
        data->entries =
            realloc(data->entries, data->capacity * sizeof(FontEntry));
    }
    FontEntry *e = &data->entries[data->count++];
    e->family = strdup(family);
    e->style = strdup(style);
    e->path = strdup(path);
}

/* Case-insensitive family search. Returns first match with given style,
 * or first "Regular" match if style is NULL. */
static FontEntry *find_entry(W32FontData *data, const char *family,
                             const char *style)
{
    FontEntry *regular_fallback = NULL;

    for (int i = 0; i < data->count; i++) {
        FontEntry *e = &data->entries[i];
        if (_stricmp(e->family, family) != 0)
            continue;
        if (style && _stricmp(e->style, style) == 0)
            return e;
        if (!regular_fallback &&
            _stricmp(e->style, "Regular") == 0)
            regular_fallback = e;
    }
    return regular_fallback;
}

static int has_family(W32FontData *data, const char *family)
{
    for (int i = 0; i < data->count; i++) {
        if (_stricmp(data->entries[i].family, family) == 0)
            return 1;
    }
    return 0;
}

/* --- Pattern parsing --- */

typedef struct
{
    char family[256];
    float size;
    int bold;
    int italic;
} ParsedPattern;

static void parse_fontconfig_pattern(const char *pattern,
                                     ParsedPattern *out,
                                     W32FontData *data)
{
    memset(out, 0, sizeof(*out));

    /* Copy pattern for manipulation */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", pattern);

    /* Split on first ':' */
    char *props = strchr(buf, ':');
    if (props) {
        *props++ = '\0';

        /* Parse key=value pairs */
        char *tok = props;
        while (tok && *tok) {
            char *next = strchr(tok, ':');
            if (next)
                *next++ = '\0';

            if (strncmp(tok, "weight=", 7) == 0) {
                if (_stricmp(tok + 7, "bold") == 0)
                    out->bold = 1;
            } else if (strncmp(tok, "slant=", 6) == 0) {
                if (_stricmp(tok + 6, "italic") == 0 ||
                    _stricmp(tok + 6, "oblique") == 0)
                    out->italic = 1;
            } else if (strncmp(tok, "size=", 5) == 0) {
                out->size = (float)atof(tok + 5);
            }
            tok = next;
        }
    }

    /* Parse family-size from the family part */
    char *dash = strrchr(buf, '-');
    if (dash && dash > buf && dash[1] >= '0' && dash[1] <= '9') {
        out->size = (float)atof(dash + 1);
        *dash = '\0';
    }

    /* Map "monospace" to a real Windows family */
    if (_stricmp(buf, "monospace") == 0) {
        static const char *defaults[] = { "Cascadia Mono",
                                          "Consolas", "Courier New",
                                          NULL };
        for (int i = 0; defaults[i]; i++) {
            if (has_family(data, defaults[i])) {
                snprintf(out->family, sizeof(out->family), "%s",
                         defaults[i]);
                return;
            }
        }
        snprintf(out->family, sizeof(out->family), "Courier New");
    } else {
        snprintf(out->family, sizeof(out->family), "%s", buf);
    }
}

/* --- Backend implementation --- */

static bool w32_init(FontResolveBackend *resolve)
{
    W32FontData *data = calloc(1, sizeof(W32FontData));
    if (!data)
        return false;

    /* Get Windows fonts directory via CSIDL_FONTS */
    WCHAR fontsdir[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_FONTS, NULL, 0,
                                   fontsdir))) {
        wcscat(fontsdir, L"\\");
    } else {
        /* Fallback */
        WCHAR windir[MAX_PATH];
        GetWindowsDirectoryW(windir, MAX_PATH);
        swprintf(fontsdir, MAX_PATH, L"%s\\Fonts\\", windir);
    }

    char *fonts_utf8 = wide_to_utf8(fontsdir);
    if (fonts_utf8) {
        snprintf(data->fonts_dir, sizeof(data->fonts_dir), "%s",
                 fonts_utf8);
        free(fonts_utf8);
    }
    vlog("Fonts directory: %s\n", data->fonts_dir);

    /* Scan font registry */
    HKEY hkey;
    LONG rc = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0,
        KEY_READ, &hkey);

    if (rc != ERROR_SUCCESS) {
        vlog("Failed to open font registry key: %ld\n", rc);
        free(data);
        return false;
    }

    DWORD index = 0;
    WCHAR name_w[512];
    WCHAR value_w[MAX_PATH];
    DWORD name_len, value_len, type;

    for (;;) {
        name_len = sizeof(name_w) / sizeof(WCHAR);
        value_len = sizeof(value_w);
        rc = RegEnumValueW(hkey, index++, name_w, &name_len, NULL,
                           &type, (BYTE *)value_w, &value_len);
        if (rc != ERROR_SUCCESS)
            break;
        if (type != REG_SZ)
            continue;

        char *name_utf8 = wide_to_utf8(name_w);
        char *file_utf8 = wide_to_utf8(value_w);
        if (!name_utf8 || !file_utf8) {
            free(name_utf8);
            free(file_utf8);
            continue;
        }

        /* Build absolute path if relative */
        char path[MAX_PATH * 2];
        if (strchr(file_utf8, '\\') || strchr(file_utf8, '/')) {
            snprintf(path, sizeof(path), "%s", file_utf8);
        } else {
            snprintf(path, sizeof(path), "%s%s", data->fonts_dir,
                     file_utf8);
        }

        /* Parse display name into family + style */
        char *family = NULL;
        char *style = NULL;
        parse_display_name(name_utf8, &family, &style);

        add_entry(data, family, style, path);

        free(name_utf8);
        free(file_utf8);
    }

    RegCloseKey(hkey);

    vlog("W32 font resolver: loaded %d font entries from registry\n",
         data->count);

    resolve->backend_data = data;
    return true;
}

static void w32_destroy(FontResolveBackend *resolve)
{
    if (!resolve || !resolve->backend_data)
        return;

    W32FontData *data = (W32FontData *)resolve->backend_data;
    for (int i = 0; i < data->count; i++) {
        free(data->entries[i].family);
        free(data->entries[i].style);
        free(data->entries[i].path);
    }
    free(data->entries);
    free(data);
    resolve->backend_data = NULL;
}

static int w32_find_font(FontResolveBackend *resolve, FontType type,
                         const char *pattern,
                         FontResolutionResult *result)
{
    W32FontData *data = (W32FontData *)resolve->backend_data;
    if (!data)
        return -1;

    result->font_path = NULL;
    result->family_name = NULL;
    result->size = 0;

    /* Handle emoji directly */
    if (type == FONT_TYPE_EMOJI) {
        FontEntry *e = find_entry(data, "Segoe UI Emoji", "Regular");
        if (!e)
            return -1;
        result->font_path = strdup(e->path);
        result->family_name = strdup(e->family);
        return 0;
    }

    if (type == FONT_TYPE_FALLBACK)
        return -1;

    /* Parse the pattern (default to "monospace" if NULL) */
    ParsedPattern pp;
    parse_fontconfig_pattern(pattern ? pattern : "monospace", &pp,
                             data);

    /* Override style from FontType */
    const char *target_style = NULL;
    switch (type) {
    case FONT_TYPE_BOLD:
        target_style = "Bold";
        break;
    case FONT_TYPE_ITALIC:
        target_style = "Italic";
        break;
    case FONT_TYPE_BOLD_ITALIC:
        target_style = "Bold Italic";
        break;
    default:
        if (pp.bold && pp.italic)
            target_style = "Bold Italic";
        else if (pp.bold)
            target_style = "Bold";
        else if (pp.italic)
            target_style = "Italic";
        else
            target_style = "Regular";
        break;
    }

    FontEntry *e = find_entry(data, pp.family, target_style);
    if (!e)
        return -1;

    result->font_path = strdup(e->path);
    result->family_name = strdup(e->family);
    result->size = pp.size;

    vlog("W32 font resolver: %s style=%s -> %s\n", pp.family,
         target_style, e->path);

    return 0;
}

static int w32_find_font_for_codepoint(FontResolveBackend *resolve,
                                       uint32_t codepoint,
                                       FontResolutionResult *result)
{
    W32FontData *data = (W32FontData *)resolve->backend_data;
    if (!data)
        return -1;

    result->font_path = NULL;
    result->family_name = NULL;
    result->size = 0;

    /* Priority fonts to check first */
    static const char *priority[] = { "Segoe UI",
                                      "Segoe UI Symbol",
                                      "Segoe UI Historic",
                                      "Yu Gothic",
                                      "MS Gothic",
                                      "Malgun Gothic",
                                      NULL };

    for (int p = 0; priority[p]; p++) {
        FontEntry *e = find_entry(data, priority[p], "Regular");
        if (!e)
            continue;

        FT_Library lib;
        if (FT_Init_FreeType(&lib) != 0)
            continue;

        FT_Face face;
        if (FT_New_Face(lib, e->path, 0, &face) == 0) {
            if (FT_Get_Char_Index(face, codepoint) != 0) {
                result->font_path = strdup(e->path);
                result->family_name = strdup(e->family);
                FT_Done_Face(face);
                FT_Done_FreeType(lib);
                return 0;
            }
            FT_Done_Face(face);
        }
        FT_Done_FreeType(lib);
    }

    /* Fall back to scanning all entries */
    for (int i = 0; i < data->count; i++) {
        FontEntry *e = &data->entries[i];

        FT_Library lib;
        if (FT_Init_FreeType(&lib) != 0)
            continue;

        FT_Face face;
        if (FT_New_Face(lib, e->path, 0, &face) == 0) {
            if (FT_Get_Char_Index(face, codepoint) != 0) {
                result->font_path = strdup(e->path);
                result->family_name = strdup(e->family);
                FT_Done_Face(face);
                FT_Done_FreeType(lib);
                return 0;
            }
            FT_Done_Face(face);
        }
        FT_Done_FreeType(lib);
    }

    return -1;
}

static int cmp_str(const void *a, const void *b)
{
    return _stricmp(*(const char **)a, *(const char **)b);
}

static void w32_list_monospace(FontResolveBackend *resolve)
{
    W32FontData *data = (W32FontData *)resolve->backend_data;
    if (!data)
        return;

    /* Collect unique families that are fixed-width */
    char **families = NULL;
    int n_families = 0;

    for (int i = 0; i < data->count; i++) {
        FontEntry *e = &data->entries[i];

        /* Only check "Regular" style to avoid duplicates */
        if (_stricmp(e->style, "Regular") != 0)
            continue;

        /* Deduplicate */
        int dup = 0;
        for (int j = 0; j < n_families; j++) {
            if (_stricmp(families[j], e->family) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;

        if (font_resolve_is_fixed_width(e->path)) {
            families =
                realloc(families, (n_families + 1) * sizeof(char *));
            families[n_families++] = e->family;
        }
    }

    /* Sort and print */
    if (n_families > 0) {
        qsort(families, n_families, sizeof(char *), cmp_str);
        for (int i = 0; i < n_families; i++)
            fprintf(stdout, "%s\n", families[i]);
        fflush(stdout);
    }
    free(families);
}

FontResolveBackend font_resolve_backend_w32 = {
    .name = "w32",
    .backend_data = NULL,
    .init = w32_init,
    .destroy = w32_destroy,
    .find_font = w32_find_font,
    .find_font_for_codepoint = w32_find_font_for_codepoint,
    .list_monospace = w32_list_monospace,
};

#endif /* _WIN32 */
