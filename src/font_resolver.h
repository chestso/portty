#ifndef FONT_RESOLVER_H
#define FONT_RESOLVER_H

#include <stdint.h>

// Font resolution types
typedef enum
{
    FONT_TYPE_NORMAL,  // Primary text font (monospace/terminal)
    FONT_TYPE_BOLD,    // Bold monospace font
    FONT_TYPE_EMOJI,   // Color emoji font
    FONT_TYPE_FALLBACK // Dynamic fallback (not used with font_resolver_find_font)
} FontType;

// Font resolution result
typedef struct
{
    char *font_path;   // Resolved font file path
    char *family_name; // Matched font family name
    float size;        // Font size
} FontResolutionResult;

// Font resolver functions
int font_resolver_init();

// Resolve a specific font type with fallback strategies
int font_resolver_find_font(
    FontType type,               // Font type to resolve
    const char *family,          // Font family name (NULL for default)
    FontResolutionResult *result // Output resolution result
);

// Find a font containing a specific codepoint (for dynamic fallback)
int font_resolver_find_font_for_codepoint(uint32_t codepoint, FontResolutionResult *result);

// Free resolution result
void font_resolver_free_result(FontResolutionResult *result);

// List all monospace font family names to stdout
void font_resolver_list_monospace();

// Cleanup font resolver
void font_resolver_cleanup();

#endif // FONT_RESOLVER_H