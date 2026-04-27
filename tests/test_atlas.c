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

// --- Test cases ---

// Test 1: Insert returns entry; lookup finds it; different key misses
static void test_insert_and_lookup(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    GlyphBitmap *bmp = make_bitmap(10, 10, 0xAA);
    void *font = (void *)0x1000;

    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_insert(&atlas, font, 42, 0xFF0000, bmp);
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
    RendSdl3AtlasEntry *e1 = rend_sdl3_atlas_insert(&atlas, font, 1, 0, bmp1);
    ASSERT_TRUE(e1 != NULL);

    GlyphBitmap *bmp2 = make_bitmap(25, 30, 0x22);
    RendSdl3AtlasEntry *e2 = rend_sdl3_atlas_insert(&atlas, font, 2, 0, bmp2);
    ASSERT_TRUE(e2 != NULL);

    // Same shelf means same y coordinate
    ASSERT_EQ(e1->region.y, e2->region.y);
    // Second glyph placed after the first (with 1px padding)
    ASSERT_EQ(e2->region.x, 20 + 1);

    // Different height gets a different shelf
    GlyphBitmap *bmp3 = make_bitmap(15, 40, 0x33);
    RendSdl3AtlasEntry *e3 = rend_sdl3_atlas_insert(&atlas, font, 3, 0, bmp3);
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

    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_insert(&atlas, font, 10, 0, bmp);
    ASSERT_TRUE(entry != NULL);

    // Verify pixels in staging buffer
    int staging_pitch = REND_SDL3_ATLAS_TEXTURE_SIZE * 4;

    for (int y = 0; y < 3; y++) {
        uint8_t *row = atlas.staging + (entry->region.y + y) * staging_pitch +
                       entry->region.x * 4;
        for (int x = 0; x < 4 * 4; x++) {
            ASSERT_EQ(row[x], 0xBB);
        }
    }

    free_bitmap(bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 4: Filling the atlas then inserting one more sets eviction_occurred = true
static void test_eviction_sets_flag(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x4000;
    ASSERT_TRUE(!atlas.eviction_occurred);

    // Insert 40 wide glyphs (2000x49) — each gets its own shelf (50px with padding)
    // After 40: next_shelf_y = 2000
    for (int i = 0; i < 40; i++) {
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0x10 + (uint8_t)i);
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, 100 + i, 0, bmp);
        ASSERT_TRUE(e != NULL);
        free_bitmap(bmp);
    }

    ASSERT_TRUE(!atlas.eviction_occurred);

    // 41st insert: needs next_shelf_y(2000) + 50 = 2050 > 2048 → eviction
    GlyphBitmap *trigger_bmp = make_bitmap(2000, 49, 0xFF);
    RendSdl3AtlasEntry *trigger = rend_sdl3_atlas_insert(&atlas, font, 200, 0, trigger_bmp);
    ASSERT_TRUE(trigger != NULL);
    ASSERT_TRUE(atlas.eviction_occurred);

    free_bitmap(trigger_bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 5: After eviction, old entries are gone from hash table
static void test_eviction_clears_old_entries(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x5000;

    // Fill atlas with 40 wide glyphs
    for (int i = 0; i < 40; i++) {
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0x10);
        rend_sdl3_atlas_insert(&atlas, font, 100 + i, 0, bmp);
        free_bitmap(bmp);
    }

    // Trigger eviction
    GlyphBitmap *trigger_bmp = make_bitmap(2000, 49, 0xFF);
    rend_sdl3_atlas_insert(&atlas, font, 200, 0, trigger_bmp);
    free_bitmap(trigger_bmp);

    // Old entries should be gone
    for (int i = 0; i < 40; i++) {
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_lookup(&atlas, font, 100 + i, 0);
        ASSERT_TRUE(e == NULL);
    }

    // The trigger glyph should be present (re-inserted after eviction)
    RendSdl3AtlasEntry *survivor = rend_sdl3_atlas_lookup(&atlas, font, 200, 0);
    ASSERT_TRUE(survivor != NULL);

    rend_sdl3_atlas_destroy(&atlas);
}

// Test 6: Hash table load factor triggers eviction before spatial exhaustion
static void test_load_factor_eviction(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x6000;
    ASSERT_TRUE(!atlas.eviction_occurred);

    // Insert enough small glyphs to hit 75% hash table load
    // REND_SDL3_ATLAS_HASH_SIZE = 8192, threshold = 6144
    int threshold = REND_SDL3_ATLAS_HASH_SIZE * 3 / 4;
    for (int i = 0; i < threshold; i++) {
        GlyphBitmap *bmp = make_bitmap(1, 1, 0x11);
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, i, 0, bmp);
        ASSERT_TRUE(e != NULL);
        free_bitmap(bmp);
    }

    ASSERT_TRUE(!atlas.eviction_occurred);

    // Next insert should trigger load factor eviction
    GlyphBitmap *bmp = make_bitmap(1, 1, 0x22);
    RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, threshold, 0, bmp);
    ASSERT_TRUE(e != NULL);
    ASSERT_TRUE(atlas.eviction_occurred);

    // Only the new entry should exist
    ASSERT_EQ(atlas.entry_count, 1);

    free_bitmap(bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 7: After eviction, new glyph has correct pixels in staging
// Regression test: ensures post-eviction insert copies pixel data correctly
static void test_post_eviction_staging_correct(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x7000;

    // Fill atlas to trigger eviction
    for (int i = 0; i < 40; i++) {
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0x10);
        rend_sdl3_atlas_insert(&atlas, font, 100 + i, 0, bmp);
        free_bitmap(bmp);
    }

    // Trigger eviction with a glyph that has a distinctive fill pattern
    uint8_t marker = 0xDE;
    GlyphBitmap *trigger_bmp = make_bitmap(2000, 49, marker);
    RendSdl3AtlasEntry *entry = rend_sdl3_atlas_insert(&atlas, font, 200, 0, trigger_bmp);
    ASSERT_TRUE(entry != NULL);
    ASSERT_TRUE(atlas.eviction_occurred);

    // Verify that the staging buffer has the correct pixel data
    int staging_pitch = REND_SDL3_ATLAS_TEXTURE_SIZE * 4;

    // Check first row of the glyph in staging
    uint8_t *row = atlas.staging + entry->region.y * staging_pitch + entry->region.x * 4;
    // Check a few pixels (first 16 bytes = 4 RGBA pixels)
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(row[i], marker);
    }

    // Check last row too
    uint8_t *last_row = atlas.staging + (entry->region.y + 48) * staging_pitch +
                        entry->region.x * 4;
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(last_row[i], marker);
    }

    free_bitmap(trigger_bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// --- Regression tests for past bugs ---

// Regression: f0ee6ff — hash table overflow causing permanent blank glyphs.
// Without load factor eviction, inserting many unique glyph+color keys would
// silently fill the hash table. Once full, insert returned NULL and glyphs
// went permanently blank. Verify that bulk inserts always succeed (load factor
// eviction keeps the table usable).
static void test_regression_bulk_inserts_never_fail(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0xA000;

    // Insert 10000 unique keys — more than the hash table can hold at once.
    // Load factor eviction should fire multiple times, but every insert
    // should still succeed.
    for (int i = 0; i < 10000; i++) {
        GlyphBitmap *bmp = make_bitmap(1, 1, 0x55);
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, i, 0, bmp);
        ASSERT_TRUE(e != NULL);
        free_bitmap(bmp);
    }

    // Atlas must have evicted at least once (10000 > 8192*0.75 = 6144)
    ASSERT_TRUE(atlas.eviction_occurred);

    rend_sdl3_atlas_destroy(&atlas);
}

