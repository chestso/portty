#include "term_vt.h"
#include "common.h"
#include "sixel.h"
#include "term.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vterm.h>

#define SCROLLBACK_SIZE 1000

// Per-cell extension attributes not tracked by libvterm
typedef struct
{
    uint8_t underline_style; // 0-5
    uint8_t ul_color[3];     // RGB
    bool has_ul_color;       // false = use default constant
} CellExtAttrs;

// Scrollback line entry - stores cells with original column count
typedef struct
{
    VTermScreenCell *cells;
    CellExtAttrs *ext_cells; // extension attributes per cell (may be NULL for old lines)
    int cols;
    bool continuation; // true if this line is a soft-wrap continuation of the previous
} ScrollbackLine;

// Default ANSI 16-color palette inspired by charm.land color style
// clang-format off
static const uint8_t default_palette[16][3] = {
    [0]  = { 0x1a, 0x1a, 0x1a }, // Black
    [1]  = { 0xed, 0x56, 0x7a }, // Red        — Charm brand red
    [2]  = { 0x02, 0xbf, 0x87 }, // Green      — Charm brand green
    [3]  = { 0xec, 0xcc, 0x68 }, // Yellow     — warm golden
    [4]  = { 0x75, 0x71, 0xf9 }, // Blue       — Charm indigo
    [5]  = { 0xf7, 0x80, 0xe2 }, // Magenta    — Charm fuchsia
    [6]  = { 0x6e, 0xef, 0xc0 }, // Cyan       — Charm mint
    [7]  = { 0xd0, 0xd0, 0xd0 }, // White
    [8]  = { 0x67, 0x67, 0x67 }, // Bright Black
    [9]  = { 0xff, 0x8d, 0xa1 }, // Bright Red
    [10] = { 0x5a, 0xee, 0xad }, // Bright Green
    [11] = { 0xf5, 0xdf, 0xa0 }, // Bright Yellow
    [12] = { 0x9b, 0x98, 0xff }, // Bright Blue
    [13] = { 0xff, 0x9c, 0xe8 }, // Bright Magenta
    [14] = { 0xa5, 0xf5, 0xd4 }, // Bright Cyan
    [15] = { 0xff, 0xfd, 0xf5 }, // Bright White — Charm cream
};
// clang-format on

static const uint8_t default_fg[3] = { 0xff, 0xfd, 0xf5 }; // Charm cream
static const uint8_t default_bg[3] = { 0x00, 0x00, 0x00 };

// Forward declaration for callback type
typedef void (*TerminalOutputCallback)(const char *data, size_t len, void *user);

typedef struct TerminalVtData
{
    VTerm *vt;
    VTermState *state;
    VTermScreen *screen;
    int width;
    int height;
    TerminalDamageRect damage;
    bool has_damage;
    VTermPos cursor_pos;
    bool cursor_visible;
    bool cursor_blink_enabled;
    char *title;
    ScrollbackLine *scrollback;
    int scrollback_lines;
    int scrollback_capacity;

    // Alternate screen and mouse mode state
    bool in_altscreen;
    int mouse_mode; // 0=none, 1=click, 2=drag, 3=move
    TerminalOutputCallback output_cb;
    void *output_cb_user;

    // Sixel support
    TerminalBackend *backend; // back-pointer to owning backend
    SixelParser *sixel_parser;

    // Extension attribute tracking (SGR 4:4/4:5 underline style, SGR 58/59 underline color)
    // libvterm only supports underline 0-3; we track styles 4-5 and underline color.
    uint8_t ext_ul_style;    // current pen underline style (0-5)
    uint8_t ext_ul_color[3]; // current pen underline color RGB
    bool ext_has_ul_color;   // current pen has explicit underline color
    CellExtAttrs *ext_grid;  // per-cell grid [height * width]
    TerminalDamageRect grid_damage;
    bool has_grid_damage;

    // Reusable buffer for SGR rewriting in process_input
    char *sgr_buf;
    size_t sgr_buf_cap;
} TerminalVtData;

// Forward declarations
static void damage_union(TerminalVtData *data, int start_row, int start_col, int end_row, int end_col);
static int term_damage(VTermRect rect, void *user);
static int term_moverect(VTermRect dest, VTermRect src, void *user);
static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user);
static int term_settermprop(VTermProp prop, VTermValue *val, void *user);
static int term_bell(void *user);
static int term_sb_pushline(int cols, const VTermScreenCell *cells, void *user);
static int term_sb_popline(int cols, VTermScreenCell *cells, void *user);
static void term_output_callback(const char *data, size_t len, void *user);

static VTermScreenCallbacks cb = {
    .damage = term_damage,
    .moverect = term_moverect,
    .movecursor = term_movecursor,
    .settermprop = term_settermprop,
    .bell = term_bell,
    .sb_pushline = term_sb_pushline,
    .sb_popline = term_sb_popline,
};

// Convert a VTermColor to TerminalColor, resolving indexed colors via VTermState
static TerminalColor convert_vterm_color(const VTermColor *vcol, VTermState *state, bool is_bg)
{
    TerminalColor result = { 0, 0, 0, false };

    if (is_bg ? VTERM_COLOR_IS_DEFAULT_BG(vcol) : VTERM_COLOR_IS_DEFAULT_FG(vcol)) {
        result.is_default = true;
        if (is_bg) {
            result.r = default_bg[0];
            result.g = default_bg[1];
            result.b = default_bg[2];
        } else {
            result.r = default_fg[0];
            result.g = default_fg[1];
            result.b = default_fg[2];
        }
        return result;
    }

    if (VTERM_COLOR_IS_RGB(vcol)) {
        result.r = vcol->rgb.red;
        result.g = vcol->rgb.green;
        result.b = vcol->rgb.blue;
    } else if (VTERM_COLOR_IS_INDEXED(vcol)) {
        // Use vterm's state to resolve indexed colors to RGB
        VTermColor resolved = *vcol;
        vterm_state_convert_color_to_rgb(state, &resolved);
        result.r = resolved.rgb.red;
        result.g = resolved.rgb.green;
        result.b = resolved.rgb.blue;
    } else {
        // Fallback
        result.r = is_bg ? default_bg[0] : default_fg[0];
        result.g = is_bg ? default_bg[1] : default_fg[1];
        result.b = is_bg ? default_bg[2] : default_fg[2];
    }

    return result;
}

