/*
 * portty — Terminal state dump to structured JSON for debugging
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * Serializes the whole VT-engine state to a single JSON document for bug
 * reports and scripted diffing: geometry, cursor, every DEC mode, scrollback,
 * selection, the color palette, images (sixel/kitty/iTerm2), Lottie
 * animations, interned OSC-8 hyperlink URIs, and every cell of the visible
 * grid plus the full scrollback.
 *
 * Streams directly to a FILE* (a full scrollback is hundreds of thousands of
 * cells — far too large to buffer as one string). Stays free of SDL and
 * renderer dependencies so it can be linked into the test suite.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "dump.h"
#include "portty_version.h"

#include <coffer/coffer.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef DEP_COFFER_VERSION
#define DEP_COFFER_VERSION "unknown"
#endif

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define DUMP_PATH_MAX PATH_MAX

#ifdef _WIN32
#define DUMP_PATH_SEP '\\'
#else
#define DUMP_PATH_SEP '/'
#endif

/* ── JSON primitives ── */

static void jw_str(FILE *f, const char *s)
{
    fputc('"', f);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            switch (*p) {
            case '"':
                fputs("\\\"", f);
                break;
            case '\\':
                fputs("\\\\", f);
                break;
            case '\b':
                fputs("\\b", f);
                break;
            case '\f':
                fputs("\\f", f);
                break;
            case '\n':
                fputs("\\n", f);
                break;
            case '\r':
                fputs("\\r", f);
                break;
            case '\t':
                fputs("\\t", f);
                break;
            default:
                if (*p < 0x20)
                    fprintf(f, "\\u%04x", *p);
                else
                    fputc(*p, f);
            }
        }
    }
    fputc('"', f);
}

/* Begin a new object member (key + ": ") inside an indented object. */
static void jkw(FILE *f, bool *first, const char *key)
{
    fputs(*first ? "\n    " : ",\n    ", f);
    *first = false;
    jw_str(f, key);
    fputs(": ", f);
}

static void jk_str(FILE *f, bool *first, const char *key, const char *val)
{
    jkw(f, first, key);
    jw_str(f, val);
}

static void jk_int(FILE *f, bool *first, const char *key, long val)
{
    jkw(f, first, key);
    fprintf(f, "%ld", val);
}

static void jk_float(FILE *f, bool *first, const char *key, double val)
{
    jkw(f, first, key);
    fprintf(f, "%.3f", val);
}

static void jk_bool(FILE *f, bool *first, const char *key, bool val)
{
    jkw(f, first, key);
    fputs(val ? "true" : "false", f);
}

/* Close a two-space-indented object started with `fputs("  \"key\": {", f)`. */
static void jclose(FILE *f, bool first, bool comma)
{
    fputs(first ? " }" : "\n  }", f);
    fputs(comma ? ",\n" : "\n", f);
}

static void color_json(FILE *f, const TerminalColor *c)
{
    char buf[16];
    if (c->is_default) {
        jw_str(f, "default");
    } else {
        snprintf(buf, sizeof(buf), "#%02x%02x%02x", c->r, c->g, c->b);
        jw_str(f, buf);
    }
}

/* Encode cell attributes as a compact canonical token string. Fixed order
 * so two dumps of the same state diff cleanly. See "cell_attr_flags". */
static void attrs_token(char *buf, size_t n, const TerminalCellAttr *a)
{
    size_t p = 0;
    if (a->bold)
        buf[p++] = 'b';
    if (a->dim)
        buf[p++] = 'd';
    if (a->italic)
        buf[p++] = 'i';
    if (a->blink)
        buf[p++] = 'k';
    if (a->reverse)
        buf[p++] = 'r';
    if (a->strikethrough)
        buf[p++] = 's';
    if (a->invis)
        buf[p++] = 'v';
    if (a->dwl)
        buf[p++] = 'w';
    if (a->dhl == 1)
        buf[p++] = 'T';
    else if (a->dhl == 2)
        buf[p++] = 'B';
    if (a->underline && p + 2 < n)
        p += (size_t)snprintf(buf + p, n - p, "u%u", a->underline);
    if (a->font && p + 3 < n)
        p += (size_t)snprintf(buf + p, n - p, "f%u", a->font);
    buf[p < n ? p : n - 1] = '\0';
}

