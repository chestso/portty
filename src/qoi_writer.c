#define QOI_IMPLEMENTATION
#include "qoi.h"

#include "qoi_writer.h"

int qoi_write_rgba(const char *filename, const uint8_t *pixels,
                   int width, int height)
{
    if (!filename || !pixels || width <= 0 || height <= 0)
        return -1;

    qoi_desc desc;
    desc.width = (unsigned int)width;
    desc.height = (unsigned int)height;
    desc.channels = 4;
    desc.colorspace = QOI_SRGB;

    return qoi_write(filename, pixels, &desc) > 0 ? 0 : -1;
}
