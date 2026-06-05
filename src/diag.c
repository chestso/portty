// Diagnostics report builder. Produces a styled, self-contained UTF-8 document
// (truecolor SGR, box-drawing, bold/underline) for bug reporting — shown through
// the system pager by the Ctrl+Shift+F6 binding in main.c. See diag.h.
//
// Kept free of SDL / terminal / renderer dependencies: all runtime values arrive
// via DiagSources. Only the static build/version block reads config.h macros.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bloom_version.h"

#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/utsname.h>
#endif

#define DW 64 // content width for rules

// ---- styling ---------------------------------------------------------------
#define RST  "\x1b[0m"
#define BOLD "\x1b[1m"
#define DIM  "\x1b[2m"
#define ITAL "\x1b[3m"
#define ULN  "\x1b[4m"
// 24-bit colours (Dracula-ish, matches the default Glamour-dark palette feel)
#define C_TITLE_A 255, 121, 198 // pink
#define C_TITLE_B 139, 233, 253 // cyan
#define C_HEADER  189, 147, 249 // purple
#define C_RULE    68, 71, 90    // muted
#define C_KEY     98, 114, 164  // comment grey-blue
#define C_VAL     248, 248, 242 // foreground
#define C_ON      80, 250, 123  // green
#define C_OFF     255, 85, 85   // red
#define C_ACCENT  241, 250, 140 // yellow

// OSC 8 hyperlink wrappers. The visible text between OPEN and CLOSE is made
// clickable by OSC-8-aware terminals (bloom-terminal's internal pager does
// this); others ignore the wrapper and just show the text. ST is ESC '\'.
// The "\x1b]8" hex escape stops at ']' (not a hex digit), so concatenating a
// URL literal right after is safe.
#define OSC8_OPEN(url) "\x1b]8;;" url "\x1b\\"
#define OSC8_CLOSE     "\x1b]8;;\x1b\\"
#define ISSUES_URL     "https://codeberg.org/thomasc/bloom-terminal/issues"

// ---- growable string buffer ------------------------------------------------
typedef struct
{
    char *buf;
    size_t len, cap;
    bool oom;
} SB;

static void sb_reserve(SB *sb, size_t extra)
{
    if (sb->oom)
        return;
    if (sb->len + extra + 1 <= sb->cap)
        return;
    size_t ncap = sb->cap ? sb->cap * 2 : 1024;
    while (ncap < sb->len + extra + 1)
        ncap *= 2;
    char *n = realloc(sb->buf, ncap);
    if (!n) {
        sb->oom = true;
        return;
    }
    sb->buf = n;
    sb->cap = ncap;
}

