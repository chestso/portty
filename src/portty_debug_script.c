#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_debug_script.h"
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

/* ── PorttyDebugScript struct ── */

struct PorttyDebugScript
{
    DebugCmd *cmds;
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

static void script_set_error(PorttyDebugScript *s, int line, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s->error, sizeof(s->error), fmt, args);
    va_end(args);
    s->has_error = true;
    s->error_line = line;
}

static bool script_ensure_capacity(PorttyDebugScript *s)
{
    if (s->count >= s->capacity) {
        int new_cap = s->capacity == 0 ? 16 : s->capacity * 2;
        DebugCmd *new_cmds = realloc(s->cmds, (size_t)new_cap * sizeof(DebugCmd));
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

static DebugCmd *script_new_cmd(PorttyDebugScript *s)
{
    if (!script_ensure_capacity(s))
        return NULL;
    DebugCmd *cmd = &s->cmds[s->count++];
    memset(cmd, 0, sizeof(DebugCmd));
    cmd->col_start = 0;
    cmd->col_end = 0;
    cmd->row = 0;
    cmd->wait_seconds = 0;
    cmd->text = NULL;
    cmd->path[0] = '\0';
    return cmd;
}

static bool parse_command(PorttyDebugScript *s, char *line, int line_num)
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
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_WAIT;
        cmd->wait_seconds = atof(args);
        return true;
    }

    if (strcmp(line, "send") == 0 || strcmp(line, "sendln") == 0 ||
        strcmp(line, "emit") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        if (strcmp(line, "emit") == 0) {
            cmd->type = DBG_CMD_EMIT;
        } else if (strcmp(line, "sendln") == 0) {
            cmd->type = DBG_CMD_SENDLN;
        } else {
            cmd->type = DBG_CMD_SEND;
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
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = (strcmp(line, "emit-raw") == 0) ? DBG_CMD_EMIT_RAW : DBG_CMD_RAW;
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
         * the length as the buffer size. But DebugCmd doesn't have a len
         * field. For now, raw bytes that are NUL will truncate at NUL
         * when used as a C string. This is acceptable for debug scripts
         * where raw is typically escape sequences (no NULs). */
        return true;
    }

    if (strcmp(line, "assert-contains") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_ASSERT_CONTAINS;
        strip_quotes(args);
        cmd->text = strdup(args);
        if (!cmd->text)
            return false;
        return true;
    }

    if (strcmp(line, "assert-not-contains") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_ASSERT_NOT_CONTAINS;
        strip_quotes(args);
        cmd->text = strdup(args);
        if (!cmd->text)
            return false;
        return true;
    }

    if (strcmp(line, "screendump") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_SCREENDUMP;
        strip_quotes(args);
        snprintf(cmd->path, sizeof(cmd->path), "%s", args);
        return true;
    }

    if (strcmp(line, "dumprow") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_DUMPROW;
        cmd->col_start = -1; /* all columns */
        cmd->col_end = 0;
        if (sscanf(args, "%d", &cmd->row) != 1) {
            script_set_error(s, line_num, "dumprow: missing or invalid row argument");
            return false;
        }
        return true;
    }

    if (strcmp(line, "dumpcells") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_DUMPCELLS;
        if (sscanf(args, "%d %d %d", &cmd->row, &cmd->col_start, &cmd->col_end) != 3) {
            script_set_error(s, line_num, "dumpcells: requires row col_start col_end");
            return false;
        }
        return true;
    }

    if (strcmp(line, "dumpverts") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_DUMPVERTS;
        if (sscanf(args, "%d %d %d", &cmd->row, &cmd->col_start, &cmd->col_end) != 3) {
            script_set_error(s, line_num, "dumpverts: requires row col_start col_end");
            return false;
        }
        return true;
    }

    if (strcmp(line, "verifybuf") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_VERIFYBUF;
        if (sscanf(args, "%d %d %d", &cmd->row, &cmd->col_start, &cmd->col_end) != 3) {
            script_set_error(s, line_num, "verifybuf: requires row col_start col_end");
            return false;
        }
        return true;
    }

    if (strcmp(line, "notify") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_NOTIFY;
        strip_quotes(args);
        char *sep = strstr(args, "\" \"");
        if (sep) {
            *sep = '\0';
            snprintf(cmd->notify_title, sizeof(cmd->notify_title), "%s", args + 1);
            snprintf(cmd->notify_body, sizeof(cmd->notify_body), "%s", sep + 3);
            cmd->notify_title[strlen(cmd->notify_title) - 1] = '\0';
            cmd->notify_body[strlen(cmd->notify_body) - 1] = '\0';
        } else {
            snprintf(cmd->notify_title, sizeof(cmd->notify_title), "%s", args);
            cmd->notify_body[0] = '\0';
        }
        return true;
    }

    if (strcmp(line, "mousemove") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_MOUSEMOVE;
        if (sscanf(args, "%d %d", &cmd->mouse_x, &cmd->mouse_y) != 2) {
            script_set_error(s, line_num, "mousemove: requires x y");
            return false;
        }
        return true;
    }

    if (strcmp(line, "quit") == 0) {
        DebugCmd *cmd = script_new_cmd(s);
        if (!cmd)
            return false;
        cmd->type = DBG_CMD_QUIT;
        return true;
    }

    script_set_error(s, line_num, "unknown command: %s", line);
    return false;
}

/* ── Public API ── */

PorttyDebugScript *portty_debug_script_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        // Create a struct to hold the file open error
        PorttyDebugScript *s = calloc(1, sizeof(PorttyDebugScript));
        if (s)
            script_set_error(s, 0, "cannot open file: %s", strerror(errno));
        return s;
    }

    PorttyDebugScript *s = calloc(1, sizeof(PorttyDebugScript));
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

