/*
 * portty — GDI font resolver for Windows
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef _WIN32

#define INITGUID
#include "font_resolve_w32.h"
#include "common.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <dwrite.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

/* --- Font entry (one face from GDI enumeration) --- */

typedef struct
{
    char *family;
    char *style;
    char *path;
    bool is_fixed_pitch;
    int weight;
    bool italic;
} FontEntry;

typedef struct
{
    FontEntry *entries;
    int count;
    int capacity;
    char fonts_dir[MAX_PATH];
    char user_fonts_dir[MAX_PATH];
    FT_Library ft_lib;
    /* DirectWrite objects for path resolution (lazy init) */
    IDWriteFactory *dw_factory;
    IDWriteFontCollection *dw_collection;
    bool dw_init_done;
    bool dw_init_ok;
} W32FontData;

/* --- Helpers --- */

static char *wide_to_utf8(const WCHAR *wide)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0,
                                  NULL, NULL);
    if (len <= 0)
        return NULL;
    char *buf = malloc(len);
    if (!buf)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, buf, len, NULL, NULL);
    return buf;
}

static void add_entry(W32FontData *data, const char *family,
                      const char *style, const char *path,
                      bool is_fixed_pitch, int weight, bool italic)
{
    if (data->count >= data->capacity) {
        data->capacity = data->capacity ? data->capacity * 2 : 256;
        data->entries =
            realloc(data->entries, data->capacity * sizeof(FontEntry));
    }
    FontEntry *e = &data->entries[data->count++];
    e->family = strdup(family);
    e->style = strdup(style);
    e->path = path ? strdup(path) : NULL;
    e->is_fixed_pitch = is_fixed_pitch;
    e->weight = weight;
    e->italic = italic;
}

static const char *style_from_weight_italic(int weight, bool italic)
{
    if (weight >= FW_BOLD && italic)
        return "Bold Italic";
    if (weight >= FW_BOLD)
        return "Bold";
    if (italic)
        return "Italic";
    return "Regular";
}

/* --- Registry scanning for file paths --- */

