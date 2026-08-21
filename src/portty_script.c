/*
 * portty — Debug/automation script parser and execution
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_script.h"
#include "common.h"
#include "rend_common.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* ── PorttyScript struct ── */

struct PorttyScript
{
    ScriptCmd *cmds;
    int count;
    int capacity;
    char error[256];
    bool has_error;
    int error_line; // Line number where error occurred (0 if no error)
};

/* ── Helpers ── */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return 0;
}

static void script_set_error(PorttyScript *s, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s->error, sizeof(s->error), fmt, args);
    va_end(args);
    s->has_error = true;
    s->error_line = line;
}

static bool script_ensure_capacity(PorttyScript *s)
{
    if (s->count >= s->capacity) {
        int new_cap = s->capacity == 0 ? 16 : s->capacity * 2;
        ScriptCmd *new_cmds = realloc(s->cmds, (size_t)new_cap * sizeof(ScriptCmd));
        if (!new_cmds)
            return false;
        s->cmds = new_cmds;
        s->capacity = new_cap;
    }
    return true;
}

/* Strip trailing whitespace from a string in-place */
static void strip_trailing(char *str)
{
    int len = (int)strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1]))
        str[--len] = '\0';
}

/* Skip leading whitespace, return pointer to first non-ws char */
static char *skip_ws(char *str)
{
    while (*str && isspace((unsigned char)*str))
        str++;
    return str;
}

/* Expand backslash escapes (\n \r \t \e \\) in place.
 * Returns the new length (may be shorter than input). */
static int expand_escapes(char *str, int len)
{
    int out = 0;
    int in = 0;
    while (in < len) {
        if (str[in] == '\\' && in + 1 < len) {
            switch (str[in + 1]) {
            case 'n':
                str[out++] = '\n';
                in += 2;
                break;
            case 'r':
                str[out++] = '\r';
                in += 2;
                break;
            case 't':
                str[out++] = '\t';
                in += 2;
                break;
            case 'e':
                str[out++] = '\x1b';
                in += 2;
                break;
            case 'x':
            {
                /* \xNN - exactly two hex digits */
                if (in + 3 < len && isxdigit((unsigned char)str[in + 2]) &&
                    isxdigit((unsigned char)str[in + 3])) {
                    int hi = hex_val(str[in + 2]);
                    int lo = hex_val(str[in + 3]);
                    str[out++] = (char)((hi << 4) | lo);
                    in += 4;
                } else {
                    /* malformed: keep backslash and continue */
                    str[out++] = str[in++];
                }
                break;
            }
            case '\\':
                str[out++] = '\\';
                in += 2;
                break;
            default:
                str[out++] = str[in++];
                break;
            }
        } else {
            str[out++] = str[in++];
        }
    }
    str[out] = '\0';
    return out;
}

/* Parse hex byte string into a binary buffer.
 * Returns malloc'd buffer (caller frees) or NULL on error.
 * Sets *out_len to the number of bytes. */
static char *parse_hex_bytes(const char *args, int *out_len)
{
    /* Count tokens to estimate buffer size */
    int count = 0;
    const char *p = args;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p)
            count++;
        while (*p && !isspace((unsigned char)*p))
            p++;
    }

    if (count == 0) {
        *out_len = 0;
        return calloc(1, 1); /* empty string */
    }

    char *buf = malloc((size_t)count + 1);
    if (!buf)
        return NULL;

    int idx = 0;
    p = args;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;

        /* Parse two hex digits */
        int hi = -1, lo = -1;
        if (isxdigit((unsigned char)p[0])) {
            hi = (p[0] >= '0' && p[0] <= '9') ? p[0] - '0' : (p[0] >= 'a' && p[0] <= 'f') ? p[0] - 'a' + 10
                                                         : (p[0] >= 'A' && p[0] <= 'F')   ? p[0] - 'A' + 10
                                                                                          : -1;
        }
        if (hi >= 0 && isxdigit((unsigned char)p[1])) {
            lo = (p[1] >= '0' && p[1] <= '9') ? p[1] - '0' : (p[1] >= 'a' && p[1] <= 'f') ? p[1] - 'a' + 10
                                                         : (p[1] >= 'A' && p[1] <= 'F')   ? p[1] - 'A' + 10
                                                                                          : -1;
        }

        if (hi < 0 || lo < 0) {
            free(buf);
            return NULL;
        }

        buf[idx++] = (char)((hi << 4) | lo);
        p += 2;
        /* Skip any trailing non-space chars (shouldn't happen for valid hex) */
    }

    buf[idx] = '\0';
    *out_len = idx;
    return buf;
}

