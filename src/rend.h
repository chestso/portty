#ifndef REND_H
#define REND_H

#include "term.h"
#include <stdbool.h>

// Forward declarations
struct RendererBackend;
typedef struct RendererBackend RendererBackend;

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
    bool (*get_padding)(RendererBackend *rend, int *left, int *top, int *right, int *bottom);
    void (*set_padding)(RendererBackend *rend, int left, int top, int right, int bottom);
    void (*scroll)(RendererBackend *rend, TerminalBackend *term, int delta);
    void (*reset_scroll)(RendererBackend *rend);
    int (*get_scroll_offset)(RendererBackend *rend);
    void (*set_title)(RendererBackend *rend, const char *title);
    int (*render_to_png)(RendererBackend *rend, TerminalBackend *term,
                         const char *output_path);
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
bool renderer_get_padding(RendererBackend *rend, int *left, int *top, int *right, int *bottom);
void renderer_set_padding(RendererBackend *rend, int left, int top, int right, int bottom);
void renderer_scroll(RendererBackend *rend, TerminalBackend *term, int delta);
void renderer_reset_scroll(RendererBackend *rend);
int renderer_get_scroll_offset(RendererBackend *rend);
void renderer_set_title(RendererBackend *rend, const char *title);
int renderer_render_to_png(RendererBackend *rend, TerminalBackend *term,
                           const char *output_path);

#endif /* REND_H */