/* ── Interned hyperlink table ── */

typedef struct
{
    uint16_t id;
    char *uri;
} LinkEntry;

typedef struct
{
    LinkEntry *v;
    int n, cap;
} LinkTable;

static void link_add(LinkTable *t, uint16_t id, const char *uri)
{
    for (int i = 0; i < t->n; i++)
        if (t->v[i].id == id)
            return; /* first URI seen for an id wins */
    if (t->n == t->cap) {
        int ncap = t->cap ? t->cap * 2 : 8;
        LinkEntry *nv = realloc(t->v, (size_t)ncap * sizeof(*nv));
        if (!nv)
            return;
        t->v = nv;
        t->cap = ncap;
    }
    t->v[t->n].id = id;
    t->v[t->n].uri = strdup(uri);
    if (t->v[t->n].uri)
        t->n++;
}

static void link_free(LinkTable *t)
{
    for (int i = 0; i < t->n; i++)
        free(t->v[i].uri);
    free(t->v);
}

/* ── Name tables ── */

typedef struct
{
    CfrMode mode;
    const char *name;
} ModeName;

static const ModeName mode_names[] = {
    { CFR_MODE_ALTSCREEN, "altscreen" },
    { CFR_MODE_CURSOR_VISIBLE, "cursor_visible" },
    { CFR_MODE_CURSOR_BLINK, "cursor_blink" },
    { CFR_MODE_DECAWM, "autowrap" },
    { CFR_MODE_REVERSE_VIDEO, "reverse_video" },
    { CFR_MODE_BRACKETED_PASTE, "bracketed_paste" },
    { CFR_MODE_MOUSE_X10, "mouse_x10" },
    { CFR_MODE_MOUSE_BTN_EVENT, "mouse_btn_event" },
    { CFR_MODE_MOUSE_DRAG, "mouse_drag" },
    { CFR_MODE_MOUSE_ANY_EVENT, "mouse_any_event" },
    { CFR_MODE_MOUSE_SGR, "mouse_sgr" },
    { CFR_MODE_FOCUS_REPORTING, "focus_reporting" },
    { CFR_MODE_GRAPHEME_CLUSTERS, "grapheme_clusters" },
    { CFR_MODE_SYNC_OUTPUT, "sync_output" },
    { CFR_MODE_SIXEL_SCROLLING, "sixel_scrolling" },
    { CFR_MODE_SIXEL_PRIVATE_REGS, "sixel_private_regs" },
    { CFR_MODE_SIXEL_CURSOR_RIGHT, "sixel_cursor_right" },
    { CFR_MODE_META, "meta" },
    { CFR_MODE_LEFT_RIGHT_MARGINS, "left_right_margins" },
};

static const char *img_source_name(uint8_t src)
{
    switch (src) {
    case IMG_SRC_SIXEL:
        return "sixel";
    case IMG_SRC_LOTTIE:
        return "lottie";
    case IMG_SRC_ITERM:
        return "iterm2";
    case IMG_SRC_KITTY:
        return "kitty";
    default:
        return "unknown";
    }
}

static const char *sel_mode_name(TerminalSelectMode m)
{
    switch (m) {
    case TERM_SELECT_CHAR:
        return "char";
    case TERM_SELECT_WORD:
        return "word";
    case TERM_SELECT_LINE:
        return "line";
    default:
        return "none";
    }
}

/* ── Timestamp / host helpers ── */

