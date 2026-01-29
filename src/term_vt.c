#include "term_vt.h"
#include "common.h"
#include "term.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vterm.h>

#define SCROLLBACK_SIZE 1000

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
    VTermScreenCell **scrollback;
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
    vterm_screen_enable_reflow(data->screen, true);

    // Set up output callback for mouse escape sequences
    vterm_output_set_callback(data->vt, term_output_callback, data);

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

// Callback for vterm output (e.g., mouse escape sequences)
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
        data->scrollback = malloc(SCROLLBACK_SIZE * sizeof(VTermScreenCell *));
        if (!data->scrollback)
            return 0;
        data->scrollback_capacity = SCROLLBACK_SIZE;
        data->scrollback_lines = 0;
    }

    // Expand scrollback buffer if needed
    if (data->scrollback_lines >= data->scrollback_capacity) {
        VTermScreenCell **new_scrollback = realloc(
            data->scrollback,
            (data->scrollback_capacity + SCROLLBACK_SIZE) * sizeof(VTermScreenCell *));
        if (!new_scrollback)
            return 0;
        data->scrollback = new_scrollback;
        data->scrollback_capacity += SCROLLBACK_SIZE;
    }

    // Allocate and copy the line
    VTermScreenCell *line = malloc(cols * sizeof(VTermScreenCell));
    if (!line)
        return 0;

    memcpy(line, cells, cols * sizeof(VTermScreenCell));

    data->scrollback[data->scrollback_lines] = line;
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
    if (col < 0 || col >= data->width)
        return -1;

    VTermScreenCell *line = data->scrollback[internal_row];
    if (!line)
        return -1;

    VTermScreenCell *vcell = &line[col];

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
};
