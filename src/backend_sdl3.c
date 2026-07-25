#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_version.h"

#include "backend_sdl3.h"
#include "common.h"
#include "os_compat.h"
#include "pager.h"
#include "path_compat.h"
#include "png_reader.h"
#include "png_writer.h"
#include "portty_app.h"
#include "portty_debug_script.h"
#include "portty_pty.h"
#include "rend_sdl3.h"
#include "term.h"
#include "timer.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <dwmapi.h>
#include <io.h>
#include <windows.h>
#define access _access
#define R_OK   4
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

// ── Custom event codes for SDL_EVENT_USER ────────────────────────────────
enum PorttyEventCode
{
    EVENT_PTY_DATA = 1,
    EVENT_PTY_CLOSED,
    EVENT_PTY_CHILD_EXIT,
    EVENT_CURSOR_BLINK,
    EVENT_AUTOSCROLL_TICK,
    EVENT_LOTTIE_TICK,
    EVENT_NOTIFY_SHOW,
};

// PTY data event payload
typedef struct
{
    size_t len;
    char data[];
} PtyDataPayload;

// Deferred clipboard free list (Wayland use-after-free workaround).
typedef struct ClipboardDeferredFree
{
    char *ptr;
    int age;
    struct ClipboardDeferredFree *next;
} ClipboardDeferredFree;
static ClipboardDeferredFree *clipboard_deferred_head;

#define CLIPBOARD_DEFERRED_MIN_AGE 2

// SDL keycode → terminal key mapping
static const struct
{
    int sdl_key;
    int term_key;
} key_map[] = {
    { SDLK_RETURN, TERM_KEY_ENTER },
    { SDLK_BACKSPACE, TERM_KEY_BACKSPACE },
    { SDLK_ESCAPE, TERM_KEY_ESCAPE },
    { SDLK_TAB, TERM_KEY_TAB },
    { SDLK_UP, TERM_KEY_UP },
    { SDLK_DOWN, TERM_KEY_DOWN },
    { SDLK_RIGHT, TERM_KEY_RIGHT },
    { SDLK_LEFT, TERM_KEY_LEFT },
    { SDLK_HOME, TERM_KEY_HOME },
    { SDLK_END, TERM_KEY_END },
    { SDLK_INSERT, TERM_KEY_INS },
    { SDLK_DELETE, TERM_KEY_DEL },
    { SDLK_PAGEUP, TERM_KEY_PAGEUP },
    { SDLK_PAGEDOWN, TERM_KEY_PAGEDOWN },
    { SDLK_F1, TERM_KEY_F1 },
    { SDLK_F2, TERM_KEY_F2 },
    { SDLK_F3, TERM_KEY_F3 },
    { SDLK_F4, TERM_KEY_F4 },
    { SDLK_F5, TERM_KEY_F5 },
    { SDLK_F6, TERM_KEY_F6 },
    { SDLK_F7, TERM_KEY_F7 },
    { SDLK_F8, TERM_KEY_F8 },
    { SDLK_F9, TERM_KEY_F9 },
    { SDLK_F10, TERM_KEY_F10 },
    { SDLK_F11, TERM_KEY_F11 },
    { SDLK_F12, TERM_KEY_F12 },
    { SDLK_KP_0, TERM_KEY_KP_0 },
    { SDLK_KP_1, TERM_KEY_KP_1 },
    { SDLK_KP_2, TERM_KEY_KP_2 },
    { SDLK_KP_3, TERM_KEY_KP_3 },
    { SDLK_KP_4, TERM_KEY_KP_4 },
    { SDLK_KP_5, TERM_KEY_KP_5 },
    { SDLK_KP_6, TERM_KEY_KP_6 },
    { SDLK_KP_7, TERM_KEY_KP_7 },
    { SDLK_KP_8, TERM_KEY_KP_8 },
    { SDLK_KP_9, TERM_KEY_KP_9 },
    { SDLK_KP_MULTIPLY, TERM_KEY_KP_MULTIPLY },
    { SDLK_KP_PLUS, TERM_KEY_KP_PLUS },
    { SDLK_KP_COMMA, TERM_KEY_KP_COMMA },
    { SDLK_KP_MINUS, TERM_KEY_KP_MINUS },
    { SDLK_KP_PERIOD, TERM_KEY_KP_PERIOD },
    { SDLK_KP_DIVIDE, TERM_KEY_KP_DIVIDE },
    { SDLK_KP_ENTER, TERM_KEY_KP_ENTER },
    { SDLK_KP_EQUALS, TERM_KEY_KP_EQUAL },
};

typedef struct
{
    PorttyApp *app;
    PtyContext *pty;
    TerminalBackend *term;
    RendererSdl3Data rend;

    SDL_Window *window;
    SDL_Renderer *sdl_renderer;
    SDL_Thread *pty_reader_thread;
    SDL_AtomicInt running;
    SDL_AtomicInt quit_requested;
    SDL_AtomicInt pty_paused;

#ifdef _WIN32
    HANDLE wakeup_event;
#else
    int wakeup_pipe[2];
#endif

    TimerManager *timers;
    TimerId cursor_blink_timer;
    TimerId autoscroll_timer;
    TimerId lottie_timer;
    bool cursor_blink_visible;
    bool has_focus;

    SDL_Cursor *cursor_text;
    SDL_Cursor *cursor_pointer;
    PorttyCursor current_cursor;

    char exe_path[PATH_MAX];
    char working_dir[PATH_MAX];
    char *last_title;

    bool left_button_down;
    bool left_button_up_buffered;
    int left_button_up_x;
    int left_button_up_y;
    Uint32 left_button_up_tick;

    float wheel_accum_y;

    // Debug script infrastructure
    PorttyDebugScript *debug_script;
    int debug_cmd_index;
    bool debug_script_done;
    bool debug_pending_screendump;
    char debug_screendump_path[512];
} Sdl3BackendData;

static Sdl3BackendData *sdl3_data(PorttyBackend *self)
{
    return (Sdl3BackendData *)self->data;
}

static int sdl_mod_to_term(int mod)
{
    int m = TERM_MOD_NONE;
    if (mod & SDL_KMOD_SHIFT)
        m |= TERM_MOD_SHIFT;
    if (mod & SDL_KMOD_ALT)
        m |= TERM_MOD_ALT;
    if (mod & SDL_KMOD_CTRL)
        m |= TERM_MOD_CTRL;
    return m;
}

#include "portty_icon_png.h"

static void set_window_icon(SDL_Window *win)
{
    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    if (png_read_rgba_mem(portty_icon_png, portty_icon_png_len, &pixels, &w,
                          &h) != 0) {
        fprintf(stderr, "WARNING: Failed to decode embedded icon PNG\n");
        return;
    }

    SDL_Surface *surface =
        SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) {
        fprintf(stderr, "WARNING: Failed to create icon surface: %s\n",
                SDL_GetError());
        free(pixels);
        return;
    }

    if (!SDL_SetWindowIcon(win, surface)) {
        vlog("SDL_SetWindowIcon skipped: %s\n", SDL_GetError());
    } else {
        vlog("Window icon set from embedded PNG (%dx%d)\n", w, h);
    }

    SDL_DestroySurface(surface);
    free(pixels);
}