// DCS callback for sixel sequences
static int vt_dcs_callback(const char *command, size_t commandlen, VTermStringFragment frag,
                           void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;

    // Check if command is "q" (sixel introducer)
    if (commandlen == 1 && command[0] == 'q') {
        if (frag.initial) {
            sixel_parser_begin(data->sixel_parser);
        }
        sixel_parser_feed(data->sixel_parser, frag.str, frag.len);
        if (frag.final) {
            SixelImage *img = sixel_parser_finish(data->sixel_parser);
            if (img) {
                img->cursor_row = data->cursor_pos.row;
                img->cursor_col = data->cursor_pos.col;
                terminal_add_sixel_image(data->backend, img);
                // Force a full redraw so the image appears
                damage_union(data, 0, 0, data->height, data->width);
            }
        }
        return 1; // consumed
    }
    return 0; // not handled
}

static const VTermStateFallbacks vt_fallbacks = {
    .dcs = vt_dcs_callback,
};

// Record of an extension attribute change at a byte offset in the input buffer.
#define MAX_EXT_TRANSITIONS 64
typedef struct
{
    size_t pos;              // byte offset of the ESC that starts this SGR
    uint8_t underline_style; // new underline style (0-5)
    uint8_t ul_color[3];     // new underline color RGB
    bool has_ul_color;       // whether explicit underline color is set
} SgrExtTransition;

// Resolve a 256-color index to RGB
static void resolve_color_index(uint8_t index, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (index < 16) {
        *r = default_palette[index][0];
        *g = default_palette[index][1];
        *b = default_palette[index][2];
    } else if (index < 232) {
        // 6x6x6 color cube (indices 16-231)
        int ci = index - 16;
        int ri = ci / 36;
        int gi = (ci % 36) / 6;
        int bi = ci % 6;
        *r = ri ? (uint8_t)(ri * 40 + 55) : 0;
        *g = gi ? (uint8_t)(gi * 40 + 55) : 0;
        *b = bi ? (uint8_t)(bi * 40 + 55) : 0;
    } else {
        // Grayscale ramp (indices 232-255)
        uint8_t v = (uint8_t)(8 + (index - 232) * 10);
        *r = v;
        *g = v;
        *b = v;
    }
}

// Parse a colon-separated sub-parameter value starting at buf[*p].
// Advances *p past the value. Returns the parsed integer or -1 if empty.
static long parse_colon_subparam(const char *buf, size_t len, size_t *p)
{
    if (*p >= len || buf[*p] == ':' || buf[*p] == ';' || buf[*p] == 'm')
        return -1;
    long val = 0;
    while (*p < len && buf[*p] >= '0' && buf[*p] <= '9') {
        val = val * 10 + (buf[*p] - '0');
        (*p)++;
    }
    return val;
}

// Skip to the next colon sub-parameter. Returns false if at ';' or 'm' or end.
static bool skip_to_next_colon(const char *buf, size_t len, size_t *p)
{
    if (*p < len && buf[*p] == ':') {
        (*p)++;
        return true;
    }
    return false;
}

