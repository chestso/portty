// Stubs for font_resolve functions used by rend_common.c's font loading
// code. The atlas/rend_common tests don't actually call rend_load_fonts,
// but the linker pulls in all symbols from rend_common.o.
#include "font_resolve.h"
#include <string.h>

FontResolveBackend *font_resolve_init(FontResolveBackend *backend)
{
    (void)backend;
    return NULL;
}

void font_resolve_destroy(FontResolveBackend *resolve)
{
    (void)resolve;
}

int font_resolve_find_font(FontResolveBackend *resolve, FontType type,
                           const char *pattern, FontResolutionResult *result)
{
    (void)resolve;
    (void)type;
    (void)pattern;
    memset(result, 0, sizeof(*result));
    return -1;
}

int font_resolve_find_font_for_codepoint(FontResolveBackend *resolve,
                                         uint32_t codepoint,
                                         FontResolutionResult *result)
{
    (void)resolve;
    (void)codepoint;
    memset(result, 0, sizeof(*result));
    return -1;
}

void font_resolve_free_result(FontResolutionResult *result)
{
    (void)result;
}

void font_resolve_list_monospace(FontResolveBackend *resolve)
{
    (void)resolve;
}