static void dump_timestamp(char *buf, size_t n)
{
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    if (strftime(buf, n, "%Y-%m-%dT%H:%M:%SZ", &tmv) == 0)
        snprintf(buf, n, "unknown");
}

static bool dump_os_string(char *buf, size_t n)
{
#ifdef _WIN32
    snprintf(buf, n, "Windows");
    return true;
#else
    struct utsname u;
    if (uname(&u) != 0)
        return false;
    snprintf(buf, n, "%s %s %s", u.sysname, u.release, u.machine);
    return true;
#endif
}

/* ── Cell / section emission ── */

static void emit_cell(FILE *out, TerminalBackend *term, int unified_row, int col,
                      LinkTable *links)
{
    TerminalCell cell;
    int rc = (unified_row < 0)
                 ? terminal_get_scrollback_cell(term, -unified_row - 1, col, &cell)
                 : terminal_get_cell(term, unified_row, col, &cell);
    if (rc != 0) {
        memset(&cell, 0, sizeof(cell));
        cell.fg.is_default = true;
        cell.bg.is_default = true;
        cell.ul_color.is_default = true;
    }

    fprintf(out, "[%u,", (unsigned)cell.cp);

    if (cell.grapheme_id != 0) {
        uint32_t g[64];
        size_t gn = terminal_cell_get_grapheme(term, unified_row, col, g,
                                               sizeof(g) / sizeof(g[0]));
        fputc('[', out);
        for (size_t k = 0; k < gn; k++) {
            if (k)
                fputc(',', out);
            fprintf(out, "%u", (unsigned)g[k]);
        }
        fputc(']', out);
    } else {
        fputc('0', out);
    }

    char attrs[32];
    attrs_token(attrs, sizeof(attrs), &cell.attrs);

    fprintf(out, ",%d,", cell.width);
    jw_str(out, attrs);
    fputc(',', out);
    color_json(out, &cell.fg);
    fputc(',', out);
    color_json(out, &cell.bg);
    fputc(',', out);
    color_json(out, &cell.ul_color);
    fprintf(out, ",%u]", (unsigned)cell.hyperlink_id);

    if (cell.hyperlink_id != 0) {
        char uri[1024];
        if (terminal_cell_get_hyperlink(term, unified_row, col, uri, sizeof(uri)) > 0)
            link_add(links, cell.hyperlink_id, uri);
    }
}

/* ── Public API ── */

