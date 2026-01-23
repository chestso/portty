#include "common.h"
#include "term.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vterm.h>

#define SCROLLBACK_SIZE 1000

struct Terminal
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
};

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

Terminal *terminal_init(int width, int height)
{
    Terminal *term = malloc(sizeof(Terminal));
    if (!term) {
        return NULL;
    }

    term->width = width;
    term->height = height;
    term->need_redraw = 0;
    term->cursor_pos.row = 0;
    term->cursor_pos.col = 0;
    term->title = NULL;
    term->scrollback = NULL;
    term->scrollback_lines = 0;
    term->scrollback_capacity = 0;

    term->vt = vterm_new(height, width);
    if (!term->vt) {
        free(term);
        return NULL;
    }

    vterm_set_utf8(term->vt, 1);

    term->screen = vterm_obtain_screen(term->vt);
    term->state = vterm_obtain_state(term->vt);
    vterm_screen_enable_altscreen(term->screen, 1);
    vterm_screen_set_callbacks(term->screen, &cb, term);
    vterm_screen_set_damage_merge(term->screen, VTERM_DAMAGE_SCROLL);

    vterm_screen_reset(term->screen, 1);

    return term;
}

void terminal_destroy(Terminal *term)
{
    if (term) {
        if (term->vt) {
            vterm_free(term->vt);
        }
        if (term->title) {
            free(term->title);
        }
        if (term->scrollback) {
            for (int i = 0; i < term->scrollback_lines; i++) {
                free(term->scrollback[i]);
            }
            free(term->scrollback);
        }
        free(term);
    }
}

void terminal_resize(Terminal *term, int width, int height)
{
    if (term && term->vt) {
        term->width = width;
        term->height = height;
        vterm_set_size(term->vt, height, width);
        vterm_screen_flush_damage(term->screen);
    }
}

int terminal_process_input(Terminal *term, const char *input, size_t len)
{
    if (term && term->vt) {
        return vterm_input_write(term->vt, input, len);
    }
    return -1;
}

int terminal_get_cell(Terminal *term, int row, int col, TerminalCell *cell)
{
    if (!term || !term->screen || !cell) {
        return -1;
    }

    int rows, cols;
    terminal_get_dimensions(term, &rows, &cols);
    if (row < 0 || row >= rows || col < 0 || col >= cols) {
        return -1;
    }

    VTermScreenCell vcell;
    if (vterm_screen_get_cell(term->screen, (VTermPos){ .row = row, .col = col }, &vcell) < 0) {
        return -1;
    }

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
    cell->fg = convert_vterm_color(&vcell.fg, term->state, false);
    cell->bg = convert_vterm_color(&vcell.bg, term->state, true);

    return 0;
}

int terminal_get_dimensions(Terminal *term, int *rows, int *cols)
{
    if (!term || !rows || !cols) {
        return -1;
    }

    *rows = term->height;
    *cols = term->width;
    return 0;
}

TerminalPos terminal_get_cursor_pos(Terminal *term)
{
    if (!term) {
        return (TerminalPos){ 0, 0 };
    }
    return (TerminalPos){ term->cursor_pos.row, term->cursor_pos.col };
}

const char *terminal_get_title(Terminal *term)
{
    if (!term) {
        return NULL;
    }
    return term->title;
}

bool terminal_needs_redraw(Terminal *term)
{
    if (!term) {
        return false;
    }
    return term->need_redraw != 0;
}

void terminal_clear_redraw(Terminal *term)
{
    if (term) {
        term->need_redraw = 0;
    }
}

// Callback implementations
static int term_damage(VTermRect rect, void *user)
{
    Terminal *term = (Terminal *)user;
    vlog("Terminal damage callback: rect=(%d,%d)-(%d,%d)\n",
         rect.start_row, rect.start_col, rect.end_row, rect.end_col);
    term->need_redraw = 1;
    return 1;
}

static int term_moverect(VTermRect dest, VTermRect src, void *user)
{
    Terminal *term = (Terminal *)user;
    vlog("Terminal move rectangle callback: src=(%d,%d)-(%d,%d) dest=(%d,%d)-(%d,%d)\n",
         src.start_row, src.start_col, src.end_row, src.end_col,
         dest.start_row, dest.start_col, dest.end_row, dest.end_col);
    term->need_redraw = 1;
    return 1;
}

static int term_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    Terminal *term = (Terminal *)user;
    vlog("Terminal move cursor callback: pos=(%d,%d) oldpos=(%d,%d) visible=%d\n",
         pos.row, pos.col, oldpos.row, oldpos.col, visible);
    term->cursor_pos = pos;
    term->need_redraw = 1;
    return 1;
}

