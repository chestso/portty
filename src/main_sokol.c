#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_version.h"

#include "backend_sokol.h"
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
}

static void print_version(void)
{
    printf("portty %s\n", PORTTY_VERSION);
}

// Sokol's default model uses sokol_main() as the entry point, and
// sokol_app.h hijacks the platform's main().  We provide a single
// sokol_main() that builds the PorttyApp state, calls backend.init(),
// and then relies on sokol callbacks for the frame and event loops.
sapp_desc sokol_main(int argc, char *argv[])
{
    int opt;
    int list_fonts = 0;
    char *demo_text = NULL;
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

    PorttyConf *conf = malloc(sizeof(PorttyConf));
    if (!conf) {
        fprintf(stderr, "ERROR: Out of memory\n");
        return (sapp_desc){ 0 };
    }
    portty_conf_init(conf);
    portty_conf_load(conf);

    if (conf->verbose == 1)
        verbose = 1;
    if (conf->cols > 0)
        init_cols = conf->cols;
    if (conf->rows > 0)
        init_rows = conf->rows;
    if (conf->scrollback >= 0)
        init_scrollback = conf->scrollback;
    if (conf->text_gamma > 0.0f)
        portty_text_gamma = conf->text_gamma;
    if (conf->text_contrast >= 0.0f)
        portty_text_contrast = conf->text_contrast;
    if (conf->notification_transparency == 1)
        portty_notification_transparent = true;

    while ((opt = getopt_long(argc, argv, "hvVf:g:Ld:H:s:S:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            exit(0);
        case 'V':
            print_version();
            exit(0);
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
            conf->font = optarg;
            break;
        case 'L':
            list_fonts = 1;
            break;
        case 'H':
            if (strcmp(optarg, "none") == 0) {
                conf->hinting = PORTTY_HINT_NONE;
            } else if (strcmp(optarg, "light") == 0) {
                conf->hinting = PORTTY_HINT_LIGHT;
            } else if (strcmp(optarg, "normal") == 0) {
                conf->hinting = PORTTY_HINT_NORMAL;
            } else if (strcmp(optarg, "mono") == 0) {
                conf->hinting = PORTTY_HINT_MONO;
            } else {
                fprintf(stderr, "ERROR: Invalid hinting target: %s\n", optarg);
                exit(1);
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
                exit(1);
            }
            break;
        }
        case 's':
        {
            char *end = NULL;
            long n = strtol(optarg, &end, 10);
            if (end == optarg || *end != '\0' || n < 0 || n > INT_MAX) {
                fprintf(stderr, "ERROR: Invalid scrollback: %s\n", optarg);
                exit(1);
            }
            init_scrollback = (int)n;
            break;
        }
        case '?':
            print_usage(argv[0]);
            exit(1);
        }
    }

    if (optind < argc) {
        exec_argv = &argv[optind];
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
        exec_argv = shell_argv;
    }

    if (list_fonts) {
        FontResolveBackend *resolve = font_resolve_init(&FONT_RESOLVE_BACKEND);
        if (!resolve) {
            fprintf(stderr, "ERROR: Failed to initialize font resolver\n");
            exit(1);
        }
        font_resolve_list_monospace(resolve);
        font_resolve_destroy(resolve);
        exit(0);
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
        exit(1);
    }

    if (conf->word_chars)
        terminal_selection_set_word_chars(term, conf->word_chars);
    if (init_scrollback >= 0)
        terminal_set_scrollback_size(term, init_scrollback);

    PorttyBackend *backend = calloc(1, sizeof(*backend));
    if (!backend) {
        fprintf(stderr, "ERROR: Out of memory\n");
        terminal_destroy(term);
        portty_conf_free(conf);
        free(conf);
        exit(1);
    }
    *backend = backend_sokol;

    PorttyApp *app = calloc(1, sizeof(*app));
    if (!app) {
        fprintf(stderr, "ERROR: Out of memory\n");
        free(backend);
        terminal_destroy(term);
        portty_conf_free(conf);
        free(conf);
        exit(1);
    }
    app->term = term;
    app->conf = conf;
    app->backend = backend;
    app->demo_text = demo_text;
    app->exec_argv = exec_argv;
    app->font_size = font_size;
    app->script_path = script_path;
    backend->data = app;

    // Initialization happens in the Sokol init callback so that sokol_app
    // has already created the window and GL context.  We just configure the
    // sapp_desc here and stash the application state in the backend's
    // user-data pointer for the callbacks to use.
    return backend_sokol_desc(app, backend, "portty", 800, 600);
}