static void parse_display_name(char *name, char **out_family,
                               char **out_style)
{
    char *paren = strrchr(name, '(');
    if (paren && paren > name && *(paren - 1) == ' ')
        *(paren - 1) = '\0';

    size_t len = strlen(name);
    while (len > 0 && name[len - 1] == ' ')
        name[--len] = '\0';

    static const struct
    {
        const char *suffix;
        const char *style;
    } styles[] = {
        { " Bold Italic", "Bold Italic" },
        { " Bold", "Bold" },
        { " Italic", "Italic" },
        { " Light", "Light" },
        { " SemiBold", "SemiBold" },
        { " Semibold", "SemiBold" },
        { " Medium", "Medium" },
        { " Thin", "Thin" },
        { " Black", "Black" },
        { " SemiLight", "SemiLight" },
        { " Semilight", "SemiLight" },
    };

    *out_style = "Regular";
    for (int i = 0; i < (int)(sizeof(styles) / sizeof(styles[0])); i++) {
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

typedef struct
{
    char *family;
    char *style;
    char *path;
} RegEntry;

typedef struct
{
    RegEntry *entries;
    int count;
    int capacity;
} RegScanData;

static void add_reg_entry(RegScanData *scan, const char *family,
                          const char *style, const char *path)
{
    if (scan->count >= scan->capacity) {
        scan->capacity = scan->capacity ? scan->capacity * 2 : 256;
        scan->entries =
            realloc(scan->entries, scan->capacity * sizeof(RegEntry));
    }
    RegEntry *e = &scan->entries[scan->count++];
    e->family = strdup(family);
    e->style = strdup(style);
    e->path = strdup(path);
}

static void scan_registry_key(HKEY root, const WCHAR *subkey_w,
                              const char *fonts_dir,
                              RegScanData *scan)
{
    HKEY hkey;
    LONG rc = RegOpenKeyExW(root, subkey_w, 0, KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS)
        return;

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

        char path[MAX_PATH * 2];
        if (strchr(file_utf8, '\\') || strchr(file_utf8, '/')) {
            snprintf(path, sizeof(path), "%s", file_utf8);
        } else {
            snprintf(path, sizeof(path), "%s%s", fonts_dir, file_utf8);
        }

        char *family = NULL;
        char *style = NULL;
        parse_display_name(name_utf8, &family, &style);

        add_reg_entry(scan, family, style, path);

        free(name_utf8);
        free(file_utf8);
    }

    RegCloseKey(hkey);
}

static const char *find_reg_path(RegScanData *scan, const char *family,
                                 const char *style)
{
    for (int i = 0; i < scan->count; i++) {
        if (_stricmp(scan->entries[i].family, family) == 0 &&
            _stricmp(scan->entries[i].style, style) == 0)
            return scan->entries[i].path;
    }

    if (_stricmp(style, "Regular") != 0) {
        for (int i = 0; i < scan->count; i++) {
            if (_stricmp(scan->entries[i].family, family) == 0 &&
                _stricmp(scan->entries[i].style, "Regular") == 0)
                return scan->entries[i].path;
        }
    }

    for (int i = 0; i < scan->count; i++) {
        if (_stricmp(scan->entries[i].family, family) == 0)
            return scan->entries[i].path;
    }

    return NULL;
}

static void free_reg_scan(RegScanData *scan)
{
    for (int i = 0; i < scan->count; i++) {
        free(scan->entries[i].family);
        free(scan->entries[i].style);
        free(scan->entries[i].path);
    }
    free(scan->entries);
}

/* --- GDI font enumeration --- */

typedef struct
{
    W32FontData *data;
    RegScanData *reg_scan;
} EnumCtx;

static bool seen_face(W32FontData *data, const char *family, int weight,
                      bool italic)
{
    for (int i = 0; i < data->count; i++) {
        if (_stricmp(data->entries[i].family, family) == 0 &&
            data->entries[i].weight == weight &&
            data->entries[i].italic == italic)
            return true;
    }
    return false;
}

static int CALLBACK enum_font_cb(const LOGFONTW *lf,
                                 const TEXTMETRICW *tm,
                                 DWORD font_type, LPARAM lParam)
{
    EnumCtx *ctx = (EnumCtx *)lParam;
    W32FontData *data = ctx->data;

    if (!(font_type & TRUETYPE_FONTTYPE))
        return 1;

    const WCHAR *face_w = lf->lfFaceName;

    if (face_w[0] == L'@')
        return 1;

    char *family = wide_to_utf8(face_w);
    if (!family)
        return 1;

    int weight = lf->lfWeight;
    bool italic = (lf->lfItalic != 0);

    if (seen_face(data, family, weight, italic)) {
        free(family);
        return 1;
    }

    const char *entry_style = style_from_weight_italic(weight, italic);

    bool is_fixed_pitch =
        !(tm->tmPitchAndFamily & TMPF_FIXED_PITCH);

    const char *path = find_reg_path(ctx->reg_scan, family, entry_style);
    if (!path) {
        const char *weight_style = style_from_weight_italic(weight, italic);
        path = find_reg_path(ctx->reg_scan, family, weight_style);
    }

    add_entry(data, family, entry_style, path, is_fixed_pitch, weight,
              italic);

    free(family);

    return 1;
}

/* --- DirectWrite path resolution for UWP/Store fonts --- */

static bool ensure_dwrite(W32FontData *data)
{
    if (data->dw_init_done)
        return data->dw_init_ok;
    data->dw_init_done = true;

    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory,
        (IUnknown **)&data->dw_factory);
    if (FAILED(hr)) {
        vlog("W32: DWriteCreateFactory failed: 0x%08lx\n", hr);
        return false;
    }

    hr = data->dw_factory->lpVtbl->GetSystemFontCollection(
        data->dw_factory, &data->dw_collection, FALSE);
    if (FAILED(hr)) {
        vlog("W32: GetSystemFontCollection failed: 0x%08lx\n", hr);
        data->dw_factory->lpVtbl->Release(data->dw_factory);
        data->dw_factory = NULL;
        return false;
    }

    data->dw_init_ok = true;
    vlog("W32: DirectWrite initialized\n");
    return true;
}

/* Resolve the file path for a font family via DirectWrite.
 * This handles UWP/Store fonts (e.g. Cascadia Mono) that GDI can
 * enumerate but the registry has no path for. */
