#ifndef PLATFORM_H
#define PLATFORM_H

#include "bloom_pty.h"
#include "rend.h"
#include "term.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declarations
struct PlatformBackend;
typedef struct PlatformBackend PlatformBackend;
struct PlatformCallbacks;
typedef struct PlatformCallbacks PlatformCallbacks;

// Cursor shape requested by the application (for hyperlink hover, etc.)
typedef enum
{
    PLATFORM_CURSOR_TEXT = 0,
    PLATFORM_CURSOR_POINTER,
} PlatformCursor;

// Severity of a notification panel message. Drives the SDL3 accent colour and
// the GTK4 strip's icon + libadwaita colour class.
typedef enum
{
    PLATFORM_NOTIFY_INFO = 0,
    PLATFORM_NOTIFY_WARNING,
    PLATFORM_NOTIFY_ERROR,
} PlatformNotifyLevel;

// Result from keyboard callbacks — bytes to write to PTY
typedef struct
{
    char data[16];
    size_t len;
    bool handled;      // If true, event was consumed by an app-level shortcut
    bool request_quit; // If true, request event loop to quit
    bool force_redraw; // If true, request a redraw after this event
} KeyboardResult;

// Callbacks from platform to main application
struct PlatformCallbacks
{
    // Special keys and Ctrl/Alt combos — platform translates SDLK→TERM_KEY,
    // resolves scancode→codepoint for Ctrl+letter before calling.
    // key: TERM_KEY_* constant (TERM_KEY_NONE if not a special key)
    // mod: TERM_MOD_* flags
    // codepoint: resolved character for Ctrl/Alt+letter (0 if special key)
    KeyboardResult (*on_key)(void *user_data, int key, int mod,
                             uint32_t codepoint);

    // Pure UTF-8 text from IME/compose (platform filters Ctrl/Alt-held)
    KeyboardResult (*on_text)(void *user_data, const char *text);

    // Handle window resize
    void (*on_resize)(void *user_data, int pixel_w, int pixel_h);

    // Handle scroll wheel (fallback for scrollback when mouse mode is disabled)
    void (*on_scroll)(void *user_data, int delta);

    // Handle mouse events - returns true if event was consumed
    // button: 1=left, 2=middle, 3=right, 4=wheel up, 5=wheel down
    // clicks: click count (1=single, 2=double, 3=triple; 0 for motion/wheel)
    // mod: TERM_MOD_* flags
    bool (*on_mouse)(void *user_data, int pixel_x, int pixel_y,
                     int button, bool pressed, int clicks, int mod);

    // Periodic tick fired while platform_set_autoscroll(true) is in effect.
    // Used by main to scroll the viewport and extend the selection while the
    // user drags past the top/bottom edges of the window.
    void (*on_autoscroll_tick)(void *user_data);

    // Re-resolve OSC 8 hover at the current pointer after content changed
    // (e.g. an app redrew the screen) without a fresh motion event. px,py are
    // in the same pixel space as on_mouse; (-1,-1) means the pointer is not
    // over our content. Returns true if hover state changed (caller repaints).
    bool (*on_revalidate_hover)(void *user_data, int px, int py);

    // User data passed to callbacks
    void *user_data;
};

// Backend interface definition
struct PlatformBackend
{
    const char *name;
    void *backend_data;

    // Lifecycle
    bool (*init)(PlatformBackend *plat);
    void (*destroy)(PlatformBackend *plat);

    // Window
    bool (*create_window)(PlatformBackend *plat, const char *title,
                          int width, int height);
    void (*show_window)(PlatformBackend *plat);
    void (*set_window_size)(PlatformBackend *plat, int width, int height);
    void (*set_window_title)(PlatformBackend *plat, const char *title);

    // SDL handles (renderer backend needs these for init)
    void *(*get_sdl_renderer)(PlatformBackend *plat);
    void *(*get_sdl_window)(PlatformBackend *plat);

    // Clipboard
    char *(*clipboard_get)(PlatformBackend *plat);
    bool (*clipboard_set)(PlatformBackend *plat, const char *text);
    void (*clipboard_free)(PlatformBackend *plat, char *text);

    // Async clipboard paste — write clipboard content to PTY with
    // bracketed paste. Returns true if handled asynchronously (GTK4).
    // If NULL or returns false, caller falls back to synchronous path.
    bool (*clipboard_paste_async)(PlatformBackend *plat,
                                  TerminalBackend *term, PtyContext *pty);

    // Event loop (absorbs EventLoopBackend)
    bool (*register_pty)(PlatformBackend *plat, PtyContext *pty);
    void (*run)(PlatformBackend *plat, TerminalBackend *term,
                RendererBackend *rend, PlatformCallbacks *callbacks);
    void (*request_quit)(PlatformBackend *plat);

    // PTY backpressure — pause/resume reading from PTY fd
    void (*pause_pty)(PlatformBackend *plat);
    void (*resume_pty)(PlatformBackend *plat);

    // Query desktop environment for preferred monospace font (optional)
    char *(*get_default_font)(PlatformBackend *plat);

