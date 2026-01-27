#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "event_loop_sdl3.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Custom SDL event types
static Uint32 SDL_EVENT_PTY_DATA = 0;
static Uint32 SDL_EVENT_PTY_CLOSED = 0;

// PTY data event payload
typedef struct
{
    char *data;
    size_t len;
} PtyDataPayload;

// Backend-specific context
typedef struct
{
    SDL_Renderer *sdl_renderer;
    SDL_Window *sdl_window;
    PtyContext *pty;
    SDL_Thread *pty_reader_thread;
    SDL_AtomicInt running;
    SDL_AtomicInt quit_requested;
    int force_redraw;
} SDL3EventLoopContext;

// Forward declarations
static bool sdl3_init(EventLoopBackend *loop, void *window, void *renderer);
static void sdl3_destroy(EventLoopBackend *loop);
static bool sdl3_register_pty(EventLoopBackend *loop, PtyContext *pty);
static void sdl3_unregister_pty(EventLoopBackend *loop);
static void sdl3_run(EventLoopBackend *loop, TerminalBackend *term, RendererBackend *rend,
                     EventLoopCallbacks *callbacks);
static void sdl3_request_quit(EventLoopBackend *loop);

// Backend definition
EventLoopBackend event_loop_backend_sdl3 = {
    .name = "sdl3",
    .backend_data = NULL,
    .init = sdl3_init,
    .destroy = sdl3_destroy,
    .register_pty = sdl3_register_pty,
    .unregister_pty = sdl3_unregister_pty,
    .run = sdl3_run,
    .request_quit = sdl3_request_quit,
};

// PTY reader thread function
static int pty_reader_thread_func(void *data)
{
    SDL3EventLoopContext *ctx = (SDL3EventLoopContext *)data;
    char buf[4096];

    vlog("PTY reader thread started\n");

    while (SDL_GetAtomicInt(&ctx->running)) {
        int pty_fd = pty_get_master_fd(ctx->pty);
        struct pollfd pfd = { .fd = pty_fd, .events = POLLIN, .revents = 0 };

        // Poll with 100ms timeout to check running flag periodically
        int poll_ret = poll(&pfd, 1, 100);

        if (poll_ret < 0) {
            if (errno == EINTR)
                continue;
            vlog("PTY reader thread: poll error: %s\n", strerror(errno));
            break;
        }

        if (poll_ret == 0) {
            // Timeout, check if PTY is still running
            if (!pty_is_running(ctx->pty)) {
                vlog("PTY reader thread: child process exited\n");
                break;
            }
            continue;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            vlog("PTY reader thread: poll error condition (revents=0x%x)\n", pfd.revents);
            break;
        }

        if (pfd.revents & POLLIN) {
            ssize_t n = pty_read(ctx->pty, buf, sizeof(buf));
            if (n > 0) {
                // Allocate payload and copy data
                PtyDataPayload *payload = malloc(sizeof(PtyDataPayload));
                if (payload) {
                    payload->data = malloc(n);
                    if (payload->data) {
                        memcpy(payload->data, buf, n);
                        payload->len = n;

                        // Push event to SDL queue
                        SDL_Event event = { 0 };
                        event.type = SDL_EVENT_PTY_DATA;
                        event.user.data1 = payload;

                        if (!SDL_PushEvent(&event)) {
                            vlog("PTY reader thread: failed to push event: %s\n", SDL_GetError());
                            free(payload->data);
                            free(payload);
                        }
                    } else {
                        free(payload);
                    }
                }
            } else if (n == 0) {
                // EOF - shell exited
                vlog("PTY reader thread: EOF from PTY\n");
                break;
            } else if (errno != EAGAIN && errno != EINTR) {
                // Error
                vlog("PTY reader thread: read error: %s\n", strerror(errno));
                break;
            }
        }
    }

    // Push PTY_CLOSED event
    SDL_Event event = { 0 };
    event.type = SDL_EVENT_PTY_CLOSED;
    SDL_PushEvent(&event);

    vlog("PTY reader thread exiting\n");
    return 0;
}

