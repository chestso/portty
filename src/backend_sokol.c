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
#include "qoi_writer.h"
#include "display_info.h"
#include "portty_app.h"
#include "portty_conf.h"
#include "portty_script.h"
#include "portty_frame_rec.h"
#include "portty_pty.h"
#include "portty_panel.h"
#include "os_compat.h"
#include "rend_sokol_atlas.h"
#include "rend_sokol.h"
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
    SOKOL_EVENT_RECORD_TICK,
};

// Rendering scheduler mode.  DAMAGE mode only emits GPU work when the
// terminal reports damage (or a debug/recording action forces a frame).
// FIXED_FPS mode renders at a fixed frame rate for recording automation.
typedef enum
{
    RENDER_MODE_DAMAGE = 0,
    RENDER_MODE_FIXED_FPS,
} RenderMode;

// Cache limits (shared with rend_sokol.h)
#define SOKOL_LOTTIE_TICK_MS 16

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
    // Font hinting (backend-specific, not in rend)
    char hint_name[8];
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
    PorttyScript *script;
    int cmd_index;
    bool script_done;
    bool pending_screendump;
    char screendump_path[512];
    bool pending_verifybuf;
    int verify_row, verify_col_start, verify_col_end;
    unsigned int glyph_vbuf_gl_id; // GL buffer ID for verifybuf
    // Frame recorder
    FrameRecorder *frame_recorder;
    TimerId record_timer;
    bool pending_record_frame;
    // Lottie animation timer (animation data lives in rend)
    TimerId lottie_timer;
    // Window title state
    char *last_title;

    // Renderer module (rend_sokol)
    RendererSokolData rend;
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
    d->rend.content_scale = 1.0f;
    d->rend.cell_w = 10;
    d->rend.cell_h = 20;
#ifndef _WIN32
    d->wakeup_pipe[0] = d->wakeup_pipe[1] = -1;
#else
    d->wakeup_event = NULL;
#endif
    pthread_mutex_init(&d->pty_queue_mtx, NULL);
    memset(&d->rend.scroll, 0, sizeof(d->rend.scroll));
    rend_fallback_init(&d->rend.fallback);
    d->timers = timer_manager_create();
    d->cursor_blink_timer = TIMER_INVALID;
    d->cursor_blink_visible = true;
    d->has_focus = true;
    d->rend.linear_ok = true;
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
    d->rend.lottie_pip_created = false;
    d->rend.lottie_cache_count = 0;
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
    portty_script_free(d->script);
    d->script = NULL;

    // Cleanup frame recorder
    if (d->frame_recorder) {
        if (d->frame_recorder->recording)
            frame_recorder_stop(d->frame_recorder);
        if (d->record_timer != TIMER_INVALID) {
            timer_remove(d->timers, d->record_timer);
            d->record_timer = TIMER_INVALID;
        }
        frame_recorder_free(d->frame_recorder);
        d->frame_recorder = NULL;
    }

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
    for (int i = 0; i < d->rend.lottie_cache_count; i++) {
        if (d->rend.lottie_cache[i].image.id != SG_INVALID_ID)
            sg_destroy_image(d->rend.lottie_cache[i].image);
        if (d->rend.lottie_cache[i].view.id != SG_INVALID_ID)
            sg_destroy_view(d->rend.lottie_cache[i].view);
    }
    d->rend.lottie_cache_count = 0;
    if (d->rend.lottie_pip_created) {
        sg_destroy_pipeline(d->rend.lottie_pip);
        sg_destroy_buffer(d->rend.lottie_vbuf);
        sg_destroy_sampler(d->rend.lottie_sampler);
    }

    // Destroy sixel cache and vbuf
    for (int i = 0; i < d->rend.sixel_cache_count; i++) {
        if (d->rend.sixel_cache[i].image.id != SG_INVALID_ID)
            sg_destroy_image(d->rend.sixel_cache[i].image);
        if (d->rend.sixel_cache[i].view.id != SG_INVALID_ID)
            sg_destroy_view(d->rend.sixel_cache[i].view);
    }
    d->rend.sixel_cache_count = 0;
    if (d->rend.sixel_vbuf_created)
        sg_destroy_buffer(d->rend.sixel_vbuf);

    if (d->tex_created)
        sg_destroy_image(d->tex);
    if (d->pip_created) {
        sg_destroy_pipeline(d->pip);
        sg_destroy_buffer(d->vbuf);
        sg_destroy_sampler(d->smp);
        sg_destroy_view(d->tex_view);
    }
    free(d->pixels);
    if (d->rend.font) {
        rend_fallback_destroy(&d->rend.fallback, d->rend.font);
        font_destroy(d->rend.font);
        d->rend.font = NULL;
    }
    if (d->rend.resolve) {
        font_resolve_destroy(d->rend.resolve);
        d->rend.resolve = NULL;
    }
    free(d->rend.font_path);
    free(d->last_title);
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
        if (pushed > 0 && rend_get_scroll_offset(&d->rend.scroll) > 0)
            rend_scroll(&d->rend.scroll, d->term, pushed);
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

// ── Panel ANSI builder ────────────────────────────────────────────────────

