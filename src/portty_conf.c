/*
 * portty — INI-style configuration parsing
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "portty_conf.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define access _access
#define R_OK   4
#else
#include <unistd.h>
#endif

#define MAX_LINE 1024

/* Trim leading and trailing whitespace in-place, return pointer into buf */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

/* Parse a boolean string. Returns 1 for true, 0 for false, -1 for invalid. */
static int parse_bool(const char *s)
{
    if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 || strcmp(s, "1") == 0)
        return 1;
    if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 || strcmp(s, "0") == 0)
        return 0;
    return -1;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Scale a `digits`-wide hex component (0..16^digits - 1) to 0..255,
 * rounded to nearest — the XParseColor fixed-point rule. */
static uint32_t scale_hex_component(uint32_t v, int digits)
{
    static const uint32_t max[5] = { 0, 0xfu, 0xffu, 0xfffu, 0xffffu };
    return (v * 255u + max[digits] / 2u) / max[digits];
}

/* Parse a color value — the XParseColor RGB forms shared with the OSC
 * 10/11/12 grammar: `#RGB`, `#RRGGBB`, `#RRRRRGGGGGGBBBBBB`, or
 * `rgb:R[/G[/B]]` with 1-4 hex digits per component (missing
 * components 0). X color names are not accepted. */
static bool parse_color_value(const char *s, uint32_t *out_rgb)
{
    size_t len = strlen(s);
    uint32_t comp[3] = { 0, 0, 0 };

    if (len >= 4 && strncmp(s, "rgb:", 4) == 0) {
        const char *p = s + 4;
        size_t pos = 4;
        int parsed = 0;
        for (int i = 0; i < 3 && pos < len; i++) {
            uint32_t v = 0;
            int digits = 0;
            while (pos < len && p[0] != '/') {
                int h = hex_digit(p[0]);
                if (h < 0 || digits == 4)
                    return false;
                v = v * 16u + (uint32_t)h;
                digits++;
                p++;
                pos++;
            }
            if (digits == 0)
                return false;
            comp[i] = scale_hex_component(v, digits);
            parsed++;
            if (i < 2 && pos < len) {
                p++;
                pos++;
            }
        }
        if (parsed == 0 || pos != len)
            return false;
    } else if (len >= 1 && s[0] == '#') {
        int width = ((int)len - 1) / 3;
        if (len != 4 && len != 7 && len != 13) /* #RGB, #RRGGBB, #RRRRRGGGGGGBBBBBB */
            return false;
        for (int i = 0; i < 3; i++) {
            uint32_t v = 0;
            for (int k = 0; k < width; k++) {
                int h = hex_digit(s[1 + i * width + k]);
                if (h < 0)
                    return false;
                v = v * 16u + (uint32_t)h;
            }
            comp[i] = scale_hex_component(v, width);
        }
    } else {
        return false;
    }

    *out_rgb = (comp[0] << 16) | (comp[1] << 8) | comp[2];
    return true;
}

/* "#rrggbb", or "(unset)". */
static const char *fmt_color(char *buf, size_t cap, PorttyColor c)
{
    if (!c.set) {
        snprintf(buf, cap, "(unset)");
        return buf;
    }
    snprintf(buf, cap, "#%06x", c.rgb & 0xFFFFFFu);
    return buf;
}

/* Find the config file path. Returns a malloc'd string or NULL. */
static char *find_config_file(void)
{
    /* 1. CWD */
    if (access("portty.conf", R_OK) == 0)
        return strdup("portty.conf");

    char path[MAX_LINE];

#ifdef _WIN32
    /* 2. XDG_CONFIG_HOME (power user override) */
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        snprintf(path, sizeof(path), "%s\\portty\\portty.conf", xdg);
        if (access(path, R_OK) == 0)
            return strdup(path);
    }
    /* 3. %APPDATA%\portty\portty.conf */
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        snprintf(path, sizeof(path), "%s\\portty\\portty.conf", appdata);
        if (access(path, R_OK) == 0)
            return strdup(path);
    }
