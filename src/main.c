#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_version.h"

#include "common.h"
#include "font_ft_internal.h"
#include "font_resolve.h"
#include "pager.h"
#include "portty_app.h"
#include "portty_conf.h"
#include "portty_pty.h"
#ifdef _WIN32
#include "font_resolve_w32.h"
#define FONT_RESOLVE_BACKEND font_resolve_backend_w32
#elif defined(__APPLE__)
#include "font_resolve_ct.h"
#define FONT_RESOLVE_BACKEND font_resolve_backend_ct
#else
#include "font_resolve_fc.h"
#define FONT_RESOLVE_BACKEND font_resolve_backend_fc
#endif
#include "term.h"
#include "term_cfr.h"
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(PORTTY_BACKEND_SOKOL)
#include "backend_sokol.h"
#elif defined(PORTTY_BACKEND_SDL3)
#include "backend_sdl3.h"
#else
#error "No backend selected; define PORTTY_BACKEND_SDL3 or PORTTY_BACKEND_SOKOL"
#endif

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

/* Global verbose flag - controls debug output */
int verbose = 0;
float portty_text_gamma = 1.0f;
float portty_text_contrast = 0.0f;
bool portty_notification_transparent = false;

/* ASan/UBSan runtime defaults. */
#if defined(__SANITIZE_ADDRESS__)
#define PORTTY_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PORTTY_HAS_ASAN 1
#endif
#endif
#ifdef PORTTY_HAS_ASAN
const char *__asan_default_options(void)
{
    return "abort_on_error=1:disable_coredump=0:detect_leaks=0:"
           "log_path=/tmp/portty-asan:print_module_map=1";
}
const char *__ubsan_default_options(void)
{
    return "abort_on_error=1:print_stacktrace=1:log_path=/tmp/portty-ubsan";
}
#endif

static void print_usage(const char *progname);
static void print_version(void);

void vlog_impl(const char *file, const char *func, int line, const char *format, ...)
{
    if (!verbose)
        return;

    const char *basename = strrchr(file, '/');
#ifdef _WIN32
    const char *basename2 = strrchr(file, '\\');
    if (!basename || (basename2 && basename2 > basename))
        basename = basename2;
#endif
    basename = basename ? basename + 1 : file;

#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    int hour = st.wHour, min = st.wMinute, sec = st.wSecond;
    long ms = st.wMilliseconds;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    int hour = tm.tm_hour, min = tm.tm_min, sec = tm.tm_sec;
    long ms = ts.tv_nsec / 1000000;
#endif

    va_list args;
    va_start(args, format);
    fprintf(stderr, "[%02d:%02d:%02d.%03ld] %s:%s:%d ", hour, min, sec, ms, basename, func,
            line);
    vfprintf(stderr, format, args);
    va_end(args);
}

static void print_usage(const char *progname)
{
    printf("Usage: %s [options] [--] [command [args...]]\n", progname);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help            Show this help message and exit\n");
    printf("  -V, --version         Show version information and exit\n");
    printf("  -v                    Enable verbose logging\n");
    printf("  -f FONT               Set font family or file path\n");
    printf("  -g COLSxROWS          Set initial geometry (e.g. 120x40)\n");
    printf("  -L, --list-fonts      List available monospace fonts and exit\n");
    printf("  -H none|light|normal|mono  Set FreeType hinting target\n");
    printf("  -s LINES              Set scrollback buffer size in lines\n");
    printf("  -d TEXT               Demo mode: feed TEXT into the terminal\n");
    printf("  -S, --script FILE     Run debug script FILE (see docs/debug-infrastructure-design.md)\n");
    printf("  --dpi-scale SCALE     Multiply detected DPI scale (default: 1.0)\n");
}

static void print_version(void)
{
    printf("portty %s\n", PORTTY_VERSION);
}

