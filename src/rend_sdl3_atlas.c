#include "rend_sdl3_atlas.h"
#include "common.h"
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
    void *pixels;
    int pitch;
    if (SDL_LockTexture(page->texture, NULL, &pixels, &pitch)) {
        for (int y = 0; y < REND_SDL3_ATLAS_TEXTURE_SIZE; y++) {
            memset((uint8_t *)pixels + y * pitch, 0, REND_SDL3_ATLAS_TEXTURE_SIZE * 4);
        }
        SDL_UnlockTexture(page->texture);
        vlog("Atlas[%d]: page cleared (had %d shelves)\n", page_index, page->num_shelves);
    }
}

static bool atlas_page_init(RendSdl3AtlasPage *page, SDL_Renderer *renderer, int page_index)
{
    page->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      REND_SDL3_ATLAS_TEXTURE_SIZE,
                                      REND_SDL3_ATLAS_TEXTURE_SIZE);
    if (!page->texture) {
        vlog("Atlas[%d]: failed to create atlas texture: %s\n", page_index, SDL_GetError());
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

    if (!atlas_page_init(&atlas->pages[0], renderer, 0)) {
        return false;
    }
    if (!atlas_page_init(&atlas->pages[1], renderer, 1)) {
        SDL_DestroyTexture(atlas->pages[0].texture);
        atlas->pages[0].texture = NULL;
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
    }
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

// Evict all entries belonging to a given page index
static void atlas_evict_page(RendSdl3Atlas *atlas, int page_index)
{
    int evicted = 0;
    for (int i = 0; i < REND_SDL3_ATLAS_HASH_SIZE; i++) {
        RendSdl3AtlasEntry *e = &atlas->entries[i];
        if (e->occupied && e->page_index == page_index) {
            e->occupied = false;
            atlas->entry_count--;
            evicted++;
        }
    }
    vlog("Atlas[%d]: evicted page (%d entries removed, %d total remaining)\n",
         page_index, evicted, atlas->entry_count);
    atlas_page_reset(&atlas->pages[page_index], page_index);
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

    // Upload pixels to atlas texture
    SDL_Rect dst_rect = { region.x, region.y, region.w, region.h };
    int pitch = bmp->width * 4;
    if (!SDL_UpdateTexture(page->texture, &dst_rect, bmp->pixels, pitch)) {
        vlog("Atlas[%d]: SDL_UpdateTexture failed: %s\n", page_index, SDL_GetError());
        return NULL;
    }
    vlog("Atlas[%d]: uploaded glyph %d to region (%d,%d %dx%d)\n",
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