/* Strip surrounding double quotes from a string in-place */
static void strip_quotes(char *str)
{
    int len = (int)strlen(str);
    if (len >= 2 && str[0] == '"' && str[len - 1] == '"') {
        memmove(str, str + 1, (size_t)(len - 2));
        str[len - 2] = '\0';
    }
}

/* ── Parser ── */

static ScriptCmd *script_new_cmd(PorttyScript *s)
{
    if (!script_ensure_capacity(s))
        return NULL;
    ScriptCmd *cmd = &s->cmds[s->count++];
    memset(cmd, 0, sizeof(ScriptCmd));
    cmd->col_start = 0;
    cmd->col_end = 0;
    cmd->row = 0;
    cmd->wait_seconds = 0;
    cmd->text = NULL;
    cmd->path[0] = '\0';
    return cmd;
}

static bool parse_command(PorttyScript *s, char *line, int line_num)
{
    /* Skip leading whitespace */
    line = skip_ws(line);
    if (*line == '\0' || *line == '#')
        return true; /* blank or comment */

    /* Split on first whitespace into keyword + args */
    char *space = line;
    while (*space && !isspace((unsigned char)*space))
        space++;
    char *args = NULL;
    if (*space) {
        *space = '\0';
        args = skip_ws(space + 1);
    } else {
        args = line + strlen(line); /* points to '\0' */
    }

    strip_trailing(args);

    if (strcmp(line, "wait") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_WAIT;
        cmd->wait_seconds = atof(args);
        return true;
    }

    if (strcmp(line, "send") == 0 || strcmp(line, "sendln") == 0 ||
        strcmp(line, "emit") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        if (strcmp(line, "emit") == 0) {
            cmd->type = SCRIPT_CMD_EMIT;
        } else if (strcmp(line, "sendln") == 0) {
            cmd->type = SCRIPT_CMD_SENDLN;
        } else {
            cmd->type = SCRIPT_CMD_SEND;
        }
        strip_quotes(args);
        int len = (int)strlen(args);
        len = expand_escapes(args, len);
        cmd->text = malloc((size_t)len + 1);
        if (!cmd->text)
            return false;
        memcpy(cmd->text, args, (size_t)len + 1);
        return true;
    }

    if (strcmp(line, "raw") == 0 || strcmp(line, "emit-raw") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = (strcmp(line, "emit-raw") == 0) ? SCRIPT_CMD_EMIT_RAW : SCRIPT_CMD_RAW;
        int raw_len = 0;
        char *raw = parse_hex_bytes(args, &raw_len);
        if (!raw) {
            script_set_error(s, line_num, "invalid hex bytes in raw command: %s", args);
            return false;
        }
        cmd->text = raw;
        /* Store length in col_start as a hack? No — text is NUL-terminated
         * but may contain NULs. We store the length separately by using
         * a convention: for RAW, text is a malloc'd buffer and we store
         * the length as the buffer size. But ScriptCmd doesn't have a len
         * field. For now, raw bytes that are NUL will truncate at NUL
         * when used as a C string. This is acceptable for debug scripts
         * where raw is typically escape sequences (no NULs). */
        return true;
    }

    if (strcmp(line, "assert-contains") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_ASSERT_CONTAINS;
        strip_quotes(args);
        cmd->text = strdup(args);
        if (!cmd->text)
            return false;
        return true;
    }

    if (strcmp(line, "assert-not-contains") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_ASSERT_NOT_CONTAINS;
        strip_quotes(args);
        cmd->text = strdup(args);
        if (!cmd->text)
            return false;
        return true;
    }

    if (strcmp(line, "screendump") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_SCREENDUMP;
        strip_quotes(args);
        snprintf(cmd->path, sizeof(cmd->path), "%s", args);
        return true;
    }

    if (strcmp(line, "dumprow") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_DUMPROW;
        cmd->col_start = -1; /* all columns */
        cmd->col_end = 0;
        if (sscanf(args, "%d", &cmd->row) != 1) {
            script_set_error(s, line_num, "dumprow: missing or invalid row argument");
            return false;
        }
        return true;
    }

    if (strcmp(line, "dumpcells") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_DUMPCELLS;
        if (sscanf(args, "%d %d %d", &cmd->row, &cmd->col_start, &cmd->col_end) != 3) {
            script_set_error(s, line_num, "dumpcells: requires row col_start col_end");
            return false;
        }
        return true;
    }

    if (strcmp(line, "panel") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_PANEL;
        cmd->panel_level = 0; /* default */
        /* Format: panel <id> <col> <row> <cols> <rows> "title" "body" [level] */
        int pos_count = 0;
        char *p = args;
        int parsed[5];
        while (pos_count < 5 && *p) {
            while (*p && isspace(*p))
                p++;
            if (!*p)
                break;
            char *end;
            parsed[pos_count] = (int)strtol(p, &end, 10);
            if (end == p)
                break;
            pos_count++;
            p = end;
        }
        if (pos_count < 5) {
            script_set_error(s, line_num, "panel: requires id col row cols rows");
            return false;
        }
        cmd->panel_id = parsed[0];
        cmd->panel_col = parsed[1];
        cmd->panel_row = parsed[2];
        cmd->panel_cols = parsed[3];
        cmd->panel_rows = parsed[4];
        /* Skip to first quote */
        while (*p && *p != '"')
            p++;
        if (*p != '"') {
            script_set_error(s, line_num, "panel: requires quoted title and body");
            return false;
        }
        p++; /* skip opening quote */
        char *title_start = p;
        while (*p && !(*p == '"' && (p == title_start || *(p - 1) != '\\')))
            p++;
        if (*p != '"') {
            script_set_error(s, line_num, "panel: unterminated title string");
            return false;
        }
        size_t title_len = (size_t)(p - title_start);
        if (title_len >= sizeof(cmd->panel_title))
            title_len = sizeof(cmd->panel_title) - 1;
        memcpy(cmd->panel_title, title_start, title_len);
        cmd->panel_title[title_len] = '\0';
        expand_escapes(cmd->panel_title, (int)strlen(cmd->panel_title));
        p++; /* skip closing quote */
        while (*p && isspace(*p))
            p++;
        if (*p != '"') {
            script_set_error(s, line_num, "panel: requires quoted body string");
            return false;
        }
        p++; /* skip opening quote of body */
        char *body_start = p;
        while (*p && !(*p == '"' && (p == body_start || *(p - 1) != '\\')))
            p++;
        if (*p != '"') {
            script_set_error(s, line_num, "panel: unterminated body string");
            return false;
        }
        size_t body_len = (size_t)(p - body_start);
        if (body_len >= sizeof(cmd->panel_body))
            body_len = sizeof(cmd->panel_body) - 1;
        memcpy(cmd->panel_body, body_start, body_len);
        cmd->panel_body[body_len] = '\0';
        expand_escapes(cmd->panel_body, (int)strlen(cmd->panel_body));

        p++; /* skip closing quote */
        while (*p && isspace(*p))
            p++;
        if (*p) {
            char *next = NULL;
            cmd->panel_level = (int)strtol(p, &next, 10);
            /* Check for optional flags parameter */
            if (next && *next) {
                while (*next && isspace(*next))
                    next++;
                if (*next) {
                    cmd->panel_flags = (unsigned int)strtoul(next, NULL, 10);
                }
            }
        }
        return true;
    }

    if (strcmp(line, "panel_hide") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_PANEL_HIDE;
        if (sscanf(args, "%d", &cmd->panel_id) != 1) {
            script_set_error(s, line_num, "panel_hide: requires id");
            return false;
        }
        return true;
    }

    if (strcmp(line, "mousemove") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_MOUSEMOVE;
        if (sscanf(args, "%d %d", &cmd->mouse_x, &cmd->mouse_y) != 2) {
            script_set_error(s, line_num, "mousemove: requires x y");
            return false;
        }
        return true;
    }

    if (strcmp(line, "resize") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_RESIZE;
        if (sscanf(args, "%d %d", &cmd->resize_cols, &cmd->resize_rows) != 2) {
            script_set_error(s, line_num, "resize: requires cols rows");
            return false;
        }
        return true;
    }

    if (strcmp(line, "winsize") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_WINSIZE;
        if (sscanf(args, "%d %d", &cmd->winsize_w, &cmd->winsize_h) != 2) {
            script_set_error(s, line_num, "winsize: requires width height");
            return false;
        }
        return true;
    }

    if (strcmp(line, "record-start") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_RECORD_START;
        cmd->record_fps = 30;
        char *dir = strtok(args, " \t\n");
        if (!dir) {
            script_set_error(s, line_num, "record-start requires directory");
            return false;
        }
        strip_quotes(dir);
        snprintf(cmd->record_dir, sizeof(cmd->record_dir), "%s", dir);
        char *fps_str = strtok(NULL, " \t\n");
        if (fps_str)
            cmd->record_fps = atoi(fps_str);
        return true;
    }

    if (strcmp(line, "record-stop") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_RECORD_STOP;
        return true;
    }

    if (strcmp(line, "quit") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_QUIT;
        return true;
    }

    if (strcmp(line, "assert-hover") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_ASSERT_HOVER;
        return true;
    }

    if (strcmp(line, "assert-no-hover") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_ASSERT_NO_HOVER;
        return true;
    }

    if (strcmp(line, "dump-sixel") == 0) {
        ScriptCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = SCRIPT_CMD_DUMP_SIXEL;
        return true;
    }

    script_set_error(s, line_num, "unknown command: %s", line);
    return false;
}

