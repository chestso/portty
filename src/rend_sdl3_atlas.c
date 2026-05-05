#include "rend_sdl3_atlas.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>

// FNV-1a hash combining font_data pointer, glyph_id, and color
static uint32_t atlas_hash(void *font_data, int glyph_id, uint32_t color)
{
    uint32_t hash = 2166136261u;
    uintptr_t ptr = (uintptr_t)font_data;
    for (int i = 0; i < (int)sizeof(ptr); i++) {
        hash ^= (uint8_t)(ptr >> (i * 8));
        hash *= 16777619u;
    }
    uint32_t gid = (uint32_t)glyph_id;
    for (int i = 0; i < 4; i++) {
        hash ^= (uint8_t)(gid >> (i * 8));
        hash *= 16777619u;
    }
    for (int i = 0; i < 4; i++) {
        hash ^= (uint8_t)(color >> (i * 8));
        hash *= 16777619u;
    }
    return hash;
}

static void atlas_clear(RendSdl3Atlas *atlas)
{
    memset(atlas->staging, 0,
           (size_t)REND_SDL3_ATLAS_TEXTURE_SIZE * REND_SDL3_ATLAS_TEXTURE_SIZE * 4);
    atlas->dirty = true;
    atlas->dirty_rect = (SDL_Rect){ 0, 0, REND_SDL3_ATLAS_TEXTURE_SIZE,
                                    REND_SDL3_ATLAS_TEXTURE_SIZE };
    vlog("Atlas: cleared (had %d shelves)\n", atlas->num_shelves);
}

static void atlas_reset(RendSdl3Atlas *atlas)
{
    vlog("Atlas: resetting (had %d shelves)\n", atlas->num_shelves);
    atlas_clear(atlas);
    atlas->num_shelves = 0;
    atlas->next_shelf_y = 0;
}

// Try to allocate a region using shelf packing.
// Returns true if a region was allocated, false if the atlas is full.
static bool atlas_alloc(RendSdl3Atlas *atlas, int w, int h,
                        RendSdl3AtlasRegion *out)
{
    int padded_w = w + 1; // 1px padding to avoid bleed
    int padded_h = h + 1;

    // Try to fit on an existing shelf
    for (int i = 0; i < atlas->num_shelves; i++) {
        RendSdl3AtlasShelf *shelf = &atlas->shelves[i];
        if (shelf->height >= h && shelf->cursor_x + padded_w <= REND_SDL3_ATLAS_TEXTURE_SIZE) {
            out->x = shelf->cursor_x;
            out->y = shelf->y;
            out->w = w;
            out->h = h;
            shelf->cursor_x += padded_w;
            return true;
        }
    }

    // Need a new shelf
    if (atlas->num_shelves >= REND_SDL3_ATLAS_MAX_SHELVES) {
        vlog("Atlas: full - max shelves reached (%d)\n", REND_SDL3_ATLAS_MAX_SHELVES);
        return false;
    }
    if (atlas->next_shelf_y + padded_h > REND_SDL3_ATLAS_TEXTURE_SIZE) {
        vlog("Atlas: full - no vertical space (next_y=%d, needed=%d, max=%d)\n",
             atlas->next_shelf_y, padded_h, REND_SDL3_ATLAS_TEXTURE_SIZE);
        return false;
    }

    RendSdl3AtlasShelf *shelf = &atlas->shelves[atlas->num_shelves];
    shelf->y = atlas->next_shelf_y;
    shelf->height = h;
    shelf->cursor_x = padded_w;
    atlas->num_shelves++;
    atlas->next_shelf_y += padded_h;
    vlog("Atlas: allocated new shelf #%d (y=%d, height=%d)\n",
         atlas->num_shelves - 1, shelf->y, h);

    out->x = 0;
    out->y = shelf->y;
    out->w = w;
    out->h = h;
    return true;
}

