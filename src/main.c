#include "common.h"
#include "font.h"
#include "font_ft.h"
#include "font_resolver.h"
#include "png_writer.h"
#include "rend.h"
#include "rend_sdl3.h"
#include "term.h"
#include "term_vt.h"
#include "unicode.h"
#include <SDL3/SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WINDOW_WIDTH  800
#define WINDOW_HEIGHT 600

/* Global verbose flag - controls debug output */
int verbose = 0;

// Function prototypes
static void print_usage(const char *progname);
static void process_input_from_source(TerminalBackend *term, const char *source);
static int png_render_text(const char *text, const char *output_path);

int main(int argc, char *argv[])
{
    TerminalBackend *term;
    RendererBackend *rend = NULL;
    SDL_Window *window = NULL;
    SDL_Renderer *sdl_rend = NULL;
    int running = 1;
    SDL_Event event;
    char *input_source = NULL;
    int opt;

    // Parse command line arguments
    int debug_grid_enabled = 0;
    char *png_text = NULL;
    while ((opt = getopt(argc, argv, "hvedP:")) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'v':
            verbose = 1;
            break;
        case 'e':
            running = 0; // Set running to false to exit event loop
            fprintf(stderr, "STATUS: exit_flag_set=1\n");
            break;
        case 'd':
            debug_grid_enabled = 1;
            fprintf(stderr, "STATUS: debug grid enabled via CLI flag\n");
            break;
        case 'P':
            png_text = optarg;
            break;
        case '?':
            fprintf(stderr, "ERROR: Unknown option: -%c\n", optopt);
            print_usage(argv[0]);
            return 1;
        }
    }

    // Check for input source argument
    if (optind < argc) {
        input_source = argv[optind];
    }

    // PNG render mode: skip SDL entirely, render text to PNG and exit
    if (png_text) {
        if (!input_source) {
            fprintf(stderr, "ERROR: -P requires output PNG path as positional argument\n");
            fprintf(stderr, "Usage: %s -P \"text\" output.png\n", argv[0]);
            return 1;
        }
        return png_render_text(png_text, input_source);
    }

    // Set app metadata before SDL initialization as recommended by SDL3
    if (verbose) {
        fprintf(stderr, "DEBUG: Setting SDL app metadata\n");
    }
    if (!SDL_SetAppMetadata("vterm-sdl3", "1.0.0", "org.vterm.sdl3")) {
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

    // Clear any previous errors before attempting initialization
    SDL_ClearError();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        const char *error = SDL_GetError();
        fprintf(stderr, "ERROR: Failed to initialize SDL video subsystem\n");

        // Check if SDL provided a specific error message
        if (error && error[0] != '\0') {
            fprintf(stderr, "ERROR: SDL_GetError() returned: '%s'\n", error);
        } else {
            fprintf(stderr, "ERROR: No specific error message from SDL\n");
        }

        fprintf(stderr, "ERROR: This could be due to:\n");
        fprintf(stderr, "ERROR: 1. Missing SDL3 runtime libraries\n");
        fprintf(stderr, "ERROR: 2. No display available (DISPLAY environment variable)\n");
        fprintf(stderr, "ERROR: 3. SDL3 driver issues\n");

        return 1;
    } else {
        if (verbose) {
            fprintf(stderr, "DEBUG: SDL initialized successfully\n");
        }
    }

    // FreeType is initialized in renderer_init, not here
    vlog("FreeType will be initialized in renderer\n");

    // Initialize terminal
    term = terminal_init(&terminal_backend_vt, WINDOW_WIDTH, WINDOW_HEIGHT);
    if (!term) {
        fprintf(stderr, "Failed to initialize terminal\n");
        SDL_Quit();
        return 1;
    }

    // Only create window and renderer if we're going to run the event loop
    if (running) {
        // Create window with flags
        vlog("Creating window with dimensions %dx%d\n", WINDOW_WIDTH, WINDOW_HEIGHT);

        // Clear errors before window creation
        SDL_ClearError();

        Uint32 window_flags = SDL_WINDOW_RESIZABLE;
        window = SDL_CreateWindow("vterm-sdl3 Terminal", WINDOW_WIDTH, WINDOW_HEIGHT, window_flags);
        if (!window) {
            const char *error = SDL_GetError();
            if (error && error[0] != '\0') {
                fprintf(stderr, "ERROR: Failed to create window: %s\n", error);
            } else {
                fprintf(stderr, "ERROR: Failed to create window (no specific error message)\n");
            }
            terminal_destroy(term);
            SDL_Quit();
            return 1;
        }
        vlog("Window created successfully\n");

        // Create renderer with flags
        vlog("Creating renderer\n");

        // Clear errors before renderer creation
        SDL_ClearError();

        sdl_rend = SDL_CreateRenderer(window, NULL);
        if (!sdl_rend) {
            const char *error = SDL_GetError();
            if (error && error[0] != '\0') {
                fprintf(stderr, "ERROR: Failed to create renderer: %s\n", error);
            } else {
                fprintf(stderr, "ERROR: Failed to create renderer (no specific error message)\n");
            }
            SDL_DestroyWindow(window);
            terminal_destroy(term);
            SDL_Quit();
            return 1;
        }
        vlog("Renderer created successfully\n");

        // Initialize renderer
        rend = renderer_init(&renderer_backend_sdl3, window, sdl_rend);
        if (!rend) {
            fprintf(stderr, "Failed to initialize renderer\n");
            terminal_destroy(term);
            SDL_DestroyRenderer(sdl_rend);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        // Set initial width/height in renderer
        renderer_resize(rend, WINDOW_WIDTH, WINDOW_HEIGHT);

        // Load fonts
        if (renderer_load_fonts(rend) < 0) {
            fprintf(stderr, "Failed to load fonts\n");
            renderer_destroy(rend);
            terminal_destroy(term);
            SDL_DestroyRenderer(sdl_rend);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    // Process input if provided
    if (input_source) {
        vlog("Processing input from: %s\n",
             strcmp(input_source, "-") == 0 ? "stdin" : input_source);
        process_input_from_source(term, input_source);
    }

    // Only enter event loop if running
    if (running) {
        int force_redraw = 1; // Force initial render

        // Enable debug grid if requested via CLI
        if (debug_grid_enabled && rend) {
            renderer_toggle_debug_grid(rend);
            vlog("Debug grid enabled via CLI flag\n");
        }

        // Main event loop
        while (running) {
            // Process events
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = 0;
                    break;

                case SDL_EVENT_KEY_DOWN:
                    // Handle key presses and send to terminal
                    {
                        char key_buffer[16] = { 0 };
                        size_t len = 0;

                        // Convert SDL key events to terminal input
                        switch (event.key.key) {
                        case SDLK_RETURN:
                            vlog("ENTER key pressed, sending CRLF (%d bytes)\n", 2);
                            key_buffer[0] = '\r';
                            key_buffer[1] = '\n';
                            len = 2;
                            break;
                        case SDLK_BACKSPACE:
                            key_buffer[0] = '\x7f'; // DEL character
                            len = 1;
                            break;
                        case SDLK_ESCAPE:
                            key_buffer[0] = '\x1b'; // ESC character
                            len = 1;
                            break;
                        case SDLK_UP:
                            strcpy(key_buffer, "\x1b[A");
                            len = 3;
                            break;
                        case SDLK_DOWN:
                            strcpy(key_buffer, "\x1b[B");
                            len = 3;
                            break;
                        case SDLK_RIGHT:
                            strcpy(key_buffer, "\x1b[C");
                            len = 3;
                            break;
                        case SDLK_LEFT:
                            strcpy(key_buffer, "\x1b[D");
                            len = 3;
                            break;
                        default:
                            // Handle printable characters
                            if (event.key.key >= 32 && event.key.key < 127) {
                                // Check for Ctrl+G specifically
                                if (event.key.key == 'g' || event.key.key == 'G') {
                                    if (event.key.mod & SDL_KMOD_CTRL) {
                                        if (rend) {
                                            renderer_toggle_debug_grid(rend);
                                            force_redraw = 1;
                                        }
                                        // Don't send to terminal
                                        len = 0;
                                        vlog("Ctrl+G pressed, debug grid toggled\n");
                                    } else {
                                        key_buffer[0] = (char)event.key.key;
                                        len = 1;
                                    }
                                } else {
                                    key_buffer[0] = (char)event.key.key;
                                    len = 1;
                                }
                            }
                            break;
                        }

                        if (len > 0) {
                            terminal_process_input(term, key_buffer, len);
                        }
                    }
                    break;

                case SDL_EVENT_WINDOW_RESIZED:
                    // Handle window resize
                    terminal_resize(term, event.window.data1, event.window.data2);
                    renderer_resize(rend, event.window.data1, event.window.data2);
                    break;
                }
            }

            // Render terminal only if needed
            if (terminal_needs_redraw(term) || force_redraw) {
                renderer_draw_terminal(rend, term);
                SDL_RenderPresent(sdl_rend);
                terminal_clear_redraw(term);
                force_redraw = 0;
            }

            // Small delay to prevent excessive CPU usage
            SDL_Delay(16); // ~60 FPS
        }
    }

    // Cleanup
    if (rend)
        renderer_destroy(rend);
    if (sdl_rend)
        SDL_DestroyRenderer(sdl_rend);
    if (window)
        SDL_DestroyWindow(window);
    terminal_destroy(term);
    SDL_Quit();

    return 0;
}