// Scan input for CSI SGR sequences (ESC [ ... m).
// Rewrite 4:4→4:1 and 4:5→4:1 so libvterm stores underline=1.
// Parse SGR 58 (underline color) and SGR 59 (reset underline color).
// Record extension attribute transitions so the caller can split processing.
static int parse_sgr_extensions(TerminalVtData *data, char *buf, size_t len,
                                SgrExtTransition *transitions, int max_trans)
{
    int count = 0;
    for (size_t i = 0; i + 2 < len; i++) {
        if (buf[i] != '\x1b' || buf[i + 1] != '[')
            continue;
        size_t sgr_start = i;
        i += 2;
        size_t params_start = i;
        while (i < len && (unsigned char)buf[i] < 0x40)
            i++;
        if (i >= len || buf[i] != 'm')
            continue;
        size_t params_end = i;

        uint8_t old_style = data->ext_ul_style;
        uint8_t old_color[3] = { data->ext_ul_color[0], data->ext_ul_color[1],
                                 data->ext_ul_color[2] };
        bool old_has_color = data->ext_has_ul_color;

        size_t p = params_start;
        if (p == params_end) {
            // ESC[m = full reset
            data->ext_ul_style = 0;
            data->ext_has_ul_color = false;
        } else {
            while (p < params_end) {
                long val = 0;
                while (p < params_end && buf[p] >= '0' && buf[p] <= '9') {
                    val = val * 10 + (buf[p] - '0');
                    p++;
                }
                if (val == 0) {
                    // SGR 0 = full reset
                    data->ext_ul_style = 0;
                    data->ext_has_ul_color = false;
                } else if (val == 24) {
                    data->ext_ul_style = 0;
                } else if (val == 4) {
                    if (p < params_end && buf[p] == ':') {
                        p++;
                        long sub = 0;
                        size_t sub_start = p;
                        while (p < params_end && buf[p] >= '0' && buf[p] <= '9') {
                            sub = sub * 10 + (buf[p] - '0');
                            p++;
                        }
                        if (sub >= 0 && sub <= 5)
                            data->ext_ul_style = (uint8_t)sub;
                        if (sub == 4 || sub == 5)
                            buf[sub_start] = '1';
                    } else {
                        // Plain ESC[4m defaults to dotted style
                        data->ext_ul_style = 4;
                    }
                } else if (val == 58) {
                    // SGR 58 - set underline color
                    if (p < params_end && buf[p] == ':') {
                        p++;
                        long color_type = parse_colon_subparam(buf, params_end, &p);
                        if (color_type == 2) {
                            // 58:2[:CS]:R:G:B — truecolor
                            // Read up to 4 colon-separated values, then
                            // interpret based on count:
                            //   3 values → R:G:B (no colorspace)
                            //   4 values → CS:R:G:B (CS ignored)
                            long v[4] = { -1, -1, -1, -1 };
                            int nvals = 0;
                            for (int vi = 0; vi < 4; vi++) {
                                if (!skip_to_next_colon(buf, params_end, &p))
                                    break;
                                v[nvals] = parse_colon_subparam(buf, params_end, &p);
                                nvals++;
                            }
                            if (nvals >= 4) {
                                // 58:2:CS:R:G:B — v[0]=CS, v[1]=R, v[2]=G, v[3]=B
                                data->ext_ul_color[0] =
                                    (uint8_t)(v[1] < 0 ? 0 : v[1] > 255 ? 255
                                                                        : v[1]);
                                data->ext_ul_color[1] =
                                    (uint8_t)(v[2] < 0 ? 0 : v[2] > 255 ? 255
                                                                        : v[2]);
                                data->ext_ul_color[2] =
                                    (uint8_t)(v[3] < 0 ? 0 : v[3] > 255 ? 255
                                                                        : v[3]);
                                data->ext_has_ul_color = true;
                            } else if (nvals >= 3) {
                                // 58:2:R:G:B — no colorspace
                                data->ext_ul_color[0] =
                                    (uint8_t)(v[0] < 0 ? 0 : v[0] > 255 ? 255
                                                                        : v[0]);
                                data->ext_ul_color[1] =
                                    (uint8_t)(v[1] < 0 ? 0 : v[1] > 255 ? 255
                                                                        : v[1]);
                                data->ext_ul_color[2] =
                                    (uint8_t)(v[2] < 0 ? 0 : v[2] > 255 ? 255
                                                                        : v[2]);
                                data->ext_has_ul_color = true;
                            }
                        } else if (color_type == 5) {
                            // 58:5:INDEX — 256-color
                            if (skip_to_next_colon(buf, params_end, &p)) {
                                long idx = parse_colon_subparam(buf, params_end, &p);
                                if (idx >= 0 && idx <= 255) {
                                    resolve_color_index((uint8_t)idx, &data->ext_ul_color[0],
                                                        &data->ext_ul_color[1],
                                                        &data->ext_ul_color[2]);
                                    data->ext_has_ul_color = true;
                                }
                            }
                        }
                    } else if (p < params_end && buf[p] == ';') {
                        // SGR 58;2;R;G;B or 58;5;INDEX (semicolon-separated form)
                        p++;
                        long color_type = 0;
                        while (p < params_end && buf[p] >= '0' && buf[p] <= '9') {
                            color_type = color_type * 10 + (buf[p] - '0');
                            p++;
                        }
                        if (color_type == 2 && p < params_end && buf[p] == ';') {
                            p++;
                            long r = 0;
                            while (p < params_end && buf[p] >= '0' && buf[p] <= '9') {
                                r = r * 10 + (buf[p] - '0');
                                p++;
                            }
                            if (p < params_end && buf[p] == ';') {
                                p++;
                                long g = 0;
                                while (p < params_end && buf[p] >= '0' && buf[p] <= '9') {
                                    g = g * 10 + (buf[p] - '0');
                                    p++;
                                }
                                if (p < params_end && buf[p] == ';') {
                                    p++;
                                    long b = 0;
                                    while (p < params_end && buf[p] >= '0' && buf[p] <= '9') {
                                        b = b * 10 + (buf[p] - '0');
                                        p++;
                                    }
                                    data->ext_ul_color[0] = (uint8_t)(r > 255 ? 255 : r);
                                    data->ext_ul_color[1] = (uint8_t)(g > 255 ? 255 : g);
                                    data->ext_ul_color[2] = (uint8_t)(b > 255 ? 255 : b);
                                    data->ext_has_ul_color = true;
                                }
                            }
                        } else if (color_type == 5 && p < params_end && buf[p] == ';') {
                            p++;
                            long idx = 0;
                            while (p < params_end && buf[p] >= '0' && buf[p] <= '9') {
                                idx = idx * 10 + (buf[p] - '0');
                                p++;
                            }
                            if (idx >= 0 && idx <= 255) {
                                resolve_color_index((uint8_t)idx, &data->ext_ul_color[0],
                                                    &data->ext_ul_color[1],
                                                    &data->ext_ul_color[2]);
                                data->ext_has_ul_color = true;
                            }
                        }
                        continue; // already advanced past params
                    }
                } else if (val == 59) {
                    // SGR 59 - reset underline color to default
                    data->ext_has_ul_color = false;
                }
                while (p < params_end && buf[p] != ';')
                    p++;
                if (p < params_end)
                    p++;
            }
        }

        bool changed = (data->ext_ul_style != old_style) ||
                       (data->ext_has_ul_color != old_has_color) ||
                       (data->ext_has_ul_color &&
                        (data->ext_ul_color[0] != old_color[0] ||
                         data->ext_ul_color[1] != old_color[1] ||
                         data->ext_ul_color[2] != old_color[2]));
        if (changed && count < max_trans) {
            transitions[count].pos = sgr_start;
            transitions[count].underline_style = data->ext_ul_style;
            memcpy(transitions[count].ul_color, data->ext_ul_color, 3);
            transitions[count].has_ul_color = data->ext_has_ul_color;
            count++;
        }
    }
    return count;
}