bool rend_sdl3_atlas_init(RendSdl3Atlas *atlas, SDL_Renderer *renderer)
{
    memset(atlas, 0, sizeof(*atlas));
    atlas->renderer = renderer;
    atlas->current_frame = 0;
    atlas->entry_count = 0;

    atlas->staging = calloc((size_t)REND_SDL3_ATLAS_TEXTURE_SIZE * REND_SDL3_ATLAS_TEXTURE_SIZE, 4);
    if (!atlas->staging) {
        vlog("Atlas: failed to allocate staging buffer\n");
        return false;
    }
    atlas->dirty = false;
    atlas->dirty_rect = (SDL_Rect){ 0, 0, 0, 0 };

    atlas->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       REND_SDL3_ATLAS_TEXTURE_SIZE,
                                       REND_SDL3_ATLAS_TEXTURE_SIZE);
    if (!atlas->texture) {
        vlog("Atlas: failed to create texture: %s\n", SDL_GetError());
        free(atlas->staging);
        atlas->staging = NULL;
        return false;
    }
    SDL_SetTextureBlendMode(atlas->texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(atlas->texture, SDL_SCALEMODE_NEAREST);
    atlas_clear(atlas);
    atlas->num_shelves = 0;
    atlas->next_shelf_y = 0;

    vlog("Atlas initialized: %dx%d RGBA\n",
         REND_SDL3_ATLAS_TEXTURE_SIZE, REND_SDL3_ATLAS_TEXTURE_SIZE);
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
    free(atlas->staging);
    atlas->staging = NULL;
    memset(atlas->entries, 0, sizeof(atlas->entries));
    atlas->entry_count = 0;
}

void rend_sdl3_atlas_begin_frame(RendSdl3Atlas *atlas)
{
    atlas->current_frame++;
}

RendSdl3AtlasEntry *rend_sdl3_atlas_lookup(RendSdl3Atlas *atlas, void *font_data,
                                           int glyph_id, uint32_t color)
{
    uint32_t h = atlas_hash(font_data, glyph_id, color);
    uint32_t idx = h & (REND_SDL3_ATLAS_HASH_SIZE - 1);

    for (int probe = 0; probe < REND_SDL3_ATLAS_HASH_SIZE; probe++) {
        uint32_t i = (idx + probe) & (REND_SDL3_ATLAS_HASH_SIZE - 1);
        RendSdl3AtlasEntry *e = &atlas->entries[i];
        if (!e->occupied)
            return NULL;
        if (e->font_data == font_data && e->glyph_id == glyph_id && e->color == color) {
            e->last_used_frame = atlas->current_frame;
            return e;
        }
    }
    return NULL;
}

// Evict all entries and reset the atlas.
static void atlas_evict(RendSdl3Atlas *atlas)
{
    vlog("Atlas: evicting all entries (%d entries removed)\n", atlas->entry_count);
    memset(atlas->entries, 0, sizeof(atlas->entries));
    atlas->entry_count = 0;
    atlas_reset(atlas);
    atlas->eviction_occurred = true;
}

RendSdl3AtlasEntry *rend_sdl3_atlas_insert(RendSdl3Atlas *atlas, void *font_data,
                                           int glyph_id, uint32_t color,
                                           GlyphBitmap *bmp)
{
    if (!bmp || bmp->width <= 0 || bmp->height <= 0 || !bmp->pixels)
        return NULL;

    RendSdl3AtlasRegion region;

    // Evict if hash table is getting full (75% load factor)
    if (atlas->entry_count >= REND_SDL3_ATLAS_HASH_SIZE * 3 / 4) {
        vlog("Atlas: hash table load factor exceeded (%d/%d), evicting\n",
             atlas->entry_count, REND_SDL3_ATLAS_HASH_SIZE);
        atlas_evict(atlas);
    }

    // Try to allocate space
    if (!atlas_alloc(atlas, bmp->width, bmp->height, &region)) {
        // Atlas is full — evict and retry
        atlas_evict(atlas);
        if (!atlas_alloc(atlas, bmp->width, bmp->height, &region)) {
            // Glyph is too large even for an empty atlas
            vlog("Atlas: glyph %d too large (%dx%d)\n",
                 glyph_id, bmp->width, bmp->height);
            return NULL;
        }
    }

    // Copy pixels to staging buffer
    int staging_pitch = REND_SDL3_ATLAS_TEXTURE_SIZE * 4;
    int src_pitch = bmp->width * 4;
    for (int y = 0; y < bmp->height; y++) {
        uint8_t *dst = atlas->staging + (region.y + y) * staging_pitch + region.x * 4;
        uint8_t *src = bmp->pixels + y * src_pitch;
        memcpy(dst, src, src_pitch);
    }

    // Expand dirty rect to include this region
    SDL_Rect glyph_rect = { region.x, region.y, region.w, region.h };
    if (atlas->dirty) {
        SDL_GetRectUnion(&atlas->dirty_rect, &glyph_rect, &atlas->dirty_rect);
    } else {
        atlas->dirty_rect = glyph_rect;
        atlas->dirty = true;
    }
    vlog("Atlas: staged glyph %d to region (%d,%d %dx%d)\n",
         glyph_id, region.x, region.y, region.w, region.h);

    // Find a hash table slot (open addressing with linear probing)
    uint32_t h = atlas_hash(font_data, glyph_id, color);
    uint32_t idx = h & (REND_SDL3_ATLAS_HASH_SIZE - 1);
    RendSdl3AtlasEntry *slot = NULL;

    for (int probe = 0; probe < REND_SDL3_ATLAS_HASH_SIZE; probe++) {
        uint32_t i = (idx + probe) & (REND_SDL3_ATLAS_HASH_SIZE - 1);
        RendSdl3AtlasEntry *e = &atlas->entries[i];
        if (!e->occupied) {
            slot = e;
            break;
        }
    }

    if (!slot) {
        vlog("Atlas: hash table full\n");
        return NULL;
    }

    slot->font_data = font_data;
    slot->glyph_id = glyph_id;
    slot->color = color;
    slot->region = region;
    slot->x_offset = bmp->x_offset;
    slot->y_offset = bmp->y_offset;
    slot->centered = bmp->centered;
    slot->last_used_frame = atlas->current_frame;
    slot->occupied = true;
    atlas->entry_count++;

    vlog("Atlas: inserted glyph %d (total entries: %d)\n", glyph_id, atlas->entry_count);

    return slot;
}

