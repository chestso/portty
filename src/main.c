#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bloom_pty.h"
#include "common.h"
#include "event_loop.h"
#include "event_loop_sdl3.h"
#include "font.h"
#include "font_ft.h"
#include "font_ft_internal.h"
#include "font_resolver.h"
#include "png_writer.h"
#include "rend.h"
#include "rend_sdl3.h"
#include "term.h"
#include "term_vt.h"
#include "unicode.h"
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

/* Global verbose flag - controls debug output */
int verbose = 0;

// Function prototypes
static void print_usage(const char *progname);
static int png_render_text(const char *text, const char *output_path);

// Context passed to event loop callbacks
typedef struct
{
    TerminalBackend *term;
    RendererBackend *rend;
    PtyContext *pty;
} MainContext;

// Keyboard callback for event loop
static KeyboardResult on_keyboard(void *user_data, int key, int mod, bool is_text,
                                  const char *text)
{
    MainContext *ctx = (MainContext *)user_data;
    KeyboardResult result = { 0 };

    // Handle text input for proper Unicode support
    if (is_text && text) {
        size_t text_len = strlen(text);
        if (text_len > 0 && text_len < sizeof(result.data)) {
            memcpy(result.data, text, text_len);
            result.len = text_len;
        }
        return result;
    }

    // Ctrl+Q: exit
    if (key == SDLK_Q && (mod & SDL_KMOD_CTRL)) {
        result.request_quit = true;
        return result;
    }

    // Convert SDL key events to terminal input
    switch (key) {
    case SDLK_RETURN:
        result.data[0] = '\r';
        result.len = 1;
        break;
    case SDLK_BACKSPACE:
        result.data[0] = '\x7f'; // DEL character
        result.len = 1;
        break;
    case SDLK_ESCAPE:
        result.data[0] = '\x1b'; // ESC character
        result.len = 1;
        break;
    case SDLK_TAB:
        result.data[0] = '\t';
        result.len = 1;
        break;
    case SDLK_UP:
        strcpy(result.data, "\x1b[A");
        result.len = 3;
        break;
    case SDLK_DOWN:
        strcpy(result.data, "\x1b[B");
        result.len = 3;
        break;
    case SDLK_RIGHT:
        strcpy(result.data, "\x1b[C");
        result.len = 3;
        break;
    case SDLK_LEFT:
        strcpy(result.data, "\x1b[D");
        result.len = 3;
        break;
    case SDLK_HOME:
        strcpy(result.data, "\x1b[H");
        result.len = 3;
        break;
    case SDLK_END:
        strcpy(result.data, "\x1b[F");
        result.len = 3;
        break;
    case SDLK_INSERT:
        strcpy(result.data, "\x1b[2~");
        result.len = 4;
        break;
    case SDLK_DELETE:
        strcpy(result.data, "\x1b[3~");
        result.len = 4;
        break;
    case SDLK_PAGEUP:
        if (mod & SDL_KMOD_SHIFT) {
            // Shift+PageUp: scroll back one page
            int rows, cols;
            terminal_get_dimensions(ctx->term, &rows, &cols);
            renderer_scroll(ctx->rend, ctx->term, rows);
            result.force_redraw = true;
            result.handled = true;
        } else {
            strcpy(result.data, "\x1b[5~");
            result.len = 4;
        }
        break;
    case SDLK_PAGEDOWN:
        if (mod & SDL_KMOD_SHIFT) {
            // Shift+PageDown: scroll forward one page
            int rows, cols;
            terminal_get_dimensions(ctx->term, &rows, &cols);
            renderer_scroll(ctx->rend, ctx->term, -rows);
            result.force_redraw = true;
            result.handled = true;
        } else {
            strcpy(result.data, "\x1b[6~");
            result.len = 4;
        }
        break;
    default:
        // Handle printable characters
        if (key >= 32 && key < 127) {
            // Check for Ctrl+G specifically (debug grid toggle)
            if (key == 'g' || key == 'G') {
                if (mod & SDL_KMOD_CTRL) {
                    if (ctx->rend) {
                        renderer_toggle_debug_grid(ctx->rend);
                        result.force_redraw = true;
                        result.handled = true;
                    }
                    vlog("Ctrl+G pressed, debug grid toggled\n");
                } else {
                    result.data[0] = (char)key;
                    result.len = 1;
                }
            } else if (mod & SDL_KMOD_CTRL) {
                // Ctrl+letter: send as control character
                char ch = (char)key;
                if (ch >= 'a' && ch <= 'z') {
                    result.data[0] = (char)(ch - 'a' + 1);
                    result.len = 1;
                } else if (ch >= 'A' && ch <= 'Z') {
                    result.data[0] = (char)(ch - 'A' + 1);
                    result.len = 1;
                }
            } else {
                result.data[0] = (char)key;
                result.len = 1;
            }
        }
        break;
    }

    return result;
}

