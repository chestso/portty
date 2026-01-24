#ifndef REND_H
#define REND_H

#include "term.h"
#include <SDL3/SDL.h>

typedef struct Renderer Renderer;

Renderer *renderer_init(SDL_Renderer *sdl_renderer, SDL_Window *window);
void renderer_destroy(Renderer *rend);
int renderer_load_fonts(Renderer *rend);
void renderer_draw_terminal(Renderer *rend, Terminal *term);
void renderer_present(Renderer *rend);
void renderer_resize(Renderer *rend, int width, int height);
void renderer_toggle_debug_grid(Renderer *rend);

#endif /* REND_H */
