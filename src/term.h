#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sixel.h"

#define TERM_MAX_SIXEL_IMAGES   64

// Forward declarations
struct TerminalBackend;
typedef struct TerminalBackend TerminalBackend;

// Callback type for terminal output (e.g., mouse escape sequences to send to PTY)
typedef void (*TerminalOutputCallback)(const char *data, size_t len, void *user);

// Callback fired when selection becomes active or inactive
typedef void (*TerminalSelectionChangeFn)(bool active, void *user_data);

typedef struct
{
    int row;
    int col;
} TerminalPos;

typedef struct
{
    int start_row, start_col;
    int end_row, end_col;
} TerminalDamageRect;

typedef enum
{
    TERM_SELECT_NONE = 0,
    TERM_SELECT_CHAR, // click-drag
    TERM_SELECT_WORD, // double-click
    TERM_SELECT_LINE, // triple-click
} TerminalSelectMode;

typedef struct
{
    bool active;
    TerminalSelectMode mode;
    TerminalPos anchor; // original click point
    TerminalPos start;  // normalized start (always <= end)
    TerminalPos end;    // normalized end (always >= start)
    char *word_chars;   // configurable word character set
} TerminalSelection;

typedef struct
{
    uint8_t r, g, b;
    bool is_default;
} TerminalColor;

typedef struct
{
    unsigned int bold : 1;
    unsigned int underline : 3;
    unsigned int italic : 1;
    unsigned int blink : 1;
    unsigned int reverse : 1;
    unsigned int strikethrough : 1;
    unsigned int font : 4;
    unsigned int dwl : 1;
    unsigned int dhl : 2;
} TerminalCellAttr;

typedef struct
{
    // Primary codepoint of the cell's grapheme cluster. 0 for an empty cell.
    uint32_t cp;
    // Opaque cluster handle. 0 ⇒ single-codepoint cluster, use `cp` directly.
    // Non-zero ⇒ multi-codepoint cluster (emoji ZWJ, flag, long combining
    // run); fetch the full sequence with `terminal_cell_get_grapheme(term,
    // row, col, ...)`. The value is backend-private and lifetime-tied to
    // the cell at (row, col) — never store across reads.
    uint32_t grapheme_id;
    int width;
    TerminalCellAttr attrs;
    TerminalColor fg;
    TerminalColor bg;
    TerminalColor ul_color; // per-cell underline color (is_default=true → use theme default)
} TerminalCell;

// Backend interface definition
struct TerminalBackend
{
    const char *name;

    // Backend-specific data
    void *backend_data;

    // Application-level selection state (not backend-specific)
    TerminalSelection selection;
    TerminalSelectionChangeFn selection_change_cb;
    void *selection_change_data;

    // Application-level sixel image storage
    SixelImage *sixel_images[TERM_MAX_SIXEL_IMAGES];
    int sixel_image_count;

    // Backend function pointers
    bool (*init)(TerminalBackend *term, int width, int height);
    void (*destroy)(TerminalBackend *term);
    void (*resize)(TerminalBackend *term, int width, int height);
    int (*process_input)(TerminalBackend *term, const char *input, size_t len);
    int (*get_cell)(TerminalBackend *term, int row, int col, TerminalCell *cell);
    int (*get_dimensions)(TerminalBackend *term, int *rows, int *cols);
    TerminalPos (*get_cursor_pos)(TerminalBackend *term);
    bool (*get_cursor_visible)(TerminalBackend *term);
    bool (*get_cursor_blink)(TerminalBackend *term);
    const char *(*get_title)(TerminalBackend *term);
    bool (*needs_redraw)(TerminalBackend *term);
    void (*clear_redraw)(TerminalBackend *term);
    bool (*get_damage_rect)(TerminalBackend *term, TerminalDamageRect *rect);
    int (*get_scrollback_lines)(TerminalBackend *term);
    int (*get_scrollback_cell)(TerminalBackend *term, int scrollback_row, int col,
                               TerminalCell *cell);
    /* Read the full codepoint sequence for a multi-codepoint cluster.
     * `unified_row` follows the same convention as the renderer: visible
     * rows >= 0, scrollback rows < 0 (-1 = most recent). For
     * single-codepoint cells the caller should use `cell.cp` directly
     * instead of going through this hook. Returns the number of
     * codepoints written (clamped to `cap`). */
    size_t (*get_grapheme)(TerminalBackend *term, int unified_row, int col,
                           uint32_t *out, size_t cap);

