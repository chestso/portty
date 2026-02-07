#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bloom_pty.h"
#include "common.h"
#include "event_loop.h"
#include "event_loop_sdl3.h"
#include "font_ft_internal.h"
#include "font_resolve.h"
#include "font_resolve_fc.h"
#include "png_reader.h"
#include "rend.h"
#include "rend_sdl3.h"
#include "term.h"
#include "term_vt.h"
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdarg.h>
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
static int png_render_text(const char *text, const char *output_path,
                           const char *font_name, int ft_hint_target);

// Find the icon file by probing several paths relative to the executable
static const char *find_icon_path(void)
{
    static char path[PATH_MAX];
    const char *base = SDL_GetBasePath();
    const char *icon_rel = "icons/hicolor/256x256/apps/bloom-terminal.png";

    struct
    {
        const char *fmt;
    } probes[] = {
        /* Dev build: exe is build/src/bloom-terminal, data is at project root */
        { "%s../../data/%s" },
        /* Installed: exe is $PREFIX/bin/, data is $PREFIX/share/ */
        { "%s../share/%s" },
    };

    if (base) {
        for (int i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
            snprintf(path, sizeof(path), probes[i].fmt, base, icon_rel);
            if (access(path, R_OK) == 0) {
                vlog("Found icon at %s\n", path);
                return path;
            }
        }
    }

#ifdef DATADIR
    /* Autotools compile-time datadir */
    snprintf(path, sizeof(path), "%s/%s", DATADIR, icon_rel);
    if (access(path, R_OK) == 0) {
        vlog("Found icon at %s (DATADIR)\n", path);
        return path;
    }
#endif

    /* CWD fallback */
    snprintf(path, sizeof(path), "data/%s", icon_rel);
    if (access(path, R_OK) == 0) {
        vlog("Found icon at %s (CWD)\n", path);
        return path;
    }

    return NULL;
}

// Load and set the window icon from a PNG file
static void set_window_icon(SDL_Window *win)
{
    const char *icon_path = find_icon_path();
    if (!icon_path) {
        vlog("No icon file found, skipping window icon\n");
        return;
    }

    uint8_t *pixels = NULL;
    int w = 0, h = 0;
    if (png_read_rgba(icon_path, &pixels, &w, &h) != 0) {
        fprintf(stderr, "WARNING: Failed to read icon %s\n", icon_path);
        return;
    }

    SDL_Surface *surface =
        SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) {
        fprintf(stderr, "WARNING: Failed to create icon surface: %s\n",
                SDL_GetError());
        free(pixels);
        return;
    }

    if (!SDL_SetWindowIcon(win, surface)) {
        /* Expected on Wayland — icon comes from .desktop + hicolor theme */
        vlog("SDL_SetWindowIcon skipped: %s\n", SDL_GetError());
    } else {
        vlog("Window icon set from %s (%dx%d)\n", icon_path, w, h);
    }

    SDL_DestroySurface(surface);
    free(pixels);
}

// Context passed to event loop callbacks
typedef struct
{
    TerminalBackend *term;
    RendererBackend *rend;
    PtyContext *pty;
} MainContext;

// Convert SDL modifier flags to TERM_MOD_* flags
static int sdl_mod_to_term(int mod)
{
    int m = TERM_MOD_NONE;
    if (mod & SDL_KMOD_SHIFT)
        m |= TERM_MOD_SHIFT;
    if (mod & SDL_KMOD_ALT)
        m |= TERM_MOD_ALT;
    if (mod & SDL_KMOD_CTRL)
        m |= TERM_MOD_CTRL;
    return m;
}

