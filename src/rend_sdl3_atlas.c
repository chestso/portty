/*
 * portty — Texture atlas: shelf packing, hashing, and LRU eviction
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "rend_sdl3_atlas.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>

bool rend_sdl3_atlas_init(RendSdl3Atlas *atlas, SDL_Renderer *renderer)
{
    memset(atlas, 0, sizeof(*atlas));
    atlas->renderer = renderer;

    uint8_t *staging = calloc(
        (size_t)REND_ATLAS_TEXTURE_SIZE * REND_ATLAS_TEXTURE_SIZE, 4);
    if (!staging) {
        vlog("Atlas: failed to allocate staging buffer\n");
        return false;
    }

    rend_atlas_init(&atlas->packing, staging);

    atlas->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       REND_ATLAS_TEXTURE_SIZE,
                                       REND_ATLAS_TEXTURE_SIZE);
    if (!atlas->texture) {
        vlog("Atlas: failed to create texture: %s\n", SDL_GetError());
        free(staging);
        return false;
    }
    SDL_SetTextureBlendMode(atlas->texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas->texture, SDL_SCALEMODE_NEAREST);

    vlog("Atlas initialized: %dx%d RGBA\n",
         REND_ATLAS_TEXTURE_SIZE, REND_ATLAS_TEXTURE_SIZE);
    return true;
}

void rend_sdl3_atlas_destroy(RendSdl3Atlas *atlas)
{
    if (!atlas)
        return;
    if (atlas->texture) {
        SDL_DestroyTexture(atlas->texture);
        atlas->texture = NULL;
    }
    free(atlas->packing.staging);
    atlas->packing.staging = NULL;
    memset(atlas->packing.entries, 0, sizeof(atlas->packing.entries));
    atlas->packing.entry_count = 0;
}

void rend_sdl3_atlas_begin_frame(RendSdl3Atlas *atlas)
{
    rend_atlas_begin_frame(&atlas->packing);
}

RendSdl3AtlasEntry *rend_sdl3_atlas_lookup(RendSdl3Atlas *atlas, void *font_data,
                                           int glyph_id, uint32_t color)
{
    return rend_atlas_lookup(&atlas->packing, font_data, glyph_id, color);
}

RendSdl3AtlasEntry *rend_sdl3_atlas_insert(RendSdl3Atlas *atlas, void *font_data,
                                           int glyph_id, uint32_t color,
                                           GlyphBitmap *bmp, bool is_color)
{
    return rend_atlas_insert(&atlas->packing, font_data, glyph_id, color,
                             bmp, is_color, atlas->linearize_color);
}

RendSdl3AtlasEntry *rend_sdl3_atlas_insert_empty(RendSdl3Atlas *atlas, void *font_data,
                                                 int glyph_id, uint32_t color)
{
    return rend_atlas_insert_empty(&atlas->packing, font_data, glyph_id, color);
}

void rend_sdl3_atlas_flush(RendSdl3Atlas *atlas)
{
    if (!atlas->packing.dirty)
        return;
    RendAtlasRegion *dr = &atlas->packing.dirty_rect;
    int staging_pitch = REND_ATLAS_TEXTURE_SIZE * 4;
    uint8_t *src = atlas->packing.staging + dr->y * staging_pitch + dr->x * 4;
    SDL_Rect sdl_rect = { dr->x, dr->y, dr->w, dr->h };
    if (!SDL_UpdateTexture(atlas->texture, &sdl_rect, src, staging_pitch)) {
        vlog("Atlas: flush SDL_UpdateTexture failed: %s\n", SDL_GetError());
    } else {
        vlog("Atlas: flushed dirty rect (%d,%d %dx%d)\n",
             dr->x, dr->y, dr->w, dr->h);
    }
    atlas->packing.dirty = false;
}

void rend_sdl3_atlas_log_stats(RendSdl3Atlas *atlas)
{
    rend_atlas_log_stats(&atlas->packing);
}
