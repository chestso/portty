#include "png_writer.h"
#include <png.h>
#include <stdio.h>
#include <stdlib.h>

int png_write_rgba(const char *filename, const uint8_t *pixels,
                   int width, int height)
{
    if (!filename || !pixels || width <= 0 || height <= 0)
        return -1;

    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "ERROR: Cannot open %s for writing\n", filename);
        return -1;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);

    png_set_IHDR(png, info, width, height, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    png_write_info(png, info);

    /* Write row by row */
    for (int y = 0; y < height; y++) {
        png_write_row(png, (png_const_bytep)(pixels + y * width * 4));
    }

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);

    return 0;
}
