#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bloom_conf.h"
#include "bloom_pty.h"
#include "common.h"
#include "font_ft_internal.h"
#include "font_resolve.h"
#include "font_resolve_fc.h"
#include "platform.h"
#include "platform_gtk4.h"
#include "platform_sdl3.h"
#include "png_mode.h"
#include "rend.h"
#include "rend_sdl3.h"
#include "term.h"
#include "term_vt.h"
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

/* Global verbose flag - controls debug output */
int verbose = 0;

// Function prototypes
static void print_usage(const char *progname);

// Context passed to platform callbacks
typedef struct
{
    TerminalBackend *term;
    RendererBackend *rend;
    PtyContext *pty;
    PlatformBackend *plat;
    bool drag_pending;
    int drag_start_row; // unified row
    int drag_start_col; // display col
} MainContext;

// Convert pixel coordinates to display row/col (accounting for padding)
static bool pixel_to_cell(MainContext *ctx, int pixel_x, int pixel_y, int *out_row, int *out_col)
{
    int cell_w, cell_h;
    int pad_l = 0, pad_t = 0, pad_r = 0, pad_b = 0;
    if (!renderer_get_cell_size(ctx->rend, &cell_w, &cell_h) || cell_w <= 0 || cell_h <= 0)
        return false;
    renderer_get_padding(ctx->rend, &pad_l, &pad_t, &pad_r, &pad_b);
    *out_col = (pixel_x - pad_l) / cell_w;
    *out_row = (pixel_y - pad_t) / cell_h;
    return true;
}

// Convert display row to unified row (scrollback rows are negative)
static int display_row_to_unified(RendererBackend *rend, int display_row)
{
    int scroll_offset = renderer_get_scroll_offset(rend);
    int scrollback_row = scroll_offset - 1 - display_row;
    if (scrollback_row >= 0) {
        return -(scrollback_row + 1);
    } else {
        return display_row - scroll_offset;
    }
}

// Selection change callback — pauses/resumes PTY during selection
static void on_selection_change(bool active, void *user_data)
{
    MainContext *ctx = (MainContext *)user_data;
    if (active)
        platform_pause_pty(ctx->plat);
    else
        platform_resume_pty(ctx->plat);
}

// Key callback — receives TERM_KEY_* and TERM_MOD_* (platform-independent)
static KeyboardResult on_key(void *user_data, int key, int mod,
                             uint32_t codepoint)
{
    MainContext *ctx = (MainContext *)user_data;
    KeyboardResult result = { 0 };

    // Ctrl+C with active selection → copy and cancel (not SIGINT)
    if (codepoint == 'c' && (mod & TERM_MOD_CTRL) && !(mod & TERM_MOD_SHIFT) &&
        terminal_selection_active(ctx->term)) {
        char *text = terminal_selection_get_text(ctx->term);
        if (text) {
            platform_clipboard_set(ctx->plat, text);
            free(text);
        }
        terminal_selection_clear(ctx->term);
        result.force_redraw = true;
        result.handled = true;
        return result;
    }

    // Ctrl+Shift+C → copy
    if (key == TERM_KEY_NONE && codepoint == 'C' && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        if (terminal_selection_active(ctx->term)) {
            char *text = terminal_selection_get_text(ctx->term);
            if (text) {
                platform_clipboard_set(ctx->plat, text);
                free(text);
            }
            terminal_selection_clear(ctx->term);
            result.force_redraw = true;
        }
        result.handled = true;
        return result;
    }
    // Any key with active selection → cancel selection
    if (terminal_selection_active(ctx->term)) {
        terminal_selection_clear(ctx->term);
        result.force_redraw = true;
    }

    // Ctrl+Shift+V → paste
    if (key == TERM_KEY_NONE && codepoint == 'V' && (mod & TERM_MOD_CTRL) && (mod & TERM_MOD_SHIFT)) {
        // Try async paste first (GTK4), fall back to synchronous (SDL3)
        if (!platform_clipboard_paste_async(ctx->plat, ctx->term, ctx->pty)) {
            char *clipboard = platform_clipboard_get(ctx->plat);
            if (clipboard && clipboard[0] != '\0') {
                terminal_start_paste(ctx->term);
                pty_write(ctx->pty, clipboard, strlen(clipboard));
                terminal_end_paste(ctx->term);
            }
            platform_clipboard_free(ctx->plat, clipboard);
        }
        result.handled = true;
        return result;
    }

    // Shift+PageUp/Down: scrollback navigation (normal screen only)
    if ((mod & TERM_MOD_SHIFT) && !terminal_is_altscreen(ctx->term)) {
        if (key == TERM_KEY_PAGEUP || key == TERM_KEY_PAGEDOWN) {
            int rows, cols;
            terminal_get_dimensions(ctx->term, &rows, &cols);
            renderer_scroll(ctx->rend, ctx->term, key == TERM_KEY_PAGEUP ? rows : -rows);
            result.force_redraw = true;
            result.handled = true;
            return result;
        }
    }

    // Special keys
    if (key != TERM_KEY_NONE) {
        terminal_send_key(ctx->term, key, mod);
        result.handled = true;
        return result;
    }

    // Ctrl/Alt + printable
    if (codepoint && (mod & (TERM_MOD_CTRL | TERM_MOD_ALT))) {
        // Pass Ctrl and Alt to libvterm but not Shift (already baked into resolved char)
        int mod_no_shift = mod & ~TERM_MOD_SHIFT;
        terminal_send_char(ctx->term, codepoint, mod_no_shift);
        result.handled = true;
    }

    return result;
}

