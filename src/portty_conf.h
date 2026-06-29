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

typedef struct
{
    char *font;                    /* NULL = not set */
    int cols;                      /* 0 = not set */
    int rows;                      /* 0 = not set */
    PorttyHintMode hinting;        /* PORTTY_HINT_UNSET = not set */
    int verbose;                   /* -1 = not set, 0 = false, 1 = true */
    char *word_chars;              /* NULL = not set */
    char *platform;                /* NULL = not set; "sdl3" or "gtk4" */
    int scrollback;                /* -1 = not set; >= 0 = lines (0 disables) */
    float text_gamma;              /* < 0 = unset (neutral); kitty text_composition_strategy gamma */
    float text_contrast;           /* < 0 = unset (neutral); kitty contrast, 0..100 */
    int notification_transparency; /* -1 = unset; 0 = opaque (default); 1 = translucent */
    char *shell;                   /* NULL = not set; overrides $SHELL/COMSPEC when no -- args */
    char *source_path;             /* path the config was loaded from, or NULL (defaults) */
} PorttyConf;

void portty_conf_init(PorttyConf *conf);
bool portty_conf_load(PorttyConf *conf);                        /* returns true if file found */
bool portty_conf_load_path(PorttyConf *conf, const char *path); /* load from explicit path */
void portty_conf_free(PorttyConf *conf);

#endif /* PORTTY_CONF_H */
