#ifndef PNG_WRITER_H
#define PNG_WRITER_H

#include <stdint.h>

/*
 * Write an RGBA pixel buffer to a PNG file.
 *
 * pixels: row-major RGBA data (4 bytes per pixel)
 * width/height: image dimensions in pixels
 *
 * Returns 0 on success, -1 on failure.
 */
int png_write_rgba(const char *filename, const uint8_t *pixels,
                   int width, int height);

#endif /* PNG_WRITER_H */
