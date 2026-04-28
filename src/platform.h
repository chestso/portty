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

    // Open a URL with the system's default handler. Returns true on
    // success. Implementations: SDL_OpenURL on SDL3, gtk_show_uri on GTK4.
    bool (*open_url)(PlatformBackend *plat, const char *url);

    // Set the mouse cursor shape. Used for OSC-8 hyperlink hover.
    void (*set_cursor)(PlatformBackend *plat, PlatformCursor cursor);

    // Window title dedup (managed by platform_set_window_title wrapper)
    char *last_title;
};

// Platform API
PlatformBackend *platform_init(PlatformBackend *plat);
void platform_destroy(PlatformBackend *plat);

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

bool platform_open_url(PlatformBackend *plat, const char *url);
void platform_set_cursor(PlatformBackend *plat, PlatformCursor cursor);

#endif /* PLATFORM_H */
