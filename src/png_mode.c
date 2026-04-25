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
#include "term_vt.h"
#include <SDL3/SDL.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Backend defaults to bloom-vt; legacy libvterm path stays opt-in via
 * BLOOM_TERMINAL_VT=libvterm during the removal window. */
static TerminalBackend *select_backend(void)
{
    const char *vt_choice = getenv("BLOOM_TERMINAL_VT");
    if (vt_choice && strcmp(vt_choice, "libvterm") == 0)
        return &terminal_backend_vt;
    return &terminal_backend_bvt;
}

/* Common SDL + renderer setup. Caller must free the resources via
 * cleanup_render_context() in reverse order. */
typedef struct {
    SDL_Window      *window;
    SDL_Renderer    *sdl_rend;
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
    ctx->sdl_rend = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->sdl_rend) {
        fprintf(stderr, "ERROR: Failed to create renderer: %s\n", SDL_GetError());
        return -1;
    }
    ctx->term = terminal_init(select_backend(), cols, rows);
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
    return 0;
}

static void cleanup_render_context(RenderContext *ctx)
{
    if (ctx->rend)     renderer_destroy(ctx->rend);
    if (ctx->sdl_rend) SDL_DestroyRenderer(ctx->sdl_rend);
    if (ctx->window)   SDL_DestroyWindow(ctx->window);
    if (ctx->term)     terminal_destroy(ctx->term);
    SDL_Quit();
}

int png_render_text(const char *text, const char *output_path,
                    const char *font_name, int ft_hint_target)
{
    vlog("PNG mode: text=\"%s\", output=%s\n", text, output_path);

    /* Generous column count — renderer trims to actual content. */
    int cols = (int)strlen(text) + 4;
    if (cols < 10) cols = 10;
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
    int fd = pty_get_master_fd(pty);
    long long deadline = now_ms() + wait_ms;
    char buf[4096];
    while (now_ms() < deadline) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int wait = (int)(deadline - now_ms());
        if (wait <= 0) break;
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
            if (n <= 0) break;
            terminal_process_input(ctx.term, buf, (size_t)n);
        }
        if (pfd.revents & (POLLHUP | POLLERR)) break;
    }

    ret = renderer_render_to_png(ctx.rend, ctx.term, output_path);

cleanup:
    cleanup_render_context(&ctx);
    pty_destroy(pty);
    pty_signal_cleanup();
    return ret;
}