// SDL keycode → terminal key mapping
static const struct
{
    int sdl_key;
    int term_key;
} key_map[] = {
    { SDLK_RETURN, TERM_KEY_ENTER },
    { SDLK_BACKSPACE, TERM_KEY_BACKSPACE },
    { SDLK_ESCAPE, TERM_KEY_ESCAPE },
    { SDLK_TAB, TERM_KEY_TAB },
    { SDLK_UP, TERM_KEY_UP },
    { SDLK_DOWN, TERM_KEY_DOWN },
    { SDLK_RIGHT, TERM_KEY_RIGHT },
    { SDLK_LEFT, TERM_KEY_LEFT },
    { SDLK_HOME, TERM_KEY_HOME },
    { SDLK_END, TERM_KEY_END },
    { SDLK_INSERT, TERM_KEY_INS },
    { SDLK_DELETE, TERM_KEY_DEL },
    { SDLK_PAGEUP, TERM_KEY_PAGEUP },
    { SDLK_PAGEDOWN, TERM_KEY_PAGEDOWN },
    { SDLK_F1, TERM_KEY_F1 },
    { SDLK_F2, TERM_KEY_F2 },
    { SDLK_F3, TERM_KEY_F3 },
    { SDLK_F4, TERM_KEY_F4 },
    { SDLK_F5, TERM_KEY_F5 },
    { SDLK_F6, TERM_KEY_F6 },
    { SDLK_F7, TERM_KEY_F7 },
    { SDLK_F8, TERM_KEY_F8 },
    { SDLK_F9, TERM_KEY_F9 },
    { SDLK_F10, TERM_KEY_F10 },
    { SDLK_F11, TERM_KEY_F11 },
    { SDLK_F12, TERM_KEY_F12 },
    { SDLK_KP_0, TERM_KEY_KP_0 },
    { SDLK_KP_1, TERM_KEY_KP_1 },
    { SDLK_KP_2, TERM_KEY_KP_2 },
    { SDLK_KP_3, TERM_KEY_KP_3 },
    { SDLK_KP_4, TERM_KEY_KP_4 },
    { SDLK_KP_5, TERM_KEY_KP_5 },
    { SDLK_KP_6, TERM_KEY_KP_6 },
    { SDLK_KP_7, TERM_KEY_KP_7 },
    { SDLK_KP_8, TERM_KEY_KP_8 },
    { SDLK_KP_9, TERM_KEY_KP_9 },
    { SDLK_KP_MULTIPLY, TERM_KEY_KP_MULTIPLY },
    { SDLK_KP_PLUS, TERM_KEY_KP_PLUS },
    { SDLK_KP_COMMA, TERM_KEY_KP_COMMA },
    { SDLK_KP_MINUS, TERM_KEY_KP_MINUS },
    { SDLK_KP_PERIOD, TERM_KEY_KP_PERIOD },
    { SDLK_KP_DIVIDE, TERM_KEY_KP_DIVIDE },
    { SDLK_KP_ENTER, TERM_KEY_KP_ENTER },
    { SDLK_KP_EQUALS, TERM_KEY_KP_EQUAL },
};