// Update ext_grid for cells damaged since last clear.
static void update_ext_grid(TerminalVtData *data)
{
    if (!data->has_grid_damage || !data->ext_grid)
        return;
    for (int row = data->grid_damage.start_row; row < data->grid_damage.end_row; row++) {
        for (int col = data->grid_damage.start_col; col < data->grid_damage.end_col; col++) {
            if (row < 0 || row >= data->height || col < 0 || col >= data->width)
                continue;
            VTermScreenCell vcell;
            vterm_screen_get_cell(data->screen, (VTermPos){ .row = row, .col = col }, &vcell);
            int idx = row * data->width + col;
            if (vcell.attrs.underline > 0 && data->ext_ul_style >= 4)
                data->ext_grid[idx].underline_style = data->ext_ul_style;
            else
                data->ext_grid[idx].underline_style = vcell.attrs.underline;
            data->ext_grid[idx].has_ul_color = data->ext_has_ul_color;
            memcpy(data->ext_grid[idx].ul_color, data->ext_ul_color, 3);
        }
    }
}

static bool vt_init(TerminalBackend *backend, int width, int height)
{
    // Allocate libvterm-specific data
    TerminalVtData *data = malloc(sizeof(TerminalVtData));
    if (!data) {
        return false;
    }

    data->width = width;
    data->height = height;
    data->has_damage = false;
    data->damage = (TerminalDamageRect){ 0, 0, 0, 0 };
    data->cursor_pos.row = 0;
    data->cursor_pos.col = 0;
    data->cursor_visible = true;
    data->cursor_blink_enabled = true;
    data->title = NULL;
    data->scrollback = NULL;
    data->scrollback_lines = 0;
    data->scrollback_capacity = 0;
    data->in_altscreen = false;
    data->mouse_mode = 0;
    data->output_cb = NULL;
    data->output_cb_user = NULL;
    data->backend = backend;
    data->sixel_parser = sixel_parser_create();
    data->ext_ul_style = 0;
    data->ext_ul_color[0] = data->ext_ul_color[1] = data->ext_ul_color[2] = 0;
    data->ext_has_ul_color = false;
    data->ext_grid = calloc(width * height, sizeof(CellExtAttrs));
    data->has_grid_damage = false;
    data->grid_damage = (TerminalDamageRect){ 0, 0, 0, 0 };
    data->sgr_buf = NULL;
    data->sgr_buf_cap = 0;

    data->vt = vterm_new(height, width);
    if (!data->vt) {
        free(data);
        return false;
    }

    vterm_set_utf8(data->vt, 1);

    data->screen = vterm_obtain_screen(data->vt);
    data->state = vterm_obtain_state(data->vt);
    vterm_screen_enable_altscreen(data->screen, 1);
    vterm_screen_set_callbacks(data->screen, &cb, data);
    vterm_screen_set_damage_merge(data->screen, VTERM_DAMAGE_SCROLL);
    // Reflow disabled by default due to libvterm bug that can cause crashes
    // during extreme resizes. Enable via --reflow CLI flag.
    // See: https://github.com/neovim/neovim/issues/25234
    vterm_screen_enable_reflow(data->screen, false);

    // Set up output callback for mouse escape sequences
    vterm_output_set_callback(data->vt, term_output_callback, data);

    vterm_screen_reset(data->screen, 1);

    // Set up sixel DCS fallback handler
    vterm_screen_set_unrecognised_fallbacks(data->screen, &vt_fallbacks, data);

    // Apply default palette
    for (int i = 0; i < 16; i++) {
        VTermColor col;
        vterm_color_rgb(&col, default_palette[i][0], default_palette[i][1], default_palette[i][2]);
        vterm_state_set_palette_color(data->state, i, &col);
    }

    // Apply default foreground and background
    VTermColor fg, bg;
    vterm_color_rgb(&fg, default_fg[0], default_fg[1], default_fg[2]);
    vterm_color_rgb(&bg, default_bg[0], default_bg[1], default_bg[2]);
    vterm_screen_set_default_colors(data->screen, &fg, &bg);

    // Store in backend
    backend->backend_data = data;

    return true;
}

static void vt_destroy(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;

    if (data->vt)
        vterm_free(data->vt);
    if (data->title)
        free(data->title);
    if (data->scrollback) {
        for (int i = 0; i < data->scrollback_lines; i++) {
            free(data->scrollback[i].cells);
            free(data->scrollback[i].ext_cells);
        }
        free(data->scrollback);
    }
    sixel_parser_destroy(data->sixel_parser);
    terminal_clear_sixel_images(backend);
    free(data->ext_grid);
    free(data->sgr_buf);

    free(data);
    backend->backend_data = NULL;
}

static void vt_resize(TerminalBackend *backend, int width, int height)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;

    if (data->vt) {
        data->width = width;
        data->height = height;
        vterm_set_size(data->vt, height, width);
        vterm_screen_flush_damage(data->screen);

        // Reallocate extension grid for new dimensions
        free(data->ext_grid);
        data->ext_grid = calloc(width * height, sizeof(CellExtAttrs));

        // Sync cursor position directly from libvterm state after resize
        // The movecursor callback may not fire during resize, causing desync
        VTermPos cursorpos;
        vterm_state_get_cursorpos(data->state, &cursorpos);
        data->cursor_pos = cursorpos;

        // Full redraw needed after resize
        damage_union(data, 0, 0, height, width);
    }
}

