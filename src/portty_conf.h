/*
 * portty — INI-style configuration parsing interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef PORTTY_CONF_H
#define PORTTY_CONF_H

#include <stdbool.h>

typedef enum
{
    PORTTY_HINT_UNSET = -1,
    PORTTY_HINT_NONE = 0,
    PORTTY_HINT_LIGHT = 1,
    PORTTY_HINT_NORMAL = 2,
    PORTTY_HINT_MONO = 3,
} PorttyHintMode;

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
} PorttyConf;

void portty_conf_init(PorttyConf *conf);
const char *portty_bell_mode_name(PorttyBellMode mode);         /* "always", "unfocused", "none" */
bool portty_conf_load(PorttyConf *conf);                        /* returns true if file found */
bool portty_conf_load_path(PorttyConf *conf, const char *path); /* load from explicit path */
void portty_conf_free(PorttyConf *conf);

#endif /* PORTTY_CONF_H */
