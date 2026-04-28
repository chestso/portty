#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    free(plat->last_title);
    plat->last_title = NULL;

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
    if (plat->last_title && title && strcmp(plat->last_title, title) == 0)
        return;
    if (!plat->last_title && !title)
        return;
    free(plat->last_title);
    plat->last_title = title ? strdup(title) : NULL;
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

bool platform_clipboard_paste_async(PlatformBackend *plat,
                                    TerminalBackend *term, PtyContext *pty)
{
    if (!plat || !plat->clipboard_paste_async)
        return false;
    return plat->clipboard_paste_async(plat, term, pty);
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

void platform_pause_pty(PlatformBackend *plat)
{
    if (!plat || !plat->pause_pty)
        return;
    plat->pause_pty(plat);
}

void platform_resume_pty(PlatformBackend *plat)
{
    if (!plat || !plat->resume_pty)
        return;
    plat->resume_pty(plat);
}

char *platform_get_default_font(PlatformBackend *plat)
{
    if (plat && plat->get_default_font)
        return plat->get_default_font(plat);
    return NULL;
}

float platform_get_display_scale(PlatformBackend *plat)
{
    if (plat && plat->get_display_scale)
        return plat->get_display_scale(plat);
    return 0.0f;
}

bool platform_get_display_size(PlatformBackend *plat, int *width, int *height)
{
    if (plat && plat->get_display_size)
        return plat->get_display_size(plat, width, height);
    return false;
}

bool platform_open_url(PlatformBackend *plat, const char *url)
{
    if (!plat || !plat->open_url || !url)
        return false;
    return plat->open_url(plat, url);
}

void platform_set_cursor(PlatformBackend *plat, PlatformCursor cursor)
{
    if (!plat || !plat->set_cursor)
        return;
    plat->set_cursor(plat, cursor);
}
