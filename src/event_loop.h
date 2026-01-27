#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include "bloom_pty.h"
#include "rend.h"
#include "term.h"
#include <stdbool.h>

// Forward declarations
struct EventLoopBackend;
typedef struct EventLoopBackend EventLoopBackend;

// Callback for handling keyboard input - returns bytes to write to PTY
typedef struct
{
    char data[16];
    size_t len;
    bool handled;      // If true, event was consumed (e.g., Ctrl+G for debug grid)
    bool request_quit; // If true, request event loop to quit
    bool force_redraw; // If true, request a redraw after this event
} KeyboardResult;

// Callbacks from event loop to main application
typedef struct
{
    // Handle keyboard input, return data to write to PTY
    KeyboardResult (*on_keyboard)(void *user_data, int key, int mod, bool is_text,
                                  const char *text);

    // Handle window resize
    void (*on_resize)(void *user_data, int width, int height);

    // Handle scroll wheel
    void (*on_scroll)(void *user_data, int delta);

    // User data passed to callbacks
    void *user_data;
} EventLoopCallbacks;

// Backend interface definition
struct EventLoopBackend
{
    const char *name;

    // Backend-specific data
    void *backend_data;

    // Backend function pointers
    bool (*init)(EventLoopBackend *loop, void *window, void *renderer);
    void (*destroy)(EventLoopBackend *loop);
    bool (*register_pty)(EventLoopBackend *loop, PtyContext *pty);
    void (*unregister_pty)(EventLoopBackend *loop);
    void (*run)(EventLoopBackend *loop, TerminalBackend *term, RendererBackend *rend,
                EventLoopCallbacks *callbacks);
    void (*request_quit)(EventLoopBackend *loop);
};

// Event loop API
EventLoopBackend *event_loop_init(EventLoopBackend *loop, void *window, void *renderer);
void event_loop_destroy(EventLoopBackend *loop);
bool event_loop_register_pty(EventLoopBackend *loop, PtyContext *pty);
void event_loop_unregister_pty(EventLoopBackend *loop);
void event_loop_run(EventLoopBackend *loop, TerminalBackend *term, RendererBackend *rend,
                    EventLoopCallbacks *callbacks);
void event_loop_request_quit(EventLoopBackend *loop);

#endif /* EVENT_LOOP_H */
