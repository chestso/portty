#include "font_resolve.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>

FontResolveBackend *font_resolve_init(FontResolveBackend *backend)
{
    if (!backend || !backend->init)
        return NULL;
    if (!backend->init(backend))
        return NULL;
    return backend;
}

void font_resolve_destroy(FontResolveBackend *resolve)
{
    if (!resolve || !resolve->destroy)
        return;
    resolve->destroy(resolve);
}

int font_resolve_find_font(FontResolveBackend *resolve, FontType type,
                           const char *pattern, FontResolutionResult *result)
{
    if (!resolve || !resolve->find_font)
        return -1;
    return resolve->find_font(resolve, type, pattern, result);
}

int font_resolve_find_font_for_codepoint(FontResolveBackend *resolve,
                                         uint32_t codepoint, FontResolutionResult *result)
{
    if (!resolve || !resolve->find_font_for_codepoint)
        return -1;
    return resolve->find_font_for_codepoint(resolve, codepoint, result);
}

void font_resolve_free_result(FontResolutionResult *result)
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

void font_resolve_list_monospace(FontResolveBackend *resolve)
{
    if (!resolve || !resolve->list_monospace)
        return;
    resolve->list_monospace(resolve);
}

int font_resolve_is_fixed_width(const char *font_path)
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
