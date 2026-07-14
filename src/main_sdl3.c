#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_version.h"

#include "backend_sdl3.h"
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

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24

/* Global verbose flag - controls debug output */
int verbose = 0;
float portty_text_gamma = 1.0f;
float portty_text_contrast = 0.0f;
bool portty_notification_transparent = false;

/* ASan/UBSan runtime defaults. */
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
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
}

static void print_version(void)
{
    printf("portty %s\n", PORTTY_VERSION);
}

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

    int opt;
    int list_fonts = 0;
    int ft_hint_target = FT_LOAD_TARGET_LIGHT;
    char *demo_text = NULL;
    const char *font_name = NULL;
    int font_from_flag = 0;
    const char *font_source = NULL;
    const char *colr_debug_path = NULL;
    char **exec_argv = NULL;
    const float font_size = 12.0f;
    int init_cols = DEFAULT_COLS;
    int init_rows = DEFAULT_ROWS;
    int init_scrollback = -1;
    const char *script_path = NULL;

    static struct option long_options[] = {
        { "help", no_argument, NULL, 'h' },
        { "version", no_argument, NULL, 'V' },
        { "list-fonts", no_argument, NULL, 'L' },
        { "ft-hinting", required_argument, NULL, 'H' },
        { "demo", required_argument, NULL, 'd' },
        { "scrollback", required_argument, NULL, 's' },
        { "script", required_argument, NULL, 'S' },
        { NULL, 0, NULL, 0 }
    };

    PorttyConf conf;
    portty_conf_init(&conf);
    portty_conf_load(&conf);

    if (conf.verbose == 1)
        verbose = 1;
    if (conf.font)
        font_name = conf.font;
    if (conf.cols > 0)
        init_cols = conf.cols;
    if (conf.rows > 0)
        init_rows = conf.rows;
    if (conf.hinting != PORTTY_HINT_UNSET) {
        static const int hint_map[] = { FT_LOAD_NO_HINTING, FT_LOAD_TARGET_LIGHT,
                                        FT_LOAD_TARGET_NORMAL, FT_LOAD_TARGET_MONO };
        ft_hint_target = hint_map[conf.hinting];
    }
    if (conf.scrollback >= 0)
        init_scrollback = conf.scrollback;
    if (conf.text_gamma > 0.0f)
        portty_text_gamma = conf.text_gamma;
    if (conf.text_contrast >= 0.0f)
        portty_text_contrast = conf.text_contrast;
    if (conf.notification_transparency == 1)
        portty_notification_transparent = true;

    while ((opt = getopt_long(argc, argv, "hvVf:g:D:s:S:", long_options, NULL)) != -1) {
        switch (opt) {
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
            demo_text = optarg;
            break;
        case 'S':
            script_path = optarg;
            break;
        case 'f':
            font_name = optarg;
            font_from_flag = 1;
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
                fprintf(stderr, "ERROR: Invalid hinting target: %s\n", optarg);
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
                fprintf(stderr, "ERROR: Invalid geometry: %s\n", optarg);
                return 1;
            }
            break;
        }
        case 'D':
            colr_debug_path = optarg;
            break;
        case 's':
        {
            char *end = NULL;
            long n = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n < 0 || n > INT_MAX) {
                fprintf(stderr, "ERROR: Invalid scrollback: %s\n", optarg);
                return 1;
            }
            init_scrollback = (int)n;
            break;
        }
        case '?':
            print_usage(argv[0]);
            return 1;
        }
    }

    if (optind < argc) {
        exec_argv = &argv[optind];
    } else if (conf.shell) {
        char *shell_copy = strdup(conf.shell);
        if (!shell_copy) {
            fprintf(stderr, "ERROR: Out of memory\n");
            return 1;
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
            return 1;
        }
        for (int i = 0; i < ntok; i++)
            shell_argv[i] = strdup(tokens[i]);
        shell_argv[ntok] = NULL;
        free(shell_copy);
        exec_argv = shell_argv;
    }

    if (list_fonts) {
        FontResolveBackend *resolve = font_resolve_init(&FONT_RESOLVE_BACKEND);
        if (!resolve) {
            fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
            return 1;
        }
        font_resolve_list_monospace(resolve);
        font_resolve_destroy(resolve);
        return 0;
    }

    if (colr_debug_path) {
        colr_set_debug_prefix(colr_debug_path);
        vlog("COLR layer debug enabled, prefix: %s\n", colr_debug_path);
    }

    TerminalBackend *vt_backend = &terminal_backend_cfr;
    CfrConfig cfg = CFR_CONFIG_DEFAULTS;
    cfg.rows = init_rows;
    cfg.cols = init_cols;
    cfg.cell_w_px = 10;
    cfg.cell_h_px = 20;
    cfg.reflow = true;
    TerminalBackend *term = terminal_init(vt_backend, &cfg);
    if (!term) {
        fprintf(stderr, "Failed to initialize terminal\n");
        return 1;
    }

    if (conf.word_chars)
        terminal_selection_set_word_chars(term, conf.word_chars);
    if (init_scrollback >= 0)
        terminal_set_scrollback_size(term, init_scrollback);

    PorttyBackend backend = backend_sdl3;
    PorttyApp app = {
        .term = term,
        .conf = &conf,
        .font_source = font_source,
        .backend = &backend,
        .demo_text = demo_text,
        .exec_argv = exec_argv,
        .font_size = font_size,
        .script_path = script_path,
    };
    backend.data = &app;

    if (!backend.init(&backend, &app, "portty", 800, 600)) {
        fprintf(stderr, "ERROR: Failed to initialize SDL3 backend\n");
        terminal_destroy(term);
        return 1;
    }

    pty_signal_init();

    char *desktop_font = NULL;
    if (!font_name) {
        desktop_font = backend.get_default_font(&backend);
        if (desktop_font)
            font_name = desktop_font;
    }
    if (font_from_flag)
        font_source = "-f flag";
    else if (conf.font)
        font_source = "config file";
    else if (desktop_font)
        font_source = "desktop default";
    else