static void sb_puts(SB *sb, const char *s)
{
    if (!s)
        return;
    size_t n = strlen(s);
    sb_reserve(sb, n);
    if (sb->oom)
        return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_printf(SB *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof(tmp)) {
        sb_puts(sb, tmp);
        return;
    }
    // Rare long line: grow and reformat directly.
    sb_reserve(sb, (size_t)n);
    if (sb->oom)
        return;
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

// 24-bit foreground colour escape.
static void sb_fg(SB *sb, int r, int g, int b)
{
    sb_printf(sb, "\x1b[38;2;%d;%d;%dm", r, g, b);
}

// ---- formatting primitives -------------------------------------------------

// Horizontal rule of `w` box-drawing chars in the muted rule colour.
static void rule(SB *sb, const char *glyph, int w)
{
    sb_fg(sb, C_RULE);
    for (int i = 0; i < w; i++)
        sb_puts(sb, glyph);
    sb_puts(sb, RST "\n");
}

// Per-character horizontal colour gradient — showcases truecolor.
static void gradient(SB *sb, const char *text, int r0, int g0, int b0, int r1, int g1, int b1)
{
    size_t n = strlen(text);
    if (n == 0)
        return;
    for (size_t i = 0; i < n; i++) {
        float t = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
        int r = (int)(r0 + (r1 - r0) * t);
        int g = (int)(g0 + (g1 - g0) * t);
        int b = (int)(b0 + (b1 - b0) * t);
        sb_fg(sb, r, g, b);
        char c[2] = { text[i], 0 };
        sb_puts(sb, c);
    }
    sb_puts(sb, RST);
}

// Section header: blank line, "▍ TITLE" in bold purple, then a thin rule.
static void section(SB *sb, const char *title)
{
    sb_puts(sb, "\n");
    sb_fg(sb, C_HEADER);
    sb_printf(sb, BOLD "  ▍ %s" RST "\n", title);
    sb_puts(sb, "  ");
    rule(sb, "─", DW);
}

// Key/value row: dim key padded into an 18-col gutter, then the value (or dim
// "(unset)"). 18 keeps a 2-space gap even for the longest key.
static void kv(SB *sb, const char *key, const char *val)
{
    sb_fg(sb, C_KEY);
    sb_printf(sb, "  %-18s" RST, key);
    if (val && *val) {
        sb_fg(sb, C_VAL);
        sb_puts(sb, val);
        sb_puts(sb, RST);
    } else {
        sb_puts(sb, DIM "(unset)" RST);
    }
    sb_puts(sb, "\n");
}

static void kvf(SB *sb, const char *key, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    kv(sb, key, tmp);
}

// Coloured boolean value row (green on / red off).
static void kv_bool(SB *sb, const char *key, bool on, const char *on_s, const char *off_s)
{
    sb_fg(sb, C_KEY);
    sb_printf(sb, "  %-18s" RST, key);
    if (on)
        sb_fg(sb, C_ON);
    else
        sb_fg(sb, C_OFF);
    sb_puts(sb, on ? on_s : off_s);
    sb_puts(sb, RST "\n");
}

// Key/value row with a caller-chosen value colour (for multi-state values).
static void kv_colored(SB *sb, const char *key, int r, int g, int b, const char *val)
{
    sb_fg(sb, C_KEY);
    sb_printf(sb, "  %-18s" RST, key);
    sb_fg(sb, r, g, b);
    sb_puts(sb, val);
    sb_puts(sb, RST "\n");
}

static const char *or_unset(const char *s)
{
    return (s && *s) ? s : NULL;
}

// ---- report ----------------------------------------------------------------

char *diag_build_report(const DiagSources *s)
{
    if (!s)
        return NULL;

    SB sb = { 0 };

    // Banner: emoji + gradient wordmark + version, framed by heavy rules.
    sb_puts(&sb, "\n  ");
    rule(&sb, "━", DW);
    sb_puts(&sb, "  \U0001f338  "); // 🌸
    gradient(&sb, "bloom-terminal", C_TITLE_A, C_TITLE_B);
    sb_fg(&sb, C_KEY);
    sb_puts(&sb, DIM "  ·  diagnostics" RST "\n");
    sb_fg(&sb, C_ACCENT);
    sb_printf(&sb, "  %s\n", BLOOM_TERMINAL_VERSION);
    sb_puts(&sb, RST "  ");
    rule(&sb, "━", DW);

    // Version & build
    section(&sb, "VERSION & BUILD");
    kv(&sb, "bloom-terminal", BLOOM_TERMINAL_VERSION);
    kv(&sb, "built with", BUILD_CC);
    kv(&sb, "bloom-vt", DEP_BLOOM_VT_VERSION);
    kv(&sb, "SDL3", DEP_SDL3_VERSION);
    kv(&sb, "FreeType", DEP_FREETYPE_VERSION);
    kv(&sb, "HarfBuzz", DEP_HARFBUZZ_VERSION);
    kv(&sb, "libpng", DEP_LIBPNG_VERSION);
#ifdef DEP_FONTCONFIG_VERSION
    kv(&sb, "Fontconfig", DEP_FONTCONFIG_VERSION);
#endif
#ifdef __APPLE__
    kv(&sb, "font resolver", "Core Text (system)");
#elif defined(_WIN32)
    kv(&sb, "font resolver", "W32 native");
#else
    kv(&sb, "font resolver", "Fontconfig");
#endif
#ifdef HAVE_GTK4
    kvf(&sb, "GTK4", "%s  (libadwaita %s)", DEP_GTK4_VERSION, DEP_LIBADWAITA_VERSION);
#ifdef HAVE_VULKAN_DMABUF
    kv(&sb, "GTK4 present", "Vulkan zero-copy DMA-BUF");
#else
    kv(&sb, "GTK4 present", "readback");
#endif
#endif

    // Rendering
    section(&sb, "RENDERING");
    kv(&sb, "platform", or_unset(s->platform_name));
    kv(&sb, "renderer", or_unset(s->renderer_name));
    // GPU + driver (when the backend can report them). Permissively-licensed
    // open-source drivers (Mesa) are shown green; others plain.
    if (s->gpu_device)
        kv(&sb, "GPU", s->gpu_device);
    if (s->gpu_driver) {
        if (s->gpu_driver_libre)
            kv_colored(&sb, "driver", C_ON, s->gpu_driver);
        else
            kv(&sb, "driver", s->gpu_driver);
    }
    kv_bool(&sb, "linear-light", s->linear_light, "enabled", "disabled (sRGB)");
    // Glyph curve has three distinct states, not two: a neutral curve applies
    // nothing (no shader needed); a non-neutral curve runs either in the GPU
    // luminance-aware shader or, where that's unavailable, the uniform baked LUT.
    if (s->text_gamma == 1.0f && s->text_contrast == 0.0f)
        kv_colored(&sb, "glyph curve", C_VAL, "neutral (identity)");
    else if (s->glyph_shader)
        kv_colored(&sb, "glyph curve", C_ON, "GPU shader (luminance-aware)");
    else
        kv_colored(&sb, "glyph curve", C_ACCENT, "baked LUT (uniform — no GPU shader)");
    kvf(&sb, "content scale", "%.2f", (double)s->content_scale);
    kvf(&sb, "window", "%d x %d px", s->pixel_width, s->pixel_height);
    kvf(&sb, "cell", "%d x %d px", s->cell_width, s->cell_height);
    kvf(&sb, "grid", "%d cols x %d rows", s->cols, s->rows);

    // Configuration
    section(&sb, "CONFIGURATION");
    kv(&sb, "config file", s->config_path ? s->config_path : DIM "(defaults)" RST);
    kv(&sb, "font pattern", or_unset(s->font_pattern));
    kv(&sb, "font resolved", or_unset(s->font_path));
    kv(&sb, "font source", or_unset(s->font_source));
    kv(&sb, "hinting", or_unset(s->hinting));
    kvf(&sb, "scrollback", "%d lines%s", s->scrollback, s->scrollback == 0 ? " (disabled)" : "");
    if (s->text_gamma == 1.0f && s->text_contrast == 0.0f)
        kv(&sb, "text composition", "neutral (gamma 1.0, contrast 0)");
    else
        kvf(&sb, "text composition", "gamma %.2f, contrast %.1f", (double)s->text_gamma,
            (double)s->text_contrast);
    kv(&sb, "word chars", or_unset(s->word_chars));

    // Session / environment
    section(&sb, "SESSION");
    kv(&sb, "TERM", or_unset(s->term_env));
    kv(&sb, "COLORTERM", or_unset(s->colorterm_env));
    kv(&sb, "LANG", or_unset(s->lang_env));
    kv(&sb, "title", or_unset(s->title));
    // Neutral state, not a good/bad condition — use the plain value colour
    // rather than kv_bool's green/red (red "no" wrongly reads as an error).
    kv(&sb, "alt screen", s->altscreen ? "yes" : "no");
    if (s->mouse_mode == 0)
        kv(&sb, "mouse mode", "off");
    else
        kvf(&sb, "mouse mode", "on (mode %d)", s->mouse_mode);

    // System
    section(&sb, "SYSTEM");
#ifndef _WIN32
    struct utsname u;
    if (uname(&u) == 0) {
        kvf(&sb, "os", "%s %s", u.sysname, u.release);
        kv(&sb, "kernel", u.version);
        kv(&sb, "arch", u.machine);
        kv(&sb, "host", u.nodename);
    } else {
        kv(&sb, "os", NULL);
    }
#else
    kv(&sb, "os", "Windows");
#endif

    // Footer
    sb_puts(&sb, "\n  ");
    rule(&sb, "─", DW);
    sb_fg(&sb, C_KEY);
    sb_puts(&sb, DIM "  Report issues at " RST);
    sb_fg(&sb, C_TITLE_B);
    // Clickable OSC 8 hyperlink (cyan URL as the visible text). No explicit
    // underline — bloom-terminal draws the hyperlink underline itself.
    sb_puts(&sb, OSC8_OPEN(ISSUES_URL) ISSUES_URL RST OSC8_CLOSE);
    sb_puts(&sb, "\n\n");

    if (sb.oom) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf ? sb.buf : strdup("");
}