static char *dwrite_resolve_path(W32FontData *data, const char *family,
                                 bool bold, bool italic)
{
    if (!ensure_dwrite(data))
        return NULL;

    WCHAR family_w[256];
    MultiByteToWideChar(CP_UTF8, 0, family, -1, family_w, 256);

    UINT32 index = 0;
    BOOL exists = FALSE;
    HRESULT hr = data->dw_collection->lpVtbl->FindFamilyName(
        data->dw_collection, family_w, &index, &exists);
    if (FAILED(hr) || !exists)
        return NULL;

    IDWriteFontFamily *dw_family = NULL;
    hr = data->dw_collection->lpVtbl->GetFontFamily(
        data->dw_collection, index, &dw_family);
    if (FAILED(hr))
        return NULL;

    /* Find the best matching font in the family by weight/stretch/style */
    UINT32 font_count = dw_family->lpVtbl->GetFontCount(dw_family);
    IDWriteFont *best_font = NULL;
    int best_score = 100;

    for (UINT32 fi = 0; fi < font_count; fi++) {
        IDWriteFont *dw_font = NULL;
        hr = dw_family->lpVtbl->GetFont(dw_family, fi, &dw_font);
        if (FAILED(hr))
            continue;

        DWRITE_FONT_WEIGHT dw_weight =
            dw_font->lpVtbl->GetWeight(dw_font);
        DWRITE_FONT_STYLE dw_style =
            dw_font->lpVtbl->GetStyle(dw_font);
        bool dw_bold = (dw_weight >= DWRITE_FONT_WEIGHT_BOLD);
        bool dw_italic =
            (dw_style == DWRITE_FONT_STYLE_ITALIC ||
             dw_style == DWRITE_FONT_STYLE_OBLIQUE);

        int score = 0;
        if (bold != dw_bold)
            score += 2;
        if (italic != dw_italic)
            score += 2;

        if (score < best_score) {
            if (best_font)
                best_font->lpVtbl->Release(best_font);
            best_font = dw_font;
            best_score = score;
            if (score == 0)
                break;
        } else {
            dw_font->lpVtbl->Release(dw_font);
        }
    }

    if (!best_font) {
        dw_family->lpVtbl->Release(dw_family);
        return NULL;
    }

    /* Get the font face, then its file path */
    IDWriteFontFace *dw_face = NULL;
    hr = best_font->lpVtbl->CreateFontFace(best_font, &dw_face);
    best_font->lpVtbl->Release(best_font);
    dw_family->lpVtbl->Release(dw_family);

    if (FAILED(hr))
        return NULL;

    UINT32 file_count = 0;
    hr = dw_face->lpVtbl->GetFiles(dw_face, &file_count, NULL);
    if (FAILED(hr) || file_count == 0) {
        dw_face->lpVtbl->Release(dw_face);
        return NULL;
    }

    IDWriteFontFile **files =
        malloc(file_count * sizeof(IDWriteFontFile *));
    hr = dw_face->lpVtbl->GetFiles(dw_face, &file_count, files);
    dw_face->lpVtbl->Release(dw_face);

    if (FAILED(hr)) {
        free(files);
        return NULL;
    }

    char *result = NULL;

    for (UINT32 fi = 0; fi < file_count && !result; fi++) {
        IDWriteFontFileLoader *loader = NULL;
        hr = files[fi]->lpVtbl->GetLoader(files[fi], &loader);
        if (FAILED(hr))
            continue;

        /* Try to get the local file path via IDWriteLocalFontFileLoader */
        IDWriteLocalFontFileLoader *local_loader = NULL;
        hr = loader->lpVtbl->QueryInterface(
            loader, &IID_IDWriteLocalFontFileLoader,
            (void **)&local_loader);
        loader->lpVtbl->Release(loader);

        if (FAILED(hr))
            continue;

        const void *key = NULL;
        UINT32 key_size = 0;
        hr = files[fi]->lpVtbl->GetReferenceKey(files[fi], &key,
                                                &key_size);
        if (FAILED(hr)) {
            local_loader->lpVtbl->Release(local_loader);
            continue;
        }

        UINT32 path_len = 0;
        hr = local_loader->lpVtbl->GetFilePathLengthFromKey(
            local_loader, key, key_size, &path_len);
        if (FAILED(hr)) {
            local_loader->lpVtbl->Release(local_loader);
            continue;
        }

        WCHAR *path_w = malloc((path_len + 1) * sizeof(WCHAR));
        hr = local_loader->lpVtbl->GetFilePathFromKey(
            local_loader, key, key_size, path_w, path_len + 1);
        local_loader->lpVtbl->Release(local_loader);

        if (SUCCEEDED(hr)) {
            result = wide_to_utf8(path_w);
        }
        free(path_w);
    }

    for (UINT32 fi = 0; fi < file_count; fi++)
        files[fi]->lpVtbl->Release(files[fi]);
    free(files);

    return result;
}

/* --- Pattern parsing --- */

typedef struct
{
    char family[256];
    float size;
    int bold;
    int italic;
} ParsedPattern;

static int has_family_with_path(W32FontData *data, const char *family)
{
    for (int i = 0; i < data->count; i++) {
        if (_stricmp(data->entries[i].family, family) == 0 &&
            data->entries[i].path)
            return 1;
    }
    return 0;
}