// ── Clipboard deferred-free helpers ──────────────────────────────────────

static const void *clipboard_data_callback(void *userdata, const char *mime_type,
                                           size_t *size)
{
    (void)mime_type;
    const char *text = (const char *)userdata;
    if (size)
        *size = text ? strlen(text) : 0;
    return text;
}

static void clipboard_cleanup_callback(void *userdata)
{
    char *ptr = (char *)userdata;
    if (!ptr)
        return;
    ClipboardDeferredFree *entry = malloc(sizeof(*entry));
    if (!entry) {
        free(ptr);
        return;
    }
    entry->ptr = ptr;
    entry->age = 0;
    entry->next = clipboard_deferred_head;
    clipboard_deferred_head = entry;
}

static void clipboard_deferred_free_advance(void)
{
    ClipboardDeferredFree **pp = &clipboard_deferred_head;
    while (*pp) {
        (*pp)->age++;
        if ((*pp)->age >= CLIPBOARD_DEFERRED_MIN_AGE) {
            ClipboardDeferredFree *old = *pp;
            *pp = old->next;
            free(old->ptr);
            free(old);
        } else {
            pp = &(*pp)->next;
        }
    }
}

// ── PTY reader thread ────────────────────────────────────────────────────

#ifdef _WIN32
static int pty_reader_thread_func(void *data)
{
    Sdl3BackendData *d = (Sdl3BackendData *)data;
    char buf[4096];

    vlog("PTY reader thread started (W32)\n");

    HANDLE hProcess = (HANDLE)pty_get_process_handle(d->pty);

    while (SDL_GetAtomicInt(&d->running)) {
        if (SDL_GetAtomicInt(&d->pty_paused)) {
            HANDLE wait_h[2] = { d->wakeup_event, hProcess };
            DWORD wr = WaitForMultipleObjects(2, wait_h, FALSE, INFINITE);
            ResetEvent(d->wakeup_event);

            if (wr == WAIT_OBJECT_0) {
                if (!SDL_GetAtomicInt(&d->running))
                    break;
                continue;
            }
            if (wr == WAIT_OBJECT_0 + 1) {
                vlog("PTY reader thread: child process exited\n");
                SDL_Event ev = { 0 };
                ev.type = SDL_EVENT_USER;
                ev.user.code = EVENT_PTY_CHILD_EXIT;
                SDL_PushEvent(&ev);
                break;
            }
            break;
        }

        ssize_t n = pty_read(d->pty, buf, sizeof(buf));
        if (n > 0) {
            PtyDataPayload *payload = malloc(sizeof(PtyDataPayload) + n);
            if (payload) {
                payload->len = n;
                memcpy(payload->data, buf, n);
                SDL_Event ev = { 0 };
                ev.type = SDL_EVENT_USER;
                ev.user.code = EVENT_PTY_DATA;
                ev.user.data1 = payload;
                if (!SDL_PushEvent(&ev)) {
                    vlog("PTY reader thread: failed to push event: %s\n",
                         SDL_GetError());
                    free(payload);
                }
            }
        } else if (n == 0) {
            vlog("PTY reader thread: EOF from PTY\n");
            break;
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                vlog("PTY reader thread: pipe closed\n");
            } else {
                vlog("PTY reader thread: read error: %lu\n", err);
            }
            break;
        }

        if (!pty_is_running(d->pty)) {
            vlog("PTY reader thread: child exited after read\n");
            SDL_Event ev = { 0 };
            ev.type = SDL_EVENT_USER;
            ev.user.code = EVENT_PTY_CHILD_EXIT;
            SDL_PushEvent(&ev);
            break;
        }
    }

    SDL_Event event = { 0 };
    event.type = SDL_EVENT_USER;
    event.user.code = EVENT_PTY_CLOSED;
    SDL_PushEvent(&event);

    vlog("PTY reader thread exiting\n");
    return 0;
}
#else
static int pty_reader_thread_func(void *data)
{
    Sdl3BackendData *d = (Sdl3BackendData *)data;
    char buf[4096];

    vlog("PTY reader thread started\n");

    int pty_fd = pty_get_master_fd(d->pty);
    int signal_fd = pty_signal_get_fd();
    int wakeup_fd = d->wakeup_pipe[0];

    while (SDL_GetAtomicInt(&d->running)) {
        bool paused = SDL_GetAtomicInt(&d->pty_paused) != 0;

        struct pollfd pfds[3];
        int nfds = 0;
        int pty_idx = -1;
        int signal_idx = -1;
        int wakeup_idx = -1;

        if (!paused) {
            pty_idx = nfds;
            pfds[nfds].fd = pty_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        if (signal_fd >= 0) {
            signal_idx = nfds;
            pfds[nfds].fd = signal_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        if (wakeup_fd >= 0) {
            wakeup_idx = nfds;
            pfds[nfds].fd = wakeup_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        int poll_ret = poll(pfds, nfds, -1);

        if (poll_ret < 0) {
            if (errno == EINTR)
                continue;
            vlog("PTY reader thread: poll error: %s\n", strerror(errno));
            break;
        }

        if (wakeup_idx >= 0 && (pfds[wakeup_idx].revents & POLLIN)) {
            char tmp;
            while (read(wakeup_fd, &tmp, 1) > 0)
                ;
            if (!SDL_GetAtomicInt(&d->running)) {
                vlog("PTY reader thread: wakeup received, shutting down\n");
                break;
            }
            continue;
        }

        if (signal_idx >= 0 && (pfds[signal_idx].revents & POLLIN)) {
            pty_signal_drain();
            vlog("PTY reader thread: SIGCHLD received\n");

            if (!pty_is_running(d->pty)) {
                vlog("PTY reader thread: child process has exited\n");
                SDL_Event event = { 0 };
                event.type = SDL_EVENT_USER;
                event.user.code = EVENT_PTY_CHILD_EXIT;
                SDL_PushEvent(&event);
                break;
            }
            vlog("PTY reader thread: SIGCHLD was not for our child, continuing\n");
        }

        if (pty_idx >= 0 && (pfds[pty_idx].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            vlog("PTY reader thread: poll error condition (revents=0x%x)\n", pfds[pty_idx].revents);
            break;
        }

        if (pty_idx >= 0 && (pfds[pty_idx].revents & POLLIN)) {
            ssize_t n = pty_read(d->pty, buf, sizeof(buf));
            if (n > 0) {
                PtyDataPayload *payload = malloc(sizeof(PtyDataPayload) + n);
                if (payload) {
                    payload->len = n;
                    memcpy(payload->data, buf, n);
                    SDL_Event event = { 0 };
                    event.type = SDL_EVENT_USER;
                    event.user.code = EVENT_PTY_DATA;
                    event.user.data1 = payload;
                    if (!SDL_PushEvent(&event)) {
                        vlog("PTY reader thread: failed to push event: %s\n", SDL_GetError());
                        free(payload);
                    }
                }
            } else if (n == 0) {
                vlog("PTY reader thread: EOF from PTY\n");
                break;
            } else if (errno != EAGAIN && errno != EINTR) {
                vlog("PTY reader thread: read error: %s\n", strerror(errno));
                break;
            }
        }
    }

    SDL_Event event = { 0 };
    event.type = SDL_EVENT_USER;
    event.user.code = EVENT_PTY_CLOSED;
    SDL_PushEvent(&event);

    vlog("PTY reader thread exiting\n");
    return 0;
}
#endif

// ── Wakeup helper ────────────────────────────────────────────────────────

static void sdl3_wakeup_reader(Sdl3BackendData *d)
{
#ifdef _WIN32
    SetEvent(d->wakeup_event);
#else
    if (d->wakeup_pipe[1] >= 0) {
        char c = 1;
        (void)write(d->wakeup_pipe[1], &c, 1);
    }
#endif
}

// ── Lifecycle ────────────────────────────────────────────────────────────

static bool sdl3_init(PorttyBackend *self, PorttyApp *app,
                      const char *title, int width, int height)
{
    (void)width;
    (void)height;
    Sdl3BackendData *d = calloc(1, sizeof(*d));
    if (!d)
        return false;
    self->data = d;
    d->app = app;
    d->term = app->term;
    d->pty = app->pty;
    d->timers = timer_manager_create();
    d->cursor_blink_timer = TIMER_INVALID;
    d->lottie_timer = TIMER_INVALID;
    d->autoscroll_timer = TIMER_INVALID;
    d->cursor_blink_visible = true;
    d->has_focus = true;

    app->backend = self;

    // Set app metadata before SDL initialization
    if (!SDL_SetAppMetadata("portty", PORTTY_VERSION, "portty")) {
        fprintf(stderr, "WARNING: Failed to set SDL app metadata: %s\n", SDL_GetError());
    }

    if (verbose) {
        int sdl_version = SDL_GetVersion();
        fprintf(stderr, "DEBUG: SDL version %d.%d.%d\n",
                SDL_VERSIONNUM_MAJOR(sdl_version),
                SDL_VERSIONNUM_MINOR(sdl_version),
                SDL_VERSIONNUM_MICRO(sdl_version));
    }

    SDL_ClearError();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        const char *error = SDL_GetError();
        fprintf(stderr, "ERROR: Failed to initialize SDL video subsystem\n");
        if (error && error[0] != '\0')
            fprintf(stderr, "ERROR: SDL_GetError() returned: '%s'\n", error);
        timer_manager_destroy(d->timers);
        free(d);
        self->data = NULL;
        return false;
    }

    SDL_SetAtomicInt(&d->running, 0);
    SDL_SetAtomicInt(&d->quit_requested, 0);
    SDL_SetAtomicInt(&d->pty_paused, 0);

    // Create wakeup mechanism for reader thread
#ifdef _WIN32
    d->wakeup_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!d->wakeup_event) {
        fprintf(stderr, "ERROR: Failed to create wakeup event: %lu\n", GetLastError());
        timer_manager_destroy(d->timers);
        SDL_Quit();
        free(d);
        self->data = NULL;
        return false;
    }
#else
    d->wakeup_pipe[0] = -1;
    d->wakeup_pipe[1] = -1;
    if (pipe(d->wakeup_pipe) < 0) {
        fprintf(stderr, "ERROR: Failed to create wakeup pipe: %s\n", strerror(errno));
        timer_manager_destroy(d->timers);
        SDL_Quit();
        free(d);
        self->data = NULL;
        return false;
    }
    fcntl(d->wakeup_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(d->wakeup_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(d->wakeup_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(d->wakeup_pipe[1], F_SETFD, FD_CLOEXEC);
#endif

    // Cache exe path
    os_compat_get_exe_path(d->exe_path, sizeof(d->exe_path));

    // Create window
    vlog("Creating window (placeholder size, will resize after font load)\n");
    SDL_ClearError();

    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    d->window = SDL_CreateWindow(title, 800, 600, window_flags);
    if (!d->window) {
        const char *error = SDL_GetError();
        fprintf(stderr, "ERROR: Failed to create window: %s\n",
                (error && error[0]) ? error : "no specific error message");
        goto fail_wakeup;
    }
    vlog("Window created successfully\n");

    set_window_icon(d->window);

#ifdef _WIN32
    {
        HWND hwnd = (HWND)SDL_GetPointerProperty(
            SDL_GetWindowProperties(d->window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd) {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                  &dark, sizeof(dark));
            int backdrop = 2;
            DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                  &backdrop, sizeof(backdrop));
            COLORREF caption = 0x00282828;
            DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR,
                                  &caption, sizeof(caption));
            DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                  &corner, sizeof(corner));
            vlog("DWM: dark mode + Mica + rounded corners applied\n");
        }
    }
#endif

    // Create GPU renderer
    vlog("Creating GPU renderer\n");
    SDL_ClearError();

    SDL_PropertiesID rprops = SDL_CreateProperties();
    if (rprops) {
        SDL_SetPointerProperty(rprops, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, d->window);
        SDL_SetStringProperty(rprops, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
        d->sdl_renderer = SDL_CreateRendererWithProperties(rprops);
        SDL_DestroyProperties(rprops);
    }
    if (!d->sdl_renderer) {
        const char *error = SDL_GetError();
        fprintf(stderr,
                "ERROR: Failed to create GPU renderer (requires Vulkan/D3D12/Metal): %s\n",
                (error && error[0]) ? error : "no specific error message");
        SDL_DestroyWindow(d->window);
        d->window = NULL;
        goto fail_wakeup;
    }
    vlog("Renderer created: %s\n", SDL_GetRendererName(d->sdl_renderer));

    SDL_SetRenderVSync(d->sdl_renderer, 0);

    // Initialize renderer backend
    if (!rend_sdl3_init(&d->rend, d->window, d->sdl_renderer)) {
        SDL_DestroyRenderer(d->sdl_renderer);
        d->sdl_renderer = NULL;
        SDL_DestroyWindow(d->window);
        d->window = NULL;
        goto fail_wakeup;
    }

    return true;

fail_wakeup:
#ifdef _WIN32
    CloseHandle(d->wakeup_event);
#else
    close(d->wakeup_pipe[0]);
    close(d->wakeup_pipe[1]);
#endif
    timer_manager_destroy(d->timers);
    SDL_Quit();
    free(d);
    self->data = NULL;
    return false;
}

static void sdl3_destroy(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;

    // Stop reader thread if running
    if (d->pty_reader_thread) {
        SDL_SetAtomicInt(&d->running, 0);
#ifdef _WIN32
        if (d->pty)
            pty_close_console(d->pty);
#endif
        sdl3_wakeup_reader(d);
        SDL_WaitThread(d->pty_reader_thread, NULL);
        d->pty_reader_thread = NULL;
    }

    rend_sdl3_destroy(&d->rend);

    if (d->timers) {
        timer_manager_destroy(d->timers);
        d->timers = NULL;
    }

#ifdef _WIN32
    if (d->wakeup_event) {
        CloseHandle(d->wakeup_event);
        d->wakeup_event = NULL;
    }
#else
    if (d->wakeup_pipe[0] >= 0)
        close(d->wakeup_pipe[0]);
    if (d->wakeup_pipe[1] >= 0)
        close(d->wakeup_pipe[1]);
#endif

    if (d->cursor_text) {
        SDL_DestroyCursor(d->cursor_text);
        d->cursor_text = NULL;
    }
    if (d->cursor_pointer) {
        SDL_DestroyCursor(d->cursor_pointer);
        d->cursor_pointer = NULL;
    }

    free(d->last_title);

    // Flush any remaining deferred clipboard frees
    while (clipboard_deferred_head) {
        ClipboardDeferredFree *next = clipboard_deferred_head->next;
        free(clipboard_deferred_head->ptr);
        free(clipboard_deferred_head);
        clipboard_deferred_head = next;
    }

    if (d->sdl_renderer) {
        SDL_DestroyRenderer(d->sdl_renderer);
        d->sdl_renderer = NULL;
    }
    if (d->window) {
        SDL_DestroyWindow(d->window);
        d->window = NULL;
    }

    free(d);
    self->data = NULL;

    SDL_Quit();
}

// ── Window management ────────────────────────────────────────────────────

static void sdl3_show_window(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (d && d->window) {
        vlog("Showing window\n");
        SDL_ShowWindow(d->window);
    }
}

static void sdl3_set_window_title(PorttyBackend *self, const char *title)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d || !d->window)
        return;
    // Title dedup
    if (d->last_title && title && strcmp(d->last_title, title) == 0)
        return;
    if (!d->last_title && !title)
        return;
    free(d->last_title);
    d->last_title = title ? strdup(title) : NULL;
    SDL_SetWindowTitle(d->window, title ? title : "portty");
    vlog("Window title set to: %s\n", title ? title : "(default)");
}

static void sdl3_set_window_size(PorttyBackend *self, int width, int height)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (d && d->window) {
        float scale = d->rend.content_scale;
        if (scale <= 0.0f)
            scale = 1.0f;
        int logical_w = (int)((float)width / scale + 0.5f);
        int logical_h = (int)((float)height / scale + 0.5f);
        SDL_SetWindowSize(d->window, logical_w, logical_h);

        SDL_SyncWindow(d->window);
        int pix_w, pix_h;
        SDL_GetWindowSizeInPixels(d->window, &pix_w, &pix_h);
        if (pix_w > 0 && pix_h > 0) {
            if (pix_w < width) {
                int extra = (int)ceilf((float)(width - pix_w) / scale);
                SDL_SetWindowSize(d->window, logical_w + extra, logical_h);
                SDL_SyncWindow(d->window);
                SDL_GetWindowSizeInPixels(d->window, &pix_w, &pix_h);
            }
            if (pix_w > 0 && pix_h > 0)
                rend_sdl3_resize(&d->rend, pix_w, pix_h);
        }
    }
}