// Regression: 288da22 — eviction breaking hash table probe chains.
// After partial eviction (old design), leftover entries could become
// unreachable orphans because deleted slots broke linear probing chains.
// Verify that all entries inserted after eviction are findable via lookup.
static void test_regression_post_eviction_all_entries_reachable(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0xB000;

    // Fill atlas to trigger spatial eviction
    for (int i = 0; i < 40; i++) {
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0x10);
        rend_sdl3_atlas_insert(&atlas, font, 1000 + i, 0, bmp);
        free_bitmap(bmp);
    }

    // Trigger eviction
    GlyphBitmap *trigger = make_bitmap(2000, 49, 0xFF);
    rend_sdl3_atlas_insert(&atlas, font, 9999, 0, trigger);
    free_bitmap(trigger);
    ASSERT_TRUE(atlas.eviction_occurred);

    // Now insert many new entries post-eviction
    int post_count = 200;
    for (int i = 0; i < post_count; i++) {
        GlyphBitmap *bmp = make_bitmap(8, 16, (uint8_t)(i & 0xFF));
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, 2000 + i, 0, bmp);
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
// When eviction occurred mid-populate, staging was cleared, destroying pixel
// data for glyphs staged earlier in the same pass. Verify that multiple
// glyphs inserted after eviction all have correct staging data.
static void test_regression_multi_glyph_staging_after_eviction(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0xC000;

    // Fill atlas to trigger eviction on the next wide insert
    for (int i = 0; i < 40; i++) {
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0x10);
        rend_sdl3_atlas_insert(&atlas, font, 1000 + i, 0, bmp);
        free_bitmap(bmp);
    }

    // This wide glyph triggers eviction (2000 + 50 > 2048)
    GlyphBitmap *trigger = make_bitmap(2000, 49, 0x99);
    RendSdl3AtlasEntry *te = rend_sdl3_atlas_insert(&atlas, font, 9999, 0, trigger);
    ASSERT_TRUE(te != NULL);
    ASSERT_TRUE(atlas.eviction_occurred);
    free_bitmap(trigger);

    // Now insert 3 more glyphs with distinct fill values post-eviction
    uint8_t fills[] = { 0xAA, 0xBB, 0xCC };
    RendSdl3AtlasEntry *entries[3];
    GlyphBitmap *bmps[3];

    for (int i = 0; i < 3; i++) {
        bmps[i] = make_bitmap(10, 10, fills[i]);
        entries[i] = rend_sdl3_atlas_insert(&atlas, font, 3000 + i, 0, bmps[i]);
        ASSERT_TRUE(entries[i] != NULL);
    }

    // Verify staging data for all 3 glyphs — none should be zeroed out
    int staging_pitch = REND_SDL3_ATLAS_TEXTURE_SIZE * 4;
    for (int g = 0; g < 3; g++) {
        uint8_t *row = atlas.staging + entries[g]->region.y * staging_pitch +
                       entries[g]->region.x * 4;
        for (int x = 0; x < 10 * 4; x++) {
            ASSERT_EQ(row[x], fills[g]);
        }
    }

    for (int i = 0; i < 3; i++)
        free_bitmap(bmps[i]);
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
        // Fill atlas with wide glyphs to trigger spatial eviction
        for (int i = 0; i < 40; i++) {
            GlyphBitmap *bmp = make_bitmap(2000, 49, 0x10);
            RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(
                &atlas, font, cycle * 1000 + i, 0, bmp);
            ASSERT_TRUE(e != NULL);
            free_bitmap(bmp);
        }

        // Trigger eviction
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0xEE);
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(
            &atlas, font, cycle * 1000 + 500, 0, bmp);
        ASSERT_TRUE(e != NULL);
        free_bitmap(bmp);

        // Verify the last entry survived
        RendSdl3AtlasEntry *found = rend_sdl3_atlas_lookup(
            &atlas, font, cycle * 1000 + 500, 0);
        ASSERT_TRUE(found != NULL);
    }

    ASSERT_TRUE(atlas.eviction_occurred);
    rend_sdl3_atlas_destroy(&atlas);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("Atlas unit tests\n");

    RUN_TEST(test_insert_and_lookup);
    RUN_TEST(test_shelf_reuse);
    RUN_TEST(test_staging_buffer_contents);
    RUN_TEST(test_eviction_sets_flag);
    RUN_TEST(test_eviction_clears_old_entries);
    RUN_TEST(test_load_factor_eviction);
    RUN_TEST(test_post_eviction_staging_correct);

    printf("\nRegression tests\n");

    RUN_TEST(test_regression_bulk_inserts_never_fail);
    RUN_TEST(test_regression_post_eviction_all_entries_reachable);
    RUN_TEST(test_regression_multi_glyph_staging_after_eviction);
    RUN_TEST(test_regression_repeated_eviction_cycles);

    TEST_SUMMARY();
}