static int vt_process_input(TerminalBackend *backend, const char *input, size_t len)
{
    if (!backend || !backend->backend_data)
        return -1;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;

    if (data->vt) {
        // Copy input so we can rewrite SGR 4:4/4:5 → 4:1
        if (len > data->sgr_buf_cap) {
            free(data->sgr_buf);
            data->sgr_buf = malloc(len);
            if (!data->sgr_buf) {
                data->sgr_buf_cap = 0;
                return -1;
            }
            data->sgr_buf_cap = len;
        }
        memcpy(data->sgr_buf, input, len);

        // Parse SGR sequences: rewrite buffer and record extension transitions
        uint8_t saved_style = data->ext_ul_style;
        uint8_t saved_color[3] = { data->ext_ul_color[0], data->ext_ul_color[1],
                                   data->ext_ul_color[2] };
        bool saved_has_color = data->ext_has_ul_color;
        SgrExtTransition transitions[MAX_EXT_TRANSITIONS];
        int ntrans =
            parse_sgr_extensions(data, data->sgr_buf, len, transitions, MAX_EXT_TRANSITIONS);
        // Restore pen — we'll advance it segment by segment below
        data->ext_ul_style = saved_style;
        memcpy(data->ext_ul_color, saved_color, 3);
        data->ext_has_ul_color = saved_has_color;

        // Process input in segments, splitting at extension transitions
        // so each segment's grid update uses the correct pen values.
        int total_written = 0;
        size_t offset = 0;
        for (int t = 0; t <= ntrans; t++) {
            size_t end = (t < ntrans) ? transitions[t].pos : len;
            size_t chunk_len = end - offset;

            if (chunk_len > 0) {
                data->has_grid_damage = false;
                total_written += vterm_input_write(data->vt, data->sgr_buf + offset, chunk_len);
                vterm_screen_flush_damage(data->screen);
                update_ext_grid(data);
            }

            if (t < ntrans) {
                data->ext_ul_style = transitions[t].underline_style;
                memcpy(data->ext_ul_color, transitions[t].ul_color, 3);
                data->ext_has_ul_color = transitions[t].has_ul_color;
            }
            offset = end;
        }

        return total_written;
    }

    return -1;
}

static void convert_vterm_screen_cell(const VTermScreenCell *vcell, VTermState *state,
                                      TerminalCell *cell)
{
    // Copy character data
    int i;
    for (i = 0; i < VTERM_MAX_CHARS_PER_CELL && i < TERM_MAX_CHARS_PER_CELL && vcell->chars[i] != 0;
         i++) {
        cell->chars[i] = vcell->chars[i];
    }
    if (i < TERM_MAX_CHARS_PER_CELL) {
        cell->chars[i] = 0;
    }

    // Copy width — keep libvterm's value as-is. Presentation width for
    // VS16-marked emoji is computed renderer-locally so iteration loops
    // (bg fill, span runs, selection) don't skip the cell at col+1.
    // See README.md "Emoji Width Paradigm".
    cell->width = vcell->width;

    // Copy attributes
    cell->attrs.bold = vcell->attrs.bold;
    cell->attrs.underline = vcell->attrs.underline;
    cell->attrs.italic = vcell->attrs.italic;
    cell->attrs.blink = vcell->attrs.blink;
    cell->attrs.reverse = vcell->attrs.reverse;
    cell->attrs.strikethrough = vcell->attrs.strike;
    cell->attrs.font = vcell->attrs.font;
    cell->attrs.dwl = vcell->attrs.dwl;
    cell->attrs.dhl = vcell->attrs.dhl;

    // Convert colors
    cell->fg = convert_vterm_color(&vcell->fg, state, false);
    cell->bg = convert_vterm_color(&vcell->bg, state, true);
    cell->ul_color = (TerminalColor){ 0, 0, 0, true }; // default until ext_grid overrides

    // Apply reverse video: swap fg/bg so renderer sees visual colors
    if (cell->attrs.reverse) {
        TerminalColor tmp = cell->fg;
        cell->fg = cell->bg;
        cell->bg = tmp;
        cell->bg.is_default = false;
    }
}

static int vt_get_cell(TerminalBackend *backend, int row, int col, TerminalCell *cell)
{
    if (!backend || !backend->backend_data || !cell)
        return -1;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;

    if (!data->screen)
        return -1;

    // Check bounds
    if (row < 0 || row >= data->height || col < 0 || col >= data->width)
        return -1;

    VTermScreenCell vcell;
    if (vterm_screen_get_cell(data->screen, (VTermPos){ .row = row, .col = col }, &vcell) < 0)
        return -1;

    convert_vterm_screen_cell(&vcell, data->state, cell);

    // Apply extension attributes from grid
    if (data->ext_grid) {
        CellExtAttrs *ext = &data->ext_grid[row * data->width + col];
        if (ext->underline_style > 0)
            cell->attrs.underline = ext->underline_style;
        if (ext->has_ul_color) {
            cell->ul_color.r = ext->ul_color[0];
            cell->ul_color.g = ext->ul_color[1];
            cell->ul_color.b = ext->ul_color[2];
            cell->ul_color.is_default = false;
        }
    }

    return 0;
}

static int vt_get_dimensions(TerminalBackend *backend, int *rows, int *cols)
{
    if (!backend || !backend->backend_data || !rows || !cols)
        return -1;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;

    *rows = data->height;
    *cols = data->width;
    return 0;
}

static TerminalPos vt_get_cursor_pos(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return (TerminalPos){ 0, 0 };

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    return (TerminalPos){ data->cursor_pos.row, data->cursor_pos.col };
}

static const char *vt_get_title(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return NULL;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    return data->title;
}

static bool vt_get_cursor_visible(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return true;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    return data->cursor_visible;
}

static bool vt_get_cursor_blink(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return true;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    return data->cursor_blink_enabled;
}

static bool vt_needs_redraw(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return false;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    return data->has_damage;
}

static void vt_clear_redraw(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    data->has_damage = false;
    data->damage = (TerminalDamageRect){ 0, 0, 0, 0 };
}

static bool vt_get_damage_rect(TerminalBackend *backend, TerminalDamageRect *rect)
{
    if (!backend || !backend->backend_data || !rect)
        return false;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    if (!data->has_damage)
        return false;

    *rect = data->damage;
    return true;
}

