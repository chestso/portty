#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "backend_sokol.h"
#include "common.h"
#include "font.h"
#include "font_ft.h"
#include "font_ft_internal.h"
#include "font_resolve.h"
#ifdef _WIN32
#include "font_resolve_w32.h"
#elif defined(__APPLE__)
#include "font_resolve_ct.h"
#else
#include "font_resolve_fc.h"
#endif
#include "pager.h"
#include "png_writer.h"
#include "display_info.h"
#include "portty_app.h"
#include "portty_conf.h"
#include "portty_debug_script.h"
#include "portty_pty.h"
#include "os_compat.h"
#include "rend_sokol_atlas.h"
#include "rend_common.h"
#include "term.h"
#include "term_cfr.h"
#include "timer.h"
#include "unicode.h"
#ifdef _WIN32
#include <pthread.h>
#include <io.h>
#else
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <limits.h>
#include <math.h>
#include <coffer/coffer.h>

#ifdef _WIN32
#define CINTERFACE
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#endif

#define SOKOL_IMPL

#include <sokol/sokol_app.h>
#include <sokol/sokol_gfx.h>
#include <sokol/sokol_glue.h>
#include <sokol/sokol_log.h>
#include <sokol/sokol_time.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#endif

// Sokol backend for portty.
//
// This is a stub implementation that provides the full PorttyBackend vtable
// with minimal no-op / fallback behavior. It compiles when Sokol headers are
// available but does not yet implement real rendering, input, or the PTY
// thread. It exists so the build system can switch backends and so the
// skeleton can be filled in incrementally.

#define SOKOL_MAX_EVENTS 16

#define SOKOL_PTY_BUF_SIZE 8192
#define EMOJI_FONT_SCALE   4.0f

// Portable string builder used in place of open_memstream(), which is not
// available on Windows (MSYS2/MinGW).
typedef struct
{
    char *buf;
    size_t len;
    size_t cap;
} SokolStrBuf;

static bool sokol_strbuf_init(SokolStrBuf *sb)
{
    sb->buf = malloc(64);
    if (!sb->buf)
        return false;
    sb->buf[0] = '\0';
    sb->len = 0;
    sb->cap = 64;
    return true;
}

static bool sokol_strbuf_appendf(SokolStrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return false;
    size_t need = (size_t)n + 1;
    if (sb->len + need > sb->cap) {
        size_t new_cap = sb->cap * 2;
        while (new_cap < sb->len + need)
            new_cap *= 2;
        char *p = realloc(sb->buf, new_cap);
        if (!p)
            return false;
        sb->buf = p;
        sb->cap = new_cap;
    }
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, need, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
    return true;
}

static char *sokol_strbuf_finish(SokolStrBuf *sb)
{
    char *out = sb->buf;
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}

static void sokol_strbuf_free(SokolStrBuf *sb)
{
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

// Event codes for the poll-based timer system (mirrors SDL3 platform layer)
enum
{
    SOKOL_EVENT_CURSOR_BLINK = 1,
    SOKOL_EVENT_AUTOSCROLL_TICK,
    SOKOL_EVENT_LOTTIE_TICK,
};

// Rendering scheduler mode.  DAMAGE mode only emits GPU work when the
// terminal reports damage (or a debug/recording action forces a frame).
// FIXED_FPS mode renders at a fixed frame rate for recording automation.
typedef enum
{
    RENDER_MODE_DAMAGE = 0,
    RENDER_MODE_FIXED_FPS,
} RenderMode;

#define SOKOL_LOTTIE_CACHE_MAX 64
#define SOKOL_LOTTIE_TICK_MS   16
#define SOKOL_SIXEL_CACHE_MAX  256

typedef struct
{
    sg_image image;
    sg_view view;
    uint64_t id;
    uint32_t version;
    int w, h;
} SokolLottieCacheEntry;

typedef struct
{
    sg_image image;
    sg_view view;
    uint64_t id;
    uint32_t version;
    int w, h;
} SokolSixelCacheEntry;

typedef struct PtyDataNode
{
    struct PtyDataNode *next;
    size_t len;
    char data[];
} PtyDataNode;

typedef struct
{
    PorttyApp *app;
    PorttyBackend *self;
    PtyContext *pty;
    TerminalBackend *term;
    bool running;
    bool quit_requested;
    int cell_w, cell_h;
    float content_scale;
    char *working_dir;
    char *exe_path;
    // Renderer state
    sg_image tex;
    sg_sampler smp;
    sg_view tex_view;
    sg_pipeline pip;
    sg_buffer vbuf;
    uint8_t *pixels; // CPU RGBA pixel buffer
    int pix_w, pix_h;
    bool tex_created;
    bool pip_created;
    // Screenshot automation
    int screenshot_frames; // >0 = countdown to screenshot
    char screenshot_path[512];
    bool screenshot_saved;
    // Font
    FontBackend *font;
    FontResolveBackend *resolve;
    int font_ascent, font_descent, font_cap_height;
    char *font_path;
    char hint_name[8];
    // Scroll & fallback
    RendScrollState scroll;
    RendFallbackState fallback;
    float font_size;
    FontOptions font_options;
    // Glyph atlas
    RendSokolAtlas atlas;
    sg_pipeline glyph_pip;
    sg_buffer glyph_vbuf;
    bool glyph_pip_created;
    sg_pipeline sel_pip;
    bool sel_pip_created;
    // PTY reader thread
    pthread_t pty_thread;
    bool pty_thread_running;
#ifndef _WIN32
    int wakeup_pipe[2]; // [0]=read, [1]=write
#else
    HANDLE wakeup_event;
#endif
    pthread_mutex_t pty_queue_mtx;
    PtyDataNode *pty_queue_head;
    PtyDataNode *pty_queue_tail;
    bool pty_closed;
    bool child_exited;
    // Timer system
    TimerManager *timers;
    TimerId cursor_blink_timer;
    bool cursor_blink_visible;
    bool has_focus;
    bool linear_ok;
    PorttyCursor current_cursor;
    // Render scheduler
    RenderMode render_mode;
    double record_fps;
    double record_accumulator_ms;
    bool capture_this_frame;
    // Mouse selection state
    bool left_button_down;
    int last_mouse_x, last_mouse_y;
    int click_count;
    uint64_t last_click_time;
    int last_click_x, last_click_y;
    TimerId autoscroll_timer;
    bool pty_paused;
    bool suppress_next_char; // KEY_DOWN handled by pager — drop the paired CHAR event
    float wheel_accum_y;     // fractional mouse-wheel accumulation (matches SDL3 backend)
    // Debug script infrastructure
    PorttyDebugScript *debug_script;
    int debug_cmd_index;
    bool debug_script_done;
    bool debug_pending_screendump;
    char debug_screendump_path[512];
    bool debug_pending_verifybuf;
    int debug_verify_row, debug_verify_col_start, debug_verify_col_end;
    unsigned int debug_glyph_vbuf_gl_id; // GL buffer ID for verifybuf
    // Lottie animation rendering
    TimerId lottie_timer;
    SokolLottieCacheEntry lottie_cache[SOKOL_LOTTIE_CACHE_MAX];
    int lottie_cache_count;
    sg_pipeline lottie_pip;
    sg_buffer lottie_vbuf;
    sg_sampler lottie_sampler;
    bool lottie_pip_created;
    // Sixel image rendering
    SokolSixelCacheEntry sixel_cache[SOKOL_SIXEL_CACHE_MAX];
    int sixel_cache_count;
    sg_buffer sixel_vbuf;
    bool sixel_vbuf_created;
    // Window title state
    char *last_title;

    // Notification panel — PTY-less coffer terminal overlay
    TerminalBackend *notif_term;
    bool notif_active;
    bool notif_close_hover;
    int notif_level;
    int notif_x, notif_y;
    int notif_w, notif_h;
    int notif_rows, notif_cols;
    int notif_close_size;
    float notif_close_x, notif_close_y;
    char *notif_title;
    char *notif_body;

    // Hover link hint panel — second instance of the same panel system
    TerminalBackend *hint_term;
    bool hint_active;
    char *hint_text;
    int hint_anchor_py;
    int hint_h;
} SokolData;

static SokolData *sokol_data(PorttyBackend *self)
{
    return (SokolData *)self->data;
}

// ── Lifecycle ───────────────────────────────────────────────────────────

static bool sokol_init(PorttyBackend *self, PorttyApp *app,
                       const char *title, int width, int height)
{
    (void)title;
    (void)width;
    (void)height;
    SokolData *d = calloc(1, sizeof(*d));
    if (!d)
        return false;
    self->data = d;
    d->app = app;
    d->self = self;
    d->term = app->term;
    d->pty = app->pty;
    d->running = true;
    d->content_scale = 1.0f;
    d->cell_w = 10;
    d->cell_h = 20;
#ifndef _WIN32
    d->wakeup_pipe[0] = d->wakeup_pipe[1] = -1;
#else
    d->wakeup_event = NULL;
#endif
    pthread_mutex_init(&d->pty_queue_mtx, NULL);
    memset(&d->scroll, 0, sizeof(d->scroll));
    rend_fallback_init(&d->fallback);
    d->timers = timer_manager_create();
    d->cursor_blink_timer = TIMER_INVALID;
    d->cursor_blink_visible = true;
    d->has_focus = true;
    d->linear_ok = true;
    d->render_mode = RENDER_MODE_DAMAGE;
    d->record_fps = 0.0;
    d->record_accumulator_ms = 0.0;
    d->capture_this_frame = false;
    d->left_button_down = false;
    d->last_mouse_x = 0;
    d->last_mouse_y = 0;
    d->click_count = 0;
    d->last_click_time = 0;
    d->last_click_x = 0;
    d->last_click_y = 0;
    d->autoscroll_timer = TIMER_INVALID;
    d->lottie_timer = TIMER_INVALID;
    d->lottie_pip_created = false;
    d->lottie_cache_count = 0;
    d->pty_paused = false;

    // Cache exe path for spawn_new_terminal
    char exe_buf[PATH_MAX];
    if (os_compat_get_exe_path(exe_buf, sizeof(exe_buf)))
        d->exe_path = strdup(exe_buf);

    app->backend = self;

    stm_setup();
    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });
    if (!sg_isvalid()) {
        free(d);
        self->data = NULL;
        return false;
    }
    return true;
}

static void sokol_destroy(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;

    // Stop PTY reader thread
    if (d->pty_thread_running) {
        d->pty_thread_running = false;
#ifdef _WIN32
        if (d->wakeup_event)
            SetEvent(d->wakeup_event);
#else
        if (d->wakeup_pipe[1] >= 0) {
            char c = 1;
            (void)write(d->wakeup_pipe[1], &c, 1);
        }
#endif
        pthread_join(d->pty_thread, NULL);
        d->pty_thread_running = false;
    }
#ifndef _WIN32
    if (d->wakeup_pipe[0] >= 0)
        close(d->wakeup_pipe[0]);
    if (d->wakeup_pipe[1] >= 0)
        close(d->wakeup_pipe[1]);
#else
    if (d->wakeup_event) {
        CloseHandle(d->wakeup_event);
        d->wakeup_event = NULL;
    }
#endif

    // Free pending PTY data
    PtyDataNode *node = d->pty_queue_head;
    while (node) {
        PtyDataNode *next = node->next;
        free(node);
        node = next;
    }
    pthread_mutex_destroy(&d->pty_queue_mtx);

    // Free debug script
    portty_debug_script_free(d->debug_script);
    d->debug_script = NULL;

    if (d->cursor_blink_timer != TIMER_INVALID) {
        timer_remove(d->timers, d->cursor_blink_timer);
        d->cursor_blink_timer = TIMER_INVALID;
    }
    if (d->autoscroll_timer != TIMER_INVALID) {
        timer_remove(d->timers, d->autoscroll_timer);
        d->autoscroll_timer = TIMER_INVALID;
    }
    if (d->lottie_timer != TIMER_INVALID) {
        timer_remove(d->timers, d->lottie_timer);
        d->lottie_timer = TIMER_INVALID;
    }
    if (d->timers) {
        timer_manager_destroy(d->timers);
        d->timers = NULL;
    }

    // Destroy lottie cache and pipeline
    for (int i = 0; i < d->lottie_cache_count; i++) {
        if (d->lottie_cache[i].image.id != SG_INVALID_ID)
            sg_destroy_image(d->lottie_cache[i].image);
        if (d->lottie_cache[i].view.id != SG_INVALID_ID)
            sg_destroy_view(d->lottie_cache[i].view);
    }
    d->lottie_cache_count = 0;
    if (d->lottie_pip_created) {
        sg_destroy_pipeline(d->lottie_pip);
        sg_destroy_buffer(d->lottie_vbuf);
        sg_destroy_sampler(d->lottie_sampler);
    }

    // Destroy sixel cache and vbuf
    for (int i = 0; i < d->sixel_cache_count; i++) {
        if (d->sixel_cache[i].image.id != SG_INVALID_ID)
            sg_destroy_image(d->sixel_cache[i].image);
        if (d->sixel_cache[i].view.id != SG_INVALID_ID)
            sg_destroy_view(d->sixel_cache[i].view);
    }
    d->sixel_cache_count = 0;
    if (d->sixel_vbuf_created)
        sg_destroy_buffer(d->sixel_vbuf);

    if (d->tex_created)
        sg_destroy_image(d->tex);
    if (d->pip_created) {
        sg_destroy_pipeline(d->pip);
        sg_destroy_buffer(d->vbuf);
        sg_destroy_sampler(d->smp);
        sg_destroy_view(d->tex_view);
    }
    free(d->pixels);
    if (d->font) {
        rend_fallback_destroy(&d->fallback, d->font);
        font_destroy(d->font);
        d->font = NULL;
    }
    if (d->resolve) {
        font_resolve_destroy(d->resolve);
        d->resolve = NULL;
    }
    free(d->font_path);
    free(d->last_title);
    free(d->notif_title);
    free(d->notif_body);
    if (d->notif_term) {
        terminal_destroy(d->notif_term);
        free(d->notif_term);
        d->notif_term = NULL;
    }
    free(d->hint_text);
    if (d->hint_term) {
        terminal_destroy(d->hint_term);
        free(d->hint_term);
        d->hint_term = NULL;
    }
    sg_shutdown();
    free(d->working_dir);
    free(d->exe_path);
    free(d);
    self->data = NULL;
}

static void sokol_request_quit(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;
    d->quit_requested = true;
    sapp_request_quit();
}

// ── Clipboard ───────────────────────────────────────────────────────────

static char *sokol_clipboard_get(PorttyBackend *self)
{
    (void)self;
    const char *s = sapp_get_clipboard_string();
    return s ? strdup(s) : NULL;
}

static bool sokol_clipboard_set(PorttyBackend *self, const char *text)
{
    (void)self;
    sapp_set_clipboard_string(text);
    return true;
}

static void sokol_clipboard_free(PorttyBackend *self, char *text)
{
    (void)self;
    free(text);
}

static bool sokol_clipboard_paste_async(PorttyBackend *self,
                                        TerminalBackend *term, PtyContext *pty)
{
    (void)self;
    (void)term;
    (void)pty;
    return false;
}

// ── PTY thread ───────────────────────────────────────────────────────────

#ifdef _WIN32
static void *sokol_pty_reader_thread(void *arg)
{
    SokolData *d = (SokolData *)arg;
    char buf[SOKOL_PTY_BUF_SIZE];

    vlog("sokol_pty_reader_thread: started (W32)\n");

    HANDLE hProcess = (HANDLE)pty_get_process_handle(d->pty);

    while (d->pty_thread_running) {
        if (d->pty_paused) {
            HANDLE wait_h[2] = { d->wakeup_event, hProcess };
            DWORD wr = WaitForMultipleObjects(2, wait_h, FALSE, INFINITE);
            ResetEvent(d->wakeup_event);
            if (wr == WAIT_OBJECT_0) {
                if (!d->pty_thread_running)
                    break;
                continue;
            }
            if (wr == WAIT_OBJECT_0 + 1) {
                vlog("sokol_pty_reader_thread: child process exited\n");
                break;
            }
            break;
        }

        ssize_t n = pty_read(d->pty, buf, sizeof(buf));
        if (n > 0) {
            PtyDataNode *node = malloc(sizeof(PtyDataNode) + n);
            if (node) {
                node->next = NULL;
                node->len = (size_t)n;
                memcpy(node->data, buf, n);
                pthread_mutex_lock(&d->pty_queue_mtx);
                if (d->pty_queue_tail)
                    d->pty_queue_tail->next = node;
                else
                    d->pty_queue_head = node;
                d->pty_queue_tail = node;
                pthread_mutex_unlock(&d->pty_queue_mtx);
            }
        } else if (n == 0) {
            break;
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                vlog("sokol_pty_reader_thread: pipe closed\n");
            } else {
                vlog("sokol_pty_reader_thread: read error: %lu\n", err);
            }
            break;
        }

        if (!pty_is_running(d->pty)) {
            vlog("sokol_pty_reader_thread: child exited after read\n");
            break;
        }
    }

    pthread_mutex_lock(&d->pty_queue_mtx);
    d->pty_closed = true;
    pthread_mutex_unlock(&d->pty_queue_mtx);
    return NULL;
}
#else
static void *sokol_pty_reader_thread(void *arg)
{
    SokolData *d = (SokolData *)arg;
    int pty_fd = pty_get_master_fd(d->pty);
    int wakeup_fd = d->wakeup_pipe[0];
    char buf[SOKOL_PTY_BUF_SIZE];

    while (d->pty_thread_running) {
        struct pollfd pfds[2];
        int nfds = 0;
        pfds[nfds].fd = pty_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
        pfds[nfds].fd = wakeup_fd;
        pfds[nfds].events = POLLIN;
        nfds++;

        int ret = poll(pfds, nfds, -1);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        // Check wakeup pipe (shutdown notification)
        if (pfds[1].revents & POLLIN) {
            char c;
            (void)read(wakeup_fd, &c, 1);
            // re-loop, re-check pty_thread_running
        }

        // Check PTY fd
        if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(pty_fd, buf, sizeof(buf));
            if (n > 0) {
                PtyDataNode *node = malloc(sizeof(PtyDataNode) + n);
                if (node) {
                    node->next = NULL;
                    node->len = (size_t)n;
                    memcpy(node->data, buf, n);
                    pthread_mutex_lock(&d->pty_queue_mtx);
                    if (d->pty_queue_tail)
                        d->pty_queue_tail->next = node;
                    else
                        d->pty_queue_head = node;
                    d->pty_queue_tail = node;
                    pthread_mutex_unlock(&d->pty_queue_mtx);
                }
            } else if (n == 0) {
                break; // EOF
            } else if (errno != EAGAIN && errno != EINTR) {
                break;
            }
        }
    }

    pthread_mutex_lock(&d->pty_queue_mtx);
    d->pty_closed = true;
    pthread_mutex_unlock(&d->pty_queue_mtx);
    return NULL;
}
#endif

static void sokol_drain_pty(SokolData *d)
{
    if (!d->pty)
        return;

    // When PTY is paused (e.g. during selection), keep draining the queue
    // from the reader thread so it doesn't block, but don't process the
    // data — that would scroll the viewport and disrupt the selection.
    if (d->pty_paused) {
        PtyDataNode *head;
        pthread_mutex_lock(&d->pty_queue_mtx);
        head = d->pty_queue_head;
        d->pty_queue_head = NULL;
        d->pty_queue_tail = NULL;
        pthread_mutex_unlock(&d->pty_queue_mtx);
        while (head) {
            PtyDataNode *next = head->next;
            free(head);
            head = next;
        }
        return;
    }

    PtyDataNode *head;
    pthread_mutex_lock(&d->pty_queue_mtx);
    head = d->pty_queue_head;
    d->pty_queue_head = NULL;
    d->pty_queue_tail = NULL;
    pthread_mutex_unlock(&d->pty_queue_mtx);

    while (head) {
        PtyDataNode *next = head->next;
        terminal_consume_pushed_rows(d->term);
        terminal_process_input(d->term, head->data, head->len);
        int pushed = terminal_consume_pushed_rows(d->term);
        if (pushed > 0 && rend_get_scroll_offset(&d->scroll) > 0)
            rend_scroll(&d->scroll, d->term, pushed);
        free(head);
        head = next;
    }

    terminal_flush_damage(d->term);

    // Update window title after PTY data, mirroring SDL3 backend.
    d->self->set_window_title(d->self, terminal_get_title(d->term));

    // Start lottie tick timer if animations are active
    if (d->lottie_timer == TIMER_INVALID && terminal_lottie_count(d->term) > 0) {
        d->lottie_timer = timer_add(d->timers, SOKOL_LOTTIE_TICK_MS,
                                    SOKOL_EVENT_LOTTIE_TICK, NULL);
    }

    if (d->pty_closed && !d->child_exited) {
        d->child_exited = true;
        portty_app_handle_pty_closed(d->app);
        vlog("PTY closed, requesting quit\n");
        d->quit_requested = true;
    }
}

static bool sokol_register_pty(PorttyBackend *self, PtyContext *pty)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return false;
    d->pty = pty;

#ifdef _WIN32
    d->wakeup_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!d->wakeup_event)
        return false;
#else
    if (pipe(d->wakeup_pipe) != 0) {
        d->wakeup_pipe[0] = d->wakeup_pipe[1] = -1;
        return false;
    }
#endif

    d->pty_thread_running = true;
    int rc = pthread_create(&d->pty_thread, NULL, sokol_pty_reader_thread, d);
    if (rc != 0) {
        d->pty_thread_running = false;
#ifdef _WIN32
        CloseHandle(d->wakeup_event);
        d->wakeup_event = NULL;
#else
        close(d->wakeup_pipe[0]);
        close(d->wakeup_pipe[1]);
        d->wakeup_pipe[0] = d->wakeup_pipe[1] = -1;
#endif
        return false;
    }
    vlog("sokol_register_pty: reader thread started\n");
    return true;
}

