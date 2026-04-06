#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "platform_sdl3.h"
#include "png_reader.h"
#include "timer.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <limits.h>
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

// Custom event codes for SDL_EVENT_USER
enum BloomEventCode
{
    EVENT_PTY_DATA = 1,
    EVENT_PTY_CLOSED,
    EVENT_PTY_CHILD_EXIT,
    EVENT_CURSOR_BLINK,
};

// PTY data event payload
typedef struct
{
    size_t len;
    char data[];
} PtyDataPayload;

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

// Backend-specific context (merged from SDL3EventLoopContext + window state)
typedef struct
{
    SDL_Window *window;
    SDL_Renderer *sdl_renderer;
    PtyContext *pty;
    SDL_Thread *pty_reader_thread;
    SDL_AtomicInt running;
    SDL_AtomicInt quit_requested;
    SDL_AtomicInt pty_paused;
    bool force_redraw;

    // Wakeup mechanism to interrupt reader thread on shutdown/pause
#ifdef _WIN32
    HANDLE wakeup_event;
#else
    int wakeup_pipe[2];
#endif

    // Timer system
    TimerManager *timers;
    TimerId cursor_blink_timer;
    bool cursor_blink_visible;
    bool has_focus;
} SDL3PlatformData;

// Convert SDL modifier flags to TERM_MOD_* flags
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

// Find the icon file by probing several paths relative to the executable
static const char *find_icon_path(void)
{
    static char path[PATH_MAX];
    const char *base = SDL_GetBasePath();
    const char *icon_rel = "icons/hicolor/256x256/apps/bloom-terminal.png";

    struct
    {
        const char *fmt;
    } probes[] = {
        /* Dev build: exe is build/src/bloom-terminal, data is at project root */
        { "%s../../data/%s" },
        /* Installed: exe is $PREFIX/bin/, data is $PREFIX/share/ */
        { "%s../share/%s" },
    };

    if (base) {
        for (int i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
            snprintf(path, sizeof(path), probes[i].fmt, base, icon_rel);
            if (access(path, R_OK) == 0) {
                vlog("Found icon at %s\n", path);
                return path;
            }
        }
    }

#ifdef BLOOM_DATADIR
    /* Autotools compile-time datadir */
    snprintf(path, sizeof(path), "%s/%s", BLOOM_DATADIR, icon_rel);
    if (access(path, R_OK) == 0) {
        vlog("Found icon at %s (BLOOM_DATADIR)\n", path);
        return path;
    }
#endif

    /* CWD fallback */
    snprintf(path, sizeof(path), "data/%s", icon_rel);
    if (access(path, R_OK) == 0) {
        vlog("Found icon at %s (CWD)\n", path);
        return path;
    }

    return NULL;
}

// Load and set the window icon from a PNG file
static void set_window_icon(SDL_Window *win)
{
    const char *icon_path = find_icon_path();
    if (!icon_path) {
        vlog("No icon file found, skipping window icon\n");
        return;
    }

    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    if (png_read_rgba(icon_path, &pixels, &w, &h) != 0) {
        fprintf(stderr, "WARNING: Failed to read icon %s\n", icon_path);
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
        /* Expected on Wayland — icon comes from .desktop + hicolor theme */
        vlog("SDL_SetWindowIcon skipped: %s\n", SDL_GetError());
    } else {
        vlog("Window icon set from %s (%dx%d)\n", icon_path, w, h);
    }

    SDL_DestroySurface(surface);
    free(pixels);
}

// Forward declarations
static bool sdl3_plat_init(PlatformBackend *plat);
static void sdl3_plat_destroy(PlatformBackend *plat);
static bool sdl3_create_window(PlatformBackend *plat, const char *title,
                               int width, int height);
