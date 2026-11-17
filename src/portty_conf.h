/*
 * portty — INI-style configuration parsing interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef PORTTY_CONF_H
#define PORTTY_CONF_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
    PORTTY_HINT_UNSET = -1,
    PORTTY_HINT_NONE = 0,
    PORTTY_HINT_LIGHT = 1,
    PORTTY_HINT_NORMAL = 2,
    PORTTY_HINT_MONO = 3,
} PorttyHintMode;

/* Cursor shape (`cursor` key), in xterm's DECSCUSR vocabulary: `block`,
 * `underline` (a bar along the cell bottom), `bar` (a vertical bar on
 * the cell's left edge). */
typedef enum
{
    PORTTY_CURSOR_UNSET = -1,
    PORTTY_CURSOR_BLOCK = 0,
    PORTTY_CURSOR_UNDERLINE = 1,
    PORTTY_CURSOR_BAR = 2,
} PorttyCursorShape;

/* A configured color (0x00RRGGBB) with its explicit-set flag: `set`
 * false means the key was never given and the default applies. */
typedef struct
{
    bool set;
    uint32_t rgb;
} PorttyColor;

/* How BEL is presented. There is no audible bell: `always` shows the status
 * panel whenever one arrives, `unfocused` only when the window lacks keyboard
 * focus, `none` ignores BEL. */
typedef enum
{
    PORTTY_BELL_ALWAYS = 0,
    PORTTY_BELL_UNFOCUSED = 1,
    PORTTY_BELL_NONE = 2,
} PorttyBellMode;

typedef struct
{
    char *font;             /* NULL = not set */
    int cols;               /* 0 = not set */
    int rows;               /* 0 = not set */
    PorttyHintMode hinting; /* PORTTY_HINT_UNSET = not set */
    int verbose;            /* -1 = not set, 0 = false, 1 = true */
    char *word_chars;       /* NULL = not set */
    int scrollback;         /* -1 = not set; >= 0 = lines (0 disables) */
    int ambiguous_wide;     /* -1 = not set, 0 = false, 1 = true */
    int borderless;         /* -1 = not set, 0 = false, 1 = true */
    float text_gamma;       /* < 0 = unset (neutral); kitty text_composition_strategy gamma */
    float text_contrast;    /* < 0 = unset (neutral); kitty contrast, 0..100 */
    float dim_opacity;      /* < 0 = unset; SGR 2 dim foreground opacity, 0..1 */
    PorttyBellMode bell;    /* BEL presentation; PORTTY_BELL_ALWAYS unless set */
    char *shell;            /* NULL = not set; overrides $SHELL/COMSPEC when no -- args */
    char *dump_dir;         /* NULL = default; directory for Ctrl+Shift+F7 state dumps */
    char *source_path;      /* path the config was loaded from, or NULL (defaults) */
    /* Terminal colors (the xterm OSC 10/11/12 family): seeds for the
     * engine's dynamic colors. `cursor_text_color` is portty-only — the
     * color of the glyph under a block cursor. */
    PorttyColor fg_color;
    PorttyColor bg_color;
    PorttyColor cursor_color;
    PorttyColor cursor_text_color;
    PorttyCursorShape cursor_shape;
    int cursor_blink; /* -1 = not set, 0 = steady, 1 = blinking */
} PorttyConf;

void portty_conf_init(PorttyConf *conf);
const char *portty_bell_mode_name(PorttyBellMode mode);         /* "always", "unfocused", "none" */
const char *portty_cursor_shape_name(PorttyCursorShape shape);  /* "block", "underline", "bar" */
bool portty_conf_load(PorttyConf *conf);                        /* returns true if file found */
bool portty_conf_load_path(PorttyConf *conf, const char *path); /* load from explicit path */
void portty_conf_free(PorttyConf *conf);

#endif /* PORTTY_CONF_H */