    // Alternate screen and mouse mode support
    bool (*is_altscreen)(TerminalBackend *term);
    int (*get_mouse_mode)(TerminalBackend *term);
    void (*send_mouse_event)(TerminalBackend *term, int row, int col, int button, bool pressed,
                             int mod);
    void (*set_output_callback)(TerminalBackend *term, TerminalOutputCallback cb, void *user);

    // Keyboard input via terminal emulator (handles DECCKM, modifiers, etc.)
    void (*send_key)(TerminalBackend *term, int key, int mod);
    void (*send_char)(TerminalBackend *term, uint32_t codepoint, int mod);

    // Bracketed paste support
    void (*start_paste)(TerminalBackend *term);
    void (*end_paste)(TerminalBackend *term);

    // Reflow setting
    void (*set_reflow)(TerminalBackend *term, bool enabled);

    // Line continuation (soft-wrap) query
    bool (*get_line_continuation)(TerminalBackend *term, int row);
};

TerminalBackend *terminal_init(TerminalBackend *term, int width, int height);
void terminal_destroy(TerminalBackend *term);
void terminal_resize(TerminalBackend *term, int width, int height);
int terminal_process_input(TerminalBackend *term, const char *input, size_t len);
int terminal_get_cell(TerminalBackend *term, int row, int col, TerminalCell *cell);
int terminal_get_dimensions(TerminalBackend *term, int *rows, int *cols);
TerminalPos terminal_get_cursor_pos(TerminalBackend *term);
bool terminal_get_cursor_visible(TerminalBackend *term);
bool terminal_get_cursor_blink(TerminalBackend *term);
const char *terminal_get_title(TerminalBackend *term);
bool terminal_needs_redraw(TerminalBackend *term);
void terminal_clear_redraw(TerminalBackend *term);
bool terminal_get_damage_rect(TerminalBackend *term, TerminalDamageRect *rect);
int terminal_get_scrollback_lines(TerminalBackend *term);
int terminal_get_scrollback_cell(TerminalBackend *term, int scrollback_row, int col,
                                 TerminalCell *cell);
/* Fill `out` with the full codepoint sequence at unified row `unified_row`
 * (visible >= 0, scrollback < 0; -1 == most recent). Returns the count
 * written (clamped to `cap`). For single-codepoint cells, prefer
 * `cell.cp` directly — this entry exists for the multi-cp clusters bvt
 * stores in its grapheme arena (ZWJ, flags, long combining runs, etc.). */
size_t terminal_cell_get_grapheme(TerminalBackend *term, int unified_row, int col,
                                  uint32_t *out, size_t cap);

// Alternate screen and mouse mode support
bool terminal_is_altscreen(TerminalBackend *term);
int terminal_get_mouse_mode(TerminalBackend *term);
void terminal_send_mouse_event(TerminalBackend *term, int row, int col, int button, bool pressed,
                               int mod);
void terminal_set_output_callback(TerminalBackend *term, TerminalOutputCallback cb, void *user);

// Keyboard input via terminal emulator (handles DECCKM, modifiers, etc.)
// Modifier flags (match VTermModifier values)
#define TERM_MOD_NONE  0x00
#define TERM_MOD_SHIFT 0x01
#define TERM_MOD_ALT   0x02
#define TERM_MOD_CTRL  0x04