typedef struct
{
    int opt;
    int list_fonts;
    int ft_hint_target;
    char *demo_text;
    const char *font_name;
    int font_from_flag;
    const char *font_source;
    const char *colr_debug_path;
    char **exec_argv;
    const float font_size;
    int init_cols;
    int init_rows;
    int init_scrollback;
    const char *script_path;
    float dpi_scale;
} PorttyArgs;

static void portty_args_init(PorttyArgs *args)
{
    memset(args, 0, sizeof(*args));
    args->ft_hint_target = FT_LOAD_TARGET_LIGHT;
    *(float *)&args->font_size = 12.0f;
    args->init_cols = DEFAULT_COLS;
    args->init_rows = DEFAULT_ROWS;
    args->init_scrollback = -1;
    args->dpi_scale = 1.0f;
}

/* Helper for --dpi-scale parsing (shared with tests) */
int portty_parse_dpi_scale(const char *arg, float *out)
{
    if (!arg || !out)
        return -1;
    char *end = NULL;
    float scale = strtof(arg, &end);
    if (end == arg || *end != '\0' || scale <= 0.0f)
        return -1;
    *out = scale;
    return 0;
}

static int parse_args(PorttyArgs *args, int argc, char *argv[])
{
    static struct option long_options[] = {
        { "help", no_argument, NULL, 'h' },
        { "version", no_argument, NULL, 'V' },
        { "list-fonts", no_argument, NULL, 'L' },
        { "ft-hinting", required_argument, NULL, 'H' },
        { "demo", required_argument, NULL, 'd' },
        { "scrollback", required_argument, NULL, 's' },
        { "script", required_argument, NULL, 'S' },
        { "dpi-scale", required_argument, NULL, 'D' },
        { NULL, 0, NULL, 0 }
    };

    while ((args->opt = getopt_long(argc, argv, "hvVf:g:Ld:H:s:S:D:", long_options, NULL)) != -1) {
        switch (args->opt) {
        case 'h':
            print_usage(argv[0]);
            return 0;
        case 'V':
            print_version();
            return 0;
        case 'v':
            verbose = 1;
            break;
        case 'd':
            args->demo_text = optarg;
            break;
        case 'S':
            args->script_path = optarg;
            break;
        case 'f':
            args->font_name = optarg;
            args->font_from_flag = 1;
            break;
        case 'L':
            args->list_fonts = 1;
            break;
        case 'H':
            if (strcmp(optarg, "none") == 0) {
                args->ft_hint_target = FT_LOAD_NO_HINTING;
            } else if (strcmp(optarg, "light") == 0) {
                args->ft_hint_target = FT_LOAD_TARGET_LIGHT;
            } else if (strcmp(optarg, "normal") == 0) {
                args->ft_hint_target = FT_LOAD_TARGET_NORMAL;
            } else if (strcmp(optarg, "mono") == 0) {
                args->ft_hint_target = FT_LOAD_TARGET_MONO;
            } else {
                fprintf(stderr, "ERROR: Invalid hinting target: %s\n", optarg);
                return 1;
            }
            break;
        case 'g':
        {
            int w = 0, h = 0;
            if (sscanf(optarg, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                args->init_cols = w;
                args->init_rows = h;
            } else {
                fprintf(stderr, "ERROR: Invalid geometry: %s\n", optarg);
                return 1;
            }
            break;
        }
        case 'D':
        {
            char *end = NULL;
            float scale = strtof(optarg, &end);
            if (end == optarg || *end != '\0' || scale <= 0.0f) {
                fprintf(stderr, "ERROR: Invalid DPI scale: %s (must be > 0)\n", optarg);
                return 1;
            }
            args->dpi_scale = scale;
            break;
        }
        case 's':
        {
            char *end = NULL;
            long n = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n < 0 || n > INT_MAX) {
                fprintf(stderr, "ERROR: Invalid scrollback: %s\n", optarg);
                return 1;
            }
            args->init_scrollback = (int)n;
            break;
        }
        case '?':
            print_usage(argv[0]);
            return 1;
        }
    }

    return -1;
}