static bool sdl3_get_drawable_size(PorttyBackend *self, int *w, int *h)
{
    (void)self;
    (void)w;
    (void)h;
    return false;
}

static float sdl3_get_display_scale(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (d && d->window) {
        float scale = SDL_GetWindowDisplayScale(d->window);
        if (scale > 0.0f)
            return scale;
    }
    return 0.0f;
}

static bool sdl3_get_display_size(PorttyBackend *self, int *w, int *h)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return false;
    SDL_DisplayID display_id = 0;
    if (d->window)
        display_id = SDL_GetDisplayForWindow(d->window);
    if (!display_id)
        display_id = SDL_GetPrimaryDisplay();
    if (!display_id)
        return false;
    SDL_Rect bounds;
    if (!SDL_GetDisplayUsableBounds(display_id, &bounds))
        return false;
    /* SDL_GetDisplayUsableBounds returns logical coordinates. Convert to
     * physical pixels using the window's display scale so comparisons
     * against physical pixel dimensions in main.c are correct. */
    float scale = 1.0f;
    if (d->window) {
        scale = SDL_GetWindowDisplayScale(d->window);
        if (scale <= 0.0f)
            scale = 1.0f;
    }
    if (w)
        *w = (int)((float)bounds.w * scale);
    if (h)
        *h = (int)((float)bounds.h * scale);
    return true;
}

