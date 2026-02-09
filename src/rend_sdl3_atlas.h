#ifndef REND_SDL3_ATLAS_H
#define REND_SDL3_ATLAS_H

#include "font.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define REND_SDL3_ATLAS_HASH_SIZE       8192
#define REND_SDL3_ATLAS_TEXTURE_SIZE    2048
#define REND_SDL3_ATLAS_LARGE_THRESHOLD 48
#define REND_SDL3_ATLAS_MAX_SHELVES     128

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
    SDL_Texture *texture;
    uint8_t *staging;
    bool dirty;
    SDL_Rect dirty_rect;
    RendSdl3AtlasShelf shelves[REND_SDL3_ATLAS_MAX_SHELVES];
    int num_shelves;
    int next_shelf_y;
} RendSdl3AtlasPage;

typedef struct
{
    void *font_data;
    int glyph_id;
    uint32_t color;
    int page_index;
    RendSdl3AtlasRegion region;
    int x_offset, y_offset;
    uint64_t last_used_frame;
    bool occupied;
} RendSdl3AtlasEntry;

typedef struct
{
    RendSdl3AtlasPage pages[2];
    RendSdl3AtlasEntry entries[REND_SDL3_ATLAS_HASH_SIZE];
    RendSdl3AtlasEntry *evict_scratch;
    int entry_count;
    uint64_t current_frame;
    SDL_Renderer *renderer;
    bool eviction_occurred;
} RendSdl3Atlas;

bool rend_sdl3_atlas_init(RendSdl3Atlas *atlas, SDL_Renderer *renderer);
void rend_sdl3_atlas_destroy(RendSdl3Atlas *atlas);
void rend_sdl3_atlas_begin_frame(RendSdl3Atlas *atlas);
RendSdl3AtlasEntry *rend_sdl3_atlas_lookup(RendSdl3Atlas *atlas, void *font_data,
                                           int glyph_id, uint32_t color);
RendSdl3AtlasEntry *rend_sdl3_atlas_insert(RendSdl3Atlas *atlas, void *font_data,
                                           int glyph_id, uint32_t color,
                                           GlyphBitmap *bmp);
RendSdl3AtlasEntry *rend_sdl3_atlas_insert_empty(RendSdl3Atlas *atlas, void *font_data,
                                                 int glyph_id, uint32_t color);
void rend_sdl3_atlas_flush(RendSdl3Atlas *atlas);
void rend_sdl3_atlas_log_stats(RendSdl3Atlas *atlas);

#endif // REND_SDL3_ATLAS_H