static void resolve_exec_argv(PorttyArgs *args, PorttyConf *conf, int argc, char *argv[])
{
    if (optind < argc) {
        args->exec_argv = &argv[optind];
    } else if (conf->shell) {
        char *shell_copy = strdup(conf->shell);
        if (!shell_copy) {
            fprintf(stderr, "ERROR: Out of memory\n");
            exit(1);
        }
        char *tokens[64];
        int ntok = 0;
        char *tok = strtok(shell_copy, " \t");
        while (tok && ntok < 63) {
            tokens[ntok++] = tok;
            tok = strtok(NULL, " \t");
        }
        tokens[ntok] = NULL;

        char **shell_argv = calloc(ntok + 1, sizeof(char *));
        if (!shell_argv) {
            free(shell_copy);
            fprintf(stderr, "ERROR: Out of memory\n");
            exit(1);
        }
        for (int i = 0; i < ntok; i++)
            shell_argv[i] = strdup(tokens[i]);
        shell_argv[ntok] = NULL;
        free(shell_copy);
        args->exec_argv = shell_argv;
    }
}

static void apply_conf_to_args(PorttyArgs *args, PorttyConf *conf)
{
    if (conf->verbose == 1)
        verbose = 1;
    if (conf->font && !args->font_name)
        args->font_name = conf->font;
    if (conf->cols > 0)
        args->init_cols = conf->cols;
    if (conf->rows > 0)
        args->init_rows = conf->rows;
    if (conf->hinting != PORTTY_HINT_UNSET) {
        static const int hint_map[] = { FT_LOAD_NO_HINTING, FT_LOAD_TARGET_LIGHT,
                                        FT_LOAD_TARGET_NORMAL, FT_LOAD_TARGET_MONO };
        args->ft_hint_target = hint_map[conf->hinting];
    }
    if (conf->scrollback >= 0)
        args->init_scrollback = conf->scrollback;
    if (conf->text_gamma > 0.0f)
        portty_text_gamma = conf->text_gamma;
    if (conf->text_contrast >= 0.0f)
        portty_text_contrast = conf->text_contrast;
    if (conf->notification_transparency == 1)
        portty_notification_transparent = true;
}

static TerminalBackend *create_terminal(PorttyArgs *args)
{
    TerminalBackend *vt_backend = &terminal_backend_cfr;
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = args->init_rows;
    cfg.cols = args->init_cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    cfg.reflow = true;
    TerminalBackend *term = terminal_init(vt_backend, &cfg);
    if (!term) {
        fprintf(stderr, "Failed to initialize terminal\n");
        return NULL;
    }
    return term;
}

