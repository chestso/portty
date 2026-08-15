/*
 * portty — Lottie cache logic tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "term.h"
#include "test_helpers.h"
#include <stdlib.h>
#include <string.h>

// ---- Lottie cache entry (mirrors the GPU-specific structs in both backends) ----

#define LOTTIE_CACHE_MAX 64

typedef struct
{
    int texture; // fake texture handle (0 = none, 1+ = "allocated")
    uint64_t id;
    uint32_t version;
    int w, h;
} TestLottieEntry;

typedef struct
{
    TestLottieEntry entries[LOTTIE_CACHE_MAX];
    int count;
} TestLottieCache;

// ---- Cache logic (pure, no GPU calls) ----

// Returns index of entry with matching id, or -1 if not found.
static int lottie_cache_find(TestLottieCache *cache, uint64_t id)
{
    for (int i = 0; i < cache->count; i++)
        if (cache->entries[i].id == id)
            return i;
    return -1;
}

// Returns:
//  0 = cache hit (same version, same size)
//  1 = version changed (texture needs update, same size)
//  2 = size changed (texture needs recreate)
//  3 = not in cache (new entry needed)
static int lottie_cache_check(TestLottieCache *cache, uint64_t id,
                              uint32_t version, int w, int h)
{
    int idx = lottie_cache_find(cache, id);
    if (idx < 0)
        return 3;
    if (cache->entries[idx].w != w || cache->entries[idx].h != h)
        return 2;
    if (cache->entries[idx].version != version)
        return 1;
    return 0;
}

// Remove entries whose id is not in the active list.
// Calls destroy_fn for each removed entry's texture.
static void lottie_cache_reconcile(TestLottieCache *cache,
                                   const CfrLottie *anims, int count)
{
    for (int i = 0; i < cache->count;) {
        bool live = false;
        for (int j = 0; j < count; j++) {
            if (anims[j].id == cache->entries[i].id) {
                live = true;
                break;
            }
        }
        if (live) {
            i++;
        } else {
            // "Destroy" texture (in tests, just set to 0)
            cache->entries[i].texture = 0;
            cache->entries[i] = cache->entries[--cache->count];
        }
    }
}

// Insert or update a cache entry. Returns the index, or -1 if cache is full.
static int lottie_cache_upsert(TestLottieCache *cache, uint64_t id,
                               uint32_t version, int w, int h, int texture)
{
    int idx = lottie_cache_find(cache, id);
    if (idx >= 0) {
        cache->entries[idx].texture = texture;
        cache->entries[idx].version = version;
        cache->entries[idx].w = w;
        cache->entries[idx].h = h;
        return idx;
    }
    if (cache->count >= LOTTIE_CACHE_MAX)
        return -1;
    idx = cache->count++;
    cache->entries[idx].texture = texture;
    cache->entries[idx].id = id;
    cache->entries[idx].version = version;
    cache->entries[idx].w = w;
    cache->entries[idx].h = h;
    return idx;
}

// ---- Tests ----

static void test_cache_find_empty(void)
{
    TestLottieCache cache = { 0 };
    ASSERT_EQ(lottie_cache_find(&cache, 42), -1);
}

static void test_cache_find_existing(void)
{
    TestLottieCache cache = { 0 };
    lottie_cache_upsert(&cache, 10, 1, 100, 100, 1);
    lottie_cache_upsert(&cache, 20, 1, 200, 200, 2);
    ASSERT_EQ(lottie_cache_find(&cache, 10), 0);
    ASSERT_EQ(lottie_cache_find(&cache, 20), 1);
    ASSERT_EQ(lottie_cache_find(&cache, 99), -1);
}

static void test_cache_check_hit(void)
{
    TestLottieCache cache = { 0 };
    lottie_cache_upsert(&cache, 10, 5, 100, 100, 1);
    ASSERT_EQ(lottie_cache_check(&cache, 10, 5, 100, 100), 0);
}

static void test_cache_check_version_changed(void)
{
    TestLottieCache cache = { 0 };
    lottie_cache_upsert(&cache, 10, 5, 100, 100, 1);
    ASSERT_EQ(lottie_cache_check(&cache, 10, 6, 100, 100), 1);
}

static void test_cache_check_size_changed(void)
{
    TestLottieCache cache = { 0 };
    lottie_cache_upsert(&cache, 10, 5, 100, 100, 1);
    ASSERT_EQ(lottie_cache_check(&cache, 10, 5, 200, 200), 2);
}

static void test_cache_check_not_found(void)
{
    TestLottieCache cache = { 0 };
    lottie_cache_upsert(&cache, 10, 5, 100, 100, 1);
    ASSERT_EQ(lottie_cache_check(&cache, 99, 1, 50, 50), 3);
}

static void test_cache_reconcile_removes_stale(void)
{
    TestLottieCache cache = { 0 };
    lottie_cache_upsert(&cache, 10, 1, 100, 100, 1);
    lottie_cache_upsert(&cache, 20, 1, 200, 200, 2);
    lottie_cache_upsert(&cache, 30, 1, 300, 300, 3);

    // Active animations: only id=20
    CfrLottie anims[1] = { { .id = 20 } };
    lottie_cache_reconcile(&cache, anims, 1);

    ASSERT_EQ(cache.count, 1);
    ASSERT_EQ(cache.entries[0].id, 20);
    ASSERT_EQ(cache.entries[0].texture, 2);
}

static void test_cache_reconcile_empty_anims(void)
{
    TestLottieCache cache = { 0 };
    lottie_cache_upsert(&cache, 10, 1, 100, 100, 1);
    lottie_cache_upsert(&cache, 20, 1, 200, 200, 2);

    lottie_cache_reconcile(&cache, NULL, 0);

    ASSERT_EQ(cache.count, 0);
}

static void test_cache_reconcile_all_live(void)
{
    TestLottieCache cache = { 0 };
    lottie_cache_upsert(&cache, 10, 1, 100, 100, 1);
    lottie_cache_upsert(&cache, 20, 1, 200, 200, 2);

    CfrLottie anims[2] = { { .id = 10 }, { .id = 20 } };
    lottie_cache_reconcile(&cache, anims, 2);

    ASSERT_EQ(cache.count, 2);
}

static void test_cache_upsert_new(void)
{
    TestLottieCache cache = { 0 };
    int idx = lottie_cache_upsert(&cache, 10, 1, 100, 100, 1);
    ASSERT_EQ(idx, 0);
    ASSERT_EQ(cache.count, 1);
    ASSERT_EQ(cache.entries[0].id, 10);
    ASSERT_EQ(cache.entries[0].version, 1);
    ASSERT_EQ(cache.entries[0].w, 100);
    ASSERT_EQ(cache.entries[0].h, 100);
    ASSERT_EQ(cache.entries[0].texture, 1);
}

static void test_cache_upsert_update(void)
{
    TestLottieCache cache = { 0 };
    lottie_cache_upsert(&cache, 10, 1, 100, 100, 1);
    int idx = lottie_cache_upsert(&cache, 10, 2, 200, 200, 3);
    ASSERT_EQ(idx, 0);
    ASSERT_EQ(cache.count, 1);
    ASSERT_EQ(cache.entries[0].version, 2);
    ASSERT_EQ(cache.entries[0].w, 200);
    ASSERT_EQ(cache.entries[0].texture, 3);
}

static void test_cache_upsert_full(void)
{
    TestLottieCache cache = { 0 };
    for (int i = 0; i < LOTTIE_CACHE_MAX; i++)
        lottie_cache_upsert(&cache, (uint64_t)(i + 1), 1, 50, 50, i + 1);
    ASSERT_EQ(cache.count, LOTTIE_CACHE_MAX);
    int idx = lottie_cache_upsert(&cache, 999, 1, 50, 50, 999);
    ASSERT_EQ(idx, -1);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("test_lottie_cache\n");

    RUN_TEST(test_cache_find_empty);
    RUN_TEST(test_cache_find_existing);
    RUN_TEST(test_cache_check_hit);
    RUN_TEST(test_cache_check_version_changed);
    RUN_TEST(test_cache_check_size_changed);
    RUN_TEST(test_cache_check_not_found);
    RUN_TEST(test_cache_reconcile_removes_stale);
    RUN_TEST(test_cache_reconcile_empty_anims);
    RUN_TEST(test_cache_reconcile_all_live);
    RUN_TEST(test_cache_upsert_new);
    RUN_TEST(test_cache_upsert_update);
    RUN_TEST(test_cache_upsert_full);

    TEST_SUMMARY();
}
