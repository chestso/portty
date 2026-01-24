#include "term_vt.h"
#include "common.h"
#include "term.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vterm.h>

#define SCROLLBACK_SIZE 1000

typedef struct TerminalVtData
{
    VTerm *vt;
    VTermState *state;
    VTermScreen *screen;
    int width;
    int height;
    int need_redraw;
    VTermPos cursor_pos;
    char *title;
    VTermScreenCell **scrollback;
    int scrollback_lines;
    int scrollback_capacity;
} TerminalVtData;

// Forward declarations for callback functions
static int term_damage(VTermRect rect, void *user);
static int term_moverect(VTermRect dest, VTermRect src, void *user);
static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user);
static int term_settermprop(VTermProp prop, VTermValue *val, void *user);
static int term_bell(void *user);
static int term_sb_pushline(int cols, const VTermScreenCell *cells, void *user);
static int term_sb_popline(int cols, VTermScreenCell *cells, void *user);

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
            result.r = 0;
            result.g = 0;
            result.b = 0;
        } else {
            result.r = 255;
            result.g = 255;
            result.b = 255;
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
        result.r = is_bg ? 0 : 255;
        result.g = is_bg ? 0 : 255;
        result.b = is_bg ? 0 : 255;
    }

    return result;
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
    data->need_redraw = 0;
    data->cursor_pos.row = 0;
    data->cursor_pos.col = 0;
    data->title = NULL;
    data->scrollback = NULL;
    data->scrollback_lines = 0;
    data->scrollback_capacity = 0;

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

    vterm_screen_reset(data->screen, 1);

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
        for (int i = 0; i < data->scrollback_lines; i++)
            free(data->scrollback[i]);
        free(data->scrollback);
    }

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
    }
}

static int vt_process_input(TerminalBackend *backend, const char *input, size_t len)
{
    if (!backend || !backend->backend_data)
        return -1;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;

    if (data->vt)
        return vterm_input_write(data->vt, input, len);

    return -1;
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

    // Copy character data
    int i;
    for (i = 0; i < VTERM_MAX_CHARS_PER_CELL && i < TERM_MAX_CHARS_PER_CELL && vcell.chars[i] != 0; i++) {
        cell->chars[i] = vcell.chars[i];
    }
    if (i < TERM_MAX_CHARS_PER_CELL) {
        cell->chars[i] = 0;
    }

    // Copy width
    cell->width = vcell.width;

    // Copy attributes
    cell->attrs.bold = vcell.attrs.bold;
    cell->attrs.underline = vcell.attrs.underline;
    cell->attrs.italic = vcell.attrs.italic;
    cell->attrs.blink = vcell.attrs.blink;
    cell->attrs.reverse = vcell.attrs.reverse;
    cell->attrs.strikethrough = vcell.attrs.strike;
    cell->attrs.font = vcell.attrs.font;
    cell->attrs.dwl = vcell.attrs.dwl;
    cell->attrs.dhl = vcell.attrs.dhl;

    // Convert colors
    cell->fg = convert_vterm_color(&vcell.fg, data->state, false);
    cell->bg = convert_vterm_color(&vcell.bg, data->state, true);

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

static bool vt_needs_redraw(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return false;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    return data->need_redraw != 0;
}

static void vt_clear_redraw(TerminalBackend *backend)
{
    if (!backend || !backend->backend_data)
        return;

    TerminalVtData *data = (TerminalVtData *)backend->backend_data;
    data->need_redraw = 0;
}

// Callback implementations
static int term_damage(VTermRect rect, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;
    vlog("Terminal damage callback: rect=(%d,%d)-(%d,%d)\n",
         rect.start_row, rect.start_col, rect.end_row, rect.end_col);
    data->need_redraw = 1;
    return 1;
}

static int term_moverect(VTermRect dest, VTermRect src, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;
    vlog("Terminal move rectangle callback: src=(%d,%d)-(%d,%d) dest=(%d,%d)-(%d,%d)\n",
         src.start_row, src.start_col, src.end_row, src.end_col,
         dest.start_row, dest.start_col, dest.end_row, dest.end_col);
    data->need_redraw = 1;
    return 1;
}

static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;
    vlog("Terminal move cursor callback: pos=(%d,%d) oldpos=(%d,%d) visible=%d\n",
         pos.row, pos.col, oldpos.row, oldpos.col, visible);
    data->cursor_pos = pos;
    data->need_redraw = 1;
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
        vlog("Terminal set property: icon name = %.*s\n", val->string.len, val->string.str);
        break;
    case VTERM_PROP_CURSORVISIBLE:
        vlog("Terminal set property: cursor visible = %d\n", val->boolean);
        data->need_redraw = 1;
        break;
    case VTERM_PROP_CURSORBLINK:
        vlog("Terminal set property: cursor blink = %d\n", val->boolean);
        data->need_redraw = 1;
        break;
    case VTERM_PROP_REVERSE:
        vlog("Terminal set property: reverse video = %d\n", val->boolean);
        data->need_redraw = 1;
        break;
    case VTERM_PROP_ALTSCREEN:
        vlog("Terminal set property: alt screen = %d\n", val->boolean);
        data->need_redraw = 1;
        break;
    default:
        vlog("Terminal set property: unknown property %d\n", prop);
        break;
    }

    data->need_redraw = 1;
    return 1;
}