static int has_family(W32FontData *data, const char *family)
{
    for (int i = 0; i < data->count; i++) {
        if (_stricmp(data->entries[i].family, family) == 0)
            return 1;
    }
    return 0;
}

static void parse_fontconfig_pattern(const char *pattern,
                                     ParsedPattern *out,
                                     W32FontData *data)
{
    memset(out, 0, sizeof(*out));

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", pattern);

    char *props = strchr(buf, ':');
    if (props) {
        *props++ = '\0';

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

    char *dash = strrchr(buf, '-');
    if (dash && dash > buf && dash[1] >= '0' && dash[1] <= '9') {
        out->size = (float)atof(dash + 1);
        *dash = '\0';
    }

    if (_stricmp(buf, "monospace") == 0) {
        static const char *defaults[] = { "Cascadia Mono",
                                          "Cascadia Code",
                                          "Consolas",
                                          "Courier New",
                                          NULL };
        /* Prefer families that have a resolvable file path */
        for (int i = 0; defaults[i]; i++) {
            if (has_family_with_path(data, defaults[i])) {
                snprintf(out->family, sizeof(out->family), "%s",
                         defaults[i]);
                return;
            }
        }
        /* Also try families that GDI knows about (path may be
         * resolved later via DirectWrite) */
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

    if (FT_Init_FreeType(&data->ft_lib) != 0) {
        free(data);
        return false;
    }

    /* Get Windows system fonts directory via CSIDL_FONTS */
    WCHAR fontsdir[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_FONTS, NULL, 0,
                                   fontsdir))) {
        wcscat(fontsdir, L"\\");
    } else {
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

    /* Get user fonts directory */
    WCHAR local_appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0,
                                   local_appdata))) {
        WCHAR user_fonts[MAX_PATH];
        swprintf(user_fonts, MAX_PATH,
                 L"%s\\Microsoft\\Windows\\Fonts\\", local_appdata);
        char *user_fonts_utf8 = wide_to_utf8(user_fonts);
        if (user_fonts_utf8) {
            snprintf(data->user_fonts_dir, sizeof(data->user_fonts_dir),
                     "%s", user_fonts_utf8);
            free(user_fonts_utf8);
        }
    }
    vlog("User fonts directory: %s\n", data->user_fonts_dir);

    /* Scan registry for file path mappings (both system and per-user) */
    RegScanData reg_scan = { NULL, 0, 0 };
    scan_registry_key(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
        data->fonts_dir, &reg_scan);
    scan_registry_key(
        HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
        data->user_fonts_dir[0] ? data->user_fonts_dir : data->fonts_dir,
        &reg_scan);

    vlog("Registry scan: found %d font path entries\n", reg_scan.count);

    /* Enumerate fonts via GDI for accurate family/style/pitch info */
    HDC hdc = CreateCompatibleDC(NULL);
    if (hdc) {
        EnumCtx ctx = { data, &reg_scan };

        LOGFONTW lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfFaceName[0] = L'\0';

        EnumFontFamiliesExW(hdc, &lf, enum_font_cb, (LPARAM)&ctx, 0);
        DeleteDC(hdc);
    } else {
        vlog("Failed to create DC for font enumeration\n");
    }

    /* If GDI enumeration found nothing, fall back to registry-only */
    if (data->count == 0 && reg_scan.count > 0) {
        vlog("GDI enumeration found no fonts, falling back to "
             "registry-only mode\n");
        for (int i = 0; i < reg_scan.count; i++) {
            add_entry(data, reg_scan.entries[i].family,
                      reg_scan.entries[i].style,
                      reg_scan.entries[i].path,
                      false, FW_DONTCARE, false);
        }
    }

    vlog("W32 font resolver: loaded %d font entries\n", data->count);

    free_reg_scan(&reg_scan);

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
    if (data->dw_collection)
        data->dw_collection->lpVtbl->Release(data->dw_collection);
    if (data->dw_factory)
        data->dw_factory->lpVtbl->Release(data->dw_factory);
    if (data->ft_lib)
        FT_Done_FreeType(data->ft_lib);
    free(data);
    resolve->backend_data = NULL;
}

/* Find best matching entry using weight/italic.  Falls back through
 * style variations and then to DirectWrite for path resolution of
 * UWP/Store fonts absent from the registry. */