void vlog(const char *format, ...)
{
    if (!verbose)
        return;

    va_list args;
    va_start(args, format);
    fprintf(stderr, "DEBUG: ");
    vfprintf(stderr, format, args);
    va_end(args);
}

static void print_usage(const char *progname)
{
    printf("Usage: %s [OPTIONS] [INPUT_FILE]\n", progname);
    printf("Terminal emulator using libvterm and SDL3\n\n");
    printf("Options:\n");
    printf("  -h          Show this help message\n");
    printf("  -v          Verbose output (debug information)\n");
    printf("  -e          Exit immediately (for testing)\n");
    printf("  -d          Enable debug grid (for testing)\n");
    printf("  -P TEXT     Render TEXT to a PNG file (output path as positional arg)\n");
    printf("\n");
    printf("Input:\n");
    printf("  INPUT_FILE  File containing terminal input to process\n");
    printf("  -           Read terminal input from stdin\n");
    printf("  (none)      Run interactively without pre-loaded input\n");
}

static void process_input_from_source(TerminalBackend *term, const char *source)
{
    FILE *input_file;
    char buffer[4096];
    size_t bytes_read;
    size_t total_bytes = 0;

    if (strcmp(source, "-") == 0) {
        input_file = stdin;
        vlog("Reading from stdin\n");
    } else {
        input_file = fopen(source, "r");
        if (!input_file) {
            fprintf(stderr, "ERROR: Cannot open file '%s': %s\n",
                    source, strerror(errno));
            return;
        }
        vlog("Opened file: %s\n", source);
    }

    // Read and process input
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), input_file)) > 0) {
        terminal_process_input(term, buffer, bytes_read);
        total_bytes += bytes_read;

        vlog("Processed %zu bytes (total: %zu)\n",
             bytes_read, total_bytes);
    }

    if (input_file != stdin) {
        fclose(input_file);
    }

    // Output machine-readable status
    fprintf(stderr, "STATUS: input_bytes=%zu\n", total_bytes);
    fprintf(stderr, "STATUS: input_source=%s\n",
            strcmp(source, "-") == 0 ? "stdin" : source);

    // Get terminal dimensions for debug output
    int rows, cols;
    if (terminal_get_dimensions(term, &rows, &cols) == 0) {
        fprintf(stderr, "STATUS: terminal_dimensions=%dx%d\n", cols, rows);
    }
}