// Text input callback — pure UTF-8 from IME/compose
static KeyboardResult on_text(void *user_data, const char *text)
{
    MainContext *ctx = (MainContext *)user_data;
    KeyboardResult result = { 0 };

    if (terminal_selection_active(ctx->term)) {
        terminal_selection_clear(ctx->term);
        result.force_redraw = true;
    }

    size_t text_len = strlen(text);
    if (text_len > 0 && text_len < sizeof(result.data)) {
        memcpy(result.data, text, text_len);
        result.len = text_len;
    }

    return result;
}

// Window resize callback
static void on_resize(void *user_data, int pixel_w, int pixel_h)
{
    MainContext *ctx = (MainContext *)user_data;

    terminal_selection_clear(ctx->term);
    renderer_resize(ctx->rend, pixel_w, pixel_h);

    int cell_w, cell_h;
    int pad_l = 0, pad_t = 0, pad_r = 0, pad_b = 0;
    if (renderer_get_cell_size(ctx->rend, &cell_w, &cell_h)) {
        renderer_get_padding(ctx->rend, &pad_l, &pad_t, &pad_r, &pad_b);
        int cols = (pixel_w - pad_l - pad_r) / cell_w;
        int rows = (pixel_h - pad_t - pad_b) / cell_h;
        if (cols > 0 && rows > 0) {
            terminal_resize(ctx->term, cols, rows);
            pty_resize(ctx->pty, rows, cols);
        }
    }
}

// Scroll callback
static void on_scroll(void *user_data, int delta)
{
    MainContext *ctx = (MainContext *)user_data;
    renderer_scroll(ctx->rend, ctx->term, delta);
}

// Output callback for terminal - sends data to PTY
static void term_output_to_pty(const char *data, size_t len, void *user)
{
    PtyContext *pty = (PtyContext *)user;
    if (pty) {
        pty_write(pty, data, len);
    }
}

