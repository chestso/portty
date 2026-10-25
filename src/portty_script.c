/*
 * portty — Script engine wiring: coffer core + UI extension commands
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_script.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct PorttyScript
{
    CfrScript *script;
    CfrScriptRun *run;
};

/* UI extension command names, registered with the engine at load time.
 * Anything not in this list and not a core command is a parse error. */
static const char *const EXT_NAMES[] = {
    "screendump", "renderdump", "dumpstate", "record-start",
    "record-stop", "panel", "panel_hide", "mousemove",
    "mousedown", "mouseup", "winsize", "display-scale",
    "assert-hover", "assert-no-hover", NULL
};

/* ── Engine host hooks (adapt ScriptUIExt to CfrScriptHost) ── */

static void host_pty_write(void *ud, const char *data, size_t len)
{
    ScriptUIExt *ext = ud;
    if (ext->pty)
        pty_write(ext->pty, data, len);
}

static void host_emit(void *ud, const char *data, size_t len)
{
    ScriptUIExt *ext = ud;
    if (ext->emit_fn)
        ext->emit_fn(ext->app, data, len);
}

static int host_poll_exit(void *ud, int *status)
{
    ScriptUIExt *ext = ud;
    if (!ext->pty)
        return -1;
    return pty_poll_exit(ext->pty, status);
}

static int host_resize(void *ud, int cols, int rows)
{
    ScriptUIExt *ext = ud;
    if (!ext->resize_fn)
        return -1;
    ext->resize_fn(ext->resize_ud, cols, rows);
    return 0;
}

/* ── Extension command arg parsing helpers ── */

/* Strip one layer of surrounding quotes in place. */
static void strip_quotes(char *s)
{
    size_t len = strlen(s);
    if (len >= 2 && ((s[0] == '"' && s[len - 1] == '"') ||
                     (s[0] == '\'' && s[len - 1] == '\''))) {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

/* panel <id> <col> <row> <cols> <rows> "title" "body" [level] [flags] */
static bool exec_panel(ScriptUIExt *ext, char *args, int line)
{
    if (!ext->panel_fn) {
        fprintf(stderr, "[%d] panel: not supported by this backend\n", line);
        return true;
    }

    int parsed[5];
    char *p = args;
    int count = 0;
    while (count < 5 && *p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        char *end;
        parsed[count] = (int)strtol(p, &end, 10);
        if (end == p)
            break;
        count++;
        p = end;
    }
    if (count < 5) {
        fprintf(stderr, "[%d] panel: requires id col row cols rows\n", line);
        return false;
    }

    while (*p && *p != '"')
        p++;
    if (*p != '"') {
        fprintf(stderr, "[%d] panel: requires quoted title and body\n", line);
        return false;
    }
    p++;
    char *title_start = p;
    while (*p && !(*p == '"' && (p == title_start || *(p - 1) != '\\')))
        p++;
    if (*p != '"') {
        fprintf(stderr, "[%d] panel: unterminated title string\n", line);
        return false;
    }
    char title[128];
    size_t title_len = (size_t)(p - title_start);
    if (title_len >= sizeof(title))
        title_len = sizeof(title) - 1;
    memcpy(title, title_start, title_len);
    title[title_len] = '\0';

    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != '"') {
        fprintf(stderr, "[%d] panel: requires quoted body string\n", line);
        return false;
    }
    p++;
    char *body_start = p;
    while (*p && !(*p == '"' && (p == body_start || *(p - 1) != '\\')))
        p++;
    if (*p != '"') {
        fprintf(stderr, "[%d] panel: unterminated body string\n", line);
        return false;
    }
    char body[256];
    size_t body_len = (size_t)(p - body_start);
    if (body_len >= sizeof(body))
        body_len = sizeof(body) - 1;
    memcpy(body, body_start, body_len);
    body[body_len] = '\0';

    int level = 0;
    unsigned int flags = 0;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p) {
        char *next = NULL;
        level = (int)strtol(p, &next, 10);
        if (next && *next) {
            while (*next && isspace((unsigned char)*next))
                next++;
            if (*next)
                flags = (unsigned int)strtoul(next, NULL, 10);
        }
    }

    char *title_x = cfr_script_expand_escapes(title);
    char *body_x = cfr_script_expand_escapes(body);
    ext->panel_fn(ext->panel_ud, parsed[0], parsed[1], parsed[2],
                  parsed[3], parsed[4], title_x ? title_x : title,
                  body_x ? body_x : body, level, flags);
    free(title_x);
    free(body_x);
    return true;
}

