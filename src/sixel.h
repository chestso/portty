#ifndef SIXEL_H
#define SIXEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct SixelImage
{
    uint8_t *pixels; // RGBA pixel data (width * height * 4)
    int width, height;
    int cursor_row, cursor_col; // cell position where image was placed
    bool valid;
} SixelImage;

typedef struct SixelParser SixelParser;

SixelParser *sixel_parser_create(void);
void sixel_parser_destroy(SixelParser *parser);
void sixel_parser_begin(SixelParser *parser);
void sixel_parser_feed(SixelParser *parser, const char *data, size_t len);
SixelImage *sixel_parser_finish(SixelParser *parser); // caller owns result
void sixel_image_free(SixelImage *img);

#endif /* SIXEL_H */
