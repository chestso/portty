#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERM_MAX_CHARS_PER_CELL 6

// Forward declarations
struct TerminalBackend;
typedef struct TerminalBackend TerminalBackend;

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

typedef struct
{
    uint8_t r, g, b;
    bool is_default;
} TerminalColor;

typedef struct
{
    unsigned int bold : 1;
    unsigned int underline : 1;
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
    uint32_t chars[TERM_MAX_CHARS_PER_CELL];
    int width;
    TerminalCellAttr attrs;
    TerminalColor fg;
    TerminalColor bg;
} TerminalCell;

// Backend interface definition
struct TerminalBackend
{
    const char *name;

    // Backend-specific data
    void *backend_data;

    // Backend function pointers
    bool (*init)(TerminalBackend *term, int width, int height);
    void (*destroy)(TerminalBackend *term);
    void (*resize)(TerminalBackend *term, int width, int height);
    int (*process_input)(TerminalBackend *term, const char *input, size_t len);
    int (*get_cell)(TerminalBackend *term, int row, int col, TerminalCell *cell);
    int (*get_dimensions)(TerminalBackend *term, int *rows, int *cols);
    TerminalPos (*get_cursor_pos)(TerminalBackend *term);
    const char *(*get_title)(TerminalBackend *term);
    bool (*needs_redraw)(TerminalBackend *term);
    void (*clear_redraw)(TerminalBackend *term);
    bool (*get_damage_rect)(TerminalBackend *term, TerminalDamageRect *rect);
    int (*get_scrollback_lines)(TerminalBackend *term);
    int (*get_scrollback_cell)(TerminalBackend *term, int scrollback_row, int col,
                               TerminalCell *cell);
};

TerminalBackend *terminal_init(TerminalBackend *term, int width, int height);
void terminal_destroy(TerminalBackend *term);
void terminal_resize(TerminalBackend *term, int width, int height);
int terminal_process_input(TerminalBackend *term, const char *input, size_t len);
int terminal_get_cell(TerminalBackend *term, int row, int col, TerminalCell *cell);
int terminal_get_dimensions(TerminalBackend *term, int *rows, int *cols);
TerminalPos terminal_get_cursor_pos(TerminalBackend *term);
const char *terminal_get_title(TerminalBackend *term);
bool terminal_needs_redraw(TerminalBackend *term);
void terminal_clear_redraw(TerminalBackend *term);
bool terminal_get_damage_rect(TerminalBackend *term, TerminalDamageRect *rect);
int terminal_get_scrollback_lines(TerminalBackend *term);
int terminal_get_scrollback_cell(TerminalBackend *term, int scrollback_row, int col,
                                 TerminalCell *cell);

#endif /* TERM_H */
