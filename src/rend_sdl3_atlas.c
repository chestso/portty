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

static void atlas_page_clear(RendSdl3AtlasPage *page, int page_index)
{
    memset(page->staging, 0,
           (size_t)REND_SDL3_ATLAS_TEXTURE_SIZE * REND_SDL3_ATLAS_TEXTURE_SIZE * 4);
    page->dirty = true;
    page->dirty_rect = (SDL_Rect){ 0, 0, REND_SDL3_ATLAS_TEXTURE_SIZE,
                                   REND_SDL3_ATLAS_TEXTURE_SIZE };
    vlog("Atlas[%d]: page cleared (had %d shelves)\n", page_index, page->num_shelves);
}

static bool atlas_page_init(RendSdl3AtlasPage *page, SDL_Renderer *renderer, int page_index)
{
    page->staging = calloc((size_t)REND_SDL3_ATLAS_TEXTURE_SIZE * REND_SDL3_ATLAS_TEXTURE_SIZE, 4);
    if (!page->staging) {
        vlog("Atlas[%d]: failed to allocate staging buffer\n", page_index);
        return false;
    }
    page->dirty = false;
    page->dirty_rect = (SDL_Rect){ 0, 0, 0, 0 };

    page->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      REND_SDL3_ATLAS_TEXTURE_SIZE,
                                      REND_SDL3_ATLAS_TEXTURE_SIZE);
    if (!page->texture) {
        vlog("Atlas[%d]: failed to create atlas texture: %s\n", page_index, SDL_GetError());
        free(page->staging);
        page->staging = NULL;
        return false;
    }
    SDL_SetTextureBlendMode(page->texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(page->texture, SDL_SCALEMODE_NEAREST);
    atlas_page_clear(page, page_index);
    page->num_shelves = 0;
    page->next_shelf_y = 0;
    return true;
}

static void atlas_page_reset(RendSdl3AtlasPage *page, int page_index)
{
    vlog("Atlas[%d]: resetting page (had %d shelves)\n", page_index, page->num_shelves);
    atlas_page_clear(page, page_index);
    page->num_shelves = 0;
    page->next_shelf_y = 0;
}