static char *sokol_panel_build_ansi(SokolData *d, const char *title,
                                    const char *body, PorttyNotifyLevel level)
{
    (void)d;
    (void)level;
    SokolStrBuf sb;
    if (!sokol_strbuf_init(&sb))
        return NULL;

    const char *panel_bg = "\x1b[48;2;38;38;44m";

    // Clear screen first in case terminal is being reused
    if (!sokol_strbuf_appendf(&sb, "\x1b[2J\x1b[H")) {
        sokol_strbuf_free(&sb);
        return NULL;
    }

    if (title) {
        if (!sokol_strbuf_appendf(&sb,
                                  "%s\x1b[1m\x1b[38;2;236;236;241m%s\x1b[22m\x1b[39m",
                                  panel_bg, title)) {
            sokol_strbuf_free(&sb);
            return NULL;
        }
    }
    if (body) {
        if (title) {
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
                                  body)) {
            sokol_strbuf_free(&sb);
            return NULL;
        }
    }
    return sokol_strbuf_finish(&sb);
}

// ── Panel functions ─────────────────────────────────────────────────────

static void sokol_panel_show(PorttyBackend *self, int id,
                             int col, int row, int cols, int rows,
                             const char *title, const char *body,
                             PorttyNotifyLevel level, unsigned int flags)
{
    SokolData *d = sokol_data(self);
    if (!d || !d->rend.font || d->rend.cell_w <= 0 || d->rend.cell_h <= 0)
        return;

    panel_mgr_set_cell_size(&d->rend.panels, d->rend.cell_w, d->rend.cell_h);
    PanelState *p = panel_mgr_show(&d->rend.panels, id, col, row, cols, rows,
                                   title, body, level, flags);
    if (!p)
        return;

    // Create or reuse terminal for this panel
    int idx = p - d->rend.panels.panels;
    if (idx < 0 || idx >= PORTTY_PANEL_MAX)
        return;

    // Terminal size = panel size minus decoration cells
    int term_cols = panel_term_cols(cols, panel_show_accent(flags));
    int term_rows = panel_term_rows(rows);
    if (term_cols <= 0 || term_rows <= 0)
        return;

    // Check if terminal needs recreation (wrong size or doesn't exist)
    int existing_cols = 0, existing_rows = 0;
    if (d->rend.panel_terms[idx]) {
        terminal_get_dimensions(d->rend.panel_terms[idx], &existing_rows, &existing_cols);
    }

    if (!d->rend.panel_terms[idx] || existing_cols != term_cols || existing_rows != term_rows) {
        // Destroy old terminal if it exists
        if (d->rend.panel_terms[idx]) {
            terminal_destroy(d->rend.panel_terms[idx]);
            free(d->rend.panel_terms[idx]);
            d->rend.panel_terms[idx] = NULL;
        }
        // Create new terminal with correct dimensions
        CfrConfig cfg = CFR_CONFIG_DEFAULTS;
        cfg.cols = term_cols;
        cfg.rows = term_rows;
        cfg.cell_w_px = d->rend.cell_w;
        cfg.cell_h_px = d->rend.cell_h;
        d->rend.panel_terms[idx] = term_cfr_new(&cfg);
    }

    if (d->rend.panel_terms[idx]) {
        // Build ANSI content
        char *ansi = sokol_panel_build_ansi(d, title, body, level);
        if (ansi) {
            terminal_process_input(d->rend.panel_terms[idx], ansi, strlen(ansi));
            terminal_flush_damage(d->rend.panel_terms[idx]);
            free(ansi);
        }
    }

    if (d->term)
        terminal_mark_dirty(d->term);
}

static void sokol_panel_hide(PorttyBackend *self, int id)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;

    PanelState *p = panel_mgr_find(&d->rend.panels, id);
    if (!p)
        return;

    int idx = p - d->rend.panels.panels;
    if (idx >= 0 && idx < PORTTY_PANEL_MAX && d->rend.panel_terms[idx]) {
        terminal_destroy(d->rend.panel_terms[idx]);
        free(d->rend.panel_terms[idx]);
        d->rend.panel_terms[idx] = NULL;
    }

    panel_mgr_hide(&d->rend.panels, id);
    if (d->term)
        terminal_mark_dirty(d->term);
}

static int sokol_panel_hit_test(PorttyBackend *self, int px, int py, bool *close_btn)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return -1;
    return panel_mgr_hit_test(&d->rend.panels, px, py, close_btn);
}

static void sokol_panel_set_hover(PorttyBackend *self, int id, bool hovered)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return;
    panel_mgr_set_hover(&d->rend.panels, id, hovered);
}

// ── Rendering ────────────────────────────────────────────────────────────

// Color constants moved to rend_sokol.c - use rend_sokol_emit_cursor_quad/sel_quad

// Vertex format defined in rend_sokol.h
// Static buffers moved to rend_sokol.c - use accessor functions

#define SOKOL_MAX_SIXEL_VERTICES 4096

