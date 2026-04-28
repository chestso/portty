#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bloom_conf.h"
#include "common.h"
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

/* Find the config file path. Returns a malloc'd string or NULL. */
static char *find_config_file(void)
{
    /* 1. CWD */
    if (access("bloom.conf", R_OK) == 0)
        return strdup("bloom.conf");

    char path[MAX_LINE];

#ifdef _WIN32
    /* 2. %APPDATA%\bloom\bloom.conf */
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        snprintf(path, sizeof(path), "%s\\bloom\\bloom.conf", appdata);
        if (access(path, R_OK) == 0)
            return strdup(path);
    }
#else
    /* 2. XDG_CONFIG_HOME or ~/.config */
    const char *xdg = getenv("XDG_CONFIG_HOME");

    if (xdg && xdg[0] != '\0') {
        snprintf(path, sizeof(path), "%s/bloom/bloom.conf", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home)
            return NULL;
        snprintf(path, sizeof(path), "%s/.config/bloom/bloom.conf", home);
    }

    if (access(path, R_OK) == 0)
        return strdup(path);
#endif

    return NULL;
}

void bloom_conf_init(BloomConf *conf)
{
    conf->font = NULL;
    conf->cols = 0;
    conf->rows = 0;
    conf->hinting = BLOOM_HINT_UNSET;
    conf->padding = -1;
    conf->verbose = -1;
    conf->word_chars = NULL;
    conf->platform = NULL;
    conf->scrollback = -1;
}

bool bloom_conf_load_path(BloomConf *conf, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return false;

    vlog("Loading config from %s\n", path);

    char line[MAX_LINE];
    int in_terminal_section = 0;
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
            *end = '\0';
            in_terminal_section = (strcmp(s + 1, "terminal") == 0);
            continue;
        }

        /* Only process keys inside [terminal] */
        if (!in_terminal_section)
            continue;

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
                conf->hinting = BLOOM_HINT_NONE;
            } else if (strcmp(val, "light") == 0) {
                conf->hinting = BLOOM_HINT_LIGHT;
            } else if (strcmp(val, "normal") == 0) {
                conf->hinting = BLOOM_HINT_NORMAL;
            } else if (strcmp(val, "mono") == 0) {
                conf->hinting = BLOOM_HINT_MONO;
            } else {
                fprintf(stderr,
                        "WARNING: %s:%d: invalid hinting '%s' (use none, light, normal, mono)\n",
                        path, lineno, val);
            }
        } else if (strcmp(key, "padding") == 0) {
            int b = parse_bool(val);
            if (b < 0)
                fprintf(stderr, "WARNING: %s:%d: invalid boolean '%s'\n", path, lineno, val);
            else
                conf->padding = b;
        } else if (strcmp(key, "verbose") == 0) {
            int b = parse_bool(val);
            if (b < 0)
                fprintf(stderr, "WARNING: %s:%d: invalid boolean '%s'\n", path, lineno, val);
            else
                conf->verbose = b;
        } else if (strcmp(key, "word_chars") == 0) {
            free(conf->word_chars);
            conf->word_chars = strdup(val);
        } else if (strcmp(key, "platform") == 0) {
            if (strcmp(val, "sdl3") == 0 || strcmp(val, "gtk4") == 0) {
                free(conf->platform);
                conf->platform = strdup(val);
            } else {
                fprintf(stderr,
                        "WARNING: %s:%d: invalid platform '%s' (use sdl3, gtk4)\n",
                        path, lineno, val);
            }
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
        } else {
            fprintf(stderr, "WARNING: %s:%d: unknown key '%s'\n", path, lineno, key);
        }
    }

    fclose(fp);

    vlog("Config: font=%s cols=%d rows=%d hinting=%d padding=%d verbose=%d"
         " word_chars=%s platform=%s scrollback=%d\n",
         conf->font ? conf->font : "(unset)", conf->cols, conf->rows, conf->hinting,
         conf->padding, conf->verbose, conf->word_chars ? conf->word_chars : "(unset)",
         conf->platform ? conf->platform : "(unset)", conf->scrollback);

    return true;
}

bool bloom_conf_load(BloomConf *conf)
{
    char *path = find_config_file();
    if (!path)
        return false;

    bool result = bloom_conf_load_path(conf, path);
    free(path);
    return result;
}

void bloom_conf_free(BloomConf *conf)
{
    free(conf->font);
    conf->font = NULL;
    free(conf->word_chars);
    conf->word_chars = NULL;
    free(conf->platform);
    conf->platform = NULL;
}
