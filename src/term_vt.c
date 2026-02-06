#include "term_vt.h"
#include "common.h"
#include "term.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vterm.h>

#define SCROLLBACK_SIZE 1000

// Scrollback line entry - stores cells with original column count
typedef struct
{
    VTermScreenCell *cells;
    int cols;
} ScrollbackLine;

// Default ANSI 16-color palette based on charmbracelet/vhs theme
// clang-format off
static const uint8_t default_palette[16][3] = {
    [0]  = { 0x28, 0x2a, 0x2e }, // Black
    [1]  = { 0xd7, 0x4e, 0x6f }, // Red
    [2]  = { 0x31, 0xbb, 0x71 }, // Green
    [3]  = { 0xd3, 0xe5, 0x61 }, // Yellow
    [4]  = { 0x80, 0x56, 0xff }, // Blue (Charm purple)
    [5]  = { 0xed, 0x61, 0xd7 }, // Magenta
    [6]  = { 0x04, 0xd7, 0xd7 }, // Cyan
    [7]  = { 0xbf, 0xbf, 0xbf }, // White
    [8]  = { 0x4d, 0x4d, 0x4d }, // Bright Black
    [9]  = { 0xfe, 0x5f, 0x86 }, // Bright Red
    [10] = { 0x00, 0xd7, 0x87 }, // Bright Green
    [11] = { 0xeb, 0xff, 0x71 }, // Bright Yellow
    [12] = { 0x9b, 0x79, 0xff }, // Bright Blue (lavender)
    [13] = { 0xff, 0x7a, 0xea }, // Bright Magenta
    [14] = { 0x00, 0xfe, 0xfe }, // Bright Cyan
    [15] = { 0xe6, 0xe6, 0xe6 }, // Bright White
};
// clang-format on

static const uint8_t default_fg[3] = { 0xf8, 0xf8, 0xf2 };
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
        for (int i = 0; i < data->scrollback_lines; i++)
            free(data->scrollback[i].cells);
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
        int written = vterm_input_write(data->vt, input, len);
        vterm_screen_flush_damage(data->screen);
        return written;
    }

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
    return 1;
}

static int term_moverect(VTermRect dest, VTermRect src, void *user)
{
    TerminalVtData *data = (TerminalVtData *)user;
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

    ScrollbackLine *entry = &data->scrollback[data->scrollback_lines];
    entry->cells = line_cells;
    entry->cols = cols;
    data->scrollback_lines++;

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
    entry->cells = NULL;
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

    VTermScreenCell *vcell = &entry->cells[col];

    // Copy character data
    int i;
    for (i = 0; i < VTERM_MAX_CHARS_PER_CELL && i < TERM_MAX_CHARS_PER_CELL && vcell->chars[i] != 0;
         i++) {
        cell->chars[i] = vcell->chars[i];
    }
    if (i < TERM_MAX_CHARS_PER_CELL) {
        cell->chars[i] = 0;
    }

    // Copy width
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
    cell->fg = convert_vterm_color(&vcell->fg, data->state, false);
    cell->bg = convert_vterm_color(&vcell->bg, data->state, true);

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
};