static bool sdl3_init(EventLoopBackend *loop, void *window, void *renderer)
{
    // Register custom SDL events
    if (SDL_EVENT_PTY_DATA == 0) {
        Uint32 events = SDL_RegisterEvents(2);
        if (events == 0) {
            fprintf(stderr, "ERROR: Failed to register custom SDL events: %s\n", SDL_GetError());
            return false;
        }
        SDL_EVENT_PTY_DATA = events;
        SDL_EVENT_PTY_CLOSED = events + 1;
        vlog("Registered custom SDL events: PTY_DATA=%u, PTY_CLOSED=%u\n",
             SDL_EVENT_PTY_DATA, SDL_EVENT_PTY_CLOSED);
    }

    // Allocate context
    SDL3EventLoopContext *ctx = calloc(1, sizeof(SDL3EventLoopContext));
    if (!ctx) {
        fprintf(stderr, "ERROR: Failed to allocate event loop context\n");
        return false;
    }

    ctx->sdl_window = (SDL_Window *)window;
    ctx->sdl_renderer = (SDL_Renderer *)renderer;
    ctx->pty = NULL;
    ctx->pty_reader_thread = NULL;
    SDL_SetAtomicInt(&ctx->running, 0);
    SDL_SetAtomicInt(&ctx->quit_requested, 0);
    ctx->force_redraw = 1; // Force initial render

    loop->backend_data = ctx;
    return true;
}

static void sdl3_destroy(EventLoopBackend *loop)
{
    if (!loop || !loop->backend_data)
        return;

    SDL3EventLoopContext *ctx = (SDL3EventLoopContext *)loop->backend_data;

    // Stop reader thread if running
    sdl3_unregister_pty(loop);

    free(ctx);
    loop->backend_data = NULL;
}

static bool sdl3_register_pty(EventLoopBackend *loop, PtyContext *pty)
{
    if (!loop || !loop->backend_data || !pty)
        return false;

    SDL3EventLoopContext *ctx = (SDL3EventLoopContext *)loop->backend_data;

    // Unregister existing PTY if any
    sdl3_unregister_pty(loop);

    ctx->pty = pty;
    return true;
}

static void sdl3_unregister_pty(EventLoopBackend *loop)
{
    if (!loop || !loop->backend_data)
        return;

    SDL3EventLoopContext *ctx = (SDL3EventLoopContext *)loop->backend_data;

    // Stop reader thread
    if (ctx->pty_reader_thread) {
        SDL_SetAtomicInt(&ctx->running, 0);
        SDL_WaitThread(ctx->pty_reader_thread, NULL);
        ctx->pty_reader_thread = NULL;
    }

    ctx->pty = NULL;
}

