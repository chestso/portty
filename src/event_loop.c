#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "event_loop.h"
#include <stdio.h>

EventLoopBackend *event_loop_init(EventLoopBackend *loop, void *window, void *renderer)
{
    if (!loop || !loop->init) {
        fprintf(stderr, "ERROR: Invalid event loop backend\n");
        return NULL;
    }

    if (!loop->init(loop, window, renderer)) {
        fprintf(stderr, "ERROR: Failed to initialize event loop backend '%s'\n",
                loop->name ? loop->name : "unknown");
        return NULL;
    }

    vlog("Event loop backend '%s' initialized\n", loop->name ? loop->name : "unknown");
    return loop;
}

void event_loop_destroy(EventLoopBackend *loop)
{
    if (!loop)
        return;

    if (loop->destroy) {
        loop->destroy(loop);
    }

    vlog("Event loop backend '%s' destroyed\n", loop->name ? loop->name : "unknown");
}

bool event_loop_register_pty(EventLoopBackend *loop, PtyContext *pty)
{
    if (!loop || !loop->register_pty) {
        fprintf(stderr, "ERROR: Invalid event loop backend or missing register_pty\n");
        return false;
    }

    return loop->register_pty(loop, pty);
}

void event_loop_unregister_pty(EventLoopBackend *loop)
{
    if (!loop || !loop->unregister_pty)
        return;

    loop->unregister_pty(loop);
}

void event_loop_run(EventLoopBackend *loop, TerminalBackend *term, RendererBackend *rend,
                    EventLoopCallbacks *callbacks)
{
    if (!loop || !loop->run) {
        fprintf(stderr, "ERROR: Invalid event loop backend or missing run function\n");
        return;
    }

    loop->run(loop, term, rend, callbacks);
}

void event_loop_request_quit(EventLoopBackend *loop)
{
    if (!loop || !loop->request_quit)
        return;

    loop->request_quit(loop);
}