#if defined(PORTTY_BACKEND_SDL3)
static int portty_run_sdl3(PorttyArgs *args, PorttyConf *conf)
{
    TerminalBackend *term = create_terminal(args);
    if (!term)
        return 1;

    if (conf->word_chars)
        terminal_selection_set_word_chars(term, conf->word_chars);
    if (args->init_scrollback >= 0)
        terminal_set_scrollback_size(term, args->init_scrollback);

    PorttyBackend backend = backend_sdl3;
    PorttyApp app = {
        .term = term,
        .conf = conf,
        .font_source = args->font_source,
        .backend = &backend,
        .demo_text = args->demo_text,
        .exec_argv = args->exec_argv,
        .font_size = args->font_size,
        .font_name = args->font_name,
        .script_path = args->script_path,
        .dpi_scale = args->dpi_scale,
    };
    backend.data = &app;

    if (!backend.init(&backend, &app, "portty", 800, 600)) {
        fprintf(stderr, "ERROR: Failed to initialize SDL3 backend\n");
        terminal_destroy(term);
        return 1;
    }

    pty_signal_init();

    char *desktop_font = NULL;
    if (!args->font_name) {
        desktop_font = backend.get_default_font(&backend);
        if (desktop_font)
            args->font_name = desktop_font;
    }
    if (args->font_from_flag)
        args->font_source = "-f flag";
    else if (conf->font)
        args->font_source = "config file";
    else if (desktop_font)
        args->font_source = "desktop default";
    else
#ifdef _WIN32
        args->font_source = "system default (no console font set)";
#else
        args->font_source = "fontconfig generic (no desktop default)";
#endif
    app.font_source = args->font_source;

    float display_scale = backend.get_display_scale(&backend);
    if (args->dpi_scale != 1.0f && display_scale > 0.0f)
        display_scale *= args->dpi_scale;
    if (display_scale > 0.0f)
        backend.set_content_scale(&backend, display_scale);

    if (backend.load_fonts(&backend, args->font_size, args->font_name, args->ft_hint_target) < 0) {
        fprintf(stderr, "Failed to load fonts\n");
        free(desktop_font);
        backend.destroy(&backend);
        terminal_destroy(term);
        return 1;
    }
    free(desktop_font);

    int cell_w, cell_h;
    int win_w = 800, win_h = 600;
    if (backend.get_cell_size(&backend, &cell_w, &cell_h)) {
        terminal_set_cell_px(term, cell_w, cell_h);
        terminal_set_content_scale(term, backend.get_display_scale(&backend));
        win_w = args->init_cols * cell_w;
        win_h = args->init_rows * cell_h;
        vlog("Derived window size from font: %dx%d\n", win_w, win_h);

        int disp_w, disp_h;
        if (backend.get_display_size(&backend, &disp_w, &disp_h)) {
            if (win_w > disp_w || win_h > disp_h) {
                if (win_w > disp_w)
                    win_w = disp_w;
                if (win_h > disp_h)
                    win_h = disp_h;
                args->init_cols = win_w / cell_w;
                args->init_rows = win_h / cell_h;
                if (args->init_cols < 1)
                    args->init_cols = 1;
                if (args->init_rows < 1)
                    args->init_rows = 1;
                win_w = args->init_cols * cell_w;
                win_h = args->init_rows * cell_h;
                terminal_resize(term, args->init_cols, args->init_rows);
            }
        }
    }
    backend.set_window_size(&backend, win_w, win_h);
    // Note: SDL3 backend's set_window_size now handles resize internally
    // by querying SDL_GetWindowSizeInPixels for proper high-DPI support
    if (backend.show_window)
        backend.show_window(&backend);

    PtyContext *pty = NULL;
    if (args->demo_text) {
        terminal_process_input(term, args->demo_text, strlen(args->demo_text));
    } else {
        pty = pty_create(args->init_rows, args->init_cols, args->exec_argv);
        if (!pty) {
            fprintf(stderr, "ERROR: Failed to create PTY\n");
            backend.destroy(&backend);
            terminal_destroy(term);
            return 1;
        }
        app.pty = pty;
        if (!backend.register_pty(&backend, pty)) {
            fprintf(stderr, "ERROR: Failed to register PTY with backend\n");
            pty_destroy(pty);
            backend.destroy(&backend);
            terminal_destroy(term);
            return 1;
        }
    }

    term->exe_path = backend.get_exe_path(&backend);

    terminal_set_output_callback(term, portty_app_term_output_to_pty, &app);
    terminal_set_selection_callback(term, portty_app_selection_change, &app);
    terminal_set_clipboard_set_callback(term, portty_app_clipboard_set, &app);
    terminal_set_cwd_callback(term, portty_app_cwd_change, &app);

    app.pager = pager_create(&backend, &app);

    backend.run(&backend);

    pager_destroy(app.pager);
    if (pty)
        pty_destroy(pty);
    pty_signal_cleanup();
    backend.destroy(&backend);
    terminal_destroy(term);

    return 0;
}
#endif