static void sdl3_run(EventLoopBackend *loop, TerminalBackend *term, RendererBackend *rend,
                     EventLoopCallbacks *callbacks)
{
    if (!loop || !loop->backend_data)
        return;

    SDL3EventLoopContext *ctx = (SDL3EventLoopContext *)loop->backend_data;

    if (!ctx->pty) {
        fprintf(stderr, "ERROR: No PTY registered with event loop\n");
        return;
    }

    // Start PTY reader thread
    SDL_SetAtomicInt(&ctx->running, 1);
    SDL_SetAtomicInt(&ctx->quit_requested, 0);
    ctx->pty_reader_thread = SDL_CreateThread(pty_reader_thread_func, "pty_reader", ctx);
    if (!ctx->pty_reader_thread) {
        fprintf(stderr, "ERROR: Failed to create PTY reader thread: %s\n", SDL_GetError());
        return;
    }

    vlog("Event loop starting\n");

    SDL_Event event;
    while (!SDL_GetAtomicInt(&ctx->quit_requested)) {
        // Wait for events with 16ms timeout (~60 FPS for rendering checks)
        if (SDL_WaitEventTimeout(&event, 16)) {
            do {
                if (event.type == SDL_EVENT_PTY_DATA) {
                    // Process PTY data
                    PtyDataPayload *payload = (PtyDataPayload *)event.user.data1;
                    if (payload) {
                        terminal_process_input(term, payload->data, payload->len);
                        free(payload->data);
                        free(payload);
                    }
                } else if (event.type == SDL_EVENT_PTY_CLOSED) {
                    // PTY closed, exit loop
                    vlog("PTY closed event received\n");
                    SDL_SetAtomicInt(&ctx->quit_requested, 1);
                } else if (event.type == SDL_EVENT_QUIT) {
                    vlog("SDL quit event received\n");
                    SDL_SetAtomicInt(&ctx->quit_requested, 1);
                } else if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (callbacks && callbacks->on_keyboard) {
                        KeyboardResult result = callbacks->on_keyboard(
                            callbacks->user_data,
                            event.key.key,
                            event.key.mod,
                            false,
                            NULL);

                        if (result.request_quit) {
                            SDL_SetAtomicInt(&ctx->quit_requested, 1);
                        } else if (result.force_redraw) {
                            ctx->force_redraw = 1;
                        } else if (result.len > 0 && !result.handled) {
                            // Reset scroll position when typing
                            if (renderer_get_scroll_offset(rend) != 0) {
                                renderer_reset_scroll(rend);
                                ctx->force_redraw = 1;
                            }
                            ssize_t written = pty_write(ctx->pty, result.data, result.len);
                            if (written < 0) {
                                vlog("PTY write failed: %s\n", strerror(errno));
                            }
                        }
                    }
                } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                    if (callbacks && callbacks->on_keyboard) {
                        KeyboardResult result = callbacks->on_keyboard(
                            callbacks->user_data,
                            0,
                            0,
                            true,
                            event.text.text);

                        if (result.len > 0 && !result.handled) {
                            // Reset scroll position when typing
                            if (renderer_get_scroll_offset(rend) != 0) {
                                renderer_reset_scroll(rend);
                                ctx->force_redraw = 1;
                            }
                            pty_write(ctx->pty, result.data, result.len);
                        }
                    }
                } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                    if (callbacks && callbacks->on_resize) {
                        callbacks->on_resize(callbacks->user_data,
                                             event.window.data1,
                                             event.window.data2);
                    }
                    ctx->force_redraw = 1;
                } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (callbacks && callbacks->on_scroll && event.wheel.y != 0) {
                        callbacks->on_scroll(callbacks->user_data, (int)event.wheel.y * 3);
                    }
                    ctx->force_redraw = 1;
                }
            } while (SDL_PollEvent(&event));
        }

        // Check if PTY child is still running
        if (ctx->pty && !pty_is_running(ctx->pty)) {
            vlog("PTY child process no longer running\n");
            SDL_SetAtomicInt(&ctx->quit_requested, 1);
        }

        // Render terminal only if needed
        if (terminal_needs_redraw(term) || ctx->force_redraw) {
            renderer_draw_terminal(rend, term);
            SDL_RenderPresent(ctx->sdl_renderer);
            terminal_clear_redraw(term);
            ctx->force_redraw = 0;

            // Log atlas stats after rendering activity
            renderer_log_stats(rend);
        }
    }

    vlog("Event loop exiting\n");

    // Stop reader thread
    SDL_SetAtomicInt(&ctx->running, 0);
    if (ctx->pty_reader_thread) {
        SDL_WaitThread(ctx->pty_reader_thread, NULL);
        ctx->pty_reader_thread = NULL;
    }
}

static void sdl3_request_quit(EventLoopBackend *loop)
{
    if (!loop || !loop->backend_data)
        return;

    SDL3EventLoopContext *ctx = (SDL3EventLoopContext *)loop->backend_data;
    SDL_SetAtomicInt(&ctx->quit_requested, 1);
}
