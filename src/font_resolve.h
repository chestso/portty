#ifndef FONT_RESOLVE_H
#define FONT_RESOLVE_H

#include <stdbool.h>
#include <stdint.h>

// Font resolution types
typedef enum
{
    FONT_TYPE_NORMAL,  // Primary text font (monospace/terminal)
    FONT_TYPE_BOLD,    // Bold monospace font
    FONT_TYPE_EMOJI,   // Color emoji font
    FONT_TYPE_FALLBACK // Dynamic fallback (not used with font_resolve_find_font)
} FontType;

// Font resolution result
typedef struct
{
    char *font_path;   // Resolved font file path
    char *family_name; // Matched font family name
    float size;        // Font size
} FontResolutionResult;

// Forward declaration
typedef struct FontResolveBackend FontResolveBackend;

struct FontResolveBackend
{
    const char *name;
    void *backend_data;

    bool (*init)(FontResolveBackend *resolve);
    void (*destroy)(FontResolveBackend *resolve);
    int (*find_font)(FontResolveBackend *resolve, FontType type,
                     const char *pattern, FontResolutionResult *result);
    int (*find_font_for_codepoint)(FontResolveBackend *resolve,
                                   uint32_t codepoint, FontResolutionResult *result);
    void (*list_monospace)(FontResolveBackend *resolve);
};

// Public wrapper functions
FontResolveBackend *font_resolve_init(FontResolveBackend *backend);
void font_resolve_destroy(FontResolveBackend *resolve);
int font_resolve_find_font(FontResolveBackend *resolve, FontType type,
                           const char *pattern, FontResolutionResult *result);
int font_resolve_find_font_for_codepoint(FontResolveBackend *resolve,
                                         uint32_t codepoint, FontResolutionResult *result);
void font_resolve_free_result(FontResolutionResult *result);
void font_resolve_list_monospace(FontResolveBackend *resolve);

#endif // FONT_RESOLVE_H
