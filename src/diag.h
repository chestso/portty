#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>

// Plain-data snapshot of everything the diagnostics report prints. The caller
// (main.c) fills this from the live backends; diag.c has no dependency on SDL,
// the terminal, or renderer internals, so it stays self-contained and
// unit-testable. String members are borrowed (diag.c never frees them) and may
// be NULL — rendered as "(unset)".
typedef struct
{
    // Rendering / runtime
    const char *renderer_name; // "gpu" / "vulkan" / "opengl" / "software"
    bool linear_light;
    bool glyph_shader; // luminance-aware GPU glyph-coverage shader active
    float content_scale;
    int pixel_width, pixel_height;
    int cell_width, cell_height;
    int cols, rows;

    // Configuration (effective)
    const char *config_path;  // NULL => built-in defaults
    const char *font_pattern; // requested font pattern (config/CLI), or NULL
    const char *font_path;    // resolved normal font file, or NULL
    const char *hinting;      // effective FT hint target: "none"/"light"/"normal"/"mono"
    int scrollback;
    float text_gamma;
    float text_contrast;
    const char *word_chars;
    const char *platform_name; // "sdl3" / "gtk4"

    // Session / environment
    const char *term_env;      // TERM advertised to the shell
    const char *colorterm_env; // COLORTERM advertised to the shell
    const char *pager_env;     // host $PAGER (what the report is shown through)
    const char *lang_env;      // host $LANG
    const char *title;
    bool altscreen;
    int mouse_mode;
} DiagSources;

// Build the formatted, ANSI-styled UTF-8 diagnostics document (truecolor,
// box-drawing, bold/underline). Returns a malloc'd NUL-terminated string the
// caller must free, or NULL on allocation failure.
char *diag_build_report(const DiagSources *src);

#endif // DIAG_H