int terminal_dump_json(FILE *out, TerminalBackend *term, const TerminalDumpMeta *meta)
{
    if (!out || !term)
        return -1;

    int rows = 0, cols = 0;
    terminal_get_dimensions(term, &rows, &cols);
    int sb_lines = terminal_get_scrollback_lines(term);
    int sb_cap = terminal_get_scrollback_capacity(term);
    TerminalPos cur = terminal_get_cursor_pos(term);

    fputs("{\n", out);
    fputs("  \"schema\": \"portty.terminal-dump\",\n", out);
    fputs("  \"version\": 1,\n", out);

    char ts[32];
    dump_timestamp(ts, sizeof(ts));
    fputs("  \"generated\": ", out);
    jw_str(out, ts);
    fputs(",\n", out);

    fputs("  \"portty\": { \"version\": ", out);
    jw_str(out, PORTTY_VERSION);
    fputs(" },\n", out);

    fputs("  \"engine\": { \"name\": ", out);
    jw_str(out, term->name ? term->name : "unknown");
    fputs(", \"version\": ", out);
    jw_str(out, DEP_COFFER_VERSION);
    fputs(" },\n", out);

    /* Host / renderer snapshot (optional). */
    if (meta && meta->diag) {
        const PorttyDiag *d = meta->diag;
        bool first = true;
        fputs("  \"host\": {", out);
        if (d->platform_name)
            jk_str(out, &first, "platform", d->platform_name);
        if (d->backend_name)
            jk_str(out, &first, "renderer", d->backend_name);
        if (d->graphics_api)
            jk_str(out, &first, "graphics_api", d->graphics_api);
        if (d->gpu_device)
            jk_str(out, &first, "gpu", d->gpu_device);
        if (d->gpu_driver)
            jk_str(out, &first, "driver", d->gpu_driver);
        char osbuf[256];
        if (dump_os_string(osbuf, sizeof(osbuf)))
            jk_str(out, &first, "os", osbuf);
        jclose(out, first, true);
    }

    /* Effective config (optional). */
    if (meta && meta->conf) {
        const PorttyConf *c = meta->conf;
        bool first = true;
        fputs("  \"config\": {", out);
        if (c->source_path)
            jk_str(out, &first, "source", c->source_path);
        if (c->font)
            jk_str(out, &first, "font", c->font);
        if (c->hinting != PORTTY_HINT_UNSET)
            jk_int(out, &first, "hinting", c->hinting);
        if (c->cols > 0)
            jk_int(out, &first, "config_cols", c->cols);
        if (c->rows > 0)
            jk_int(out, &first, "config_rows", c->rows);
        if (c->scrollback >= 0)
            jk_int(out, &first, "config_scrollback", c->scrollback);
        if (c->word_chars)
            jk_str(out, &first, "word_chars", c->word_chars);
        if (c->shell)
            jk_str(out, &first, "shell", c->shell);
        if (c->text_gamma >= 0.0f)
            jk_float(out, &first, "text_gamma", (double)c->text_gamma);
        if (c->text_contrast >= 0.0f)
            jk_float(out, &first, "text_contrast", (double)c->text_contrast);
        if (c->dump_dir)
            jk_str(out, &first, "dump_dir", c->dump_dir);
        jclose(out, first, true);
    }

    /* Geometry. */
    {
        bool first = true;
        int cell_w = (meta && meta->diag) ? meta->diag->cell_width : 0;
        int cell_h = (meta && meta->diag) ? meta->diag->cell_height : 0;
        fputs("  \"geometry\": {", out);
        jk_int(out, &first, "rows", rows);
        jk_int(out, &first, "cols", cols);
        jk_int(out, &first, "cell_w_px", cell_w);
        jk_int(out, &first, "cell_h_px", cell_h);
        if (meta && meta->diag) {
            jk_float(out, &first, "content_scale", (double)meta->diag->content_scale);
            jk_int(out, &first, "window_w_px", meta->diag->pixel_width);
            jk_int(out, &first, "window_h_px", meta->diag->pixel_height);
        }
        jclose(out, first, true);
    }

    const char *title = terminal_get_title(term);
    fputs("  \"title\": ", out);
    jw_str(out, title ? title : "");
    fputs(",\n", out);

    fputs("  \"altscreen\": ", out);
    fputs(terminal_is_altscreen(term) ? "true,\n" : "false,\n", out);
    fprintf(out, "  \"mouse_mode\": %d,\n", terminal_get_mouse_mode(term));

    /* Cursor. */
    {
        bool first = true;
        fputs("  \"cursor\": {", out);
        jk_int(out, &first, "row", cur.row);
        jk_int(out, &first, "col", cur.col);
        jk_bool(out, &first, "visible", terminal_get_cursor_visible(term));
        jk_bool(out, &first, "blink", terminal_get_cursor_blink(term));
        jclose(out, first, true);
    }

    /* Modes. */
    {
        bool first = true;
        fputs("  \"modes\": {", out);
        for (size_t i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); i++)
            jk_bool(out, &first, mode_names[i].name,
                    terminal_get_mode(term, mode_names[i].mode));
        jclose(out, first, true);
    }

    /* Scrollback. */
    {
        bool first = true;
        fputs("  \"scrollback\": {", out);
        jk_int(out, &first, "lines", sb_lines);
        jk_int(out, &first, "capacity", sb_cap);
        jk_int(out, &first, "view_offset", meta ? meta->scroll_offset : 0);
        jclose(out, first, true);
    }

    /* Selection. */
    {
        TerminalSelection sel;
        if (terminal_get_selection(term, &sel)) {
            bool first = true;
            fputs("  \"selection\": {", out);
            jk_str(out, &first, "mode", sel_mode_name(sel.mode));
            jk_int(out, &first, "anchor_row", sel.anchor.row);
            jk_int(out, &first, "anchor_col", sel.anchor.col);
            jk_int(out, &first, "start_row", sel.start.row);
            jk_int(out, &first, "start_col", sel.start.col);
            jk_int(out, &first, "end_row", sel.end.row);
            jk_int(out, &first, "end_col", sel.end.col);
            jclose(out, first, true);
        } else {
            fputs("  \"selection\": null,\n", out);
        }
    }

    /* Palette. */
    {
        fputs("  \"palette\": {\n", out);
        fprintf(out, "    \"default_fg\": \"#%06x\",\n", cfr_default_fg_rgb() & 0xFFFFFFu);
        fprintf(out, "    \"default_bg\": \"#%06x\",\n", cfr_default_bg_rgb() & 0xFFFFFFu);
        fputs("    \"colors\": [", out);
        for (int i = 0; i < 256; i++) {
            if (i)
                fputc(',', out);
            if (i % 16 == 0)
                fputs("\n      ", out);
            fprintf(out, "\"#%06x\"", cfr_default_palette_rgb((uint8_t)i) & 0xFFFFFFu);
        }
        fputs("\n    ]\n  },\n", out);
    }

    /* Images (sixel / kitty / iTerm2). */
    {
        int count = 0;
        const CfrImage *imgs = terminal_get_images(term, &count);
        fputs("  \"images\": [", out);
        for (int i = 0; imgs && i < count; i++) {
            fputs(i ? ",\n    " : "\n    ", out);
            fprintf(out, "{ \"id\": %llu, \"version\": %u, \"source\": ",
                    (unsigned long long)imgs[i].id, imgs[i].version);
            jw_str(out, img_source_name(imgs[i].source));
            fprintf(out,
                    ", \"layer\": %u, \"row\": %d, \"col\": %d, "
                    "\"w_px\": %d, \"h_px\": %d, \"buf_w\": %d, \"buf_h\": %d }",
                    (unsigned)imgs[i].layer, imgs[i].row, imgs[i].col,
                    imgs[i].width_px, imgs[i].height_px, imgs[i].buf_w,
                    imgs[i].buf_h);
        }
        fputs(count ? "\n  ],\n" : "],\n", out);
    }

    {
        int count = 0;
        const CfrImagePlacement *pls = terminal_get_image_placements(term, &count);
        fputs("  \"image_placements\": [", out);
        for (int i = 0; pls && i < count; i++) {
            fputs(i ? ",\n    " : "\n    ", out);
            fprintf(out,
                    "{ \"id\": %llu, \"image_id\": %llu, \"row\": %d, \"col\": %d, "
                    "\"rows\": %d, \"cols\": %d, \"layer\": %u, \"opacity\": %u, "
                    "\"z_index\": %d, \"src\": [%d,%d,%d,%d] }",
                    (unsigned long long)pls[i].id,
                    (unsigned long long)pls[i].image_id, pls[i].row, pls[i].col,
                    pls[i].rows, pls[i].cols, (unsigned)pls[i].layer,
                    (unsigned)pls[i].opacity_x256, pls[i].z_index, pls[i].src_x,
                    pls[i].src_y, pls[i].src_w, pls[i].src_h);
        }
        fputs(count ? "\n  ],\n" : "],\n", out);
    }

    /* Lottie animations + placements. */
    {
        int count = 0;
        const CfrLottie *los = terminal_get_lotties(term, &count);
        fputs("  \"lotties\": [", out);
        for (int i = 0; los && i < count; i++) {
            fputs(i ? ",\n    " : "\n    ", out);
            fprintf(out,
                    "{ \"id\": %llu, \"version\": %u, \"canvas_w\": %d, "
                    "\"canvas_h\": %d, \"current_frame\": %d, \"frame_count\": %d, "
                    "\"playing\": %s, \"speed\": %.3f, \"loop\": %s, \"placements\": [",
                    (unsigned long long)los[i].id, los[i].version, los[i].canvas_w,
                    los[i].canvas_h, los[i].current_frame, los[i].frame_count,
                    los[i].playing ? "true" : "false", los[i].speed,
                    los[i].loop ? "true" : "false");
            int pc = 0;
            const CfrLottiePlacement *lp =
                terminal_get_lottie_placements(term, los[i].id, &pc);
            for (int j = 0; lp && j < pc; j++) {
                fputs(j ? ", " : "", out);
                fprintf(out,
                        "{ \"id\": %llu, \"abs_line\": %ld, \"row\": %d, \"col\": %d, "
                        "\"rows\": %d, \"cols\": %d, \"layer\": %u, \"opacity\": %u }",
                        (unsigned long long)lp[j].id, lp[j].abs_line, lp[j].row,
                        lp[j].col, lp[j].rows, lp[j].cols, (unsigned)lp[j].layer,
                        (unsigned)lp[j].opacity_x256);
            }
            fputs("] }", out);
        }
        fputs(count ? "\n  ],\n" : "],\n", out);
    }

    /* Cells: visible grid + full scrollback. */
    {
        LinkTable links = { 0 };
        int row_min = -sb_lines;
        int row_max = rows - 1;

        fputs("  \"cell_columns\": "
              "[\"cp\",\"grapheme\",\"width\",\"attrs\",\"fg\",\"bg\",\"ul\",\"link\"],\n",
              out);
        fputs("  \"cell_attr_flags\": {\"bold\":\"b\",\"dim\":\"d\",\"italic\":\"i\","
              "\"blink\":\"k\",\"reverse\":\"r\",\"strikethrough\":\"s\","
              "\"invis\":\"v\",\"dwl\":\"w\",\"dhl_top\":\"T\",\"dhl_bottom\":\"B\","
              "\"underline\":\"u<n>\",\"font\":\"f<n>\"},\n",
              out);

        fprintf(out,
                "  \"cells\": { \"unified_row_min\": %d, \"unified_row_max\": %d, "
                "\"rows\": [\n",
                row_min, row_max);

        for (int u = row_min; u <= row_max; u++) {
            fprintf(out, "    { \"row\": %d, \"wrap\": %s, \"cells\": [", u,
                    terminal_get_line_continuation(term, u) ? "true" : "false");
            for (int col = 0; col < cols; col++) {
                if (col)
                    fputc(',', out);
                emit_cell(out, term, u, col, &links);
            }
            fputs("] }", out);
            fputs(u < row_max ? ",\n" : "\n", out);
        }
        fputs("  ] },\n", out);

        /* Interned hyperlink URIs (keyed by the cell `link` field). */
        fputs("  \"links\": {", out);
        for (int i = 0; i < links.n; i++) {
            fputs(i ? ",\n    " : "\n    ", out);
            fprintf(out, "\"%u\": ", (unsigned)links.v[i].id);
            jw_str(out, links.v[i].uri);
        }
        fputs(links.n ? "\n  }\n" : "}\n", out);
        link_free(&links);
    }

    fputs("}\n", out);
    return 0;
}

