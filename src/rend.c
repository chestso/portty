#include "rend.h"
#include <stdlib.h>

RendererBackend *renderer_init(RendererBackend *backend, void *window, void *renderer)
{
    if (!backend || !backend->init)
        return NULL;

    if (!backend->init(backend, window, renderer))
        return NULL;

    return backend;
}

void renderer_destroy(RendererBackend *rend)
{
    if (!rend || !rend->destroy)
        return;
    rend->destroy(rend);
}

int renderer_load_fonts(RendererBackend *rend, float font_size, const char *font_name, int ft_hint_target)
{
    if (!rend || !rend->load_fonts)
        return -1;
    return rend->load_fonts(rend, font_size, font_name, ft_hint_target);
}

void renderer_draw_terminal(RendererBackend *rend, TerminalBackend *term, bool cursor_visible)
{
    if (!rend || !rend->draw_terminal)
        return;
    rend->draw_terminal(rend, term, cursor_visible);
}

void renderer_present(RendererBackend *rend)
{
    if (!rend || !rend->present)
        return;
    rend->present(rend);
}

void renderer_resize(RendererBackend *rend, int width, int height)
{
    if (!rend || !rend->resize)
        return;
    rend->resize(rend, width, height);
}

void renderer_log_stats(RendererBackend *rend)
{
    if (!rend || !rend->log_stats)
        return;
    rend->log_stats(rend);
}

bool renderer_get_cell_size(RendererBackend *rend, int *cell_width, int *cell_height)
{
    if (!rend || !rend->get_cell_size)
        return false;
    return rend->get_cell_size(rend, cell_width, cell_height);
}

bool renderer_get_padding(RendererBackend *rend, int *left, int *top, int *right, int *bottom)
{
    if (!rend || !rend->get_padding)
        return false;
    return rend->get_padding(rend, left, top, right, bottom);
}

void renderer_set_padding(RendererBackend *rend, int left, int top, int right, int bottom)
{
    if (!rend || !rend->set_padding)
        return;
    rend->set_padding(rend, left, top, right, bottom);
}

void renderer_scroll(RendererBackend *rend, TerminalBackend *term, int delta)
{
    if (!rend || !rend->scroll)
        return;
    rend->scroll(rend, term, delta);
}

void renderer_reset_scroll(RendererBackend *rend)
{
    if (!rend || !rend->reset_scroll)
        return;
    rend->reset_scroll(rend);
}

int renderer_get_scroll_offset(RendererBackend *rend)
{
    if (!rend || !rend->get_scroll_offset)
        return 0;
    return rend->get_scroll_offset(rend);
}

void renderer_set_title(RendererBackend *rend, const char *title)
{
    if (!rend || !rend->set_title)
        return;
    rend->set_title(rend, title);
}

int renderer_render_to_png(RendererBackend *rend, TerminalBackend *term,
                           const char *output_path)
{
    if (!rend || !rend->render_to_png)
        return -1;
    return rend->render_to_png(rend, term, output_path);
}