// Try to allocate a region on the given page using shelf packing.
// Returns true if a region was allocated, false if the page is full.
static bool atlas_page_alloc(RendSdl3AtlasPage *page, int page_index, int w, int h,
                             RendSdl3AtlasRegion *out)
{
    int padded_w = w + 1; // 1px padding to avoid bleed
    int padded_h = h + 1;

    // Try to fit on an existing shelf
    for (int i = 0; i < page->num_shelves; i++) {
        RendSdl3AtlasShelf *shelf = &page->shelves[i];
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
    if (page->num_shelves >= REND_SDL3_ATLAS_MAX_SHELVES) {
        vlog("Atlas[%d]: page full - max shelves reached (%d)\n", page_index, REND_SDL3_ATLAS_MAX_SHELVES);
        return false;
    }
    if (page->next_shelf_y + padded_h > REND_SDL3_ATLAS_TEXTURE_SIZE) {
        vlog("Atlas[%d]: page full - no vertical space (next_y=%d, needed=%d, max=%d)\n",
             page_index, page->next_shelf_y, padded_h, REND_SDL3_ATLAS_TEXTURE_SIZE);
        return false;
    }

    RendSdl3AtlasShelf *shelf = &page->shelves[page->num_shelves];
    shelf->y = page->next_shelf_y;
    shelf->height = h;
    shelf->cursor_x = padded_w;
    page->num_shelves++;
    page->next_shelf_y += padded_h;
    vlog("Atlas[%d]: allocated new shelf #%d (y=%d, height=%d)\n",
         page_index, page->num_shelves - 1, shelf->y, h);

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

    atlas->evict_scratch =
        calloc(REND_SDL3_ATLAS_HASH_SIZE, sizeof(RendSdl3AtlasEntry));
    if (!atlas->evict_scratch) {
        return false;
    }

    if (!atlas_page_init(&atlas->pages[0], renderer, 0)) {
        free(atlas->evict_scratch);
        atlas->evict_scratch = NULL;
        return false;
    }
    if (!atlas_page_init(&atlas->pages[1], renderer, 1)) {
        SDL_DestroyTexture(atlas->pages[0].texture);
        atlas->pages[0].texture = NULL;
        free(atlas->evict_scratch);
        atlas->evict_scratch = NULL;
        return false;
    }

    vlog("Atlas initialized: 2 pages (0=small glyphs, 1=large glyphs), each %dx%d RGBA\n",
         REND_SDL3_ATLAS_TEXTURE_SIZE, REND_SDL3_ATLAS_TEXTURE_SIZE);
    return true;
}

void rend_sdl3_atlas_destroy(RendSdl3Atlas *atlas)
{
    if (!atlas)
        return;
    for (int i = 0; i < 2; i++) {
        if (atlas->pages[i].texture) {
            SDL_DestroyTexture(atlas->pages[i].texture);
            atlas->pages[i].texture = NULL;
        }
        free(atlas->pages[i].staging);
        atlas->pages[i].staging = NULL;
    }
    memset(atlas->entries, 0, sizeof(atlas->entries));
    atlas->entry_count = 0;
    free(atlas->evict_scratch);
    atlas->evict_scratch = NULL;
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

// Evict all entries belonging to a given page index.
// Rebuilds the hash table to preserve linear probe chains.
static void atlas_evict_page(RendSdl3Atlas *atlas, int page_index)
{
    // Collect surviving entries into scratch buffer
    int survivors = 0;
    int evicted = 0;
    for (int i = 0; i < REND_SDL3_ATLAS_HASH_SIZE; i++) {
        RendSdl3AtlasEntry *e = &atlas->entries[i];
        if (e->occupied) {
            if (e->page_index == page_index) {
                evicted++;
            } else {
                atlas->evict_scratch[survivors++] = *e;
            }
        }
    }

    // Clear the entire hash table
    memset(atlas->entries, 0, sizeof(atlas->entries));
    atlas->entry_count = 0;

    // Re-insert survivors with proper linear probing
    for (int i = 0; i < survivors; i++) {
        RendSdl3AtlasEntry *src = &atlas->evict_scratch[i];
        uint32_t h = atlas_hash(src->font_data, src->glyph_id, src->color);
        uint32_t idx = h & (REND_SDL3_ATLAS_HASH_SIZE - 1);

        for (int probe = 0; probe < REND_SDL3_ATLAS_HASH_SIZE; probe++) {
            uint32_t slot = (idx + probe) & (REND_SDL3_ATLAS_HASH_SIZE - 1);
            if (!atlas->entries[slot].occupied) {
                atlas->entries[slot] = *src;
                atlas->entry_count++;
                break;
            }
        }
    }

    vlog("Atlas[%d]: evicted page (%d entries removed, %d survivors rehashed)\n",
         page_index, evicted, survivors);
    atlas_page_reset(&atlas->pages[page_index], page_index);
    atlas->eviction_occurred = true;
}

RendSdl3AtlasEntry *rend_sdl3_atlas_insert(RendSdl3Atlas *atlas, void *font_data,
                                           int glyph_id, uint32_t color,
                                           GlyphBitmap *bmp)
{
    if (!bmp || bmp->width <= 0 || bmp->height <= 0 || !bmp->pixels)
        return NULL;

    // Select page based on glyph size
    int page_index = (bmp->width > REND_SDL3_ATLAS_LARGE_THRESHOLD ||
                      bmp->height > REND_SDL3_ATLAS_LARGE_THRESHOLD)
                         ? 1
                         : 0;

    RendSdl3AtlasPage *page = &atlas->pages[page_index];
    RendSdl3AtlasRegion region;

    // Try to allocate space
    if (!atlas_page_alloc(page, page_index, bmp->width, bmp->height, &region)) {
        // Page is full — evict and retry
        atlas_evict_page(atlas, page_index);
        if (!atlas_page_alloc(page, page_index, bmp->width, bmp->height, &region)) {
            // Glyph is too large even for an empty page
            vlog("Atlas[%d]: glyph %d too large (%dx%d) for page\n",
                 page_index, glyph_id, bmp->width, bmp->height);
            return NULL;
        }
    }

    // Copy pixels to staging buffer
    int staging_pitch = REND_SDL3_ATLAS_TEXTURE_SIZE * 4;
    int src_pitch = bmp->width * 4;
    for (int y = 0; y < bmp->height; y++) {
        uint8_t *dst = page->staging + (region.y + y) * staging_pitch + region.x * 4;
        uint8_t *src = bmp->pixels + y * src_pitch;
        memcpy(dst, src, src_pitch);
    }

    // Expand dirty rect to include this region
    SDL_Rect glyph_rect = { region.x, region.y, region.w, region.h };
    if (page->dirty) {
        SDL_GetRectUnion(&page->dirty_rect, &glyph_rect, &page->dirty_rect);
    } else {
        page->dirty_rect = glyph_rect;
        page->dirty = true;
    }
    vlog("Atlas[%d]: staged glyph %d to region (%d,%d %dx%d)\n",
         page_index, glyph_id, region.x, region.y, region.w, region.h);

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
        // Table is full — this shouldn't happen if hash size > max glyphs per page
        vlog("Atlas: hash table full\n");
        return NULL;
    }

    slot->font_data = font_data;
    slot->glyph_id = glyph_id;
    slot->color = color;
    slot->page_index = page_index;
    slot->region = region;
    slot->x_offset = bmp->x_offset;
    slot->y_offset = bmp->y_offset;
    slot->last_used_frame = atlas->current_frame;
    slot->occupied = true;
    atlas->entry_count++;

    vlog("Atlas[%d]: inserted glyph %d (total entries: %d)\n", page_index, glyph_id, atlas->entry_count);

    return slot;
}

RendSdl3AtlasEntry *rend_sdl3_atlas_insert_empty(RendSdl3Atlas *atlas, void *font_data,
                                                 int glyph_id, uint32_t color)
{
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
    slot->page_index = -1;
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
    int staging_pitch = REND_SDL3_ATLAS_TEXTURE_SIZE * 4;
    for (int i = 0; i < 2; i++) {
        RendSdl3AtlasPage *page = &atlas->pages[i];
        if (!page->dirty)
            continue;
        uint8_t *src = page->staging + page->dirty_rect.y * staging_pitch +
                       page->dirty_rect.x * 4;
        if (!SDL_UpdateTexture(page->texture, &page->dirty_rect, src, staging_pitch)) {
            vlog("Atlas[%d]: flush SDL_UpdateTexture failed: %s\n", i, SDL_GetError());
        } else {
            vlog("Atlas[%d]: flushed dirty rect (%d,%d %dx%d)\n", i,
                 page->dirty_rect.x, page->dirty_rect.y,
                 page->dirty_rect.w, page->dirty_rect.h);
        }
        page->dirty = false;
    }
}

void rend_sdl3_atlas_log_stats(RendSdl3Atlas *atlas)
{
    if (!atlas)
        return;

    int page0_entries = 0;
    int page1_entries = 0;
    for (int i = 0; i < REND_SDL3_ATLAS_HASH_SIZE; i++) {
        if (atlas->entries[i].occupied) {
            if (atlas->entries[i].page_index == 0)
                page0_entries++;
            else
                page1_entries++;
        }
    }

    vlog("Atlas stats: frame=%llu total_entries=%d (page0: %d entries/%d shelves, page1: %d entries/%d shelves)\n",
         (unsigned long long)atlas->current_frame,
         atlas->entry_count,
         page0_entries, atlas->pages[0].num_shelves,
         page1_entries, atlas->pages[1].num_shelves);
}
