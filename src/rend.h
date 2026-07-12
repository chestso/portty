#ifndef REND_H
#define REND_H

#include "term.h"
#include <stdbool.h>

// Forward declarations
struct RendererBackend;
typedef struct RendererBackend RendererBackend;

// Read-only snapshot of renderer state for the diagnostics report.
// String members point at renderer-owned storage valid for the renderer's
// lifetime; do not free them.
typedef struct
{
    const char *renderer_name; // SDL renderer ("gpu"/"vulkan"/"opengl"/...), or NULL
    bool linear_light;         // linear-light compositing active
    bool glyph_shader;         // luminance-aware GPU glyph-coverage shader active
    float content_scale;       // HiDPI content scale
    int pixel_width;           // window drawable size, pixels
    int pixel_height;
    int cell_width; // glyph cell size, pixels
    int cell_height;
    const char *font_path; // resolved normal font file path, or NULL
    const char *hinting;   // effective FT hint target ("none"/"light"/"normal"/"mono"), or NULL
    // GPU + driver via SDL's GPU renderer (SDL_GetGPUDeviceProperties).
    // gpu_driver_libre flags a permissively-licensed (Mesa) driver.
    const char *gpu_device;
    const char *gpu_driver;
    bool gpu_driver_libre;
} RendererDiag;

// Backend interface definition
struct RendererBackend
{
    const char *name;

    // Backend-specific data
    void *backend_data;

    // Backend function pointers
    bool (*init)(RendererBackend *rend, void *window_handle, void *renderer_handle);
    void (*destroy)(RendererBackend *rend);
    int (*load_fonts)(RendererBackend *rend, float font_size, const char *font_name, int ft_hint_target);
    void (*draw_terminal)(RendererBackend *rend, TerminalBackend *term, bool cursor_visible);
    void (*present)(RendererBackend *rend);
    void (*resize)(RendererBackend *rend, int width, int height);
    void (*log_stats)(RendererBackend *rend);
    bool (*get_cell_size)(RendererBackend *rend, int *cell_width, int *cell_height);
    void (*scroll)(RendererBackend *rend, TerminalBackend *term, int delta);
    void (*reset_scroll)(RendererBackend *rend);
    int (*get_scroll_offset)(RendererBackend *rend);
    /* Internal pager overlay: when set, the renderer draws `overlay`
     * full-screen in place of the host terminal and reuses its scroll view
     * for paging (host scroll position is stashed and restored on clear). */
    void (*set_overlay)(RendererBackend *rend, TerminalBackend *overlay);
    void (*clear_overlay)(RendererBackend *rend);
    bool (*has_overlay)(RendererBackend *rend);
    /* Top notification panel (SDL3-only). When set, the
     * renderer draws a dismissable message bar across the top of the window.
     * `level` is a PlatformNotifyLevel (0=info, 1=warning, 2=error). */
    void (*set_notification)(RendererBackend *rend, const char *title,
                             const char *body, int level);
    void (*clear_notification)(RendererBackend *rend);
    /* Hit-test in physical (drawable) pixels: 0 = miss, 1 = on panel (consume
     * the click), 2 = on the close button (dismiss). */
    int (*notification_hit)(RendererBackend *rend, int px, int py);
    /* Set whether the close button is hovered (draws a highlight behind it).
     * Returns true if the state changed (caller should request a repaint). */
    bool (*set_notification_hover)(RendererBackend *rend, bool hovered);
    /* Transient OSC-8 hover hint.
     * Draws a single-line full-width strip showing the link's real URI;
     * url == NULL/"" hides it. anchor_py is the link's physical-pixel Y — the
     * strip sits at the top, flipping to the bottom when the link is up there. */
    void (*set_link_hint)(RendererBackend *rend, const char *url, int anchor_py);
    int (*render_to_png)(RendererBackend *rend, TerminalBackend *term,
                         const char *output_path);
    void (*set_content_scale)(RendererBackend *rend, float scale);
    bool (*get_diag)(RendererBackend *rend, RendererDiag *out);
};

// Renderer API
RendererBackend *renderer_init(RendererBackend *rend, void *window, void *renderer);
void renderer_destroy(RendererBackend *rend);
int renderer_load_fonts(RendererBackend *rend, float font_size, const char *font_name, int ft_hint_target);
void renderer_draw_terminal(RendererBackend *rend, TerminalBackend *term, bool cursor_visible);
void renderer_present(RendererBackend *rend);
void renderer_resize(RendererBackend *rend, int width, int height);
void renderer_log_stats(RendererBackend *rend);
bool renderer_get_cell_size(RendererBackend *rend, int *cell_width, int *cell_height);
void renderer_scroll(RendererBackend *rend, TerminalBackend *term, int delta);
void renderer_reset_scroll(RendererBackend *rend);
int renderer_get_scroll_offset(RendererBackend *rend);
void renderer_set_overlay(RendererBackend *rend, TerminalBackend *overlay);
void renderer_clear_overlay(RendererBackend *rend);
bool renderer_has_overlay(RendererBackend *rend);
void renderer_set_notification(RendererBackend *rend, const char *title,
                               const char *body, int level);
void renderer_clear_notification(RendererBackend *rend);
int renderer_notification_hit(RendererBackend *rend, int px, int py);
bool renderer_set_notification_hover(RendererBackend *rend, bool hovered);
void renderer_set_link_hint(RendererBackend *rend, const char *url, int anchor_py);
int renderer_render_to_png(RendererBackend *rend, TerminalBackend *term,
                           const char *output_path);
void renderer_set_content_scale(RendererBackend *rend, float scale);
bool renderer_get_diag(RendererBackend *rend, RendererDiag *out);

void renderer_process_pty_data(RendererBackend *rend, TerminalBackend *term,
                               const char *data, size_t len);

#endif /* REND_H */
