#include "term.h"
#include <stdlib.h>

TerminalBackend *terminal_init(TerminalBackend *backend, int width, int height)
{
    if (!backend || !backend->init)
        return NULL;

    if (!backend->init(backend, width, height))
        return NULL;

    return backend;
}

void terminal_destroy(TerminalBackend *term)
{
    if (!term || !term->destroy)
        return;
    term->destroy(term);
}

void terminal_resize(TerminalBackend *term, int width, int height)
{
    if (!term || !term->resize)
        return;
    term->resize(term, width, height);
}

int terminal_process_input(TerminalBackend *term, const char *input, size_t len)
{
    if (!term || !term->process_input)
        return -1;
    return term->process_input(term, input, len);
}

int terminal_get_cell(TerminalBackend *term, int row, int col, TerminalCell *cell)
{
    if (!term || !term->get_cell)
        return -1;
    return term->get_cell(term, row, col, cell);
}

int terminal_get_dimensions(TerminalBackend *term, int *rows, int *cols)
{
    if (!term || !term->get_dimensions)
        return -1;
    return term->get_dimensions(term, rows, cols);
}

TerminalPos terminal_get_cursor_pos(TerminalBackend *term)
{
    if (!term || !term->get_cursor_pos)
        return (TerminalPos){ 0, 0 };
    return term->get_cursor_pos(term);
}

bool terminal_get_cursor_visible(TerminalBackend *term)
{
    if (!term || !term->get_cursor_visible)
        return true;
    return term->get_cursor_visible(term);
}

bool terminal_get_cursor_blink(TerminalBackend *term)
{
    if (!term || !term->get_cursor_blink)
        return true;
    return term->get_cursor_blink(term);
}

const char *terminal_get_title(TerminalBackend *term)
{
    if (!term || !term->get_title)
        return NULL;
    return term->get_title(term);
}

bool terminal_needs_redraw(TerminalBackend *term)
{
    if (!term || !term->needs_redraw)
        return false;
    return term->needs_redraw(term);
}

void terminal_clear_redraw(TerminalBackend *term)
{
    if (!term || !term->clear_redraw)
        return;
    term->clear_redraw(term);
}

bool terminal_get_damage_rect(TerminalBackend *term, TerminalDamageRect *rect)
{
    if (!term || !term->get_damage_rect || !rect)
        return false;
    return term->get_damage_rect(term, rect);
}

int terminal_get_scrollback_lines(TerminalBackend *term)
{
    if (!term || !term->get_scrollback_lines)
        return 0;
    return term->get_scrollback_lines(term);
}

int terminal_get_scrollback_cell(TerminalBackend *term, int scrollback_row, int col,
                                 TerminalCell *cell)
{
    if (!term || !term->get_scrollback_cell || !cell)
        return -1;
    return term->get_scrollback_cell(term, scrollback_row, col, cell);
}