void portty_debug_script_free(PorttyDebugScript *s)
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

int portty_debug_script_count(const PorttyDebugScript *s)
{
    return s ? s->count : 0;
}

const DebugCmd *portty_debug_script_get(const PorttyDebugScript *s, int index)
{
    if (!s || index < 0 || index >= s->count)
        return NULL;
    return &s->cmds[index];
}

const char *portty_debug_script_error(const PorttyDebugScript *s)
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

double portty_debug_now_seconds(void)
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

        printf("  col=%3d cp=%04X w=%d rev=%d invis=%d "
               "fg(%s %02X%02X%02X) "
               "bg(%s %02X%02X%02X)\n",
               col, cell.cp, cell.width, cell.attrs.reverse,
               cell.attrs.invis,
               cell.fg.is_default ? "def" : "set",
               cell.fg.r, cell.fg.g, cell.fg.b,
               cell.bg.is_default ? "def" : "set",
               cell.bg.r, cell.bg.g, cell.bg.b);
    }
    printf("=== end dumpcells ===\n");
}

/* ── Script step execution ── */

/* Static-local wait deadline (safe because only one script runs at a time) */
static double s_wait_deadline = 0;
static bool s_waiting = false;

void portty_debug_script_step(PorttyDebugScript *script,
                              int *cmd_index,
                              DebugExecCtx *ctx)
{
    if (!script || !cmd_index || !ctx)
        return;

    if (*cmd_index >= script->count)
        return;

    const DebugCmd *cmd = &script->cmds[*cmd_index];

    switch (cmd->type) {
    case DBG_CMD_WAIT:
    {
        if (!s_waiting) {
            s_wait_deadline = portty_debug_now_seconds() + cmd->wait_seconds;
            s_waiting = true;
        }
        if (portty_debug_now_seconds() >= s_wait_deadline) {
            s_waiting = false;
            (*cmd_index)++;
        }
        break;
    }

    case DBG_CMD_SEND:
    {
        if (ctx->pty && cmd->text)
            pty_write(ctx->pty, cmd->text, strlen(cmd->text));
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_SENDLN:
    {
        if (ctx->pty && cmd->text) {
            pty_write(ctx->pty, cmd->text, strlen(cmd->text));
            pty_write(ctx->pty, "\r\n", 2);
        }
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_RAW:
    {
        if (ctx->pty && cmd->text)
            pty_write(ctx->pty, cmd->text, strlen(cmd->text));
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_EMIT:
    {
        if (ctx->emit_fn && cmd->text) {
            ctx->emit_fn(ctx->emit_user_data, cmd->text, strlen(cmd->text));
        } else {
            fprintf(stderr, "emit: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_EMIT_RAW:
    {
        if (ctx->emit_fn && cmd->text) {
            ctx->emit_fn(ctx->emit_user_data, cmd->text, strlen(cmd->text));
        } else {
            fprintf(stderr, "emit-raw: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_ASSERT_CONTAINS:
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

    case DBG_CMD_ASSERT_NOT_CONTAINS:
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

    case DBG_CMD_SCREENDUMP:
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

    case DBG_CMD_DUMPROW:
    {
        int rows = 0, cols = 0;
        if (ctx->term)
            terminal_get_dimensions(ctx->term, &rows, &cols);
        execute_dumpcells(ctx->term, ctx->scroll_offset,
                          cmd->row, 0, cols - 1);
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_DUMPCELLS:
    {
        execute_dumpcells(ctx->term, ctx->scroll_offset,
                          cmd->row, cmd->col_start, cmd->col_end);
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_DUMPVERTS:
    {
        if (ctx->dumpverts_fn) {
            ctx->dumpverts_fn(cmd->row, cmd->col_start, cmd->col_end);
        } else {
            fprintf(stderr, "dumpverts: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_VERIFYBUF:
    {
        if (ctx->pending_verifybuf && ctx->verify_row &&
            ctx->verify_col_start && ctx->verify_col_end) {
            *ctx->pending_verifybuf = true;
            *ctx->verify_row = cmd->row;
            *ctx->verify_col_start = cmd->col_start;
            *ctx->verify_col_end = cmd->col_end;
        } else {
            fprintf(stderr, "verifybuf: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_NOTIFY:
    {
        if (ctx->notify_fn) {
            ctx->notify_fn(ctx->notify_user_data,
                           cmd->notify_title[0] ? cmd->notify_title : "",
                           cmd->notify_body[0] ? cmd->notify_body : "");
        } else {
            fprintf(stderr, "notify: not supported by this backend\n");
        }
        (*cmd_index)++;
        break;
    }

    case DBG_CMD_MOUSEMOVE:
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

    case DBG_CMD_QUIT:
    {
        if (ctx->backend && ctx->backend->request_quit)
            ctx->backend->request_quit(ctx->backend);
        (*cmd_index)++;
        break;
    }
    }
}
