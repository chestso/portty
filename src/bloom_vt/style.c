/*
 * bloom-vt — per-page style intern table.
 *
 * Layout:
 *   page->styles.entries[]: dense array of BvtStyle, indexed by style id.
 *   page->styles.index[]:   open-addressed hash table mapping
 *                           (slot = hash & (index_capacity-1)) -> entry id.
 *
 * Slot value `UINT32_MAX` means empty. Style id 0 is reserved for the
 * page's default style and is never inserted via the hash path; we
 * special-case it with a memcmp so cells initialized via memset (id = 0)
 * resolve correctly.
 *
 * Growth: entries doubles when count == capacity. Index doubles (and
 * is rebuilt) when load factor exceeds 70%. Both events are rare in
 * a typical session.
 */

#include "bloom_vt_internal.h"

#include <string.h>

#define STYLE_INDEX_EMPTY UINT32_MAX

static const BvtStyle DEFAULT_STYLE = {
    .fg_rgb = 0,
    .bg_rgb = 0,
    .ul_rgb = 0,
    .attrs = 0,
    .underline = 0,
    .font = 0,
    .color_flags = BVT_COLOR_DEFAULT_FG | BVT_COLOR_DEFAULT_BG | BVT_COLOR_DEFAULT_UL,
};

/* FNV-1a 32-bit. */
static uint32_t fnv1a(const void *data, size_t len)
{
    const uint8_t *b = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static bool ensure_init(BvtTerm *vt, BvtPage *page)
{
    if (page->styles.entries)
        return true;
    page->styles.capacity = BVT_STYLES_INIT;
    page->styles.entries = bvt_alloc(vt, page->styles.capacity * sizeof(BvtStyle));
    if (!page->styles.entries) {
        page->styles.capacity = 0;
        return false;
    }
    /* Slot 0 = default style. */
    page->styles.entries[0] = DEFAULT_STYLE;
    page->styles.count = 1;

    /* Index sized 2x so open addressing has headroom from the start. */
    page->styles.index_capacity = BVT_STYLES_INIT * 2;
    page->styles.index = bvt_alloc(
        vt, page->styles.index_capacity * sizeof(uint32_t));
    if (!page->styles.index) {
        bvt_dealloc(vt, page->styles.entries);
        page->styles.entries = NULL;
        page->styles.capacity = 0;
        page->styles.count = 0;
        page->styles.index_capacity = 0;
        return false;
    }
    for (uint32_t i = 0; i < page->styles.index_capacity; ++i)
        page->styles.index[i] = STYLE_INDEX_EMPTY;
    return true;
}

static bool grow_index(BvtTerm *vt, BvtPage *page)
{
    uint32_t new_cap = page->styles.index_capacity * 2;
    uint32_t *new_index = bvt_alloc(vt, new_cap * sizeof(uint32_t));
    if (!new_index)
        return false;
    for (uint32_t i = 0; i < new_cap; ++i)
        new_index[i] = STYLE_INDEX_EMPTY;
    /* Re-insert each non-default entry. */
    uint32_t mask = new_cap - 1;
    for (uint32_t e = 1; e < page->styles.count; ++e) {
        uint32_t h = fnv1a(&page->styles.entries[e], sizeof(BvtStyle));
        uint32_t slot = h & mask;
        while (new_index[slot] != STYLE_INDEX_EMPTY)
            slot = (slot + 1) & mask;
        new_index[slot] = e;
    }
    bvt_dealloc(vt, page->styles.index);
    page->styles.index = new_index;
    page->styles.index_capacity = new_cap;
    return true;
}

static bool grow_entries(BvtTerm *vt, BvtPage *page)
{
    uint32_t new_cap = page->styles.capacity * 2;
    BvtStyle *ne = bvt_realloc(
        vt, page->styles.entries, new_cap * sizeof(BvtStyle));
    if (!ne)
        return false;
    page->styles.entries = ne;
    page->styles.capacity = new_cap;
    return true;
}

uint32_t bvt_style_intern(BvtTerm *vt, BvtPage *page, const BvtStyle *style)
{
    if (!page)
        return 0;
    /* Default style → reserved id 0, no allocation. */
    if (memcmp(style, &DEFAULT_STYLE, sizeof(*style)) == 0)
        return 0;

    if (!ensure_init(vt, page))
        return 0;

    uint32_t hash = fnv1a(style, sizeof(*style));
    uint32_t mask = page->styles.index_capacity - 1;
    uint32_t slot = hash & mask;

    /* Probe for either a match or an empty slot. */
    for (;;) {
        uint32_t entry_idx = page->styles.index[slot];
        if (entry_idx == STYLE_INDEX_EMPTY) {
            /* Insert. */
            if (page->styles.count >= page->styles.capacity) {
                if (!grow_entries(vt, page))
                    return 0;
            }
            uint32_t new_id = page->styles.count++;
            page->styles.entries[new_id] = *style;
            page->styles.index[slot] = new_id;

            /* Maintain load factor < 70%. Rebuild rehashes; the just-
             * placed entry's slot may move, which is fine. */
            if (page->styles.count * 10 > page->styles.index_capacity * 7) {
                grow_index(vt, page);
            }
            return new_id;
        }
        if (memcmp(&page->styles.entries[entry_idx], style, sizeof(*style)) == 0)
            return entry_idx;
        slot = (slot + 1) & mask;
    }
}

const BvtStyle *bvt_style_lookup(const BvtPage *page, uint32_t id)
{
    if (id == 0) {
        /* If the page has been initialized, entries[0] holds the
         * default; otherwise return the static default. Both yield
         * stable pointers within a single page lifetime, which is what
         * callers need. */
        if (page && page->styles.entries)
            return &page->styles.entries[0];
        return &DEFAULT_STYLE;
    }
    if (!page || id >= page->styles.count)
        return NULL;
    return &page->styles.entries[id];
}
