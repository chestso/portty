#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bloom_pty.h"
#include "common.h"
#include "png_mode.h"
#include "rend.h"
#include "rend_sdl3.h"
#include "term.h"
#include "term_bvt.h"
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static TerminalBackend *select_backend(void) { return &terminal_backend_bvt; }

/* Common SDL + renderer setup. Caller must free the resources via
 * cleanup_render_context() in reverse order. */
typedef struct
{
    SDL_Window *window;
    SDL_Renderer *sdl_rend;
    TerminalBackend *term;
    RendererBackend *rend;
} RenderContext;

static int init_render_context(RenderContext *ctx, int cols, int rows,
                               const char *font_name, int ft_hint_target)
{
    const float font_size = 12.0f;
    memset(ctx, 0, sizeof(*ctx));

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "ERROR: Failed to initialize SDL: %s\n", SDL_GetError());
        return -1;
    }
    ctx->window = SDL_CreateWindow("bloom-png", 1, 1, SDL_WINDOW_HIDDEN);
    if (!ctx->window) {
        fprintf(stderr, "ERROR: Failed to create hidden window: %s\n", SDL_GetError());
        return -1;
    }
    // Use SDL's GPU renderer so PNG output matches on-screen rendering
    // (gamma-correct linear-light blending; see platform_sdl3.c).
    SDL_PropertiesID rprops = SDL_CreateProperties();
    if (rprops) {
        SDL_SetPointerProperty(rprops, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, ctx->window);
        SDL_SetStringProperty(rprops, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
        ctx->sdl_rend = SDL_CreateRendererWithProperties(rprops);
        SDL_DestroyProperties(rprops);
    }
    if (!ctx->sdl_rend) {
        fprintf(stderr, "ERROR: Failed to create GPU renderer: %s\n", SDL_GetError());
        return -1;
    }
    BvtConfig cfg = BVT_CONFIG_DEFAULTS;
    cfg.rows = rows;
    cfg.cols = cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    ctx->term = terminal_init(select_backend(), &cfg);
    if (!ctx->term) {
        fprintf(stderr, "ERROR: Failed to initialize terminal for PNG\n");
        return -1;
    }
    ctx->rend = renderer_init(&renderer_backend_sdl3, ctx->window, ctx->sdl_rend);
    if (!ctx->rend) {
        fprintf(stderr, "ERROR: Failed to initialize renderer for PNG\n");
        return -1;
    }
    if (renderer_load_fonts(ctx->rend, font_size, font_name, ft_hint_target) < 0) {
        fprintf(stderr, "ERROR: Failed to load fonts for PNG\n");
        return -1;
    }
    // Let the VT engine place sixel images by giving it the cell pixel size.
    int cell_w, cell_h;
    if (renderer_get_cell_size(ctx->rend, &cell_w, &cell_h))
        terminal_set_cell_px(ctx->term, cell_w, cell_h);
    return 0;
}

static void cleanup_render_context(RenderContext *ctx)
{
    if (ctx->rend)
        renderer_destroy(ctx->rend);
    if (ctx->sdl_rend)
        SDL_DestroyRenderer(ctx->sdl_rend);
    if (ctx->window)
        SDL_DestroyWindow(ctx->window);
    if (ctx->term)
        terminal_destroy(ctx->term);
    SDL_Quit();
}

int png_render_text(const char *text, const char *output_path,
                    const char *font_name, int ft_hint_target)
{
    vlog("PNG mode: text=\"%s\", output=%s\n", text, output_path);

    /* Generous column count — renderer trims to actual content. */
    int cols = (int)strlen(text) + 4;
    if (cols < 10)
        cols = 10;
    int rows = 1;

    RenderContext ctx;
    int ret = 1;
    if (init_render_context(&ctx, cols, rows, font_name, ft_hint_target) < 0)
        goto cleanup;

    terminal_process_input(ctx.term, text, strlen(text));
    ret = renderer_render_to_png(ctx.rend, ctx.term, output_path);

cleanup:
    cleanup_render_context(&ctx);
    return ret;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int png_render_exec(const char *cmd, int wait_ms, int cols, int rows,
                    const char *output_path, const char *font_name,
                    int ft_hint_target)
{
    vlog("PNG mode: exec=\"%s\", wait=%dms, geometry=%dx%d, output=%s\n",
         cmd, wait_ms, cols, rows, output_path);

    if (pty_signal_init() != 0) {
        fprintf(stderr, "ERROR: pty_signal_init failed\n");
        return 1;
    }

    char *const argv[] = { "sh", "-c", (char *)cmd, NULL };
    PtyContext *pty = pty_create(rows, cols, argv);
    if (!pty) {
        fprintf(stderr, "ERROR: pty_create failed\n");
        pty_signal_cleanup();
        return 1;
    }

    RenderContext ctx;
    int ret = 1;
    if (init_render_context(&ctx, cols, rows, font_name, ft_hint_target) < 0)
        goto cleanup;

    /* Drain PTY into the chosen backend until child exits or wait_ms
     * elapses. After EOF we still pump for a short tail in case the
     * kernel hasn't delivered every byte yet. */
#ifndef _WIN32
    int fd = pty_get_master_fd(pty);
#endif
    long long deadline = now_ms() + wait_ms;
    char buf[4096];
    while (now_ms() < deadline) {
#ifdef _WIN32
        HANDLE h = (HANDLE)pty_get_process_handle(pty);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wait_result =
                WaitForSingleObject(h, (wait_ms > 50) ? 50 : (DWORD)wait_ms);
            (void)wait_result;
        }
        ssize_t n = pty_read(pty, buf, sizeof(buf));
        if (n > 0)
            terminal_process_input(ctx.term, buf, (size_t)n);
        else if (n <= 0)
            break;
        if (!pty_is_running(pty)) {
            n = pty_read(pty, buf, sizeof(buf));
            if (n > 0)
                terminal_process_input(ctx.term, buf, (size_t)n);
            break;
        }
#else
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int wait = (int)(deadline - now_ms());
        if (wait <= 0)
            break;
        int r = poll(&pfd, 1, wait);
        if (r <= 0) {
            if (!pty_is_running(pty)) {
                ssize_t n = pty_read(pty, buf, sizeof(buf));
                if (n > 0)
                    terminal_process_input(ctx.term, buf, (size_t)n);
                break;
            }
            continue;
        }
        if (pfd.revents & POLLIN) {
            ssize_t n = pty_read(pty, buf, sizeof(buf));
            if (n <= 0)
                break;
            terminal_process_input(ctx.term, buf, (size_t)n);
        }
        if (pfd.revents & (POLLHUP | POLLERR))
            break;
#endif
    }

    ret = renderer_render_to_png(ctx.rend, ctx.term, output_path);

cleanup:
    cleanup_render_context(&ctx);
    pty_destroy(pty);
    pty_signal_cleanup();
    return ret;
}