/* ── Destination resolution ── */

static bool dump_dir(char *buf, size_t n, const char *override_dir)
{
    if (override_dir && *override_dir) {
        snprintf(buf, n, "%s", override_dir);
        return true;
    }
    const char *env = getenv("PORTTY_DUMP_DIR");
    if (env && *env) {
        snprintf(buf, n, "%s", env);
        return true;
    }
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !*base)
        base = getenv("TEMP");
    if (!base || !*base)
        return false;
    snprintf(buf, n, "%s\\portty\\dumps", base);
#else
    const char *state = getenv("XDG_STATE_HOME");
    if (state && *state) {
        snprintf(buf, n, "%s/portty/dumps", state);
        return true;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, n, "%s/.local/state/portty/dumps", home);
        return true;
    }
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    snprintf(buf, n, "%s/portty-dumps", tmp);
#endif
    return true;
}

static void ensure_dir(const char *dir)
{
    char tmp[DUMP_PATH_MAX];
    size_t len = dir ? strlen(dir) : 0;
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, dir, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0700);
#endif
            *p = c;
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0700);
#endif
}

/* Create the parent directory of a caller-supplied path. */
static void ensure_parent_dir(const char *path)
{
    char tmp[DUMP_PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);
    char *slash = NULL;
    for (char *p = tmp; *p; p++)
        if (*p == '/' || *p == '\\')
            slash = p;
    if (!slash || slash == tmp)
        return;
    *slash = '\0';
    ensure_dir(tmp);
}

