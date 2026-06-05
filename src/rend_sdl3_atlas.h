#ifndef REND_SDL3_ATLAS_H
#define REND_SDL3_ATLAS_H

#include "font.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define REND_SDL3_ATLAS_HASH_SIZE    8192
#define REND_SDL3_ATLAS_TEXTURE_SIZE 2048
#define REND_SDL3_ATLAS_MAX_SHELVES  128

typedef struct
{
    int x, y, w, h;
} RendSdl3AtlasRegion;

typedef struct
{
    int y;
    int height;
    int cursor_x;
} RendSdl3AtlasShelf;

typedef struct
{
    void *font_data;
    int glyph_id;
    uint32_t color;
    RendSdl3AtlasRegion region;
    int x_offset, y_offset;
    uint64_t last_used_frame;
    bool occupied;
    bool centered; // Forwarded from GlyphBitmap.centered.
} RendSdl3AtlasEntry;

typedef struct
{
    SDL_Texture *texture;
    uint8_t *staging;
    bool dirty;
    SDL_Rect dirty_rect;
    RendSdl3AtlasShelf shelves[REND_SDL3_ATLAS_MAX_SHELVES];
    int num_shelves;
    int next_shelf_y;
    RendSdl3AtlasEntry entries[REND_SDL3_ATLAS_HASH_SIZE];
    int entry_count;
    uint64_t current_frame;
    SDL_Renderer *renderer;
    bool eviction_occurred;
    // When the renderer composites in linear light (SDL gpu/vulkan into an
    // SRGB_LINEAR target), color-glyph texels must be sRGB->linear decoded on
    // insert: SDL re-encodes the float target on blit-out but never decodes
    // sampled texels, so without this color emoji come back double-encoded
    // (washed out). White text-coverage texels are gamma-invariant. Set from
    // the renderer's linear_ok in sdl3_init.
    bool linearize_color;
} RendSdl3Atlas;

bool rend_sdl3_atlas_init(RendSdl3Atlas *atlas, SDL_Renderer *renderer);
void rend_sdl3_atlas_destroy(RendSdl3Atlas *atlas);
void rend_sdl3_atlas_begin_frame(RendSdl3Atlas *atlas);
RendSdl3AtlasEntry *rend_sdl3_atlas_lookup(RendSdl3Atlas *atlas, void *font_data,
                                           int glyph_id, uint32_t color);
RendSdl3AtlasEntry *rend_sdl3_atlas_insert(RendSdl3Atlas *atlas, void *font_data,
                                           int glyph_id, uint32_t color,
                                           GlyphBitmap *bmp, bool is_color);
RendSdl3AtlasEntry *rend_sdl3_atlas_insert_empty(RendSdl3Atlas *atlas, void *font_data,
                                                 int glyph_id, uint32_t color);
void rend_sdl3_atlas_flush(RendSdl3Atlas *atlas);
void rend_sdl3_atlas_log_stats(RendSdl3Atlas *atlas);

#endif // REND_SDL3_ATLAS_H