RendSdl3AtlasEntry *rend_sdl3_atlas_insert_empty(RendSdl3Atlas *atlas, void *font_data,
                                                 int glyph_id, uint32_t color)
{
    // Evict if hash table is getting full (75% load factor)
    if (atlas->entry_count >= REND_SDL3_ATLAS_HASH_SIZE * 3 / 4) {
        vlog("Atlas: hash table load factor exceeded (%d/%d), evicting\n",
             atlas->entry_count, REND_SDL3_ATLAS_HASH_SIZE);
        atlas_evict(atlas);
    }

    uint32_t h = atlas_hash(font_data, glyph_id, color);
    uint32_t idx = h & (REND_SDL3_ATLAS_HASH_SIZE - 1);
    RendSdl3AtlasEntry *slot = NULL;

    for (int probe = 0; probe < REND_SDL3_ATLAS_HASH_SIZE; probe++) {
        uint32_t i = (idx + probe) & (REND_SDL3_ATLAS_HASH_SIZE - 1);
        RendSdl3AtlasEntry *e = &atlas->entries[i];
        if (!e->occupied) {
            slot = e;
            break;
        }
    }

    if (!slot)
        return NULL;

    slot->font_data = font_data;
    slot->glyph_id = glyph_id;
    slot->color = color;
    slot->region = (RendSdl3AtlasRegion){ 0, 0, 0, 0 };
    slot->x_offset = 0;
    slot->y_offset = 0;
    slot->last_used_frame = atlas->current_frame;
    slot->occupied = true;
    atlas->entry_count++;

    return slot;
}

void rend_sdl3_atlas_flush(RendSdl3Atlas *atlas)
{
    if (!atlas->dirty)
        return;
    int staging_pitch = REND_SDL3_ATLAS_TEXTURE_SIZE * 4;
    uint8_t *src = atlas->staging + atlas->dirty_rect.y * staging_pitch +
                   atlas->dirty_rect.x * 4;
    if (!SDL_UpdateTexture(atlas->texture, &atlas->dirty_rect, src, staging_pitch)) {
        vlog("Atlas: flush SDL_UpdateTexture failed: %s\n", SDL_GetError());
    } else {
        vlog("Atlas: flushed dirty rect (%d,%d %dx%d)\n",
             atlas->dirty_rect.x, atlas->dirty_rect.y,
             atlas->dirty_rect.w, atlas->dirty_rect.h);
    }
    atlas->dirty = false;
}

void rend_sdl3_atlas_log_stats(RendSdl3Atlas *atlas)
{
    if (!atlas)
        return;

    vlog("Atlas stats: frame=%llu entries=%d shelves=%d\n",
         (unsigned long long)atlas->current_frame,
         atlas->entry_count,
         atlas->num_shelves);
}