// Helper: union a VTermRect into the accumulated damage
static void damage_union(TerminalVtData *data, int start_row, int start_col, int end_row, int end_col)
{
    if (!data->has_damage) {
        data->damage.start_row = start_row;
        data->damage.start_col = start_col;
        data->damage.end_row = end_row;
        data->damage.end_col = end_col;
        data->has_damage = true;
    } else {
        if (start_row < data->damage.start_row)
            data->damage.start_row = start_row;
        if (start_col < data->damage.start_col)
            data->damage.start_col = start_col;
        if (end_row > data->damage.end_row)
            data->damage.end_row = end_row;
        if (end_col > data->damage.end_col)
            data->damage.end_col = end_col;
    }
}

// Callback implementations
static int term_damage(VTermRect rect, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;
    damage_union(data, rect.start_row, rect.start_col, rect.end_row, rect.end_col);

    // Track cell-content damage separately (excludes cursor movement)
    // for extended underline grid updates
    if (!data->has_grid_damage) {
        data->grid_damage.start_row = rect.start_row;
        data->grid_damage.start_col = rect.start_col;
        data->grid_damage.end_row = rect.end_row;
        data->grid_damage.end_col = rect.end_col;
        data->has_grid_damage = true;
    } else {
        if (rect.start_row < data->grid_damage.start_row)
            data->grid_damage.start_row = rect.start_row;
        if (rect.start_col < data->grid_damage.start_col)
            data->grid_damage.start_col = rect.start_col;
        if (rect.end_row > data->grid_damage.end_row)
            data->grid_damage.end_row = rect.end_row;
        if (rect.end_col > data->grid_damage.end_col)
            data->grid_damage.end_col = rect.end_col;
    }

    return 1;
}

static int term_moverect(VTermRect dest, VTermRect src, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;

    // Shift extension grid to follow content movement
    if (data->ext_grid && src.start_col == 0 && dest.start_col == 0 &&
        src.end_col == data->width && dest.end_col == data->width) {
        int dy = dest.start_row - src.start_row;
        if (dy < 0) {
            // Content moves up (scroll up)
            for (int row = dest.start_row; row < dest.end_row; row++)
                memcpy(&data->ext_grid[row * data->width],
                       &data->ext_grid[(row - dy) * data->width],
                       data->width * sizeof(CellExtAttrs));
        } else if (dy > 0) {
            // Content moves down (scroll down)
            for (int row = dest.end_row - 1; row >= dest.start_row; row--)
                memcpy(&data->ext_grid[row * data->width],
                       &data->ext_grid[(row - dy) * data->width],
                       data->width * sizeof(CellExtAttrs));
        }
    }

    damage_union(data, src.start_row, src.start_col, src.end_row, src.end_col);
    damage_union(data, dest.start_row, dest.start_col, dest.end_row, dest.end_col);
    return 1;
}

static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;
    (void)visible;
    data->cursor_pos = pos;
    // Damage both old and new cursor cells
    damage_union(data, oldpos.row, oldpos.col, oldpos.row + 1, oldpos.col + 1);
    damage_union(data, pos.row, pos.col, pos.row + 1, pos.col + 1);
    return 1;
}

static int term_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;

    switch (prop) {
    case VTERM_PROP_TITLE:
        vlog("Terminal set property: title = %.*s\n", val->string.len, val->string.str);
        if (data->title) {
            free(data->title);
        }
        data->title = malloc(val->string.len + 1);
        if (data->title) {
            memcpy(data->title, val->string.str, val->string.len);
            data->title[val->string.len] = '\0';
        }
        break;
    case VTERM_PROP_ICONNAME:
        break;
    case VTERM_PROP_CURSORVISIBLE:
        vlog("Cursor visibility changed to: %s\n", val->boolean ? "visible" : "hidden");
        data->cursor_visible = val->boolean;
        damage_union(data, 0, 0, data->height, data->width);
        break;
    case VTERM_PROP_CURSORBLINK:
        vlog("Cursor blink %s\n", val->boolean ? "enabled" : "disabled");
        data->cursor_blink_enabled = val->boolean;
        break;
    case VTERM_PROP_REVERSE:
        // Full screen damage for display-affecting properties
        damage_union(data, 0, 0, data->height, data->width);
        break;
    case VTERM_PROP_ALTSCREEN:
        vlog("Alternate screen %s\n", val->boolean ? "enabled" : "disabled");
        data->in_altscreen = val->boolean;
        damage_union(data, 0, 0, data->height, data->width);
        break;
    case VTERM_PROP_MOUSE:
        vlog("Mouse mode changed to: %d\n", val->number);
        data->mouse_mode = val->number;
        break;
    default:
        break;
    }

    return 1;
}

static int term_bell(void *user)
{
    (void)user;
    fprintf(stderr, "Bell!\n");
    return 1;
}

// Callback for vterm output (e.g., mouse escape sequences, keyboard input)
static void term_output_callback(const char *s, size_t len, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;
    if (data->output_cb) {
        data->output_cb(s, len, data->output_cb_user);
    }
}