static void sdl3_show_window(PlatformBackend *plat);
static void sdl3_set_window_size(PlatformBackend *plat, int width, int height);
static void sdl3_set_window_title(PlatformBackend *plat, const char *title);
static void *sdl3_get_sdl_renderer(PlatformBackend *plat);
static void *sdl3_get_sdl_window(PlatformBackend *plat);
static char *sdl3_clipboard_get(PlatformBackend *plat);
static bool sdl3_clipboard_set(PlatformBackend *plat, const char *text);
static void sdl3_clipboard_free(PlatformBackend *plat, char *text);
static bool sdl3_register_pty(PlatformBackend *plat, PtyContext *pty);
static void sdl3_run(PlatformBackend *plat, TerminalBackend *term,
                     RendererBackend *rend, PlatformCallbacks *callbacks);
static void sdl3_request_quit(PlatformBackend *plat);
static void sdl3_pause_pty(PlatformBackend *plat);
static void sdl3_resume_pty(PlatformBackend *plat);
static float sdl3_get_display_scale(PlatformBackend *plat);
static bool sdl3_get_display_size(PlatformBackend *plat, int *width, int *height);

// Backend definition
PlatformBackend platform_backend_sdl3 = {
    .name = "sdl3",
    .backend_data = NULL,
    .init = sdl3_plat_init,
    .destroy = sdl3_plat_destroy,
    .create_window = sdl3_create_window,
    .show_window = sdl3_show_window,
    .set_window_size = sdl3_set_window_size,
    .set_window_title = sdl3_set_window_title,
    .get_sdl_renderer = sdl3_get_sdl_renderer,
    .get_sdl_window = sdl3_get_sdl_window,
    .clipboard_get = sdl3_clipboard_get,
    .clipboard_set = sdl3_clipboard_set,
    .clipboard_free = sdl3_clipboard_free,
    .register_pty = sdl3_register_pty,
    .run = sdl3_run,
    .request_quit = sdl3_request_quit,
    .pause_pty = sdl3_pause_pty,
    .resume_pty = sdl3_resume_pty,
    .get_display_scale = sdl3_get_display_scale,
    .get_display_size = sdl3_get_display_size,
};