static int term_bell(void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;
    (void)data;
    vlog("Terminal bell callback\n");
    fprintf(stderr, "Bell!\n");
    return 1;
}

static int term_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;

    vlog("Terminal scrollback push line: cols=%d, current_lines=%d\n", cols, data->scrollback_lines);

    // Initialize scrollback buffer if needed
    if (!data->scrollback) {
        data->scrollback = malloc(SCROLLBACK_SIZE * sizeof(VTermScreenCell *));
        if (!data->scrollback) {
            vlog("Failed to allocate initial scrollback buffer\n");
            return 0;
        }
        data->scrollback_capacity = SCROLLBACK_SIZE;
        data->scrollback_lines = 0;
        vlog("Allocated initial scrollback buffer with capacity %d\n", SCROLLBACK_SIZE);
    }

    // Expand scrollback buffer if needed
    if (data->scrollback_lines >= data->scrollback_capacity) {
        VTermScreenCell **new_scrollback = realloc(
            data->scrollback,
            (data->scrollback_capacity + SCROLLBACK_SIZE) * sizeof(VTermScreenCell *));
        if (!new_scrollback) {
            vlog("Failed to expand scrollback buffer\n");
            return 0;
        }
        data->scrollback = new_scrollback;
        data->scrollback_capacity += SCROLLBACK_SIZE;
        vlog("Expanded scrollback buffer to capacity %d\n", data->scrollback_capacity);
    }

    // Allocate and copy the line
    VTermScreenCell *line = malloc(cols * sizeof(VTermScreenCell));
    if (!line) {
        vlog("Failed to allocate line for scrollback\n");
        return 0;
    }

    memcpy(line, cells, cols * sizeof(VTermScreenCell));

    // Add to scrollback buffer
    data->scrollback[data->scrollback_lines] = line;
    data->scrollback_lines++;

    vlog("Successfully pushed line to scrollback, now %d lines\n", data->scrollback_lines);

    return 1;
}

static int term_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;

    vlog("Terminal scrollback pop line: cols=%d, current_lines=%d\n", cols, data->scrollback_lines);

    if (data->scrollback_lines <= 0) {
        vlog("No lines in scrollback to pop\n");
        return 0;
    }

    // Get the last line from scrollback
    data->scrollback_lines--;
    VTermScreenCell *line = data->scrollback[data->scrollback_lines];

    // Copy cells to output
    int copy_cols = (cols < data->width) ? cols : data->width;
    memcpy(cells, line, copy_cols * sizeof(VTermScreenCell));

    // Fill remaining cells with default cells if needed
    if (copy_cols < cols) {
        VTermScreenCell default_cell;
        VTermPos pos = { .row = 0, .col = 0 };
        vterm_screen_get_cell(data->screen, pos, &default_cell);
        default_cell.chars[0] = 0;
        for (int i = copy_cols; i < cols; i++) {
            cells[i] = default_cell;
        }
    }

    // Free the line
    free(line);
    data->scrollback[data->scrollback_lines] = NULL;

    vlog("Successfully popped line from scrollback, now %d lines\n", data->scrollback_lines);

    return 1;
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
    .get_title = vt_get_title,
    .needs_redraw = vt_needs_redraw,
    .clear_redraw = vt_clear_redraw
};