/* ── Public API ── */

PorttyScript *portty_script_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        // Create a struct to hold the file open error
        PorttyScript *s = calloc(1, sizeof(PorttyScript));
        if (s)
            script_set_error(s, 0, "cannot open file: %s", strerror(errno));
        return s;
    }

    PorttyScript *s = calloc(1, sizeof(PorttyScript));
    if (!s) {
        fclose(fp);
        return NULL;
    }

    char line[4096];
    int line_num = 0;
    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        /* Remove trailing newline */
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (!parse_command(s, line, line_num)) {
            /* parse_command set error; return struct with error */
            fclose(fp);
            return s;
        }
    }

    fclose(fp);
    return s;
}

void portty_script_free(PorttyScript *s)
{
    if (!s)
        return;
    if (s->cmds) {
        for (int i = 0; i < s->count; i++) {
            free(s->cmds[i].text);
        }
        free(s->cmds);
    }
    free(s);
}

int portty_script_count(const PorttyScript *s)
{
    return s ? s->count : 0;
}

const ScriptCmd *portty_script_get(const PorttyScript *s, int index)
{
    if (!s || index < 0 || index >= s->count)
        return NULL;
    return &s->cmds[index];
}

const char *portty_script_error(const PorttyScript *s)
{
    if (!s || !s->has_error)
        return NULL;

    // If error has a line number, prepend it
    static char full_error[320];
    if (s->error_line > 0)
        snprintf(full_error, sizeof(full_error), "line %d: %s", s->error_line, s->error);
    else
        snprintf(full_error, sizeof(full_error), "%s", s->error);

    return full_error;
}

