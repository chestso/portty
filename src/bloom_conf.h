#ifndef BLOOM_CONF_H
#define BLOOM_CONF_H

#include <stdbool.h>

typedef enum
{
    BLOOM_HINT_UNSET = -1,
    BLOOM_HINT_NONE = 0,
    BLOOM_HINT_LIGHT = 1,
    BLOOM_HINT_NORMAL = 2,
    BLOOM_HINT_MONO = 3,
} BloomHintMode;

typedef struct
{
    char *font;            /* NULL = not set */
    int cols;              /* 0 = not set */
    int rows;              /* 0 = not set */
    BloomHintMode hinting; /* BLOOM_HINT_UNSET = not set */
    int reflow;            /* -1 = not set, 0 = false, 1 = true */
    int padding;           /* -1 = not set, 0 = false, 1 = true */
    int verbose;           /* -1 = not set, 0 = false, 1 = true */
    char *word_chars;      /* NULL = not set */
    char *platform;        /* NULL = not set; "sdl3" or "gtk4" */
} BloomConf;

void bloom_conf_init(BloomConf *conf);
bool bloom_conf_load(BloomConf *conf);                        /* returns true if file found */
bool bloom_conf_load_path(BloomConf *conf, const char *path); /* load from explicit path */
void bloom_conf_free(BloomConf *conf);

#endif /* BLOOM_CONF_H */