#else
    /* 2. XDG_CONFIG_HOME or ~/.config */
    const char *xdg = getenv("XDG_CONFIG_HOME");

    if (xdg && xdg[0] != '\0') {
        snprintf(path, sizeof(path), "%s/portty/portty.conf", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home)
            return NULL;
        snprintf(path, sizeof(path), "%s/.config/portty/portty.conf", home);
    }

    if (access(path, R_OK) == 0)
        return strdup(path);
#endif

    return NULL;
}

void portty_conf_init(PorttyConf *conf)
{
    conf->font = NULL;
    conf->cols = 0;
    conf->rows = 0;
    conf->hinting = PORTTY_HINT_UNSET;
    conf->verbose = -1;
    conf->word_chars = NULL;
    conf->scrollback = -1;
    conf->ambiguous_wide = -1;
    conf->borderless = -1;
    conf->text_gamma = -1.0f;
    conf->text_contrast = -1.0f;
    conf->dim_opacity = -1.0f;
    conf->bell = PORTTY_BELL_ALWAYS;
    conf->shell = NULL;
    conf->dump_dir = NULL;
    conf->source_path = NULL;
    conf->fg_color = (PorttyColor){ false, 0 };
    conf->bg_color = (PorttyColor){ false, 0 };
    conf->cursor_color = (PorttyColor){ false, 0 };
    conf->cursor_text_color = (PorttyColor){ false, 0 };
    conf->cursor_shape = PORTTY_CURSOR_UNSET;
    conf->cursor_blink = -1;
}

const char *portty_bell_mode_name(PorttyBellMode mode)
{
    switch (mode) {
    case PORTTY_BELL_UNFOCUSED:
        return "unfocused";
    case PORTTY_BELL_NONE:
        return "none";
    case PORTTY_BELL_ALWAYS:
    default:
        return "always";
    }
}

const char *portty_cursor_shape_name(PorttyCursorShape shape)
{
    switch (shape) {
    case PORTTY_CURSOR_UNDERLINE:
        return "underline";
    case PORTTY_CURSOR_BAR:
        return "bar";
    case PORTTY_CURSOR_BLOCK:
    default:
        return "block";
    }
}

