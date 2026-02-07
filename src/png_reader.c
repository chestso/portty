#include "png_reader.h"
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int png_read_rgba(const char *filename, uint8_t **pixels, int *width,
                  int *height)
{
    if (!filename || !pixels || !width || !height)
        return -1;

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "ERROR: Cannot open %s for reading\n", filename);
        return -1;
    }

    /* Verify PNG signature */
    uint8_t sig[8];
    if (fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8)) {
        fprintf(stderr, "ERROR: %s is not a valid PNG file\n", filename);
        fclose(fp);
        return -1;
    }

    png_structp png =
        png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        return -1;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        fclose(fp);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    png_init_io(png, fp);
    png_set_sig_bytes(png, 8);
    png_read_info(png, info);

    int w = png_get_image_width(png, info);
    int h = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    /* Normalize to 8-bit RGBA regardless of source format */
    if (bit_depth == 16)
        png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    /* Allocate pixel buffer */
    size_t row_bytes = (size_t)w * 4;
    uint8_t *buf = malloc(row_bytes * h);
    if (!buf) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -1;
    }

    /* Read row by row */
    for (int y = 0; y < h; y++) {
        png_read_row(png, buf + y * row_bytes, NULL);
    }

    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    *pixels = buf;
    *width = w;
    *height = h;
    return 0;
}