// Key codes for special keys (match VTermKey values)
enum
{
    TERM_KEY_NONE = 0,
    TERM_KEY_ENTER,
    TERM_KEY_TAB,
    TERM_KEY_BACKSPACE,
    TERM_KEY_ESCAPE,
    TERM_KEY_UP,
    TERM_KEY_DOWN,
    TERM_KEY_LEFT,
    TERM_KEY_RIGHT,
    TERM_KEY_INS,
    TERM_KEY_DEL,
    TERM_KEY_HOME,
    TERM_KEY_END,
    TERM_KEY_PAGEUP,
    TERM_KEY_PAGEDOWN,
    TERM_KEY_F1,
    TERM_KEY_F2,
    TERM_KEY_F3,
    TERM_KEY_F4,
    TERM_KEY_F5,
    TERM_KEY_F6,
    TERM_KEY_F7,
    TERM_KEY_F8,
    TERM_KEY_F9,
    TERM_KEY_F10,
    TERM_KEY_F11,
    TERM_KEY_F12,
    TERM_KEY_KP_0,
    TERM_KEY_KP_1,
    TERM_KEY_KP_2,
    TERM_KEY_KP_3,
    TERM_KEY_KP_4,
    TERM_KEY_KP_5,
    TERM_KEY_KP_6,
    TERM_KEY_KP_7,
    TERM_KEY_KP_8,
    TERM_KEY_KP_9,
    TERM_KEY_KP_MULTIPLY,
    TERM_KEY_KP_PLUS,
    TERM_KEY_KP_COMMA,
    TERM_KEY_KP_MINUS,
    TERM_KEY_KP_PERIOD,
    TERM_KEY_KP_DIVIDE,
    TERM_KEY_KP_ENTER,
    TERM_KEY_KP_EQUAL,
};

void terminal_send_key(TerminalBackend *term, int key, int mod);
void terminal_send_char(TerminalBackend *term, uint32_t codepoint, int mod);

// Bracketed paste support
void terminal_start_paste(TerminalBackend *term);
void terminal_end_paste(TerminalBackend *term);

// Reflow setting
void terminal_set_reflow(TerminalBackend *term, bool enabled);

// Line continuation (soft-wrap) query
bool terminal_get_line_continuation(TerminalBackend *term, int row);

// Selection API
void terminal_selection_start(TerminalBackend *term, int row, int col, TerminalSelectMode mode);
void terminal_selection_update(TerminalBackend *term, int row, int col);
void terminal_selection_clear(TerminalBackend *term);
bool terminal_selection_active(TerminalBackend *term);
bool terminal_cell_in_selection(TerminalBackend *term, int row, int col);
char *terminal_selection_get_text(TerminalBackend *term);
void terminal_selection_set_word_chars(TerminalBackend *term, const char *chars);
void terminal_set_selection_callback(TerminalBackend *term, TerminalSelectionChangeFn cb,
                                     void *user_data);

// Row iterator over a terminal row. bloom-vt stores UAX #11 + #29
// cluster widths on the cell, so the iterator is now a plain
// `vt_col += cell.width` walk. `vis_col` is kept identical to `vt_col`
// for source-compatibility with renderer code that still passes both.
typedef struct
{
    // Inputs
    TerminalBackend *term;
    int unified_row;
    int max_vt_cols;
    // Current step — valid after terminal_row_iter_next() returns true
    int vt_col;        // column of the current cell
    int vis_col;       // == vt_col
    int pres_w;        // == cell.width (1 normal, 2 wide, 0 continuation)
    TerminalCell cell; // fetched cell contents
    // Internal state
    int next_vt_col;
    int next_vis_col;
} TerminalRowIter;

// `unified_row` is in unified-row space (negative for scrollback,
// non-negative for visible area). See rend_sdl3.c "Coordinate Spaces".
void terminal_row_iter_init(TerminalRowIter *it, TerminalBackend *term,
                            int unified_row, int max_vt_cols);
bool terminal_row_iter_next(TerminalRowIter *it);

// Visual ↔ libvterm column translation for mouse, cursor, selection.
// Thin wrappers around TerminalRowIter.
int terminal_vt_col_to_vis_col(TerminalBackend *term, int unified_row, int vt_col);
int terminal_vis_col_to_vt_col(TerminalBackend *term, int unified_row, int vis_col);

// Sixel image API
void terminal_add_sixel_image(TerminalBackend *term, SixelImage *image);
void terminal_clear_sixel_images(TerminalBackend *term);
void terminal_scroll_sixel_images(TerminalBackend *term, int delta);

#endif /* TERM_H */
