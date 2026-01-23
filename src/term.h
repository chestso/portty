#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TERM_MAX_CHARS_PER_CELL 6

typedef struct Terminal Terminal;

typedef struct
{
    int row;
    int col;
} TerminalPos;

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

Terminal *terminal_init(int width, int height);
void terminal_destroy(Terminal *term);
void terminal_resize(Terminal *term, int width, int height);
int terminal_process_input(Terminal *term, const char *input, size_t len);
int terminal_get_cell(Terminal *term, int row, int col, TerminalCell *cell);
int terminal_get_dimensions(Terminal *term, int *rows, int *cols);
TerminalPos terminal_get_cursor_pos(Terminal *term);
const char *terminal_get_title(Terminal *term);
bool terminal_needs_redraw(Terminal *term);
void terminal_clear_redraw(Terminal *term);

#endif /* TERM_H */
