/*
 * portty — Texture atlas interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef REND_SDL3_ATLAS_H
#define REND_SDL3_ATLAS_H

#include "font.h"
#include "rend_common.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

// SDL3-specific glyph atlas. Embeds the shared RendAtlas packing engine
// (rend_common.c) and adds an SDL_Texture for GPU upload.

typedef struct
{
    RendAtlas packing;
    SDL_Texture *texture;
    SDL_Renderer *renderer;
    bool linearize_color;
} RendSdl3Atlas;

// RendSdl3AtlasEntry is an alias for RendAtlasEntry (back-compat with
// existing renderer code that uses the SDL3-prefixed name).
typedef RendAtlasEntry RendSdl3AtlasEntry;
typedef RendAtlasRegion RendSdl3AtlasRegion;

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
