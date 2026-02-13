#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "png_mode.h"
#include "rend.h"
#include "rend_sdl3.h"
#include "term.h"
#include "term_vt.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

int png_render_text(const char *text, const char *output_path,
                    const char *font_name, int ft_hint_target)
{
    const float font_size = 12.0f;
    int ret = 1;
    SDL_Window *window = NULL;
    SDL_Renderer *sdl_rend = NULL;
    TerminalBackend *term = NULL;
    RendererBackend *rend = NULL;

    vlog("PNG mode: text=\"%s\", output=%s\n", text, output_path);

    // Initialize SDL (needed for render target even in headless mode)
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "ERROR: Failed to initialize SDL: %s\n", SDL_GetError());
        return 1;
    }

    // Create a hidden window + renderer for offscreen rendering
    window = SDL_CreateWindow("bloom-png", 1, 1, SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "ERROR: Failed to create hidden window: %s\n", SDL_GetError());
        goto cleanup;
    }

    sdl_rend = SDL_CreateRenderer(window, NULL);
    if (!sdl_rend) {
        fprintf(stderr, "ERROR: Failed to create renderer: %s\n", SDL_GetError());
        goto cleanup;
    }

    // Use a generous column count — we trim to actual content later
    int cols = (int)strlen(text) + 4;
    if (cols < 10)
        cols = 10;
    int rows = 1;

    // Initialize terminal and feed text
    term = terminal_init(&terminal_backend_vt, cols, rows);
    if (!term) {
        fprintf(stderr, "ERROR: Failed to initialize terminal for PNG\n");
        goto cleanup;
    }

    terminal_process_input(term, text, strlen(text));

    // Initialize renderer backend
    rend = renderer_init(&renderer_backend_sdl3, window, sdl_rend);
    if (!rend) {
        fprintf(stderr, "ERROR: Failed to initialize renderer for PNG\n");
        goto cleanup;
    }

    // Load fonts using the same pipeline as interactive mode
    if (renderer_load_fonts(rend, font_size, font_name, ft_hint_target) < 0) {
        fprintf(stderr, "ERROR: Failed to load fonts for PNG\n");
        goto cleanup;
    }

    // Render to PNG via the backend
    ret = renderer_render_to_png(rend, term, output_path);

cleanup:
    if (rend)
        renderer_destroy(rend);
    if (sdl_rend)
        SDL_DestroyRenderer(sdl_rend);
    if (window)
        SDL_DestroyWindow(window);
    if (term)
        terminal_destroy(term);
    SDL_Quit();
    return ret;
}