// ── Clipboard ────────────────────────────────────────────────────────────

static char *sdl3_clipboard_get(PorttyBackend *self)
{
    (void)self;
    return SDL_GetClipboardText();
}

static bool sdl3_clipboard_set(PorttyBackend *self, const char *text)
{
    (void)self;
    char *copy = strdup(text ? text : "");
    if (!copy)
        return false;
    static const char *const mime_types[] = { "text/plain;charset=utf-8" };
    if (!SDL_SetClipboardData(clipboard_data_callback, clipboard_cleanup_callback,
                              copy, mime_types, 1)) {
        free(copy);
        return false;
    }
    return true;
}

static void sdl3_clipboard_free(PorttyBackend *self, char *text)
{
    (void)self;
    SDL_free(text);
}

static bool sdl3_clipboard_paste_async(PorttyBackend *self,
                                       TerminalBackend *term, PtyContext *pty)
{
    (void)self;
    (void)term;
    (void)pty;
    return false;
}

// ── PTY ──────────────────────────────────────────────────────────────────

static bool sdl3_register_pty(PorttyBackend *self, PtyContext *pty)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d || !pty)
        return false;
    d->pty = pty;
    return true;
}

static void sdl3_pause_pty(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;
    if (SDL_GetAtomicInt(&d->pty_paused))
        return;
    SDL_SetAtomicInt(&d->pty_paused, 1);
    vlog("PTY paused (backpressure)\n");
    sdl3_wakeup_reader(d);
}

static void sdl3_resume_pty(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;
    if (!SDL_GetAtomicInt(&d->pty_paused))
        return;
    SDL_SetAtomicInt(&d->pty_paused, 0);
    vlog("PTY resumed\n");
    sdl3_wakeup_reader(d);
}

// ── OS integration ───────────────────────────────────────────────────────

static void sdl3_set_cursor(PorttyBackend *self, PorttyCursor shape)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;
    PorttyCursor cursor = (shape == PORTTY_CURSOR_POINTER) ? PORTTY_CURSOR_POINTER
                                                           : PORTTY_CURSOR_TEXT;
    if (d->current_cursor == cursor &&
        ((cursor == PORTTY_CURSOR_TEXT && d->cursor_text) ||
         (cursor == PORTTY_CURSOR_POINTER && d->cursor_pointer)))
        return;

    SDL_Cursor *c = NULL;
    if (cursor == PORTTY_CURSOR_POINTER) {
        if (!d->cursor_pointer)
            d->cursor_pointer = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
        c = d->cursor_pointer;
    } else {
        if (!d->cursor_text)
            d->cursor_text = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
        c = d->cursor_text;
    }
    if (c)
        SDL_SetCursor(c);
    d->current_cursor = cursor;
}

