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

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

/* Global verbose flag - controls debug output */
int verbose = 0;

/* Bench mode scripted event structures */
typedef struct BenchEvent
{
    uint32_t delay_ms; // delay before this event
    SDL_Keycode key;   // SDL keycode
    SDL_Keymod mod;    // modifier (SDL_KMOD_CTRL for Ctrl combos)
} BenchEvent;

typedef struct BenchScript
{
    BenchEvent *events;
    int count;
    int capacity;
} BenchScript;

// Function prototypes
static void print_usage(const char *progname);
static void process_input_from_source(TerminalBackend *term, const char *source);
static int png_render_text(const char *text, const char *output_path);
static BenchScript *bench_parse_script(const char *path);
static void bench_free_script(BenchScript *script);

/* Thread function: pushes bench script events with real delays */
static int bench_thread_func(void *data)
{
    BenchScript *script = (BenchScript *)data;
    for (int i = 0; i < script->count; i++) {
        if (script->events[i].delay_ms > 0)
            SDL_Delay(script->events[i].delay_ms);

        SDL_Event synth = { 0 };
        synth.type = SDL_EVENT_KEY_DOWN;
        synth.key.key = script->events[i].key;
        synth.key.mod = script->events[i].mod;
        SDL_PushEvent(&synth);
    }
    return 0;
}

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
    int list_fonts = 0;
    int bench_timing = 0;
    const char *bench_script_path = NULL;
    int ft_hint_target = FT_LOAD_NO_HINTING; // Default: no hinting
    char *png_text = NULL;
    const char *font_name = NULL;
    float font_size = 12.0f; // Default font size in points
    int init_cols = DEFAULT_COLS;
    int init_rows = DEFAULT_ROWS;

    static struct option long_options[] = {
        { "list-fonts", no_argument, NULL, 'L' },
        { "ft-hinting", required_argument, NULL, 'H' },
        { "bench", optional_argument, NULL, 'B' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "hveds:f:g:P:", long_options, NULL)) != -1) {
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
        case 'B':
            bench_timing = 1;
            if (optarg)
                bench_script_path = optarg;
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
        case '?':
            print_usage(argv[0]);
            return 1;
        }
    }

    // Check for input source argument
    if (optind < argc) {
        input_source = argv[optind];
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
        window = SDL_CreateWindow("vterm-sdl3 Terminal", 800, 600, window_flags);
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
    }

    // Process input if provided
    if (input_source) {
        vlog("Processing input from: %s\n",
             strcmp(input_source, "-") == 0 ? "stdin" : input_source);
        process_input_from_source(term, input_source);
    }

    // Only enter event loop if running
    if (running) {
        // Enable debug grid if requested via CLI
        if (debug_grid_enabled && rend) {
            renderer_toggle_debug_grid(rend);
            vlog("Debug grid enabled via CLI flag\n");
        }

        // Parse bench script and spawn feeder thread
        BenchScript *bench = NULL;
        SDL_Thread *bench_thread = NULL;

        if (bench_script_path) {
            bench = bench_parse_script(bench_script_path);
            if (!bench) {
                fprintf(stderr, "ERROR: Failed to parse bench script: %s\n", bench_script_path);
                renderer_destroy(rend);
                terminal_destroy(term);
                SDL_DestroyRenderer(sdl_rend);
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 1;
            }
            vlog("Bench mode: loaded %d events from %s\n", bench->count, bench_script_path);
            bench_thread = SDL_CreateThread(bench_thread_func, "bench", bench);
        }

        {
            int force_redraw = 1; // Force initial render

            // Frame timing stats (bench mode)
            uint64_t perf_freq = SDL_GetPerformanceFrequency();
            double *frame_times_ms = NULL;
            int frame_count = 0;
            int frame_capacity = 0;
            int total_keys_processed = 0;

            // Main event loop
            while (running) {
                // Wait for events (bench thread pushes events that wake this up)
                int keys_this_frame = 0;
                int have_event = SDL_WaitEventTimeout(&event, 16);
                // Start timing AFTER the first event arrives (not during idle wait)
                uint64_t frame_start = SDL_GetPerformanceCounter();
                if (have_event)
                    do {
                        switch (event.type) {
                        case SDL_EVENT_QUIT:
                            running = 0;
                            break;

                        case SDL_EVENT_KEY_DOWN:
                            // Handle key presses and send to terminal
                            {
                                char key_buffer[16] = { 0 };
                                size_t len = 0;

                                // Ctrl+Q: exit
                                if (event.key.key == SDLK_Q &&
                                    (event.key.mod & SDL_KMOD_CTRL)) {
                                    running = 0;
                                    break;
                                }

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
                                        } else if (event.key.mod & SDL_KMOD_CTRL) {
                                            // Ctrl+letter: send as control character
                                            char ch = (char)event.key.key;
                                            if (ch >= 'a' && ch <= 'z') {
                                                key_buffer[0] = (char)(ch - 'a' + 1);
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
                                    keys_this_frame++;
                                }
                            }
                            break;

                        case SDL_EVENT_WINDOW_RESIZED:
                        {
                            int pixel_w = event.window.data1;
                            int pixel_h = event.window.data2;
                            renderer_resize(rend, pixel_w, pixel_h);

                            int cell_w, cell_h;
                            if (renderer_get_cell_size(rend, &cell_w, &cell_h)) {
                                int cols = pixel_w / cell_w;
                                int rows = pixel_h / cell_h;
                                if (cols > 0 && rows > 0)
                                    terminal_resize(term, cols, rows);
                            }
                            force_redraw = 1;
                            break;
                        }
                        }
                    } while (SDL_PollEvent(&event));

                // Render terminal only if needed
                if (terminal_needs_redraw(term) || force_redraw) {
                    uint64_t t_draw_start = SDL_GetPerformanceCounter();
                    renderer_draw_terminal(rend, term);
                    uint64_t t_present_start = SDL_GetPerformanceCounter();
                    SDL_RenderPresent(sdl_rend);
                    uint64_t t_present_end = SDL_GetPerformanceCounter();
                    terminal_clear_redraw(term);
                    force_redraw = 0;

                    // Record frame latency breakdown (bench mode)
                    if (bench_timing && keys_this_frame > 0) {
                        double event_ms = (double)(t_draw_start - frame_start) * 1000.0 / (double)perf_freq;
                        double draw_ms = (double)(t_present_start - t_draw_start) * 1000.0 / (double)perf_freq;
                        double present_ms = (double)(t_present_end - t_present_start) * 1000.0 / (double)perf_freq;
                        double total_ms = event_ms + draw_ms + present_ms;

                        if (frame_count >= frame_capacity) {
                            frame_capacity = frame_capacity ? frame_capacity * 2 : 1024;
                            frame_times_ms = realloc(frame_times_ms, frame_capacity * sizeof(double));
                        }
                        frame_times_ms[frame_count++] = total_ms;
                        total_keys_processed += keys_this_frame;

                        // Print per-frame breakdown for first 20 frames
                        if (frame_count <= 20) {
                            fprintf(stderr, "  frame %3d: events=%.3fms draw=%.3fms present=%.3fms total=%.3fms keys=%d\n",
                                    frame_count, event_ms, draw_ms, present_ms, total_ms, keys_this_frame);
                        }
                    }

                    // Log atlas stats after rendering activity
                    renderer_log_stats(rend);
                }
            }

            // Print frame latency summary in bench mode
            if (bench_timing && frame_count > 0) {
                // Sort for percentiles
                for (int i = 0; i < frame_count - 1; i++)
                    for (int j = i + 1; j < frame_count; j++)
                        if (frame_times_ms[j] < frame_times_ms[i]) {
                            double t = frame_times_ms[i];
                            frame_times_ms[i] = frame_times_ms[j];
                            frame_times_ms[j] = t;
                        }

                double sum = 0, min_ms = frame_times_ms[0], max_ms = frame_times_ms[frame_count - 1];
                for (int i = 0; i < frame_count; i++)
                    sum += frame_times_ms[i];

                fprintf(stderr, "\n=== Bench Frame Latency (input-to-render) ===\n");
                fprintf(stderr, "  Frames rendered: %d\n", frame_count);
                fprintf(stderr, "  Keys processed:  %d\n", total_keys_processed);
                fprintf(stderr, "  Keys/frame avg:  %.1f\n", (double)total_keys_processed / frame_count);
                fprintf(stderr, "  Min:    %7.3f ms\n", min_ms);
                fprintf(stderr, "  Avg:    %7.3f ms\n", sum / frame_count);
                fprintf(stderr, "  Median: %7.3f ms\n", frame_times_ms[frame_count / 2]);
                fprintf(stderr, "  P95:    %7.3f ms\n", frame_times_ms[(int)(frame_count * 0.95)]);
                fprintf(stderr, "  P99:    %7.3f ms\n", frame_times_ms[(int)(frame_count * 0.99)]);
                fprintf(stderr, "  Max:    %7.3f ms\n", max_ms);
                fprintf(stderr, "  Total:  %7.1f ms\n", sum);
                fprintf(stderr, "============================================\n");
            }
            free(frame_times_ms);
        }

        if (bench_thread)
            SDL_WaitThread(bench_thread, NULL);
        bench_free_script(bench);
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
    printf("  -s SIZE     Font size in points (default: 12.0)\n");
    printf("  -f FONT     Font family name (e.g., \"Adwaita Mono\")\n");
    printf("  -g COLSxROWS  Initial terminal size (default: 80x24)\n");
    printf("  --ft-hinting S  Set FreeType hinting: none, light, normal, mono (default: none)\n");
    printf("  --list-fonts  List available monospace fonts and exit\n");
    printf("  --bench[=FILE]  Enable frame timing (optionally play scripted events from FILE)\n");
    printf("  -P TEXT     Render TEXT to a PNG file (output path as positional arg)\n");
    printf("  --          End of options (use before - for stdin)\n");
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

static void bench_add_event(BenchScript *script, uint32_t delay_ms, SDL_Keycode key, SDL_Keymod mod)
{
    if (script->count >= script->capacity) {
        script->capacity = script->capacity ? script->capacity * 2 : 256;
        script->events = realloc(script->events, script->capacity * sizeof(BenchEvent));
    }
    script->events[script->count].delay_ms = delay_ms;
    script->events[script->count].key = key;
    script->events[script->count].mod = mod;
    script->count++;
}

static BenchScript *bench_parse_script(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open bench script '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    BenchScript *script = calloc(1, sizeof(BenchScript));
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        // Skip empty lines and comments
        if (len == 0 || line[0] == '#')
            continue;

        // Parse optional leading delay
        uint32_t delay_ms = 0;
        const char *p = line;
        if (*p >= '0' && *p <= '9') {
            delay_ms = (uint32_t)strtoul(p, (char **)&p, 10);
            // Skip whitespace after delay
            while (*p == ' ' || *p == '\t')
                p++;
        }

        // Parse key_spec: expand characters and escape sequences
        int first_event = 1;
        while (*p) {
            SDL_Keycode key = 0;
            SDL_Keymod mod = 0;
            int advance = 1;

            if (*p == '\\' && *(p + 1)) {
                switch (*(p + 1)) {
                case 'n':
                    key = SDLK_RETURN;
                    advance = 2;
                    break;
                case 'b':
                    key = SDLK_BACKSPACE;
                    advance = 2;
                    break;
                case 'e':
                    key = SDLK_ESCAPE;
                    advance = 2;
                    break;
                case 'C':
                    // \C-x = Ctrl+x
                    if (*(p + 2) == '-' && *(p + 3)) {
                        char ch = *(p + 3);
                        if (ch >= 'A' && ch <= 'Z')
                            ch = (char)(ch - 'A' + 'a');
                        key = (SDL_Keycode)ch;
                        mod = SDL_KMOD_CTRL;
                        advance = 4;
                    } else {
                        key = (SDL_Keycode)*p;
                        advance = 1;
                    }
                    break;
                case '\\':
                    key = (SDL_Keycode)'\\';
                    advance = 2;
                    break;
                default:
                    // Check for arrow key names
                    if (strncmp(p + 1, "UP", 2) == 0) {
                        key = SDLK_UP;
                        advance = 3;
                    } else if (strncmp(p + 1, "DOWN", 4) == 0) {
                        key = SDLK_DOWN;
                        advance = 5;
                    } else if (strncmp(p + 1, "LEFT", 4) == 0) {
                        key = SDLK_LEFT;
                        advance = 5;
                    } else if (strncmp(p + 1, "RIGHT", 5) == 0) {
                        key = SDLK_RIGHT;
                        advance = 6;
                    } else {
                        key = (SDL_Keycode)*p;
                        advance = 1;
                    }
                    break;
                }
            } else {
                key = (SDL_Keycode)*p;
            }

            // Only the first event in a line gets the delay
            bench_add_event(script, first_event ? delay_ms : 0, key, mod);
            first_event = 0;
            p += advance;
        }
    }

    fclose(f);
    return script;
}

static void bench_free_script(BenchScript *script)
{
    if (!script)
        return;
    free(script->events);
    free(script);
}
