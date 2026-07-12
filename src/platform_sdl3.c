#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_version.h"

#include "common.h"
#include "path_compat.h"
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
enum PorttyEventCode
{
    EVENT_PTY_DATA = 1,
    EVENT_PTY_CLOSED,
    EVENT_PTY_CHILD_EXIT,
    EVENT_CURSOR_BLINK,
    EVENT_AUTOSCROLL_TICK,
    EVENT_LOTTIE_TICK,
    // Posted by sdl3_notify to wake an idle SDL_WaitEvent and force one repaint
    // so a notification shown out-of-band appears immediately.
    EVENT_NOTIFY_SHOW,
};

// PTY data event payload
typedef struct
{
    size_t len;
    char data[];
} PtyDataPayload;

// Deferred clipboard free list (Wayland use-after-free workaround).
// See the detailed comment near clipboard_cleanup_callback below.
typedef struct ClipboardDeferredFree
{
    char *ptr;
    int age; // number of event-loop iterations survived
    struct ClipboardDeferredFree *next;
} ClipboardDeferredFree;
static ClipboardDeferredFree *clipboard_deferred_head;

// Minimum age before a deferred string is safe to free.  The string
// must survive the iteration where cleanup fires plus one more full
// iteration, because the compositor's data_source_send can arrive
// during the SDL_WaitEvent of the *next* iteration (observed under Wayland).
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

    // Wakeup mechanism to interrupt reader thread on shutdown/pause
#ifdef _WIN32
    HANDLE wakeup_event;
#else
    int wakeup_pipe[2];
#endif

    // Timer system
    TimerManager *timers;
    TimerId cursor_blink_timer;
    TimerId autoscroll_timer;
    TimerId lottie_timer;
    bool cursor_blink_visible;
    bool has_focus;

    // Cached system cursors for OSC-8 hyperlink hover. Created lazily on
    // first set_cursor call; freed in sdl3_plat_destroy.
    SDL_Cursor *cursor_text;
    SDL_Cursor *cursor_pointer;
    PlatformCursor current_cursor;

    // Cached exe path (resolved once at startup via SDL_GetBasePath)
    char exe_path[PATH_MAX];

    // Working directory reported by the shell via OSC 7 or OSC 9;9.
    // Used by Ctrl+Shift+N to spawn a new terminal in the same directory.
    // On Windows, this is the only CWD source (the PEB-walk approach was
    // removed because ReadProcessMemory fails with ERROR_PARTIAL_COPY
    // for ConPTY children). On Unix, /proc/PID/cwd is tried first.
    char working_dir[PATH_MAX];

    // Stashed in sdl3_run so notify() can reach the renderer (which owns the
    // SDL-drawn panel) and mark the terminal dirty for a repaint.
    RendererBackend *rend;
    TerminalBackend *term;

    // True while the left mouse button is held down. On Wayland, when the
    // pointer crosses the window border during a drag, the compositor sends
    // wl_pointer.leave and SDL synthesizes a BUTTON_UP — but the physical
    // button may still be held. We track the real press state here so motion
    // events after re-entry can keep the drag alive. The release after a
    // border crossing cannot be detected on Wayland (SDL loses all button
    // state); the drag stays active until a click or copy resets it.
    bool left_button_down;
    // When a left-button BUTTON_UP arrives during an active selection, we
    // buffer it briefly instead of forwarding immediately. On Wayland, SDL
    // delivers a synthetic BUTTON_UP just before MOUSE_LEAVE when the
    // pointer crosses the window border — if MOUSE_LEAVE arrives within a
    // short time window, we discard the buffer (border artifact) and keep
    // the drag alive. Otherwise we forward it as a real release.
    bool left_button_up_buffered;
    int left_button_up_x;
    int left_button_up_y;
    Uint32 left_button_up_tick;

    // Sub-tick scroll accumulator. SDL3 reports wheel deltas as floats;
    // trackpads produce fractional values (e.g. 0.1 per event). We accumulate
    // until a whole tick is reached before dispatching scroll callbacks,
    // so slow trackpad scrolling is not silently dropped.
    float wheel_accum_y;
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