static bool sdl3_open_url(PorttyBackend *self, const char *url,
                          char *err, size_t errlen)
{
    (void)self;
    return os_compat_open_url(url, err, errlen);
}

static void sdl3_set_autoscroll(PorttyBackend *self, bool enabled)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;
    if (enabled) {
        if (d->autoscroll_timer == TIMER_INVALID && d->timers) {
            d->autoscroll_timer = timer_add(d->timers, AUTOSCROLL_INTERVAL_MS,
                                            EVENT_AUTOSCROLL_TICK, NULL);
        }
    } else {
        if (d->autoscroll_timer != TIMER_INVALID) {
            timer_remove(d->timers, d->autoscroll_timer);
            d->autoscroll_timer = TIMER_INVALID;
        }
    }
}

static void sdl3_set_working_dir(PorttyBackend *self, const char *dir)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d || !dir)
        return;
    snprintf(d->working_dir, sizeof(d->working_dir), "%s", dir);
}

static const char *sdl3_get_exe_path(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    return (d && d->exe_path[0]) ? d->exe_path : NULL;
}

static bool sdl3_spawn_new_terminal(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d || !d->exe_path[0])
        return false;

    char cwd_path[PATH_MAX] = "";
    if (d->working_dir[0]) {
        snprintf(cwd_path, sizeof(cwd_path), "%s", d->working_dir);
    }
#ifndef _WIN32
    else if (d->pty) {
        pty_get_child_cwd(d->pty, cwd_path, sizeof(cwd_path));
    }
#endif

    return os_compat_spawn_process(d->exe_path, cwd_path);
}

static char *sdl3_get_default_font(PorttyBackend *self)
{
    (void)self;
#ifdef _WIN32
    HKEY hkey;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, L"Console", 0,
                            KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS)
        return NULL;

    WCHAR face_w[256];
    DWORD len = sizeof(face_w);
    DWORD type;
    rc = RegQueryValueExW(hkey, L"FaceName", NULL, &type,
                          (BYTE *)face_w, &len);
    RegCloseKey(hkey);

    if (rc != ERROR_SUCCESS || type != REG_SZ)
        return NULL;

    if (_wcsicmp(face_w, L"0") == 0 ||
        _wcsicmp(face_w, L"Terminal") == 0)
        return NULL;

    if (_wcsicmp(face_w, L"__DefaultTTFont__") == 0) {
        HKEY ttkey;
        rc = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
            L"Console\\TrueTypeFont",
            0, KEY_READ, &ttkey);
        if (rc != ERROR_SUCCESS)
            return NULL;
        len = sizeof(face_w);
        rc = RegQueryValueExW(ttkey, L"00", NULL, &type,
                              (BYTE *)face_w, &len);
        RegCloseKey(ttkey);
        if (rc != ERROR_SUCCESS || type != REG_SZ)
            return NULL;
    }

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, face_w, -1,
                                       NULL, 0, NULL, NULL);
    if (utf8_len <= 0)
        return NULL;
    char *buf = malloc(utf8_len);
    if (!buf)
        return NULL;
    WideCharToMultiByte(CP_UTF8, 0, face_w, -1, buf, utf8_len,
                        NULL, NULL);
    vlog("W32: system default console font: %s\n", buf);
    return buf;
#else
    return NULL;
#endif
}

// ── Notifications & link hints ────────────────────────────────────────────

static void sdl3_notify(PorttyBackend *self, const char *title,
                        const char *body, PorttyNotifyLevel level)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;
    rend_sdl3_set_notification(&d->rend, title, body, (int)level);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_USER;
    ev.user.code = EVENT_NOTIFY_SHOW;
    SDL_PushEvent(&ev);
}

static void sdl3_notify_dismiss(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;
    rend_sdl3_clear_notification(&d->rend);
    if (d->term)
        terminal_mark_dirty(d->term);
}

static void sdl3_set_link_hint(PorttyBackend *self, const char *url, int anchor_py)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;
    rend_sdl3_set_link_hint(&d->rend, url, anchor_py);
}

static int sdl3_notification_hit(PorttyBackend *self, int px, int py)
{
    return rend_sdl3_notification_hit(&sdl3_data(self)->rend, px, py);
}

static bool sdl3_set_notification_hover(PorttyBackend *self, bool hovered)
{
    return rend_sdl3_set_notification_hover(&sdl3_data(self)->rend, hovered);
}

// ── Rendering ────────────────────────────────────────────────────────────

static int sdl3_load_fonts(PorttyBackend *self, float size,
                           const char *name, int ft_hint_target)
{
    return rend_sdl3_load_fonts(&sdl3_data(self)->rend, size, name, ft_hint_target);
}

static void sdl3_draw_terminal(PorttyBackend *self, TerminalBackend *term,
                               bool cursor_visible)
{
    rend_sdl3_draw_terminal(&sdl3_data(self)->rend, term, cursor_visible);
}

static void sdl3_present(PorttyBackend *self)
{
    rend_sdl3_present(&sdl3_data(self)->rend);
}

static void sdl3_resize(PorttyBackend *self, int w, int h)
{
    rend_sdl3_resize(&sdl3_data(self)->rend, w, h);
}

static bool sdl3_get_cell_size(PorttyBackend *self, int *cw, int *ch)
{
    return rend_sdl3_get_cell_size(&sdl3_data(self)->rend, cw, ch);
}

static void sdl3_scroll(PorttyBackend *self, TerminalBackend *term, int delta)
{
    rend_sdl3_scroll(&sdl3_data(self)->rend, term, delta);
}

static void sdl3_reset_scroll(PorttyBackend *self)
{
    rend_sdl3_reset_scroll(&sdl3_data(self)->rend);
}

static int sdl3_get_scroll_offset(PorttyBackend *self)
{
    return rend_sdl3_get_scroll_offset(&sdl3_data(self)->rend);
}

static void sdl3_set_content_scale(PorttyBackend *self, float scale)
{
    rend_sdl3_set_content_scale(&sdl3_data(self)->rend, scale);
}

// ── Pager overlay ────────────────────────────────────────────────────────

static void sdl3_set_overlay(PorttyBackend *self, TerminalBackend *overlay)
{
    rend_sdl3_set_overlay(&sdl3_data(self)->rend, overlay);
}

static void sdl3_clear_overlay(PorttyBackend *self)
{
    rend_sdl3_clear_overlay(&sdl3_data(self)->rend);
}

static bool sdl3_has_overlay(PorttyBackend *self)
{
    return rend_sdl3_has_overlay(&sdl3_data(self)->rend);
}

// ── Offscreen rendering ──────────────────────────────────────────────────

static int sdl3_render_to_png(PorttyBackend *self, TerminalBackend *term,
                              const char *path)
{
    return rend_sdl3_render_to_png(&sdl3_data(self)->rend, term, path);
}

// ── Diagnostics ───────────────────────────────────────────────────────────

static void sdl3_log_stats(PorttyBackend *self)
{
    rend_sdl3_log_stats(&sdl3_data(self)->rend);
}

