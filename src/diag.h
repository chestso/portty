#ifndef DIAG_H
#define DIAG_H

#include <stdbool.h>

// Tri-state driver licensing classification for the diagnostics report.
typedef enum
{
    GPU_DRIVER_LIBRE_UNKNOWN = -1,
    GPU_DRIVER_LIBRE_NO = 0,
    GPU_DRIVER_LIBRE_YES = 1,
} GpuDriverLibre;

// Plain-data snapshot of everything the diagnostics report prints. The caller
// (main.c) fills this from the live backends; diag.c has no dependency on SDL,
// the terminal, or renderer internals, so it stays self-contained and
// unit-testable. String members are borrowed (diag.c never frees them) and may
// be NULL — rendered as "(unset)".
typedef struct
{
    // Rendering / runtime
    const char *renderer_name;       // "gpu" / "vulkan" / "opengl" / "software"
    const char *gpu_device;          // GPU model (e.g. "NVIDIA GeForce RTX 4060 (NVK AD107)"), or NULL
    const char *gpu_driver;          // driver description (e.g. "NVK (open source) — Mesa 25.x"), or NULL
    GpuDriverLibre gpu_driver_libre; // driver licensing: libre / proprietary / unknown
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
    const char *font_source;  // provenance of the effective font, or NULL.
                              // e.g. "config file", "-f flag", "desktop default",
                              // or "fontconfig generic (no desktop default)" —
                              // the last is expected on the SDL backend, which
                              // has no desktop font integration.
    const char *hinting;      // effective FT hint target: "none"/"light"/"normal"/"mono"
    int scrollback;
    float text_gamma;
    float text_contrast;
    const char *word_chars;
    const char *platform_name; // "sdl3"

    // Session / environment
    const char *term_env;      // TERM advertised to the shell
    const char *colorterm_env; // COLORTERM advertised to the shell
    const char *lang_env;      // host $LANG
    const char *title;
    bool altscreen;
    int mouse_mode;

    // VT engine features (coffer)
    const char *vt_backend; // VT engine name, e.g. "coffer"
    bool lottie_rasterizer; // ThorVG available for Lottie rasterization
    bool osc52;             // OSC 52 clipboard set wired up
    bool bracketed_paste;   // CFR_MODE_BRACKETED_PASTE active
    bool sync_output;       // CFR_MODE_SYNC_OUTPUT active
    bool focus_reporting;   // CFR_MODE_FOCUS_REPORTING active
    bool sixel_scrolling;   // CFR_MODE_SIXEL_SCROLLING active
    bool hardened_heap;     // PORTTY_HARDEN_HEAP compile-time guard

    // Display / scaling (all platforms, NULL if unavailable)
    const char *display_session;  // "wayland", "x11", "macOS", "windows", or NULL
    const char *display_xwayland; // "yes" / "no" / NULL (Linux only)
    const char *display_screen;   // "3072x1728 px (Display Name)" or NULL
    const char *display_dpi;      // "physical 96.1, Xft.dpi 192" or NULL
    const char *display_scale;    // "content 2.00, window 2.00" or NULL
    const char *display_physical; // "1920x1080, 309x174 mm (eDP-1)" or NULL
} DiagSources;

// Build the formatted, ANSI-styled UTF-8 diagnostics document (truecolor,
// box-drawing, bold/underline). Returns a malloc'd NUL-terminated string the
// caller must free, or NULL on allocation failure.
char *diag_build_report(const DiagSources *src);

#endif // DIAG_H