// Window resize callback for event loop
static void on_resize(void *user_data, int pixel_w, int pixel_h)
{
    MainContext *ctx = (MainContext *)user_data;

    renderer_resize(ctx->rend, pixel_w, pixel_h);

    int cell_w, cell_h;
    if (renderer_get_cell_size(ctx->rend, &cell_w, &cell_h)) {
        int cols = pixel_w / cell_w;
        int rows = pixel_h / cell_h;
        if (cols > 0 && rows > 0) {
            terminal_resize(ctx->term, cols, rows);
            pty_resize(ctx->pty, rows, cols);
        }
    }
}

// Scroll callback for event loop
static void on_scroll(void *user_data, int delta)
{
    MainContext *ctx = (MainContext *)user_data;
    renderer_scroll(ctx->rend, ctx->term, delta);
}

int main(int argc, char *argv[])
{
    TerminalBackend *term;
    RendererBackend *rend = NULL;
    SDL_Window *window = NULL;
    SDL_Renderer *sdl_rend = NULL;
    PtyContext *pty = NULL;
    EventLoopBackend *event_loop = NULL;
    int running = 1;
    int opt;

    // Parse command line arguments
    int debug_grid_enabled = 0;
    int list_fonts = 0;
    int ft_hint_target = FT_LOAD_TARGET_LIGHT; // Default: light hinting
    char *png_text = NULL;
    const char *font_name = NULL;
    const char *colr_debug_path = NULL; // COLR layer debug prefix
    char **exec_argv = NULL;            // Command to run (NULL = default shell)
    float font_size = 12.0f;            // Default font size in points
    int init_cols = DEFAULT_COLS;
    int init_rows = DEFAULT_ROWS;

    static struct option long_options[] = {
        { "list-fonts", no_argument, NULL, 'L' },
        { "ft-hinting", required_argument, NULL, 'H' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "hveds:f:g:P:D:", long_options, NULL)) != -1) {
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
        case 's':
            font_size = atof(optarg);
            if (font_size <= 0.0f) {
                fprintf(stderr, "ERROR: Invalid font size: %s\n", optarg);
                return 1;
            }
            break;
        case 'f':
            font_name = optarg;
            break;
        case 'L':
            list_fonts = 1;
            break;
        case 'H':
            if (strcmp(optarg, "none") == 0) {
                ft_hint_target = FT_LOAD_NO_HINTING;
            } else if (strcmp(optarg, "light") == 0) {
                ft_hint_target = FT_LOAD_TARGET_LIGHT;
            } else if (strcmp(optarg, "normal") == 0) {
                ft_hint_target = FT_LOAD_TARGET_NORMAL;
            } else if (strcmp(optarg, "mono") == 0) {
                ft_hint_target = FT_LOAD_TARGET_MONO;
            } else {
                fprintf(stderr, "ERROR: Invalid hinting target: %s (use none, light, normal, mono)\n", optarg);
                return 1;
            }
            break;
        case 'g':
        {
            int w = 0, h = 0;
            if (sscanf(optarg, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                init_cols = w;
                init_rows = h;
            } else {
                fprintf(stderr, "ERROR: Invalid geometry: %s (use COLSxROWS, e.g. 120x40)\n", optarg);
                return 1;
            }
            break;
        }
        case 'P':
            png_text = optarg;
            break;
        case 'D':
            colr_debug_path = optarg;
            break;
        case '?':
            print_usage(argv[0]);
            return 1;
        }
    }

    // After getopt, remaining args (after --) are the command to execute
    if (optind < argc) {
        exec_argv = &argv[optind];
    }

    // List monospace fonts and exit
    if (list_fonts) {
        if (font_resolver_init() != 0) {
            fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
            return 1;
        }
        font_resolver_list_monospace();
        font_resolver_cleanup();
        return 0;
    }

    // Set COLR layer debug prefix if specified (for debugging COLR rendering)
    if (colr_debug_path) {
        colr_set_debug_prefix(colr_debug_path);
        vlog("COLR layer debug enabled, prefix: %s\n", colr_debug_path);
    }

    // PNG render mode: skip SDL entirely, render text to PNG and exit
    if (png_text) {
        if (optind >= argc) {
            fprintf(stderr, "ERROR: -P requires output PNG path as positional argument\n");
            fprintf(stderr, "Usage: %s -P \"text\" output.png\n", argv[0]);
            return 1;
        }
        return png_render_text(png_text, argv[optind]);
    }

    // Set app metadata before SDL initialization as recommended by SDL3
    if (verbose) {
        fprintf(stderr, "DEBUG: Setting SDL app metadata\n");
    }
    if (!SDL_SetAppMetadata("bloom-term", "1.0.0", "org.bloom.term")) {
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

    // Initialize terminal with cell dimensions (cols x rows)
    term = terminal_init(&terminal_backend_vt, init_cols, init_rows);
    if (!term) {
        fprintf(stderr, "Failed to initialize terminal\n");
        SDL_Quit();
        return 1;
    }

    // Only create window and renderer if we're going to run the event loop
    if (running) {
        // Create window at placeholder size; will be resized after font loading
        vlog("Creating window (placeholder size, will resize after font load)\n");

        // Clear errors before window creation
        SDL_ClearError();

        Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;
        window = SDL_CreateWindow("bloom-term", 800, 600, window_flags);
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

        // Disable VSync for lowest input latency — we only render on
        // demand so there is no wasted GPU work or tearing concern.
        SDL_SetRenderVSync(sdl_rend, 0);

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

        // Load fonts
        if (renderer_load_fonts(rend, font_size, font_name, ft_hint_target) < 0) {
            fprintf(stderr, "Failed to load fonts\n");
            renderer_destroy(rend);
            terminal_destroy(term);
            SDL_DestroyRenderer(sdl_rend);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        // Derive window size from font cell dimensions
        int cell_w, cell_h;
        int win_w = 800, win_h = 600;
        if (renderer_get_cell_size(rend, &cell_w, &cell_h)) {
            win_w = init_cols * cell_w;
            win_h = init_rows * cell_h;
            vlog("Derived window size from font: %dx%d (%d cols * %d px, %d rows * %d px)\n",
                 win_w, win_h, init_cols, cell_w, init_rows, cell_h);
        }
        SDL_SetWindowSize(window, win_w, win_h);
        renderer_resize(rend, win_w, win_h);
        SDL_ShowWindow(window);

        // Create PTY and spawn shell (or custom command)
        pty = pty_create(init_rows, init_cols, exec_argv);
        if (!pty) {
            fprintf(stderr, "ERROR: Failed to create PTY\n");
            renderer_destroy(rend);
            terminal_destroy(term);
            SDL_DestroyRenderer(sdl_rend);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        // Initialize event loop
        event_loop = event_loop_init(&event_loop_backend_sdl3, window, sdl_rend);
        if (!event_loop) {
            fprintf(stderr, "ERROR: Failed to initialize event loop\n");
            pty_destroy(pty);
            renderer_destroy(rend);
            terminal_destroy(term);
            SDL_DestroyRenderer(sdl_rend);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        // Register PTY with event loop
        if (!event_loop_register_pty(event_loop, pty)) {
            fprintf(stderr, "ERROR: Failed to register PTY with event loop\n");
            event_loop_destroy(event_loop);
            pty_destroy(pty);
            renderer_destroy(rend);
            terminal_destroy(term);
            SDL_DestroyRenderer(sdl_rend);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }

    // Only enter event loop if running
    if (running) {
        // Enable debug grid if requested via CLI
        if (debug_grid_enabled && rend) {
            renderer_toggle_debug_grid(rend);
            vlog("Debug grid enabled via CLI flag\n");
        }

        // Set up context and callbacks for event loop
        MainContext main_ctx = {
            .term = term,
            .rend = rend,
            .pty = pty,
        };

        EventLoopCallbacks callbacks = {
            .on_keyboard = on_keyboard,
            .on_resize = on_resize,
            .on_scroll = on_scroll,
            .user_data = &main_ctx,
        };

        // Run the event loop
        event_loop_run(event_loop, term, rend, &callbacks);
    }

    // Cleanup
    if (event_loop)
        event_loop_destroy(event_loop);
    if (pty)
        pty_destroy(pty);
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
    printf("Usage: %s [OPTIONS] [-- COMMAND [ARGS...]]\n", progname);
    printf("Terminal emulator using libvterm and SDL3\n\n");
    printf("Options:\n");
    printf("  -h          Show this help message\n");
    printf("  -v          Verbose output (debug information)\n");
    printf("  -e          Exit immediately (for testing)\n");
    printf("  -d          Enable debug grid (for testing)\n");
    printf("  -s SIZE     Font size in points (default: 12.0)\n");
    printf("  -f FONT     Font family name (e.g., \"Adwaita Mono\")\n");
    printf("  -g COLSxROWS  Initial terminal size (default: 80x24)\n");
    printf("  --ft-hinting S  Set FreeType hinting: none, light, normal, mono (default: light)\n");
    printf("  --list-fonts  List available monospace fonts and exit\n");
    printf("  -P TEXT     Render TEXT to a PNG file (output path as positional arg)\n");
    printf("  -D PREFIX   Debug COLR layers: save each layer as PREFIX_layer00.png, etc.\n");
    printf("\n");
    printf("Command execution:\n");
    printf("  Use -- to separate options from command. Without --, runs default shell.\n");
    printf("  Examples:\n");
    printf("    %s                              # Run default shell\n", progname);
    printf("    %s -- htop                      # Run htop directly\n", progname);
    printf("    %s -- sh -c 'echo hello'        # Run shell command\n", progname);
    printf("\n");
    printf("Runtime controls:\n");
    printf("  Ctrl+G      Toggle debug grid\n");
    printf("  Ctrl+Q      Quit\n");
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
    options.ft_hint_target = FT_LOAD_TARGET_LIGHT;
    options.dpi_x = 72;
    options.dpi_y = 72;

    /* Load emoji font */
    FontResolutionResult result;
    if (font_resolver_find_font(FONT_TYPE_EMOJI, NULL, &result) != 0) {
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