static bool sdl3_get_diag(PorttyBackend *self, PorttyDiag *out)
{
    if (!self || !self->data || !out)
        return false;
    Sdl3BackendData *d = sdl3_data(self);
    if (!rend_sdl3_get_diag(&d->rend, out))
        return false;
    out->platform_name = self->name;
    return true;
}

static void sdl3_request_quit(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;
    SDL_SetAtomicInt(&d->quit_requested, 1);
}

static void sdl3_debug_screendump(Sdl3BackendData *d, const char *path)
{
    int w, h;
    SDL_GetCurrentRenderOutputSize(d->sdl_renderer, &w, &h);
    SDL_Surface *surface = SDL_RenderReadPixels(d->sdl_renderer, NULL);
    if (!surface)
        return;
    SDL_Surface *rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (rgba) {
        png_write_rgba(path, rgba->pixels, rgba->w, rgba->h);
        SDL_DestroySurface(rgba);
    }
    SDL_DestroySurface(surface);
    vlog("screendump: saved %s (%dx%d)\n", path, w, h);
}

// ── Event loop ───────────────────────────────────────────────────────────

static void sdl3_debug_resize(void *user_data, int cols, int rows)
{
    Sdl3BackendData *d = (Sdl3BackendData *)user_data;
    if (!d || !d->app)
        return;
    int cell_w, cell_h;
    if (!d->app->backend->get_cell_size(d->app->backend, &cell_w, &cell_h))
        return;
    int pixel_w = cols * cell_w;
    int pixel_h = rows * cell_h;
    portty_app_handle_resize(d->app, pixel_w, pixel_h);
    fprintf(stderr, "resize: %d cols x %d rows\n", cols, rows);
}