// Keyboard callback for event loop
static KeyboardResult on_keyboard(void *user_data, int key, int mod, int scancode,
                                  bool is_text, const char *text)
{
    MainContext *ctx = (MainContext *)user_data;
    KeyboardResult result = { 0 };

    // Handle text input for proper Unicode support
    if (is_text && text) {
        // Skip if Ctrl or Alt is held — already handled in KEY_DOWN
        if (SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) {
            return result;
        }
        size_t text_len = strlen(text);
        if (text_len > 0 && text_len < sizeof(result.data)) {
            memcpy(result.data, text, text_len);
            result.len = text_len;
        }
        return result;
    }

    // Application shortcuts (not terminal input)
    if (key == SDLK_C && (mod & SDL_KMOD_CTRL) && (mod & SDL_KMOD_SHIFT)) {
        if (terminal_selection_active(ctx->term)) {
            char *text = terminal_selection_get_text(ctx->term);
            if (text) {
                SDL_SetClipboardText(text);
                free(text);
            }
            terminal_selection_clear(ctx->term);
            result.force_redraw = true;
        }
        result.handled = true;
        return result;
    }
    if (key == SDLK_V && (mod & SDL_KMOD_CTRL) && (mod & SDL_KMOD_SHIFT)) {
        char *clipboard = SDL_GetClipboardText();
        if (clipboard && clipboard[0] != '\0') {
            terminal_start_paste(ctx->term);
            pty_write(ctx->pty, clipboard, strlen(clipboard));
            terminal_end_paste(ctx->term);
        }
        SDL_free(clipboard);
        result.handled = true;
        return result;
    }
    int tmod = sdl_mod_to_term(mod);

    // Shift+PageUp/Down: scrollback navigation (normal screen only)
    if ((mod & SDL_KMOD_SHIFT) && !terminal_is_altscreen(ctx->term)) {
        if (key == SDLK_PAGEUP || key == SDLK_PAGEDOWN) {
            int rows, cols;
            terminal_get_dimensions(ctx->term, &rows, &cols);
            renderer_scroll(ctx->rend, ctx->term, key == SDLK_PAGEUP ? rows : -rows);
            result.force_redraw = true;
            result.handled = true;
            return result;
        }
    }

    // Special keys via lookup table
    for (int i = 0; i < (int)(sizeof(key_map) / sizeof(key_map[0])); i++) {
        if (key_map[i].sdl_key == key) {
            terminal_send_key(ctx->term, key_map[i].term_key, tmod);
            result.handled = true;
            return result;
        }
    }

    // Ctrl or Alt with printable key: resolve shifted character and route through libvterm
    if ((mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT)) && scancode != 0) {
        // Use SDL to resolve the shifted character from the scancode
        SDL_Keycode resolved = SDL_GetKeyFromScancode(scancode, mod & SDL_KMOD_SHIFT, false);
        if (resolved >= 32 && resolved < 127) {
            char ch = (char)resolved;
            // SDL_GetKeyFromScancode returns uppercase for letters; lowercase if Shift not held
            if (ch >= 'A' && ch <= 'Z' && !(mod & SDL_KMOD_SHIFT)) {
                ch = ch - 'A' + 'a';
            }
            // Pass Ctrl and Alt to libvterm but not Shift (already baked into resolved char)
            int tmod_no_shift = tmod & ~TERM_MOD_SHIFT;
            terminal_send_char(ctx->term, (uint32_t)ch, tmod_no_shift);
            result.handled = true;
        }
    }

    return result;
}

// Window resize callback for event loop
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

// Scroll callback for event loop
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
        // In scrollback: unified row is -(scrollback_index + 1)
        return -(scrollback_row + 1);
    } else {
        // Visible terminal: unified row is display_row - scroll_offset
        return display_row - scroll_offset;
    }
}