bool portty_conf_load_path(PorttyConf *conf, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return false;

    vlog("Loading config from %s\n", path);

    free(conf->source_path);
    conf->source_path = strdup(path);

    char line[MAX_LINE];
    int lineno = 0;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char *s = trim(line);

        /* Skip empty lines and comments */
        if (*s == '\0' || *s == '#' || *s == ';')
            continue;

        /* Section header */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) {
                fprintf(stderr, "WARNING: %s:%d: malformed section header\n", path, lineno);
                continue;
            }
            continue;
        }

        /* Parse key = value */
        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "WARNING: %s:%d: expected key = value\n", path, lineno);
            continue;
        }

        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcmp(key, "font") == 0) {
            free(conf->font);
            conf->font = strdup(val);
        } else if (strcmp(key, "geometry") == 0) {
            int w = 0, h = 0;
            if (sscanf(val, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                conf->cols = w;
                conf->rows = h;
            } else {
                fprintf(stderr, "WARNING: %s:%d: invalid geometry '%s' (use COLSxROWS)\n",
                        path, lineno, val);
            }
        } else if (strcmp(key, "hinting") == 0) {
            if (strcmp(val, "none") == 0) {
                conf->hinting = PORTTY_HINT_NONE;
            } else if (strcmp(val, "light") == 0) {
                conf->hinting = PORTTY_HINT_LIGHT;
            } else if (strcmp(val, "normal") == 0) {
                conf->hinting = PORTTY_HINT_NORMAL;
            } else if (strcmp(val, "mono") == 0) {
                conf->hinting = PORTTY_HINT_MONO;
            } else {
                fprintf(stderr,
                        "WARNING: %s:%d: invalid hinting '%s' (use none, light, normal, mono)\n",
                        path, lineno, val);
            }
        } else if (strcmp(key, "verbose") == 0) {
            int b = parse_bool(val);
            if (b < 0)
                fprintf(stderr, "WARNING: %s:%d: invalid boolean '%s'\n", path, lineno, val);
            else
                conf->verbose = b;
        } else if (strcmp(key, "word_chars") == 0) {
            free(conf->word_chars);
            conf->word_chars = strdup(val);
        } else if (strcmp(key, "scrollback") == 0) {
            char *end = NULL;
            long n = strtol(val, &end, 10);
            if (end == val || *end != '\0' || n < 0 || n > INT_MAX) {
                fprintf(stderr,
                        "WARNING: %s:%d: invalid scrollback '%s' (use a non-negative integer)\n",
                        path, lineno, val);
            } else {
                conf->scrollback = (int)n;
            }
        } else if (strcmp(key, "shell") == 0) {
            free(conf->shell);
            conf->shell = strdup(val);
        } else if (strcmp(key, "dump_dir") == 0) {
            free(conf->dump_dir);
            conf->dump_dir = strdup(val);
        } else if (strcmp(key, "ambiguous_wide") == 0) {
            int b = parse_bool(val);
            if (b < 0)
                fprintf(stderr, "WARNING: %s:%d: invalid ambiguous_wide '%s' "
                                "(expected true/false, yes/no, or 1/0)\n",
                        path, lineno, val);
            else
                conf->ambiguous_wide = b;
        } else if (strcmp(key, "borderless") == 0) {
            int b = parse_bool(val);
            if (b < 0)
                fprintf(stderr, "WARNING: %s:%d: invalid borderless '%s' "
                                "(expected true/false, yes/no, or 1/0)\n",
                        path, lineno, val);
            else
                conf->borderless = b;
        } else if (strcmp(key, "text_composition_strategy") == 0) {
            /* kitty-compatible: "kitty" (gamma 1.7, contrast 30),
             * "neutral"/"correct" (pure linear), or "<gamma> [contrast]". */
            if (strcmp(val, "kitty") == 0) {
                conf->text_gamma = 1.7f;
                conf->text_contrast = 30.0f;
            } else if (strcmp(val, "neutral") == 0 || strcmp(val, "correct") == 0) {
                conf->text_gamma = 1.0f;
                conf->text_contrast = 0.0f;
            } else {
                char *end = NULL;
                float g = strtof(val, &end);
                float c = 0.0f;
                bool ok = (end != val && g > 0.0f && g <= 4.0f);
                if (ok && *end != '\0') {
                    char *end2 = NULL;
                    c = strtof(end, &end2);
                    while (*end2 == ' ' || *end2 == '\t')
                        end2++;
                    if (end2 == end || *end2 != '\0' || c < 0.0f || c > 100.0f)
                        ok = false;
                }
                if (!ok) {
                    fprintf(stderr,
                            "WARNING: %s:%d: invalid text_composition_strategy "
                            "'%s' (use 'kitty', 'neutral', or '<gamma> "
                            "<contrast>')\n",
                            path, lineno, val);
                } else {
                    conf->text_gamma = g;
                    conf->text_contrast = c;
                }
            }
        } else if (strcmp(key, "dim_opacity") == 0) {
            char *end = NULL;
            float o = strtof(val, &end);
            while (*end == ' ' || *end == '\t')
                end++;
            if (end == val || *end != '\0' || o < 0.0f || o > 1.0f) {
                fprintf(stderr,
                        "WARNING: %s:%d: invalid dim_opacity '%s' (use a number "
                        "between 0 and 1)\n",
                        path, lineno, val);
            } else {
                conf->dim_opacity = o;
            }
        } else if (strcmp(key, "bell") == 0) {
            if (strcmp(val, "always") == 0) {
                conf->bell = PORTTY_BELL_ALWAYS;
            } else if (strcmp(val, "unfocused") == 0) {
                conf->bell = PORTTY_BELL_UNFOCUSED;
            } else if (strcmp(val, "none") == 0) {
                conf->bell = PORTTY_BELL_NONE;
            } else {
                fprintf(stderr,
                        "WARNING: %s:%d: invalid bell '%s' (use always, "
                        "unfocused, none)\n",
                        path, lineno, val);
            }
        } else if (strcmp(key, "foreground") == 0 || strcmp(key, "background") == 0 ||
                   strcmp(key, "cursor_color") == 0 || strcmp(key, "cursor_text_color") == 0) {
            uint32_t rgb = 0;
            if (!parse_color_value(val, &rgb)) {
                fprintf(stderr,
                        "WARNING: %s:%d: invalid %s '%s' (use #RGB, #RRGGBB, "
                        "#RRRRRGGGGGGBBBBBB, or rgb:R/G/B)\n",
                        path, lineno, key, val);
            } else {
                PorttyColor *slot = &conf->fg_color;
                if (strcmp(key, "background") == 0)
                    slot = &conf->bg_color;
                else if (strcmp(key, "cursor_color") == 0)
                    slot = &conf->cursor_color;
                else if (strcmp(key, "cursor_text_color") == 0)
                    slot = &conf->cursor_text_color;
                slot->set = true;
                slot->rgb = rgb;
            }
        } else if (strcmp(key, "cursor") == 0) {
            if (strcmp(val, "block") == 0) {
                conf->cursor_shape = PORTTY_CURSOR_BLOCK;
            } else if (strcmp(val, "underline") == 0 || strcmp(val, "hbar") == 0) {
                conf->cursor_shape = PORTTY_CURSOR_UNDERLINE;
            } else if (strcmp(val, "bar") == 0 || strcmp(val, "vbar") == 0) {
                conf->cursor_shape = PORTTY_CURSOR_BAR;
            } else {
                fprintf(stderr,
                        "WARNING: %s:%d: invalid cursor '%s' (use block, "
                        "underline, bar)\n",
                        path, lineno, val);
            }
        } else if (strcmp(key, "cursor_blink") == 0) {
            int b = parse_bool(val);
            if (b < 0)
                fprintf(stderr, "WARNING: %s:%d: invalid cursor_blink '%s' "
                                "(expected true/false, yes/no, or 1/0)\n",
                        path, lineno, val);
            else
                conf->cursor_blink = b;
        } else {
            fprintf(stderr, "WARNING: %s:%d: unknown key '%s'\n", path, lineno, key);
        }
    }

    fclose(fp);

    vlog("Config: font=%s cols=%d rows=%d hinting=%d verbose=%d"
         " word_chars=%s scrollback=%d ambiguous_wide=%d shell=%s"
         " text_gamma=%.2f text_contrast=%.1f dim_opacity=%.2f borderless=%d"
         " bell=%s\n",
         conf->font ? conf->font : "(unset)", conf->cols, conf->rows, conf->hinting,
         conf->verbose, conf->word_chars ? conf->word_chars : "(unset)",
         conf->scrollback, conf->ambiguous_wide,
         conf->shell ? conf->shell : "(unset)",
         conf->text_gamma, conf->text_contrast, conf->dim_opacity, conf->borderless,
         portty_bell_mode_name(conf->bell));

    char fg[16], bg[16], cur[16], cur_text[16];
    vlog("Config: fg=%s bg=%s cursor=%s cursor_blink=%d cursor_color=%s"
         " cursor_text_color=%s\n",
         fmt_color(fg, sizeof(fg), conf->fg_color),
         fmt_color(bg, sizeof(bg), conf->bg_color),
         portty_cursor_shape_name(conf->cursor_shape), conf->cursor_blink,
         fmt_color(cur, sizeof(cur), conf->cursor_color),
         fmt_color(cur_text, sizeof(cur_text), conf->cursor_text_color));

    return true;
}

bool portty_conf_load(PorttyConf *conf)
{
    char *path = find_config_file();
    if (!path)
        return false;

    bool result = portty_conf_load_path(conf, path);
    free(path);
    return result;
}

void portty_conf_free(PorttyConf *conf)
{
    free(conf->font);
    conf->font = NULL;
    free(conf->word_chars);
    conf->word_chars = NULL;
    free(conf->shell);
    conf->shell = NULL;
    free(conf->dump_dir);
    conf->dump_dir = NULL;
    free(conf->source_path);
    conf->source_path = NULL;
}