static void sdl3_run(PorttyBackend *self)
{
    Sdl3BackendData *d = sdl3_data(self);
    if (!d)
        return;

    TerminalBackend *term = d->term;
    RendererSdl3Data *rend = &d->rend;

    // Start cursor blink timer
    d->cursor_blink_visible = true;
    d->has_focus = true;
    d->cursor_blink_timer = timer_add(d->timers, CURSOR_BLINK_INTERVAL_MS,
                                      EVENT_CURSOR_BLINK, NULL);

    // Start PTY reader thread
    SDL_SetAtomicInt(&d->running, 1);
    SDL_SetAtomicInt(&d->quit_requested, 0);
    if (d->pty) {
        d->pty_reader_thread = SDL_CreateThread(pty_reader_thread_func, "pty_reader", d);
        if (!d->pty_reader_thread) {
            fprintf(stderr, "ERROR: Failed to create PTY reader thread: %s\n", SDL_GetError());
            return;
        }
    }

    SDL_StartTextInput(d->window);

    // Load debug script if specified
    if (d->app->script_path) {
        d->debug_script = portty_debug_script_load(d->app->script_path);
        if (!d->debug_script) {
            fprintf(stderr, "ERROR: Failed to load debug script: %s: out of memory\n",
                    d->app->script_path);
            SDL_SetAtomicInt(&d->quit_requested, 1);
        } else {
            const char *err = portty_debug_script_error(d->debug_script);
            if (err) {
                fprintf(stderr, "ERROR: %s: %s\n", d->app->script_path, err);
                SDL_SetAtomicInt(&d->quit_requested, 1);
            }
        }
    }

    vlog("Event loop starting (event-driven)\n");
    terminal_mark_dirty(term);

    SDL_Event event;
    Uint64 last_tick = SDL_GetTicks();
    while (!SDL_GetAtomicInt(&d->quit_requested)) {
        // === Debug script: pre-render commands ===
        if (d->debug_script && !d->debug_script_done) {
            DebugExecCtx ctx = {
                .backend = self,
                .term = term,
                .pty = d->pty,
                .scroll_offset = rend_sdl3_get_scroll_offset(rend),
                .emit_fn = (void (*)(void *, const char *, size_t))portty_app_feed_terminal,
                .emit_user_data = d->app,
                .pending_screendump = &d->debug_pending_screendump,
                .screendump_path_buf = d->debug_screendump_path,
                .pending_verifybuf = NULL,
                .dumpverts_fn = NULL,
                .resize_fn = sdl3_debug_resize,
                .resize_user_data = d,
            };
            portty_debug_script_step(d->debug_script, &d->debug_cmd_index, &ctx);
            if (d->debug_cmd_index >= portty_debug_script_count(d->debug_script))
                d->debug_script_done = true;
        }

        if (!SDL_WaitEventTimeout(&event, 33)) {
            // Timeout — no SDL events, but timers may need to fire.
        }

        bool pty_processed = false;

        // Process all pending events
        do {
            switch (event.type) {
            case SDL_EVENT_USER:
                switch (event.user.code) {
                case EVENT_PTY_DATA:
                {
                    PtyDataPayload *payload = (PtyDataPayload *)event.user.data1;
                    if (payload) {
                        rend_sdl3_process_pty_data(rend, term, payload->data, payload->len);
                        self->set_window_title(self, terminal_get_title(term));
                        free(payload);
                        pty_processed = true;
                        if (d->lottie_timer == TIMER_INVALID &&
                            terminal_lottie_count(term) > 0) {
                            d->lottie_timer = timer_add(d->timers, 16,
                                                        EVENT_LOTTIE_TICK, NULL);
                        }
                    }
                    break;
                }
                case EVENT_PTY_CLOSED:
                    vlog("PTY closed event received\n");
                    SDL_SetAtomicInt(&d->quit_requested, 1);
                    break;

                case EVENT_PTY_CHILD_EXIT:
                    vlog("PTY child exit event received\n");
                    SDL_SetAtomicInt(&d->quit_requested, 1);
                    break;

                case EVENT_CURSOR_BLINK:
                    if (terminal_get_cursor_blink(term)) {
                        d->cursor_blink_visible = !d->cursor_blink_visible;
                        terminal_mark_dirty(term);
                    }
                    break;

                case EVENT_AUTOSCROLL_TICK:
                    portty_app_handle_autoscroll_tick(d->app);
                    terminal_mark_dirty(term);
                    break;

                case EVENT_LOTTIE_TICK:
                    if (terminal_lottie_tick(term, SDL_GetTicksNS() / 1000))
                        terminal_mark_dirty(term);
                    if (terminal_lottie_count(term) == 0) {
                        timer_remove(d->timers, d->lottie_timer);
                        d->lottie_timer = TIMER_INVALID;
                    }
                    break;

                case EVENT_NOTIFY_SHOW:
                    terminal_mark_dirty(term);
                    break;
                }
                break;

            case SDL_EVENT_QUIT:
                vlog("SDL quit event received\n");
                SDL_SetAtomicInt(&d->quit_requested, 1);
                break;

            case SDL_EVENT_KEY_DOWN:
            {
                int sdl_key = event.key.key;
                int sdl_mod = event.key.mod;
                int scancode = event.key.scancode;
                int tmod = sdl_mod_to_term(sdl_mod);
                KeyboardResult result = { 0 };

                int term_key = TERM_KEY_NONE;
                for (int i = 0; i < (int)(sizeof(key_map) / sizeof(key_map[0])); i++) {
                    if (key_map[i].sdl_key == sdl_key) {
                        term_key = key_map[i].term_key;
                        break;
                    }
                }

                if (term_key != TERM_KEY_NONE) {
                    result = portty_app_handle_key(d->app, term_key, tmod, 0);
                } else if ((sdl_mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) && scancode != 0) {
                    SDL_Keycode resolved = SDL_GetKeyFromScancode(scancode, sdl_mod & SDL_KMOD_SHIFT, false);
                    if (resolved >= 32 && resolved < 127) {
                        uint32_t cp = (uint32_t)resolved;
                        if (cp >= 'A' && cp <= 'Z' && !(sdl_mod & SDL_KMOD_SHIFT))
                            cp = cp - 'A' + 'a';
                        result = portty_app_handle_key(d->app, TERM_KEY_NONE, tmod, cp);
                    }
                }

                if (result.request_quit) {
                    SDL_SetAtomicInt(&d->quit_requested, 1);
                } else if (result.force_redraw) {
                    terminal_mark_dirty(term);
                } else if (result.handled || (result.len > 0)) {
                    if (rend_sdl3_get_scroll_offset(rend) != 0) {
                        rend_sdl3_reset_scroll(rend);
                        terminal_mark_dirty(term);
                    }
                    d->cursor_blink_visible = true;
                    timer_reset(d->timers, d->cursor_blink_timer);
                    terminal_mark_dirty(term);
                    if (result.len > 0 && !result.handled && d->pty) {
                        ssize_t written = pty_write(d->pty, result.data, result.len);
                        if (written < 0)
                            vlog("PTY write failed: %s\n", strerror(errno));
                    }
                }
                break;
            }

            case SDL_EVENT_TEXT_INPUT:
                if (!(SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
                    KeyboardResult result = portty_app_handle_text(
                        d->app, event.text.text);

                    if (result.handled || result.force_redraw || result.len > 0) {
                        if (rend_sdl3_get_scroll_offset(rend) != 0) {
                            rend_sdl3_reset_scroll(rend);
                            terminal_mark_dirty(term);
                        }
                        d->cursor_blink_visible = true;
                        timer_reset(d->timers, d->cursor_blink_timer);
                        terminal_mark_dirty(term);
                        if (result.len > 0 && !result.handled && d->pty)
                            pty_write(d->pty, result.data, result.len);
                    }
                }
                break;

            case SDL_EVENT_WINDOW_RESIZED:
                // With SDL_WINDOW_HIGH_PIXEL_DENSITY, event dimensions are in
                // logical points. Convert to physical pixels for the renderer.
                {
                    int pix_w, pix_h;
                    SDL_GetWindowSizeInPixels(d->window, &pix_w, &pix_h);
                    portty_app_handle_resize(d->app, pix_w, pix_h);
                }
                terminal_mark_dirty(term);
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                vlog("Window close requested\n");
                SDL_SetAtomicInt(&d->quit_requested, 1);
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                d->has_focus = (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
                if (!d->has_focus)
                    rend_sdl3_set_link_hint(rend, NULL, 0);
                terminal_mark_dirty(term);
                break;

            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                rend_sdl3_set_link_hint(rend, NULL, 0);
                if (term)
                    terminal_set_hovered_hyperlink(term, 0);
                if (d->left_button_up_buffered &&
                    SDL_GetTicks() - d->left_button_up_tick < 100) {
                    d->left_button_up_buffered = false;
                    d->left_button_down = true;
                }
                {
                    float mx, my;
                    SDL_GetMouseState(&mx, &my);
                    portty_app_handle_mouse_leave(d->app, (int)mx, (int)my);
                }
                terminal_mark_dirty(term);
                break;

            case SDL_EVENT_WINDOW_MOUSE_ENTER:
                portty_app_handle_mouse_enter(d->app);
                break;

            case SDL_EVENT_MOUSE_WHEEL:
            {
                float dy = event.wheel.y;
                if (dy != 0.0f) {
                    d->wheel_accum_y += dy;
                    int whole_ticks = (int)d->wheel_accum_y;
                    if (whole_ticks != 0) {
                        d->wheel_accum_y -= (float)whole_ticks;
                        bool consumed = false;
                        int button = (whole_ticks > 0) ? 4 : 5;
                        int clicks = abs(whole_ticks);
                        int tmod = sdl_mod_to_term(SDL_GetModState());
                        // Scale from logical points to physical pixels
                        float scale = d->rend.content_scale;
                        if (scale <= 0.0f)
                            scale = 1.0f;
                        int mx = (int)(event.wheel.mouse_x * scale);
                        int my = (int)(event.wheel.mouse_y * scale);
                        for (int i = 0; i < clicks && !consumed; i++) {
                            consumed = portty_app_handle_mouse(
                                d->app, mx, my, button, true, 0, tmod);
                        }
                        if (!consumed)
                            portty_app_handle_scroll(d->app, whole_ticks);
                    }
                }
                terminal_mark_dirty(term);
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                bool pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                int button = event.button.button;
                int clicks = pressed ? event.button.clicks : 0;
                int tmod = sdl_mod_to_term(SDL_GetModState());

                // Scale from logical points to physical pixels
                float scale = d->rend.content_scale;
                if (scale <= 0.0f)
                    scale = 1.0f;
                int px = (int)(event.button.x * scale);
                int py = (int)(event.button.y * scale);

                // Flush any buffered button-up from a previous event
                if (d->left_button_up_buffered) {
                    d->left_button_up_buffered = false;
                    d->left_button_down = false;
                    SDL_CaptureMouse(false);
                    if (portty_app_handle_mouse(d->app,
                                                d->left_button_up_x,
                                                d->left_button_up_y, 1, false,
                                                0, tmod)) {
                        terminal_mark_dirty(term);
                    }
                }
                if (button == 1) {
                    if (pressed) {
                        d->left_button_down = true;
                        SDL_CaptureMouse(true);
                        if (portty_app_handle_mouse(d->app,
                                                    px, py, button,
                                                    pressed, clicks, tmod)) {
                            terminal_mark_dirty(term);
                        }
                    } else {
                        d->left_button_up_buffered = true;
                        d->left_button_up_x = px;
                        d->left_button_up_y = py;
                        d->left_button_up_tick = SDL_GetTicks();
                        d->left_button_down = false;
                    }
                } else {
                    if (portty_app_handle_mouse(d->app,
                                                px, py, button,
                                                pressed, clicks, tmod)) {
                        terminal_mark_dirty(term);
                    }
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
            {
                // Flush a buffered button-up if present
                if (d->left_button_up_buffered) {
                    d->left_button_up_buffered = false;
                    d->left_button_down = false;
                    SDL_CaptureMouse(false);
                    int tmod = sdl_mod_to_term(SDL_GetModState());
                    if (portty_app_handle_mouse(d->app,
                                                d->left_button_up_x,
                                                d->left_button_up_y, 1, false,
                                                0, tmod)) {
                        terminal_mark_dirty(term);
                    }
                }
                bool any_button_pressed = (event.motion.state != 0);
                int tmod = sdl_mod_to_term(SDL_GetModState());
                if (d->left_button_down)
                    any_button_pressed = true;
                // Scale from logical points to physical pixels
                float scale = d->rend.content_scale;
                if (scale <= 0.0f)
                    scale = 1.0f;
                int mx = (int)(event.motion.x * scale);
                int my = (int)(event.motion.y * scale);
                if (portty_app_handle_mouse(d->app, mx, my,
                                            0, any_button_pressed,
                                            0, tmod)) {
                    terminal_mark_dirty(term);
                }
                break;
            }

            default:
                break;
            }
        } while (SDL_PollEvent(&event));

        // Poll timers
        Uint64 now = SDL_GetTicks();
        uint32_t elapsed = (uint32_t)(now - last_tick);
        last_tick = now;
        if (elapsed > 0) {
            TimerEvent tevents[8];
            size_t n = timer_poll(d->timers, elapsed, tevents, 8);
            for (size_t i = 0; i < n; i++) {
                switch (tevents[i].code) {
                case EVENT_CURSOR_BLINK:
                    if (terminal_get_cursor_blink(term)) {
                        d->cursor_blink_visible = !d->cursor_blink_visible;
                        terminal_mark_dirty(term);
                    }
                    break;
                case EVENT_AUTOSCROLL_TICK:
                    portty_app_handle_autoscroll_tick(d->app);
                    terminal_mark_dirty(term);
                    break;
                case EVENT_LOTTIE_TICK:
                    if (terminal_lottie_tick(term, SDL_GetTicksNS() / 1000))
                        terminal_mark_dirty(term);
                    if (terminal_lottie_count(term) == 0) {
                        timer_remove(d->timers, d->lottie_timer);
                        d->lottie_timer = TIMER_INVALID;
                    }
                    break;
                default:
                    break;
                }
            }
        }

        // Flush buffered button-up if no more events pending
        if (d->left_button_up_buffered) {
            d->left_button_up_buffered = false;
            d->left_button_down = false;
            SDL_CaptureMouse(false);
            int tmod = sdl_mod_to_term(SDL_GetModState());
            portty_app_handle_mouse(d->app,
                                    d->left_button_up_x,
                                    d->left_button_up_y, 1, false,
                                    0, tmod);
            terminal_mark_dirty(term);
        }

        // Age and prune deferred-clipboard entries
        clipboard_deferred_free_advance();

        // Re-resolve OSC-8 hover after PTY output
        if (pty_processed) {
            int px = -1, py = -1;
            if (SDL_GetMouseFocus() == d->window) {
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                px = (int)mx;
                py = (int)my;
            }
            if (portty_app_revalidate_hover(d->app, px, py))
                terminal_mark_dirty(term);
        }

        // Drain VT damage and render
        terminal_flush_damage(term);
        if (terminal_needs_redraw(term)) {
            bool cursor_vis = !d->has_focus || !terminal_get_cursor_blink(term) || d->cursor_blink_visible;
            rend_sdl3_draw_terminal(rend, term, cursor_vis);
            SDL_RenderPresent(d->sdl_renderer);
            terminal_clear_redraw(term);
        }

        // === Debug script: post-render screendump ===
        if (d->debug_pending_screendump) {
            d->debug_pending_screendump = false;
            sdl3_debug_screendump(d, d->debug_screendump_path);
        }
    }

    vlog("Event loop exiting\n");

    SDL_StopTextInput(d->window);

    // Free debug script
    portty_debug_script_free(d->debug_script);
    d->debug_script = NULL;

    if (d->cursor_blink_timer != TIMER_INVALID) {
        timer_remove(d->timers, d->cursor_blink_timer);
        d->cursor_blink_timer = TIMER_INVALID;
    }
    if (d->lottie_timer != TIMER_INVALID) {
        timer_remove(d->timers, d->lottie_timer);
        d->lottie_timer = TIMER_INVALID;
    }
    if (d->autoscroll_timer != TIMER_INVALID) {
        timer_remove(d->timers, d->autoscroll_timer);
        d->autoscroll_timer = TIMER_INVALID;
    }

    // Stop reader thread
    SDL_SetAtomicInt(&d->running, 0);
    if (d->pty_reader_thread) {
#ifdef _WIN32
        if (d->pty)
            pty_close_console(d->pty);
#endif
        sdl3_wakeup_reader(d);
        SDL_WaitThread(d->pty_reader_thread, NULL);
        d->pty_reader_thread = NULL;
    }
}

// ── Backend definition ───────────────────────────────────────────────────

PorttyBackend backend_sdl3 = {
    .name = "sdl3",
    .data = NULL,
    .init = sdl3_init,
    .run = sdl3_run,
    .destroy = sdl3_destroy,
    .request_quit = sdl3_request_quit,

    .clipboard_get = sdl3_clipboard_get,
    .clipboard_set = sdl3_clipboard_set,
    .clipboard_free = sdl3_clipboard_free,
    .clipboard_paste_async = sdl3_clipboard_paste_async,

    .register_pty = sdl3_register_pty,
    .pause_pty = sdl3_pause_pty,
    .resume_pty = sdl3_resume_pty,

    .set_window_title = sdl3_set_window_title,
    .set_window_size = sdl3_set_window_size,
    .show_window = sdl3_show_window,
    .get_drawable_size = sdl3_get_drawable_size,
    .get_display_scale = sdl3_get_display_scale,
    .get_display_size = sdl3_get_display_size,

    .set_cursor = sdl3_set_cursor,
    .open_url = sdl3_open_url,
    .set_autoscroll = sdl3_set_autoscroll,
    .spawn_new_terminal = sdl3_spawn_new_terminal,
    .set_working_dir = sdl3_set_working_dir,
    .get_exe_path = sdl3_get_exe_path,
    .get_default_font = sdl3_get_default_font,

    .notify = sdl3_notify,
    .notify_dismiss = sdl3_notify_dismiss,
    .set_link_hint = sdl3_set_link_hint,
    .notification_hit = sdl3_notification_hit,
    .set_notification_hover = sdl3_set_notification_hover,

    .load_fonts = sdl3_load_fonts,
    .draw_terminal = sdl3_draw_terminal,
    .present = sdl3_present,
    .resize = sdl3_resize,
    .get_cell_size = sdl3_get_cell_size,
    .scroll = sdl3_scroll,
    .reset_scroll = sdl3_reset_scroll,
    .get_scroll_offset = sdl3_get_scroll_offset,
    .set_content_scale = sdl3_set_content_scale,

    .set_overlay = sdl3_set_overlay,
    .clear_overlay = sdl3_clear_overlay,
    .has_overlay = sdl3_has_overlay,

    .render_to_png = sdl3_render_to_png,

    .log_stats = sdl3_log_stats,
    .get_diag = sdl3_get_diag,
};
