#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "platform.h"
#include <stdio.h>

PlatformBackend *platform_init(PlatformBackend *plat)
{
    if (!plat || !plat->init) {
        fprintf(stderr, "ERROR: Invalid platform backend\n");
        return NULL;
    }

    if (!plat->init(plat)) {
        fprintf(stderr, "ERROR: Failed to initialize platform backend '%s'\n",
                plat->name ? plat->name : "unknown");
        return NULL;
    }

    vlog("Platform backend '%s' initialized\n", plat->name ? plat->name : "unknown");
    return plat;
}

void platform_destroy(PlatformBackend *plat)
{
    if (!plat)
        return;

    if (plat->destroy) {
        plat->destroy(plat);
    }

    vlog("Platform backend '%s' destroyed\n", plat->name ? plat->name : "unknown");
}

bool platform_create_window(PlatformBackend *plat, const char *title,
                            int width, int height)
{
    if (!plat || !plat->create_window)
        return false;
    return plat->create_window(plat, title, width, height);
}

void platform_show_window(PlatformBackend *plat)
{
    if (!plat || !plat->show_window)
        return;
    plat->show_window(plat);
}

void platform_set_window_size(PlatformBackend *plat, int width, int height)
{
    if (!plat || !plat->set_window_size)
        return;
    plat->set_window_size(plat, width, height);
}

void platform_set_window_title(PlatformBackend *plat, const char *title)
{
    if (!plat || !plat->set_window_title)
        return;
    plat->set_window_title(plat, title);
}

void *platform_get_sdl_renderer(PlatformBackend *plat)
{
    if (!plat || !plat->get_sdl_renderer)
        return NULL;
    return plat->get_sdl_renderer(plat);
}

void *platform_get_sdl_window(PlatformBackend *plat)
{
    if (!plat || !plat->get_sdl_window)
        return NULL;
    return plat->get_sdl_window(plat);
}

char *platform_clipboard_get(PlatformBackend *plat)
{
    if (!plat || !plat->clipboard_get)
        return NULL;
    return plat->clipboard_get(plat);
}

bool platform_clipboard_set(PlatformBackend *plat, const char *text)
{
    if (!plat || !plat->clipboard_set)
        return false;
    return plat->clipboard_set(plat, text);
}

void platform_clipboard_free(PlatformBackend *plat, char *text)
{
    if (!plat || !plat->clipboard_free)
        return;
    plat->clipboard_free(plat, text);
}

bool platform_register_pty(PlatformBackend *plat, PtyContext *pty)
{
    if (!plat || !plat->register_pty) {
        fprintf(stderr, "ERROR: Invalid platform backend or missing register_pty\n");
        return false;
    }
    return plat->register_pty(plat, pty);
}

void platform_run(PlatformBackend *plat, TerminalBackend *term,
                  RendererBackend *rend, PlatformCallbacks *callbacks)
{
    if (!plat || !plat->run) {
        fprintf(stderr, "ERROR: Invalid platform backend or missing run function\n");
        return;
    }
    plat->run(plat, term, rend, callbacks);
}

void platform_request_quit(PlatformBackend *plat)
{
    if (!plat || !plat->request_quit)
        return;
    plat->request_quit(plat);
}
