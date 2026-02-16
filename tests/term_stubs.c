#include "sixel.h"

// Stub for sixel_image_free — term.c calls this from
// terminal_add_sixel_image and terminal_clear_sixel_images
void sixel_image_free(SixelImage *img)
{
    (void)img;
}