// Mouse callback — mod uses TERM_MOD_* flags (platform-independent)
static bool on_mouse(void *user_data, int pixel_x, int pixel_y, int button, bool pressed,
                     int clicks, int mod)
{
    MainContext *ctx = (MainContext *)user_data;

    int mouse_mode = terminal_get_mouse_mode(ctx->term);
    bool shift_held = (mod & TERM_MOD_SHIFT) != 0;

    // Forward to terminal if mouse mode is active and Shift is not held
    if (mouse_mode > 0 && !shift_held) {
        bool should_forward = false;
        bool in_altscreen = terminal_is_altscreen(ctx->term);

        if (button == 4 || button == 5) {
            should_forward = in_altscreen || (mouse_mode > 0);
        } else if (button > 0) {
            should_forward = true;
        } else {
            should_forward = (mouse_mode >= 2 && pressed) || (mouse_mode >= 3);
        }

        if (should_forward) {
            int cell_w, cell_h;
            int pad_l = 0, pad_t = 0, pad_r = 0, pad_b = 0;
            if (!renderer_get_cell_size(ctx->rend, &cell_w, &cell_h) || cell_w <= 0 ||
                cell_h <= 0)
                return false;
            renderer_get_padding(ctx->rend, &pad_l, &pad_t, &pad_r, &pad_b);
            int col = (pixel_x - pad_l) / cell_w;
            int row = (pixel_y - pad_t) / cell_h;

            int term_rows, term_cols;
            terminal_get_dimensions(ctx->term, &term_rows, &term_cols);
            if (col >= term_cols)
                col = term_cols - 1;
            if (row >= term_rows)
                row = term_rows - 1;
            if (col < 0)
                col = 0;
            if (row < 0)
                row = 0;

            terminal_send_mouse_event(ctx->term, row, col, button, pressed, mod);
            return true;
        }

        if (button == 4 || button == 5)
            return false;
    }

    // Wheel events not consumed by terminal — convert to arrow keys in altscreen
    if (button == 4 || button == 5) {
        if (terminal_is_altscreen(ctx->term)) {
            int key = (button == 4) ? TERM_KEY_UP : TERM_KEY_DOWN;
            for (int i = 0; i < 3; i++)
                terminal_send_key(ctx->term, key, TERM_MOD_NONE);
            return true;
        }
        return false;
    }

    int display_row, display_col;
    if (!pixel_to_cell(ctx, pixel_x, pixel_y, &display_row, &display_col))
        return false;

    // Clamp to terminal dimensions
    int term_rows, term_cols;
    terminal_get_dimensions(ctx->term, &term_rows, &term_cols);
    if (display_col >= term_cols)
        display_col = term_cols - 1;
    if (display_col < 0)
        display_col = 0;
    if (display_row >= term_rows)
        display_row = term_rows - 1;
    if (display_row < 0)
        display_row = 0;

    int unified_row = display_row_to_unified(ctx->rend, display_row);

    // Left button press — start selection (or defer for char mode)
    if (button == 1 && pressed) {
        if (clicks >= 3) {
            ctx->drag_pending = false;
            terminal_selection_start(ctx->term, unified_row, display_col, TERM_SELECT_LINE);
        } else if (clicks == 2) {
            ctx->drag_pending = false;
            terminal_selection_start(ctx->term, unified_row, display_col, TERM_SELECT_WORD);
        } else if (terminal_selection_active(ctx->term)) {
            ctx->drag_pending = false;
            terminal_selection_clear(ctx->term);
        } else {
            // Defer char selection until drag — don't pause PTY on stray clicks
            ctx->drag_pending = true;
            ctx->drag_start_row = unified_row;
            ctx->drag_start_col = display_col;
        }
        return true;
    }

    // Left button release — cancel pending drag if no motion occurred
    if (button == 1 && !pressed) {
        ctx->drag_pending = false;
        return false;
    }

    // Motion with button held — start or update selection
    if (button == 0 && pressed) {
        if (ctx->drag_pending) {
            // First motion after click — start char selection from saved position
            ctx->drag_pending = false;
            terminal_selection_start(ctx->term, ctx->drag_start_row, ctx->drag_start_col,
                                     TERM_SELECT_CHAR);
            terminal_selection_update(ctx->term, unified_row, display_col);
            return true;
        }
        if (terminal_selection_active(ctx->term)) {
            terminal_selection_update(ctx->term, unified_row, display_col);
            return true;
        }
    }

    // Right button press — copy selection if active, otherwise paste
    if (button == 3 && pressed) {
        if (terminal_selection_active(ctx->term)) {
            char *text = terminal_selection_get_text(ctx->term);
            if (text) {
                platform_clipboard_set(ctx->plat, text);
                free(text);
            }
            terminal_selection_clear(ctx->term);
        } else {
            // Try async paste first (GTK4), fall back to synchronous (SDL3)
            if (!platform_clipboard_paste_async(ctx->plat, ctx->term, ctx->pty)) {
                char *clipboard = platform_clipboard_get(ctx->plat);
                if (clipboard && clipboard[0] != '\0') {
                    terminal_start_paste(ctx->term);
                    pty_write(ctx->pty, clipboard, strlen(clipboard));
                    terminal_end_paste(ctx->term);
                }
                platform_clipboard_free(ctx->plat, clipboard);
            }
        }
        return true;
    }

    return false;
}