static int term_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;

    // Initialize scrollback buffer if needed
    if (!data->scrollback) {
        data->scrollback = malloc(SCROLLBACK_SIZE * sizeof(ScrollbackLine));
        if (!data->scrollback)
            return 0;
        data->scrollback_capacity = SCROLLBACK_SIZE;
        data->scrollback_lines = 0;
    }

    // Expand scrollback buffer if needed
    if (data->scrollback_lines >= data->scrollback_capacity) {
        ScrollbackLine *new_scrollback = realloc(
            data->scrollback,
            (data->scrollback_capacity + SCROLLBACK_SIZE) * sizeof(ScrollbackLine));
        if (!new_scrollback)
            return 0;
        data->scrollback = new_scrollback;
        data->scrollback_capacity += SCROLLBACK_SIZE;
    }

    // Allocate and copy the line, storing original column count
    VTermScreenCell *line_cells = malloc(cols * sizeof(VTermScreenCell));
    if (!line_cells)
        return 0;

    memcpy(line_cells, cells, cols * sizeof(VTermScreenCell));

    // Copy extension attributes from row 0 of ext_grid (the line being pushed)
    CellExtAttrs *ext_cells = NULL;
    if (data->ext_grid) {
        int copy_cols = cols < data->width ? cols : data->width;
        ext_cells = malloc(cols * sizeof(CellExtAttrs));
        if (ext_cells) {
            memcpy(ext_cells, data->ext_grid, copy_cols * sizeof(CellExtAttrs));
            // Zero-fill any extra columns
            if (copy_cols < cols)
                memset(&ext_cells[copy_cols], 0, (cols - copy_cols) * sizeof(CellExtAttrs));
        }
    }

    ScrollbackLine *entry = &data->scrollback[data->scrollback_lines];
    entry->cells = line_cells;
    entry->ext_cells = ext_cells;
    entry->cols = cols;

    // Capture soft-wrap continuation flag from row 0 (the line being pushed)
    const VTermLineInfo *li = vterm_state_get_lineinfo(data->state, 0);
    entry->continuation = li ? (bool)li->continuation : false;

    data->scrollback_lines++;

    // Scroll sixel images up by one row when content scrolls
    if (data->backend)
        terminal_scroll_sixel_images(data->backend, 1);

    return 1;
}

static int term_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;

    if (data->scrollback_lines <= 0)
        return 0;

    // Get the last line from scrollback
    data->scrollback_lines--;
    ScrollbackLine *entry = &data->scrollback[data->scrollback_lines];

    // Copy cells to output, using the stored column count (not current width)
    int stored_cols = entry->cols;
    int copy_cols = (cols < stored_cols) ? cols : stored_cols;
    memcpy(cells, entry->cells, copy_cols * sizeof(VTermScreenCell));

    // Fill remaining cells with default cells if needed
    if (copy_cols < cols) {
        VTermScreenCell default_cell;
        memset(&default_cell, 0, sizeof(default_cell));
        default_cell.width = 1; // Must be 1 for valid empty cell
        for (int i = copy_cols; i < cols; i++) {
            cells[i] = default_cell;
        }
    }

    // Free the line
    free(entry->cells);
    free(entry->ext_cells);
    entry->cells = NULL;
    entry->ext_cells = NULL;
    entry->cols = 0;

    return 1;
}

static int vt_get_scrollback_lines(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return 0;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    return data->scrollback_lines;
}

static int vt_get_scrollback_cell(TerminalBackend *backend, int scrollback_row, int col,
                                  TerminalCell *cell)
{
    if (!backend || !backend->backend_data || !cell)
        return -1;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;

    // scrollback_row 0 = most recent line (map to internal scrollback_lines - 1 - scrollback_row)
    int internal_row = data->scrollback_lines - 1 - scrollback_row;
    if (internal_row < 0 || internal_row >= data->scrollback_lines)
        return -1;

    ScrollbackLine *entry = &data->scrollback[internal_row];
    if (!entry->cells)
        return -1;

    // Check column bounds against stored line width
    if (col < 0 || col >= entry->cols)
        return -1;

    convert_vterm_screen_cell(&entry->cells[col], data->state, cell);

    // Apply extension attributes from scrollback
    if (entry->ext_cells) {
        CellExtAttrs *ext = &entry->ext_cells[col];
        if (ext->underline_style > 0)
            cell->attrs.underline = ext->underline_style;
        if (ext->has_ul_color) {
            cell->ul_color.r = ext->ul_color[0];
            cell->ul_color.g = ext->ul_color[1];
            cell->ul_color.b = ext->ul_color[2];
            cell->ul_color.is_default = false;
        }
    }
    return 0;
}

static bool vt_is_altscreen(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return false;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    return data->in_altscreen;
}

static int vt_get_mouse_mode(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return 0;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    return data->mouse_mode;
}

static void vt_send_mouse_event(TerminalBackend *backend, int row, int col, int button,
                                bool pressed, int mod)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    if (!data->vt)
        return;

    // Map modifier keys to VTermModifier
    VTermModifier vtmod = VTERM_MOD_NONE;
    if (mod & 0x01)
        vtmod |= VTERM_MOD_SHIFT; // SDL_KMOD_SHIFT
    if (mod & 0x40)
        vtmod |= VTERM_MOD_CTRL; // SDL_KMOD_CTRL
    if (mod & 0x100)
        vtmod |= VTERM_MOD_ALT; // SDL_KMOD_ALT

    // Update mouse position
    vterm_mouse_move(data->vt, row, col, vtmod);

    // Send button event if applicable
    if (button > 0) {
        vterm_mouse_button(data->vt, button, pressed, vtmod);
    }
}

static void vt_set_output_callback(TerminalBackend *backend, TerminalOutputCallback cb, void *user)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    data->output_cb = cb;
    data->output_cb_user = user;
}

