#ifndef RENDERER_H
#define RENDERER_H

#include "font.h"
#include "term.h"
#include <SDL3/SDL.h>
#include <fontconfig/fontconfig.h>

typedef struct
{
    SDL_Renderer *renderer;
    SDL_Window *window; // Window handle for DPI detection
    Font *font;         // Font backend directly
    int cell_width;
    int cell_height;
    int char_width;   // Actual character width from font
    int char_height;  // Actual character height from font
    int font_ascent;  // Distance from baseline to top
    int font_descent; // Distance from baseline to bottom
    int width;
    int height;
    int debug_grid; // Debug grid toggle flag

    // Glyph texture cache (simple LRU)
    struct GlyphCacheEntry *glyph_cache;
    int glyph_cache_size;
    uint64_t cache_tick;
} Renderer;

Renderer *renderer_init(SDL_Renderer *sdl_renderer, SDL_Window *window);
void renderer_destroy(Renderer *rend);
int renderer_load_fonts(Renderer *rend);
void renderer_draw_terminal(Renderer *rend, Terminal *term);
void renderer_present(Renderer *rend);
void renderer_resize(Renderer *rend, int width, int height);
void renderer_toggle_debug_grid(Renderer *rend); // Add function to toggle debug grid

#endif /* RENDERER_H */
