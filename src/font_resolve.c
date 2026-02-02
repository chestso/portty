#include "font_resolve.h"
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