#include "portty_icon_png.h"

// Load and set the window icon from the embedded PNG
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
        /* Expected on Wayland — icon comes from .desktop + hicolor theme */
        vlog("SDL_SetWindowIcon skipped: %s\n", SDL_GetError());
    } else {
        vlog("Window icon set from embedded PNG (%dx%d)\n", w, h);
    }

    SDL_DestroySurface(surface);
    free(pixels);
}

// Forward declarations
static bool sdl3_plat_init(PlatformBackend *plat);
static void sdl3_plat_destroy(PlatformBackend *plat);
static void clipboard_deferred_free_advance(void);
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
static bool sdl3_open_url(PlatformBackend *plat, const char *url, char *err,
                          size_t errlen);
static void sdl3_notify(PlatformBackend *plat, const char *title,
                        const char *body, PlatformNotifyLevel level);
static void sdl3_notify_dismiss(PlatformBackend *plat);
static void sdl3_set_link_hint(PlatformBackend *plat, const char *url, int anchor_py);
static void sdl3_set_cursor(PlatformBackend *plat, PlatformCursor cursor);
static void sdl3_set_autoscroll(PlatformBackend *plat, bool enabled);
static bool sdl3_spawn_new_terminal(PlatformBackend *plat);

static void sdl3_set_working_dir(PlatformBackend *plat, const char *dir)
{
    if (!plat || !plat->backend_data || !dir)
        return;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    snprintf(ctx->working_dir, sizeof(ctx->working_dir), "%s", dir);
}

static const char *sdl3_get_exe_path(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return NULL;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    return ctx->exe_path[0] ? ctx->exe_path : NULL;
}