#if defined(PORTTY_BACKEND_SOKOL)
static sapp_desc portty_run_sokol(PorttyArgs *args, PorttyConf *conf)
{
    TerminalBackend *term = create_terminal(args);
    if (!term) {
        return (sapp_desc){ 0 };
    }

    if (conf->word_chars)
        terminal_selection_set_word_chars(term, conf->word_chars);
    if (args->init_scrollback >= 0)
        terminal_set_scrollback_size(term, args->init_scrollback);

    PorttyBackend *backend = calloc(1, sizeof(*backend));
    if (!backend) {
        fprintf(stderr, "ERROR: Out of memory\n");
        terminal_destroy(term);
        return (sapp_desc){ 0 };
    }
    *backend = backend_sokol;

    PorttyApp *app = calloc(1, sizeof(*app));
    if (!app) {
        fprintf(stderr, "ERROR: Out of memory\n");
        free(backend);
        terminal_destroy(term);
        return (sapp_desc){ 0 };
    }
    app->term = term;
    app->conf = conf;
    app->backend = backend;
    app->demo_text = args->demo_text;
    app->exec_argv = args->exec_argv;
    app->font_size = args->font_size;
    app->font_name = args->font_name;
    app->script_path = args->script_path;
    app->dpi_scale = args->dpi_scale;
    backend->data = app;

    return backend_sokol_desc(app, backend, "portty", 800, 600);
}
#endif

#if defined(PORTTY_BACKEND_SDL3)
int main(int argc, char *argv[])
{
#ifdef _WIN32
    bool console_attached = false;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        fflush(stdout);
        fflush(stderr);
        console_attached = true;
    }
#endif

    PorttyConf conf;
    portty_conf_init(&conf);
    portty_conf_load(&conf);

    PorttyArgs args;
    portty_args_init(&args);
    apply_conf_to_args(&args, &conf);

    int ret = parse_args(&args, argc, argv);
    if (ret >= 0) {
        portty_conf_free(&conf);
#ifdef _WIN32
        if (console_attached)
            FreeConsole();
#endif
        return ret;
    }

    resolve_exec_argv(&args, &conf, argc, argv);

    if (args.list_fonts) {
        FontResolveBackend *resolve = font_resolve_init(&FONT_RESOLVE_BACKEND);
        if (!resolve) {
            fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
            portty_conf_free(&conf);
            return 1;
        }
        font_resolve_list_monospace(resolve);
        font_resolve_destroy(resolve);
        portty_conf_free(&conf);
        return 0;
    }

    if (args.colr_debug_path) {
        colr_set_debug_prefix(args.colr_debug_path);
        vlog("COLR layer debug enabled, prefix: %s\n", args.colr_debug_path);
    }

    ret = portty_run_sdl3(&args, &conf);

    portty_conf_free(&conf);

#ifdef _WIN32
    if (console_attached)
        FreeConsole();
#endif

    return ret;
}
#endif

#if defined(PORTTY_BACKEND_SOKOL)
sapp_desc sokol_main(int argc, char *argv[])
{
    PorttyConf *conf = malloc(sizeof(PorttyConf));
    if (!conf) {
        fprintf(stderr, "ERROR: Out of memory\n");
        return (sapp_desc){ 0 };
    }
    portty_conf_init(conf);
    portty_conf_load(conf);

    PorttyArgs args;
    portty_args_init(&args);
    apply_conf_to_args(&args, conf);

    int ret = parse_args(&args, argc, argv);
    if (ret >= 0) {
        portty_conf_free(conf);
        free(conf);
        exit(ret);
    }

    resolve_exec_argv(&args, conf, argc, argv);

    if (args.list_fonts) {
        FontResolveBackend *resolve = font_resolve_init(&FONT_RESOLVE_BACKEND);
        if (!resolve) {
            fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
            portty_conf_free(conf);
            free(conf);
            return (sapp_desc){ 0 };
        }
        font_resolve_list_monospace(resolve);
        font_resolve_destroy(resolve);
        portty_conf_free(conf);
        free(conf);
        return (sapp_desc){ 0 };
    }

    if (args.colr_debug_path) {
        colr_set_debug_prefix(args.colr_debug_path);
        vlog("COLR layer debug enabled, prefix: %s\n", args.colr_debug_path);
    }

    return portty_run_sokol(&args, conf);
}
#endif
