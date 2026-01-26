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
    void (*draw_terminal)(RendererBackend *rend, TerminalBackend *term);
    void (*present)(RendererBackend *rend);
    void (*resize)(RendererBackend *rend, int width, int height);
    void (*toggle_debug_grid)(RendererBackend *rend);
    void (*log_stats)(RendererBackend *rend);
    bool (*get_cell_size)(RendererBackend *rend, int *cell_width, int *cell_height);
    void (*scroll)(RendererBackend *rend, TerminalBackend *term, int delta);
    void (*reset_scroll)(RendererBackend *rend);
    int (*get_scroll_offset)(RendererBackend *rend);
};

// Renderer API
RendererBackend *renderer_init(RendererBackend *rend, void *window, void *renderer);
void renderer_destroy(RendererBackend *rend);
int renderer_load_fonts(RendererBackend *rend, float font_size, const char *font_name, int ft_hint_target);
void renderer_draw_terminal(RendererBackend *rend, TerminalBackend *term);
void renderer_present(RendererBackend *rend);
void renderer_resize(RendererBackend *rend, int width, int height);
void renderer_toggle_debug_grid(RendererBackend *rend);
void renderer_log_stats(RendererBackend *rend);
bool renderer_get_cell_size(RendererBackend *rend, int *cell_width, int *cell_height);
void renderer_scroll(RendererBackend *rend, TerminalBackend *term, int delta);
void renderer_reset_scroll(RendererBackend *rend);
int renderer_get_scroll_offset(RendererBackend *rend);

#endif /* REND_H */
