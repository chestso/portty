#ifndef QOI_WRITER_H
#define QOI_WRITER_H

#include <stdint.h>

/*
 * Write an RGBA pixel buffer to a QOI file.
 *
 * pixels: row-major RGBA data (4 bytes per pixel)
 * width/height: image dimensions in pixels
 *
 * Returns 0 on success, -1 on failure.
 */
int qoi_write_rgba(const char *filename, const uint8_t *pixels,
                   int width, int height);

#endif /* QOI_WRITER_H */
