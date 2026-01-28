#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "event_loop_sdl3.h"
#include "timer.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Custom event codes for SDL_EVENT_USER
enum BloomEventCode
{
    EVENT_PTY_DATA = 1,
    EVENT_PTY_CLOSED,
    EVENT_PTY_CHILD_EXIT,
    EVENT_CURSOR_BLINK,
};

// Cursor blink interval in milliseconds
#define CURSOR_BLINK_INTERVAL_MS 1000

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

    // Wakeup pipe to interrupt reader thread on shutdown
    int wakeup_pipe[2];

    // Timer system
    TimerManager *timers;
    TimerId cursor_blink_timer;
    bool cursor_blink_visible;
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

    int pty_fd = pty_get_master_fd(ctx->pty);
    int signal_fd = pty_signal_get_fd();
    int wakeup_fd = ctx->wakeup_pipe[0];

    while (SDL_GetAtomicInt(&ctx->running)) {
        struct pollfd pfds[3];
        int nfds = 1;

        pfds[0].fd = pty_fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;

        // Poll signal pipe if available
        if (signal_fd >= 0) {
            pfds[nfds].fd = signal_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        // Poll wakeup pipe for shutdown notification
        int wakeup_idx = -1;
        if (wakeup_fd >= 0) {
            wakeup_idx = nfds;
            pfds[nfds].fd = wakeup_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        // Poll indefinitely - we'll wake on PTY data, SIGCHLD, or wakeup
        int poll_ret = poll(pfds, nfds, -1);

        if (poll_ret < 0) {
            if (errno == EINTR)
                continue;
            vlog("PTY reader thread: poll error: %s\n", strerror(errno));
            break;
        }

        // Check wakeup pipe first (shutdown request)
        if (wakeup_idx >= 0 && (pfds[wakeup_idx].revents & POLLIN)) {
            vlog("PTY reader thread: wakeup received, shutting down\n");
            break;
        }

        // Check signal pipe (child exit)
        if (signal_fd >= 0 && (pfds[1].revents & POLLIN)) {
            pty_signal_drain();
            vlog("PTY reader thread: SIGCHLD received\n");

            // Check if our specific child actually exited
            // (SIGCHLD can be triggered by grandchild processes too)
            if (!pty_is_running(ctx->pty)) {
                vlog("PTY reader thread: child process has exited\n");
                // Push child exit event
                SDL_Event event = { 0 };
                event.type = SDL_EVENT_USER;
                event.user.code = EVENT_PTY_CHILD_EXIT;
                SDL_PushEvent(&event);
                break;
            }
            vlog("PTY reader thread: SIGCHLD was not for our child, continuing\n");
        }

        // Check for PTY errors
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            vlog("PTY reader thread: poll error condition (revents=0x%x)\n", pfds[0].revents);
            break;
        }

        // Read PTY data
        if (pfds[0].revents & POLLIN) {
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
                        event.type = SDL_EVENT_USER;
                        event.user.code = EVENT_PTY_DATA;
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
    event.type = SDL_EVENT_USER;
    event.user.code = EVENT_PTY_CLOSED;
    SDL_PushEvent(&event);

    vlog("PTY reader thread exiting\n");
    return 0;
}

static bool sdl3_init(EventLoopBackend *loop, void *window, void *renderer)
{
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

    // Create wakeup pipe for reader thread shutdown
    ctx->wakeup_pipe[0] = -1;
    ctx->wakeup_pipe[1] = -1;
    if (pipe(ctx->wakeup_pipe) < 0) {
        fprintf(stderr, "ERROR: Failed to create wakeup pipe: %s\n", strerror(errno));
        free(ctx);
        return false;
    }
    // Set non-blocking and close-on-exec
    fcntl(ctx->wakeup_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(ctx->wakeup_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(ctx->wakeup_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(ctx->wakeup_pipe[1], F_SETFD, FD_CLOEXEC);

    // Initialize timer system
    ctx->timers = timer_manager_create();
    if (!ctx->timers) {
        fprintf(stderr, "ERROR: Failed to create timer manager\n");
        close(ctx->wakeup_pipe[0]);
        close(ctx->wakeup_pipe[1]);
        free(ctx);
        return false;
    }
    ctx->cursor_blink_timer = TIMER_INVALID;
    ctx->cursor_blink_visible = true;

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

    // Destroy timer manager
    if (ctx->timers) {
        timer_manager_destroy(ctx->timers);
        ctx->timers = NULL;
    }

    // Close wakeup pipe
    if (ctx->wakeup_pipe[0] >= 0) {
        close(ctx->wakeup_pipe[0]);
        ctx->wakeup_pipe[0] = -1;
    }
    if (ctx->wakeup_pipe[1] >= 0) {
        close(ctx->wakeup_pipe[1]);
        ctx->wakeup_pipe[1] = -1;
    }

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

        // Wake up the reader thread from poll() by writing to the wakeup pipe
        if (ctx->wakeup_pipe[1] >= 0) {
            char c = 1;
            write(ctx->wakeup_pipe[1], &c, 1);
        }

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

    // Start cursor blink timer
    ctx->cursor_blink_visible = true;
    ctx->cursor_blink_timer = timer_add(ctx->timers, CURSOR_BLINK_INTERVAL_MS, true,
                                        EVENT_CURSOR_BLINK, NULL);

    // Start PTY reader thread
    SDL_SetAtomicInt(&ctx->running, 1);
    SDL_SetAtomicInt(&ctx->quit_requested, 0);
    ctx->pty_reader_thread = SDL_CreateThread(pty_reader_thread_func, "pty_reader", ctx);
    if (!ctx->pty_reader_thread) {
        fprintf(stderr, "ERROR: Failed to create PTY reader thread: %s\n", SDL_GetError());
        return;
    }

    // Enable text input for proper Unicode character handling
    SDL_StartTextInput(ctx->sdl_window);

    vlog("Event loop starting (event-driven)\n");

    SDL_Event event;
    while (!SDL_GetAtomicInt(&ctx->quit_requested)) {
        // Wait for events - truly event-driven, no timeout
        if (!SDL_WaitEvent(&event)) {
            vlog("SDL_WaitEvent error: %s\n", SDL_GetError());
            break;
        }

        // Process all pending events
        do {
            if (event.type == SDL_EVENT_USER) {
                switch (event.user.code) {
                case EVENT_PTY_DATA:
                {
                    // Process PTY data
                    PtyDataPayload *payload = (PtyDataPayload *)event.user.data1;
                    if (payload) {
                        terminal_process_input(term, payload->data, payload->len);
                        free(payload->data);
                        free(payload);
                    }
                    break;
                }
                case EVENT_PTY_CLOSED:
                    // PTY closed, exit loop
                    vlog("PTY closed event received\n");
                    SDL_SetAtomicInt(&ctx->quit_requested, 1);
                    break;

                case EVENT_PTY_CHILD_EXIT:
                    // Child process exited via SIGCHLD
                    vlog("PTY child exit event received\n");
                    SDL_SetAtomicInt(&ctx->quit_requested, 1);
                    break;

                case EVENT_CURSOR_BLINK:
                    // Only toggle if terminal has blink enabled
                    if (terminal_get_cursor_blink(term)) {
                        ctx->cursor_blink_visible = !ctx->cursor_blink_visible;
                        ctx->force_redraw = 1;
                    }
                    break;
                }
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

                        // Reset cursor blink on user input
                        ctx->cursor_blink_visible = true;
                        timer_reset(ctx->timers, ctx->cursor_blink_timer);
                        ctx->force_redraw = 1;

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

                        // Reset cursor blink on user input
                        ctx->cursor_blink_visible = true;
                        timer_reset(ctx->timers, ctx->cursor_blink_timer);
                        ctx->force_redraw = 1;

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
            } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                vlog("Window close requested\n");
                SDL_SetAtomicInt(&ctx->quit_requested, 1);
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                if (event.wheel.y != 0) {
                    bool consumed = false;
                    // Try on_mouse first (for programs that capture scroll events)
                    if (callbacks && callbacks->on_mouse) {
                        // Button 4 = wheel up, button 5 = wheel down
                        int button = (event.wheel.y > 0) ? 4 : 5;
                        int clicks = abs((int)event.wheel.y);
                        for (int i = 0; i < clicks && !consumed; i++) {
                            // Get current mouse position
                            float mx, my;
                            SDL_GetMouseState(&mx, &my);
                            consumed = callbacks->on_mouse(callbacks->user_data, (int)mx, (int)my,
                                                           button, true, SDL_GetModState());
                        }
                    }
                    // Fall back to scrollback scrolling if not consumed
                    if (!consumed && callbacks && callbacks->on_scroll) {
                        callbacks->on_scroll(callbacks->user_data, (int)event.wheel.y * 3);
                    }
                }
                ctx->force_redraw = 1;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                if (callbacks && callbacks->on_mouse) {
                    bool pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                    int button = event.button.button; // SDL: 1=left, 2=middle, 3=right
                    if (callbacks->on_mouse(callbacks->user_data, (int)event.button.x,
                                            (int)event.button.y, button, pressed,
                                            SDL_GetModState())) {
                        ctx->force_redraw = 1;
                    }
                }
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                if (callbacks && callbacks->on_mouse) {
                    // Motion events: button=0, pressed indicates buttons currently held
                    bool any_button_pressed = (event.motion.state != 0);
                    if (callbacks->on_mouse(callbacks->user_data, (int)event.motion.x,
                                            (int)event.motion.y, 0, any_button_pressed,
                                            SDL_GetModState())) {
                        ctx->force_redraw = 1;
                    }
                }
            }
        } while (SDL_PollEvent(&event));

        // Render terminal only if needed
        if (terminal_needs_redraw(term) || ctx->force_redraw) {
            // Update window title if changed
            renderer_set_title(rend, terminal_get_title(term));

            // Cursor visible if: blink disabled OR (blink enabled AND in visible phase)
            bool cursor_vis = !terminal_get_cursor_blink(term) || ctx->cursor_blink_visible;
            renderer_draw_terminal(rend, term, cursor_vis);
            SDL_RenderPresent(ctx->sdl_renderer);
            terminal_clear_redraw(term);
            ctx->force_redraw = 0;

            // Log atlas stats after rendering activity
            renderer_log_stats(rend);
        }
    }

    vlog("Event loop exiting\n");

    // Stop text input
    SDL_StopTextInput(ctx->sdl_window);

    // Stop cursor blink timer
    if (ctx->cursor_blink_timer != TIMER_INVALID) {
        timer_remove(ctx->timers, ctx->cursor_blink_timer);
        ctx->cursor_blink_timer = TIMER_INVALID;
    }

    // Stop reader thread
    SDL_SetAtomicInt(&ctx->running, 0);
    if (ctx->pty_reader_thread) {
        // Wake up the reader thread from poll()
        if (ctx->wakeup_pipe[1] >= 0) {
            char c = 1;
            write(ctx->wakeup_pipe[1], &c, 1);
        }
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