int main(int argc, char *argv[])
{
    TerminalBackend *term;
    RendererBackend *rend = NULL;
    PtyContext *pty = NULL;
    PlatformBackend *plat = NULL;
    int running = 1;
    int opt;

    // Parse command line arguments
    int list_fonts = 0;
    int ft_hint_target = FT_LOAD_TARGET_LIGHT; // Default: light hinting
    char *png_text = NULL;
    char *demo_text = NULL;
    const char *font_name = NULL;
    const char *colr_debug_path = NULL;
    char **exec_argv = NULL;
    const float font_size = 12.0f;
    int init_cols = DEFAULT_COLS;
    int init_rows = DEFAULT_ROWS;

    int use_gtk4 = 0;

    static struct option long_options[] = {
        { "list-fonts", no_argument, NULL, 'L' },
        { "ft-hinting", required_argument, NULL, 'H' },
        { "reflow", no_argument, NULL, 'R' },
        { "padding", no_argument, NULL, 'N' },
        { "gtk4", no_argument, NULL, 'G' },
        { "sdl3", no_argument, NULL, 'S' },
        { "demo", required_argument, NULL, 'd' },
        { NULL, 0, NULL, 0 }
    };

    int reflow_enabled = 0;
    int padding = 0;

    /* Load config file (CLI flags below will override) */
    BloomConf conf;
    bloom_conf_init(&conf);
    bloom_conf_load(&conf);

    if (conf.verbose == 1)
        verbose = 1;
    if (conf.font)
        font_name = conf.font;
    if (conf.cols > 0)
        init_cols = conf.cols;
    if (conf.rows > 0)
        init_rows = conf.rows;
    if (conf.hinting != BLOOM_HINT_UNSET) {
        static const int hint_map[] = { FT_LOAD_NO_HINTING, FT_LOAD_TARGET_LIGHT,
                                        FT_LOAD_TARGET_NORMAL, FT_LOAD_TARGET_MONO };
        ft_hint_target = hint_map[conf.hinting];
    }
    if (conf.reflow == 1)
        reflow_enabled = 1;
    if (conf.padding == 1)
        padding = 1;
    if (conf.platform && strcmp(conf.platform, "gtk4") == 0)
        use_gtk4 = 1;

    while ((opt = getopt_long(argc, argv, "hvf:g:P:D:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'v':
            verbose = 1;
            break;
        case 'd':
            demo_text = optarg;
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
        case 'R':
            reflow_enabled = 1;
            break;
        case 'N':
            padding = 1;
            break;
        case 'G':
            use_gtk4 = 1;
            break;
        case 'S':
            use_gtk4 = 0;
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
        FontResolveBackend *resolve = font_resolve_init(&font_resolve_backend_fc);
        if (!resolve) {
            fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
            return 1;
        }
        font_resolve_list_monospace(resolve);
        font_resolve_destroy(resolve);
        return 0;
    }

    // Set COLR layer debug prefix if specified
    if (colr_debug_path) {
        colr_set_debug_prefix(colr_debug_path);
        vlog("COLR layer debug enabled, prefix: %s\n", colr_debug_path);
    }

    // PNG render mode: skip interactive mode, render text to PNG and exit
    if (png_text) {
        if (optind >= argc) {
            fprintf(stderr, "ERROR: -P requires output PNG path as positional argument\n");
            fprintf(stderr, "Usage: %s -P \"text\" output.png\n", argv[0]);
            return 1;
        }
        return png_render_text(png_text, argv[optind], font_name, ft_hint_target);
    }

    // Select and initialize platform backend
    PlatformBackend *selected_backend = &platform_backend_sdl3;
    void *gtk4_plugin_handle = NULL;

    if (use_gtk4) {
        // Probe for the GTK4 plugin shared object
        static const char *plugin_name = "bloom-terminal-gtk4.so";
        char probe_path[PATH_MAX];
        const char *base = SDL_GetBasePath();
        const char *try_paths[] = { NULL, NULL, NULL };
        int n_paths = 0;

        // Build tree: exe is build/src/bloom-terminal, plugin is build/src/.libs/
        if (base) {
            snprintf(probe_path, sizeof(probe_path), "%s.libs/%s", base, plugin_name);
            try_paths[n_paths++] = probe_path;
        }

        // Installed: $PREFIX/lib/bloom-terminal/
        char installed_path[PATH_MAX];
        if (base) {
            snprintf(installed_path, sizeof(installed_path),
                     "%s../lib/bloom-terminal/%s", base, plugin_name);
            try_paths[n_paths++] = installed_path;
        }

#ifdef PKGLIBDIR
        // Compile-time pkglibdir fallback
        char pkglib_path[PATH_MAX];
        snprintf(pkglib_path, sizeof(pkglib_path), "%s/%s", PKGLIBDIR, plugin_name);
        try_paths[n_paths++] = pkglib_path;
#endif

        const char *loaded_path = NULL;
        for (int i = 0; i < n_paths && !gtk4_plugin_handle; i++) {
            vlog("Probing GTK4 plugin: %s\n", try_paths[i]);
            gtk4_plugin_handle = dlopen(try_paths[i], RTLD_NOW);
            if (gtk4_plugin_handle)
                loaded_path = try_paths[i];
        }

        if (!gtk4_plugin_handle) {
            fprintf(stderr, "ERROR: --gtk4 requested but plugin not found\n");
            fprintf(stderr, "  %s\n", dlerror());
            return 1;
        }

        bloom_platform_gtk4_get_fn get_backend =
            (bloom_platform_gtk4_get_fn)dlsym(gtk4_plugin_handle, "bloom_platform_gtk4_get");
        if (!get_backend) {
            fprintf(stderr, "ERROR: GTK4 plugin missing bloom_platform_gtk4_get: %s\n",
                    dlerror());
            dlclose(gtk4_plugin_handle);
            return 1;
        }

        selected_backend = get_backend();
        vlog("Loaded GTK4 plugin from %s\n", loaded_path);
    }

    plat = platform_init(selected_backend);
    if (!plat) {
        fprintf(stderr, "ERROR: Failed to initialize platform\n");
        if (gtk4_plugin_handle)
            dlclose(gtk4_plugin_handle);
        return 1;
    }

    // FreeType is initialized in renderer_init, not here
    vlog("FreeType will be initialized in renderer\n");

    // Initialize terminal with cell dimensions (cols x rows)
    term = terminal_init(&terminal_backend_vt, init_cols, init_rows);
    if (!term) {
        fprintf(stderr, "Failed to initialize terminal\n");
        platform_destroy(plat);
        return 1;
    }

    // Enable reflow if requested
    if (reflow_enabled) {
        terminal_set_reflow(term, true);
        vlog("Text reflow enabled (UNSTABLE: may crash on extreme resize)\n");
    }

    if (conf.word_chars)
        terminal_selection_set_word_chars(term, conf.word_chars);

    // Only create window and renderer if we're going to run the event loop
    if (running) {
        // Create window (placeholder size; will be resized after font loading)
        if (!platform_create_window(plat, "bloom-terminal", 800, 600)) {
            terminal_destroy(term);
            platform_destroy(plat);
            return 1;
        }

        // Initialize renderer using SDL handles from platform
        rend = renderer_init(&renderer_backend_sdl3,
                             platform_get_sdl_window(plat),
                             platform_get_sdl_renderer(plat));
        if (!rend) {
            fprintf(stderr, "Failed to initialize renderer\n");
            terminal_destroy(term);
            platform_destroy(plat);
            return 1;
        }

        // Query desktop environment for preferred monospace font
        char *desktop_font = NULL;
        if (!font_name) {
            desktop_font = platform_get_default_font(plat);
            if (desktop_font)
                font_name = desktop_font;
        }

        // Set content scale before font loading so FreeType uses correct DPI
        float display_scale = platform_get_display_scale(plat);
        if (display_scale > 0.0f)
            renderer_set_content_scale(rend, display_scale);

        // Load fonts
        if (renderer_load_fonts(rend, font_size, font_name, ft_hint_target) < 0) {
            fprintf(stderr, "Failed to load fonts\n");
            free(desktop_font);
            renderer_destroy(rend);
            terminal_destroy(term);
            platform_destroy(plat);
            return 1;
        }
        free(desktop_font);

        // Disable padding unless --padding is passed
        if (!padding) {
            renderer_set_padding(rend, 0, 0, 0, 0);
        }

        // Derive window size from font cell dimensions plus padding
        int cell_w, cell_h;
        int pad_l = 0, pad_t = 0, pad_r = 0, pad_b = 0;
        int win_w = 800, win_h = 600;
        if (renderer_get_cell_size(rend, &cell_w, &cell_h)) {
            renderer_get_padding(rend, &pad_l, &pad_t, &pad_r, &pad_b);
            win_w = init_cols * cell_w + pad_l + pad_r;
            win_h = init_rows * cell_h + pad_t + pad_b;
            vlog("Derived window size from font: %dx%d (%d cols * %d px + %d pad, %d rows * %d px + %d pad)\n",
                 win_w, win_h, init_cols, cell_w, pad_l + pad_r, init_rows, cell_h, pad_t + pad_b);
        }
        platform_set_window_size(plat, win_w, win_h);
        renderer_resize(rend, win_w, win_h);
        platform_show_window(plat);

        if (demo_text) {
            // Demo mode: feed text directly into terminal, no PTY needed
            terminal_process_input(term, demo_text, strlen(demo_text));
            vlog("Demo mode: fed %zu bytes into terminal\n", strlen(demo_text));
        } else {
            // Initialize signal handling before creating PTY
            if (pty_signal_init() < 0) {
                fprintf(stderr, "WARNING: Failed to initialize SIGCHLD handling\n");
            }

            // Create PTY and spawn shell (or custom command)
            pty = pty_create(init_rows, init_cols, exec_argv);
            if (!pty) {
                fprintf(stderr, "ERROR: Failed to create PTY\n");
                pty_signal_cleanup();
                renderer_destroy(rend);
                terminal_destroy(term);
                platform_destroy(plat);
                return 1;
            }

            // Register PTY with platform
            if (!platform_register_pty(plat, pty)) {
                fprintf(stderr, "ERROR: Failed to register PTY with platform\n");
                pty_destroy(pty);
                pty_signal_cleanup();
                renderer_destroy(rend);
                terminal_destroy(term);
                platform_destroy(plat);
                return 1;
            }
        }
    }

    // Only enter event loop if running
    if (running) {
        MainContext main_ctx = {
            .term = term,
            .rend = rend,
            .pty = pty,
            .plat = plat,
        };

        PlatformCallbacks callbacks = {
            .on_key = on_key,
            .on_text = on_text,
            .on_resize = on_resize,
            .on_scroll = on_scroll,
            .on_mouse = on_mouse,
            .user_data = &main_ctx,
        };

        // Connect terminal output to PTY (for mouse escape sequences)
        terminal_set_output_callback(term, term_output_to_pty, pty);

        // Pause/resume PTY on selection changes in alt screen
        terminal_set_selection_callback(term, on_selection_change, &main_ctx);

        // Run the event loop (blocks)
        platform_run(plat, term, rend, &callbacks);
    }

    // Cleanup
    if (pty)
        pty_destroy(pty);
    pty_signal_cleanup();
    if (rend)
        renderer_destroy(rend);
    terminal_destroy(term);
    bloom_conf_free(&conf);
    platform_destroy(plat);
    if (gtk4_plugin_handle)
        dlclose(gtk4_plugin_handle);

    return 0;
}

void vlog_impl(const char *file, const char *func, int line, const char *format, ...)
{
    if (!verbose)
        return;

    // Get current time with milliseconds
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    // Extract basename from file path
    const char *basename = strrchr(file, '/');
    basename = basename ? basename + 1 : file;

    va_list args;
    va_start(args, format);
    fprintf(stderr, "DEBUG [%02d:%02d:%02d.%03ld] %s:%d %s(): ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
            basename, line, func);
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
    printf("  -f PATTERN  Font (fontconfig pattern, default: monospace)\n");
    printf("              Size is part of the pattern, e.g. -f monospace-16\n");
    printf("              Examples: -f \"Cascadia Code-14\", -f monospace-24\n");
    printf("  -g COLSxROWS  Initial terminal size (default: 80x24)\n");
    printf("  --ft-hinting S  Set FreeType hinting: none, light, normal, mono (default: light)\n");
    printf("  --list-fonts  List available monospace fonts and exit\n");
    printf("  --padding     Enable padding around terminal content\n");
    printf("  --gtk4        Use GTK4/libadwaita platform backend (native CSD)\n");
    printf("  --sdl3        Use SDL3 platform backend (overrides config file)\n");
    printf("  --reflow    Enable text reflow on resize (UNSTABLE: may crash on extreme\n");
    printf("              window sizes due to libvterm bug, see github.com/neovim/neovim/issues/25234)\n");
    printf("  --demo TEXT Display TEXT in terminal without spawning a shell (for testing)\n");
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
}
