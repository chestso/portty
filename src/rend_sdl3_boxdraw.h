#ifndef REND_SDL3_BOXDRAW_H
#define REND_SDL3_BOXDRAW_H

#include <stdbool.h>
#include <stdint.h>

// Returns true if the codepoint is a box drawing (U+2500-U+257F)
// or block element (U+2580-U+259F) character we can draw procedurally.
bool rend_sdl3_boxdraw_is_supported(uint32_t cp);

// Render a box drawing or block element character into a CPU-side RGBA
// pixel buffer (GlyphBitmap).  The bitmap is cell-sized (w×h) with
// centered=true so the atlas pipeline places it at the cell centre.
// Caller must free the returned bitmap with free(pixels); free(bmp).
typedef struct GlyphBitmap GlyphBitmap;
GlyphBitmap *rend_sdl3_boxdraw_render(uint32_t cp, int cell_w, int cell_h,
                                      uint8_t r, uint8_t g, uint8_t b);

// Sentinel font_data pointer used as the atlas key for box-drawing
// glyphs, distinguishing them from real font glyphs.
#define BOXDRAW_FONT_DATA ((void *)(intptr_t)1)

#endif // REND_SDL3_BOXDRAW_H
