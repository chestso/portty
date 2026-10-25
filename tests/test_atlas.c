/*
 * portty — Texture atlas insertion, lookup, and eviction tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "rend_sdl3_atlas.h"
#include "test_helpers.h"
#include <stdlib.h>
#include <string.h>

// Helper: allocate a GlyphBitmap with uniform RGBA fill
static GlyphBitmap *make_bitmap(int w, int h, uint8_t fill)
{
    GlyphBitmap *bmp = calloc(1, sizeof(GlyphBitmap));
    bmp->width = w;
    bmp->height = h;
    bmp->pixels = malloc((size_t)w * h * 4);
    memset(bmp->pixels, fill, (size_t)w * h * 4);
    bmp->x_offset = 0;
    bmp->y_offset = 0;
    bmp->advance = w;
    bmp->glyph_id = 0;
    return bmp;
}

static void free_bitmap(GlyphBitmap *bmp)
{
    if (bmp) {
        free(bmp->pixels);
        free(bmp);
    }
}

// Fill a fresh atlas with `count` full-width glyphs, all in the current
// frame. Each one opens its own shelf (49px + 1px padding), so 40 of them end
// the shelf layout at y=2000 of the 2048px texture — the next one has no room
// unless an evicted glyph's region is recycled.
#define WIDE_GLYPH_COUNT 40
static void fill_with_wide_glyphs(RendSdl3Atlas *atlas, void *font, int gid_base)
{
    for (int i = 0; i < WIDE_GLYPH_COUNT; i++) {
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0x10 + (uint8_t)i);
        RendSdl3AtlasEntry *e =
            rend_sdl3_atlas_insert(atlas, font, gid_base + i, 0, bmp, false);
        ASSERT_TRUE(e != NULL);
        free_bitmap(bmp);
    }
}

// --- Test cases ---

// Test 1: Insert returns entry; lookup finds it; different key misses
static void test_insert_and_lookup(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    GlyphBitmap *bmp = make_bitmap(10, 10, 0xAA);
    void *font = (void *)0x1000;

    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_insert(&atlas, font, 42, 0xFF0000, bmp, false);
    ASSERT_TRUE(entry != NULL);
    ASSERT_EQ(entry->glyph_id, 42);
    ASSERT_EQ(entry->color, 0xFF0000u);

    // Lookup should find it
    RendSdl3AtlasEntry *found = rend_sdl3_atlas_lookup(&atlas, font, 42, 0xFF0000);
    ASSERT_TRUE(found != NULL);
    ASSERT_EQ(found->glyph_id, 42);

    // Different key should miss
    RendSdl3AtlasEntry *miss = rend_sdl3_atlas_lookup(&atlas, font, 99, 0xFF0000);
    ASSERT_TRUE(miss == NULL);

    free_bitmap(bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 2: Glyphs with same height share a shelf
static void test_shelf_reuse(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x2000;

    // Insert two narrow glyphs with the same height — should share a shelf
    GlyphBitmap *bmp1 = make_bitmap(20, 30, 0x11);
    RendSdl3AtlasEntry *e1 = rend_sdl3_atlas_insert(&atlas, font, 1, 0, bmp1, false);
    ASSERT_TRUE(e1 != NULL);

    GlyphBitmap *bmp2 = make_bitmap(25, 30, 0x22);
    RendSdl3AtlasEntry *e2 = rend_sdl3_atlas_insert(&atlas, font, 2, 0, bmp2, false);
    ASSERT_TRUE(e2 != NULL);

    // Same shelf means same y coordinate
    ASSERT_EQ(e1->region.y, e2->region.y);
    // Second glyph placed after the first (with 1px padding)
    ASSERT_EQ(e2->region.x, 20 + 1);

    // Different height gets a different shelf
    GlyphBitmap *bmp3 = make_bitmap(15, 40, 0x33);
    RendSdl3AtlasEntry *e3 = rend_sdl3_atlas_insert(&atlas, font, 3, 0, bmp3, false);
    ASSERT_TRUE(e3 != NULL);
    ASSERT_TRUE(e3->region.y != e1->region.y);

    free_bitmap(bmp1);
    free_bitmap(bmp2);
    free_bitmap(bmp3);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 3: Pixel data correctly memcpy'd to staging at entry's region
static void test_staging_buffer_contents(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x3000;
    GlyphBitmap *bmp = make_bitmap(4, 3, 0xBB);

    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_insert(&atlas, font, 10, 0, bmp, false);
    ASSERT_TRUE(entry != NULL);

    // Verify pixels in staging buffer
    int staging_pitch = REND_ATLAS_TEXTURE_SIZE * 4;

    for (int y = 0; y < 3; y++) {
        uint8_t *row = atlas.packing.staging + (entry->region.y + y) * staging_pitch +
                       entry->region.x * 4;
        for (int x = 0; x < 4 * 4; x++) {
            ASSERT_EQ(row[x], 0xBB);
        }
    }

    free_bitmap(bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 4: running out of room drops the glyphs the current frame did not use,
// and their pixels are recycled for the glyph that needs room.
static void test_eviction_drops_stale_entries(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x4000;
    ASSERT_TRUE(!atlas.packing.eviction_occurred);

    rend_sdl3_atlas_begin_frame(&atlas);
    fill_with_wide_glyphs(&atlas, font, 100);
    ASSERT_TRUE(!atlas.packing.eviction_occurred);
    ASSERT_EQ(atlas.packing.entry_count, WIDE_GLYPH_COUNT);

    // Next frame: the whole screen changed, so those 40 glyphs are stale and
    // the first of them can be handed to the new glyph.
    rend_sdl3_atlas_begin_frame(&atlas);

    GlyphBitmap *trigger_bmp = make_bitmap(2000, 49, 0xFF);
    RendSdl3AtlasEntry *trigger = rend_sdl3_atlas_insert(&atlas, font, 200, 0, trigger_bmp, false);
    ASSERT_TRUE(trigger != NULL);
    ASSERT_TRUE(atlas.packing.eviction_occurred);
    // Recycling the previous frame is not a reset: the pass that staged this
    // glyph keeps its work.
    ASSERT_TRUE(!atlas.packing.reset_occurred);

    // It landed on one of the recycled regions — the shelf layout did not
    // grow a new one past the end of the texture.
    ASSERT_EQ(atlas.packing.next_shelf_y, 2000);
    ASSERT_EQ(trigger->region.w, 2000);
    ASSERT_EQ(trigger->region.y % 50, 0);
    ASSERT_EQ(atlas.packing.entry_count, 1);

    free_bitmap(trigger_bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 5: only the stale entries go — a glyph the frame still uses keeps its
// entry and its pixels.
static void test_current_frame_entries_are_pinned(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x4800;

    rend_sdl3_atlas_begin_frame(&atlas);
    fill_with_wide_glyphs(&atlas, font, 100);

    rend_sdl3_atlas_begin_frame(&atlas);

    // The frame still shows the first of them.
    ASSERT_NOT_NULL(rend_sdl3_atlas_lookup(&atlas, font, 100, 0));

    GlyphBitmap *bmp = make_bitmap(2000, 49, 0xFF);
    ASSERT_NOT_NULL(rend_sdl3_atlas_insert(&atlas, font, 200, 0, bmp, false));
    free_bitmap(bmp);

    ASSERT_TRUE(atlas.packing.eviction_occurred);
    ASSERT_NOT_NULL(rend_sdl3_atlas_lookup(&atlas, font, 100, 0));
    ASSERT_NULL(rend_sdl3_atlas_lookup(&atlas, font, 101, 0));
    ASSERT_EQ(atlas.packing.entry_count, 2);

    rend_sdl3_atlas_destroy(&atlas);
}

// Test 6: when nothing is recyclable — every cached glyph belongs to the frame
// being built — the atlas starts over instead of leaving the glyph out.
static void test_full_reset_when_nothing_is_stale(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x5000;

    rend_sdl3_atlas_begin_frame(&atlas);
    fill_with_wide_glyphs(&atlas, font, 100);
    ASSERT_TRUE(!atlas.packing.eviction_occurred);

    // No room left and nothing stale to drop: the shelf layout is rebuilt.
    GlyphBitmap *trigger_bmp = make_bitmap(2000, 49, 0xFF);
    RendSdl3AtlasEntry *trigger = rend_sdl3_atlas_insert(&atlas, font, 200, 0, trigger_bmp, false);
    ASSERT_TRUE(trigger != NULL);
    ASSERT_TRUE(atlas.packing.eviction_occurred);
    // The glyphs staged during the pass went with it, so the pass has to
    // start over.
    ASSERT_TRUE(atlas.packing.reset_occurred);
    ASSERT_EQ(atlas.packing.entry_count, 1);

    for (int i = 0; i < WIDE_GLYPH_COUNT; i++)
        ASSERT_NULL(rend_sdl3_atlas_lookup(&atlas, font, 100 + i, 0));

    ASSERT_NOT_NULL(rend_sdl3_atlas_lookup(&atlas, font, 200, 0));

    free_bitmap(trigger_bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 7: the hash table load factor drops the stale entries rather than
// letting the table fill up.
static void test_load_factor_eviction(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x6000;
    ASSERT_TRUE(!atlas.packing.eviction_occurred);

    rend_sdl3_atlas_begin_frame(&atlas);

    int threshold = REND_ATLAS_HASH_SIZE * 3 / 4;
    for (int i = 0; i < threshold; i++) {
        GlyphBitmap *bmp = make_bitmap(1, 1, 0x11);
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, i, 0, bmp, false);
        ASSERT_TRUE(e != NULL);
        free_bitmap(bmp);
    }

    ASSERT_TRUE(!atlas.packing.eviction_occurred);
    ASSERT_EQ(atlas.packing.entry_count, threshold);

    // The next frame needs one glyph; the table is at its load limit, so the
    // frame before it is dropped.
    rend_sdl3_atlas_begin_frame(&atlas);
    GlyphBitmap *bmp = make_bitmap(1, 1, 0x22);
    RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, threshold, 0, bmp, false);
    ASSERT_TRUE(e != NULL);
    ASSERT_TRUE(atlas.packing.eviction_occurred);
    ASSERT_TRUE(!atlas.packing.reset_occurred);
    ASSERT_EQ(atlas.packing.entry_count, 1);

    free_bitmap(bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 8: a frame whose own glyphs fill the table gets no more entries — the
// table is kept below its load factor, where probing stays bounded.
static void test_load_factor_refuses_when_nothing_is_stale(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x6800;
    rend_sdl3_atlas_begin_frame(&atlas);

    int threshold = REND_ATLAS_HASH_SIZE * 3 / 4;
    for (int i = 0; i < threshold; i++) {
        GlyphBitmap *bmp = make_bitmap(1, 1, 0x11);
        ASSERT_NOT_NULL(rend_sdl3_atlas_insert(&atlas, font, i, 0, bmp, false));
        free_bitmap(bmp);
    }
    ASSERT_EQ(atlas.packing.entry_count, threshold);

    // Same frame: nothing is stale, so this glyph is left out.
    GlyphBitmap *bmp = make_bitmap(1, 1, 0x22);
    ASSERT_NULL(rend_sdl3_atlas_insert(&atlas, font, threshold, 0, bmp, false));
    free_bitmap(bmp);
    ASSERT_EQ(atlas.packing.entry_count, threshold);
    ASSERT_NULL(rend_sdl3_atlas_lookup(&atlas, font, threshold, 0));
    ASSERT_NOT_NULL(rend_sdl3_atlas_lookup(&atlas, font, 0, 0));

    rend_sdl3_atlas_destroy(&atlas);
}

// Test 9: after eviction, new glyph has correct pixels in staging
// Regression test: ensures post-eviction insert copies pixel data correctly
static void test_post_eviction_staging_correct(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x7000;

    rend_sdl3_atlas_begin_frame(&atlas);
    fill_with_wide_glyphs(&atlas, font, 100);

    // Trigger eviction with a glyph that has a distinctive fill pattern
    rend_sdl3_atlas_begin_frame(&atlas);
    uint8_t marker = 0xDE;
    GlyphBitmap *trigger_bmp = make_bitmap(2000, 49, marker);
    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_insert(&atlas, font, 200, 0, trigger_bmp, false);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(atlas.packing.eviction_occurred);

    // Verify that the staging buffer has the correct pixel data
    int staging_pitch = REND_ATLAS_TEXTURE_SIZE * 4;

    // Check first row of the glyph in staging
    uint8_t *row = atlas.packing.staging + entry->region.y * staging_pitch + entry->region.x * 4;
    // Check a few pixels (first 16 bytes = 4 RGBA pixels)
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(row[i], marker);
    }

    // Check last row too
    uint8_t *last_row = atlas.packing.staging + (entry->region.y + 48) * staging_pitch +
                        entry->region.x * 4;
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(last_row[i], marker);
    }

    free_bitmap(trigger_bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 9: fallback glyph keys are stable per path and independent of the
// address the face happens to be loaded at, so evicting and reloading a face
// keeps its cached glyphs usable.
static void test_fallback_key_is_stable_per_path(void)
{
    RendFallbackState st;
    rend_fallback_init(&st);

    void *a = rend_fallback_key(&st, "/fonts/a.ttf");
    void *b = rend_fallback_key(&st, "/fonts/b.ttf");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(rend_fallback_key(&st, "/fonts/a.ttf") == a);
    ASSERT_TRUE(rend_fallback_key(&st, "/fonts/b.ttf") == b);

    // Growing the registry must not move the records earlier keys point at.
    for (int i = 0; i < 100; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/fonts/grow-%d.ttf", i);
        ASSERT_NOT_NULL(rend_fallback_key(&st, path));
    }
    ASSERT_TRUE(rend_fallback_key(&st, "/fonts/a.ttf") == a);
    ASSERT_TRUE(rend_fallback_key(&st, "/fonts/b.ttf") == b);

    FontBackend font;
    memset(&font, 0, sizeof(font));
    rend_fallback_destroy(&st, &font);
}

// --- Regression tests for past bugs ---

// Regression: f0ee6ff — hash table overflow causing permanent blank glyphs.
// Downscaled color glyphs fill the table fast; every insert must still
// succeed because the glyphs of earlier frames are dropped to make room, and
// no insert may ever fail while the atlas is merely full.
static void test_regression_bulk_inserts_never_fail(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0xA000;

    int rounds = 3;
    int per_round = REND_ATLAS_HASH_SIZE * 3 / 4 * 2 / 3;
    for (int round = 0; round < rounds; round++) {
        rend_sdl3_atlas_begin_frame(&atlas);
        for (int i = 0; i < per_round; i++) {
            GlyphBitmap *bmp = make_bitmap(1, 1, 0x55);
            RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font,
                                                           round * per_round + i, 0,
                                                           bmp, false);
            ASSERT_TRUE(e != NULL);
            free_bitmap(bmp);
        }
    }

    ASSERT_TRUE(atlas.packing.eviction_occurred);
    ASSERT_TRUE(atlas.packing.entry_count <= REND_ATLAS_HASH_SIZE * 3 / 4);

    rend_sdl3_atlas_destroy(&atlas);
}

// Regression: 288da22 — eviction breaking hash table probe chains.
// Dropping an entry slides the entries behind it back over the hole; every
// survivor and every newcomer must stay reachable.
static void test_regression_post_eviction_all_entries_reachable(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0xB000;

    rend_sdl3_atlas_begin_frame(&atlas);
    fill_with_wide_glyphs(&atlas, font, 1000);

    rend_sdl3_atlas_begin_frame(&atlas);

    // This wide glyph triggers eviction of the previous frame
    GlyphBitmap *trigger = make_bitmap(2000, 49, 0xFF);
    rend_sdl3_atlas_insert(&atlas, font, 9999, 0, trigger, false);
    free_bitmap(trigger);
    ASSERT_TRUE(atlas.packing.eviction_occurred);

    // Now insert many new entries post-eviction
    int post_count = 200;
    for (int i = 0; i < post_count; i++) {
        GlyphBitmap *bmp = make_bitmap(8, 16, (uint8_t)(i & 0xFF));
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, 2000 + i, 0, bmp, false);
        ASSERT_TRUE(e != NULL);
        free_bitmap(bmp);
    }

    // Every post-eviction entry must be reachable via lookup
    for (int i = 0; i < post_count; i++) {
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_lookup(&atlas, font, 2000 + i, 0);
        ASSERT_TRUE(e != NULL);
        ASSERT_EQ(e->glyph_id, 2000 + i);
    }

    rend_sdl3_atlas_destroy(&atlas);
}

// Regression: 697e2c0 — transient blank glyphs after atlas eviction.
// Dropping entries must not clear the staging buffer: the glyphs of the frame
// being built are staged in it and are flushed right after the pass.
static void test_regression_multi_glyph_staging_after_eviction(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0xC000;

    rend_sdl3_atlas_begin_frame(&atlas);
    fill_with_wide_glyphs(&atlas, font, 1000);

    rend_sdl3_atlas_begin_frame(&atlas);

    // This wide glyph triggers eviction (2000 + 50 > 2048)
    GlyphBitmap *trigger = make_bitmap(2000, 49, 0x99);
    RendSdl3AtlasEntry *te = rend_sdl3_atlas_insert(&atlas, font, 9999, 0, trigger, false);
    ASSERT_TRUE(te != NULL);
    ASSERT_TRUE(atlas.packing.eviction_occurred);
    free_bitmap(trigger);

    // Now insert 3 more glyphs with distinct fill values post-eviction
    uint8_t fills[] = { 0xAA, 0xBB, 0xCC };
    RendSdl3AtlasEntry *entries[3];
    GlyphBitmap *bmps[3];

    for (int i = 0; i < 3; i++) {
        bmps[i] = make_bitmap(10, 10, fills[i]);
        entries[i] = rend_sdl3_atlas_insert(&atlas, font, 3000 + i, 0, bmps[i], false);
        ASSERT_TRUE(entries[i] != NULL);
    }

    // Verify staging data for all 3 glyphs — none should be zeroed out
    int staging_pitch = REND_ATLAS_TEXTURE_SIZE * 4;
    for (int g = 0; g < 3; g++) {
        uint8_t *row = atlas.packing.staging + entries[g]->region.y * staging_pitch +
                       entries[g]->region.x * 4;
        for (int x = 0; x < 10 * 4; x++) {
            ASSERT_EQ(row[x], fills[g]);
        }
    }

    for (int i = 0; i < 3; i++)
        free_bitmap(bmps[i]);
    rend_sdl3_atlas_destroy(&atlas);
}

// Regression: dropping an entry has to repair the probe chains of the entries
// behind it. Randomized insert/lookup/evict cycles: any entry the atlas still
// holds must stay reachable, and a lookup must never answer with another key.
static void test_regression_eviction_keeps_probe_chains_intact(void)
{
    enum
    {
        KEYS = 1200,
        STEPS = 4000
    };
    bool present[KEYS];
    memset(present, 0, sizeof(present));
    bool any_eviction = false;

    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0xE000;
    uint32_t rng = 0x12345678u;

    for (int step = 0; step < STEPS; step++) {
        // Advance the frame now and then so eviction has stale entries to
        // take; the entries a frame touched survive.
        if ((step % 40) == 0)
            rend_sdl3_atlas_begin_frame(&atlas);

        rng = rng * 1664525u + 1013904223u;
        int k = (int)((rng >> 12) % KEYS);

        atlas.packing.eviction_occurred = false;

        if (present[k]) {
            if (!rend_sdl3_atlas_lookup(&atlas, font, k, 0))
                present[k] = false; // dropped by an earlier eviction
        } else {
            GlyphBitmap *bmp = make_bitmap(80, 80, 0x40);
            if (rend_sdl3_atlas_insert(&atlas, font, k, 0, bmp, false))
                present[k] = true;
            free_bitmap(bmp);
        }

        if (!atlas.packing.eviction_occurred)
            continue;
        any_eviction = true;

        // Everything the atlas still holds for this frame has to answer its
        // own lookup: an eviction that broke a probe chain would strand an
        // entry that was never removed.
        for (int i = 0; i < REND_ATLAS_HASH_SIZE; i++) {
            RendAtlasEntry *e = &atlas.packing.entries[i];
            if (!e->occupied || e->last_used_frame != atlas.packing.current_frame)
                continue;
            RendSdl3AtlasEntry *found =
                rend_sdl3_atlas_lookup(&atlas, e->font_data, e->glyph_id, e->color);
            ASSERT_NOT_NULL(found);
            ASSERT_EQ(found->glyph_id, e->glyph_id);
            present[e->glyph_id % KEYS] = true;
        }
    }

    ASSERT_TRUE(any_eviction);
    rend_sdl3_atlas_destroy(&atlas);
}

// Copy of the atlas's hash, used to find two keys that share a home slot.
static uint32_t test_atlas_hash(void *font_data, int glyph_id, uint32_t color)
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

// Regression: removing an entry must not orphan the entries probed past it.
// Two glyphs that hash to the same slot share a probe chain: the second is
// reachable only through the first, so evicting the first has to pull the
// chain back over the hole.
static void test_regression_eviction_keeps_probe_chain_reachable(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0xF000;

    // Find two glyph ids whose home slot is the same.
    static int seen[REND_ATLAS_HASH_SIZE];
    memset(seen, 0, sizeof(seen));
    int first = 0, second = 0;
    for (int gid = 1; gid < 20000 && second == 0; gid++) {
        int home = (int)(test_atlas_hash(font, gid, 0) & (REND_ATLAS_HASH_SIZE - 1));
        if (seen[home] == 0) {
            seen[home] = gid;
        } else {
            first = seen[home];
            second = gid;
        }
    }
    ASSERT_TRUE(first > 0 && second > 0);

    rend_sdl3_atlas_begin_frame(&atlas);
    fill_with_wide_glyphs(&atlas, font, 1000);

    GlyphBitmap *bmp = make_bitmap(8, 8, 0x11);
    ASSERT_NOT_NULL(rend_sdl3_atlas_insert(&atlas, font, first, 0, bmp, false));
    free_bitmap(bmp);
    bmp = make_bitmap(8, 8, 0x22);
    ASSERT_NOT_NULL(rend_sdl3_atlas_insert(&atlas, font, second, 0, bmp, false));
    free_bitmap(bmp);

    // Next frame: keep the second glyph, let the first one go stale, then make
    // the atlas evict.
    rend_sdl3_atlas_begin_frame(&atlas);
    ASSERT_NOT_NULL(rend_sdl3_atlas_lookup(&atlas, font, second, 0));

    bmp = make_bitmap(2000, 49, 0xFF);
    ASSERT_NOT_NULL(rend_sdl3_atlas_insert(&atlas, font, 5000, 0, bmp, false));
    free_bitmap(bmp);
    ASSERT_TRUE(atlas.packing.eviction_occurred);

    ASSERT_NULL(rend_sdl3_atlas_lookup(&atlas, font, first, 0));
    ASSERT_NOT_NULL(rend_sdl3_atlas_lookup(&atlas, font, second, 0));

    rend_sdl3_atlas_destroy(&atlas);
}

// Regression: repeated eviction cycles must not corrupt state.
// Catches accumulated state bugs across multiple evict-repopulate cycles.
static void test_regression_repeated_eviction_cycles(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0xD000;

    for (int cycle = 0; cycle < 5; cycle++) {
        rend_sdl3_atlas_begin_frame(&atlas);
        fill_with_wide_glyphs(&atlas, font, cycle * 1000);

        // Nothing in this frame is stale, so this one rebuilds the atlas.
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0xEE);
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(
            &atlas, font, cycle * 1000 + 500, 0, bmp, false);
        ASSERT_TRUE(e != NULL);
        free_bitmap(bmp);
        ASSERT_TRUE(atlas.packing.eviction_occurred);

        // Verify the last entry survived
        RendSdl3AtlasEntry *found = rend_sdl3_atlas_lookup(
            &atlas, font, cycle * 1000 + 500, 0);
        ASSERT_TRUE(found != NULL);
    }

    ASSERT_TRUE(atlas.packing.entry_count <= 1);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test: the reset drops every cached glyph, the shelf layout and the recycled
// regions, and the atlas takes new glyphs again. A font reload relies on this:
// the atlas keys its glyphs on the font_data pointers, and the new face can be
// allocated at the address of the face it replaces.
static void test_reset_drops_entries(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x3000;

    GlyphBitmap *bmp = make_bitmap(10, 10, 0x33);
    ASSERT_TRUE(rend_sdl3_atlas_insert(&atlas, font, 7, 0xFF0000, bmp, false) != NULL);
    ASSERT_TRUE(rend_sdl3_atlas_lookup(&atlas, font, 7, 0xFF0000) != NULL);
    ASSERT_EQ(atlas.packing.entry_count, 1);
    ASSERT_EQ(atlas.packing.num_shelves, 1);

    rend_sdl3_atlas_reset(&atlas);

    // The cached glyph is gone.
    ASSERT_NULL(rend_sdl3_atlas_lookup(&atlas, font, 7, 0xFF0000));
    ASSERT_EQ(atlas.packing.entry_count, 0);
    ASSERT_EQ(atlas.packing.num_shelves, 0);
    ASSERT_EQ(atlas.packing.num_free_regions, 0);

    // A new face at the same address does not answer with the old
    // bitmap: the glyph is inserted again, at the shelf origin.
    GlyphBitmap *bmp2 = make_bitmap(10, 10, 0x44);
    RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, 7, 0xFF0000, bmp2, false);
    ASSERT_NOT_NULL(e);
    ASSERT_EQ(e->region.x, 0);
    ASSERT_EQ(e->region.y, 0);
    ASSERT_EQ(atlas.packing.entry_count, 1);

    free_bitmap(bmp);
    free_bitmap(bmp2);
    rend_sdl3_atlas_destroy(&atlas);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("Atlas unit tests\n");

    RUN_TEST(test_insert_and_lookup);
    RUN_TEST(test_shelf_reuse);
    RUN_TEST(test_staging_buffer_contents);
    RUN_TEST(test_eviction_drops_stale_entries);
    RUN_TEST(test_current_frame_entries_are_pinned);
    RUN_TEST(test_full_reset_when_nothing_is_stale);
    RUN_TEST(test_load_factor_eviction);
    RUN_TEST(test_load_factor_refuses_when_nothing_is_stale);
    RUN_TEST(test_post_eviction_staging_correct);
    RUN_TEST(test_fallback_key_is_stable_per_path);

    printf("\nRegression tests\n");

    RUN_TEST(test_regression_bulk_inserts_never_fail);
    RUN_TEST(test_regression_post_eviction_all_entries_reachable);
    RUN_TEST(test_regression_multi_glyph_staging_after_eviction);
    RUN_TEST(test_regression_repeated_eviction_cycles);
    RUN_TEST(test_regression_eviction_keeps_probe_chains_intact);
    RUN_TEST(test_regression_eviction_keeps_probe_chain_reachable);

    printf("\nFont reload tests\n");

    RUN_TEST(test_reset_drops_entries);

    TEST_SUMMARY();
}