static FontEntry *find_entry(W32FontData *data, const char *family,
                             bool bold, bool italic)
{
    FontEntry *best = NULL;
    int best_score = 100;

    for (int i = 0; i < data->count; i++) {
        FontEntry *e = &data->entries[i];
        if (_stricmp(e->family, family) != 0)
            continue;

        bool e_bold = (e->weight >= FW_BOLD);
        bool e_italic = e->italic;

        int score = 0;
        if (bold != e_bold)
            score += 2;
        if (italic != e_italic)
            score += 2;
        if (!e->path)
            score += 10;

        if (score < best_score) {
            best_score = score;
            best = e;
            if (score == 0)
                break;
        }
    }

    /* If the best entry lacks a file path, try DirectWrite to
     * resolve UWP/Store fonts not in the registry. */
    if (best && !best->path) {
        char *dw_path = dwrite_resolve_path(data, family, bold, italic);
        if (dw_path) {
            best->path = dw_path;
            vlog("W32: resolved path for '%s' via DirectWrite: %s\n",
                 family, dw_path);
        }
    }

    return best;
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

    if (type == FONT_TYPE_EMOJI) {
        FontEntry *e = find_entry(data, "Segoe UI Emoji", false, false);
        if (!e || !e->path)
            return -1;
        result->font_path = strdup(e->path);
        result->family_name = strdup(e->family);
        return 0;
    }

    if (type == FONT_TYPE_FALLBACK)
        return -1;

    ParsedPattern pp;
    parse_fontconfig_pattern(pattern ? pattern : "monospace", &pp, data);

    bool want_bold = pp.bold;
    bool want_italic = pp.italic;

    switch (type) {
    case FONT_TYPE_BOLD:
        want_bold = true;
        break;
    case FONT_TYPE_ITALIC:
        want_italic = true;
        break;
    case FONT_TYPE_BOLD_ITALIC:
        want_bold = true;
        want_italic = true;
        break;
    default:
        break;
    }

    FontEntry *e = find_entry(data, pp.family, want_bold, want_italic);
    if (!e || !e->path) {
        /* Fall back to monospace defaults with on-disk files */
        if (type == FONT_TYPE_NORMAL) {
            fprintf(stderr,
                    "WARNING: font family '%s' has no accessible "
                    "file path; falling back to system monospace\n",
                    pp.family);
        }
        static const char *fallbacks[] = { "Consolas", "Courier New",
                                           NULL };
        for (int i = 0; fallbacks[i]; i++) {
            e = find_entry(data, fallbacks[i], want_bold, want_italic);
            if (e && e->path)
                break;
            e = NULL;
        }
        if (!e || !e->path)
            return -1;
    }

    result->font_path = strdup(e->path);
    result->family_name = strdup(e->family);
    result->size = pp.size;

    vlog("W32 font resolver: %s bold=%d italic=%d -> %s\n", pp.family,
         want_bold, want_italic, e->path);

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

    static const char *priority[] = { "Segoe UI",
                                      "Segoe UI Symbol",
                                      "Segoe UI Historic",
                                      "Yu Gothic",
                                      "MS Gothic",
                                      "Malgun Gothic",
                                      NULL };

    for (int p = 0; priority[p]; p++) {
        FontEntry *e = find_entry(data, priority[p], false, false);
        if (!e || !e->path)
            continue;

        FT_Face face;
        if (FT_New_Face(data->ft_lib, e->path, 0, &face) == 0) {
            if (FT_Get_Char_Index(face, codepoint) != 0) {
                result->font_path = strdup(e->path);
                result->family_name = strdup(e->family);
                FT_Done_Face(face);
                return 0;
            }
            FT_Done_Face(face);
        }
    }

    for (int i = 0; i < data->count; i++) {
        FontEntry *e = &data->entries[i];
        if (!e->path)
            continue;

        FT_Face face;
        if (FT_New_Face(data->ft_lib, e->path, 0, &face) == 0) {
            if (FT_Get_Char_Index(face, codepoint) != 0) {
                result->font_path = strdup(e->path);
                result->family_name = strdup(e->family);
                FT_Done_Face(face);
                return 0;
            }
            FT_Done_Face(face);
        }
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

    char **families = NULL;
    int n_families = 0;

    for (int i = 0; i < data->count; i++) {
        FontEntry *e = &data->entries[i];

        if (!e->is_fixed_pitch)
            continue;

        bool e_bold = (e->weight >= FW_BOLD);
        if (e_bold || e->italic)
            continue;

        int dup = 0;
        for (int j = 0; j < n_families; j++) {
            if (_stricmp(families[j], e->family) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;

        families =
            realloc(families, (n_families + 1) * sizeof(char *));
        families[n_families++] = e->family;
    }

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