static bool sdl3_spawn_new_terminal(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return false;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;

    if (!ctx->exe_path[0])
        return false;

    /* Resolve the shell's CWD so the new terminal opens in the same
     * directory. On Windows, the OSC-reported CWD is the only source
     * (the PEB-walk approach via ReadProcessMemory fails with
     * ERROR_PARTIAL_COPY for ConPTY children). On Unix, /proc/PID/cwd
     * is reliable and always up-to-date, so it's tried as a fallback. */
    char cwd_path[PATH_MAX] = "";
    if (ctx->working_dir[0]) {
        snprintf(cwd_path, sizeof(cwd_path), "%s", ctx->working_dir);
    }
#ifndef _WIN32
    else if (ctx->pty) {
        pty_get_child_cwd(ctx->pty, cwd_path, sizeof(cwd_path));
    }
#endif

#ifdef _WIN32
    /* Use CreateProcessW so we can pass lpCurrentDirectory (the shell's
     * CWD). SDL_CreateProcess has no cwd parameter, so it would inherit
     * the parent's CWD instead. */
    WCHAR wexe[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, ctx->exe_path, -1, wexe,
                        MAX_PATH);

    WCHAR wcmdline[MAX_PATH];
    swprintf(wcmdline, MAX_PATH, L"\"%s\"", wexe);

    WCHAR wcwd[MAX_PATH] = L"";
    LPWSTR lpcwd = NULL;
    if (cwd_path[0]) {
        /* working_dir is already a native Windows path (converted in
         * on_cwd_change via path_compat_msys_to_win). Just flip any
         * remaining forward slashes to backslashes for CreateProcessW. */
        char norm[PATH_MAX];
        snprintf(norm, sizeof(norm), "%s", cwd_path);
        for (char *p = norm; *p; p++)
            if (*p == '/')
                *p = '\\';
        MultiByteToWideChar(CP_UTF8, 0, norm, -1, wcwd, MAX_PATH);
        lpcwd = wcwd;
    }

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(wexe, wcmdline, NULL, NULL, FALSE,
                       DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                       NULL, lpcwd, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    vlog("Failed to spawn terminal: %lu\n", GetLastError());
    return false;
#else
    pid_t pid = fork();
    if (pid < 0) {
        vlog("Failed to fork for new terminal: %s\n", strerror(errno));
        return false;
    }
    if (pid == 0) {
        // Child process
        setsid();
        if (cwd_path[0])
            chdir(cwd_path);
        execl(ctx->exe_path, ctx->exe_path, NULL);
        _exit(1);
    }
    // Parent — reap automatically (grandchild inherits from setsid)
    return true;
#endif
}

#ifdef _WIN32
static char *sdl3_get_default_font(PlatformBackend *plat)
{
    (void)plat;

    /* Read the user's preferred console font from the registry.
     * HKCU\Console\FaceName is set by Windows when the user changes
     * the console font in cmd.exe properties. */
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

    /* Skip generic placeholders — "__DefaultTTFont__" means "use the
     * system default TrueType console font", which is the "00" entry
     * under HKLM\...\Console\TrueTypeFont (Consolas on all modern
     * Windows).  Raster "Terminal" and legacy "0" are not useful. */
    if (_wcsicmp(face_w, L"0") == 0 ||
        _wcsicmp(face_w, L"Terminal") == 0)
        return NULL;

    if (_wcsicmp(face_w, L"__DefaultTTFont__") == 0) {
        /* Resolve the system default TT console font from the
         * TrueTypeFont registry key.  The "00" value is the default
         * TrueType font (Consolas on modern Windows). */
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
}
#endif

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
    .open_url = sdl3_open_url,
    .notify = sdl3_notify,
    .notify_dismiss = sdl3_notify_dismiss,
    .set_link_hint = sdl3_set_link_hint,
    .set_cursor = sdl3_set_cursor,
    .set_autoscroll = sdl3_set_autoscroll,
    .spawn_new_terminal = sdl3_spawn_new_terminal,
    .set_working_dir = sdl3_set_working_dir,
    .get_exe_path = sdl3_get_exe_path,
#ifdef _WIN32
    .get_default_font = sdl3_get_default_font,
#endif
};

// PTY reader thread function
#ifdef _WIN32
static int pty_reader_thread_func(void *data)
{
    SDL3PlatformData *ctx = (SDL3PlatformData *)data;
    char buf[4096];

    vlog("PTY reader thread started (W32)\n");

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
    if (!SDL_SetAppMetadata("portty", PORTTY_VERSION, "portty")) {
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
    ctx->autoscroll_timer = TIMER_INVALID;
    ctx->lottie_timer = TIMER_INVALID;
    ctx->cursor_blink_visible = true;

    plat->backend_data = ctx;

    /* Cache exe path now while the binary still exists on disk.
     * SDL_GetBasePath() caches internally and is platform-independent.
     * On Windows, append .exe so CreateProcessW finds the binary. */
    const char *base = SDL_GetBasePath();
    if (base) {
#ifdef _WIN32
        snprintf(ctx->exe_path, sizeof(ctx->exe_path), "%s" PACKAGE ".exe", base);
#else
        snprintf(ctx->exe_path, sizeof(ctx->exe_path), "%s" PACKAGE, base);
#endif
    }

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

    // Destroy cached cursors
    if (ctx->cursor_text) {
        SDL_DestroyCursor(ctx->cursor_text);
        ctx->cursor_text = NULL;
    }
    if (ctx->cursor_pointer) {
        SDL_DestroyCursor(ctx->cursor_pointer);
        ctx->cursor_pointer = NULL;
    }

    // Flush any remaining deferred clipboard frees
    while (clipboard_deferred_head) {
        ClipboardDeferredFree *next = clipboard_deferred_head->next;
        free(clipboard_deferred_head->ptr);
        free(clipboard_deferred_head);
        clipboard_deferred_head = next;
    }

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

    // Create renderer. portty requires SDL's GPU renderer (Vulkan / D3D12 /
    // Metal): only it performs gamma-correct (linear-light) glyph blending,
    // via SRGB_LINEAR float render targets. The OpenGL renderer blends in sRGB
    // space (thin, gamma-incorrect text) and is intentionally not used.
    vlog("Creating GPU renderer\n");
    SDL_ClearError();

    SDL_PropertiesID rprops = SDL_CreateProperties();
    if (rprops) {
        SDL_SetPointerProperty(rprops, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, ctx->window);
        SDL_SetStringProperty(rprops, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
        ctx->sdl_renderer = SDL_CreateRendererWithProperties(rprops);
        SDL_DestroyProperties(rprops);
    }
    if (!ctx->sdl_renderer) {
        const char *error = SDL_GetError();
        fprintf(stderr,
                "ERROR: Failed to create GPU renderer (requires Vulkan/D3D12/Metal): %s\n",
                (error && error[0]) ? error : "no specific error message");
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
        return false;
    }
    vlog("Renderer created: %s\n", SDL_GetRendererName(ctx->sdl_renderer));

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
        SDL_SetWindowTitle(ctx->window, title ? title : "portty");
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

static const void *clipboard_data_callback(void *userdata, const char *mime_type,
                                           size_t *size)
{
    (void)mime_type;
    const char *text = (const char *)userdata;
    if (size)
        *size = text ? strlen(text) : 0;
    return text;
}

// On Wayland, the compositor may still dispatch data_source_send for a
// cancelled clipboard offer.  The Wayland protocol is asynchronous: a
// wl_data_source.send event that was already queued in the socket buffer
// before the client called wl_data_source_destroy will still be dispatched
// on a future wl_display_dispatch_queue_pending call.  SDL_SetClipboardData
// calls SDL_CancelClipboardData(0) which immediately invokes this cleanup
// callback, but the compositor's pending send has not been processed yet.
// If we free immediately, the subsequent data_source_handle_send calls
// clipboard_data_callback on freed memory (use-after-free).
//
// Each cancelled string is appended to a linked list with an age counter.
// The event loop increments all ages once per iteration and frees any that
// have reached CLIPBOARD_DEFERRED_MIN_AGE.  This ensures every string
// survives at least that many full iterations.
//
// A list is used instead of a fixed-size ring because the number of
// clipboard_set calls per iteration is unbounded: a user selection and
// multiple OSC 52 sequences from the PTY can all fire in the same
// iteration, each cancelling the previous offer.  Only the offer that was
// active at the last wl_display_dispatch boundary can have a pending
// data_source_send, but we cannot know which one that was, so all are
// deferred.  The list automatically handles any burst size.

static void clipboard_cleanup_callback(void *userdata)
{
    char *ptr = (char *)userdata;
    if (!ptr)
        return;
    ClipboardDeferredFree *entry = malloc(sizeof(*entry));
    if (!entry) {
        // Allocation failure — free immediately (better than leaking
        // and better than a UAF; the race window is narrow).
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

static bool sdl3_clipboard_set(PlatformBackend *plat, const char *text)
{
    (void)plat;
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

    // Stash for notify() (renderer-drawn panel + dirty marking)
    ctx->rend = rend;
    ctx->term = term;

    // Start cursor blink timer
    ctx->cursor_blink_visible = true;
    ctx->has_focus = true;
    ctx->cursor_blink_timer = timer_add(ctx->timers, CURSOR_BLINK_INTERVAL_MS, true,
                                        EVENT_CURSOR_BLINK, NULL);

    // Lottie animation tick timer (~60 fps) is started on demand when
    // animations are loaded, and stopped when all are removed. See the
    // EVENT_PTY_DATA and EVENT_LOTTIE_TICK handlers below.

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

    terminal_mark_dirty(term); // force the initial paint

    SDL_Event event;
    while (!SDL_GetAtomicInt(&ctx->quit_requested)) {
        // Wait for events - truly event-driven, no timeout
        if (!SDL_WaitEvent(&event)) {
            vlog("SDL_WaitEvent error: %s\n", SDL_GetError());
            break;
        }

        // Track whether this iteration processed PTY output, so we can
        // re-resolve OSC-8 hover at the live pointer before painting.
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
                        renderer_process_pty_data(rend, term, payload->data, payload->len);
                        platform_set_window_title(plat, terminal_get_title(term));
                        free(payload);
                        pty_processed = true;
                        // Start the Lottie tick timer if animations are now
                        // present (the shell may have loaded one via APC).
                        if (ctx->lottie_timer == TIMER_INVALID &&
                            terminal_lottie_count(term) > 0) {
                            ctx->lottie_timer = timer_add(ctx->timers, 16, true,
                                                          EVENT_LOTTIE_TICK, NULL);
                        }
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
                        terminal_mark_dirty(term);
                    }
                    break;

                case EVENT_AUTOSCROLL_TICK:
                    if (callbacks && callbacks->on_autoscroll_tick) {
                        callbacks->on_autoscroll_tick(callbacks->user_data);
                        terminal_mark_dirty(term);
                    }
                    break;

                case EVENT_LOTTIE_TICK:
                    if (terminal_lottie_tick(term, SDL_GetTicksNS() / 1000))
                        terminal_mark_dirty(term);
                    // Stop the timer if all animations have been removed.
                    if (terminal_lottie_count(term) == 0) {
                        timer_remove(ctx->timers, ctx->lottie_timer);
                        ctx->lottie_timer = TIMER_INVALID;
                    }
                    break;

                case EVENT_NOTIFY_SHOW:
                    // renderer_set_notification already ran in sdl3_notify;
                    // just force a repaint so the panel appears.
                    terminal_mark_dirty(term);
                    break;
                }
                break;

            case SDL_EVENT_QUIT:
                vlog("SDL quit event received\n");
                SDL_SetAtomicInt(&ctx->quit_requested, 1);
                break;

            case SDL_EVENT_KEY_DOWN:
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
                        terminal_mark_dirty(term);
                    } else if (result.handled || (result.len > 0)) {
                        // Reset scroll position when typing
                        if (renderer_get_scroll_offset(rend) != 0) {
                            renderer_reset_scroll(rend);
                            terminal_mark_dirty(term);
                        }

                        // Reset cursor blink on user input
                        ctx->cursor_blink_visible = true;
                        timer_reset(ctx->timers, ctx->cursor_blink_timer);
                        terminal_mark_dirty(term);

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
                break;

            case SDL_EVENT_TEXT_INPUT:
                if (callbacks && callbacks->on_text) {
                    // Skip if Ctrl or Alt is held
                    if (!(SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
                        KeyboardResult result = callbacks->on_text(
                            callbacks->user_data, event.text.text);

                        // Repaint whenever the keystroke produced PTY data OR was
                        // consumed by an app-level handler that changed the screen
                        // (e.g. the internal pager scrolling/closing on q/j/k/space):
                        // those return handled with no data, and the host term must
                        // still be marked dirty or the frame only repaints on the
                        // next unrelated event (cursor blink, PTY output). Mirrors
                        // the KEY_DOWN handler.
                        if (result.handled || result.force_redraw || result.len > 0) {
                            // Reset scroll position when typing
                            if (renderer_get_scroll_offset(rend) != 0) {
                                renderer_reset_scroll(rend);
                                terminal_mark_dirty(term);
                            }

                            // Reset cursor blink on user input
                            ctx->cursor_blink_visible = true;
                            timer_reset(ctx->timers, ctx->cursor_blink_timer);
                            terminal_mark_dirty(term);

                            // Forward only un-handled raw data to the shell.
                            if (result.len > 0 && !result.handled && ctx->pty)
                                pty_write(ctx->pty, result.data, result.len);
                        }
                    }
                }
                break;

            case SDL_EVENT_WINDOW_RESIZED:
                if (callbacks && callbacks->on_resize) {
                    callbacks->on_resize(callbacks->user_data,
                                         event.window.data1,
                                         event.window.data2);
                }
                terminal_mark_dirty(term);
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                vlog("Window close requested\n");
                SDL_SetAtomicInt(&ctx->quit_requested, 1);
                break;

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                ctx->has_focus = (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
                if (!ctx->has_focus)
                    renderer_set_link_hint(ctx->rend, NULL, 0);
                terminal_mark_dirty(term);
                break;

            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                // Pointer left the window — drop any link hover hint + underline.
                renderer_set_link_hint(ctx->rend, NULL, 0);
                if (term)
                    terminal_set_hovered_hyperlink(term, 0);
                // On Wayland, when the pointer reaches the window border
                // during a drag, SDL delivers a synthetic BUTTON_UP just
                // before MOUSE_LEAVE. If we have a buffered button-up from
                // within a short time window, discard it — it was the
                // border artifact, not a real release. Restore
                // left_button_down so motion events after re-entry keep
                // the drag alive.
                if (ctx->left_button_up_buffered &&
                    SDL_GetTicks() - ctx->left_button_up_tick < 100) {
                    ctx->left_button_up_buffered = false;
                    ctx->left_button_down = true;
                }
                // If a left-button drag (selection) is in progress, notify
                // main so it can start drag-autoscroll. On Wayland,
                // SDL_CaptureMouse is a no-op, so no further MOUSE_MOTION
                // events will arrive — the autoscroll timer is the only way
                // to keep scrolling while the pointer is outside the window.
                if (callbacks && callbacks->on_mouse_leave) {
                    float mx, my;
                    SDL_GetMouseState(&mx, &my);
                    callbacks->on_mouse_leave(callbacks->user_data, (int)mx, (int)my);
                }
                terminal_mark_dirty(term);
                break;

            case SDL_EVENT_WINDOW_MOUSE_ENTER:
                // Pointer re-entered the window — stop drag-autoscroll if it
                // was active. Motion events will resume driving selection.
                if (callbacks && callbacks->on_mouse_enter)
                    callbacks->on_mouse_enter(callbacks->user_data);
                break;

            case SDL_EVENT_MOUSE_WHEEL:
            {
                float dy = event.wheel.y;
                if (dy != 0.0f) {
                    // SDL3 delivers float wheel deltas. Trackpads produce
                    // sub-tick values (e.g. 0.1); without accumulation those
                    // events truncate to zero and are silently dropped,
                    // making slow trackpad scrolling non-functional.
                    ctx->wheel_accum_y += dy;
                    int whole_ticks = (int)ctx->wheel_accum_y; // truncates toward zero
                    if (whole_ticks != 0) {
                        ctx->wheel_accum_y -= (float)whole_ticks;
                        bool consumed = false;
                        int button = (whole_ticks > 0) ? 4 : 5;
                        int clicks = abs(whole_ticks);
                        int tmod = sdl_mod_to_term(SDL_GetModState());
                        // Use the position embedded in the wheel event rather
                        // than a separate SDL_GetMouseState() call.
                        int mx = (int)event.wheel.mouse_x;
                        int my = (int)event.wheel.mouse_y;
                        if (callbacks && callbacks->on_mouse) {
                            for (int i = 0; i < clicks && !consumed; i++) {
                                consumed = callbacks->on_mouse(
                                    callbacks->user_data, mx, my,
                                    button, true, 0, tmod);
                            }
                        }
                        if (!consumed && callbacks && callbacks->on_scroll) {
                            callbacks->on_scroll(callbacks->user_data, whole_ticks);
                        }
                    }
                }
                terminal_mark_dirty(term);
                break;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (callbacks && callbacks->on_mouse) {
                    bool pressed = (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                    int button = event.button.button;
                    int clicks = pressed ? event.button.clicks : 0;
                    int tmod = sdl_mod_to_term(SDL_GetModState());
                    // Flush any buffered button-up from a previous event
                    // (not consumed by MOUSE_LEAVE) — it was a real release.
                    if (ctx->left_button_up_buffered) {
                        ctx->left_button_up_buffered = false;
                        ctx->left_button_down = false;
                        SDL_CaptureMouse(false);
                        if (callbacks->on_mouse(callbacks->user_data,
                                                ctx->left_button_up_x,
                                                ctx->left_button_up_y, 1, false,
                                                0, tmod)) {
                            terminal_mark_dirty(term);
                        }
                    }
                    if (button == 1) {
                        if (pressed) {
                            ctx->left_button_down = true;
                            SDL_CaptureMouse(true);
                            if (callbacks->on_mouse(callbacks->user_data,
                                                    (int)event.button.x,
                                                    (int)event.button.y, button,
                                                    pressed, clicks, tmod)) {
                                terminal_mark_dirty(term);
                            }
                        } else {
                            // Buffer the release. On Wayland, a synthetic
                            // BUTTON_UP arrives just before MOUSE_LEAVE when
                            // the pointer crosses the border during a drag.
                            // MOUSE_LEAVE will discard the buffer; if no
                            // MOUSE_LEAVE follows, the next event or LOOP_END
                            // flushes it as a real release.
                            ctx->left_button_up_buffered = true;
                            ctx->left_button_up_x = (int)event.button.x;
                            ctx->left_button_up_y = (int)event.button.y;
                            ctx->left_button_up_tick = SDL_GetTicks();
                            ctx->left_button_down = false;
                        }
                    } else {
                        if (callbacks->on_mouse(callbacks->user_data,
                                                (int)event.button.x,
                                                (int)event.button.y, button,
                                                pressed, clicks, tmod)) {
                            terminal_mark_dirty(term);
                        }
                    }
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (callbacks && callbacks->on_mouse) {
                    // Flush a buffered button-up if present (real release,
                    // not consumed by MOUSE_LEAVE).
                    if (ctx->left_button_up_buffered) {
                        ctx->left_button_up_buffered = false;
                        ctx->left_button_down = false;
                        SDL_CaptureMouse(false);
                        int tmod = sdl_mod_to_term(SDL_GetModState());
                        if (callbacks->on_mouse(callbacks->user_data,
                                                ctx->left_button_up_x,
                                                ctx->left_button_up_y, 1, false,
                                                0, tmod)) {
                            terminal_mark_dirty(term);
                        }
                    }
                    bool any_button_pressed = (event.motion.state != 0);
                    int tmod = sdl_mod_to_term(SDL_GetModState());
                    // On Wayland, after the pointer crosses the window
                    // border during a drag, SDL loses the button state —
                    // motion events arrive with state=0 even though the
                    // button is still held. Use left_button_down to keep
                    // the drag alive. The release after a border crossing
                    // cannot be detected on Wayland; the drag stays active
                    // until a click or copy resets it.
                    if (ctx->left_button_down)
                        any_button_pressed = true;
                    if (callbacks->on_mouse(callbacks->user_data, (int)event.motion.x,
                                            (int)event.motion.y, 0, any_button_pressed,
                                            0, tmod)) {
                        terminal_mark_dirty(term);
                    }
                }
                break;

            default:
                break;
            }
        } while (SDL_PollEvent(&event));

        // If a left-button release was buffered (not consumed by MOUSE_LEAVE)
        // and no more events are pending, it's a real release — flush it now.
        if (ctx->left_button_up_buffered && callbacks && callbacks->on_mouse) {
            ctx->left_button_up_buffered = false;
            ctx->left_button_down = false;
            SDL_CaptureMouse(false);
            int tmod = sdl_mod_to_term(SDL_GetModState());
            callbacks->on_mouse(callbacks->user_data,
                                ctx->left_button_up_x,
                                ctx->left_button_up_y, 1, false,
                                0, tmod);
            // A release returns false from on_mouse, but we must repaint
            // to show the finalized selection state.
            terminal_mark_dirty(term);
        }

        // Age and prune deferred-clipboard entries.  By now SDL_PumpEvents
        // (inside SDL_PollEvent) has drained the Wayland dispatch, so entries
        // that have reached CLIPBOARD_DEFERRED_MIN_AGE are safe to free.
        clipboard_deferred_free_advance();

        // PTY output cleared any OSC-8 hover; re-resolve it at the live pointer
        // so a held-still mouse over a link stays highlighted across redraws
        // (otherwise the underline flickers idle until the next motion event).
        if (pty_processed && callbacks && callbacks->on_revalidate_hover) {
            int px = -1, py = -1;
            if (SDL_GetMouseFocus() == ctx->window) {
                float mx, my;
                SDL_GetMouseState(&mx, &my);
                px = (int)mx;
                py = (int)my;
            }
            if (callbacks->on_revalidate_hover(callbacks->user_data, px, py))
                terminal_mark_dirty(term);
        }

        // Drain VT damage accumulated by this iteration's PTY input, then
        // render only if anything (content, cursor, or an app-level change
        // via terminal_mark_dirty) actually needs repainting.
        terminal_flush_damage(term);
        if (terminal_needs_redraw(term)) {
            bool cursor_vis = !ctx->has_focus || !terminal_get_cursor_blink(term) || ctx->cursor_blink_visible;
            renderer_draw_terminal(rend, term, cursor_vis);
            SDL_RenderPresent(ctx->sdl_renderer);
            terminal_clear_redraw(term);
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

    // Stop lottie animation tick timer
    if (ctx->lottie_timer != TIMER_INVALID) {
        timer_remove(ctx->timers, ctx->lottie_timer);
        ctx->lottie_timer = TIMER_INVALID;
    }

    // Stop autoscroll timer
    if (ctx->autoscroll_timer != TIMER_INVALID) {
        timer_remove(ctx->timers, ctx->autoscroll_timer);
        ctx->autoscroll_timer = TIMER_INVALID;
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

static bool sdl3_open_url(PlatformBackend *plat, const char *url, char *err,
                          size_t errlen)
{
    (void)plat;
    if (!url)
        return false;
    /* SDL_OpenURL fronts xdg-open / open / ShellExecute on the host OS. */
    if (SDL_OpenURL(url))
        return true;
    if (err && errlen > 0) {
        const char *msg = SDL_GetError();
        snprintf(err, errlen, "%s", (msg && *msg) ? msg : "unknown error");
    }
    return false;
}

static void sdl3_notify(PlatformBackend *plat, const char *title,
                        const char *body, PlatformNotifyLevel level)
{
    if (!plat || !plat->backend_data)
        return;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    renderer_set_notification(ctx->rend, title, body, (int)level);

    // Force a repaint. sdl3_notify usually runs from inside on_mouse (already
    // mid-iteration), but posting a user event also wakes an idle SDL_WaitEvent
    // for any out-of-band caller.
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_USER;
    ev.user.code = EVENT_NOTIFY_SHOW;
    SDL_PushEvent(&ev);
}

static void sdl3_notify_dismiss(PlatformBackend *plat)
{
    if (!plat || !plat->backend_data)
        return;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    renderer_clear_notification(ctx->rend);
    if (ctx->term)
        terminal_mark_dirty(ctx->term);
}

// Hover hint: forward to the renderer-drawn strip. The repaint is driven by
// the caller (on_mouse / revalidate return true → mark dirty), except for the
// out-of-band pointer-leave path, which marks dirty itself.
static void sdl3_set_link_hint(PlatformBackend *plat, const char *url, int anchor_py)
{
    if (!plat || !plat->backend_data)
        return;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    renderer_set_link_hint(ctx->rend, url, anchor_py);
}

static void sdl3_set_autoscroll(PlatformBackend *plat, bool enabled)
{
    if (!plat || !plat->backend_data)
        return;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    if (enabled) {
        if (ctx->autoscroll_timer == TIMER_INVALID && ctx->timers) {
            ctx->autoscroll_timer = timer_add(ctx->timers, AUTOSCROLL_INTERVAL_MS,
                                              true, EVENT_AUTOSCROLL_TICK, NULL);
        }
    } else {
        if (ctx->autoscroll_timer != TIMER_INVALID) {
            timer_remove(ctx->timers, ctx->autoscroll_timer);
            ctx->autoscroll_timer = TIMER_INVALID;
        }
    }
}

static void sdl3_set_cursor(PlatformBackend *plat, PlatformCursor cursor)
{
    if (!plat || !plat->backend_data)
        return;
    SDL3PlatformData *ctx = (SDL3PlatformData *)plat->backend_data;
    if (ctx->current_cursor == cursor &&
        ((cursor == PLATFORM_CURSOR_TEXT && ctx->cursor_text) ||
         (cursor == PLATFORM_CURSOR_POINTER && ctx->cursor_pointer)))
        return;

    SDL_Cursor *c = NULL;
    if (cursor == PLATFORM_CURSOR_POINTER) {
        if (!ctx->cursor_pointer)
            ctx->cursor_pointer = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
        c = ctx->cursor_pointer;
    } else {
        if (!ctx->cursor_text)
            ctx->cursor_text = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
        c = ctx->cursor_text;
    }
    if (c)
        SDL_SetCursor(c);
    ctx->current_cursor = cursor;
}