static int png_render_text(const char *text, const char *output_path)
{
    const float font_size = 128.0f;

    /* Convert UTF-8 text to codepoints */
    uint32_t codepoints[256];
    int cp_count = utf8_to_codepoints(text, codepoints, 256);
    if (cp_count <= 0) {
        fprintf(stderr, "ERROR: Failed to decode UTF-8 text\n");
        return 1;
    }

    vlog("PNG mode: %d codepoints, output=%s\n", cp_count, output_path);

    /* Initialize font backend */
    if (!font_init(&font_backend_ft)) {
        fprintf(stderr, "ERROR: Failed to initialize font backend\n");
        return 1;
    }

    /* Initialize font resolver (fontconfig) */
    if (font_resolver_init() != 0) {
        fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
        font_destroy(&font_backend_ft);
        return 1;
    }

    /* Setup font options (72 DPI matches hb-view default) */
    FontOptions options = { 0 };
    options.antialias = true;
    options.hinting = 1;
    options.hint_style = 1;
    options.dpi_x = 72;
    options.dpi_y = 72;

    /* Load emoji font */
    FontResolutionResult result;
    if (font_resolver_find_font(FONT_TYPE_EMOJI, &result) != 0) {
        fprintf(stderr, "ERROR: Failed to find emoji font\n");
        font_resolver_cleanup();
        font_destroy(&font_backend_ft);
        return 1;
    }

    if (!font_load_font(&font_backend_ft, FONT_STYLE_EMOJI, result.font_path, font_size, &options)) {
        fprintf(stderr, "ERROR: Failed to load emoji font from %s\n", result.font_path);
        font_resolver_free_result(&result);
        font_resolver_cleanup();
        font_destroy(&font_backend_ft);
        return 1;
    }

    vlog("Loaded emoji font: %s\n", result.font_path);
    font_resolver_free_result(&result);
    font_resolver_cleanup();

    /* Shape and render */
    ShapedGlyphs *shaped = font_render_shaped_text(&font_backend_ft, FONT_STYLE_EMOJI,
                                                   codepoints, cp_count,
                                                   255, 255, 255);
    if (!shaped || shaped->num_glyphs == 0) {
        fprintf(stderr, "ERROR: font_render_shaped_text returned no glyphs\n");
        font_destroy(&font_backend_ft);
        return 1;
    }

    vlog("Shaped %d glyphs, total_advance=%d\n", shaped->num_glyphs, shaped->total_advance);

    /* Use font metrics to determine image size (matches hb-view behavior) */
    const FontMetrics *metrics = font_get_metrics(&font_backend_ft, FONT_STYLE_EMOJI);
    if (!metrics) {
        fprintf(stderr, "ERROR: Failed to get font metrics\n");
        font_destroy(&font_backend_ft);
        return 1;
    }

    int baseline = metrics->ascent;
    int img_w = shaped->total_advance;
    int img_h = metrics->ascent + metrics->descent;

    if (img_w <= 0 || img_h <= 0) {
        fprintf(stderr, "ERROR: Computed image has zero size (%dx%d)\n", img_w, img_h);
        font_destroy(&font_backend_ft);
        return 1;
    }

    vlog("Output image: %dx%d (advance=%d, ascent=%d, descent=%d)\n",
         img_w, img_h, shaped->total_advance, metrics->ascent, metrics->descent);

    /* Allocate output buffer (transparent black) */
    uint8_t *pixels = calloc(img_w * img_h * 4, 1);
    if (!pixels) {
        fprintf(stderr, "ERROR: Failed to allocate %dx%d pixel buffer\n", img_w, img_h);
        font_destroy(&font_backend_ft);
        return 1;
    }

    /* Composite each glyph bitmap into the output */
    for (int i = 0; i < shaped->num_glyphs; i++) {
        GlyphBitmap *gb = shaped->bitmaps[i];
        if (!gb || !gb->pixels)
            continue;

        int dst_x = shaped->x_positions[i] + gb->x_offset;
        int dst_y = baseline - gb->y_offset;

        for (int row = 0; row < gb->height; row++) {
            int dy = dst_y + row;
            if (dy < 0 || dy >= img_h)
                continue;
            for (int col = 0; col < gb->width; col++) {
                int dx = dst_x + col;
                if (dx < 0 || dx >= img_w)
                    continue;

                int src_idx = (row * gb->width + col) * 4;
                int dst_idx = (dy * img_w + dx) * 4;

                uint8_t sr = gb->pixels[src_idx + 0];
                uint8_t sg = gb->pixels[src_idx + 1];
                uint8_t sb = gb->pixels[src_idx + 2];
                uint8_t sa = gb->pixels[src_idx + 3];

                if (sa == 0)
                    continue;

                /* Alpha-over compositing (source over destination) */
                uint8_t da = pixels[dst_idx + 3];
                if (da == 0 || sa == 255) {
                    pixels[dst_idx + 0] = sr;
                    pixels[dst_idx + 1] = sg;
                    pixels[dst_idx + 2] = sb;
                    pixels[dst_idx + 3] = sa;
                } else {
                    uint16_t out_a = sa + da * (255 - sa) / 255;
                    if (out_a > 0) {
                        pixels[dst_idx + 0] = (uint8_t)((sr * sa + pixels[dst_idx + 0] * da * (255 - sa) / 255) / out_a);
                        pixels[dst_idx + 1] = (uint8_t)((sg * sa + pixels[dst_idx + 1] * da * (255 - sa) / 255) / out_a);
                        pixels[dst_idx + 2] = (uint8_t)((sb * sa + pixels[dst_idx + 2] * da * (255 - sa) / 255) / out_a);
                        pixels[dst_idx + 3] = (uint8_t)out_a;
                    }
                }
            }
        }
    }

    /* Write PNG */
    int rc = png_write_rgba(output_path, pixels, img_w, img_h);
    if (rc == 0) {
        fprintf(stderr, "STATUS: png_output=%s (%dx%d)\n", output_path, img_w, img_h);
    } else {
        fprintf(stderr, "ERROR: Failed to write PNG to %s\n", output_path);
    }

    free(pixels);
    font_destroy(&font_backend_ft);
    return rc == 0 ? 0 : 1;
}
