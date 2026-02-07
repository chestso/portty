#ifndef PNG_READER_H
#define PNG_READER_H

#include <stdint.h>

/*
 * Read a PNG file into an RGBA pixel buffer.
 *
 * pixels: heap-allocated row-major RGBA data (caller must free)
 * width/height: image dimensions in pixels (output)
 *
 * Returns 0 on success, -1 on failure.
 */
int png_read_rgba(const char *filename, uint8_t **pixels, int *width,
                  int *height);

#endif /* PNG_READER_H */