/* ── Extension dispatch ── */

static bool exec_ext(void *ud, const char *name, const char *args,
                     int line, bool *fatal)
{
    ScriptUIExt *ext = ud;
    *fatal = false;

    /* Work on a writable copy; the engine's arg string is const. */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", args ? args : "");

    if (strcmp(name, "screendump") == 0) {
        if (!ext->pending_screendump || !ext->screendump_path_buf) {
            fprintf(stderr, "[%d] screendump: not supported\n", line);
            return true;
        }
        strip_quotes(buf);
        snprintf(ext->screendump_path_buf, 512, "%s", buf);
        *ext->pending_screendump = true;
        return true;
    }

    if (strcmp(name, "renderdump") == 0) {
        if (!ext->pending_renderdump || !ext->renderdump_path_buf) {
            fprintf(stderr, "[%d] renderdump: not supported\n", line);
            return true;
        }
        strip_quotes(buf);
        snprintf(ext->renderdump_path_buf, 512, "%s", buf);
        *ext->pending_renderdump = true;
        return true;
    }

    if (strcmp(name, "dumpstate") == 0) {
        if (!ext->dump_fn) {
            fprintf(stderr, "[%d] dumpstate: not supported\n", line);
            return true;
        }
        strip_quotes(buf);
        const char *path = buf[0] ? buf : NULL;
        if (!ext->dump_fn(ext->dump_ud, path)) {
            fprintf(stderr, "[%d] dumpstate: failed to write state\n", line);
            *fatal = true;
            return false;
        }
        return true;
    }

    if (strcmp(name, "record-start") == 0) {
        if (!ext->recorder) {
            fprintf(stderr, "[%d] record-start: not supported\n", line);
            return true;
        }
        int fps = 30;
        char *dir = strtok(buf, " \t");
        if (!dir) {
            fprintf(stderr, "[%d] record-start requires directory\n", line);
            *fatal = true;
            return false;
        }
        strip_quotes(dir);
        char *fps_str = strtok(NULL, " \t");
        if (fps_str)
            fps = atoi(fps_str);
        frame_recorder_start(ext->recorder, dir, fps);
        if (ext->record_start_fn)
            ext->record_start_fn(ext->record_ud,
                                 ext->recorder->target_fps);
        return true;
    }

    if (strcmp(name, "record-stop") == 0) {
        if (!ext->recorder) {
            fprintf(stderr, "[%d] record-stop: not supported\n", line);
            return true;
        }
        if (ext->record_stop_fn)
            ext->record_stop_fn(ext->record_ud);
        frame_recorder_stop(ext->recorder);
        return true;
    }

    if (strcmp(name, "panel") == 0)
        return exec_panel(ext, buf, line);

    if (strcmp(name, "panel_hide") == 0) {
        if (!ext->panel_hide_fn) {
            fprintf(stderr, "[%d] panel_hide: not supported\n", line);
            return true;
        }
        ext->panel_hide_fn(ext->panel_hide_ud, atoi(buf));
        return true;
    }

    if (strcmp(name, "mousemove") == 0) {
        int x = 0, y = 0;
        if (sscanf(buf, "%d %d", &x, &y) != 2) {
            fprintf(stderr, "[%d] mousemove: requires x y\n", line);
            *fatal = true;
            return false;
        }
        if (!ext->mousemove_fn) {
            fprintf(stderr, "[%d] mousemove: not supported\n", line);
            return true;
        }
        ext->mousemove_fn(ext->mousemove_ud, x, y);
        return true;
    }

    if (strcmp(name, "mousedown") == 0 || strcmp(name, "mouseup") == 0) {
        int x = 0, y = 0, button = 1, clicks = 1;
        if (sscanf(buf, "%d %d %d %d", &x, &y, &button, &clicks) < 2) {
            fprintf(stderr, "[%d] %s: requires x y [button] [clicks]\n",
                    line, name);
            *fatal = true;
            return false;
        }
        if (button <= 0)
            button = 1;
        if (clicks <= 0)
            clicks = 1;
        if (!ext->mouse_button_fn) {
            fprintf(stderr, "[%d] %s: not supported\n", line, name);
            return true;
        }
        ext->mouse_button_fn(ext->mouse_button_ud, x, y, button,
                             strcmp(name, "mousedown") == 0, clicks);
        return true;
    }

    if (strcmp(name, "winsize") == 0) {
        int w = 0, h = 0;
        if (sscanf(buf, "%d %d", &w, &h) != 2) {
            fprintf(stderr, "[%d] winsize: requires width height\n", line);
            *fatal = true;
            return false;
        }
        if (!ext->winsize_fn) {
            fprintf(stderr, "[%d] winsize: not supported\n", line);
            return true;
        }
        ext->winsize_fn(ext->winsize_ud, w, h);
        return true;
    }

    if (strcmp(name, "display-scale") == 0) {
        float scale = 0.0f;
        if (sscanf(buf, "%f", &scale) != 1 || scale <= 0.0f) {
            fprintf(stderr, "[%d] display-scale: requires a positive scale\n",
                    line);
            *fatal = true;
            return false;
        }
        if (!ext->display_scale_fn) {
            fprintf(stderr, "[%d] display-scale: not supported\n", line);
            return true;
        }
        ext->display_scale_fn(ext->display_scale_ud, scale);
        return true;
    }

    if (strcmp(name, "assert-hover") == 0 ||
        strcmp(name, "assert-no-hover") == 0) {
        bool want_hover = name[7] != 'n'; /* "assert-hover" vs "no-hover" */
        if (!ext->term) {
            fprintf(stderr, "[%d] %s: no terminal\n", line, name);
            *fatal = true;
            return false;
        }
        uint16_t hid = terminal_hovered_hyperlink(ext->term);
        bool hovered = hid != 0;
        if (hovered == want_hover) {
            fprintf(stderr, "[%d] %s: PASS (hid=%u)\n", line, name, hid);
            return true;
        }
        fprintf(stderr, "[%d] %s: FAIL (hid=%u)\n", line, name, hid);
        *fatal = true;
        return false;
    }

    fprintf(stderr, "[%d] unhandled extension command: %s\n", line, name);
    *fatal = true;
    return false;
}