bool portty_dump_build_path(const TerminalDumpMeta *meta, const char *path,
                            char *out, size_t out_n)
{
    if (!out || out_n == 0)
        return false;
    if (path && *path) {
        snprintf(out, out_n, "%s", path);
        return true;
    }

    const char *override_dir = (meta && meta->conf) ? meta->conf->dump_dir : NULL;
    char dir[DUMP_PATH_MAX];
    if (!dump_dir(dir, sizeof(dir), override_dir))
        return false;
    ensure_dir(dir);

    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    char stamp[32];
    if (strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv) == 0)
        snprintf(stamp, sizeof(stamp), "unknown");

#ifdef _WIN32
    long pid = (long)_getpid();
#else
    long pid = (long)getpid();
#endif
    snprintf(out, out_n, "%s%cportty-state-%s-%ld.json", dir, DUMP_PATH_SEP, stamp,
             pid);
    return true;
}

bool portty_dump_write(TerminalBackend *term, const TerminalDumpMeta *meta,
                       const char *path, char *resolved, size_t resolved_n)
{
    if (!term)
        return false;

    if (path && strcmp(path, "-") == 0) {
        int rc = terminal_dump_json(stdout, term, meta);
        if (resolved && resolved_n)
            snprintf(resolved, resolved_n, "-");
        return rc == 0;
    }

    /* A caller-supplied path may sit in a directory that does not exist yet. */
    if (path && *path)
        ensure_parent_dir(path);

    char full[DUMP_PATH_MAX];
    if (!portty_dump_build_path(meta, path, full, sizeof(full))) {
        fprintf(stderr, "ERROR: dump: could not resolve destination path\n");
        return false;
    }

    char tmp[DUMP_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", full) >= (int)sizeof(tmp)) {
        fprintf(stderr, "ERROR: dump: destination path too long: %s\n", full);
        return false;
    }

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: dump: cannot open %s: %s\n", tmp, strerror(errno));
        return false;
    }
    int rc = terminal_dump_json(f, term, meta);
    if (fclose(f) != 0 && rc == 0)
        rc = -1;
    if (rc != 0) {
        remove(tmp);
        fprintf(stderr, "ERROR: dump: write failed: %s\n", tmp);
        return false;
    }

#ifndef _WIN32
    chmod(tmp, 0600);
#endif
    if (rename(tmp, full) != 0) {
        /* Windows rename() fails when the target exists; retry after removing. */
        remove(full);
        if (rename(tmp, full) != 0) {
            remove(tmp);
            fprintf(stderr, "ERROR: dump: cannot rename %s: %s\n", tmp,
                    strerror(errno));
            return false;
        }
    }

    if (resolved && resolved_n)
        snprintf(resolved, resolved_n, "%s", full);
    return true;
}