static void sokol_pause_pty(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (d)
        d->pty_paused = true;
}

static void sokol_resume_pty(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (d)
        d->pty_paused = false;
}

// ── Window ───────────────────────────────────────────────────────────────

static void sokol_set_window_title(PorttyBackend *self, const char *title)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;
    // Title dedup
    if (d->last_title && title && strcmp(d->last_title, title) == 0)
        return;
    if (!d->last_title && !title)
        return;
    free(d->last_title);
    d->last_title = title ? strdup(title) : NULL;
    sapp_set_window_title(title ? title : "portty");
    vlog("Window title set to: %s\n", title ? title : "(default)");
}

static void sokol_set_window_size(PorttyBackend *self, int width, int height)
{
    (void)self;
    if (width <= 0 || height <= 0)
        return;
#if !defined(_WIN32) && !defined(__APPLE__)
    // Sokol has no sapp_set_window_size; use X11 directly on Linux.
    Display *disp = (Display *)sapp_x11_get_display();
    Window win = (Window)(intptr_t)sapp_x11_get_window();
    if (disp && win)
        XResizeWindow(disp, win, (unsigned)width, (unsigned)height);
#endif
}

static bool sokol_get_drawable_size(PorttyBackend *self, int *w, int *h)
{
    (void)self;
    if (w)
        *w = sapp_width();
    if (h)
        *h = sapp_height();
    return true;
}

static float sokol_get_display_scale(PorttyBackend *self)
{
    (void)self;
    return sapp_dpi_scale();
}

static bool sokol_get_display_size(PorttyBackend *self, int *w, int *h)
{
    (void)self;
    (void)w;
    (void)h;
    return false;
}

// ── OS integration ───────────────────────────────────────────────────────

static void sokol_set_cursor(PorttyBackend *self, PorttyCursor shape)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;
    PorttyCursor cursor = (shape == PORTTY_CURSOR_POINTER) ? PORTTY_CURSOR_POINTER
                                                           : PORTTY_CURSOR_TEXT;
    if (d->current_cursor == cursor)
        return;
    sapp_set_mouse_cursor(cursor == PORTTY_CURSOR_POINTER
                              ? SAPP_MOUSECURSOR_POINTING_HAND
                              : SAPP_MOUSECURSOR_IBEAM);
    d->current_cursor = cursor;
}

static bool sokol_open_url(PorttyBackend *self, const char *url,
                           char *err, size_t errlen)
{
    (void)self;
    return os_compat_open_url(url, err, errlen);
}

static void sokol_set_autoscroll(PorttyBackend *self, bool enabled)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;
    if (enabled) {
        if (d->autoscroll_timer == TIMER_INVALID)
            d->autoscroll_timer = timer_add(d->timers, AUTOSCROLL_INTERVAL_MS,
                                            SOKOL_EVENT_AUTOSCROLL_TICK, NULL);
    } else {
        if (d->autoscroll_timer != TIMER_INVALID) {
            timer_remove(d->timers, d->autoscroll_timer);
            d->autoscroll_timer = TIMER_INVALID;
        }
    }
}

static bool sokol_spawn_new_terminal(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (!d || !d->exe_path)
        return false;

    char cwd_path[PATH_MAX] = "";
    if (d->working_dir) {
        snprintf(cwd_path, sizeof(cwd_path), "%s", d->working_dir);
    }
#ifndef _WIN32
    else if (d->pty) {
        pty_get_child_cwd(d->pty, cwd_path, sizeof(cwd_path));
    }
#endif

    return os_compat_spawn_process(d->exe_path, cwd_path);
}

static void sokol_set_working_dir(PorttyBackend *self, const char *dir)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;
    free(d->working_dir);
    d->working_dir = dir ? strdup(dir) : NULL;
}

static const char *sokol_get_exe_path(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    return d ? d->exe_path : NULL;
}

static char *sokol_get_default_font(PorttyBackend *self)
{
    (void)self;
    return NULL;
}

// ── Notifications & link hints ────────────────────────────────────────────

static void sokol_notif_rebuild(SokolData *d);

static void sokol_notify(PorttyBackend *self, const char *title,
                         const char *body, PorttyNotifyLevel level)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;
    free(d->notif_title);
    free(d->notif_body);
    d->notif_title = title ? strdup(title) : NULL;
    d->notif_body = body ? strdup(body) : NULL;
    d->notif_level = (int)level;
    d->notif_close_hover = false;
    sokol_notif_rebuild(d);
    d->notif_active = (d->notif_term != NULL);
    if (d->notif_active && d->term)
        terminal_mark_dirty(d->term);
}

static void sokol_notify_dismiss(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;
    if (d->notif_term) {
        terminal_destroy(d->notif_term);
        free(d->notif_term);
        d->notif_term = NULL;
    }
    d->notif_active = false;
    d->notif_close_hover = false;
    d->notif_h = 0;
    if (d->term)
        terminal_mark_dirty(d->term);
}

static void sokol_notif_rebuild(SokolData *d);

static char *sokol_hint_build_ansi(SokolData *d)
{
    SokolStrBuf sb;
    if (!sokol_strbuf_init(&sb))
        return NULL;

    const char *panel_bg = "\x1b[48;2;38;38;44m";
    if (!sokol_strbuf_appendf(&sb, "%s\x1b[38;2;210;210;220m%s\x1b[39m",
                              panel_bg, d->hint_text ? d->hint_text : "")) {
        sokol_strbuf_free(&sb);
        return NULL;
    }
    return sokol_strbuf_finish(&sb);
}

static void sokol_hint_rebuild(SokolData *d)
{
    if (d->hint_term) {
        terminal_destroy(d->hint_term);
        free(d->hint_term);
        d->hint_term = NULL;
    }
    d->hint_active = false;
    d->hint_h = 0;

    if (!d->hint_text || !d->hint_text[0] || !d->font || d->cell_w <= 0 || d->cell_h <= 0)
        return;

    int win_w = (int)sapp_width();
    if (win_w <= 0)
        return;

    float scale = d->content_scale > 0 ? d->content_scale : 1.0f;
    int pad = (int)(10.0f * scale + 0.5f);
    int max_cols = win_w > 2 * pad ? (win_w - 2 * pad) / d->cell_w : 1;
    if (max_cols < 1)
        max_cols = 1;

    char *ansi = sokol_hint_build_ansi(d);
    if (!ansi)
        return;

    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.cols = max_cols;
    cfg.rows = 32;
    cfg.cell_w_px = d->cell_w;
    cfg.cell_h_px = d->cell_h;
    d->hint_term = term_cfr_new(&cfg);
    if (!d->hint_term) {
        free(ansi);
        return;
    }

    terminal_process_input(d->hint_term, ansi, strlen(ansi));
    free(ansi);

    TerminalPos pos = terminal_get_cursor_pos(d->hint_term);
    int n_rows = pos.row + 1;
    if (n_rows < 1)
        n_rows = 1;

    d->hint_h = pad * 2 + n_rows * d->cell_h;
    d->hint_active = true;
}

static int sokol_hint_compute_y(SokolData *d, int anchor_py, int win_h)
{
    if (win_h <= 0 || d->hint_h <= 0)
        return 0;

    bool top_half = anchor_py + d->cell_h / 2 < win_h / 2;
    int y;
    if (top_half)
        y = anchor_py + d->cell_h;
    else
        y = anchor_py - d->hint_h;

    int notif_bottom = d->notif_active && d->notif_h > 0 ? d->notif_h : 0;

    if (y < notif_bottom && y + d->hint_h > 0) {
        int flipped = top_half ? anchor_py - d->hint_h : anchor_py + d->cell_h;
        if (flipped >= notif_bottom && flipped + d->hint_h <= win_h)
            y = flipped;
        else if (flipped + d->hint_h <= win_h && notif_bottom == 0)
            y = flipped;
    }

    if (y < notif_bottom)
        y = notif_bottom;
    if (y + d->hint_h > win_h)
        y = win_h - d->hint_h;
    if (y < 0)
        y = 0;

    return y;
}

static void sokol_set_link_hint(PorttyBackend *self, const char *url,
                                int anchor_py)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;

    if (!url || !url[0]) {
        if (d->hint_term) {
            terminal_destroy(d->hint_term);
            free(d->hint_term);
            d->hint_term = NULL;
        }
        free(d->hint_text);
        d->hint_text = NULL;
        d->hint_active = false;
        d->hint_h = 0;
        if (d->term)
            terminal_mark_dirty(d->term);
        return;
    }

    if (d->hint_active && d->hint_text && strcmp(d->hint_text, url) == 0) {
        d->hint_anchor_py = anchor_py;
        if (d->term)
            terminal_mark_dirty(d->term);
        return;
    }

    free(d->hint_text);
    d->hint_text = strdup(url);
    d->hint_anchor_py = anchor_py;
    sokol_hint_rebuild(d);
    if (d->term)
        terminal_mark_dirty(d->term);
}

static int sokol_notification_hit(PorttyBackend *self, int px, int py)
{
    SokolData *d = sokol_data(self);
    if (!d || !d->notif_active || d->notif_h <= 0)
        return 0;
    if (px < d->notif_x || px >= d->notif_x + d->notif_w ||
        py < d->notif_y || py >= d->notif_y + d->notif_h)
        return 0;
    if (px >= d->notif_close_x && px < d->notif_close_x + d->notif_close_size &&
        py >= d->notif_close_y && py < d->notif_close_y + d->notif_close_size)
        return 2;
    return 1;
}

static bool sokol_set_notification_hover(PorttyBackend *self, bool hovered)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return false;
    if (!d->notif_active)
        hovered = false;
    if (d->notif_close_hover == hovered)
        return false;
    d->notif_close_hover = hovered;
    return true;
}

// ── Rendering ────────────────────────────────────────────────────────────

#define SELECTION_COLOR_R 0x5A
#define SELECTION_COLOR_G 0x60
#define SELECTION_COLOR_B 0x7A
#define SELECTION_COLOR_A 220

// Cursor color: Charm signature purple (RGBA) — matches SDL3 renderer
#define CURSOR_COLOR_R 0x6B
#define CURSOR_COLOR_G 0x50
#define CURSOR_COLOR_B 0xFF
#define CURSOR_COLOR_A 0xFF

// Underline color: matches cursor color (same as SDL3 renderer)
#define UNDERLINE_COLOR_R CURSOR_COLOR_R
#define UNDERLINE_COLOR_G CURSOR_COLOR_G
#define UNDERLINE_COLOR_B CURSOR_COLOR_B
#define UNDERLINE_COLOR_A 255

// Default background color — used both as the render pass clear color and
// as the bg for cells with bg.is_default. Keeping them in sync ensures
// empty cells (skipped, show clear color) and default-bg cells (emit a
// bg quad) look identical.
#define DEF_BG_R 0x00
#define DEF_BG_G 0x00
#define DEF_BG_B 0x00

// Vertex format: position (xy) + texcoord (uv) + fg color (rgba) + bg color (rgba)
// 2 floats + 2 floats + 4 ubytes + 4 ubytes = 20 bytes per vertex
typedef struct
{
    float x, y;    // pixel position
    float u, v;    // atlas texcoord
    uint8_t fg[4]; // foreground color (RGBA)
    uint8_t bg[4]; // background color (RGBA)
} GlyphVertex;

#define SOKOL_MAX_COLS          400
#define SOKOL_MAX_ROWS          120
#define SOKOL_MAX_VERTICES      (SOKOL_MAX_COLS * SOKOL_MAX_ROWS * 12)
#define SOKOL_MAX_DECO_VERTICES 200000

// File-scope vertex arrays (moved from static-local in sokol_draw_terminal
// so debug dump functions can access them after the render pass).
static GlyphVertex s_frame_verts[SOKOL_MAX_VERTICES];
static int s_frame_vert_count;
static int s_vert_index[SOKOL_MAX_ROWS][SOKOL_MAX_COLS];
static GlyphVertex s_glyph_verts[SOKOL_MAX_VERTICES];
static GlyphVertex s_deco_verts[SOKOL_MAX_DECO_VERTICES];
static int s_deco_vert_count;
static bool s_deco_overflow_warned;

#define SOKOL_MAX_LOTTIE_VERTICES 4096

static GlyphVertex s_lottie_verts[SOKOL_MAX_LOTTIE_VERTICES];

#define SOKOL_MAX_SIXEL_VERTICES 4096

static void sokol_ensure_lottie_pipeline(SokolData *d)
{
    if (d->lottie_pip_created)
        return;

    static const char *vs_src =
        "#version 410\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(location=1) in vec2 uv;\n"
        "layout(location=2) in vec4 fg;\n"
        "out vec2 v_uv;\n"
        "out float v_opacity;\n"
        "uniform vec2 u_resolution;\n"
        "void main() {\n"
        "  vec2 clip = vec2(pos.x / u_resolution.x * 2.0 - 1.0,\n"
        "                   1.0 - pos.y / u_resolution.y * 2.0);\n"
        "  gl_Position = vec4(clip, 0.0, 1.0);\n"
        "  v_uv = uv;\n"
        "  v_opacity = fg.r;\n"
        "}\n";
    static const char *fs_src =
        "#version 410\n"
        "in vec2 v_uv;\n"
        "in float v_opacity;\n"
        "out vec4 frag_color;\n"
        "uniform sampler2D lottie_tex;\n"
        "vec3 srgb_to_linear(vec3 c) {\n"
        "  return mix(pow((c + vec3(0.055)) / vec3(1.055), vec3(2.4)),\n"
        "             c / 12.92,\n"
        "             lessThanEqual(c, vec3(0.04045)));\n"
        "}\n"
        "void main() {\n"
        "  vec4 texel = texture(lottie_tex, v_uv);\n"
        "  float alpha = texel.a * v_opacity;\n"
        "  frag_color = vec4(srgb_to_linear(texel.rgb), alpha);\n"
        "}\n";

    sg_shader shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func.source = vs_src,
        .fragment_func.source = fs_src,
        .attrs[0].glsl_name = "pos",
        .attrs[1].glsl_name = "uv",
        .attrs[2].glsl_name = "fg",
        .uniform_blocks[0] = {
            .stage = SG_SHADERSTAGE_VERTEX,
            .size = sizeof(float) * 2,
            .glsl_uniforms = {
                [0] = { .glsl_name = "u_resolution", .type = SG_UNIFORMTYPE_FLOAT2 },
            },
        },
        .views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT,
        .views[0].texture.image_type = SG_IMAGETYPE_2D,
        .views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT,
        .samplers[0].stage = SG_SHADERSTAGE_FRAGMENT,
        .samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING,
        .texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT,
        .texture_sampler_pairs[0].view_slot = 0,
        .texture_sampler_pairs[0].sampler_slot = 0,
        .texture_sampler_pairs[0].glsl_name = "lottie_tex",
        .label = "sokol-lottie-shader",
    });

    d->lottie_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .size = SOKOL_MAX_LOTTIE_VERTICES * sizeof(GlyphVertex),
        .usage.dynamic_update = true,
        .label = "sokol-lottie-vbuf",
    });

    d->lottie_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .layout = {
            .buffers[0].stride = sizeof(GlyphVertex),
            .attrs = {
                [0] = { .offset = offsetof(GlyphVertex, x), .format = SG_VERTEXFORMAT_FLOAT2 },
                [1] = { .offset = offsetof(GlyphVertex, u), .format = SG_VERTEXFORMAT_FLOAT2 },
                [2] = { .offset = offsetof(GlyphVertex, fg), .format = SG_VERTEXFORMAT_UBYTE4N },
            },
        },
        .colors[0] = {
            .pixel_format = d->linear_ok ? SG_PIXELFORMAT_SRGB8A8 : SG_PIXELFORMAT_RGBA8,
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .src_factor_alpha = SG_BLENDFACTOR_ONE,
                .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            },
        },
        .label = "sokol-lottie-pipeline",
    });

    d->lottie_sampler = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .label = "sokol-lottie-sampler",
    });

    d->lottie_pip_created = true;
}

static void lottie_cache_reconcile(SokolData *d, const CfrLottie *anims, int count)
{
    for (int i = 0; i < d->lottie_cache_count;) {
        bool live = false;
        for (int j = 0; j < count; j++) {
            if (anims[j].id == d->lottie_cache[i].id) {
                live = true;
                break;
            }
        }
        if (live) {
            i++;
        } else {
            if (d->lottie_cache[i].image.id != SG_INVALID_ID)
                sg_destroy_image(d->lottie_cache[i].image);
            if (d->lottie_cache[i].view.id != SG_INVALID_ID)
                sg_destroy_view(d->lottie_cache[i].view);
            d->lottie_cache[i] = d->lottie_cache[--d->lottie_cache_count];
        }
    }
}

static int lottie_get_texture(SokolData *d, const CfrLottie *anim)
{
    for (int i = 0; i < d->lottie_cache_count; i++) {
        if (d->lottie_cache[i].id != anim->id)
            continue;
        if (d->lottie_cache[i].w != anim->canvas_w ||
            d->lottie_cache[i].h != anim->canvas_h) {
            // Size changed — recreate
            if (d->lottie_cache[i].image.id != SG_INVALID_ID)
                sg_destroy_image(d->lottie_cache[i].image);
            if (d->lottie_cache[i].view.id != SG_INVALID_ID)
                sg_destroy_view(d->lottie_cache[i].view);
            d->lottie_cache[i].image.id = SG_INVALID_ID;
            d->lottie_cache[i].view.id = SG_INVALID_ID;
        } else if (d->lottie_cache[i].version != anim->version) {
            // Version changed — update pixels
            sg_update_image(d->lottie_cache[i].image, &(sg_image_data){
                                                          .mip_levels[0] = {
                                                              .ptr = (void *)anim->rgba,
                                                              .size = (size_t)anim->canvas_w * anim->canvas_h * 4,
                                                          },
                                                      });
            d->lottie_cache[i].version = anim->version;
            return i;
        } else {
            return i;
        }
        // Re-create image (size changed or first creation)
        d->lottie_cache[i].image = sg_make_image(&(sg_image_desc){
            .width = anim->canvas_w,
            .height = anim->canvas_h,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .usage.dynamic_update = true,
            .label = "sokol-lottie",
        });
        d->lottie_cache[i].view = sg_make_view(&(sg_view_desc){
            .texture.image = d->lottie_cache[i].image,
            .label = "sokol-lottie-view",
        });
        sg_update_image(d->lottie_cache[i].image, &(sg_image_data){
                                                      .mip_levels[0] = {
                                                          .ptr = (void *)anim->rgba,
                                                          .size = (size_t)anim->canvas_w * anim->canvas_h * 4,
                                                      },
                                                  });
        d->lottie_cache[i].version = anim->version;
        d->lottie_cache[i].w = anim->canvas_w;
        d->lottie_cache[i].h = anim->canvas_h;
        return i;
    }

    // New cache entry
    if (d->lottie_cache_count >= SOKOL_LOTTIE_CACHE_MAX)
        return -1;

    sg_image img = sg_make_image(&(sg_image_desc){
        .width = anim->canvas_w,
        .height = anim->canvas_h,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage.dynamic_update = true,
        .label = "sokol-lottie",
    });
    sg_update_image(img, &(sg_image_data){
                             .mip_levels[0] = {
                                 .ptr = (void *)anim->rgba,
                                 .size = (size_t)anim->canvas_w * anim->canvas_h * 4,
                             },
                         });
    sg_view view = sg_make_view(&(sg_view_desc){
        .texture.image = img,
        .label = "sokol-lottie-view",
    });
    int n = d->lottie_cache_count++;
    d->lottie_cache[n].image = img;
    d->lottie_cache[n].view = view;
    d->lottie_cache[n].id = anim->id;
    d->lottie_cache[n].version = anim->version;
    d->lottie_cache[n].w = anim->canvas_w;
    d->lottie_cache[n].h = anim->canvas_h;
    return n;
}