// Map TERM_KEY_* to VTermKey
static VTermKey term_key_to_vterm(int key)
{
    switch (key) {
    case TERM_KEY_ENTER:
        return VTERM_KEY_ENTER;
    case TERM_KEY_TAB:
        return VTERM_KEY_TAB;
    case TERM_KEY_BACKSPACE:
        return VTERM_KEY_BACKSPACE;
    case TERM_KEY_ESCAPE:
        return VTERM_KEY_ESCAPE;
    case TERM_KEY_UP:
        return VTERM_KEY_UP;
    case TERM_KEY_DOWN:
        return VTERM_KEY_DOWN;
    case TERM_KEY_LEFT:
        return VTERM_KEY_LEFT;
    case TERM_KEY_RIGHT:
        return VTERM_KEY_RIGHT;
    case TERM_KEY_INS:
        return VTERM_KEY_INS;
    case TERM_KEY_DEL:
        return VTERM_KEY_DEL;
    case TERM_KEY_HOME:
        return VTERM_KEY_HOME;
    case TERM_KEY_END:
        return VTERM_KEY_END;
    case TERM_KEY_PAGEUP:
        return VTERM_KEY_PAGEUP;
    case TERM_KEY_PAGEDOWN:
        return VTERM_KEY_PAGEDOWN;
    case TERM_KEY_F1:
        return VTERM_KEY_FUNCTION(1);
    case TERM_KEY_F2:
        return VTERM_KEY_FUNCTION(2);
    case TERM_KEY_F3:
        return VTERM_KEY_FUNCTION(3);
    case TERM_KEY_F4:
        return VTERM_KEY_FUNCTION(4);
    case TERM_KEY_F5:
        return VTERM_KEY_FUNCTION(5);
    case TERM_KEY_F6:
        return VTERM_KEY_FUNCTION(6);
    case TERM_KEY_F7:
        return VTERM_KEY_FUNCTION(7);
    case TERM_KEY_F8:
        return VTERM_KEY_FUNCTION(8);
    case TERM_KEY_F9:
        return VTERM_KEY_FUNCTION(9);
    case TERM_KEY_F10:
        return VTERM_KEY_FUNCTION(10);
    case TERM_KEY_F11:
        return VTERM_KEY_FUNCTION(11);
    case TERM_KEY_F12:
        return VTERM_KEY_FUNCTION(12);
    case TERM_KEY_KP_0:
        return VTERM_KEY_KP_0;
    case TERM_KEY_KP_1:
        return VTERM_KEY_KP_1;
    case TERM_KEY_KP_2:
        return VTERM_KEY_KP_2;
    case TERM_KEY_KP_3:
        return VTERM_KEY_KP_3;
    case TERM_KEY_KP_4:
        return VTERM_KEY_KP_4;
    case TERM_KEY_KP_5:
        return VTERM_KEY_KP_5;
    case TERM_KEY_KP_6:
        return VTERM_KEY_KP_6;
    case TERM_KEY_KP_7:
        return VTERM_KEY_KP_7;
    case TERM_KEY_KP_8:
        return VTERM_KEY_KP_8;
    case TERM_KEY_KP_9:
        return VTERM_KEY_KP_9;
    case TERM_KEY_KP_MULTIPLY:
        return VTERM_KEY_KP_MULT;
    case TERM_KEY_KP_PLUS:
        return VTERM_KEY_KP_PLUS;
    case TERM_KEY_KP_COMMA:
        return VTERM_KEY_KP_COMMA;
    case TERM_KEY_KP_MINUS:
        return VTERM_KEY_KP_MINUS;
    case TERM_KEY_KP_PERIOD:
        return VTERM_KEY_KP_PERIOD;
    case TERM_KEY_KP_DIVIDE:
        return VTERM_KEY_KP_DIVIDE;
    case TERM_KEY_KP_ENTER:
        return VTERM_KEY_KP_ENTER;
    case TERM_KEY_KP_EQUAL:
        return VTERM_KEY_KP_EQUAL;
    default:
        return VTERM_KEY_NONE;
    }
}

static void vt_send_key(TerminalBackend *backend, int key, int mod)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    VTermKey vk = term_key_to_vterm(key);
    if (vk == VTERM_KEY_NONE)
        return;

    // TERM_MOD_* values match VTermModifier values by design
    vterm_keyboard_key(data->vt, vk, (VTermModifier)mod);
}

static void vt_send_char(TerminalBackend *backend, uint32_t codepoint, int mod)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    vterm_keyboard_unichar(data->vt, codepoint, (VTermModifier)mod);
}

static void vt_start_paste(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    vterm_keyboard_start_paste(data->vt);
}

static void vt_end_paste(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    vterm_keyboard_end_paste(data->vt);
}

static bool vt_get_line_continuation(TerminalBackend *backend, int row)
{
    if (!backend || !backend->backend_data)
        return false;
    TerminalVtData *data = (TerminalVtData *)backend->backend_data;

    if (row >= 0) {
        // Visible row: query state directly
        const VTermLineInfo *li = vterm_state_get_lineinfo(data->state, row);
        return li ? (bool)li->continuation : false;
    } else {
        // Scrollback row: unified row -1 = scrollback index 0 = most recent
        int scrollback_index = -(row + 1);
        int internal_row = data->scrollback_lines - 1 - scrollback_index;
        if (internal_row < 0 || internal_row >= data->scrollback_lines)
            return false;
        return data->scrollback[internal_row].continuation;
    }
}

static void vt_set_reflow(TerminalBackend *backend, bool enabled)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    if (data->screen)
        vterm_screen_enable_reflow(data->screen, enabled);
}

// Global backend instance
TerminalBackend terminal_backend_vt = {
    .name = "libvterm",
    .backend_data = NULL,
    .init = vt_init,
    .destroy = vt_destroy,
    .resize = vt_resize,
    .process_input = vt_process_input,
    .get_cell = vt_get_cell,
    .get_dimensions = vt_get_dimensions,
    .get_cursor_pos = vt_get_cursor_pos,
    .get_cursor_visible = vt_get_cursor_visible,
    .get_cursor_blink = vt_get_cursor_blink,
    .get_title = vt_get_title,
    .needs_redraw = vt_needs_redraw,
    .clear_redraw = vt_clear_redraw,
    .get_damage_rect = vt_get_damage_rect,
    .get_scrollback_lines = vt_get_scrollback_lines,
    .get_scrollback_cell = vt_get_scrollback_cell,
    .is_altscreen = vt_is_altscreen,
    .get_mouse_mode = vt_get_mouse_mode,
    .send_mouse_event = vt_send_mouse_event,
    .set_output_callback = vt_set_output_callback,
    .send_key = vt_send_key,
    .send_char = vt_send_char,
    .start_paste = vt_start_paste,
    .end_paste = vt_end_paste,
    .set_reflow = vt_set_reflow,
    .get_line_continuation = vt_get_line_continuation,
};
