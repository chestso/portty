#include "test_helpers.h"
#include "rend_sdl3_atlas.h"
#include <stdlib.h>
#include <string.h>

// Satisfy common.h requirements
int verbose = 0;

void vlog_impl(const char *file, const char *func, int line, const char *format, ...)
{
    if (!verbose)
        return;
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[%s:%s:%d] ", file, func, line);
    vfprintf(stderr, format, args);
    va_end(args);
}

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

// Test 2: Small glyphs go to page 0; large glyphs go to page 1
static void test_page_selection(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x2000;

    // Small glyph (<=48px) → page 0
    GlyphBitmap *small_bmp = make_bitmap(20, 20, 0x11);
    RendSdl3AtlasEntry *small_entry = rend_sdl3_atlas_insert(&atlas, font, 1, 0, small_bmp);
    ASSERT_TRUE(small_entry != NULL);
    ASSERT_EQ(small_entry->page_index, 0);

    // Large glyph (>48px) → page 1
    GlyphBitmap *large_bmp = make_bitmap(60, 60, 0x22);
    RendSdl3AtlasEntry *large_entry = rend_sdl3_atlas_insert(&atlas, font, 2, 0, large_bmp);
    ASSERT_TRUE(large_entry != NULL);
    ASSERT_EQ(large_entry->page_index, 1);

    // Boundary: exactly 48px → page 0
    GlyphBitmap *boundary_bmp = make_bitmap(48, 48, 0x33);
    RendSdl3AtlasEntry *boundary_entry = rend_sdl3_atlas_insert(&atlas, font, 3, 0, boundary_bmp);
    ASSERT_TRUE(boundary_entry != NULL);
    ASSERT_EQ(boundary_entry->page_index, 0);

    // Boundary: 49px width → page 1
    GlyphBitmap *over_bmp = make_bitmap(49, 10, 0x44);
    RendSdl3AtlasEntry *over_entry = rend_sdl3_atlas_insert(&atlas, font, 4, 0, over_bmp);
    ASSERT_TRUE(over_entry != NULL);
    ASSERT_EQ(over_entry->page_index, 1);

    free_bitmap(small_bmp);
    free_bitmap(large_bmp);
    free_bitmap(boundary_bmp);
    free_bitmap(over_bmp);
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
    RendSdl3AtlasPage *page = &atlas.pages[entry->page_index];
    int staging_pitch = REND_SDL3_ATLAS_TEXTURE_SIZE * 4;

    for (int y = 0; y < 3; y++) {
        uint8_t *row = page->staging + (entry->region.y + y) * staging_pitch +
                       entry->region.x * 4;
        for (int x = 0; x < 4 * 4; x++) {
            ASSERT_EQ(row[x], 0xBB);
        }
    }

    free_bitmap(bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

// Test 4: Filling page 1 then inserting one more sets eviction_occurred = true
static void test_eviction_sets_flag(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x4000;
    ASSERT_TRUE(!atlas.eviction_occurred);

    // Insert 40 large glyphs (2000x49) — each gets its own shelf (50px tall)
    // After 40: next_shelf_y = 2000
    for (int i = 0; i < 40; i++) {
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0x10 + (uint8_t)i);
        RendSdl3AtlasEntry *e = rend_sdl3_atlas_insert(&atlas, font, 100 + i, 0, bmp);
        ASSERT_TRUE(e != NULL);
        ASSERT_EQ(e->page_index, 1);
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

// Test 5: After eviction, old page-1 entries gone from hash table
static void test_eviction_clears_old_entries(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x5000;

    // Fill page 1 with 40 glyphs
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

// Test 6: Page-0 entry survives page-1 eviction
static void test_eviction_preserves_other_page(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x6000;

    // Insert a small glyph on page 0
    GlyphBitmap *small_bmp = make_bitmap(10, 10, 0xCC);
    RendSdl3AtlasEntry *small_entry = rend_sdl3_atlas_insert(&atlas, font, 1, 0, small_bmp);
    ASSERT_TRUE(small_entry != NULL);
    ASSERT_EQ(small_entry->page_index, 0);
    free_bitmap(small_bmp);

    // Fill page 1 with 40 large glyphs
    for (int i = 0; i < 40; i++) {
        GlyphBitmap *bmp = make_bitmap(2000, 49, 0x10);
        rend_sdl3_atlas_insert(&atlas, font, 100 + i, 0, bmp);
        free_bitmap(bmp);
    }

    // Trigger page-1 eviction
    GlyphBitmap *trigger_bmp = make_bitmap(2000, 49, 0xFF);
    rend_sdl3_atlas_insert(&atlas, font, 200, 0, trigger_bmp);
    free_bitmap(trigger_bmp);

    ASSERT_TRUE(atlas.eviction_occurred);

    // Page-0 entry should still be findable
    RendSdl3AtlasEntry *found = rend_sdl3_atlas_lookup(&atlas, font, 1, 0);
    ASSERT_TRUE(found != NULL);
    ASSERT_EQ(found->page_index, 0);

    rend_sdl3_atlas_destroy(&atlas);
}

// Test 7: After eviction, new glyph has correct pixels in staging
// Regression test: ensures post-eviction insert copies pixel data correctly
static void test_post_eviction_staging_correct(void)
{
    RendSdl3Atlas atlas;
    ASSERT_TRUE(rend_sdl3_atlas_init(&atlas, NULL));

    void *font = (void *)0x7000;

    // Fill page 1 to trigger eviction
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
    RendSdl3AtlasPage *page = &atlas.pages[entry->page_index];
    int staging_pitch = REND_SDL3_ATLAS_TEXTURE_SIZE * 4;

    // Check first row of the glyph in staging
    uint8_t *row = page->staging + entry->region.y * staging_pitch + entry->region.x * 4;
    // Check a few pixels (first 16 bytes = 4 RGBA pixels)
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(row[i], marker);
    }

    // Check last row too
    uint8_t *last_row = page->staging + (entry->region.y + 48) * staging_pitch +
                        entry->region.x * 4;
    for (int i = 0; i < 16; i++) {
        ASSERT_EQ(last_row[i], marker);
    }

    free_bitmap(trigger_bmp);
    rend_sdl3_atlas_destroy(&atlas);
}

int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0)
        verbose = 1;

    printf("Atlas unit tests\n");

    RUN_TEST(test_insert_and_lookup);
    RUN_TEST(test_page_selection);
    RUN_TEST(test_staging_buffer_contents);
    RUN_TEST(test_eviction_sets_flag);
    RUN_TEST(test_eviction_clears_old_entries);
    RUN_TEST(test_eviction_preserves_other_page);
    RUN_TEST(test_post_eviction_staging_correct);

    TEST_SUMMARY();
}