// Build lottie verts for one layer AND draw them per-animation in one pass.
// Returns the number of vertices built (for offset tracking by the caller).
static int sokol_render_lottie_layer(SokolData *d, TerminalBackend *term,
                                     uint8_t target_layer, int vert_offset)
{
    int anim_count = 0;
    const CfrLottie *anims = terminal_get_lotties(term, &anim_count);
    if (anim_count == 0)
        return 0;

    int cell_w = d->cell_w;
    int cell_h = d->cell_h;
    float scale = d->content_scale > 0.0f ? d->content_scale : 1.0f;
    int win_w = (int)sapp_width();
    int win_h = (int)sapp_height();
    int scroll_offset = d->scroll.scroll_offset;
    float uniforms[2] = { (float)win_w, (float)win_h };

    int vert_base = vert_offset;

    for (int i = 0; i < anim_count; i++) {
        const CfrLottie *anim = &anims[i];
        int pl_count = 0;
        const CfrLottiePlacement *pls =
            terminal_get_lottie_placements(term, anim->id, &pl_count);

        int scaled_canvas_w = logical_to_physical(anim->canvas_w, scale);
        int scaled_canvas_h = logical_to_physical(anim->canvas_h, scale);
        int anim_vert_count = 0;

        for (int j = 0; j < pl_count; j++) {
            const CfrLottiePlacement *pl = &pls[j];
            if (pl->layer != target_layer)
                continue;

            int screen_row = pl->row + scroll_offset;
            int px = pl->col * cell_w;
            int py = screen_row * cell_h;
            int box_w = pl->cols * cell_w;
            int box_h = pl->rows * cell_h;

            if (py + box_h <= 0 || py >= win_h)
                continue;
            if (px + box_w <= 0 || px >= win_w)
                continue;
            if (vert_base + anim_vert_count + 6 > SOKOL_MAX_LOTTIE_VERTICES)
                break;

            int off_x = (box_w - scaled_canvas_w) / 2;
            int off_y = (box_h - scaled_canvas_h) / 2;
            float x0 = (float)(px + off_x);
            float y0 = (float)(py + off_y);
            float x1 = x0 + (float)scaled_canvas_w;
            float y1 = y0 + (float)scaled_canvas_h;

            uint8_t op = (uint8_t)pl->opacity_x256;
            uint8_t op_color[4] = { op, op, op, op };

            GlyphVertex *q = &s_lottie_verts[vert_base + anim_vert_count];
            q[0] = (GlyphVertex){ x0, y0, 0.0f, 0.0f, { op_color[0], op_color[1], op_color[2], op_color[3] }, { 0, 0, 0, 0 } };
            q[1] = (GlyphVertex){ x1, y0, 1.0f, 0.0f, { op_color[0], op_color[1], op_color[2], op_color[3] }, { 0, 0, 0, 0 } };
            q[2] = (GlyphVertex){ x1, y1, 1.0f, 1.0f, { op_color[0], op_color[1], op_color[2], op_color[3] }, { 0, 0, 0, 0 } };
            q[3] = (GlyphVertex){ x0, y0, 0.0f, 0.0f, { op_color[0], op_color[1], op_color[2], op_color[3] }, { 0, 0, 0, 0 } };
            q[4] = (GlyphVertex){ x1, y1, 1.0f, 1.0f, { op_color[0], op_color[1], op_color[2], op_color[3] }, { 0, 0, 0, 0 } };
            q[5] = (GlyphVertex){ x0, y1, 0.0f, 1.0f, { op_color[0], op_color[1], op_color[2], op_color[3] }, { 0, 0, 0, 0 } };
            anim_vert_count += 6;
        }

        if (anim_vert_count > 0) {
            int cache_idx = lottie_get_texture(d, anim);
            if (cache_idx >= 0) {
                sg_apply_pipeline(d->lottie_pip);
                sg_apply_bindings(&(sg_bindings){
                    .vertex_buffers[0] = d->lottie_vbuf,
                    .views[0] = d->lottie_cache[cache_idx].view,
                    .samplers[0] = d->lottie_sampler,
                });
                sg_apply_uniforms(0, &SG_RANGE(uniforms));
                sg_draw(vert_base, anim_vert_count, 1);
            }
            vert_base += anim_vert_count;
        }
    }

    return vert_base - vert_offset;
}

// --- Sixel image cache and rendering ---

static void sixel_cache_reconcile(SokolData *d, const CfrSixel *imgs, int count)
{
    for (int i = 0; i < d->sixel_cache_count;) {
        bool live = false;
        for (int j = 0; j < count; j++) {
            if (imgs[j].id == d->sixel_cache[i].id) {
                live = true;
                break;
            }
        }
        if (live) {
            i++;
        } else {
            if (d->sixel_cache[i].image.id != SG_INVALID_ID)
                sg_destroy_image(d->sixel_cache[i].image);
            if (d->sixel_cache[i].view.id != SG_INVALID_ID)
                sg_destroy_view(d->sixel_cache[i].view);
            d->sixel_cache[i] = d->sixel_cache[--d->sixel_cache_count];
        }
    }
}

static int sixel_get_texture(SokolData *d, const CfrSixel *img)
{
    for (int i = 0; i < d->sixel_cache_count; i++) {
        if (d->sixel_cache[i].id != img->id)
            continue;
        if (d->sixel_cache[i].w != img->width_px ||
            d->sixel_cache[i].h != img->height_px) {
            if (d->sixel_cache[i].image.id != SG_INVALID_ID)
                sg_destroy_image(d->sixel_cache[i].image);
            if (d->sixel_cache[i].view.id != SG_INVALID_ID)
                sg_destroy_view(d->sixel_cache[i].view);
            d->sixel_cache[i].image.id = SG_INVALID_ID;
            d->sixel_cache[i].view.id = SG_INVALID_ID;
        } else if (d->sixel_cache[i].version != img->version) {
            sg_update_image(d->sixel_cache[i].image, &(sg_image_data){
                                                         .mip_levels[0] = {
                                                             .ptr = (void *)img->rgba,
                                                             .size = (size_t)img->width_px * img->height_px * 4,
                                                         },
                                                     });
            d->sixel_cache[i].version = img->version;
            return i;
        } else {
            return i;
        }
        d->sixel_cache[i].image = sg_make_image(&(sg_image_desc){
            .width = img->width_px,
            .height = img->height_px,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .usage.dynamic_update = true,
            .label = "sokol-sixel",
        });
        d->sixel_cache[i].view = sg_make_view(&(sg_view_desc){
            .texture.image = d->sixel_cache[i].image,
            .label = "sokol-sixel-view",
        });
        sg_update_image(d->sixel_cache[i].image, &(sg_image_data){
                                                     .mip_levels[0] = {
                                                         .ptr = (void *)img->rgba,
                                                         .size = (size_t)img->width_px * img->height_px * 4,
                                                     },
                                                 });
        d->sixel_cache[i].version = img->version;
        d->sixel_cache[i].w = img->width_px;
        d->sixel_cache[i].h = img->height_px;
        return i;
    }

    if (d->sixel_cache_count >= SOKOL_SIXEL_CACHE_MAX)
        return -1;

    sg_image img_obj = sg_make_image(&(sg_image_desc){
        .width = img->width_px,
        .height = img->height_px,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage.dynamic_update = true,
        .label = "sokol-sixel",
    });
    sg_update_image(img_obj, &(sg_image_data){
                                 .mip_levels[0] = {
                                     .ptr = (void *)img->rgba,
                                     .size = (size_t)img->width_px * img->height_px * 4,
                                 },
                             });
    sg_view view = sg_make_view(&(sg_view_desc){
        .texture.image = img_obj,
        .label = "sokol-sixel-view",
    });
    int n = d->sixel_cache_count++;
    d->sixel_cache[n].image = img_obj;
    d->sixel_cache[n].view = view;
    d->sixel_cache[n].id = img->id;
    d->sixel_cache[n].version = img->version;
    d->sixel_cache[n].w = img->width_px;
    d->sixel_cache[n].h = img->height_px;
    return n;
}

static void sokol_ensure_sixel_vbuf(SokolData *d)
{
    if (d->sixel_vbuf_created)
        return;
    d->sixel_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .size = SOKOL_MAX_SIXEL_VERTICES * sizeof(GlyphVertex),
        .usage.dynamic_update = true,
        .label = "sokol-sixel-vbuf",
    });
    d->sixel_vbuf_created = true;
}

static void sokol_render_sixel_images(SokolData *d, TerminalBackend *term)
{
    int count = 0;
    const CfrSixel *imgs = terminal_get_sixels(term, &count);
    sixel_cache_reconcile(d, imgs, count);
    if (count == 0)
        return;

    int cell_w = d->cell_w;
    int cell_h = d->cell_h;
    float scale = d->content_scale > 0.0f ? d->content_scale : 1.0f;
    int win_w = (int)sapp_width();
    int win_h = (int)sapp_height();
    int scroll_offset = d->scroll.scroll_offset;
    float uniforms[2] = { (float)win_w, (float)win_h };
    uint8_t full_op[4] = { 255, 255, 255, 255 };

    static GlyphVertex sixel_verts[SOKOL_MAX_SIXEL_VERTICES];
    int vert_count = 0;

    // Pass 1: build all vertices and ensure textures are cached
    for (int i = 0; i < count; i++) {
        const CfrSixel *img = &imgs[i];

        int screen_row = img->row + scroll_offset;
        int px = img->col * cell_w;
        int py = screen_row * cell_h;
        int scaled_w = logical_to_physical(img->width_px, scale);
        int scaled_h = logical_to_physical(img->height_px, scale);

        if (py + scaled_h <= 0 || py >= win_h)
            continue;
        if (px + scaled_w <= 0 || px >= win_w)
            continue;
        if (vert_count + 6 > SOKOL_MAX_SIXEL_VERTICES)
            break;

        float x0 = (float)px;
        float y0 = (float)py;
        float x1 = x0 + (float)scaled_w;
        float y1 = y0 + (float)scaled_h;

        GlyphVertex *q = &sixel_verts[vert_count];
        q[0] = (GlyphVertex){ x0, y0, 0.0f, 0.0f, { full_op[0], full_op[1], full_op[2], full_op[3] }, { 0, 0, 0, 0 } };
        q[1] = (GlyphVertex){ x1, y0, 1.0f, 0.0f, { full_op[0], full_op[1], full_op[2], full_op[3] }, { 0, 0, 0, 0 } };
        q[2] = (GlyphVertex){ x1, y1, 1.0f, 1.0f, { full_op[0], full_op[1], full_op[2], full_op[3] }, { 0, 0, 0, 0 } };
        q[3] = (GlyphVertex){ x0, y0, 0.0f, 0.0f, { full_op[0], full_op[1], full_op[2], full_op[3] }, { 0, 0, 0, 0 } };
        q[4] = (GlyphVertex){ x1, y1, 1.0f, 1.0f, { full_op[0], full_op[1], full_op[2], full_op[3] }, { 0, 0, 0, 0 } };
        q[5] = (GlyphVertex){ x0, y1, 0.0f, 1.0f, { full_op[0], full_op[1], full_op[2], full_op[3] }, { 0, 0, 0, 0 } };

        vert_count += 6;
    }

    if (vert_count == 0)
        return;

    // Upload vertex buffer once
    sg_update_buffer(d->sixel_vbuf, &(sg_range){
                                        .ptr = sixel_verts,
                                        .size = (size_t)vert_count * sizeof(GlyphVertex),
                                    });

    // Pass 2: draw each image's 6 verts with its own texture
    int vert_offset = 0;
    for (int i = 0; i < count; i++) {
        const CfrSixel *img = &imgs[i];

        int screen_row = img->row + scroll_offset;
        int px = img->col * cell_w;
        int py = screen_row * cell_h;
        int scaled_w = logical_to_physical(img->width_px, scale);
        int scaled_h = logical_to_physical(img->height_px, scale);

        if (py + scaled_h <= 0 || py >= win_h)
            continue;
        if (px + scaled_w <= 0 || px >= win_w)
            continue;
        if (vert_offset + 6 > vert_count)
            break;

        int cache_idx = sixel_get_texture(d, img);
        if (cache_idx >= 0 && d->lottie_pip_created) {
            sg_apply_pipeline(d->lottie_pip);
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = d->sixel_vbuf,
                .views[0] = d->sixel_cache[cache_idx].view,
                .samplers[0] = d->lottie_sampler,
            });
            sg_apply_uniforms(0, &SG_RANGE(uniforms));
            sg_draw(vert_offset, 6, 1);
        }
        vert_offset += 6;
    }
}

static void sokol_ensure_glyph_pipeline(SokolData *d)
{
    if (d->glyph_pip_created)
        return;

    static const char *vs_src =
        "#version 410\n"
        "layout(location=0) in vec2 pos;\n"
        "layout(location=1) in vec2 uv;\n"
        "layout(location=2) in vec4 fg;\n"
        "layout(location=3) in vec4 bg;\n"
        "out vec2 v_uv;\n"
        "out vec4 v_fg;\n"
        "out vec4 v_bg;\n"
        "out vec2 v_cell_size;\n"
        "uniform vec2 u_resolution;\n"
        "uniform vec2 u_cell_size;\n"
        "void main() {\n"
        "  vec2 clip = vec2(pos.x / u_resolution.x * 2.0 - 1.0,\n"
        "                   1.0 - pos.y / u_resolution.y * 2.0);\n"
        "  gl_Position = vec4(clip, 0.0, 1.0);\n"
        "  v_uv = uv;\n"
        "  v_fg = fg;\n"
        "  v_bg = bg;\n"
        "  v_cell_size = u_cell_size;\n"
        "}\n";
    static const char *fs_src =
        "#version 410\n"
        "in vec2 v_uv;\n"
        "in vec4 v_fg;\n"
        "in vec4 v_bg;\n"
        "in vec2 v_cell_size;\n"
        "out vec4 frag_color;\n"
        "uniform sampler2D atlas;\n"
        "vec3 srgb_to_linear(vec3 c) {\n"
        "  return mix(pow((c + vec3(0.055)) / vec3(1.055), vec3(2.4)),\n"
        "             c / 12.92,\n"
        "             lessThanEqual(c, vec3(0.04045)));\n"
        "}\n"
        "void main() {\n"
        "  if (v_uv.x < 0.0) {\n"
        "    float lx = -v_uv.x - 2.0;\n"
        "    float ly = v_uv.y;\n"
        "    float px = lx * v_cell_size.x;\n"
        "    float py = ly * v_cell_size.y;\n"
        "    float r = 0.09 * v_cell_size.y;\n"
        "    float qx = min(px, v_cell_size.x - px);\n"
        "    float qy = min(py, v_cell_size.y - py);\n"
        "    if (qx < r && qy < r) {\n"
        "      if (length(vec2(r - qx, r - qy)) > r) discard;\n"
        "    }\n"
        "    frag_color = vec4(srgb_to_linear(v_fg.rgb), v_fg.a);\n"
        "    return;\n"
        "  }\n"
        "  if (v_uv.x >= 3.0) {\n"
        "    // color glyph quad: u was offset by 3.0 on the CPU side.\n"
        "    // The atlas stores sRGB→linear-decoded RGB + alpha for color\n"
        "    // emoji/COLRv1 glyphs. Composite the color texel over bg.\n"
        "    vec2 uv = vec2(v_uv.x - 3.0, v_uv.y);\n"
        "    vec4 texel = texture(atlas, uv);\n"
        "    vec3 bg_lin = srgb_to_linear(v_bg.rgb);\n"
        "    vec3 composited = mix(bg_lin, texel.rgb, texel.a);\n"
        "    frag_color = vec4(composited, v_bg.a);\n"
        "    return;\n"
        "  }\n"
        "  if (v_uv.x >= 2.0) {\n"
        "    // bg-only quad: output pure bg color, no glyph\n"
        "    vec3 bg_lin = srgb_to_linear(v_bg.rgb);\n"
        "    frag_color = vec4(bg_lin, v_bg.a);\n"
        "    return;\n"
        "  }\n"
        "  vec4 texel = texture(atlas, v_uv);\n"
        "  float coverage = texel.a;\n"
        "  if (coverage <= 0.0) discard;\n"
        "  vec3 fg_lin = srgb_to_linear(v_fg.rgb);\n"
        "  vec3 bg_lin = srgb_to_linear(v_bg.rgb);\n"
        "  vec3 color = mix(bg_lin, fg_lin, coverage);\n"
        "  frag_color = vec4(color, v_bg.a);\n"
        "}\n";

    sg_shader shd = sg_make_shader(&(sg_shader_desc){
        .vertex_func.source = vs_src,
        .fragment_func.source = fs_src,
        .attrs[0].glsl_name = "pos",
        .attrs[1].glsl_name = "uv",
        .attrs[2].glsl_name = "fg",
        .attrs[3].glsl_name = "bg",
        .uniform_blocks[0] = {
            .stage = SG_SHADERSTAGE_VERTEX,
            .size = sizeof(float) * 4,
            .glsl_uniforms = {
                [0] = { .glsl_name = "u_resolution", .type = SG_UNIFORMTYPE_FLOAT2 },
                [1] = { .glsl_name = "u_cell_size", .type = SG_UNIFORMTYPE_FLOAT2 },
            },
        },
        .views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT,
        .views[0].texture.image_type = SG_IMAGETYPE_2D,
        .views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT,
        .samplers[0].stage = SG_SHADERSTAGE_FRAGMENT,
        .samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING,
        .texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT,
        .texture_sampler_pairs[0].view_slot = 0,
        .texture_sampler_pairs[0].sampler_slot = 0,
        .texture_sampler_pairs[0].glsl_name = "atlas",
        .label = "sokol-glyph-shader",
    });

    d->glyph_vbuf = sg_make_buffer(&(sg_buffer_desc){
        .size = SOKOL_MAX_VERTICES * sizeof(GlyphVertex),
        .usage.dynamic_update = true,
        .label = "sokol-glyph-vbuf",
    });

    // Capture GL buffer ID for debug verifybuf (sokol leaves it bound).
#if defined(SOKOL_GLCORE)
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, (GLint *)&d->debug_glyph_vbuf_gl_id);
#endif

    d->glyph_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .layout = {
            .buffers[0].stride = sizeof(GlyphVertex),
            .attrs = {
                [0] = { .offset = offsetof(GlyphVertex, x), .format = SG_VERTEXFORMAT_FLOAT2 },
                [1] = { .offset = offsetof(GlyphVertex, u), .format = SG_VERTEXFORMAT_FLOAT2 },
                [2] = { .offset = offsetof(GlyphVertex, fg), .format = SG_VERTEXFORMAT_UBYTE4N },
                [3] = { .offset = offsetof(GlyphVertex, bg), .format = SG_VERTEXFORMAT_UBYTE4N },
            },
        },
        .colors[0] = { .pixel_format = d->linear_ok ? SG_PIXELFORMAT_SRGB8A8 : SG_PIXELFORMAT_RGBA8 },
        .label = "sokol-glyph-pipeline",
    });
    d->glyph_pip_created = true;

    // Selection overlay pipeline: same shader, alpha-blended on top.
    // UVs hit the zero-coverage texel so coverage=0, meaning the shader
    // outputs bg_lin (the selection color) with the selection alpha.
    d->sel_pip = sg_make_pipeline(&(sg_pipeline_desc){
        .shader = shd,
        .layout = {
            .buffers[0].stride = sizeof(GlyphVertex),
            .attrs = {
                [0] = { .offset = offsetof(GlyphVertex, x), .format = SG_VERTEXFORMAT_FLOAT2 },
                [1] = { .offset = offsetof(GlyphVertex, u), .format = SG_VERTEXFORMAT_FLOAT2 },
                [2] = { .offset = offsetof(GlyphVertex, fg), .format = SG_VERTEXFORMAT_UBYTE4N },
                [3] = { .offset = offsetof(GlyphVertex, bg), .format = SG_VERTEXFORMAT_UBYTE4N },
            },
        },
        .colors[0] = {
            .pixel_format = d->linear_ok ? SG_PIXELFORMAT_SRGB8A8 : SG_PIXELFORMAT_RGBA8,
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .src_factor_alpha = SG_BLENDFACTOR_ONE,
                .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            },
        },
        .label = "sokol-selection-pipeline",
    });
    d->sel_pip_created = true;
}

static void cell_color(TerminalColor tc, bool is_fg, bool reverse,
                       uint8_t out[4])
{
    const uint8_t def_bg[4] = { DEF_BG_R, DEF_BG_G, DEF_BG_B, 0xFF };
    const uint8_t def_fg[4] = { 0xD0, 0xD0, 0xD0, 0xFF };
    if (tc.is_default) {
        // term_cfr.c pre-swaps fg/bg for reverse video. The pre-swapped
        // fg carries the original bg.is_default flag, so a default color
        // on the visual fg in a reverse cell is actually the default bg.
        if (reverse && is_fg) {
            memcpy(out, def_bg, 4);
        } else {
            memcpy(out, is_fg ? def_fg : def_bg, 4);
        }
    } else {
        out[0] = tc.r;
        out[1] = tc.g;
        out[2] = tc.b;
        out[3] = 0xFF;
    }
}

