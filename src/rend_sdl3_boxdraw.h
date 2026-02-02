#ifndef REND_SDL3_BOXDRAW_H
#define REND_SDL3_BOXDRAW_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

// Returns true if the codepoint is a box drawing (U+2500-U+257F)
// or block element (U+2580-U+259F) character we can draw procedurally.
bool rend_sdl3_boxdraw_is_supported(uint32_t cp);

// Draw a box drawing or block element character into the given cell rectangle.
void rend_sdl3_boxdraw_draw(SDL_Renderer *renderer, uint32_t cp,
                            int cell_x, int cell_y, int cell_w, int cell_h,
                            uint8_t r, uint8_t g, uint8_t b);

#endif // REND_SDL3_BOXDRAW_H