/* ── Shared execution helpers ── */

double portty_now_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

bool portty_debug_grid_contains(TerminalBackend *term, int rows, int cols,
                                const char *needle)
{
    if (!term || !needle || needle[0] == '\0')
        return false;

    int needle_len = (int)strlen(needle);

    for (int row = 0; row < rows; row++) {
        /* Build the row text by reading each cell's codepoint */
        char *row_text = malloc((size_t)cols * 4 + 1);
        if (!row_text)
            return false;
        int pos = 0;
        for (int col = 0; col < cols; col++) {
            TerminalCell cell;
            if (terminal_get_cell(term, row, col, &cell) != 0)
                break;
            if (cell.cp == 0 || cell.cp == 0x20) {
                row_text[pos++] = ' ';
            } else if (cell.width == 0) {
                /* continuation cell, skip */
            } else {
                /* Encode codepoint as UTF-8 */
                uint32_t cp = cell.cp;
                if (cp < 0x80) {
                    row_text[pos++] = (char)cp;
                } else if (cp < 0x800) {
                    row_text[pos++] = (char)(0xC0 | (cp >> 6));
                    row_text[pos++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    row_text[pos++] = (char)(0xE0 | (cp >> 12));
                    row_text[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    row_text[pos++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    row_text[pos++] = (char)(0xF0 | (cp >> 18));
                    row_text[pos++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    row_text[pos++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    row_text[pos++] = (char)(0x80 | (cp & 0x3F));
                }
            }
        }
        row_text[pos] = '\0';

        /* Search for needle in row_text */
        if (pos >= needle_len && strstr(row_text, needle) != NULL) {
            free(row_text);
            return true;
        }
        free(row_text);
    }

    return false;
}

/* Execute dumpcells/dumprow — shared between backends */
static void execute_dumpcells(TerminalBackend *term, int scroll_offset,
                              int row, int col_start, int col_end)
{
    int unified_row = rend_display_row_to_unified(scroll_offset, row);

    printf("=== dumpcells row=%d (unified=%d) cols %d-%d ===\n",
           row, unified_row, col_start, col_end);

    for (int col = col_start; col <= col_end; col++) {
        TerminalCell cell;
        int ret;
        if (unified_row < 0) {
            ret = terminal_get_scrollback_cell(term, -unified_row - 1, col, &cell);
        } else {
            ret = terminal_get_cell(term, unified_row, col, &cell);
        }
        if (ret != 0)
            continue;

        printf("  col=%3d cp=%04X w=%d rev=%d invis=%d dim=%d "
               "fg(%s %02X%02X%02X) "
               "bg(%s %02X%02X%02X) "
               "ul=%d ul_color(%s %02X%02X%02X) "
               "hid=%u\n",
               col, cell.cp, cell.width, cell.attrs.reverse,
               cell.attrs.invis, cell.attrs.dim,
               cell.fg.is_default ? "def" : "set",
               cell.fg.r, cell.fg.g, cell.fg.b,
               cell.bg.is_default ? "def" : "set",
               cell.bg.r, cell.bg.g, cell.bg.b,
               cell.attrs.underline,
               cell.ul_color.is_default ? "def" : "set",
               cell.ul_color.r, cell.ul_color.g, cell.ul_color.b,
               cell.hyperlink_id);
    }
    printf("=== end dumpcells ===\n");
}

/* ── Script step execution ── */

/* Static-local wait deadline (safe because only one script runs at a time) */
static double s_wait_deadline = 0;
static bool s_wait_deadline_valid = false;

static double script_wait_deadline_for(const ScriptCmd *cmd)
{
    if (!s_wait_deadline_valid) {
        s_wait_deadline = portty_now_seconds() + cmd->wait_seconds;
        s_wait_deadline_valid = true;
    }
    return s_wait_deadline;
}

uint32_t portty_script_wait_remaining_ms(const PorttyScript *script,
                                         int cmd_index)
{
    if (!script || cmd_index < 0 || cmd_index >= script->count)
        return UINT32_MAX;

    const ScriptCmd *cmd = &script->cmds[cmd_index];
    if (cmd->type != SCRIPT_CMD_WAIT)
        return UINT32_MAX;

    double deadline = script_wait_deadline_for(cmd);
    double remaining = deadline - portty_now_seconds();
    if (remaining <= 0)
        return 0;

    double ms = remaining * 1000.0;
    uint32_t rounded = (uint32_t)ms;
    if ((double)rounded < ms)
        rounded++; // ceil so we never wake before the deadline
    if (rounded == 0)
        rounded = 1;

    return rounded;
}

void portty_script_step(PorttyScript *script,
                        int *cmd_index,
                        ScriptExecCtx *ctx)
{
    if (!script || !cmd_index || !ctx)
        return;

    if (*cmd_index >= script->count)
        return;

    const ScriptCmd *cmd = &script->cmds[*cmd_index];

    switch (cmd->type) {
    case SCRIPT_CMD_WAIT:
    {
        double deadline = script_wait_deadline_for(cmd);
        if (portty_now_seconds() >= deadline) {
            s_wait_deadline_valid = false;
            (*cmd_index)++;
        }
        break;
    }

    case SCRIPT_CMD_SEND:
    {
        if (ctx->pty && cmd->text)
            pty_write(ctx->pty, cmd->text, strlen(cmd->text));
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_SENDLN:
    {
        if (ctx->pty && cmd->text) {
            pty_write(ctx->pty, cmd->text, strlen(cmd->text));
            pty_write(ctx->pty, "\r\n", 2);
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_RAW:
    {
        if (ctx->pty && cmd->text)
            pty_write(ctx->pty, cmd->text, strlen(cmd->text));
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_EMIT:
    {
        if (ctx->emit_fn && cmd->text) {
            ctx->emit_fn(ctx->emit_user_data, cmd->text, strlen(cmd->text));
        } else {
            fprintf(stderr, "emit: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_EMIT_RAW:
    {
        if (ctx->emit_fn && cmd->text) {
            ctx->emit_fn(ctx->emit_user_data, cmd->text, strlen(cmd->text));
        } else {
            fprintf(stderr, "emit-raw: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_ASSERT_CONTAINS:
    {
        int rows = 0, cols = 0;
        if (ctx->term)
            terminal_get_dimensions(ctx->term, &rows, &cols);
        bool found = portty_debug_grid_contains(ctx->term, rows, cols,
                                                cmd->text ? cmd->text : "");
        if (found) {
            printf("assert-contains \"%s\": PASS\n",
                   cmd->text ? cmd->text : "");
        } else {
            printf("assert-contains \"%s\": FAIL\n",
                   cmd->text ? cmd->text : "");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_ASSERT_NOT_CONTAINS:
    {
        int rows = 0, cols = 0;
        if (ctx->term)
            terminal_get_dimensions(ctx->term, &rows, &cols);
        bool found = portty_debug_grid_contains(ctx->term, rows, cols,
                                                cmd->text ? cmd->text : "");
        if (!found) {
            printf("assert-not-contains \"%s\": PASS\n",
                   cmd->text ? cmd->text : "");
        } else {
            printf("assert-not-contains \"%s\": FAIL\n",
                   cmd->text ? cmd->text : "");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_SCREENDUMP:
    {
        if (ctx->pending_screendump && ctx->screendump_path_buf) {
            *ctx->pending_screendump = true;
            snprintf(ctx->screendump_path_buf, 512, "%s", cmd->path);
        } else {
            fprintf(stderr, "screendump: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_DUMPROW:
    {
        int rows = 0, cols = 0;
        if (ctx->term)
            terminal_get_dimensions(ctx->term, &rows, &cols);
        execute_dumpcells(ctx->term, ctx->scroll_offset,
                          cmd->row, 0, cols - 1);
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_DUMPCELLS:
    {
        execute_dumpcells(ctx->term, ctx->scroll_offset,
                          cmd->row, cmd->col_start, cmd->col_end);
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_PANEL:
    {
        if (ctx->panel_fn) {
            ctx->panel_fn(ctx->panel_user_data,
                          cmd->panel_id,
                          cmd->panel_col,
                          cmd->panel_row,
                          cmd->panel_cols,
                          cmd->panel_rows,
                          cmd->panel_title,
                          cmd->panel_body,
                          cmd->panel_level,
                          cmd->panel_flags);
        } else {
            fprintf(stderr, "panel: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_PANEL_HIDE:
    {
        if (ctx->panel_hide_fn) {
            ctx->panel_hide_fn(ctx->panel_hide_user_data, cmd->panel_id);
        } else {
            fprintf(stderr, "panel_hide: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_MOUSEMOVE:
    {
        if (ctx->mousemove_fn) {
            ctx->mousemove_fn(ctx->mousemove_user_data,
                              cmd->mouse_x, cmd->mouse_y);
        } else {
            fprintf(stderr, "mousemove: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_RESIZE:
    {
        if (ctx->resize_fn) {
            ctx->resize_fn(ctx->resize_user_data,
                           cmd->resize_cols, cmd->resize_rows);
        } else {
            fprintf(stderr, "resize: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_WINSIZE:
    {
        if (ctx->winsize_fn) {
            ctx->winsize_fn(ctx->winsize_user_data,
                            cmd->winsize_w, cmd->winsize_h);
        } else {
            fprintf(stderr, "winsize: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_RECORD_START:
    {
        if (ctx->recorder) {
            frame_recorder_start(ctx->recorder, cmd->record_dir, cmd->record_fps);
            if (ctx->record_start_fn)
                ctx->record_start_fn(ctx->record_user_data,
                                     ctx->recorder->target_fps);
        } else {
            fprintf(stderr, "record-start: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_RECORD_STOP:
    {
        if (ctx->recorder) {
            if (ctx->record_stop_fn)
                ctx->record_stop_fn(ctx->record_user_data);
            frame_recorder_stop(ctx->recorder);
        } else {
            fprintf(stderr, "record-stop: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_QUIT:
    {
        if (ctx->backend && ctx->backend->request_quit)
            ctx->backend->request_quit(ctx->backend);
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_ASSERT_HOVER:
    {
        if (ctx->term) {
            uint16_t hid = terminal_hovered_hyperlink(ctx->term);
            if (hid != 0) {
                printf("assert-hover: PASS (hid=%u)\n", hid);
            } else {
                printf("assert-hover: FAIL (no hover)\n");
            }
        } else {
            fprintf(stderr, "assert-hover: no terminal\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_ASSERT_NO_HOVER:
    {
        if (ctx->term) {
            uint16_t hid = terminal_hovered_hyperlink(ctx->term);
            if (hid == 0) {
                printf("assert-no-hover: PASS\n");
            } else {
                printf("assert-no-hover: FAIL (hid=%u)\n", hid);
            }
        } else {
            fprintf(stderr, "assert-no-hover: no terminal\n");
        }
        (*cmd_index)++;
        break;
    }

    case SCRIPT_CMD_DUMP_SIXEL:
    {
        if (ctx->term) {
            int count = 0;
            const CfrSixel *imgs = terminal_get_sixels(ctx->term, &count);
            vlog("dump-sixel: %d image(s)\n", count);
            for (int i = 0; i < count; i++) {
                vlog("  [%d] id=%llu row=%d col=%d %dx%dpx\n",
                     i, (unsigned long long)imgs[i].id,
                     imgs[i].row, imgs[i].col,
                     imgs[i].width_px, imgs[i].height_px);
            }
        } else {
            vlog("dump-sixel: no terminal\n");
        }
        (*cmd_index)++;
        break;
    }
    }
}