// Append a solid-color quad to the decoration vertex buffer. UV x=2.0
// selects the bg-only shader path, so fg and bg are both set to the
// requested color and alpha.
static void sokol_deco_emit_quad(float x0, float y0, float x1, float y1,
                                 const uint8_t color[4])
{
    if (s_deco_vert_count + 6 > SOKOL_MAX_DECO_VERTICES) {
        if (!s_deco_overflow_warned) {
            vlog("decoration vertex buffer exhausted; skipping remaining decorations");
            s_deco_overflow_warned = true;
        }
        return;
    }
    GlyphVertex *q = &s_deco_verts[s_deco_vert_count];
    float u = 2.0f;
    q[0] = (GlyphVertex){ x0, y0, u, 0.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[1] = (GlyphVertex){ x1, y0, u, 0.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[2] = (GlyphVertex){ x1, y1, u, 1.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[3] = (GlyphVertex){ x0, y0, u, 0.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[4] = (GlyphVertex){ x1, y1, u, 1.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    q[5] = (GlyphVertex){ x0, y1, u, 1.0f, { color[0], color[1], color[2], color[3] }, { color[0], color[1], color[2], color[3] } };
    s_deco_vert_count += 6;
}

typedef struct
{
    int y;
    float x_start;
    float x_end;
    uint8_t alpha;
} DecoStrip;

static void sokol_deco_strip_emit(float x0, float x1, int y, uint8_t alpha,
                                  const uint8_t color[4])
{
    if (alpha == 0)
        return;
    uint8_t c[4] = { color[0], color[1], color[2], alpha };
    sokol_deco_emit_quad(x0, (float)y, x1, (float)(y + 1), c);
}

// Update the scanline coalescer. `alphas` holds one alpha value per y-row in
// the range [y_min, y_max] inclusive. Active strips are kept in `strips`; on
// alpha/y changes strips are flushed and the list rebuilt.
static void sokol_deco_coalesce_update(float x, float y_min, float y_max,
                                       const uint8_t *alphas, DecoStrip *strips,
                                       int *strip_count, int max_strips,
                                       const uint8_t color[4])
{
    // Build a new set of (y, alpha) pairs for this x coordinate.
    int new_count = 0;
    int new_y[8];
    uint8_t new_alpha[8];
    for (int y = (int)y_min; y <= (int)y_max && new_count < 8; y++) {
        uint8_t a = alphas[y - (int)y_min];
        if (a > 0) {
            new_y[new_count] = y;
            new_alpha[new_count] = a;
            new_count++;
        }
    }

    // Flush any active strip that is no longer present or whose alpha changed.
    for (int i = 0; i < *strip_count; i++) {
        DecoStrip *s = &strips[i];
        bool found = false;
        uint8_t alpha = 0;
        for (int j = 0; j < new_count; j++) {
            if (new_y[j] == s->y) {
                found = true;
                alpha = new_alpha[j];
                break;
            }
        }
        if (!found || alpha != s->alpha) {
            sokol_deco_strip_emit(s->x_start, x, s->y, s->alpha, color);
            // Remove from active list by shifting remaining entries.
            memmove(&strips[i], &strips[i + 1],
                    (size_t)(*strip_count - i - 1) * sizeof(DecoStrip));
            (*strip_count)--;
            i--;
        }
    }

    // Extend or create strips for the new set.
    for (int i = 0; i < new_count; i++) {
        bool found = false;
        for (int j = 0; j < *strip_count; j++) {
            if (strips[j].y == new_y[i] &&
                strips[j].alpha == new_alpha[i]) {
                strips[j].x_end = x;
                found = true;
                break;
            }
        }
        if (!found && *strip_count < max_strips) {
            int idx = *strip_count;
            strips[idx].y = new_y[i];
            strips[idx].x_start = x;
            strips[idx].x_end = x + 1.0f;
            strips[idx].alpha = new_alpha[i];
            (*strip_count)++;
        }
    }
}

static void sokol_deco_coalesce_flush(DecoStrip *strips, int *strip_count,
                                      const uint8_t color[4])
{
    for (int i = 0; i < *strip_count; i++) {
        DecoStrip *s = &strips[i];
        sokol_deco_strip_emit(s->x_start, s->x_end, s->y, s->alpha, color);
    }
    *strip_count = 0;
}

static int sokol_underline_position(SokolData *d, int row)
{
    int cell_y = row * d->cell_h;
    int underline_y = cell_y + d->font_ascent +
                      (int)roundf(2.0f * d->content_scale);
    int thickness = (int)roundf(1.0f * d->content_scale);
    if (thickness < 1)
        thickness = 1;
    if (underline_y + thickness > cell_y + d->cell_h)
        underline_y = cell_y + d->cell_h - thickness;
    return underline_y;
}

static void sokol_draw_underline_single(SokolData *d, int row, int vis_start,
                                        int vis_end, const uint8_t color[4])
{
    int thickness = (int)roundf(1.0f * d->content_scale);
    if (thickness < 1)
        thickness = 1;
    int y = sokol_underline_position(d, row);
    float x0 = (float)(vis_start * d->cell_w);
    float x1 = (float)(vis_end * d->cell_w);
    sokol_deco_emit_quad(x0, (float)y, x1, (float)(y + thickness), color);
}

static void sokol_draw_underline_double(SokolData *d, int row, int vis_start,
                                        int vis_end, const uint8_t color[4])
{
    int thickness = (int)roundf(1.0f * d->content_scale);
    if (thickness < 1)
        thickness = 1;
    int gap = (int)roundf(1.0f * d->content_scale);
    if (gap < 1)
        gap = 1;
    int y1 = sokol_underline_position(d, row);
    int y2 = y1 + thickness + gap;
    float x0 = (float)(vis_start * d->cell_w);
    float x1 = (float)(vis_end * d->cell_w);
    sokol_deco_emit_quad(x0, (float)y1, x1, (float)(y1 + thickness), color);
    sokol_deco_emit_quad(x0, (float)y2, x1, (float)(y2 + thickness), color);
}

static void sokol_draw_underline_curly(SokolData *d, int row, int vis_start,
                                       int vis_end, const uint8_t color[4])
{
    float pd = d->content_scale;
    float amplitude = 1.5f * pd;
    if (amplitude < 1.0f)
        amplitude = 1.0f;
    float wavelength = 8.0f * pd;
    if (wavelength < 4.0f)
        wavelength = 4.0f;
    float thickness = 0.5f * pd;
    if (thickness < 0.5f)
        thickness = 0.5f;
    int underline_y = sokol_underline_position(d, row);
    float center_y = (float)underline_y + amplitude;
    int run_x = vis_start * d->cell_w;
    int run_w = (vis_end - vis_start) * d->cell_w;

    DecoStrip strips[16];
    int strip_count = 0;
    for (int px = 0; px < run_w; px++) {
        float x = (float)(run_x + px);
        float sine_y = center_y +
                       amplitude *
                           sinf((float)px / wavelength * 2.0f * (float)M_PI);
        int y_min = (int)floorf(sine_y - thickness - 1.0f);
        int y_max = (int)ceilf(sine_y + thickness + 1.0f);
        uint8_t alphas[8] = { 0 };
        for (int y = y_min, n = 0; y <= y_max && n < 8; y++, n++) {
            float dist = fabsf((float)y + 0.5f - sine_y);
            if (dist <= thickness) {
                alphas[n] = 255;
            } else if (dist <= thickness + 1.0f) {
                alphas[n] = (uint8_t)roundf(255.0f * (1.0f - (dist - thickness)));
            } else {
                alphas[n] = 0;
            }
        }
        sokol_deco_coalesce_update(x, (float)y_min, (float)y_max, alphas,
                                   strips, &strip_count, 16, color);
    }
    sokol_deco_coalesce_flush(strips, &strip_count, color);
}

static void sokol_draw_underline_dotted(SokolData *d, int row, int vis_start,
                                        int vis_end, const uint8_t color[4])
{
    float pd = d->content_scale;
    float radius = 0.5f * pd;
    if (radius < 0.5f)
        radius = 0.5f;
    float gap = roundf(2.0f * pd);
    if (gap < 2.0f)
        gap = 2.0f;
    float stride = radius * 2.0f + gap;
    int underline_y = sokol_underline_position(d, row);
    int run_x = vis_start * d->cell_w;
    int run_w = (vis_end - vis_start) * d->cell_w;

    DecoStrip strips[16];
    int strip_count = 0;
    for (float cx = (float)run_x; cx < (float)(run_x + run_w); cx += stride) {
        float cy = (float)underline_y + radius;
        int x_min = (int)floorf(cx - radius - 1.0f);
        int x_max = (int)ceilf(cx + radius + 1.0f);
        int y_min = (int)floorf(cy - radius - 1.0f);
        int y_max = (int)ceilf(cy + radius + 1.0f);
        for (int y = y_min; y <= y_max; y++) {
            for (int x = x_min; x <= x_max; x++) {
                float dx = (float)x + 0.5f - cx;
                float dy = (float)y + 0.5f - cy;
                float dist = sqrtf(dx * dx + dy * dy);
                uint8_t alpha;
                if (dist <= radius)
                    alpha = 255;
                else if (dist <= radius + 1.0f)
                    alpha = (uint8_t)roundf(255.0f * (1.0f - (dist - radius)));
                else
                    alpha = 0;
                if (alpha > 0)
                    sokol_deco_strip_emit((float)x, (float)(x + 1), y, alpha,
                                          color);
            }
        }
    }
    (void)strips;
    (void)strip_count;
}

static void sokol_draw_underline_dashed(SokolData *d, int row, int vis_start,
                                        int vis_end, const uint8_t color[4])
{
    float pd = d->content_scale;
    int thickness = (int)roundf(1.0f * pd);
    if (thickness < 1)
        thickness = 1;
    int dash_w = (int)roundf(3.0f * pd);
    if (dash_w < 1)
        dash_w = 1;
    int gap = (int)roundf(2.0f * pd);
    if (gap < 1)
        gap = 1;
    int stride = dash_w + gap;
    int y = sokol_underline_position(d, row);
    int run_x = vis_start * d->cell_w;
    int run_w = (vis_end - vis_start) * d->cell_w;
    float y0 = (float)y;
    float y1 = (float)(y + thickness);
    for (int px = 0; px < run_w; px += stride) {
        int x0 = run_x + px;
        int w = dash_w;
        if (x0 + w > run_x + run_w)
            w = run_x + run_w - x0;
        if (w > 0)
            sokol_deco_emit_quad((float)x0, y0, (float)(x0 + w), y1, color);
    }
}

static void sokol_draw_strikethrough(SokolData *d, int row, int vis_start,
                                     int vis_end, const uint8_t color[4])
{
    float pd = d->content_scale;
    int thickness = (int)roundf(1.0f * pd);
    if (thickness < 1)
        thickness = 1;
    int cell_y = row * d->cell_h;
    int strike_y = cell_y + d->font_ascent - d->font_cap_height / 2;
    float x0 = (float)(vis_start * d->cell_w);
    float x1 = (float)(vis_end * d->cell_w);
    sokol_deco_emit_quad(x0, (float)strike_y, x1,
                         (float)(strike_y + thickness), color);
}

// Reserved glyph ID for the notification panel close "×" under the
// box-drawing font-data slot. Real box-drawing codepoints are > 0.
#define NOTIF_CLOSE_GLYPH_ID 0

static void sokol_emit_notif_close(SokolData *d, int *glyph_vert_count)
{
    if (d->notif_close_size <= 0 || !glyph_vert_count)
        return;

    RendSokolAtlasEntry *entry = rend_sokol_atlas_lookup(
        &d->atlas, BOXDRAW_FONT_DATA, NOTIF_CLOSE_GLYPH_ID, 0);
    if (!entry) {
        int size = d->notif_close_size;
        uint8_t *close_buf = calloc((size_t)size * size, 4);
        if (!close_buf)
            return;
        rend_make_close_x_bitmap(close_buf, size);
        GlyphBitmap gb = {
            .pixels = close_buf,
            .width = size,
            .height = size,
            .x_offset = 0,
            .y_offset = 0,
            .advance = size,
            .centered = false,
        };
        entry = rend_sokol_atlas_insert(
            &d->atlas, BOXDRAW_FONT_DATA, NOTIF_CLOSE_GLYPH_ID, 0, &gb, false);
        free(close_buf);
        if (!entry)
            return;
    }

    if (entry->region.w <= 0 || entry->region.h <= 0)
        return;

    uint8_t lum = d->notif_close_hover ? 245 : 170;
    uint8_t fg[4] = { lum, lum, lum, 255 };
    uint8_t bg[4] = { 38, 38, 44, 255 };

    float cx0 = d->notif_close_x;
    float cy0 = d->notif_close_y;
    float cx1 = cx0 + (float)entry->region.w;
    float cy1 = cy0 + (float)entry->region.h;

    float atlas_size = (float)REND_ATLAS_TEXTURE_SIZE;
    float u0 = (float)entry->region.x / atlas_size;
    float v0 = (float)entry->region.y / atlas_size;
    float u1 = (float)(entry->region.x + entry->region.w) / atlas_size;
    float v1 = (float)(entry->region.y + entry->region.h) / atlas_size;

    if (*glyph_vert_count + 6 > SOKOL_MAX_VERTICES)
        return;
    GlyphVertex *q = &s_glyph_verts[*glyph_vert_count];
    q[0] = (GlyphVertex){ cx0, cy0, u0, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[1] = (GlyphVertex){ cx1, cy0, u1, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[2] = (GlyphVertex){ cx1, cy1, u1, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[3] = (GlyphVertex){ cx0, cy0, u0, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[4] = (GlyphVertex){ cx1, cy1, u1, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    q[5] = (GlyphVertex){ cx0, cy1, u0, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
    *glyph_vert_count += 6;
}

static void sokol_render_terminal_cells(SokolData *d, TerminalBackend *term,
                                        int origin_x, int origin_y,
                                        bool cursor_visible, int scroll_offset,
                                        int *vert_count, int *glyph_vert_count,
                                        int *sel_vert_count,
                                        GlyphVertex *sel_verts)
{
    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);
    if (rows <= 0 || cols <= 0)
        return;

    int cell_w = d->cell_w;
    int cell_h = d->cell_h;
    if (cell_w <= 0 || cell_h <= 0)
        return;

    float atlas_size = (float)REND_ATLAS_TEXTURE_SIZE;
    bool track_index = (term != d->notif_term);

    for (int row = 0; row < rows && *vert_count + 12 <= SOKOL_MAX_VERTICES; row++) {
        int unified_row = rend_display_row_to_unified(scroll_offset, row);
        for (int col = 0; col < cols && *vert_count + 12 <= SOKOL_MAX_VERTICES; col++) {
            TerminalCell cell;
            if (unified_row < 0) {
                if (terminal_get_scrollback_cell(term, -unified_row - 1, col, &cell) != 0)
                    continue;
            } else {
                if (terminal_get_cell(term, unified_row, col, &cell) != 0)
                    continue;
            }
            if (cell.width == 0) {
                if (cursor_visible && scroll_offset == 0 &&
                    terminal_get_cursor_visible(term) && *vert_count + 6 <= SOKOL_MAX_VERTICES) {
                    TerminalPos cp = terminal_get_cursor_pos(term);
                    if (cp.row == row && cp.col == col &&
                        *glyph_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                        float cx0 = (float)(origin_x + col * cell_w);
                        float cy0 = (float)(origin_y + row * cell_h);
                        float cx1 = cx0 + (float)cell_w;
                        float cy1 = cy0 + (float)cell_h;
                        uint8_t cc[4] = { CURSOR_COLOR_R, CURSOR_COLOR_G,
                                          CURSOR_COLOR_B, CURSOR_COLOR_A };
                        float cu0 = -(0.0f + 2.0f);
                        float cu1 = -(1.0f + 2.0f);
                        GlyphVertex *q = &s_glyph_verts[*glyph_vert_count];
                        q[0] = (GlyphVertex){ cx0, cy0, cu0, 0.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                        q[1] = (GlyphVertex){ cx1, cy0, cu1, 0.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                        q[2] = (GlyphVertex){ cx1, cy1, cu1, 1.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                        q[3] = (GlyphVertex){ cx0, cy0, cu0, 0.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                        q[4] = (GlyphVertex){ cx1, cy1, cu1, 1.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                        q[5] = (GlyphVertex){ cx0, cy1, cu0, 1.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                        *glyph_vert_count += 6;
                    }
                }
                continue;
            }

            uint8_t fg[4], bg[4];
            bool rev = cell.attrs.reverse;
            cell_color(cell.fg, true, rev, fg);
            cell_color(cell.bg, false, rev, bg);

            // Dim/faint (SGR 2): blend foreground toward background at 40% opacity
            if (cell.attrs.dim) {
                fg[0] = (uint8_t)(fg[0] * 0.4f + bg[0] * 0.6f);
                fg[1] = (uint8_t)(fg[1] * 0.4f + bg[1] * 0.6f);
                fg[2] = (uint8_t)(fg[2] * 0.4f + bg[2] * 0.6f);
            }

            bool in_sel = terminal_cell_in_selection(term, unified_row, col);

            bool is_cursor = false;
            if (cursor_visible && scroll_offset == 0 &&
                terminal_get_cursor_visible(term)) {
                TerminalPos cpos = terminal_get_cursor_pos(term);
                if (cpos.row == row && cpos.col == col) {
                    is_cursor = true;
                }
            }

            float cell_x0 = (float)(origin_x + col * cell_w);
            float cell_y0 = (float)(origin_y + row * cell_h);
            float cell_x1 = cell_x0 + (float)(cell.width * cell_w);
            float cell_y1 = cell_y0 + (float)cell_h;

            float bg_u = 2.0f;
            float bg_v = 0.0f;
            if (track_index)
                s_vert_index[row][col] = *vert_count;
            GlyphVertex *q = &s_frame_verts[*vert_count];
            q[0] = (GlyphVertex){ cell_x0, cell_y0, bg_u, bg_v, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[1] = (GlyphVertex){ cell_x1, cell_y0, bg_u, bg_v, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[2] = (GlyphVertex){ cell_x1, cell_y1, bg_u, bg_v, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[3] = (GlyphVertex){ cell_x0, cell_y0, bg_u, bg_v, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[4] = (GlyphVertex){ cell_x1, cell_y1, bg_u, bg_v, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[5] = (GlyphVertex){ cell_x0, cell_y1, bg_u, bg_v, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            *vert_count += 6;

            if (is_cursor && *glyph_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                uint8_t cc[4] = { CURSOR_COLOR_R, CURSOR_COLOR_G,
                                  CURSOR_COLOR_B, CURSOR_COLOR_A };
                float cu0 = -(0.0f + 2.0f);
                float cu1 = -(1.0f + 2.0f);
                q = &s_glyph_verts[*glyph_vert_count];
                q[0] = (GlyphVertex){ cell_x0, cell_y0, cu0, 0.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                q[1] = (GlyphVertex){ cell_x1, cell_y0, cu1, 0.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                q[2] = (GlyphVertex){ cell_x1, cell_y1, cu1, 1.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                q[3] = (GlyphVertex){ cell_x0, cell_y0, cu0, 0.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                q[4] = (GlyphVertex){ cell_x1, cell_y1, cu1, 1.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                q[5] = (GlyphVertex){ cell_x0, cell_y1, cu0, 1.0f, { cc[0], cc[1], cc[2], cc[3] }, { cc[0], cc[1], cc[2], cc[3] } };
                *glyph_vert_count += 6;
            }

            if (cell.cp != 0 && cell.cp != 0x20 && !is_cursor && !cell.attrs.invis) {
                if (rend_boxdraw_is_supported(cell.cp)) {
                    uint32_t bd_cp = cell.cp;
                    uint32_t color_key = 0;

                    RendSokolAtlasEntry *bd_entry = rend_sokol_atlas_lookup(
                        &d->atlas, BOXDRAW_FONT_DATA, (int)bd_cp, color_key);

                    if (!bd_entry) {
                        GlyphBitmap *bmp = rend_boxdraw_render(
                            bd_cp, cell_w, cell_h, fg[0], fg[1], fg[2]);
                        if (bmp) {
                            bd_entry = rend_sokol_atlas_insert(
                                &d->atlas, BOXDRAW_FONT_DATA,
                                (int)bd_cp, color_key, bmp, false);
                            free(bmp->pixels);
                            free(bmp);
                        } else {
                            bd_entry = rend_sokol_atlas_insert_empty(
                                &d->atlas, BOXDRAW_FONT_DATA,
                                (int)bd_cp, color_key);
                        }
                    }

                    if (bd_entry && bd_entry->region.w > 0 &&
                        bd_entry->region.h > 0 &&
                        *glyph_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                        float gx0, gy0, gx1, gy1;
                        if (bd_entry->centered) {
                            int glyph_w = cell.width * cell_w;
                            // For padded bitmaps (diagonals with region > cell), extend
                            // beyond cell bounds so overhang fills row-boundary gaps.
                            // For normal centered glyphs (region <= cell), center within.
                            int pad_x = (bd_entry->region.w - glyph_w) / 2;
                            int pad_y = (bd_entry->region.h - cell_h) / 2;
                            if (pad_x > 0 || pad_y > 0) {
                                // Bitmap has padding - extend beyond cell bounds
                                gx0 = (float)cell_x0 - (float)pad_x;
                                gy0 = (float)cell_y0 - (float)pad_y;
                            } else {
                                // Normal centered glyph (emoji, symbol) - center within cell
                                gx0 = (float)cell_x0 +
                                      ((float)glyph_w - (float)bd_entry->region.w) * 0.5f;
                                gy0 = (float)cell_y0 +
                                      ((float)cell_h - (float)bd_entry->region.h) * 0.5f;
                            }
                        } else {
                            gx0 = (float)cell_x0;
                            gy0 = (float)cell_y0;
                        }
                        gx1 = gx0 + (float)bd_entry->region.w;
                        gy1 = gy0 + (float)bd_entry->region.h;

                        float u0 = (float)bd_entry->region.x / atlas_size;
                        float v0 = (float)bd_entry->region.y / atlas_size;
                        float u1 = (float)(bd_entry->region.x + bd_entry->region.w) / atlas_size;
                        float v1 = (float)(bd_entry->region.y + bd_entry->region.h) / atlas_size;

                        q = &s_glyph_verts[*glyph_vert_count];
                        q[0] = (GlyphVertex){ gx0, gy0, u0, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                        q[1] = (GlyphVertex){ gx1, gy0, u1, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                        q[2] = (GlyphVertex){ gx1, gy1, u1, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                        q[3] = (GlyphVertex){ gx0, gy0, u0, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                        q[4] = (GlyphVertex){ gx1, gy1, u1, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                        q[5] = (GlyphVertex){ gx0, gy1, u0, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                        *glyph_vert_count += 6;
                    }
                    goto selection_check;
                }

                if (!d->font)
                    goto selection_check;

                FontStyle style = FONT_STYLE_NORMAL;
                if (cell.attrs.bold && cell.attrs.italic)
                    style = FONT_STYLE_BOLD_ITALIC;
                else if (cell.attrs.bold)
                    style = FONT_STYLE_BOLD;
                else if (cell.attrs.italic)
                    style = FONT_STYLE_ITALIC;

                if (!font_has_style(d->font, style))
                    style = FONT_STYLE_NORMAL;

                uint32_t cps[32];
                int cp_count;
                if (cell.grapheme_id == 0) {
                    cps[0] = cell.cp;
                    cp_count = 1;
                } else {
                    size_t n = terminal_cell_get_grapheme(term, unified_row, col,
                                                          cps, 32);
                    if (n == 0) {
                        cps[0] = cell.cp;
                        n = 1;
                    }
                    cp_count = (int)n;
                }
                bool emoji_available = font_has_style(d->font, FONT_STYLE_EMOJI);
                bool emoji_has_glyph = emoji_available &&
                                       font_get_glyph_index(d->font, FONT_STYLE_EMOJI, cell.cp) != 0;
                if (rend_should_use_emoji(cps, cp_count, emoji_available, emoji_has_glyph))
                    style = FONT_STYLE_EMOJI;

                int avail_w = cell.width * cell_w;
                int avail_h = cell_h;

                for (int s = 0; s < FONT_STYLE_COUNT; s++)
                    font_set_presentation_width(d->font, s, avail_w);

                if (style == FONT_STYLE_EMOJI && avail_h < avail_w)
                    avail_w = avail_h;

                bool color_baked = rend_is_color_font(d->font, style);
                uint8_t render_r = color_baked ? fg[0] : 255;
                uint8_t render_g = color_baked ? fg[1] : 255;
                uint8_t render_b = color_baked ? fg[2] : 255;
                uint32_t color_key = color_baked
                                         ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                         : 0xFFFFFF;

                bool emoji_render = (style == FONT_STYLE_EMOJI);
                bool symbol_cell = rend_is_symbol_cell_cp(cell.cp);
                bool downscale_glyph = (emoji_render && color_baked) || symbol_cell;
                bool height_only_fit = symbol_cell && !color_baked;

                int cache_w = avail_w;
                int cache_h = avail_h;
                bool is_regional = is_regional_indicator(cell.cp);
                if (is_regional) {
                    int side = avail_w < avail_h ? avail_w : avail_h;
                    cache_w = cache_h = side;
                }

                if (cp_count > 1 && d->font->render_shaped) {
                    void *sh_font_data = d->font->font_data[style];
                    ShapedGlyphs *shaped = font_render_shaped_text(
                        d->font, style, cps, cp_count,
                        render_r, render_g, render_b);

                    if (shaped) {
                        bool all_notdef = true;
                        for (int i = 0; i < shaped->num_glyphs; i++) {
                            if (shaped->glyph_ids[i] != 0) {
                                all_notdef = false;
                                break;
                            }
                        }
                        if (all_notdef) {
                            free(shaped->glyph_ids);
                            free(shaped->x_positions);
                            free(shaped->y_positions);
                            free(shaped->x_advances);
                            free(shaped);
                            shaped = NULL;
                        }
                    }

                    if (!shaped && style != FONT_STYLE_NORMAL) {
                        style = FONT_STYLE_NORMAL;
                        sh_font_data = d->font->font_data[style];
                        color_baked = rend_is_color_font(d->font, style);
                        render_r = color_baked ? fg[0] : 255;
                        render_g = color_baked ? fg[1] : 255;
                        render_b = color_baked ? fg[2] : 255;
                        color_key = color_baked
                                        ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                        : 0xFFFFFF;
                        shaped = font_render_shaped_text(
                            d->font, style, cps, cp_count,
                            render_r, render_g, render_b);
                    }

                    if (!shaped && cp_count > 0) {
                        const char *fb_path = rend_fallback_lookup(&d->fallback, d->resolve, cps[0]);
                        if (fb_path && rend_fallback_ensure(&d->fallback, d->font, fb_path,
                                                            d->font_size, &d->font_options, d->cell_w)) {
                            style = FONT_STYLE_FALLBACK;
                            sh_font_data = d->font->font_data[style];
                            font_set_presentation_width(d->font, style, avail_w);
                            color_baked = rend_is_color_font(d->font, style);
                            render_r = color_baked ? fg[0] : 255;
                            render_g = color_baked ? fg[1] : 255;
                            render_b = color_baked ? fg[2] : 255;
                            color_key = color_baked
                                            ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                            : 0xFFFFFF;
                            shaped = font_render_shaped_text(
                                d->font, style, cps, cp_count,
                                render_r, render_g, render_b);
                        }
                    }

                    if (shaped) {
                        for (int gi = 0; gi < shaped->num_glyphs; gi++) {
                            uint32_t gid = shaped->glyph_ids[gi];
                            if (gid == 0)
                                continue;
                            uint32_t atlas_gid = (cell.width >= 2) ? (gid | (1u << 29)) : gid;
                            RendSokolAtlasEntry *entry = rend_sokol_atlas_lookup(
                                &d->atlas, sh_font_data, (int)atlas_gid, color_key);
                            if (!entry) {
                                GlyphBitmap *gb = font_render_glyph_id(
                                    d->font, style, gid,
                                    render_r, render_g, render_b);
                                if (gb) {
                                    GlyphBitmap *scaled = NULL;
                                    if (downscale_glyph) {
                                        scaled = rend_downscale_bitmap(gb, cache_w, cache_h, height_only_fit);
                                        bool centered = !height_only_fit;
                                        gb->centered = centered;
                                        if (scaled)
                                            scaled->centered = centered;
                                        if (height_only_fit) {
                                            int eff_w = scaled ? scaled->width : gb->width;
                                            int x_off = (cache_w - eff_w) / 2;
                                            gb->x_offset = x_off;
                                            if (scaled)
                                                scaled->x_offset = x_off;
                                        }
                                    }
                                    uint32_t insert_id = atlas_gid ? atlas_gid
                                                                   : (uint32_t)gb->glyph_id;
                                    entry = rend_sokol_atlas_insert(
                                        &d->atlas, sh_font_data, (int)insert_id, color_key,
                                        scaled ? scaled : gb, color_baked);
                                    if (scaled) {
                                        free(scaled->pixels);
                                        free(scaled);
                                    }
                                    d->font->free_glyph_bitmap(d->font, gb);
                                } else if (atlas_gid != 0) {
                                    entry = rend_sokol_atlas_insert_empty(
                                        &d->atlas, sh_font_data, (int)atlas_gid, color_key);
                                }
                            }

                            if (entry && entry->region.w > 0 && entry->region.h > 0 &&
                                *glyph_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                                int gx = (int)cell_x0 + shaped->x_positions[gi] + entry->x_offset;
                                int gy = (int)cell_y0 + d->font_ascent - entry->y_offset;
                                if (entry->centered) {
                                    int glyph_w = cell.width * cell_w;
                                    gx = (int)floorf(cell_x0 + ((float)glyph_w - (float)entry->region.w) * 0.5f);
                                    gy = (int)floorf(cell_y0 + ((float)cell_h - (float)entry->region.h) * 0.5f);
                                }

                                float gx0 = (float)gx;
                                float gy0 = (float)gy;
                                float gx1 = gx0 + (float)entry->region.w;
                                float gy1 = gy0 + (float)entry->region.h;

                                float u0 = (float)entry->region.x / atlas_size;
                                float v0 = (float)entry->region.y / atlas_size;
                                float u1 = (float)(entry->region.x + entry->region.w) / atlas_size;
                                float v1 = (float)(entry->region.y + entry->region.h) / atlas_size;

                                float u_off = color_baked ? 3.0f : 0.0f;

                                q = &s_glyph_verts[*glyph_vert_count];
                                q[0] = (GlyphVertex){ gx0, gy0, u0 + u_off, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                                q[1] = (GlyphVertex){ gx1, gy0, u1 + u_off, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                                q[2] = (GlyphVertex){ gx1, gy1, u1 + u_off, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                                q[3] = (GlyphVertex){ gx0, gy0, u0 + u_off, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                                q[4] = (GlyphVertex){ gx1, gy1, u1 + u_off, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                                q[5] = (GlyphVertex){ gx0, gy1, u0 + u_off, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                                *glyph_vert_count += 6;
                            }
                        }
                        free(shaped->glyph_ids);
                        free(shaped->x_positions);
                        free(shaped->y_positions);
                        free(shaped->x_advances);
                        free(shaped);
                        goto selection_check;
                    }
                }

                uint32_t glyph_id = font_get_glyph_index(d->font, style, cell.cp);

                void *font_data = d->font->font_data[style];
                if (glyph_id == 0 && style != FONT_STYLE_NORMAL) {
                    style = FONT_STYLE_NORMAL;
                    font_data = d->font->font_data[style];
                    color_baked = rend_is_color_font(d->font, style);
                    render_r = color_baked ? fg[0] : 255;
                    render_g = color_baked ? fg[1] : 255;
                    render_b = color_baked ? fg[2] : 255;
                    color_key = color_baked
                                    ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                    : 0xFFFFFF;
                    glyph_id = font_get_glyph_index(d->font, style, cell.cp);
                }

                if (glyph_id == 0) {
                    const char *fb_path = rend_fallback_lookup(&d->fallback, d->resolve, cell.cp);
                    if (fb_path && rend_fallback_ensure(&d->fallback, d->font, fb_path,
                                                        d->font_size, &d->font_options, d->cell_w)) {
                        style = FONT_STYLE_FALLBACK;
                        font_data = d->font->font_data[style];
                        font_set_presentation_width(d->font, style, avail_w);
                        color_baked = rend_is_color_font(d->font, style);
                        render_r = color_baked ? fg[0] : 255;
                        render_g = color_baked ? fg[1] : 255;
                        render_b = color_baked ? fg[2] : 255;
                        color_key = color_baked
                                        ? ((uint32_t)fg[0] << 16) | ((uint32_t)fg[1] << 8) | (uint32_t)fg[2]
                                        : 0xFFFFFF;
                        glyph_id = font_get_glyph_index(d->font, style, cell.cp);
                    }
                }

                uint32_t atlas_glyph_id = glyph_id;
                if (cell.width >= 2 && atlas_glyph_id != 0)
                    atlas_glyph_id |= (1u << 29);

                RendSokolAtlasEntry *entry = NULL;
                if (atlas_glyph_id != 0)
                    entry = rend_sokol_atlas_lookup(
                        &d->atlas, font_data, (int)atlas_glyph_id, color_key);

                if (!entry) {
                    GlyphBitmap *bmp = font_render_glyphs(
                        d->font, style, &cell.cp, 1, render_r, render_g, render_b);
                    if (bmp) {
                        GlyphBitmap *scaled = NULL;
                        if (downscale_glyph) {
                            scaled = rend_downscale_bitmap(bmp, cache_w, cache_h, height_only_fit);
                            bool centered = !height_only_fit;
                            bmp->centered = centered;
                            if (scaled)
                                scaled->centered = centered;
                            if (height_only_fit) {
                                int eff_w = scaled ? scaled->width : bmp->width;
                                int x_off = (cache_w - eff_w) / 2;
                                bmp->x_offset = x_off;
                                if (scaled)
                                    scaled->x_offset = x_off;
                            }
                        }
                        uint32_t insert_id = atlas_glyph_id ? atlas_glyph_id
                                                            : (uint32_t)bmp->glyph_id;
                        entry = rend_sokol_atlas_insert(
                            &d->atlas, font_data, (int)insert_id, color_key,
                            scaled ? scaled : bmp, color_baked);
                        if (scaled) {
                            free(scaled->pixels);
                            free(scaled);
                        }
                        d->font->free_glyph_bitmap(d->font, bmp);
                    } else if (atlas_glyph_id != 0) {
                        entry = rend_sokol_atlas_insert_empty(
                            &d->atlas, font_data, (int)atlas_glyph_id, color_key);
                    }
                }

                if (entry && entry->region.w > 0 && entry->region.h > 0 &&
                    *glyph_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                    int gx = (int)cell_x0 + entry->x_offset;
                    int gy = (int)cell_y0 + d->font_ascent - entry->y_offset;
                    if (entry->centered) {
                        int glyph_w = cell.width * cell_w;
                        gx = (int)floorf(cell_x0 + ((float)glyph_w - (float)entry->region.w) * 0.5f);
                        gy = (int)floorf(cell_y0 + ((float)cell_h - (float)entry->region.h) * 0.5f);
                    }

                    float gx0 = (float)gx;
                    float gy0 = (float)gy;
                    float gx1 = gx0 + (float)entry->region.w;
                    float gy1 = gy0 + (float)entry->region.h;

                    float u0 = (float)entry->region.x / atlas_size;
                    float v0 = (float)entry->region.y / atlas_size;
                    float u1 = (float)(entry->region.x + entry->region.w) / atlas_size;
                    float v1 = (float)(entry->region.y + entry->region.h) / atlas_size;

                    float u_off = color_baked ? 3.0f : 0.0f;

                    q = &s_glyph_verts[*glyph_vert_count];
                    q[0] = (GlyphVertex){ gx0, gy0, u0 + u_off, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                    q[1] = (GlyphVertex){ gx1, gy0, u1 + u_off, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                    q[2] = (GlyphVertex){ gx1, gy1, u1 + u_off, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                    q[3] = (GlyphVertex){ gx0, gy0, u0 + u_off, v0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                    q[4] = (GlyphVertex){ gx1, gy1, u1 + u_off, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                    q[5] = (GlyphVertex){ gx0, gy1, u0 + u_off, v1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
                    *glyph_vert_count += 6;
                }
            }

        selection_check:
            if (in_sel && *sel_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                uint8_t sc[4] = { SELECTION_COLOR_R, SELECTION_COLOR_G,
                                  SELECTION_COLOR_B, SELECTION_COLOR_A };
                GlyphVertex *sq = &sel_verts[*sel_vert_count];
                sq[0] = (GlyphVertex){ cell_x0, cell_y0, bg_u, bg_v, { sc[0], sc[1], sc[2], sc[3] }, { sc[0], sc[1], sc[2], sc[3] } };
                sq[1] = (GlyphVertex){ cell_x1, cell_y0, bg_u, bg_v, { sc[0], sc[1], sc[2], sc[3] }, { sc[0], sc[1], sc[2], sc[3] } };
                sq[2] = (GlyphVertex){ cell_x1, cell_y1, bg_u, bg_v, { sc[0], sc[1], sc[2], sc[3] }, { sc[0], sc[1], sc[2], sc[3] } };
                sq[3] = (GlyphVertex){ cell_x0, cell_y0, bg_u, bg_v, { sc[0], sc[1], sc[2], sc[3] }, { sc[0], sc[1], sc[2], sc[3] } };
                sq[4] = (GlyphVertex){ cell_x1, cell_y1, bg_u, bg_v, { sc[0], sc[1], sc[2], sc[3] }, { sc[0], sc[1], sc[2], sc[3] } };
                sq[5] = (GlyphVertex){ cell_x0, cell_y1, bg_u, bg_v, { sc[0], sc[1], sc[2], sc[3] }, { sc[0], sc[1], sc[2], sc[3] } };
                *sel_vert_count += 6;
            }
        }
    }
}

static void sokol_draw_terminal(PorttyBackend *self, TerminalBackend *term,
                                bool cursor_visible)
{
    SokolData *d = sokol_data(self);
    if (!d || !term)
        return;

    // If overlay is active, render the overlay terminal instead
    if (d->scroll.overlay)
        term = d->scroll.overlay;

    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);
    if (rows <= 0 || cols <= 0)
        return;

    int cell_w = d->cell_w;
    int cell_h = d->cell_h;
    if (cell_w <= 0 || cell_h <= 0)
        return;

    // Use the actual framebuffer dimensions (sapp_width/height return
    // physical pixels on all platforms).
    int win_w = (int)sapp_width();
    int win_h = (int)sapp_height();
    if (win_w <= 0 || win_h <= 0) {
        win_w = cols * cell_w;
        win_h = rows * cell_h;
    }

    // Ensure atlas is initialized
    if (!d->atlas.texture_created) {
        if (!rend_sokol_atlas_init(&d->atlas, d->linear_ok))
            return;
    }

    // Ensure glyph pipeline is created
    sokol_ensure_glyph_pipeline(d);
    sokol_ensure_lottie_pipeline(d);

    rend_sokol_atlas_begin_frame(&d->atlas);

    // Build vertex data (using file-scope arrays for debug access)
    static GlyphVertex sel_verts[SOKOL_MAX_VERTICES];
    int vert_count = 0;       // bg quads in s_frame_verts
    int glyph_vert_count = 0; // glyph/cursor quads in s_glyph_verts
    int sel_vert_count = 0;
    int scroll_offset = d->scroll.scroll_offset;

    memset(s_vert_index, -1, sizeof(s_vert_index));

    sokol_render_terminal_cells(d, term, 0, 0, cursor_visible, scroll_offset,
                                &vert_count, &glyph_vert_count, &sel_vert_count,
                                sel_verts);

    // Notification panel cells (composited on top of primary terminal).
    // Rendered into the same vertex arrays so the panel shares the bg/glyph
    // passes with the primary terminal.
    if (d->notif_active && d->notif_term) {
        float scale = d->content_scale > 0 ? d->content_scale : 1.0f;
        int pad = (int)(10.0f * scale + 0.5f);
        int accent_w = (int)(4.0f * scale + 0.5f);
        int gap = (int)(8.0f * scale + 0.5f);
        int text_x = pad + accent_w + gap;

        sokol_render_terminal_cells(d, d->notif_term,
                                    d->notif_x + text_x, d->notif_y + pad,
                                    false, 0,
                                    &vert_count, &glyph_vert_count,
                                    &sel_vert_count, sel_verts);
    }

    // Hover link hint panel cells (second instance of the same panel system).
    // The panel text is rendered through sokol_render_terminal_cells below,
    // which appends both bg and glyph quads to the shared arrays. The full-width
    // panel background is emitted separately into the decoration buffer so it is
    // alpha-blended *after* the terminal grid glyphs, matching the SDL3 backend
    // where the hint texture is drawn on top of the finished terminal frame.
    int hint_bg_y = 0;
    bool hint_bg_valid = false;
    int hint_glyph_start = glyph_vert_count;
    if (d->hint_active && d->hint_term && d->hint_h > 0) {
        float scale = d->content_scale > 0 ? d->content_scale : 1.0f;
        int pad = (int)(10.0f * scale + 0.5f);
        int y = sokol_hint_compute_y(d, d->hint_anchor_py, win_h);
        hint_bg_y = y;
        hint_bg_valid = true;

        sokol_render_terminal_cells(d, d->hint_term,
                                    pad, y + pad,
                                    false, 0,
                                    &vert_count, &glyph_vert_count,
                                    &sel_vert_count, sel_verts);
    }

    // Decoration pass: coalesce underlines and strikethroughs by style and
    // color, and emit them into s_deco_verts for alpha-blended rendering.
    s_deco_vert_count = 0;
    s_deco_overflow_warned = false;
    for (int row = 0; row < rows; row++) {
        int unified_row = rend_display_row_to_unified(scroll_offset, row);

        // Underline coalescing (Pass 1 of decoration pass)
        {
            TerminalRowIter it;
            terminal_row_iter_init(&it, term, unified_row, cols);
            int run_start = -1;
            int vis_run_end = 0;
            unsigned int run_style = 0;
            uint8_t run_color[4] = { 0, 0, 0, 0 };
            while (terminal_row_iter_next(&it)) {
                unsigned int cs = it.cell.attrs.underline;
                uint8_t cr[4];
                if (it.cell.ul_color.is_default) {
                    cr[0] = UNDERLINE_COLOR_R;
                    cr[1] = UNDERLINE_COLOR_G;
                    cr[2] = UNDERLINE_COLOR_B;
                    cr[3] = UNDERLINE_COLOR_A;
                } else {
                    cr[0] = it.cell.ul_color.r;
                    cr[1] = it.cell.ul_color.g;
                    cr[2] = it.cell.ul_color.b;
                    cr[3] = 0xFF;
                }
                bool same_run = (run_style != 0 && cs == run_style &&
                                 cr[0] == run_color[0] &&
                                 cr[1] == run_color[1] &&
                                 cr[2] == run_color[2] &&
                                 cr[3] == run_color[3]);
                if (run_style != 0 && !same_run) {
                    switch (run_style) {
                    case 1:
                        sokol_draw_underline_single(d, row, run_start,
                                                    vis_run_end, run_color);
                        break;
                    case 2:
                        sokol_draw_underline_double(d, row, run_start,
                                                    vis_run_end, run_color);
                        break;
                    case 3:
                        sokol_draw_underline_curly(d, row, run_start,
                                                   vis_run_end, run_color);
                        break;
                    case 4:
                        sokol_draw_underline_dotted(d, row, run_start,
                                                    vis_run_end, run_color);
                        break;
                    case 5:
                        sokol_draw_underline_dashed(d, row, run_start,
                                                    vis_run_end, run_color);
                        break;
                    }
                    run_style = 0;
                }
                if (cs != 0 && run_style == 0) {
                    run_start = it.vis_col;
                    run_style = cs;
                    run_color[0] = cr[0];
                    run_color[1] = cr[1];
                    run_color[2] = cr[2];
                    run_color[3] = cr[3];
                }
                vis_run_end = it.vis_col + it.pres_w;
            }
            if (run_style != 0) {
                switch (run_style) {
                case 1:
                    sokol_draw_underline_single(d, row, run_start,
                                                vis_run_end, run_color);
                    break;
                case 2:
                    sokol_draw_underline_double(d, row, run_start,
                                                vis_run_end, run_color);
                    break;
                case 3:
                    sokol_draw_underline_curly(d, row, run_start,
                                               vis_run_end, run_color);
                    break;
                case 4:
                    sokol_draw_underline_dotted(d, row, run_start,
                                                vis_run_end, run_color);
                    break;
                case 5:
                    sokol_draw_underline_dashed(d, row, run_start,
                                                vis_run_end, run_color);
                    break;
                }
            }
        }

        // Strikethrough coalescing (Pass 2 of decoration pass)
        {
            TerminalRowIter it;
            terminal_row_iter_init(&it, term, unified_row, cols);
            int run_start = -1;
            int vis_run_end = 0;
            bool in_run = false;
            uint8_t run_color[4] = { 0, 0, 0, 0 };
            while (terminal_row_iter_next(&it)) {
                bool cs = it.cell.attrs.strikethrough;
                uint8_t cr[4];
                cell_color(it.cell.fg, true, it.cell.attrs.reverse, cr);
                bool same_run = in_run && cs &&
                                cr[0] == run_color[0] &&
                                cr[1] == run_color[1] &&
                                cr[2] == run_color[2];
                if (in_run && !same_run) {
                    sokol_draw_strikethrough(d, row, run_start,
                                             vis_run_end, run_color);
                    in_run = false;
                }
                if (cs && !in_run) {
                    run_start = it.vis_col;
                    in_run = true;
                    run_color[0] = cr[0];
                    run_color[1] = cr[1];
                    run_color[2] = cr[2];
                    run_color[3] = cr[3];
                }
                vis_run_end = it.vis_col + it.pres_w;
            }
            if (in_run) {
                sokol_draw_strikethrough(d, row, run_start,
                                         vis_run_end, run_color);
            }
        }
    }

    // Notification panel decoration: accent stripe goes into the decoration
    // buffer; close button goes into the glyph buffer. Both must be emitted
    // before the glyph/decoration buffers are appended to the frame buffer.
    if (d->notif_active && d->notif_term) {
        float scale = d->content_scale > 0 ? d->content_scale : 1.0f;
        int pad = (int)(10.0f * scale + 0.5f);
        int accent_w = (int)(4.0f * scale + 0.5f);

        uint8_t ac[4] = { 0, 0, 0, 255 };
        switch (d->notif_level) {
        case 2:
            ac[0] = 224;
            ac[1] = 27;
            ac[2] = 36;
            break;
        case 1:
            ac[0] = 245;
            ac[1] = 194;
            ac[2] = 17;
            break;
        default:
            ac[0] = 98;
            ac[1] = 160;
            ac[2] = 234;
            break;
        }
        sokol_deco_emit_quad(
            (float)(d->notif_x + pad), (float)(d->notif_y + pad),
            (float)(d->notif_x + pad + accent_w), (float)(d->notif_y + d->notif_h - pad),
            ac);

        sokol_emit_notif_close(d, &glyph_vert_count);
    }

    // Hover link hint panel background: emit last into the decoration buffer
    // so it is drawn after the terminal frame (background, glyphs, underlines,
    // and selection) but before the panel text glyphs. This matches the SDL3
    // backend where the hint texture is drawn on top of the finished terminal
    // frame, and keeps the panel text visible on top of its own background.
    int hint_deco_start = s_deco_vert_count;
    int hint_deco_count = 0;
    if (hint_bg_valid) {
        uint8_t bg[4] = { 38, 38, 44, 255 };
        sokol_deco_emit_quad(0.0f, (float)hint_bg_y,
                             (float)win_w, (float)(hint_bg_y + d->hint_h), bg);
        hint_deco_count = 6;
    }

    // Append glyph quads after bg quads, then selection overlay quads.
    // Two-pass rendering: all bg quads are drawn first (vertices 0..bg_count),
    // then all glyph/cursor quads on top (vertices bg_count..glyph_end).
    // This ensures glyph overhangs (e.g. diagonal box-drawing 1px padding)
    // are not overwritten by neighboring cells' bg quads.
    int bg_vert_count = vert_count;
    if (glyph_vert_count > 0 && vert_count + glyph_vert_count <= SOKOL_MAX_VERTICES) {
        memcpy(&s_frame_verts[vert_count], s_glyph_verts,
               (size_t)glyph_vert_count * sizeof(GlyphVertex));
        vert_count += glyph_vert_count;
    }
    int glyph_vert_start = bg_vert_count;
    int glyph_end = vert_count;

    // Append selection overlay quads after glyph quads so all fit in one
    // buffer update (Sokol allows only one sg_update_buffer per frame).
    int sel_vert_start = vert_count;
    if (sel_vert_count > 0 && vert_count + sel_vert_count <= SOKOL_MAX_VERTICES) {
        memcpy(&s_frame_verts[vert_count], sel_verts, (size_t)sel_vert_count * sizeof(GlyphVertex));
        vert_count += sel_vert_count;
    }

    // Append decoration quads after selection quads. They are drawn with
    // the selection pipeline (alpha-blended) between glyphs and selections.
    int deco_vert_start = vert_count;
    if (s_deco_vert_count > 0 &&
        vert_count + s_deco_vert_count <= SOKOL_MAX_VERTICES) {
        memcpy(&s_frame_verts[vert_count], s_deco_verts,
               (size_t)s_deco_vert_count * sizeof(GlyphVertex));
        vert_count += s_deco_vert_count;
    }

    // Flush atlas dirty regions to GPU
    rend_sokol_atlas_flush(&d->atlas);

    // Enable sRGB framebuffer for gamma-correct linear-light compositing.
    // OpenGL auto-decodes blend src/dst to linear, blends in linear, and
    // re-encodes to sRGB on store. The shader also decodes v_fg/v_bg to
    // linear before mix() since vertex attributes are not auto-linearized.
    // Note: sg_begin_pass already enables GL_FRAMEBUFFER_SRGB for sRGB
    // swapchains, so we must NOT manually enable it here (that was the
    // source of the bg color rendering bug).

    // Render
    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                           .clear_value = { rend_srgb_to_linear(DEF_BG_R) / 255.0f,
                                            rend_srgb_to_linear(DEF_BG_G) / 255.0f,
                                            rend_srgb_to_linear(DEF_BG_B) / 255.0f, 1.0f } } },
        .swapchain = sglue_swapchain(),
    });

    if (vert_count > 0 && d->glyph_pip_created) {
        s_frame_vert_count = vert_count;
        sg_update_buffer(d->glyph_vbuf, &(sg_range){ .ptr = s_frame_verts, .size = (size_t)vert_count * sizeof(GlyphVertex) });

        float uniforms[4] = { (float)win_w, (float)win_h,
                              (float)cell_w, (float)cell_h };
        sg_apply_pipeline(d->glyph_pip);
        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = d->glyph_vbuf,
            .views[0] = d->atlas.texture_view,
            .samplers[0] = d->atlas.sampler,
        });
        sg_apply_uniforms(0, &SG_RANGE(uniforms));

        // Pass 1: draw all background quads (opaque, replace)
        if (bg_vert_count > 0) {
            sg_draw(0, bg_vert_count, 1);
        }
        // Lottie: upload vertex buffer once, then render both layers
        int lottie_bg_count = 0;
        if (d->lottie_pip_created && terminal_lottie_count(term) > 0) {
            int anim_count = 0;
            const CfrLottie *anims = terminal_get_lotties(term, &anim_count);
            lottie_cache_reconcile(d, anims, anim_count);

            // Upload the full lottie vertex buffer before any draws
            sg_update_buffer(d->lottie_vbuf, &(sg_range){
                                                 .ptr = s_lottie_verts,
                                                 .size = SOKOL_MAX_LOTTIE_VERTICES * sizeof(GlyphVertex),
                                             });

            // Draw background lottie (between bg and glyph quads)
            lottie_bg_count = sokol_render_lottie_layer(d, term, 1, 0);
        }
        // Pass 2: draw terminal grid glyph/cursor quads (opaque, replace)
        int terminal_glyph_count = hint_glyph_start;
        if (terminal_glyph_count > 0) {
            sg_draw(glyph_vert_start, terminal_glyph_count, 1);
        }
        // Pass 3: draw decoration quads (alpha-blended, on top of terminal
        // glyphs). This includes underlines, strikethroughs, and the
        // notification accent stripe, but excludes the hover hint panel
        // background which is drawn later.
        int deco_main_count = hint_deco_start;
        if (deco_main_count > 0 && d->sel_pip_created) {
            sg_apply_pipeline(d->sel_pip);
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = d->glyph_vbuf,
                .views[0] = d->atlas.texture_view,
                .samplers[0] = d->atlas.sampler,
            });
            sg_apply_uniforms(0, &SG_RANGE(uniforms));
            sg_draw(deco_vert_start, deco_main_count, 1);
        }
        // Pass 4: draw selection overlay quads (alpha-blended, on top of
        // decorations so selected text remains readable).
        int sel_count = vert_count - sel_vert_start;
        if (sel_count > 0 && d->sel_pip_created) {
            sg_apply_pipeline(d->sel_pip);
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = d->glyph_vbuf,
                .views[0] = d->atlas.texture_view,
                .samplers[0] = d->atlas.sampler,
            });
            sg_apply_uniforms(0, &SG_RANGE(uniforms));
            sg_draw(sel_vert_start, sel_count, 1);
        }
        // Pass 5: draw hover hint panel background (alpha-blended, on top of
        // the finished terminal frame but behind the panel text).
        if (hint_deco_count > 0 && d->sel_pip_created) {
            sg_apply_pipeline(d->sel_pip);
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = d->glyph_vbuf,
                .views[0] = d->atlas.texture_view,
                .samplers[0] = d->atlas.sampler,
            });
            sg_apply_uniforms(0, &SG_RANGE(uniforms));
            sg_draw(deco_vert_start + hint_deco_start, hint_deco_count, 1);
        }
        // Pass 6: draw hover hint panel text glyphs (opaque, replace) on top of
        // the panel background.
        int hint_glyph_count = glyph_end - (glyph_vert_start + hint_glyph_start);
        if (hint_glyph_count > 0) {
            sg_apply_pipeline(d->glyph_pip);
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = d->glyph_vbuf,
                .views[0] = d->atlas.texture_view,
                .samplers[0] = d->atlas.sampler,
            });
            sg_apply_uniforms(0, &SG_RANGE(uniforms));
            sg_draw(glyph_vert_start + hint_glyph_start, hint_glyph_count, 1);
        }
        // Pass 7: draw foreground lottie (on top of everything)
        if (lottie_bg_count >= 0 && d->lottie_pip_created && terminal_lottie_count(term) > 0)
            (void)sokol_render_lottie_layer(d, term, 0, lottie_bg_count);
        // Pass 5: draw sixel images (on top of text and lottie)
        if (d->lottie_pip_created) {
            sokol_ensure_sixel_vbuf(d);
            sokol_render_sixel_images(d, term);
        }
    }

    // Screenshot automation: defer to after sg_commit (glReadPixels inside
    // the pass triggers VALIDATION_FAILED panics).
    if (d->screenshot_frames > 0 && !d->screenshot_saved) {
        d->screenshot_frames--;
        if (d->screenshot_frames == 0) {
            d->screenshot_saved = true;
            d->debug_pending_screendump = true;
            snprintf(d->debug_screendump_path,
                     sizeof(d->debug_screendump_path), "%s",
                     d->screenshot_path);
        }
    }

    sg_end_pass();
}

static void sokol_present(PorttyBackend *self)
{
    (void)self;
    sg_commit();
}

static void sokol_resize(PorttyBackend *self, int w, int h)
{
    (void)w;
    (void)h;
    SokolData *d = sokol_data(self);
    if (!d)
        return;
    // Sokol handles the GL swapchain internally via sglue_swapchain().
    // Rebuild notification panel at new width.
    if (d->notif_active && (d->notif_title || d->notif_body))
        sokol_notif_rebuild(d);
    // Rebuild hover link hint panel at new width.
    if (d->hint_active)
        sokol_hint_rebuild(d);
}

static bool sokol_get_cell_size(PorttyBackend *self, int *cw, int *ch)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return false;
    if (cw)
        *cw = d->cell_w;
    if (ch)
        *ch = d->cell_h;
    return true;
}

// ── Font loading ──────────────────────────────────────────────────────────

static int sokol_load_fonts(PorttyBackend *self, float size,
                            const char *name, int ft_hint_target)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return -1;

    d->font = &font_backend_ft;
    if (!font_init(d->font)) {
        fprintf(stderr, "ERROR: Failed to initialize font backend\n");
        return -1;
    }

#ifdef _WIN32
    extern FontResolveBackend font_resolve_backend_w32;
    d->resolve = font_resolve_init(&font_resolve_backend_w32);
#elif defined(__APPLE__)
    extern FontResolveBackend font_resolve_backend_ct;
    d->resolve = font_resolve_init(&font_resolve_backend_ct);
#else
    extern FontResolveBackend font_resolve_backend_fc;
    d->resolve = font_resolve_init(&font_resolve_backend_fc);
#endif
    if (!d->resolve) {
        fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
        font_destroy(d->font);
        d->font = NULL;
        return -1;
    }

    const char *hint_name = "none";
    if (ft_hint_target == FT_LOAD_TARGET_LIGHT)
        hint_name = "light";
    else if (ft_hint_target == FT_LOAD_TARGET_NORMAL)
        hint_name = "normal";
    else if (ft_hint_target == FT_LOAD_TARGET_MONO)
        hint_name = "mono";
    snprintf(d->hint_name, sizeof(d->hint_name), "%s", hint_name);

    RendFontLoadResult r = { 0 };
    if (rend_load_fonts(&r, d->font, d->resolve, size, name,
                        ft_hint_target, d->content_scale, hint_name) != 0) {
        font_resolve_destroy(d->resolve);
        d->resolve = NULL;
        font_destroy(d->font);
        d->font = NULL;
        return -1;
    }

    d->font_ascent = r.font_ascent;
    d->font_descent = r.font_descent;
    d->font_cap_height = r.font_cap_height;
    d->cell_w = r.cell_width;
    d->cell_h = r.cell_height;
    d->font_size = r.font_size;
    d->font_options = r.font_options;
    free(d->font_path);
    d->font_path = r.font_path;
    r.font_path = NULL;

    return 0;
}

static void sokol_scroll(PorttyBackend *self, TerminalBackend *term,
                         int delta)
{
    SokolData *d = sokol_data(self);
    if (d)
        rend_scroll(&d->scroll, term, delta);
}

static void sokol_reset_scroll(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (d)
        rend_reset_scroll(&d->scroll);
}

static int sokol_get_scroll_offset(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    return d ? rend_get_scroll_offset(&d->scroll) : 0;
}

static void sokol_set_content_scale(PorttyBackend *self, float scale)
{
    SokolData *d = sokol_data(self);
    if (d)
        d->content_scale = scale;
}

// ── Pager overlay ────────────────────────────────────────────────────────

static void sokol_set_overlay(PorttyBackend *self, TerminalBackend *overlay)
{
    SokolData *d = sokol_data(self);
    if (d)
        rend_set_overlay(&d->scroll, overlay);
}

static void sokol_clear_overlay(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (d)
        rend_clear_overlay(&d->scroll);
}

static bool sokol_has_overlay(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    return d ? rend_has_overlay(&d->scroll) : false;
}

// ── Offscreen rendering ──────────────────────────────────────────────────

static int sokol_render_to_png(PorttyBackend *self, TerminalBackend *term,
                               const char *path)
{
    SokolData *d = sokol_data(self);
    if (!d || !term || !path)
        return -1;

    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);
    if (rows <= 0 || cols <= 0)
        return -1;

    int cell_w = d->cell_w;
    int cell_h = d->cell_h;
    if (cell_w <= 0 || cell_h <= 0)
        return -1;

    int img_w = cols * cell_w;
    int img_h = rows * cell_h;

    // Render the terminal into the current framebuffer, then read it back.
    // This must be called from within the Sokol frame callback (i.e. after
    // sg_begin_pass is possible). We call draw_terminal which sets up its
    // own pass, then read pixels after sg_end_pass but before sg_commit.
    //
    // However, render_to_png may be called outside the frame callback.
    // In that case we do a standalone pass here.
    terminal_flush_damage(term);

    sokol_ensure_glyph_pipeline(d);
    if (!d->atlas.texture_created) {
        if (!rend_sokol_atlas_init(&d->atlas, d->linear_ok))
            return -1;
    }
    rend_sokol_atlas_begin_frame(&d->atlas);

    // Build vertex data (same logic as sokol_draw_terminal but simplified)
    static GlyphVertex verts[SOKOL_MAX_VERTICES];
    int vert_count = 0;
    float atlas_size = (float)REND_ATLAS_TEXTURE_SIZE;

    for (int row = 0; row < rows && vert_count + 6 <= SOKOL_MAX_VERTICES; row++) {
        for (int col = 0; col < cols && vert_count + 6 <= SOKOL_MAX_VERTICES; col++) {
            TerminalCell cell;
            if (terminal_get_cell(term, row, col, &cell) != 0)
                continue;
            if (cell.width == 0)
                continue;

            uint8_t fg[4], bg[4];
            bool rev = cell.attrs.reverse;
            cell_color(cell.fg, true, rev, fg);
            cell_color(cell.bg, false, rev, bg);

            // Dim/faint (SGR 2): blend foreground toward background at 40% opacity
            if (cell.attrs.dim) {
                fg[0] = (uint8_t)(fg[0] * 0.4f + bg[0] * 0.6f);
                fg[1] = (uint8_t)(fg[1] * 0.4f + bg[1] * 0.6f);
                fg[2] = (uint8_t)(fg[2] * 0.4f + bg[2] * 0.6f);
            }

            float x0 = (float)(col * cell_w);
            float y0 = (float)(row * cell_h);
            float x1 = x0 + (float)(cell.width * cell_w);
            float y1 = y0 + (float)cell_h;
            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;

            if (cell.cp != 0 && cell.cp != 0x20 && d->font && !cell.attrs.invis) {
                FontStyle style = FONT_STYLE_NORMAL;
                if (cell.attrs.bold && cell.attrs.italic)
                    style = FONT_STYLE_BOLD_ITALIC;
                else if (cell.attrs.bold)
                    style = FONT_STYLE_BOLD;
                else if (cell.attrs.italic)
                    style = FONT_STYLE_ITALIC;
                if (!font_has_style(d->font, style))
                    style = FONT_STYLE_NORMAL;

                uint32_t glyph_id = font_get_glyph_index(d->font, style, cell.cp);
                uint32_t color_key = 0;
                RendSokolAtlasEntry *entry = rend_sokol_atlas_lookup(
                    &d->atlas, d->font->font_data[style], (int)glyph_id, color_key);

                if (!entry) {
                    GlyphBitmap *bmp = font_render_glyphs(
                        d->font, style, &cell.cp, 1, 255, 255, 255);
                    if (bmp) {
                        entry = rend_sokol_atlas_insert(
                            &d->atlas, d->font->font_data[style],
                            (int)glyph_id, color_key, bmp, false);
                        d->font->free_glyph_bitmap(d->font, bmp);
                    } else {
                        entry = rend_sokol_atlas_insert_empty(
                            &d->atlas, d->font->font_data[style],
                            (int)glyph_id, color_key);
                    }
                }

                if (entry && entry->region.w > 0 && entry->region.h > 0) {
                    int gx = (int)x0 + entry->x_offset;
                    int gy = (int)y0 + d->font_ascent - entry->y_offset;
                    if (entry->centered) {
                        int glyph_w = cell.width * cell_w;
                        gx = (int)x0 + (glyph_w - entry->region.w) / 2;
                        gy = (int)y0 + (cell_h - entry->region.h) / 2;
                    }
                    x0 = (float)gx;
                    y0 = (float)gy;
                    x1 = x0 + (float)entry->region.w;
                    y1 = y0 + (float)entry->region.h;
                    u0 = (float)entry->region.x / atlas_size;
                    v0 = (float)entry->region.y / atlas_size;
                    u1 = (float)(entry->region.x + entry->region.w) / atlas_size;
                    v1 = (float)(entry->region.y + entry->region.h) / atlas_size;
                } else {
                    x0 = (float)(col * cell_w);
                    y0 = (float)(row * cell_h);
                    x1 = x0 + (float)(cell.width * cell_w);
                    y1 = y0 + (float)cell_h;
                    u0 = u1 = v0 = v1 = 0;
                }
            }

            float vu0 = v0;
            float vu1 = v1;
            GlyphVertex *q = &verts[vert_count];
            q[0] = (GlyphVertex){ x0, y0, u0, vu0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[1] = (GlyphVertex){ x1, y0, u1, vu0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[2] = (GlyphVertex){ x1, y1, u1, vu1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[3] = (GlyphVertex){ x0, y0, u0, vu0, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[4] = (GlyphVertex){ x1, y1, u1, vu1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            q[5] = (GlyphVertex){ x0, y1, u0, vu1, { fg[0], fg[1], fg[2], fg[3] }, { bg[0], bg[1], bg[2], bg[3] } };
            vert_count += 6;
        }
    }

    rend_sokol_atlas_flush(&d->atlas);

    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                           .clear_value = { rend_srgb_to_linear(DEF_BG_R) / 255.0f,
                                            rend_srgb_to_linear(DEF_BG_G) / 255.0f,
                                            rend_srgb_to_linear(DEF_BG_B) / 255.0f, 1.0f } } },
        .swapchain = sglue_swapchain(),
    });

    if (vert_count > 0 && d->glyph_pip_created) {
        sg_update_buffer(d->glyph_vbuf, &(sg_range){ .ptr = verts, .size = (size_t)vert_count * sizeof(GlyphVertex) });
        float resolution[2] = { (float)img_w, (float)img_h };
        sg_apply_pipeline(d->glyph_pip);
        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = d->glyph_vbuf,
            .views[0] = d->atlas.texture_view,
            .samplers[0] = d->atlas.sampler,
        });
        sg_apply_uniforms(0, &SG_RANGE(resolution));
        sg_draw(0, vert_count, 1);
    }

    // Read framebuffer while the pass is still active
    int rc = -1;
    uint8_t *pixels = malloc((size_t)img_w * img_h * 4);
    if (pixels) {
#if defined(SOKOL_GLCORE)
        glFinish();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, img_w, img_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        // Flip rows for PNG (top-to-bottom)
        uint8_t *flipped = malloc((size_t)img_w * img_h * 4);
        if (flipped) {
            for (int y = 0; y < img_h; y++) {
                memcpy(flipped + (size_t)y * img_w * 4,
                       pixels + (size_t)(img_h - 1 - y) * img_w * 4,
                       (size_t)img_w * 4);
            }
            rc = png_write_rgba(path, flipped, img_w, img_h);
            free(flipped);
        } else {
            rc = png_write_rgba(path, pixels, img_w, img_h);
        }
#else
        // D3D11/Metal screendump not yet implemented
        (void)path;
#endif
        free(pixels);
    }

    sg_end_pass();
    sg_commit();

    if (rc == 0)
        fprintf(stderr, "STATUS: png_output=%s (%dx%d)\n", path, img_w, img_h);
    else
        fprintf(stderr, "ERROR: Failed to write PNG to %s\n", path);

    return rc;
}

// ── Diagnostics ───────────────────────────────────────────────────────────

static void sokol_log_stats(PorttyBackend *self)
{
    (void)self;
}

static bool sokol_get_diag(PorttyBackend *self, PorttyDiag *out)
{
    if (!self || !self->data || !out)
        return false;
    SokolData *d = sokol_data(self);
    out->platform_name = self->name;
#if defined(SOKOL_GLCORE)
    out->backend_name = "Sokol (OpenGL 4.1 Core)";
#elif defined(SOKOL_GLES3)
    out->backend_name = "Sokol (OpenGL ES 3.0)";
#elif defined(SOKOL_METAL)
    out->backend_name = "Sokol (Metal)";
#elif defined(SOKOL_D3D11)
    out->backend_name = "Sokol (Direct3D 11)";
#elif defined(SOKOL_VULKAN)
    out->backend_name = "Sokol (Vulkan)";
#elif defined(SOKOL_WGPU)
    out->backend_name = "Sokol (WebGPU)";
#else
    out->backend_name = "Sokol (Unknown)";
#endif

    // Query GPU device name and driver/version. Each backend needs a
    // different API. The strings are stored in static buffers so they
    // remain valid for the caller.
    static char gpu_name[256];
    static char gpu_driver[256];
    gpu_name[0] = '\0';
    gpu_driver[0] = '\0';
    GpuDriverLibre driver_libre = GPU_DRIVER_LIBRE_UNKNOWN;

#if defined(SOKOL_GLCORE) || defined(SOKOL_GLES3)
    // OpenGL / OpenGL ES: glGetString gives us everything.
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    if (renderer && *renderer)
        snprintf(gpu_name, sizeof(gpu_name), "%s", renderer);
    if (version && *version)
        snprintf(gpu_driver, sizeof(gpu_driver), "%s", version);
    driver_libre = rend_classify_gpu_driver_libre(gpu_driver, renderer)
                       ? GPU_DRIVER_LIBRE_YES
                       : GPU_DRIVER_LIBRE_NO;

#elif defined(SOKOL_METAL) && defined(__APPLE__)
// Metal: query the default MTLDevice for its name. The device
// pointer is obtained via sg_mtl_device() and bridged to
// id<MTLDevice>.
#import <Metal/Metal.h>
    id<MTLDevice> dev = (__bridge id<MTLDevice>)sg_mtl_device();
    if (dev) {
        const char *name = [[dev name] UTF8String];
        if (name && *name)
            snprintf(gpu_name, sizeof(gpu_name), "%s", name);
        // Metal has no separate "driver" string; report the macOS
        // Metal version via MTLCopyAllDevices or device registry ID.
        // For now, use a descriptive placeholder.
        snprintf(gpu_driver, sizeof(gpu_driver), "Metal");
        driver_libre = GPU_DRIVER_LIBRE_YES; // Metal is Apple's first-party API
    }

#elif defined(SOKOL_D3D11) && defined(_WIN32)
    // Direct3D 11: query the DXGI adapter for the GPU description.
    // sg_d3d11_device() returns the ID3D11Device pointer. We query
    // it for the IDXGIDevice interface, then GetAdapter, then
    // GetDesc for the adapter name.
    ID3D11Device *d3d_dev = (ID3D11Device *)sg_d3d11_device();
    if (d3d_dev) {
        IDXGIDevice *dxgi_dev = NULL;
        if (SUCCEEDED(d3d_dev->lpVtbl->QueryInterface(
                d3d_dev, &IID_IDXGIDevice, (void **)&dxgi_dev))) {
            IDXGIAdapter *adapter = NULL;
            if (SUCCEEDED(dxgi_dev->lpVtbl->GetAdapter(dxgi_dev, &adapter))) {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(adapter->lpVtbl->GetDesc(adapter, &desc))) {
                    // desc.Description is a WCHAR[]; convert to UTF-8
                    char name_utf8[256];
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                        name_utf8, sizeof(name_utf8),
                                        NULL, NULL);
                    if (name_utf8[0])
                        snprintf(gpu_name, sizeof(gpu_name), "%s",
                                 name_utf8);
                    snprintf(gpu_driver, sizeof(gpu_driver),
                             "Dedicated VRAM: %zu MB",
                             (size_t)(desc.DedicatedVideoMemory /
                                      (1024 * 1024)));
                }
                adapter->lpVtbl->Release(adapter);
            }
            dxgi_dev->lpVtbl->Release(dxgi_dev);
        }
    }
    driver_libre = GPU_DRIVER_LIBRE_NO; // D3D11 drivers are typically proprietary

#elif defined(SOKOL_VULKAN)
    // Vulkan: query the physical device properties. Sokol's Vulkan
    // backend stores the VkPhysicalDevice internally but doesn't
    // expose it via a public API. We use vkGetPhysicalDeviceProperties
    // if we can obtain the device. Since sg doesn't expose the
    // VkPhysicalDevice, we skip this for now — the GPU name will
    // be empty. A future improvement would add sg_vk_physical_device()
    // to Sokol, or use VK_EXT_tooling_info / VK_KHR_driver_properties.
    //
    // As a fallback, leave gpu_name empty; the diagnostics panel
    // will show "(unavailable)" for Vulkan.
    driver_libre = GPU_DRIVER_LIBRE_UNKNOWN;
    // WebGPU: the WGPUDevice doesn't expose a GPU name directly.
    // On native (wgpu-native), the adapter has a name, but there's
    // no standard WebGPU API to query it. Leave empty. The libre
    // flag is unknown since dawn/wgpu-native is just an abstraction
    // over the real kernel-level driver.
    driver_libre = GPU_DRIVER_LIBRE_UNKNOWN;

#endif

    out->gpu_device = gpu_name[0] ? gpu_name : NULL;
    out->gpu_driver = gpu_driver[0] ? gpu_driver : NULL;
    out->gpu_driver_libre = driver_libre;
    out->linear_light = d->linear_ok;
    out->glyph_shader = false;
    out->content_scale = d->content_scale;
    out->pixel_width = (int)sapp_width();
    out->pixel_height = (int)sapp_height();
    out->cell_width = d->cell_w;
    out->cell_height = d->cell_h;
    out->font_path = d->font_path;
    out->hinting = d->hint_name[0] ? d->hint_name : NULL;

    // Display / scaling info
    out->display_session = NULL;
    out->display_xwayland = NULL;
    out->display_screen = NULL;
    out->display_dpi = NULL;
    out->display_scale = NULL;
    out->display_physical = display_info_get_physical();

    {
        static char scale_str[128];
        static char dpi_str[128];

        // Scale info — available on all platforms via sokol
        snprintf(scale_str, sizeof(scale_str), "sapp_dpi_scale %.2f, high_dpi %s",
                 sapp_dpi_scale(), sapp_high_dpi() ? "true" : "false");
        out->display_scale = scale_str;

        // DPI estimate from sapp_dpi_scale (relative to 96 DPI)
        float dpi = sapp_dpi_scale() * 96.0f;
        if (dpi > 0.0f) {
            snprintf(dpi_str, sizeof(dpi_str), "%.1f (from dpi_scale)", dpi);
            out->display_dpi = dpi_str;
        }
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    {
        static char session_str[16];
        static char xwayland_str[8];
        static char screen_str[128];
        static char dpi_str[128];

        const char *xdg_session = getenv("XDG_SESSION_TYPE");
        if (xdg_session && *xdg_session) {
            snprintf(session_str, sizeof(session_str), "%s", xdg_session);
            out->display_session = session_str;
        }

        bool is_xwayland = false;
        if (xdg_session && strcmp(xdg_session, "wayland") == 0)
            is_xwayland = true;

        snprintf(xwayland_str, sizeof(xwayland_str), "%s", is_xwayland ? "yes" : "no");
        out->display_xwayland = xwayland_str;

        Display *xdisp = (Display *)sapp_x11_get_display();
        if (xdisp) {
            int screen = DefaultScreen(xdisp);
            int scr_w = DisplayWidth(xdisp, screen);
            int scr_h = DisplayHeight(xdisp, screen);
            int scr_wmm = DisplayWidthMM(xdisp, screen);
            int scr_hmm = DisplayHeightMM(xdisp, screen);
            float phys_dpi = scr_wmm > 0 ? (float)scr_w * 25.4f / (float)scr_wmm : 0.0f;
            snprintf(screen_str, sizeof(screen_str), "%dx%d px, %dx%d mm",
                     scr_w, scr_h, scr_wmm, scr_hmm);
            out->display_screen = screen_str;

            float xft_dpi = 0.0f;
            char *rms = XResourceManagerString(xdisp);
            if (rms) {
                XrmDatabase db = XrmGetStringDatabase(rms);
                if (db) {
                    XrmValue value;
                    char *type = NULL;
                    if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value)) {
                        if (type && strcmp(type, "String") == 0)
                            xft_dpi = (float)atof(value.addr);
                    }
                    XrmDestroyDatabase(db);
                }
            }
            if (xft_dpi > 0.0f)
                snprintf(dpi_str, sizeof(dpi_str), "physical %.1f, Xft.dpi %.0f",
                         phys_dpi, xft_dpi);
            else
                snprintf(dpi_str, sizeof(dpi_str), "physical %.1f, Xft.dpi (unset)",
                         phys_dpi);
            out->display_dpi = dpi_str;
        }
    }
#elif defined(__APPLE__)
    out->display_session = "macOS";
#elif defined(_WIN32)
    out->display_session = "windows";
#endif

    return true;
}

PorttyBackend backend_sokol = {
    .name = "sokol",
    .data = NULL,
    .init = sokol_init,
    .run = NULL,
    .destroy = sokol_destroy,
    .request_quit = sokol_request_quit,

    .clipboard_get = sokol_clipboard_get,
    .clipboard_set = sokol_clipboard_set,
    .clipboard_free = sokol_clipboard_free,
    .clipboard_paste_async = sokol_clipboard_paste_async,

    .register_pty = sokol_register_pty,
    .pause_pty = sokol_pause_pty,
    .resume_pty = sokol_resume_pty,

    .set_window_title = sokol_set_window_title,
    .set_window_size = sokol_set_window_size,
    .show_window = NULL, // Sokol shows the window automatically
    .get_drawable_size = sokol_get_drawable_size,
    .get_display_scale = sokol_get_display_scale,
    .get_display_size = sokol_get_display_size,

    .set_cursor = sokol_set_cursor,
    .open_url = sokol_open_url,
    .set_autoscroll = sokol_set_autoscroll,
    .spawn_new_terminal = sokol_spawn_new_terminal,
    .set_working_dir = sokol_set_working_dir,
    .get_exe_path = sokol_get_exe_path,
    .get_default_font = sokol_get_default_font,

    .notify = sokol_notify,
    .notify_dismiss = sokol_notify_dismiss,
    .set_link_hint = sokol_set_link_hint,
    .notification_hit = sokol_notification_hit,
    .set_notification_hover = sokol_set_notification_hover,

    .load_fonts = sokol_load_fonts,
    .draw_terminal = sokol_draw_terminal,
    .present = sokol_present,
    .resize = sokol_resize,
    .get_cell_size = sokol_get_cell_size,
    .scroll = sokol_scroll,
    .reset_scroll = sokol_reset_scroll,
    .get_scroll_offset = sokol_get_scroll_offset,
    .set_content_scale = sokol_set_content_scale,

    .set_overlay = sokol_set_overlay,
    .clear_overlay = sokol_clear_overlay,
    .has_overlay = sokol_has_overlay,

    .render_to_png = sokol_render_to_png,

    .log_stats = sokol_log_stats,
    .get_diag = sokol_get_diag,
};

// ── Debug helpers ───────────────────────────────────────────────────────

static void sokol_debug_screendump(SokolData *d, const char *path)
{
    (void)d;
#if !defined(SOKOL_GLCORE)
    (void)path;
    vlog("screendump: not supported on this backend\n");
    return;
#else
    int ss_w = (int)sapp_width();
    int ss_h = (int)sapp_height();
    uint8_t *pixels = malloc((size_t)ss_w * ss_h * 4);
    if (!pixels)
        return;

    glFinish();
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, ss_w, ss_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // Flip vertically (GL origin is bottom-left, PNG expects top-to-bottom)
    uint8_t *flipped = malloc((size_t)ss_w * ss_h * 4);
    if (flipped) {
        for (int y = 0; y < ss_h; y++) {
            memcpy(flipped + (size_t)y * ss_w * 4,
                   pixels + (size_t)(ss_h - 1 - y) * ss_w * 4,
                   (size_t)ss_w * 4);
        }
        png_write_rgba(path, flipped, ss_w, ss_h);
        free(flipped);
    } else {
        png_write_rgba(path, pixels, ss_w, ss_h);
    }
    free(pixels);
    vlog("screendump: saved %s (%dx%d)\n", path, ss_w, ss_h);
#endif
}

static void sokol_debug_mousemove(void *app, int x, int y)
{
    PorttyApp *p = (PorttyApp *)app;
    portty_app_handle_mouse(p, x, y, 0, false, 0, 0);
    if (portty_app_revalidate_hover(p, x, y))
        terminal_mark_dirty(p->term);
}

static void sokol_debug_notify(void *backend, const char *title,
                               const char *body)
{
    PorttyBackend *self = (PorttyBackend *)backend;
    sokol_notify(self, title, body, PORTTY_NOTIFY_INFO);
}

static void sokol_debug_dumpverts(int row, int col_start, int col_end)
{
    for (int col = col_start; col <= col_end; col++) {
        if (row < 0 || row >= SOKOL_MAX_ROWS || col < 0 || col >= SOKOL_MAX_COLS) {
            printf("  col=%3d: out of range\n", col);
            continue;
        }
        int vi = s_vert_index[row][col];
        if (vi < 0 || vi + 5 >= s_frame_vert_count) {
            printf("  col=%3d: no vertices (vi=%d)\n", col, vi);
            continue;
        }
        GlyphVertex *q = &s_frame_verts[vi];
        printf("  col=%3d vi=%d: pos=(%.0f,%.0f)-(%.0f,%.0f) "
               "fg=[%d,%d,%d,%d] bg=[%d,%d,%d,%d]\n",
               col, vi,
               q[0].x, q[0].y, q[2].x, q[2].y,
               q[0].fg[0], q[0].fg[1], q[0].fg[2], q[0].fg[3],
               q[0].bg[0], q[0].bg[1], q[0].bg[2], q[0].bg[3]);
        // Hex dump for raw verification
        uint8_t *raw = (uint8_t *)q;
        printf("    hex: ");
        for (int i = 0; i < 24; i++)
            printf("%02X ", raw[i]);
        printf("\n");
    }
}

static void sokol_debug_verifybuf(SokolData *d, int row, int col_start,
                                  int col_end)
{
#if !defined(SOKOL_GLCORE)
    (void)d;
    (void)row;
    (void)col_start;
    (void)col_end;
    printf("verifybuf: not supported on this backend\n");
    return;
#else
    GLuint gl_buf = d->debug_glyph_vbuf_gl_id;
    if (!gl_buf) {
        printf("verifybuf: GL buffer ID not available\n");
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, gl_buf);

    for (int col = col_start; col <= col_end; col++) {
        if (row < 0 || row >= SOKOL_MAX_ROWS || col < 0 || col >= SOKOL_MAX_COLS)
            continue;
        int vi = s_vert_index[row][col];
        if (vi < 0)
            continue;

        size_t offset = (size_t)vi * sizeof(GlyphVertex);
        void *mapped = glMapBufferRange(GL_ARRAY_BUFFER, offset,
                                        sizeof(GlyphVertex), GL_MAP_READ_BIT);
        if (!mapped) {
            printf("  col=%3d vi=%d: FAILED to map GPU buffer\n", col, vi);
            continue;
        }
        GlyphVertex gpu_vert;
        memcpy(&gpu_vert, mapped, sizeof(GlyphVertex));
        glUnmapBuffer(GL_ARRAY_BUFFER);

        GlyphVertex *cpu_vert = &s_frame_verts[vi];
        bool match = (memcmp(cpu_vert, &gpu_vert, sizeof(GlyphVertex)) == 0);

        printf("  col=%3d vi=%d %s\n", col, vi, match ? "MATCH" : "MISMATCH");
        if (!match) {
            printf("    CPU bg: [%d,%d,%d,%d]  GPU bg: [%d,%d,%d,%d]\n",
                   cpu_vert->bg[0], cpu_vert->bg[1],
                   cpu_vert->bg[2], cpu_vert->bg[3],
                   gpu_vert.bg[0], gpu_vert.bg[1],
                   gpu_vert.bg[2], gpu_vert.bg[3]);
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
#endif
}

// ── sokol_app.h callbacks ────────────────────────────────────────────────

static struct
{
    PorttyApp *app;
    PorttyBackend *backend;
} g_sokol;

static void sokol_finish_setup(PorttyBackend *self, PorttyApp *app)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;

    char *desktop_font = NULL;
    const char *font_name = app->font_name;
    const char *font_source = "fontconfig generic (no desktop default)";
    if (!font_name) {
        desktop_font = self->get_default_font(self);
        if (desktop_font) {
            font_name = desktop_font;
            font_source = "desktop default";
        }
    } else {
        font_source = app->conf->font ? "config file" : "-f flag";
    }
    app->font_source = font_source;

    float display_scale = self->get_display_scale(self);
    if (app->dpi_scale != 1.0f && display_scale > 0.0f)
        display_scale *= app->dpi_scale;
    if (display_scale > 0.0f)
        self->set_content_scale(self, display_scale);

    // Convert hinting enum to FT int (moved here from main_sokol.c)
    static const int hint_map[] = { FT_LOAD_NO_HINTING, FT_LOAD_TARGET_LIGHT,
                                    FT_LOAD_TARGET_NORMAL, FT_LOAD_TARGET_MONO };
    int ft_hint = (app->conf->hinting != PORTTY_HINT_UNSET)
                      ? hint_map[app->conf->hinting]
                      : FT_LOAD_TARGET_LIGHT;

    if (self->load_fonts(self, app->font_size, font_name,
                         ft_hint) < 0) {
        fprintf(stderr, "Failed to load fonts\n");
        free(desktop_font);
        return;
    }
    free(desktop_font);

    int cell_w, cell_h;
    int win_w = 800, win_h = 600;
    int term_rows = 0, term_cols = 0;
    terminal_get_dimensions(app->term, &term_rows, &term_cols);
    if (self->get_cell_size(self, &cell_w, &cell_h)) {
        terminal_set_cell_px(app->term, cell_w, cell_h);
        terminal_set_content_scale(app->term, d->content_scale > 0.0f ? d->content_scale : 1.0f);
        win_w = term_cols * cell_w;
        win_h = term_rows * cell_h;
        vlog("Derived window size from font: %dx%d\n", win_w, win_h);

        int disp_w, disp_h;
        if (self->get_display_size(self, &disp_w, &disp_h)) {
            if (win_w > disp_w || win_h > disp_h) {
                if (win_w > disp_w)
                    win_w = disp_w;
                if (win_h > disp_h)
                    win_h = disp_h;
                int cols = win_w / cell_w;
                int rows = win_h / cell_h;
                if (cols < 1)
                    cols = 1;
                if (rows < 1)
                    rows = 1;
                terminal_resize(app->term, cols, rows);
                win_w = cols * cell_w;
                win_h = rows * cell_h;
            }
        }
    }
    self->set_window_size(self, win_w, win_h);
    self->resize(self, win_w, win_h);

    // Keep cell_w/cell_h as the per-cell pixel dimensions (10x20 default),
    // not the window dimensions.
    if (self->get_cell_size(self, &d->cell_w, &d->cell_h)) {
        // already set via get_cell_size
    }

    if (app->demo_text) {
        terminal_process_input(app->term, app->demo_text,
                               strlen(app->demo_text));
    } else {
        PtyContext *pty = pty_create(term_rows, term_cols, app->exec_argv);
        if (!pty) {
            fprintf(stderr, "ERROR: Failed to create PTY\n");
            return;
        }
        app->pty = pty;
        d->pty = pty;
        if (!self->register_pty(self, pty)) {
            fprintf(stderr, "ERROR: Failed to register PTY with backend\n");
            pty_destroy(pty);
            app->pty = NULL;
            d->pty = NULL;
            return;
        }
    }

    terminal_set_output_callback(app->term, portty_app_term_output_to_pty,
                                 app);
    terminal_set_selection_callback(app->term, portty_app_selection_change,
                                    app);
    terminal_set_clipboard_set_callback(app->term, portty_app_clipboard_set,
                                        app);
    terminal_set_cwd_callback(app->term, portty_app_cwd_change, app);

    app->pager = pager_create(self, app);
    app->backend = self;

    // Start cursor blink timer
    d->cursor_blink_visible = true;
    d->cursor_blink_timer = timer_add(d->timers, CURSOR_BLINK_INTERVAL_MS,
                                      SOKOL_EVENT_CURSOR_BLINK, NULL);

    // Load debug script if specified
    if (app->script_path) {
        d->debug_script = portty_debug_script_load(app->script_path);
        if (!d->debug_script) {
            fprintf(stderr, "ERROR: Failed to load debug script: %s: out of memory\n",
                    app->script_path);
            exit(1);
        }
        const char *err = portty_debug_script_error(d->debug_script);
        if (err) {
            fprintf(stderr, "ERROR: %s: %s\n", app->script_path, err);
            exit(1);
        }
    }

    // Ensure the very first frame is rendered.  coffer starts with no damage
    // pending, so without an explicit mark the window would stay blank until
    // the next PTY/timer event.
    terminal_mark_dirty(d->term);

    vlog("sokol_finish_setup: complete, win=%dx%d\n", win_w, win_h);
}

// ── Notification panel ───────────────────────────────────────────────────

static char *sokol_notif_build_ansi(SokolData *d)
{
    SokolStrBuf sb;
    if (!sokol_strbuf_init(&sb))
        return NULL;

    // Set panel background color on every cell.
    // Do NOT use \x1b[0m (full reset) — it clears bg to default (black).
    // Use targeted resets: \x1b[22m (normal intensity), \x1b[39m (default fg).
    const char *panel_bg = "\x1b[48;2;38;38;44m";

    // Title (bold, bright white on panel bg)
    if (d->notif_title) {
        if (!sokol_strbuf_appendf(&sb,
                                  "%s\x1b[1m\x1b[38;2;236;236;241m%s\x1b[22m\x1b[39m",
                                  panel_bg, d->notif_title)) {
            sokol_strbuf_free(&sb);
            return NULL;
        }
    }
    // Body (normal, dim gray on panel bg)
    if (d->notif_body) {
        if (d->notif_title) {
            if (!sokol_strbuf_appendf(&sb, "\r\n%s", panel_bg)) {
                sokol_strbuf_free(&sb);
                return NULL;
            }
        } else {
            if (!sokol_strbuf_appendf(&sb, "%s", panel_bg)) {
                sokol_strbuf_free(&sb);
                return NULL;
            }
        }
        if (!sokol_strbuf_appendf(&sb, "\x1b[38;2;190;190;198m%s\x1b[39m",
                                  d->notif_body)) {
            sokol_strbuf_free(&sb);
            return NULL;
        }
    }
    return sokol_strbuf_finish(&sb);
}

static void sokol_notif_rebuild(SokolData *d)
{
    if (d->notif_term) {
        terminal_destroy(d->notif_term);
        free(d->notif_term);
        d->notif_term = NULL;
    }
    d->notif_active = false;
    d->notif_h = 0;

    if (!d->notif_title && !d->notif_body)
        return;
    if (!d->font || d->cell_w <= 0 || d->cell_h <= 0)
        return;

    int win_w = (int)sapp_width();
    if (win_w <= 0)
        return;

    float scale = d->content_scale > 0 ? d->content_scale : 1.0f;
    int pad = (int)(10.0f * scale + 0.5f);
    int accent_w = (int)(4.0f * scale + 0.5f);
    int gap = (int)(8.0f * scale + 0.5f);
    int close_size = d->cell_h;
    int text_w = win_w - 2 * pad - accent_w - gap - close_size - pad;
    int notif_cols = text_w > 0 ? text_w / d->cell_w : 1;
    if (notif_cols < 1)
        notif_cols = 1;

    char *ansi = sokol_notif_build_ansi(d);
    if (!ansi)
        return;

    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.cols = notif_cols;
    cfg.rows = 32;
    cfg.cell_w_px = d->cell_w;
    cfg.cell_h_px = d->cell_h;
    d->notif_term = term_cfr_new(&cfg);
    if (!d->notif_term) {
        free(ansi);
        return;
    }

    terminal_process_input(d->notif_term, ansi, strlen(ansi));
    free(ansi);

    TerminalPos pos = terminal_get_cursor_pos(d->notif_term);
    int n_rows = pos.row + 1;
    if (n_rows < 1)
        n_rows = 1;

    d->notif_cols = notif_cols;
    d->notif_rows = n_rows;
    d->notif_w = win_w;
    d->notif_h = pad * 2 + n_rows * d->cell_h;
    d->notif_x = 0;
    d->notif_y = 0;
    d->notif_close_size = close_size;
    d->notif_close_x = (float)(d->notif_x + win_w - pad - close_size);
    d->notif_close_y = (float)(d->notif_y + pad + (n_rows * d->cell_h - close_size) / 2);
    d->notif_active = true;
}

static void sokol_init_cb(void)
{
    vlog("sokol_init_cb: backend=%p app=%p\n", (void *)g_sokol.backend, (void *)g_sokol.app);
    if (!g_sokol.backend || !g_sokol.app)
        return;
    if (!g_sokol.backend->init(g_sokol.backend, g_sokol.app,
                               sapp_query_desc().window_title, sapp_width(), sapp_height())) {
        fprintf(stderr, "ERROR: Failed to initialize Sokol backend\n");
        return;
    }

#if !defined(_WIN32) && !defined(__APPLE__)
    // Sokol doesn't set WM_CLASS, which prevents the desktop environment
    // from matching the window to the .desktop file for tray/taskbar icons.
    Display *xdisp = (Display *)sapp_x11_get_display();
    Window xwin = (Window)(intptr_t)sapp_x11_get_window();
    if (xdisp && xwin) {
        XClassHint *hint = XAllocClassHint();
        if (hint) {
            hint->res_name = (char *)"portty";
            hint->res_class = (char *)"portty";
            XSetClassHint(xdisp, xwin, hint);
            XFree(hint);
        }
    }
#endif

    sokol_finish_setup(g_sokol.backend, g_sokol.app);

    // Check for screenshot automation via env var
    SokolData *d = sokol_data(g_sokol.backend);
    if (d) {
        const char *ss_path = getenv("PORTTY_SCREENSHOT");
        if (ss_path && ss_path[0]) {
            snprintf(d->screenshot_path, sizeof(d->screenshot_path), "%s", ss_path);
            d->screenshot_frames = 5; // wait 5 frames for content to settle
            d->screenshot_saved = false;
            vlog("sokol_init_cb: screenshot mode, path=%s, frames=%d\n",
                 d->screenshot_path, d->screenshot_frames);
        }
    }
}

static void sokol_poll_timers(SokolData *d)
{
    if (!d->timers)
        return;

    double dur_sec = sapp_frame_duration();
    uint32_t elapsed_ms = (uint32_t)(dur_sec * 1000.0);
    if (elapsed_ms == 0)
        elapsed_ms = 1; // minimum 1ms to avoid timers never firing

    TimerEvent events[8];
    size_t n = timer_poll(d->timers, elapsed_ms, events, 8);
    for (size_t i = 0; i < n; i++) {
        if (events[i].code == SOKOL_EVENT_CURSOR_BLINK) {
            if (terminal_get_cursor_blink(d->term)) {
                d->cursor_blink_visible = !d->cursor_blink_visible;
                terminal_mark_dirty(d->term);
            }
        } else if (events[i].code == SOKOL_EVENT_AUTOSCROLL_TICK) {
            portty_app_handle_autoscroll_tick(d->app);
            terminal_mark_dirty(d->term);
        } else if (events[i].code == SOKOL_EVENT_LOTTIE_TICK) {
            uint64_t now_us = stm_now() / 1000;
            if (terminal_lottie_tick(d->term, now_us))
                terminal_mark_dirty(d->term);
            if (terminal_lottie_count(d->term) == 0) {
                timer_remove(d->timers, d->lottie_timer);
                d->lottie_timer = TIMER_INVALID;
            }
        }
    }
}

static bool sokol_should_render(SokolData *d, double elapsed_ms)
{
    if (d->render_mode == RENDER_MODE_FIXED_FPS && d->record_fps > 0.0) {
        d->record_accumulator_ms += elapsed_ms;
        double frame_ms = 1000.0 / d->record_fps;
        if (d->record_accumulator_ms >= frame_ms) {
            d->record_accumulator_ms -= frame_ms;
            d->capture_this_frame = true;
            return true;
        }
        return false;
    }

    if (d->debug_pending_screendump || d->debug_pending_verifybuf)
        return true;

    terminal_flush_damage(d->term);
    return terminal_needs_redraw(d->term);
}

static void sokol_frame_cb(void)
{
    SokolData *d = sokol_data(g_sokol.backend);
    if (!d)
        return;
    if (d->quit_requested)
        sapp_request_quit();

    // Poll timers (cursor blink, etc.)
    sokol_poll_timers(d);

    // Drain PTY data from reader thread before rendering
    sokol_drain_pty(d);

    // === Debug script: pre-render commands ===
    if (d->debug_script && !d->debug_script_done) {
        DebugExecCtx ctx = {
            .backend = g_sokol.backend,
            .term = d->term,
            .pty = d->pty,
            .scroll_offset = d->scroll.scroll_offset,
            .emit_fn = (void (*)(void *, const char *, size_t))portty_app_feed_terminal,
            .emit_user_data = g_sokol.app,
            .pending_screendump = &d->debug_pending_screendump,
            .screendump_path_buf = d->debug_screendump_path,
            .pending_verifybuf = &d->debug_pending_verifybuf,
            .verify_row = &d->debug_verify_row,
            .verify_col_start = &d->debug_verify_col_start,
            .verify_col_end = &d->debug_verify_col_end,
            .dumpverts_fn = sokol_debug_dumpverts,
            .mousemove_fn = sokol_debug_mousemove,
            .mousemove_user_data = g_sokol.app,
            .notify_fn = sokol_debug_notify,
            .notify_user_data = g_sokol.backend,
        };
        portty_debug_script_step(d->debug_script, &d->debug_cmd_index, &ctx);
        if (d->debug_cmd_index >= portty_debug_script_count(d->debug_script))
            d->debug_script_done = true;
    }

    double elapsed_ms = (double)sapp_frame_duration() * 1000.0;
    bool should_render = sokol_should_render(d, elapsed_ms);
    if (should_render) {
        // Compute cursor visibility: always shown when unfocused or blink off,
        // otherwise follows the blink toggle.
        bool cursor_vis = !d->has_focus ||
                          !terminal_get_cursor_blink(d->term) ||
                          d->cursor_blink_visible;

        d->self->draw_terminal(d->self, d->term, cursor_vis);

        // === Debug script: pre-commit deferred commands ===
        // screendump must run after sg_end_pass but BEFORE sg_commit (SwapBuffers),
        // otherwise glReadPixels reads the previous frame's back buffer.
        if (d->debug_pending_screendump) {
            d->debug_pending_screendump = false;
            sokol_debug_screendump(d, d->debug_screendump_path);
            if (d->screenshot_saved)
                d->quit_requested = true;
        }

        d->self->present(d->self);
        terminal_clear_redraw(d->term);

        // === Debug script: post-present deferred commands ===
        if (d->debug_pending_verifybuf) {
            d->debug_pending_verifybuf = false;
            sokol_debug_verifybuf(d, d->debug_verify_row,
                                  d->debug_verify_col_start,
                                  d->debug_verify_col_end);
        }
    }
}

static int sokol_map_key(sapp_keycode key)
{
    switch (key) {
    case SAPP_KEYCODE_ENTER:
        return TERM_KEY_ENTER;
    case SAPP_KEYCODE_TAB:
        return TERM_KEY_TAB;
    case SAPP_KEYCODE_BACKSPACE:
        return TERM_KEY_BACKSPACE;
    case SAPP_KEYCODE_ESCAPE:
        return TERM_KEY_ESCAPE;
    case SAPP_KEYCODE_UP:
        return TERM_KEY_UP;
    case SAPP_KEYCODE_DOWN:
        return TERM_KEY_DOWN;
    case SAPP_KEYCODE_LEFT:
        return TERM_KEY_LEFT;
    case SAPP_KEYCODE_RIGHT:
        return TERM_KEY_RIGHT;
    case SAPP_KEYCODE_HOME:
        return TERM_KEY_HOME;
    case SAPP_KEYCODE_END:
        return TERM_KEY_END;
    case SAPP_KEYCODE_INSERT:
        return TERM_KEY_INS;
    case SAPP_KEYCODE_DELETE:
        return TERM_KEY_DEL;
    case SAPP_KEYCODE_PAGE_UP:
        return TERM_KEY_PAGEUP;
    case SAPP_KEYCODE_PAGE_DOWN:
        return TERM_KEY_PAGEDOWN;
    case SAPP_KEYCODE_F1:
        return TERM_KEY_F1;
    case SAPP_KEYCODE_F2:
        return TERM_KEY_F2;
    case SAPP_KEYCODE_F3:
        return TERM_KEY_F3;
    case SAPP_KEYCODE_F4:
        return TERM_KEY_F4;
    case SAPP_KEYCODE_F5:
        return TERM_KEY_F5;
    case SAPP_KEYCODE_F6:
        return TERM_KEY_F6;
    case SAPP_KEYCODE_F7:
        return TERM_KEY_F7;
    case SAPP_KEYCODE_F8:
        return TERM_KEY_F8;
    case SAPP_KEYCODE_F9:
        return TERM_KEY_F9;
    case SAPP_KEYCODE_F10:
        return TERM_KEY_F10;
    case SAPP_KEYCODE_F11:
        return TERM_KEY_F11;
    case SAPP_KEYCODE_F12:
        return TERM_KEY_F12;
    default:
        return TERM_KEY_NONE;
    }
}

static int sokol_map_mod(uint32_t mods)
{
    // Sokol: SHIFT=0x1, CTRL=0x2, ALT=0x4
    // Term:  SHIFT=0x1, ALT=0x2,  CTRL=0x4
    int m = 0;
    if (mods & SAPP_MODIFIER_SHIFT)
        m |= TERM_MOD_SHIFT;
    if (mods & SAPP_MODIFIER_ALT)
        m |= TERM_MOD_ALT;
    if (mods & SAPP_MODIFIER_CTRL)
        m |= TERM_MOD_CTRL;
    return m;
}

static int sokol_map_button(sapp_mousebutton btn)
{
    // Sokol: LEFT=0, RIGHT=1, MIDDLE=2
    // portty_app expects X11/SDL convention: 1=left, 2=middle, 3=right
    switch (btn) {
    case SAPP_MOUSEBUTTON_LEFT:
        return 1;
    case SAPP_MOUSEBUTTON_MIDDLE:
        return 2;
    case SAPP_MOUSEBUTTON_RIGHT:
        return 3;
    default:
        return 0;
    }
}

static void sokol_event_cb(const sapp_event *ev)
{
    SokolData *d = sokol_data(g_sokol.backend);
    if (!d)
        return;
    switch (ev->type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
    {
        // Skip bare modifier keypresses (Ctrl/Shift/Alt alone) — Sokol
        // fires KEY_DOWN for these with no codepoint, which would cancel
        // an active selection via portty_app_handle_key's "any key" path.
        if (ev->key_code == SAPP_KEYCODE_LEFT_CONTROL ||
            ev->key_code == SAPP_KEYCODE_RIGHT_CONTROL ||
            ev->key_code == SAPP_KEYCODE_LEFT_SHIFT ||
            ev->key_code == SAPP_KEYCODE_RIGHT_SHIFT ||
            ev->key_code == SAPP_KEYCODE_LEFT_ALT ||
            ev->key_code == SAPP_KEYCODE_RIGHT_ALT)
            break;
        int term_key = sokol_map_key(ev->key_code);
        int mod = sokol_map_mod(ev->modifiers);
        // For Ctrl+letter, pass the codepoint so app shortcuts work
        uint32_t cp = 0;
        if (ev->key_code >= SAPP_KEYCODE_A && ev->key_code <= SAPP_KEYCODE_Z) {
            cp = 'a' + (ev->key_code - SAPP_KEYCODE_A);
            if (mod & TERM_MOD_SHIFT)
                cp = 'A' + (ev->key_code - SAPP_KEYCODE_A);
        }
        KeyboardResult kr = portty_app_handle_key(d->app, term_key, mod, cp);
        if (kr.len > 0 && !kr.handled && d->pty)
            pty_write(d->pty, kr.data, kr.len);
        if (kr.handled && term_key == TERM_KEY_NONE &&
            !(mod & (TERM_MOD_CTRL | TERM_MOD_ALT)))
            d->suppress_next_char = true;
        // Scroll to cursor when user types while scrolled back
        if (kr.force_redraw) {
            terminal_mark_dirty(d->term);
        } else if ((kr.handled || kr.len > 0) && rend_get_scroll_offset(&d->scroll) != 0) {
            rend_reset_scroll(&d->scroll);
            terminal_mark_dirty(d->term);
        }
        // Reset cursor blink on user input
        d->cursor_blink_visible = true;
        if (d->cursor_blink_timer != TIMER_INVALID)
            timer_reset(d->timers, d->cursor_blink_timer);
        terminal_mark_dirty(d->term);
        break;
    }
    case SAPP_EVENTTYPE_CHAR:
    {
        // Skip text input when Ctrl or Alt is held — the KEY_DOWN
        // handler already sent the control codepoint (e.g. Ctrl+R =
        // 0x12). Without this, Sokol's CHAR event would also send
        // the literal letter to the shell.
        if (ev->modifiers & (SAPP_MODIFIER_CTRL | SAPP_MODIFIER_ALT))
            break;
        // Drop CHAR events that were consumed by the KEY_DOWN handler
        // (e.g. 'q' closing the pager — the paired CHAR must not leak
        // to the PTY after the pager closes).
        if (d->suppress_next_char) {
            d->suppress_next_char = false;
            break;
        }
        char utf8[8] = { 0 };
        int n = 0;
        uint32_t cp = ev->char_code;
        if (cp < 0x80) {
            utf8[n++] = (char)cp;
        } else if (cp < 0x800) {
            utf8[n++] = (char)(0xC0 | (cp >> 6));
            utf8[n++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            utf8[n++] = (char)(0xE0 | (cp >> 12));
            utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            utf8[n++] = (char)(0xF0 | (cp >> 18));
            utf8[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            utf8[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8[n++] = (char)(0x80 | (cp & 0x3F));
        }
        if (n > 0) {
            KeyboardResult kr = portty_app_handle_text(d->app, utf8);
            if (kr.len > 0 && !kr.handled && d->pty)
                pty_write(d->pty, kr.data, kr.len);
            // Scroll to cursor when user types while scrolled back
            if ((kr.handled || kr.len > 0) && rend_get_scroll_offset(&d->scroll) != 0)
                rend_reset_scroll(&d->scroll);
            // Reset cursor blink on user input
            d->cursor_blink_visible = true;
            if (d->cursor_blink_timer != TIMER_INVALID)
                timer_reset(d->timers, d->cursor_blink_timer);
            terminal_mark_dirty(d->term);
        }
    } break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
    {
        int btn = sokol_map_button(ev->mouse_button);
        if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
            d->left_button_down = true;
            // Compute click count for double/triple-click detection
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now = (uint64_t)ts.tv_sec * 1000 +
                           (uint64_t)ts.tv_nsec / 1000000;
            int dx = (int)ev->mouse_x - d->last_click_x;
            int dy = (int)ev->mouse_y - d->last_click_y;
            const int CLICK_THRESHOLD_MS = 400;
            const int CLICK_THRESHOLD_PX = 5;
            if (now - d->last_click_time < CLICK_THRESHOLD_MS &&
                dx * dx + dy * dy < CLICK_THRESHOLD_PX * CLICK_THRESHOLD_PX) {
                d->click_count++;
                if (d->click_count > 3)
                    d->click_count = 1;
            } else {
                d->click_count = 1;
            }
            d->last_click_time = now;
            d->last_click_x = (int)ev->mouse_x;
            d->last_click_y = (int)ev->mouse_y;
        } else {
            d->click_count = 1;
        }
        d->last_mouse_x = (int)ev->mouse_x;
        d->last_mouse_y = (int)ev->mouse_y;
        if (portty_app_handle_mouse(d->app, (int)ev->mouse_x, (int)ev->mouse_y,
                                    btn, true, d->click_count,
                                    sokol_map_mod(ev->modifiers)))
            terminal_mark_dirty(d->term);
        break;
    }
    case SAPP_EVENTTYPE_MOUSE_UP:
        if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT)
            d->left_button_down = false;
        d->last_mouse_x = (int)ev->mouse_x;
        d->last_mouse_y = (int)ev->mouse_y;
        if (portty_app_handle_mouse(d->app, (int)ev->mouse_x, (int)ev->mouse_y,
                                    sokol_map_button(ev->mouse_button), false,
                                    d->click_count,
                                    sokol_map_mod(ev->modifiers)))
            terminal_mark_dirty(d->term);
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        d->last_mouse_x = (int)ev->mouse_x;
        d->last_mouse_y = (int)ev->mouse_y;
        if (portty_app_handle_mouse(d->app, (int)ev->mouse_x, (int)ev->mouse_y,
                                    0, d->left_button_down, 0,
                                    sokol_map_mod(ev->modifiers)))
            terminal_mark_dirty(d->term);
        break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL:
    {
        float dy = ev->scroll_y;
        if (dy != 0.0f) {
            d->wheel_accum_y += dy;
            int whole_ticks = (int)d->wheel_accum_y;
            if (whole_ticks != 0) {
                d->wheel_accum_y -= (float)whole_ticks;
                bool consumed = false;
                int button = (whole_ticks > 0) ? 4 : 5;
                int clicks = abs(whole_ticks);
                int tmod = sokol_map_mod(ev->modifiers);
                int mx = (int)ev->mouse_x;
                int my = (int)ev->mouse_y;
                for (int i = 0; i < clicks && !consumed; i++) {
                    consumed = portty_app_handle_mouse(
                        d->app, mx, my, button, true, 0, tmod);
                }
                if (!consumed)
                    portty_app_handle_scroll(d->app, whole_ticks);
            }
        }
        terminal_mark_dirty(d->term);
        break;
    }
    case SAPP_EVENTTYPE_MOUSE_ENTER:
        portty_app_handle_mouse_enter(d->app);
        terminal_mark_dirty(d->term);
        break;
    case SAPP_EVENTTYPE_MOUSE_LEAVE:
        portty_app_handle_mouse_leave(d->app, (int)ev->mouse_x,
                                      (int)ev->mouse_y);
        terminal_mark_dirty(d->term);
        break;
    case SAPP_EVENTTYPE_RESIZED:
        portty_app_handle_resize(d->app, ev->framebuffer_width,
                                 ev->framebuffer_height);
        terminal_mark_dirty(d->term);
        break;
    case SAPP_EVENTTYPE_FOCUSED:
        d->has_focus = true;
        terminal_mark_dirty(d->term);
        break;
    case SAPP_EVENTTYPE_UNFOCUSED:
        d->has_focus = false;
        terminal_mark_dirty(d->term);
        break;
    case SAPP_EVENTTYPE_QUIT_REQUESTED:
        d->quit_requested = true;
        break;
    default:
        break;
    }
}

static void sokol_cleanup_cb(void)
{
    if (g_sokol.backend)
        g_sokol.backend->destroy(g_sokol.backend);
}

sapp_desc backend_sokol_desc(PorttyApp *app, PorttyBackend *backend,
                             const char *title, int width, int height)
{
    g_sokol.app = app;
    g_sokol.backend = backend;

    return (sapp_desc){
        .init_cb = sokol_init_cb,
        .frame_cb = sokol_frame_cb,
        .event_cb = sokol_event_cb,
        .cleanup_cb = sokol_cleanup_cb,
        .width = width,
        .height = height,
        .window_title = title,
        .high_dpi = true,
        .srgb = true,
        .enable_clipboard = true,
        .logger.func = slog_func,
    };
}