#ifdef _WIN32
        font_source = "system default (no console font set)";
#else
        font_source = "fontconfig generic (no desktop default)";
#endif
    app.font_source = font_source;

    float display_scale = backend.get_display_scale(&backend);
    if (display_scale > 0.0f)
        backend.set_content_scale(&backend, display_scale);

    if (backend.load_fonts(&backend, font_size, font_name, ft_hint_target) < 0) {
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
        win_w = init_cols * cell_w;
        win_h = init_rows * cell_h;
        vlog("Derived window size from font: %dx%d\n", win_w, win_h);

        int disp_w, disp_h;
        if (backend.get_display_size(&backend, &disp_w, &disp_h)) {
            if (win_w > disp_w || win_h > disp_h) {
                if (win_w > disp_w)
                    win_w = disp_w;
                if (win_h > disp_h)
                    win_h = disp_h;
                init_cols = win_w / cell_w;
                init_rows = win_h / cell_h;
                if (init_cols < 1)
                    init_cols = 1;
                if (init_rows < 1)
                    init_rows = 1;
                win_w = init_cols * cell_w;
                win_h = init_rows * cell_h;
                terminal_resize(term, init_cols, init_rows);
            }
        }
    }
    backend.set_window_size(&backend, win_w, win_h);
    backend.resize(&backend, win_w, win_h);
    if (backend.show_window)
        backend.show_window(&backend);

    PtyContext *pty = NULL;
    if (demo_text) {
        terminal_process_input(term, demo_text, strlen(demo_text));
    } else {
        pty = pty_create(init_rows, init_cols, exec_argv);
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
    portty_conf_free(&conf);

#ifdef _WIN32
    if (console_attached)
        FreeConsole();
#endif

    return 0;
}