    // Get content display scale (physical DPI / 96). Returns 0 if unknown.
    float (*get_display_scale)(PlatformBackend *plat);

    // Get usable display size in physical pixels. Returns false if unknown.
    bool (*get_display_size)(PlatformBackend *plat, int *width, int *height);

    // Open a URL with the system's default handler. Returns true on success.
    // On failure, writes a human-readable reason into `err` (size `errlen`,
    // never overflowed, may be left untouched on success). Implementations:
    // SDL_OpenURL on SDL3, g_app_info_launch_default_for_uri on GTK4.
    bool (*open_url)(PlatformBackend *plat, const char *url, char *err,
                     size_t errlen);

    // Set the mouse cursor shape. Used for OSC-8 hyperlink hover.
    void (*set_cursor)(PlatformBackend *plat, PlatformCursor cursor);

    // Show a top notification panel with a title and optional multi-line body.
    // SDL3 draws it via the renderer; GTK4 reveals a native libadwaita strip.
    // The panel persists until dismissed (close button) or superseded by a
    // newer notify() call.
    void (*notify)(PlatformBackend *plat, const char *title, const char *body,
                   PlatformNotifyLevel level);

    // Dismiss the current notification panel (no-op if none is shown).
    void (*notify_dismiss)(PlatformBackend *plat);

    // Show (or update) a transient hover hint revealing the real URI of the
    // OSC-8 link under the pointer; url == NULL/"" hides it. anchor_py is the
    // hovered link's physical-pixel Y, used to position the hint clear of the
    // link (top, flipping to bottom). SDL3 draws a full-width strip via the
    // renderer; GTK4 reveals a native .osd pill. Independent of notify().
    void (*set_link_hint)(PlatformBackend *plat, const char *url, int anchor_py);

    // Enable or disable the drag-autoscroll tick. While enabled, the
    // backend fires `on_autoscroll_tick` on its own timer (~30Hz).
    void (*set_autoscroll)(PlatformBackend *plat, bool enabled);

    // GPU device + driver description for the diagnostics report (only the
    // GTK4/Vulkan backend, which owns device creation, knows this). Returns
    // false and leaves the out-params untouched when unavailable. *libre is set
    // true for permissively-licensed open-source drivers (Mesa). Strings are
    // platform-owned, valid for the platform's lifetime.
    bool (*get_gpu_info)(PlatformBackend *plat, const char **device,
                         const char **driver, bool *libre);

    // Spawn a new terminal window. The platform resolves the child's CWD
    // from the PTY child process and launches a new instance of the same
    // binary. Returns true on success.
    bool (*spawn_new_terminal)(PlatformBackend *plat);

    // Window title dedup (managed by platform_set_window_title wrapper)
    char *last_title;
};

// Platform API
PlatformBackend *platform_init(PlatformBackend *plat);
void platform_destroy(PlatformBackend *plat);
const char *platform_get_name(PlatformBackend *plat);

bool platform_create_window(PlatformBackend *plat, const char *title,
                            int width, int height);
void platform_show_window(PlatformBackend *plat);
void platform_set_window_size(PlatformBackend *plat, int width, int height);
void platform_set_window_title(PlatformBackend *plat, const char *title);

void *platform_get_sdl_renderer(PlatformBackend *plat);
void *platform_get_sdl_window(PlatformBackend *plat);

char *platform_clipboard_get(PlatformBackend *plat);
bool platform_clipboard_set(PlatformBackend *plat, const char *text);
void platform_clipboard_free(PlatformBackend *plat, char *text);

bool platform_clipboard_paste_async(PlatformBackend *plat,
                                    TerminalBackend *term, PtyContext *pty);

bool platform_register_pty(PlatformBackend *plat, PtyContext *pty);
void platform_run(PlatformBackend *plat, TerminalBackend *term,
                  RendererBackend *rend, PlatformCallbacks *callbacks);
void platform_request_quit(PlatformBackend *plat);

void platform_pause_pty(PlatformBackend *plat);
void platform_resume_pty(PlatformBackend *plat);

char *platform_get_default_font(PlatformBackend *plat);
float platform_get_display_scale(PlatformBackend *plat);
bool platform_get_display_size(PlatformBackend *plat, int *width, int *height);

bool platform_open_url(PlatformBackend *plat, const char *url, char *err,
                       size_t errlen);
void platform_notify(PlatformBackend *plat, const char *title, const char *body,
                     PlatformNotifyLevel level);
void platform_notify_dismiss(PlatformBackend *plat);
void platform_set_link_hint(PlatformBackend *plat, const char *url, int anchor_py);
void platform_set_cursor(PlatformBackend *plat, PlatformCursor cursor);
void platform_set_autoscroll(PlatformBackend *plat, bool enabled);
bool platform_get_gpu_info(PlatformBackend *plat, const char **device,
                           const char **driver, bool *libre);
bool platform_spawn_new_terminal(PlatformBackend *plat);

#endif /* PLATFORM_H */