/* ── Public API ── */

PorttyScript *portty_script_load(const char *path, ScriptUIExt *ext,
                                 char *err, size_t errlen)
{
    CfrScript *script = cfr_script_load(path, err, errlen, EXT_NAMES);
    if (!script)
        return NULL;

    CfrScriptHost host = {
        .term = ext->cfr_term,
        .pty_write = host_pty_write,
        .emit = host_emit,
        .poll_exit = host_poll_exit,
        .resize = host_resize,
        .exec_ext = exec_ext,
        .ud = ext,
    };
    CfrScriptRun *run = cfr_script_run_new(script, &host);
    if (!run) {
        cfr_script_free(script);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }

    PorttyScript *s = calloc(1, sizeof(*s));
    if (!s) {
        cfr_script_run_free(run);
        cfr_script_free(script);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    s->script = script;
    s->run = run;
    return s;
}

void portty_script_free(PorttyScript *s)
{
    if (!s)
        return;
    cfr_script_run_free(s->run);
    cfr_script_free(s->script);
    free(s);
}

CfrScriptStatus portty_script_step(PorttyScript *s)
{
    if (!s)
        return CFR_SCRIPT_FAILED;
    return cfr_script_step(s->run);
}

uint32_t portty_script_wait_ms(PorttyScript *s)
{
    if (!s)
        return UINT32_MAX;
    return cfr_script_wait_ms(s->run);
}

bool portty_script_quit_requested(const PorttyScript *s)
{
    return s && cfr_script_run_quit_requested(s->run);
}