static void sokol_draw_terminal(PorttyBackend *self, TerminalBackend *term,
                                bool cursor_visible)
{
    SokolData *d = sokol_data(self);
    if (!d || !term)
        return;

    // If overlay is active, render the overlay terminal instead
    if (d->rend.scroll.overlay)
        term = d->rend.scroll.overlay;

    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);
    if (rows <= 0 || cols <= 0)
        return;

    int cell_w = d->rend.cell_w;
    int cell_h = d->rend.cell_h;
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
    if (!d->rend.atlas.texture_created) {
        if (!rend_sokol_atlas_init(&d->rend.atlas, d->rend.linear_ok))
            return;
    }

    // Ensure glyph pipeline is created
    rend_sokol_ensure_glyph_pipeline(&d->rend);
    rend_sokol_ensure_lottie_pipeline(&d->rend);

    rend_sokol_atlas_begin_frame(&d->rend.atlas);

    // Build vertex data (using file-scope arrays for debug access)
    static GlyphVertex sel_verts[SOKOL_MAX_VERTICES];
    int vert_count = 0;                          // bg quads in frame_verts
    int glyph_vert_count = 0;                    // glyph quads in glyph_verts
    *rend_sokol_get_cursor_vert_count_ptr() = 0; // cursor quad in cursor_verts
    int sel_vert_count = 0;
    int scroll_offset = d->rend.scroll.scroll_offset;

    rend_sokol_reset_frame_buffers();

    rend_sokol_render_terminal_cells(&d->rend, term, 0, 0, cursor_visible, scroll_offset,
                                     &vert_count, &glyph_vert_count, &sel_vert_count,
                                     sel_verts);

    // Track where panel glyphs start for multi-pass rendering
    int panel_glyph_start = glyph_vert_count;

    // General-purpose panels
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        PanelState *p = &d->rend.panels.panels[i];
        if (p->active && d->rend.panel_terms[i]) {
            int text_x = panel_term_px(p->px, d->rend.cell_w, panel_show_accent(p->flags));
            int text_y = panel_term_py(p->py, d->rend.cell_h);
            rend_sokol_render_terminal_cells(&d->rend, d->rend.panel_terms[i],
                                             text_x, text_y,
                                             false, 0,
                                             &vert_count, &glyph_vert_count,
                                             &sel_vert_count, sel_verts);
        }
    }

    // Decoration pass: coalesce underlines and strikethroughs by style and
    // color, and emit them into s_deco_verts for alpha-blended rendering.
    rend_sokol_deco_reset();
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
                    cr[0] = TERM_UNDERLINE_R;
                    cr[1] = TERM_UNDERLINE_G;
                    cr[2] = TERM_UNDERLINE_B;
                    cr[3] = TERM_UNDERLINE_A;
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
                    rend_sokol_draw_underline(&d->rend, row, run_start,
                                              vis_run_end, run_style, run_color);
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
                rend_sokol_draw_underline(&d->rend, row, run_start,
                                          vis_run_end, run_style, run_color);
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
                rend_sokol_cell_color(it.cell.fg, true, it.cell.attrs.reverse, cr);
                bool same_run = in_run && cs &&
                                cr[0] == run_color[0] &&
                                cr[1] == run_color[1] &&
                                cr[2] == run_color[2];
                if (in_run && !same_run) {
                    rend_sokol_draw_strikethrough(&d->rend, row, run_start,
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
                rend_sokol_draw_strikethrough(&d->rend, row, run_start,
                                              vis_run_end, run_color);
            }
        }
    }

    // General-purpose panels: emit background quads
    for (int i = 0; i < PORTTY_PANEL_MAX; i++) {
        PanelState *p = &d->rend.panels.panels[i];
        if (p->active) {
            // Panel background (opaque)
            uint8_t bg[4] = { TERM_PANEL_BG_R, TERM_PANEL_BG_G, TERM_PANEL_BG_B, TERM_PANEL_BG_A };
            rend_sokol_deco_emit_quad(
                (float)p->px, (float)p->py,
                (float)(p->px + p->pw), (float)(p->py + p->ph),
                bg);

            // Accent stripe (if enabled, 1 cell wide, full height, offset by 1 cell padding)
            if (panel_show_accent(p->flags)) {
                uint8_t ac[4] = { 0, 0, 0, 255 };
                switch (p->level) {
                case PORTTY_NOTIFY_ERROR:
                    ac[0] = TERM_ACCENT_ERROR_R;
                    ac[1] = TERM_ACCENT_ERROR_G;
                    ac[2] = TERM_ACCENT_ERROR_B;
                    break;
                case PORTTY_NOTIFY_WARNING:
                    ac[0] = TERM_ACCENT_WARNING_R;
                    ac[1] = TERM_ACCENT_WARNING_G;
                    ac[2] = TERM_ACCENT_WARNING_B;
                    break;
                default:
                    ac[0] = TERM_ACCENT_DEFAULT_R;
                    ac[1] = TERM_ACCENT_DEFAULT_G;
                    ac[2] = TERM_ACCENT_DEFAULT_B;
                    break;
                }
                int accent_px = panel_accent_px(p->px, d->rend.cell_w);
                int accent_w = panel_accent_w(d->rend.cell_w);
                rend_sokol_deco_emit_quad(
                    (float)accent_px, (float)p->py,
                    (float)(accent_px + accent_w), (float)(p->py + p->ph),
                    ac);
            }

            // Close button (if enabled, top-right corner of panel)
            if (panel_show_close(p->flags) && p->close_size > 0 && glyph_vert_count + 6 <= SOKOL_MAX_VERTICES) {
                // Lookup or insert close button bitmap in atlas
                uint32_t color_key = 0; // White, no color key needed
                RendSokolAtlasEntry *entry = rend_sokol_atlas_lookup(
                    &d->rend.atlas, BOXDRAW_FONT_DATA, CLOSE_BUTTON_GLYPH_ID, color_key);
                if (!entry) {
                    // Create the close button bitmap
                    int size = p->close_size;
                    GlyphBitmap *bmp = calloc(1, sizeof(*bmp));
                    if (bmp) {
                        bmp->pixels = calloc((size_t)size * size, 4);
                        bmp->width = size;
                        bmp->height = size;
                        bmp->glyph_id = CLOSE_BUTTON_GLYPH_ID;
                        if (bmp->pixels) {
                            rend_make_close_x_bitmap(bmp->pixels, size);
                            entry = rend_sokol_atlas_insert(
                                &d->rend.atlas, BOXDRAW_FONT_DATA,
                                CLOSE_BUTTON_GLYPH_ID, color_key, bmp, false);
                        }
                        free(bmp->pixels);
                        free(bmp);
                    }
                }
                if (entry && entry->region.w > 0) {
                    float atlas_size = (float)REND_ATLAS_TEXTURE_SIZE;
                    float u0 = (float)entry->region.x / atlas_size;
                    float v0 = (float)entry->region.y / atlas_size;
                    float u1 = (float)(entry->region.x + entry->region.w) / atlas_size;
                    float v1 = (float)(entry->region.y + entry->region.h) / atlas_size;
                    uint8_t fg[4] = {
                        p->close_hover ? TERM_CLOSE_FG_HOVER_R : TERM_CLOSE_FG_R,
                        p->close_hover ? TERM_CLOSE_FG_HOVER_G : TERM_CLOSE_FG_G,
                        p->close_hover ? TERM_CLOSE_FG_HOVER_B : TERM_CLOSE_FG_B,
                        255
                    };
                    uint8_t bg[4] = { TERM_PANEL_BG_R, TERM_PANEL_BG_G, TERM_PANEL_BG_B, 0 };
                    GlyphVertex *q = &rend_sokol_get_glyph_verts()[glyph_vert_count];
                    rend_sokol_emit_glyph_quad(q,
                                               (float)p->close_px, (float)p->close_py,
                                               (float)(p->close_px + p->close_size),
                                               (float)(p->close_py + p->close_size),
                                               u0, v0, u1, v1, fg, bg);
                    glyph_vert_count += 6;
                }
            }
        }
    }

    // Append cursor quads after bg quads (cursor is drawn under glyphs, opaque).
    // Three-pass rendering: bg quads first (0..bg_count), then cursor quads,
    // then glyph quads on top.
    int bg_vert_count = vert_count;
    const int cursor_vert_start = bg_vert_count;
    int cursor_vert_count = *rend_sokol_get_cursor_vert_count_ptr();
    if (cursor_vert_count > 0 && vert_count + cursor_vert_count <= SOKOL_MAX_VERTICES) {
        memcpy(&rend_sokol_get_frame_verts()[vert_count], rend_sokol_get_cursor_verts(),
               (size_t)cursor_vert_count * sizeof(GlyphVertex));
        vert_count += cursor_vert_count;
    }
    // Append glyph quads after cursor quads.
    int glyph_vert_start = vert_count;
    if (glyph_vert_count > 0 && vert_count + glyph_vert_count <= SOKOL_MAX_VERTICES) {
        memcpy(&rend_sokol_get_frame_verts()[vert_count], rend_sokol_get_glyph_verts(),
               (size_t)glyph_vert_count * sizeof(GlyphVertex));
        vert_count += glyph_vert_count;
    }

    // Append selection overlay quads after glyph quads so all fit in one
    // buffer update (Sokol allows only one sg_update_buffer per frame).
    int sel_vert_start = vert_count;
    if (sel_vert_count > 0 && vert_count + sel_vert_count <= SOKOL_MAX_VERTICES) {
        memcpy(&rend_sokol_get_frame_verts()[vert_count], sel_verts, (size_t)sel_vert_count * sizeof(GlyphVertex));
        vert_count += sel_vert_count;
    }

    // Append decoration quads after selection quads. They are drawn with
    // the selection pipeline (alpha-blended) between glyphs and selections.
    int deco_vert_start = vert_count;
    int deco_count = rend_sokol_deco_get_count();
    if (deco_count > 0 &&
        vert_count + deco_count <= SOKOL_MAX_VERTICES) {
        memcpy(&rend_sokol_get_frame_verts()[vert_count], rend_sokol_deco_get_verts(),
               (size_t)deco_count * sizeof(GlyphVertex));
        vert_count += deco_count;
    }

    // Flush atlas dirty regions to GPU
    rend_sokol_atlas_flush(&d->rend.atlas);

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
                           .clear_value = { rend_srgb_to_linear(TERM_BG_R) / 255.0f,
                                            rend_srgb_to_linear(TERM_BG_G) / 255.0f,
                                            rend_srgb_to_linear(TERM_BG_B) / 255.0f, 1.0f } } },
        .swapchain = sglue_swapchain(),
    });

    // Lottie rendering must happen even when vert_count == 0 (no terminal updates)
    // This ensures animations continue when cursor blinks but text hasn't changed
    int lottie_bg_count = 0;
    bool has_lottie = d->rend.lottie_pip_created && terminal_lottie_count(term) > 0;
    if (has_lottie) {
        int anim_count = 0;
        const CfrLottie *anims = terminal_get_lotties(term, &anim_count);
        rend_sokol_lottie_cache_reconcile(&d->rend, anims, anim_count);

        // Upload the full lottie vertex buffer before any draws
        sg_update_buffer(d->rend.lottie_vbuf, &(sg_range){
                                                  .ptr = rend_sokol_get_lottie_verts(),
                                                  .size = SOKOL_MAX_LOTTIE_VERTICES * sizeof(GlyphVertex),
                                              });

        // Draw background lottie (before bg quads)
        lottie_bg_count = rend_sokol_render_lottie_layer(&d->rend, term, sapp_width(), sapp_height(), 1, 0);
    }

    if (vert_count > 0 && d->rend.glyph_pip_created) {
        *rend_sokol_get_frame_vert_count_ptr() = vert_count;
        sg_update_buffer(d->rend.glyph_vbuf, &(sg_range){ .ptr = rend_sokol_get_frame_verts(), .size = (size_t)vert_count * sizeof(GlyphVertex) });

        float uniforms[4] = { (float)win_w, (float)win_h,
                              (float)cell_w, (float)cell_h };
        sg_apply_pipeline(d->rend.glyph_pip);
        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = d->rend.glyph_vbuf,
            .views[0] = d->rend.atlas.texture_view,
            .samplers[0] = d->rend.atlas.sampler,
        });
        sg_apply_uniforms(0, &SG_RANGE(uniforms));

        // Pass 1: draw all background quads (opaque, replace)
        if (bg_vert_count > 0) {
            sg_draw(0, bg_vert_count, 1);
        }
        // Pass 1.5: draw cursor quads (under glyphs, opaque)
        if (cursor_vert_count > 0) {
            sg_draw(cursor_vert_start, cursor_vert_count, 1);
        }
        // Pass 2: draw terminal grid glyph quads (opaque, replace)
        int terminal_glyph_count = panel_glyph_start;
        if (terminal_glyph_count > 0) {
            sg_draw(glyph_vert_start, terminal_glyph_count, 1);
        }
        // Pass 3: draw decoration quads (alpha-blended, on top of terminal
        // glyphs). This includes underlines, strikethroughs, and the
        // notification accent stripe.
        if (deco_count > 0 && d->rend.sel_pip_created) {
            sg_apply_pipeline(d->rend.sel_pip);
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = d->rend.glyph_vbuf,
                .views[0] = d->rend.atlas.texture_view,
                .samplers[0] = d->rend.atlas.sampler,
            });
            sg_apply_uniforms(0, &SG_RANGE(uniforms));
            sg_draw(deco_vert_start, deco_count, 1);
        }
        // Pass 4: draw selection overlay quads (alpha-blended, on top of
        // decorations so selected text remains readable).
        int sel_count = vert_count - sel_vert_start;
        if (sel_count > 0 && d->rend.sel_pip_created) {
            sg_apply_pipeline(d->rend.sel_pip);
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = d->rend.glyph_vbuf,
                .views[0] = d->rend.atlas.texture_view,
                .samplers[0] = d->rend.atlas.sampler,
            });
            sg_apply_uniforms(0, &SG_RANGE(uniforms));
            sg_draw(sel_vert_start, sel_count, 1);
        }
        // Pass 5: draw panel glyph quads (opaque, on top of panel backgrounds)
        int panel_glyph_count = glyph_vert_count - panel_glyph_start;
        if (panel_glyph_count > 0) {
            sg_apply_pipeline(d->rend.glyph_pip);
            sg_apply_bindings(&(sg_bindings){
                .vertex_buffers[0] = d->rend.glyph_vbuf,
                .views[0] = d->rend.atlas.texture_view,
                .samplers[0] = d->rend.atlas.sampler,
            });
            sg_apply_uniforms(0, &SG_RANGE(uniforms));
            sg_draw(glyph_vert_start + panel_glyph_start, panel_glyph_count, 1);
        }
        // Pass 6: draw foreground lottie (on top of everything)
        if (lottie_bg_count >= 0 && has_lottie)
            (void)rend_sokol_render_lottie_layer(&d->rend, term, sapp_width(), sapp_height(), 0, lottie_bg_count);
        // Pass 8: draw sixel images (on top of text and lottie)
        if (d->rend.lottie_pip_created) {
            rend_sokol_ensure_sixel_vbuf(&d->rend);
            rend_sokol_render_sixel_images(&d->rend, term, sapp_width(), sapp_height());
        }
    } else if (has_lottie) {
        // Even with no terminal vertices, render foreground lottie layer
        if (lottie_bg_count >= 0)
            (void)rend_sokol_render_lottie_layer(&d->rend, term, sapp_width(), sapp_height(), 0, lottie_bg_count);
    }

    // Screenshot automation: defer to after sg_commit (glReadPixels inside
    // the pass triggers VALIDATION_FAILED panics).
    if (d->screenshot_frames > 0 && !d->screenshot_saved) {
        d->screenshot_frames--;
        if (d->screenshot_frames == 0) {
            d->screenshot_saved = true;
            d->pending_screendump = true;
            snprintf(d->screendump_path,
                     sizeof(d->screendump_path), "%s",
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
    // Update panel layout if cell size changed
    if (d->rend.cell_w > 0 && d->rend.cell_h > 0)
        panel_mgr_set_cell_size(&d->rend.panels, d->rend.cell_w, d->rend.cell_h);
}

static bool sokol_get_cell_size(PorttyBackend *self, int *cw, int *ch)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return false;
    if (cw)
        *cw = d->rend.cell_w;
    if (ch)
        *ch = d->rend.cell_h;
    return true;
}

// ── Font loading ──────────────────────────────────────────────────────────

static int sokol_load_fonts(PorttyBackend *self, float size,
                            const char *name, int ft_hint_target)
{
    SokolData *d = sokol_data(self);
    if (!d)
        return -1;

    d->rend.font = &font_backend_ft;
    if (!font_init(d->rend.font)) {
        fprintf(stderr, "ERROR: Failed to initialize font backend\n");
        return -1;
    }

#ifdef _WIN32
    extern FontResolveBackend font_resolve_backend_w32;
    d->rend.resolve = font_resolve_init(&font_resolve_backend_w32);
#elif defined(__APPLE__)
    extern FontResolveBackend font_resolve_backend_ct;
    d->rend.resolve = font_resolve_init(&font_resolve_backend_ct);
#else
    extern FontResolveBackend font_resolve_backend_fc;
    d->rend.resolve = font_resolve_init(&font_resolve_backend_fc);
#endif
    if (!d->rend.resolve) {
        fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
        font_destroy(d->rend.font);
        d->rend.font = NULL;
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
    if (rend_load_fonts(&r, d->rend.font, d->rend.resolve, size, name,
                        ft_hint_target, d->rend.content_scale, hint_name) != 0) {
        font_resolve_destroy(d->rend.resolve);
        d->rend.resolve = NULL;
        font_destroy(d->rend.font);
        d->rend.font = NULL;
        return -1;
    }

    d->rend.font_ascent = r.font_ascent;
    d->rend.font_descent = r.font_descent;
    d->rend.font_cap_height = r.font_cap_height;
    d->rend.cell_w = r.cell_width;
    d->rend.cell_h = r.cell_height;
    d->rend.font_size = r.font_size;
    d->rend.font_options = r.font_options;
    panel_mgr_set_cell_size(&d->rend.panels, d->rend.cell_w, d->rend.cell_h);
    free(d->rend.font_path);
    d->rend.font_path = r.font_path;
    r.font_path = NULL;

    return 0;
}

static void sokol_scroll(PorttyBackend *self, TerminalBackend *term,
                         int delta)
{
    SokolData *d = sokol_data(self);
    if (d)
        rend_scroll(&d->rend.scroll, term, delta);
}

static void sokol_reset_scroll(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (d)
        rend_reset_scroll(&d->rend.scroll);
}

static int sokol_get_scroll_offset(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    return d ? rend_get_scroll_offset(&d->rend.scroll) : 0;
}

static void sokol_set_content_scale(PorttyBackend *self, float scale)
{
    SokolData *d = sokol_data(self);
    if (d)
        d->rend.content_scale = scale;
}

// ── Pager overlay ────────────────────────────────────────────────────────

static void sokol_set_overlay(PorttyBackend *self, TerminalBackend *overlay)
{
    SokolData *d = sokol_data(self);
    if (d)
        rend_set_overlay(&d->rend.scroll, overlay);
}

static void sokol_clear_overlay(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    if (d)
        rend_clear_overlay(&d->rend.scroll);
}

static bool sokol_has_overlay(PorttyBackend *self)
{
    SokolData *d = sokol_data(self);
    return d ? rend_has_overlay(&d->rend.scroll) : false;
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

    int cell_w = d->rend.cell_w;
    int cell_h = d->rend.cell_h;
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

    rend_sokol_ensure_glyph_pipeline(&d->rend);
    if (!d->rend.atlas.texture_created) {
        if (!rend_sokol_atlas_init(&d->rend.atlas, d->rend.linear_ok))
            return -1;
    }
    rend_sokol_atlas_begin_frame(&d->rend.atlas);

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
            rend_sokol_cell_color(cell.fg, true, rev, fg);
            rend_sokol_cell_color(cell.bg, false, rev, bg);

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

            if (cell.cp != 0 && cell.cp != 0x20 && d->rend.font && !cell.attrs.invis) {
                FontStyle style = FONT_STYLE_NORMAL;
                if (cell.attrs.bold && cell.attrs.italic)
                    style = FONT_STYLE_BOLD_ITALIC;
                else if (cell.attrs.bold)
                    style = FONT_STYLE_BOLD;
                else if (cell.attrs.italic)
                    style = FONT_STYLE_ITALIC;
                if (!font_has_style(d->rend.font, style))
                    style = FONT_STYLE_NORMAL;

                uint32_t glyph_id = font_get_glyph_index(d->rend.font, style, cell.cp);
                uint32_t color_key = 0;
                RendSokolAtlasEntry *entry = rend_sokol_atlas_lookup(
                    &d->rend.atlas, d->rend.font->font_data[style], (int)glyph_id, color_key);

                if (!entry) {
                    GlyphBitmap *bmp = font_render_glyphs(
                        d->rend.font, style, &cell.cp, 1, 255, 255, 255);
                    if (bmp) {
                        entry = rend_sokol_atlas_insert(
                            &d->rend.atlas, d->rend.font->font_data[style],
                            (int)glyph_id, color_key, bmp, false);
                        d->rend.font->free_glyph_bitmap(d->rend.font, bmp);
                    } else {
                        entry = rend_sokol_atlas_insert_empty(
                            &d->rend.atlas, d->rend.font->font_data[style],
                            (int)glyph_id, color_key);
                    }
                }

                if (entry && entry->region.w > 0 && entry->region.h > 0) {
                    int gx = (int)x0 + entry->x_offset;
                    int gy = (int)y0 + d->rend.font_ascent - entry->y_offset;
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
            rend_sokol_emit_glyph_quad(q, x0, y0, x1, y1, u0, vu0, u1, vu1, fg, bg);
            vert_count += 6;
        }
    }

    rend_sokol_atlas_flush(&d->rend.atlas);

    sg_begin_pass(&(sg_pass){
        .action = {
            .colors[0] = { .load_action = SG_LOADACTION_CLEAR,
                           .clear_value = { rend_srgb_to_linear(TERM_BG_R) / 255.0f,
                                            rend_srgb_to_linear(TERM_BG_G) / 255.0f,
                                            rend_srgb_to_linear(TERM_BG_B) / 255.0f, 1.0f } } },
        .swapchain = sglue_swapchain(),
    });

    if (vert_count > 0 && d->rend.glyph_pip_created) {
        sg_update_buffer(d->rend.glyph_vbuf, &(sg_range){ .ptr = verts, .size = (size_t)vert_count * sizeof(GlyphVertex) });
        float resolution[2] = { (float)img_w, (float)img_h };
        sg_apply_pipeline(d->rend.glyph_pip);
        sg_apply_bindings(&(sg_bindings){
            .vertex_buffers[0] = d->rend.glyph_vbuf,
            .views[0] = d->rend.atlas.texture_view,
            .samplers[0] = d->rend.atlas.sampler,
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
    out->linear_light = d->rend.linear_ok;
    out->glyph_shader = false;
    out->content_scale = d->rend.content_scale;
    out->pixel_width = (int)sapp_width();
    out->pixel_height = (int)sapp_height();
    out->cell_width = d->rend.cell_w;
    out->cell_height = d->rend.cell_h;
    out->font_path = d->rend.font_path;
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

    .panel_show = sokol_panel_show,
    .panel_hide = sokol_panel_hide,
    .panel_hit_test = sokol_panel_hit_test,
    .panel_set_hover = sokol_panel_set_hover,

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

static void sokol_record_frame(SokolData *d, const char *path)
{
    (void)d;
#if !defined(SOKOL_GLCORE)
    (void)path;
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

    uint8_t *flipped = malloc((size_t)ss_w * ss_h * 4);
    if (flipped) {
        for (int y = 0; y < ss_h; y++) {
            memcpy(flipped + (size_t)y * ss_w * 4,
                   pixels + (size_t)(ss_h - 1 - y) * ss_w * 4,
                   (size_t)ss_w * 4);
        }
        qoi_write_rgba(path, flipped, ss_w, ss_h);
        free(flipped);
    } else {
        qoi_write_rgba(path, pixels, ss_w, ss_h);
    }
    free(pixels);
#endif
}

static void sokol_debug_mousemove(void *app, int x, int y)
{
    PorttyApp *p = (PorttyApp *)app;
    portty_app_handle_mouse(p, x, y, 0, false, 0, 0);
    if (portty_app_revalidate_hover(p, x, y))
        terminal_mark_dirty(p->term);
}

static void sokol_script_panel(void *backend, int id, int col, int row,
                               int cols, int rows,
                               const char *title, const char *body, int level,
                               unsigned int flags)
{
    PorttyBackend *self = (PorttyBackend *)backend;
    self->panel_show(self, id, col, row, cols, rows, title, body,
                     (PorttyNotifyLevel)level, flags);
}

static void sokol_script_panel_hide(void *backend, int id)
{
    PorttyBackend *self = (PorttyBackend *)backend;
    self->panel_hide(self, id);
}

static void sokol_record_start(void *user_data, int fps)
{
    SokolData *d = (SokolData *)user_data;
    if (!d)
        return;
    int interval_ms = 1000 / fps;
    d->record_timer = timer_add(d->timers, interval_ms, SOKOL_EVENT_RECORD_TICK, NULL);
}

static void sokol_record_stop(void *user_data)
{
    SokolData *d = (SokolData *)user_data;
    if (!d)
        return;
    if (d->record_timer != TIMER_INVALID) {
        timer_remove(d->timers, d->record_timer);
        d->record_timer = TIMER_INVALID;
    }
}

static void sokol_debug_dumpverts(int row, int col_start, int col_end)
{
    for (int col = col_start; col <= col_end; col++) {
        if (row < 0 || row >= SOKOL_MAX_ROWS || col < 0 || col >= SOKOL_MAX_COLS) {
            printf("  col=%3d: out of range\n", col);
            continue;
        }
        int vi = rend_sokol_get_vert_index()[row * SOKOL_MAX_COLS + col];
        if (vi < 0 || vi + 5 >= *rend_sokol_get_frame_vert_count_ptr()) {
            printf("  col=%3d: no vertices (vi=%d)\n", col, vi);
            continue;
        }
        GlyphVertex *q = &rend_sokol_get_frame_verts()[vi];
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
    GLuint gl_buf = d->glyph_vbuf_gl_id;
    if (!gl_buf) {
        printf("verifybuf: GL buffer ID not available\n");
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, gl_buf);

    for (int col = col_start; col <= col_end; col++) {
        if (row < 0 || row >= SOKOL_MAX_ROWS || col < 0 || col >= SOKOL_MAX_COLS)
            continue;
        int vi = rend_sokol_get_vert_index()[row * SOKOL_MAX_COLS + col];
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

        GlyphVertex *cpu_vert = &rend_sokol_get_frame_verts()[vi];
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

    float display_scale = portty_compute_content_scale(
        self->get_display_scale(self), app->dpi_scale);
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
        terminal_set_content_scale(app->term, display_scale);
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
    if (self->get_cell_size(self, &d->rend.cell_w, &d->rend.cell_h)) {
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
        d->script = portty_script_load(app->script_path);
        if (!d->script) {
            fprintf(stderr, "ERROR: Failed to load debug script: %s: out of memory\n",
                    app->script_path);
            exit(1);
        }
        const char *err = portty_script_error(d->script);
        if (err) {
            fprintf(stderr, "ERROR: %s: %s\n", app->script_path, err);
            exit(1);
        }
        d->frame_recorder = frame_recorder_new();
        if (!d->frame_recorder) {
            fprintf(stderr, "ERROR: Failed to allocate frame recorder\n");
            exit(1);
        }
    }

    // Ensure the very first frame is rendered.  coffer starts with no damage
    // pending, so without an explicit mark the window would stay blank until
    // the next PTY/timer event.
    terminal_mark_dirty(d->term);

    vlog("sokol_finish_setup: complete, win=%dx%d\n", win_w, win_h);
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
        } else if (events[i].code == SOKOL_EVENT_RECORD_TICK) {
            if (d->frame_recorder && d->frame_recorder->recording) {
                d->pending_record_frame = true;
                terminal_mark_dirty(d->term);
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

    if (d->pending_screendump || d->pending_verifybuf ||
        d->pending_record_frame)
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
    if (d->script && !d->script_done) {
        ScriptExecCtx ctx = {
            .backend = g_sokol.backend,
            .term = d->term,
            .pty = d->pty,
            .scroll_offset = d->rend.scroll.scroll_offset,
            .emit_fn = (void (*)(void *, const char *, size_t))portty_app_feed_terminal,
            .emit_user_data = g_sokol.app,
            .pending_screendump = &d->pending_screendump,
            .screendump_path_buf = d->screendump_path,
            .pending_verifybuf = &d->pending_verifybuf,
            .verify_row = &d->verify_row,
            .verify_col_start = &d->verify_col_start,
            .verify_col_end = &d->verify_col_end,
            .dumpverts_fn = sokol_debug_dumpverts,
            .mousemove_fn = sokol_debug_mousemove,
            .mousemove_user_data = g_sokol.app,
            .panel_fn = sokol_script_panel,
            .panel_user_data = g_sokol.backend,
            .panel_hide_fn = sokol_script_panel_hide,
            .panel_hide_user_data = g_sokol.backend,
            .recorder = d->frame_recorder,
            .pending_record_frame = &d->pending_record_frame,
            .record_start_fn = sokol_record_start,
            .record_stop_fn = sokol_record_stop,
            .record_user_data = d,
        };
        portty_script_step(d->script, &d->cmd_index, &ctx);
        if (d->cmd_index >= portty_script_count(d->script))
            d->script_done = true;
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
        if (d->pending_screendump) {
            d->pending_screendump = false;
            sokol_debug_screendump(d, d->screendump_path);
            if (d->screenshot_saved)
                d->quit_requested = true;
        }

        // === Debug script: frame capture ===
        if (d->pending_record_frame) {
            d->pending_record_frame = false;
            frame_recorder_build_path(d->frame_recorder, d->screendump_path,
                                      sizeof(d->screendump_path));
            sokol_record_frame(d, d->screendump_path);
            frame_recorder_advance(d->frame_recorder);
        }

        d->self->present(d->self);
        terminal_clear_redraw(d->term);

        // === Debug script: post-present deferred commands ===
        if (d->pending_verifybuf) {
            d->pending_verifybuf = false;
            sokol_debug_verifybuf(d, d->verify_row,
                                  d->verify_col_start,
                                  d->verify_col_end);
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
        } else if ((kr.handled || kr.len > 0) && rend_get_scroll_offset(&d->rend.scroll) != 0) {
            rend_reset_scroll(&d->rend.scroll);
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
            if ((kr.handled || kr.len > 0) && rend_get_scroll_offset(&d->rend.scroll) != 0)
                rend_reset_scroll(&d->rend.scroll);
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
