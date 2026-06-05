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
int renderer_render_to_png(RendererBackend *rend, TerminalBackend *term,
                           const char *output_path);
void renderer_set_content_scale(RendererBackend *rend, float scale);
bool renderer_get_diag(RendererBackend *rend, RendererDiag *out);

void renderer_process_pty_data(RendererBackend *rend, TerminalBackend *term,
                               const char *data, size_t len);

#endif /* REND_H */