// Mouse callback for event loop
static bool on_mouse(void *user_data, int pixel_x, int pixel_y, int button, bool pressed,
                     int clicks, int mod)
{
    MainContext *ctx = (MainContext *)user_data;

    int mouse_mode = terminal_get_mouse_mode(ctx->term);
    bool shift_held = (mod & SDL_KMOD_SHIFT) != 0;

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
            return false; // Let scrollback handle wheel events
    }

    // Wheel events not consumed by terminal — don't handle as selection
    if (button == 4 || button == 5)
        return false;

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

    // Left button press — start selection
    if (button == 1 && pressed) {
        if (clicks >= 3) {
            terminal_selection_start(ctx->term, unified_row, display_col, TERM_SELECT_LINE);
        } else if (clicks == 2) {
            terminal_selection_start(ctx->term, unified_row, display_col, TERM_SELECT_WORD);
        } else if (terminal_selection_active(ctx->term)) {
            terminal_selection_clear(ctx->term);
        } else {
            terminal_selection_start(ctx->term, unified_row, display_col, TERM_SELECT_CHAR);
        }
        return true;
    }

    // Motion with button held — drag to update selection
    if (button == 0 && pressed && terminal_selection_active(ctx->term)) {
        terminal_selection_update(ctx->term, unified_row, display_col);
        return true;
    }

    // Right button press — copy selection if active, otherwise paste
    if (button == 3 && pressed) {
        if (terminal_selection_active(ctx->term)) {
            char *text = terminal_selection_get_text(ctx->term);
            if (text) {
                SDL_SetClipboardText(text);
                free(text);
            }
            terminal_selection_clear(ctx->term);
        } else {
            char *clipboard = SDL_GetClipboardText();
            if (clipboard && clipboard[0] != '\0') {
                terminal_start_paste(ctx->term);
                pty_write(ctx->pty, clipboard, strlen(clipboard));
                terminal_end_paste(ctx->term);
            }
            SDL_free(clipboard);
        }
        return true;
    }

    return false;
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
    int list_fonts = 0;
    int ft_hint_target = FT_LOAD_TARGET_LIGHT; // Default: light hinting
    char *png_text = NULL;
    const char *font_name = NULL;
    const char *colr_debug_path = NULL; // COLR layer debug prefix
    char **exec_argv = NULL;            // Command to run (NULL = default shell)
    const float font_size = 12.0f;      // Default font size in points
    int init_cols = DEFAULT_COLS;
    int init_rows = DEFAULT_ROWS;

    static struct option long_options[] = {
        { "list-fonts", no_argument, NULL, 'L' },
        { "ft-hinting", required_argument, NULL, 'H' },
        { "reflow", no_argument, NULL, 'R' },
        { "padding", no_argument, NULL, 'N' },
        { NULL, 0, NULL, 0 }
    };

    int reflow_enabled = 0;
    int padding = 0;

    while ((opt = getopt_long(argc, argv, "hvef:g:P:D:", long_options, NULL)) != -1) {
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
        return png_render_text(png_text, argv[optind], font_name, ft_hint_target);
    }

    // Set app metadata before SDL initialization as recommended by SDL3
    if (verbose) {
        fprintf(stderr, "DEBUG: Setting SDL app metadata\n");
    }
    if (!SDL_SetAppMetadata("bloom-terminal", "1.0.0", "bloom-terminal")) {
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

    // Enable reflow if requested (disabled by default due to libvterm bug)
    if (reflow_enabled) {
        terminal_set_reflow(term, true);
        vlog("Text reflow enabled (UNSTABLE: may crash on extreme resize)\n");
    }

    // Only create window and renderer if we're going to run the event loop
    if (running) {
        // Create window at placeholder size; will be resized after font loading
        vlog("Creating window (placeholder size, will resize after font load)\n");

        // Clear errors before window creation
        SDL_ClearError();

        Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;
        window = SDL_CreateWindow("bloom-terminal", 800, 600, window_flags);
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

        // Set window icon (non-fatal if missing)
        set_window_icon(window);

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
        SDL_SetWindowSize(window, win_w, win_h);
        renderer_resize(rend, win_w, win_h);
        SDL_ShowWindow(window);

        // Initialize signal handling before creating PTY
        if (pty_signal_init() < 0) {
            fprintf(stderr, "WARNING: Failed to initialize SIGCHLD handling\n");
            // Continue without SIGCHLD - will fall back to polling
        }

        // Create PTY and spawn shell (or custom command)
        pty = pty_create(init_rows, init_cols, exec_argv);
        if (!pty) {
            fprintf(stderr, "ERROR: Failed to create PTY\n");
            pty_signal_cleanup();
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
            .on_mouse = on_mouse,
            .user_data = &main_ctx,
        };

        // Connect terminal output to PTY (for mouse escape sequences)
        terminal_set_output_callback(term, term_output_to_pty, pty);

        // Run the event loop
        event_loop_run(event_loop, term, rend, &callbacks);
    }

    // Cleanup
    if (event_loop)
        event_loop_destroy(event_loop);
    if (pty)
        pty_destroy(pty);
    pty_signal_cleanup();
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
    printf("  -e          Exit immediately (for testing)\n");
    printf("  -f PATTERN  Font (fontconfig pattern, default: monospace)\n");
    printf("              Size is part of the pattern, e.g. -f monospace-16\n");
    printf("              Examples: -f \"Cascadia Code-14\", -f monospace-24\n");
    printf("  -g COLSxROWS  Initial terminal size (default: 80x24)\n");
    printf("  --ft-hinting S  Set FreeType hinting: none, light, normal, mono (default: light)\n");
    printf("  --list-fonts  List available monospace fonts and exit\n");
    printf("  --padding     Enable padding around terminal content\n");
    printf("  --reflow    Enable text reflow on resize (UNSTABLE: may crash on extreme\n");
    printf("              window sizes due to libvterm bug, see github.com/neovim/neovim/issues/25234)\n");
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

static int png_render_text(const char *text, const char *output_path,
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
