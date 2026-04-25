/*
 * bloom-vt — per-page grapheme arena (UAX #29 cluster storage).
 *
 * Multi-codepoint clusters live in a per-page bump allocator with an
 * open-addressed dedup table. Each entry is laid out as three or more
 * uint32 words:
 *   [0] non-zero hash
 *   [1] codepoint count (>= 2; len 1 clusters use cell.cp directly)
 *   [2..2+len) the cluster's codepoints in canonical order
 *
 * The id stored on a BvtCell is the offset (in uint32 words) of word
 * [0]. id 0 is reserved for the single-codepoint case so cells
 * initialized via memset resolve to "no extended grapheme".
 *
 * Dedup: a screen full of the same emoji shares one entry. Lookup is
 * an FNV hash of the cluster, then open-addressed probing comparing
 * stored hash + length + bytes.
 *
 * Lifetime: page-scoped. Freeing a page frees the entire arena in
 * a single free, eliminating per-cluster fragmentation.
 */

#include "bloom_vt_internal.h"

#include <string.h>

#define DEDUP_EMPTY 0u /* offset 0 reserved as sentinel */

static uint32_t fnv1a_words(const uint32_t *data, uint32_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    size_t bytes = (size_t)len * sizeof(uint32_t);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= b[i];
        h *= 16777619u;
    }
    /* Reserve 0 to mean "no hash" — coerce to 1 if it lands there. */
    return h ? h : 1u;
}

static bool ensure_init(BvtTerm *vt, BvtPage *page)
{
    if (page->graphemes.codepoints)
        return true;
    /* Initial codepoint pool — small power of two. */
    uint32_t cap_words = BVT_ARENA_CP_INIT / sizeof(uint32_t);
    if (cap_words < 8)
        cap_words = 8;
    page->graphemes.codepoints = bvt_alloc(vt, cap_words * sizeof(uint32_t));
    if (!page->graphemes.codepoints)
        return false;
    page->graphemes.capacity = cap_words;
    /* Reserve offset 0 as the "no extended grapheme" sentinel. We
     * stash a zero-length stub there so reads at id 0 are well-defined. */
    page->graphemes.codepoints[0] = 0u; /* hash */
    page->graphemes.codepoints[1] = 0u; /* len  */
    page->graphemes.used = 2;

    page->graphemes.dedup_capacity = BVT_ARENA_DEDUP_INIT;
    page->graphemes.dedup_index = bvt_alloc(
        vt, page->graphemes.dedup_capacity * sizeof(uint32_t));
    if (!page->graphemes.dedup_index) {
        bvt_dealloc(vt, page->graphemes.codepoints);
        page->graphemes.codepoints = NULL;
        page->graphemes.capacity = 0;
        page->graphemes.used = 0;
        page->graphemes.dedup_capacity = 0;
        return false;
    }
    for (uint32_t i = 0; i < page->graphemes.dedup_capacity; ++i)
        page->graphemes.dedup_index[i] = DEDUP_EMPTY;
    return true;
}

static bool grow_codepoints(BvtTerm *vt, BvtPage *page, uint32_t need_words)
{
    uint32_t new_cap = page->graphemes.capacity;
    while (new_cap < page->graphemes.used + need_words)
        new_cap *= 2;
    if (new_cap == page->graphemes.capacity)
        return true;
    uint32_t *nc = bvt_realloc(
        vt, page->graphemes.codepoints, new_cap * sizeof(uint32_t));
    if (!nc)
        return false;
    page->graphemes.codepoints = nc;
    page->graphemes.capacity = new_cap;
    return true;
}

static bool grow_dedup(BvtTerm *vt, BvtPage *page)
{
    uint32_t new_cap = page->graphemes.dedup_capacity * 2;
    uint32_t *new_idx = bvt_alloc(vt, new_cap * sizeof(uint32_t));
    if (!new_idx)
        return false;
    for (uint32_t i = 0; i < new_cap; ++i)
        new_idx[i] = DEDUP_EMPTY;
    uint32_t mask = new_cap - 1;
    /* Walk the arena and re-insert every entry. */
    uint32_t off = 2; /* skip sentinel */
    while (off < page->graphemes.used) {
        uint32_t hash = page->graphemes.codepoints[off];
        uint32_t len = page->graphemes.codepoints[off + 1];
        uint32_t slot = hash & mask;
        while (new_idx[slot] != DEDUP_EMPTY)
            slot = (slot + 1) & mask;
        new_idx[slot] = off;
        off += 2 + len;
    }
    bvt_dealloc(vt, page->graphemes.dedup_index);
    page->graphemes.dedup_index = new_idx;
    page->graphemes.dedup_capacity = new_cap;
    return true;
}

uint32_t bvt_grapheme_intern(BvtTerm *vt, BvtPage *page,
                             const uint32_t *cps, uint32_t len)
{
    if (!page || len < 2)
        return 0;
    if (!ensure_init(vt, page))
        return 0;

    uint32_t hash = fnv1a_words(cps, len);
    uint32_t mask = page->graphemes.dedup_capacity - 1;
    uint32_t slot = hash & mask;

    for (;;) {
        uint32_t off = page->graphemes.dedup_index[slot];
        if (off == DEDUP_EMPTY) {
            /* Insert: ensure space, append. */
            uint32_t need = 2 + len;
            if (page->graphemes.used + need > page->graphemes.capacity) {
                if (!grow_codepoints(vt, page, need))
                    return 0;
            }
            uint32_t new_off = page->graphemes.used;
            page->graphemes.codepoints[new_off] = hash;
            page->graphemes.codepoints[new_off + 1] = len;
            memcpy(&page->graphemes.codepoints[new_off + 2], cps,
                   (size_t)len * sizeof(uint32_t));
            page->graphemes.used += need;
            page->graphemes.dedup_index[slot] = new_off;
            page->graphemes.dedup_count++;

            /* Keep load factor below 70%. */
            if (page->graphemes.dedup_count * 10 > page->graphemes.dedup_capacity * 7) {
                grow_dedup(vt, page);
            }
            return new_off;
        }
        uint32_t stored_hash = page->graphemes.codepoints[off];
        uint32_t stored_len = page->graphemes.codepoints[off + 1];
        if (stored_hash == hash && stored_len == len &&
            memcmp(&page->graphemes.codepoints[off + 2], cps,
                   (size_t)len * sizeof(uint32_t)) == 0) {
            return off;
        }
        slot = (slot + 1) & mask;
    }
}

size_t bvt_grapheme_read(const BvtPage *page, uint32_t id,
                         uint32_t *out, size_t out_cap)
{
    if (!page || id == 0 || !out || out_cap == 0)
        return 0;
    if (!page->graphemes.codepoints)
        return 0;
    if (id + 1 >= page->graphemes.used)
        return 0;
    uint32_t len = page->graphemes.codepoints[id + 1];
    if (id + 2 + len > page->graphemes.used)
        return 0;
    size_t to_copy = (len < out_cap) ? (size_t)len : out_cap;
    memcpy(out, &page->graphemes.codepoints[id + 2],
           to_copy * sizeof(uint32_t));
    return to_copy;
}
