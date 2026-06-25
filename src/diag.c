// Diagnostics report builder. Produces a styled, self-contained UTF-8 document
// (ANSI-palette SGR, box-drawing, bold/underline) for bug reporting — shown
// through the system pager by the Ctrl+Shift+F6 binding in main.c. See diag.h.
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
// Attributes plus 3/4-bit ANSI colour SGR. Colours resolve against the host
// terminal's own palette (CharmTone inside bloom-terminal) — no hardcoded RGB,
// so the report inherits whatever theme the viewing terminal uses.
#define RST       "\x1b[0m"
#define BOLD      "\x1b[1m"
#define DIM       "\x1b[2m"
#define ITAL      "\x1b[3m"
#define ULN       "\x1b[4m"
#define FG_HEADER "\x1b[35m" // magenta — section titles
#define FG_RULE   "\x1b[90m" // bright black — rules
#define FG_ON     "\x1b[32m" // green — enabled / libre driver
#define FG_OFF    "\x1b[31m" // red — disabled
#define FG_ACCENT "\x1b[33m" // yellow — version / accents
#define FG_LINK   "\x1b[36m" // cyan — hyperlinks

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

// ---- formatting primitives -------------------------------------------------

// Horizontal rule of `w` box-drawing chars in the muted rule colour.
static void rule(SB *sb, const char *glyph, int w)
{
    sb_puts(sb, FG_RULE);
    for (int i = 0; i < w; i++)
        sb_puts(sb, glyph);
    sb_puts(sb, RST "\n");
}

// Section header: blank line, "▍ TITLE" in bold magenta, then a thin rule.
static void section(SB *sb, const char *title)
{
    sb_puts(sb, "\n");
    sb_printf(sb, BOLD FG_HEADER "  ▍ %s" RST "\n", title);
    sb_puts(sb, "  ");
    rule(sb, "─", DW);
}

// Sub-section header inside a section: italic title, thin rule.
static void subsection(SB *sb, const char *title, const char *desc)
{
    sb_puts(sb, "\n  ");
    sb_printf(sb, ITAL FG_HEADER "%s" RST, title);
    if (desc)
        sb_printf(sb, DIM "  %s" RST, desc);
    sb_puts(sb, "\n  ");
    rule(sb, "─", DW);
}

// Key/value row: dim key padded into a 20-col gutter, then the value (or dim
// "(unset)") in the default foreground. 20 keeps a 2-space gap even for the
// longest key ("lottie rasterizer").
static void kv(SB *sb, const char *key, const char *val)
{
    sb_printf(sb, DIM "  %-20s" RST, key);
    if (val && *val)
        sb_puts(sb, val);
    else
        sb_puts(sb, DIM "(unset)" RST);
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
    sb_printf(sb, DIM "  %-20s" RST, key);
    sb_puts(sb, on ? FG_ON : FG_OFF);
    sb_puts(sb, on ? on_s : off_s);
    sb_puts(sb, RST "\n");
}

// Key/value row with a caller-chosen SGR for the value (for multi-state
// values). Pass "" to leave the value in the default foreground.
static void kv_colored(SB *sb, const char *key, const char *sgr, const char *val)
{
    sb_printf(sb, DIM "  %-20s" RST, key);
    sb_puts(sb, sgr);
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

    // Banner: emoji + bold wordmark + version, framed by heavy rules.
    sb_puts(&sb, "\n  ");
    rule(&sb, "━", DW);
    sb_puts(&sb, "  \U0001f338  "); // 🌸
    sb_puts(&sb, BOLD "bloom-terminal" RST);
    sb_puts(&sb, DIM "  ·  diagnostics" RST "\n");
    sb_printf(&sb, FG_ACCENT "  %s" RST "\n", BLOOM_TERMINAL_VERSION);
    sb_puts(&sb, "  ");
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
            kv_colored(&sb, "driver", FG_ON, s->gpu_driver);
        else
            kv(&sb, "driver", s->gpu_driver);
    }
    kv_bool(&sb, "linear-light", s->linear_light, "enabled", "disabled (sRGB)");
    // Glyph curve has three distinct states, not two: a neutral curve applies
    // nothing (no shader needed); a non-neutral curve runs either in the GPU
    // luminance-aware shader or, where that's unavailable, the uniform baked LUT.
    if (s->text_gamma == 1.0f && s->text_contrast == 0.0f)
        kv_colored(&sb, "glyph curve", "", "neutral (identity)");
    else if (s->glyph_shader)
        kv_colored(&sb, "glyph curve", FG_ON, "GPU shader (luminance-aware)");
    else
        kv_colored(&sb, "glyph curve", FG_ACCENT, "baked LUT (uniform — no GPU shader)");
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

    // VT engine features
    section(&sb, "VT FEATURES");
    kv(&sb, "engine", or_unset(s->vt_backend));
    kv(&sb, "capabilities", "sixel " FG_RULE "·" RST " OSC 8 " FG_RULE "·" RST " grapheme clusters " FG_RULE "·" RST " reflow");
    kv_bool(&sb, "lottie rasterizer", s->lottie_rasterizer, "ThorVG", "unavailable (blank frames)");
    kv_bool(&sb, "OSC 52 (clipboard)", s->osc52, "wired", "not wired");
    kv_bool(&sb, "hardened heap", s->hardened_heap, "enabled", "disabled");
    subsection(&sb, "runtime modes", "toggled by the running application via DECSET");
    kv(&sb, "bracketed paste", s->bracketed_paste ? "on" : "off");
    kv(&sb, "sync output", s->sync_output ? "on" : "off");
    kv(&sb, "focus reporting", s->focus_reporting ? "on" : "off");
    kv(&sb, "sixel scrolling", s->sixel_scrolling ? "on" : "off");
    // Neutral state, not a good/bad condition — use the plain value colour
    // rather than kv_bool's green/red (red "no" wrongly reads as an error).
    kv(&sb, "alt screen", s->altscreen ? "yes" : "no");
    if (s->mouse_mode == 0)
        kv(&sb, "mouse mode", "off");
    else
        kvf(&sb, "mouse mode", "on (mode %d)", s->mouse_mode);

    // Session / environment
    section(&sb, "SESSION");
    kv(&sb, "TERM", or_unset(s->term_env));
    kv(&sb, "COLORTERM", or_unset(s->colorterm_env));
    kv(&sb, "LANG", or_unset(s->lang_env));
    kv(&sb, "title", or_unset(s->title));

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
    sb_puts(&sb, DIM "  Report issues at " RST);
    // Clickable OSC 8 hyperlink (cyan URL as the visible text). No explicit
    // underline — bloom-terminal draws the hyperlink underline itself.
    sb_puts(&sb, FG_LINK OSC8_OPEN(ISSUES_URL) ISSUES_URL RST OSC8_CLOSE);
    sb_puts(&sb, "\n\n");

    if (sb.oom) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf ? sb.buf : strdup("");
}
