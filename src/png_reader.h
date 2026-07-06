#ifndef PNG_READER_H
#define PNG_READER_H

#include <stdint.h>
#include <stddef.h>

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

/*
 * Read a PNG from a memory buffer into an RGBA pixel buffer.
 *
 * data/len: pointer to PNG bytes and their length
 * pixels: heap-allocated row-major RGBA data (caller must free)
 * width/height: image dimensions in pixels (output)
 *
 * Returns 0 on success, -1 on failure.
 */
int png_read_rgba_mem(const uint8_t *data, size_t len, uint8_t **pixels,
                      int *width, int *height);

#endif /* PNG_READER_H */