static int term_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    Terminal *term = (Terminal *)user;

    switch (prop) {
    case VTERM_PROP_TITLE:
        vlog("Terminal set property: title = %.*s\n", val->string.len, val->string.str);
        if (term->title) {
            free(term->title);
        }
        term->title = malloc(val->string.len + 1);
        if (term->title) {
            memcpy(term->title, val->string.str, val->string.len);
            term->title[val->string.len] = '\0';
        }
        break;
    case VTERM_PROP_ICONNAME:
        vlog("Terminal set property: icon name = %.*s\n", val->string.len, val->string.str);
        break;
    case VTERM_PROP_CURSORVISIBLE:
        vlog("Terminal set property: cursor visible = %d\n", val->boolean);
        term->need_redraw = 1;
        break;
    case VTERM_PROP_CURSORBLINK:
        vlog("Terminal set property: cursor blink = %d\n", val->boolean);
        term->need_redraw = 1;
        break;
    case VTERM_PROP_REVERSE:
        vlog("Terminal set property: reverse video = %d\n", val->boolean);
        term->need_redraw = 1;
        break;
    case VTERM_PROP_ALTSCREEN:
        vlog("Terminal set property: alt screen = %d\n", val->boolean);
        term->need_redraw = 1;
        break;
    default:
        vlog("Terminal set property: unknown property %d\n", prop);
        break;
    }

    term->need_redraw = 1;
    return 1;
}

static int term_bell(void *user)
{
    Terminal *term = (Terminal *)user;
    (void)term;
    vlog("Terminal bell callback\n");
    fprintf(stderr, "Bell!\n");
    return 1;
}

static int term_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    Terminal *term = (Terminal *)user;

    vlog("Terminal scrollback push line: cols=%d, current_lines=%d\n", cols, term->scrollback_lines);

    // Initialize scrollback buffer if needed
    if (!term->scrollback) {
        term->scrollback = malloc(SCROLLBACK_SIZE * sizeof(VTermScreenCell *));
        if (!term->scrollback) {
            vlog("Failed to allocate initial scrollback buffer\n");
            return 0;
        }
        term->scrollback_capacity = SCROLLBACK_SIZE;
        term->scrollback_lines = 0;
        vlog("Allocated initial scrollback buffer with capacity %d\n", SCROLLBACK_SIZE);
    }

    // Expand scrollback buffer if needed
    if (term->scrollback_lines >= term->scrollback_capacity) {
        VTermScreenCell **new_scrollback = realloc(
            term->scrollback,
            (term->scrollback_capacity + SCROLLBACK_SIZE) * sizeof(VTermScreenCell *));
        if (!new_scrollback) {
            vlog("Failed to expand scrollback buffer\n");
            return 0;
        }
        term->scrollback = new_scrollback;
        term->scrollback_capacity += SCROLLBACK_SIZE;
        vlog("Expanded scrollback buffer to capacity %d\n", term->scrollback_capacity);
    }

    // Allocate and copy the line
    VTermScreenCell *line = malloc(cols * sizeof(VTermScreenCell));
    if (!line) {
        vlog("Failed to allocate line for scrollback\n");
        return 0;
    }

    memcpy(line, cells, cols * sizeof(VTermScreenCell));

    // Add to scrollback buffer
    term->scrollback[term->scrollback_lines] = line;
    term->scrollback_lines++;

    vlog("Successfully pushed line to scrollback, now %d lines\n", term->scrollback_lines);

    return 1;
}

static int term_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    Terminal *term = (Terminal *)user;

    vlog("Terminal scrollback pop line: cols=%d, current_lines=%d\n", cols, term->scrollback_lines);

    if (term->scrollback_lines <= 0) {
        vlog("No lines in scrollback to pop\n");
        return 0;
    }

    // Get the last line from scrollback
    term->scrollback_lines--;
    VTermScreenCell *line = term->scrollback[term->scrollback_lines];

    // Copy cells to output
    int copy_cols = (cols < term->width) ? cols : term->width;
    memcpy(cells, line, copy_cols * sizeof(VTermScreenCell));

    // Fill remaining cells with default cells if needed
    if (copy_cols < cols) {
        VTermScreenCell default_cell;
        VTermPos pos = { .row = 0, .col = 0 };
        vterm_screen_get_cell(term->screen, pos, &default_cell);
        default_cell.chars[0] = 0;
        for (int i = copy_cols; i < cols; i++) {
            cells[i] = default_cell;
        }
    }

    // Free the line
    free(line);
    term->scrollback[term->scrollback_lines] = NULL;

    vlog("Successfully popped line from scrollback, now %d lines\n", term->scrollback_lines);

    return 1;
}
