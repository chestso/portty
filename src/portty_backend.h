#ifndef PORTTY_BACKEND_H
#define PORTTY_BACKEND_H

#include "portty_pty.h"
#include "term.h"
#include "diag.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct PorttyApp;
typedef struct PorttyApp PorttyApp;

typedef struct
{
    char data[16];
    size_t len;
    bool handled;      // If true, event was consumed by an app-level shortcut
    bool request_quit; // If true, request event loop to quit
    bool force_redraw; // If true, request a redraw after this event
} KeyboardResult;

typedef enum
{
    PORTTY_CURSOR_TEXT = 0,
    PORTTY_CURSOR_POINTER,
} PorttyCursor;

typedef enum
{
    PORTTY_NOTIFY_INFO = 0,
    PORTTY_NOTIFY_WARNING,
    PORTTY_NOTIFY_ERROR,
} PorttyNotifyLevel;

// Read-only snapshot for diagnostics report.
typedef struct
{
    const char *platform_name; // windowing/platform system: "SDL3", "Sokol", etc.
    const char *backend_name;  // renderer name: "gpu", "vulkan", etc.
    const char *gpu_device;
    const char *gpu_driver;
    GpuDriverLibre gpu_driver_libre;
    bool linear_light;
    bool glyph_shader;
    float content_scale;
    int pixel_width, pixel_height;
    int cell_width, cell_height;
    const char *font_path;
    const char *hinting;
} PorttyDiag;

struct PorttyBackend;
typedef struct PorttyBackend PorttyBackend;

struct PorttyBackend
{
    const char *name;
    void *data; // backend-private state

    // ── Lifecycle ──────────────────────────────────────────────
    bool (*init)(PorttyBackend *self, struct PorttyApp *app,
                 const char *title, int width, int height);
    void (*run)(PorttyBackend *self); // enters event loop; blocks
    void (*destroy)(PorttyBackend *self);

    // Called by shared code to exit the event loop
    void (*request_quit)(PorttyBackend *self);

    // ── Clipboard ──────────────────────────────────────────────
    char *(*clipboard_get)(PorttyBackend *self);
    bool (*clipboard_set)(PorttyBackend *self, const char *text);
    void (*clipboard_free)(PorttyBackend *self, char *text);

    // Async clipboard paste — write clipboard content to PTY with
    // bracketed paste. Returns true if handled asynchronously.
    // If NULL or returns false, caller falls back to synchronous
    // clipboard_get + paste_text path.
    bool (*clipboard_paste_async)(PorttyBackend *self,
                                  TerminalBackend *term, PtyContext *pty);

    // ── PTY thread ─────────────────────────────────────────────
    bool (*register_pty)(PorttyBackend *self, PtyContext *pty);
    void (*pause_pty)(PorttyBackend *self);
    void (*resume_pty)(PorttyBackend *self);

    // ── Window ─────────────────────────────────────────────────
    void (*show_window)(PorttyBackend *self);
    void (*set_window_title)(PorttyBackend *self, const char *title);
    void (*set_window_size)(PorttyBackend *self, int width, int height);
    bool (*get_drawable_size)(PorttyBackend *self, int *w, int *h);
    float (*get_display_scale)(PorttyBackend *self);
    bool (*get_display_size)(PorttyBackend *self, int *w, int *h);

    // ── OS integration ─────────────────────────────────────────
    void (*set_cursor)(PorttyBackend *self, PorttyCursor shape);
    bool (*open_url)(PorttyBackend *self, const char *url,
                     char *err, size_t errlen);
    void (*set_autoscroll)(PorttyBackend *self, bool enabled);
    bool (*spawn_new_terminal)(PorttyBackend *self);
    void (*set_working_dir)(PorttyBackend *self, const char *dir);
    const char *(*get_exe_path)(PorttyBackend *self);
    char *(*get_default_font)(PorttyBackend *self);

    // ── Notifications & link hints (backend-owned UI) ─────────
    void (*notify)(PorttyBackend *self, const char *title,
                   const char *body, PorttyNotifyLevel level);
    void (*notify_dismiss)(PorttyBackend *self);
    void (*set_link_hint)(PorttyBackend *self, const char *url, int anchor_py);
    int (*notification_hit)(PorttyBackend *self, int px, int py);
    bool (*set_notification_hover)(PorttyBackend *self, bool hovered);

    // ── Rendering ──────────────────────────────────────────────
    int (*load_fonts)(PorttyBackend *self, float size,
                      const char *name, int ft_hint_target);
    void (*draw_terminal)(PorttyBackend *self, TerminalBackend *term,
                          bool cursor_visible);
    void (*present)(PorttyBackend *self);
    void (*resize)(PorttyBackend *self, int w, int h);
    bool (*get_cell_size)(PorttyBackend *self, int *cw, int *ch);
    void (*scroll)(PorttyBackend *self, TerminalBackend *term, int delta);
    void (*reset_scroll)(PorttyBackend *self);
    int (*get_scroll_offset)(PorttyBackend *self);

    // ── Content scale ──────────────────────────────────────────
    void (*set_content_scale)(PorttyBackend *self, float scale);

    // ── Pager overlay ──────────────────────────────────────────
    void (*set_overlay)(PorttyBackend *self, TerminalBackend *overlay);
    void (*clear_overlay)(PorttyBackend *self);
    bool (*has_overlay)(PorttyBackend *self);

    // ── Offscreen rendering ────────────────────────────────────
    int (*render_to_png)(PorttyBackend *self, TerminalBackend *term,
                         const char *path);

    // ── Diagnostics ────────────────────────────────────────────
    void (*log_stats)(PorttyBackend *self);
    bool (*get_diag)(PorttyBackend *self, PorttyDiag *out);
};

#endif // PORTTY_BACKEND_H