// PTY reader thread function
#ifdef _WIN32
static int pty_reader_thread_func(void *data)
{
    SDL3PlatformData *ctx = (SDL3PlatformData *)data;
    char buf[4096];

    vlog("PTY reader thread started (Win32)\n");

    HANDLE hProcess = (HANDLE)pty_get_process_handle(ctx->pty);

    while (SDL_GetAtomicInt(&ctx->running)) {
        if (SDL_GetAtomicInt(&ctx->pty_paused)) {
            // When paused, wait for wakeup or child exit only
            HANDLE wait_h[2] = { ctx->wakeup_event, hProcess };
            DWORD wr = WaitForMultipleObjects(2, wait_h, FALSE, INFINITE);
            ResetEvent(ctx->wakeup_event);

            if (wr == WAIT_OBJECT_0) {
                // Wakeup — check running/paused and re-loop
                if (!SDL_GetAtomicInt(&ctx->running))
                    break;
                continue;
            }
            if (wr == WAIT_OBJECT_0 + 1) {
                // Child exited
                vlog("PTY reader thread: child process exited\n");
                SDL_Event ev = { 0 };
                ev.type = SDL_EVENT_USER;
                ev.user.code = EVENT_PTY_CHILD_EXIT;
                SDL_PushEvent(&ev);
                break;
            }
            break; // error
        }

        // ReadFile on the ConPTY output pipe blocks until data arrives.
        ssize_t n = pty_read(ctx->pty, buf, sizeof(buf));
        if (n > 0) {
            PtyDataPayload *payload =
                malloc(sizeof(PtyDataPayload) + n);
            if (payload) {
                payload->len = n;
                memcpy(payload->data, buf, n);

                SDL_Event ev = { 0 };
                ev.type = SDL_EVENT_USER;
                ev.user.code = EVENT_PTY_DATA;
                ev.user.data1 = payload;

                if (!SDL_PushEvent(&ev)) {
                    vlog("PTY reader thread: failed to push event: "
                         "%s\n",
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

        // Check child exit after read
        if (!pty_is_running(ctx->pty)) {
            vlog("PTY reader thread: child exited after read\n");
            SDL_Event ev = { 0 };
            ev.type = SDL_EVENT_USER;
            ev.user.code = EVENT_PTY_CHILD_EXIT;
            SDL_PushEvent(&ev);
            break;
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
#else  /* POSIX */
static int pty_reader_thread_func(void *data)
{
    SDL3PlatformData *ctx = (SDL3PlatformData *)data;
    char buf[4096];

    vlog("PTY reader thread started\n");

    int pty_fd = pty_get_master_fd(ctx->pty);
    int signal_fd = pty_signal_get_fd();
    int wakeup_fd = ctx->wakeup_pipe[0];

    while (SDL_GetAtomicInt(&ctx->running)) {
        bool paused = SDL_GetAtomicInt(&ctx->pty_paused) != 0;

        struct pollfd pfds[3];
        int nfds = 0;
        int pty_idx = -1;
        int signal_idx = -1;
        int wakeup_idx = -1;

        // Only poll PTY fd when not paused
        if (!paused) {
            pty_idx = nfds;
            pfds[nfds].fd = pty_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        // Poll signal pipe if available
        if (signal_fd >= 0) {
            signal_idx = nfds;
            pfds[nfds].fd = signal_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }

        // Poll wakeup pipe for shutdown/pause notifications
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

        // Check wakeup pipe (shutdown or pause/resume notification)
        if (wakeup_idx >= 0 && (pfds[wakeup_idx].revents & POLLIN)) {
            char tmp;
            while (read(wakeup_fd, &tmp, 1) > 0)
                ; // drain
            if (!SDL_GetAtomicInt(&ctx->running)) {
                vlog("PTY reader thread: wakeup received, shutting down\n");
                break;
            }
            // Otherwise it was a pause/unpause wakeup — re-loop
            continue;
        }

        // Check signal pipe (child exit)
        if (signal_idx >= 0 && (pfds[signal_idx].revents & POLLIN)) {
            pty_signal_drain();
            vlog("PTY reader thread: SIGCHLD received\n");

            // Check if our specific child actually exited
            if (!pty_is_running(ctx->pty)) {
                vlog("PTY reader thread: child process has exited\n");
                SDL_Event event = { 0 };
                event.type = SDL_EVENT_USER;
                event.user.code = EVENT_PTY_CHILD_EXIT;
                SDL_PushEvent(&event);
                break;
            }
            vlog("PTY reader thread: SIGCHLD was not for our child, continuing\n");
        }

        // Check for PTY errors
        if (pty_idx >= 0 && (pfds[pty_idx].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            vlog("PTY reader thread: poll error condition (revents=0x%x)\n", pfds[pty_idx].revents);
            break;
        }

        // Read PTY data
        if (pty_idx >= 0 && (pfds[pty_idx].revents & POLLIN)) {
            ssize_t n = pty_read(ctx->pty, buf, sizeof(buf));
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

    // Push PTY_CLOSED event
    SDL_Event event = { 0 };
    event.type = SDL_EVENT_USER;
    event.user.code = EVENT_PTY_CLOSED;
    SDL_PushEvent(&event);

    vlog("PTY reader thread exiting\n");
    return 0;
}
#endif /* _WIN32 */

static bool sdl3_plat_init(PlatformBackend *plat)
{
    // Set app metadata before SDL initialization as recommended by SDL3
    if (verbose) {
        fprintf(stderr, "DEBUG: Setting SDL app metadata\n");
    }
    if (!SDL_SetAppMetadata("bloom-terminal", "1.0.0", "bloom-terminal")) {
        fprintf(stderr, "WARNING: Failed to set SDL app metadata: %s\n", SDL_GetError());
    }

    // Print SDL version info if verbose
    if (verbose) {
        int sdl_version = SDL_GetVersion();
        fprintf(stderr, "DEBUG: SDL version %d.%d.%d\n",
                SDL_VERSIONNUM_MAJOR(sdl_version),
                SDL_VERSIONNUM_MINOR(sdl_version),
                SDL_VERSIONNUM_MICRO(sdl_version));
    }

    // Initialize SDL with verbose logging
    if (verbose) {
        fprintf(stderr, "DEBUG: Initializing SDL video subsystem\n");
        fprintf(stderr, "DEBUG: DISPLAY=%s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(not set)");
        fprintf(stderr, "DEBUG: WAYLAND_DISPLAY=%s\n", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(not set)");
    }

    SDL_ClearError();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        const char *error = SDL_GetError();
        fprintf(stderr, "ERROR: Failed to initialize SDL video subsystem\n");

        if (error && error[0] != '\0') {
            fprintf(stderr, "ERROR: SDL_GetError() returned: '%s'\n", error);
        } else {
            fprintf(stderr, "ERROR: No specific error message from SDL\n");
        }

        fprintf(stderr, "ERROR: This could be due to:\n");
        fprintf(stderr, "ERROR: 1. Missing SDL3 runtime libraries\n");
        fprintf(stderr, "ERROR: 2. No display available (DISPLAY environment variable)\n");
        fprintf(stderr, "ERROR: 3. SDL3 driver issues\n");

        return false;
    }

    if (verbose) {
        fprintf(stderr, "DEBUG: SDL initialized successfully\n");
    }

    // Allocate context
    SDL3PlatformData *ctx = calloc(1, sizeof(SDL3PlatformData));
    if (!ctx) {
        fprintf(stderr, "ERROR: Failed to allocate platform context\n");
        SDL_Quit();
        return false;
    }

    ctx->window = NULL;
    ctx->sdl_renderer = NULL;
    ctx->pty = NULL;
    ctx->pty_reader_thread = NULL;
    SDL_SetAtomicInt(&ctx->running, 0);
    SDL_SetAtomicInt(&ctx->quit_requested, 0);
    SDL_SetAtomicInt(&ctx->pty_paused, 0);
    ctx->force_redraw = true; // Force initial render

    // Create wakeup mechanism for reader thread shutdown/pause
#ifdef _WIN32
    ctx->wakeup_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ctx->wakeup_event) {
        fprintf(stderr, "ERROR: Failed to create wakeup event: %lu\n",
                GetLastError());
        free(ctx);
        SDL_Quit();
        return false;
    }
#else
    ctx->wakeup_pipe[0] = -1;
    ctx->wakeup_pipe[1] = -1;
    if (pipe(ctx->wakeup_pipe) < 0) {
        fprintf(stderr, "ERROR: Failed to create wakeup pipe: %s\n", strerror(errno));
        free(ctx);
        SDL_Quit();
        return false;
    }
    fcntl(ctx->wakeup_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(ctx->wakeup_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(ctx->wakeup_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(ctx->wakeup_pipe[1], F_SETFD, FD_CLOEXEC);
#endif

    // Initialize timer system
    ctx->timers = timer_manager_create();
    if (!ctx->timers) {
        fprintf(stderr, "ERROR: Failed to create timer manager\n");
#ifdef _WIN32
        CloseHandle(ctx->wakeup_event);
#else
        close(ctx->wakeup_pipe[0]);
        close(ctx->wakeup_pipe[1]);
#endif
        free(ctx);
        SDL_Quit();
        return false;
    }
    ctx->cursor_blink_timer = TIMER_INVALID;
    ctx->cursor_blink_visible = true;

    plat->backend_data = ctx;
    return true;
}

static void sdl3_plat_destroy(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;

    // Stop reader thread if running
    if (ctx->pty_reader_thread) {
        SDL_SetAtomicInt(&ctx->running, 0);
#ifdef _WIN32
        // Close pseudo-console to unblock ReadFile in the reader thread
        if (ctx->pty)
            pty_close_console(ctx->pty);
        SetEvent(ctx->wakeup_event);
#else
        if (ctx->wakeup_pipe[1] >= 0) {
            char c = 1;
            (void)write(ctx->wakeup_pipe[1], &c, 1);
        }
#endif
        SDL_WaitThread(ctx->pty_reader_thread, NULL);
        ctx->pty_reader_thread = NULL;
    }

    // Destroy timer manager
    if (ctx->timers) {
        timer_manager_destroy(ctx->timers);
        ctx->timers = NULL;
    }

    // Close wakeup mechanism
#ifdef _WIN32
    if (ctx->wakeup_event) {
        CloseHandle(ctx->wakeup_event);
        ctx->wakeup_event = NULL;
    }
#else
    if (ctx->wakeup_pipe[0] >= 0) {
        close(ctx->wakeup_pipe[0]);
        ctx->wakeup_pipe[0] = -1;
    }
    if (ctx->wakeup_pipe[1] >= 0) {
        close(ctx->wakeup_pipe[1]);
        ctx->wakeup_pipe[1] = -1;
    }
#endif

    // Destroy SDL resources
    if (ctx->sdl_renderer) {
        SDL_DestroyRenderer(ctx->sdl_renderer);
        ctx->sdl_renderer = NULL;
    }
    if (ctx->window) {
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
    }

    free(ctx);
    plat->backend_data = NULL;

    SDL_Quit();
}

static bool sdl3_create_window(PlatformBackend *plat, const char *title,
                               int width, int height)
{
    if (!plat || !plat->backend_data)
        return false;

    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;

    vlog("Creating window (placeholder size, will resize after font load)\n");
    SDL_ClearError();

    Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;
    ctx->window = SDL_CreateWindow(title, width, height, window_flags);
    if (!ctx->window) {
        const char *error = SDL_GetError();
        if (error && error[0] != '\0') {
            fprintf(stderr, "ERROR: Failed to create window: %s\n", error);
        } else {
            fprintf(stderr, "ERROR: Failed to create window (no specific error message)\n");
        }
        return false;
    }
    vlog("Window created successfully\n");

    // Set window icon (non-fatal if missing)
    set_window_icon(ctx->window);

#ifdef _WIN32
    // Apply Windows 11 DWM attributes for native dark mode and Mica
    {
        HWND hwnd = (HWND)SDL_GetPointerProperty(
            SDL_GetWindowProperties(ctx->window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd) {
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                  &dark, sizeof(dark));

            /* Mica backdrop (Windows 11 22H2+, no-op on older) */
            int backdrop = 2; /* DWMSBT_MAINWINDOW */
            DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                  &backdrop, sizeof(backdrop));

            /* Caption color: dark gray matching terminal background */
            COLORREF caption = 0x00282828; /* BGR */
            DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR,
                                  &caption, sizeof(caption));

            /* Rounded corners (Windows 11) */
            DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
            DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                                  &corner, sizeof(corner));

            vlog("DWM: dark mode + Mica + rounded corners applied\n");
        }
    }
#endif

    // Create renderer
    vlog("Creating renderer\n");
    SDL_ClearError();

    ctx->sdl_renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->sdl_renderer) {
        const char *error = SDL_GetError();
        if (error && error[0] != '\0') {
            fprintf(stderr, "ERROR: Failed to create renderer: %s\n", error);
        } else {
            fprintf(stderr, "ERROR: Failed to create renderer (no specific error message)\n");
        }
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
        return false;
    }
    vlog("Renderer created successfully\n");

    // Disable VSync for lowest input latency
    SDL_SetRenderVSync(ctx->sdl_renderer, 0);

    return true;
}

static void sdl3_show_window(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    if (ctx->window)
        SDL_ShowWindow(ctx->window);
}

static void sdl3_set_window_size(PlatformBackend *plat, int width, int height)
{
    if (!plat || !plat->backend_data)
        return;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    if (ctx->window)
        SDL_SetWindowSize(ctx->window, width, height);
}

static void sdl3_set_window_title(PlatformBackend *plat, const char *title)
{
    if (!plat || !plat->backend_data)
        return;

    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    if (ctx->window) {
        SDL_SetWindowTitle(ctx->window, title ? title : "bloom-terminal");
        vlog("Window title set to: %s\n", title ? title : "(default)");
    }
}

static void *sdl3_get_sdl_renderer(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return NULL;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    return ctx->sdl_renderer;
}

static void *sdl3_get_sdl_window(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return NULL;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    return ctx->window;
}

static char *sdl3_clipboard_get(PlatformBackend *plat)
{
    (void)plat;
    return SDL_GetClipboardText();
}

static bool sdl3_clipboard_set(PlatformBackend *plat, const char *text)
{
    (void)plat;
    return SDL_SetClipboardText(text);
}

static void sdl3_clipboard_free(PlatformBackend *plat, char *text)
{
    (void)plat;
    SDL_free(text);
}

static bool sdl3_register_pty(PlatformBackend *plat, PtyContext *pty)
{
    if (!plat || !plat->backend_data || !pty)
        return false;

    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    ctx->pty = pty;
    return true;
}

static void sdl3_run(PlatformBackend *plat, TerminalBackend *term,
                     RendererBackend *rend, PlatformCallbacks *callbacks)
{
    if (!plat || !plat->backend_data)
        return;

    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;

    // Start cursor blink timer
    ctx->cursor_blink_visible = true;
    ctx->has_focus = true;
    ctx->cursor_blink_timer = timer_add(ctx->timers, CURSOR_BLINK_INTERVAL_MS, true,
                                        EVENT_CURSOR_BLINK, NULL);

    // Start PTY reader thread (skip in demo mode when no PTY)
    SDL_SetAtomicInt(&ctx->running, 1);
    SDL_SetAtomicInt(&ctx->quit_requested, 0);
    if (ctx->pty) {
        ctx->pty_reader_thread = SDL_CreateThread(pty_reader_thread_func, "pty_reader", ctx);
        if (!ctx->pty_reader_thread) {
            fprintf(stderr, "ERROR: Failed to create PTY reader thread: %s\n", SDL_GetError());
            return;
        }
    }

    // Enable text input for proper Unicode character handling
    SDL_StartTextInput(ctx->window);

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
                    PtyDataPayload *payload = (PtyDataPayload *)event.user.data1;
                    if (payload) {
                        renderer_process_pty_data(rend, term, payload->data, payload->len);
                        platform_set_window_title(plat, terminal_get_title(term));
                        free(payload);
                    }
                    break;
                }
                case EVENT_PTY_CLOSED:
                    vlog("PTY closed event received\n");
                    SDL_SetAtomicInt(&ctx->quit_requested, 1);
                    break;

                case EVENT_PTY_CHILD_EXIT:
                    vlog("PTY child exit event received\n");
                    SDL_SetAtomicInt(&ctx->quit_requested, 1);
                    break;

                case EVENT_CURSOR_BLINK:
                    if (terminal_get_cursor_blink(term)) {
                        ctx->cursor_blink_visible = !ctx->cursor_blink_visible;
                        ctx->force_redraw = true;
                    }
                    break;
                }
            } else if (event.type == SDL_EVENT_QUIT) {
                vlog("SDL quit event received\n");
                SDL_SetAtomicInt(&ctx->quit_requested, 1);
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (callbacks) {
                    int sdl_key = event.key.key;
                    int sdl_mod = event.key.mod;
                    int scancode = event.key.scancode;
                    int tmod = sdl_mod_to_term(sdl_mod);
                    KeyboardResult result = { 0 };

                    // Look up key_map[] for special keys
                    int term_key = TERM_KEY_NONE;
                    for (int i = 0; i < (int)(sizeof(key_map) / sizeof(key_map[0])); i++) {
                        if (key_map[i].sdl_key == sdl_key) {
                            term_key = key_map[i].term_key;
                            break;
                        }
                    }

                    if (term_key != TERM_KEY_NONE) {
                        // Special key found — call on_key with term_key
                        if (callbacks->on_key)
                            result = callbacks->on_key(callbacks->user_data, term_key, tmod, 0);
                    } else if ((sdl_mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) && scancode != 0) {
                        // Ctrl/Alt + printable: resolve scancode → codepoint
                        SDL_Keycode resolved = SDL_GetKeyFromScancode(scancode, sdl_mod & SDL_KMOD_SHIFT, false);
                        if (resolved >= 32 && resolved < 127) {
                            uint32_t cp = (uint32_t)resolved;
                            // Lowercase if Shift not held
                            if (cp >= 'A' && cp <= 'Z' && !(sdl_mod & SDL_KMOD_SHIFT))
                                cp = cp - 'A' + 'a';
                            if (callbacks->on_key)
                                result = callbacks->on_key(callbacks->user_data, TERM_KEY_NONE, tmod, cp);
                        }
                    }

                    if (result.request_quit) {
                        SDL_SetAtomicInt(&ctx->quit_requested, 1);
                    } else if (result.force_redraw) {
                        ctx->force_redraw = true;
                    } else if (result.handled || (result.len > 0)) {
                        // Reset scroll position when typing
                        if (renderer_get_scroll_offset(rend) != 0) {
                            renderer_reset_scroll(rend);
                            ctx->force_redraw = true;
                        }

                        // Reset cursor blink on user input
                        ctx->cursor_blink_visible = true;
                        timer_reset(ctx->timers, ctx->cursor_blink_timer);
                        ctx->force_redraw = true;

                        // Write to PTY if callback provided raw data
                        if (result.len > 0 && !result.handled && ctx->pty) {
                            ssize_t written =
                                pty_write(ctx->pty, result.data, result.len);
                            if (written < 0) {
                                vlog("PTY write failed: %s\n", strerror(errno));
                            }
                        }
                    }
                }
            } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                if (callbacks && callbacks->on_text) {
                    // Skip if Ctrl or Alt is held
                    if (!(SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
                        KeyboardResult result = callbacks->on_text(
                            callbacks->user_data, event.text.text);

                        if (result.len > 0 && !result.handled) {
                            // Reset scroll position when typing
                            if (renderer_get_scroll_offset(rend) != 0) {
                                renderer_reset_scroll(rend);
                                ctx->force_redraw = true;
                            }

                            // Reset cursor blink on user input
                            ctx->cursor_blink_visible = true;
                            timer_reset(ctx->timers, ctx->cursor_blink_timer);
                            ctx->force_redraw = true;

                            if (ctx->pty)
                                pty_write(ctx->pty, result.data, result.len);
                        }
                    }
                }
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                if (callbacks && callbacks->on_resize) {
                    callbacks->on_resize(callbacks->user_data,
                                         event.window.data1,
                                         event.window.data2);
                }
                ctx->force_redraw = true;
            } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                vlog("Window close requested\n");
                SDL_SetAtomicInt(&ctx->quit_requested, 1);
            } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
                       event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                ctx->has_focus = (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
                ctx->force_redraw = true;
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                if (event.wheel.y != 0) {
                    bool consumed = false;
                    if (callbacks && callbacks->on_mouse) {
                        int button = (event.wheel.y > 0) ? 4 : 5;
                        int clicks = abs((int)event.wheel.y);
                        int tmod = sdl_mod_to_term(SDL_GetModState());
                        for (int i = 0; i < clicks && !consumed; i++) {
                            float mx, my;
                            SDL_GetMouseState(&mx, &my);
                            consumed = callbacks->on_mouse(callbacks->user_data, (int)mx, (int)my,
                                                           button, true, 0, tmod);
                        }
                    }
                    if (!consumed && callbacks && callbacks->on_scroll) {
                        callbacks->on_scroll(callbacks->user_data, (int)event.wheel.y * SCROLL_LINES_PER_TICK);
                    }
                }
                ctx->force_redraw = true;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                       event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                if (callbacks && callbacks->on_mouse) {
                    bool pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                    int button = event.button.button;
                    int clicks = pressed ? event.button.clicks : 0;
                    int tmod = sdl_mod_to_term(SDL_GetModState());
                    if (callbacks->on_mouse(callbacks->user_data, (int)event.button.x,
                                            (int)event.button.y, button, pressed,
                                            clicks, tmod)) {
                        ctx->force_redraw = true;
                    }
                }
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                if (callbacks && callbacks->on_mouse) {
                    bool any_button_pressed = (event.motion.state != 0);
                    int tmod = sdl_mod_to_term(SDL_GetModState());
                    if (callbacks->on_mouse(callbacks->user_data, (int)event.motion.x,
                                            (int)event.motion.y, 0, any_button_pressed,
                                            0, tmod)) {
                        ctx->force_redraw = true;
                    }
                }
            }
        } while (SDL_PollEvent(&event));

        // Render terminal only if needed
        if (terminal_needs_redraw(term) || ctx->force_redraw) {
            bool cursor_vis = !ctx->has_focus || !terminal_get_cursor_blink(term) || ctx->cursor_blink_visible;
            renderer_draw_terminal(rend, term, cursor_vis);
            SDL_RenderPresent(ctx->sdl_renderer);
            terminal_clear_redraw(term);
            ctx->force_redraw = false;
        }
    }

    vlog("Event loop exiting\n");

    // Stop text input
    SDL_StopTextInput(ctx->window);

    // Stop cursor blink timer
    if (ctx->cursor_blink_timer != TIMER_INVALID) {
        timer_remove(ctx->timers, ctx->cursor_blink_timer);
        ctx->cursor_blink_timer = TIMER_INVALID;
    }

    // Stop reader thread
    SDL_SetAtomicInt(&ctx->running, 0);
    if (ctx->pty_reader_thread) {
#ifdef _WIN32
        if (ctx->pty)
            pty_close_console(ctx->pty);
        SetEvent(ctx->wakeup_event);
#else
        if (ctx->wakeup_pipe[1] >= 0) {
            char c = 1;
            (void)write(ctx->wakeup_pipe[1], &c, 1);
        }
#endif
        SDL_WaitThread(ctx->pty_reader_thread, NULL);
        ctx->pty_reader_thread = NULL;
    }
}

static void sdl3_request_quit(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    SDL_SetAtomicInt(&ctx->quit_requested, 1);
}

static void sdl3_pause_pty(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    if (SDL_GetAtomicInt(&ctx->pty_paused))
        return;

    SDL_SetAtomicInt(&ctx->pty_paused, 1);
    vlog("PTY paused (backpressure)\n");

    // Wake reader thread so it re-enters wait without PTY reads
#ifdef _WIN32
    SetEvent(ctx->wakeup_event);
#else
    if (ctx->wakeup_pipe[1] >= 0) {
        char c = 1;
        (void)write(ctx->wakeup_pipe[1], &c, 1);
    }
#endif
}

static void sdl3_resume_pty(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;

    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    if (!SDL_GetAtomicInt(&ctx->pty_paused))
        return;

    SDL_SetAtomicInt(&ctx->pty_paused, 0);
    vlog("PTY resumed\n");

    // Wake reader thread so it re-includes PTY reads
#ifdef _WIN32
    SetEvent(ctx->wakeup_event);
#else
    if (ctx->wakeup_pipe[1] >= 0) {
        char c = 1;
        (void)write(ctx->wakeup_pipe[1], &c, 1);
    }
#endif
}

static float sdl3_get_display_scale(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return 0.0f;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    if (ctx->window) {
        float scale = SDL_GetWindowDisplayScale(ctx->window);
        if (scale > 0.0f)
            return scale;
    }
    return 0.0f;
}

static bool sdl3_get_display_size(PlatformBackend *plat, int *width, int *height)
{
    if (!plat || !plat->backend_data)
        return false;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    SDL_DisplayID display_id = 0;
    if (ctx->window)
        display_id = SDL_GetDisplayForWindow(ctx->window);
    if (!display_id)
        display_id = SDL_GetPrimaryDisplay();
    if (!display_id)
        return false;
    SDL_Rect bounds;
    if (!SDL_GetDisplayUsableBounds(display_id, &bounds))
        return false;
    if (width)
        *width = bounds.w;
    if (height)
        *height = bounds.h;
    return true;
}
