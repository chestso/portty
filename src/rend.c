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

int renderer_load_fonts(RendererBackend *rend)
{
    if (!rend || !rend->load_fonts)
        return -1;
    return rend->load_fonts(rend);
}

void renderer_draw_terminal(RendererBackend *rend, TerminalBackend *term)
{
    if (!rend || !rend->draw_terminal)
        return;
    rend->draw_terminal(rend, term);
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

void renderer_toggle_debug_grid(RendererBackend *rend)
{
    if (!rend || !rend->toggle_debug_grid)
        return;
    rend->toggle_debug_grid(rend);
}
