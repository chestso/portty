/*
 * portty — Hardened heap allocator wrapping coffer's allocator
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "heap_harden.h"

#include "portty_bug.h"

#ifndef _WIN32
#include <execinfo.h>
#endif
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 'BLM-VT  ' — eight printable bytes so a hex dump of a chunk header
 * is recognisable to a human looking at the crash log. */
#define HEAP_MAGIC     0x424c4d2d56542020ULL
#define HEAP_CANARY    0xDEADBEEFu
#define HEAP_RING_SIZE 64u
#define HEAP_POISON    0xDD

typedef struct
{
    uint64_t magic;
    uint64_t size;
} alloc_header;

static struct
{
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t realloc_count;
    int64_t bytes_live; /* signed: an unbalanced free trips it negative */
    uint64_t peak_live;
    struct
    {
        size_t size;
        void *site;
    } ring[HEAP_RING_SIZE];
    uint32_t ring_head;
} heap_stats;

/* Capture the caller-of-our-caller of the allocator hop:
 *   frames[0] = capture_site
 *   frames[1] = hardened_alloc (us)
 *   frames[2] = cfr_alloc/realloc (in libcoffer.a)
 *   frames[3] = the actual user (style.c, grapheme.c, ...)
 * frames[3] is what we want — the bvt internal site that asked for
 * memory. backtrace() can return fewer frames on a shallow stack; use
 * the deepest available. */
#ifndef _WIN32
static void *capture_site(void)
{
    void *frames[4];
    int n = backtrace(frames, 4);
    if (n <= 0)
        return NULL;
    return frames[n - 1];
}
#else
static void *capture_site(void)
{
    return NULL;
}
#endif

static void ring_record(size_t size, void *site)
{
    uint32_t idx = heap_stats.ring_head & (HEAP_RING_SIZE - 1);
    heap_stats.ring[idx].size = size;
    heap_stats.ring[idx].site = site;
    heap_stats.ring_head++;
}

static void heap_harden_dump(void)
{
    fprintf(stderr,
            "HEAP_STATS alloc=%" PRIu64 " free=%" PRIu64 " realloc=%" PRIu64
            " bytes_live=%" PRId64 " peak_live=%" PRIu64 "\n",
            heap_stats.alloc_count, heap_stats.free_count,
            heap_stats.realloc_count, heap_stats.bytes_live,
            heap_stats.peak_live);

    /* Walk the ring oldest → newest. */
    uint32_t window = (heap_stats.ring_head < HEAP_RING_SIZE)
                          ? heap_stats.ring_head
                          : HEAP_RING_SIZE;
    if (window == 0) {
        fprintf(stderr, "HEAP_RING empty\n");
        return;
    }
    fprintf(stderr, "HEAP_RING (last %u allocations, oldest first):\n",
            window);

    void *frames[HEAP_RING_SIZE];
    size_t sizes[HEAP_RING_SIZE];
    uint32_t n = 0;
    uint32_t start = (heap_stats.ring_head - window) & (HEAP_RING_SIZE - 1);
    for (uint32_t i = 0; i < window; ++i) {
        uint32_t idx = (start + i) & (HEAP_RING_SIZE - 1);
        if (heap_stats.ring[idx].site) {
            frames[n] = heap_stats.ring[idx].site;
            sizes[n] = heap_stats.ring[idx].size;
            ++n;
        }
    }
#ifndef _WIN32
    char **syms = (n > 0) ? backtrace_symbols(frames, (int)n) : NULL;
#else
    char **syms = NULL;
#endif
    for (uint32_t i = 0; i < n; ++i) {
        fprintf(stderr, "  [%2u] size=%zu site=%p%s%s\n", i, sizes[i],
                frames[i], syms ? " " : "", syms ? syms[i] : "");
    }
    free(syms);
}

static void check_chunk(const void *p, const char *op)
{
    const alloc_header *h =
        (const alloc_header *)((const uint8_t *)p - sizeof(alloc_header));
    if (h->magic != HEAP_MAGIC) {
        PORTTY_BUG_ABORT("%s: bad magic on chunk %p (magic=%016" PRIx64
                         " expected %016" PRIx64 ")",
                         op, p, h->magic, (uint64_t)HEAP_MAGIC);
    }
    const uint32_t *canary =
        (const uint32_t *)((const uint8_t *)p + h->size);
    if (*canary != HEAP_CANARY) {
        PORTTY_BUG_ABORT("%s: canary scribbled on chunk %p (size=%zu canary=%08x"
                         " expected %08x)",
                         op, p, (size_t)h->size, *canary,
                         (unsigned)HEAP_CANARY);
    }
}

static void *hardened_alloc(size_t size, void *user)
{
    (void)user;
    void *raw = malloc(sizeof(alloc_header) + size + sizeof(uint32_t));
    if (!raw)
        return NULL;
    alloc_header *h = (alloc_header *)raw;
    h->magic = HEAP_MAGIC;
    h->size = size;
    uint8_t *payload = (uint8_t *)raw + sizeof(alloc_header);
    uint32_t canary = HEAP_CANARY;
    /* Use memcpy to dodge any alignment grumble on the trailer write. */
    memcpy(payload + size, &canary, sizeof(canary));

    heap_stats.alloc_count++;
    heap_stats.bytes_live += (int64_t)size;
    if ((uint64_t)heap_stats.bytes_live > heap_stats.peak_live)
        heap_stats.peak_live = (uint64_t)heap_stats.bytes_live;
    ring_record(size, capture_site());
    return payload;
}

static void hardened_free(void *p, void *user)
{
    (void)user;
    if (!p)
        return;
    check_chunk(p, "free");
    alloc_header *h =
        (alloc_header *)((uint8_t *)p - sizeof(alloc_header));
    size_t size = (size_t)h->size;

    /* Poison user payload so a UAF read sees obvious garbage. */
    memset(p, HEAP_POISON, size);
    /* Invalidate magic so a second free reports the right thing. */
    h->magic = 0;

    heap_stats.free_count++;
    heap_stats.bytes_live -= (int64_t)size;
    if (heap_stats.bytes_live < 0) {
        PORTTY_BUG_ABORT("unbalanced free: bytes_live=%" PRId64
                         " went negative (last free ptr=%p size=%zu)",
                         heap_stats.bytes_live, p, size);
    }
    free(h);
}

static void *hardened_realloc(void *p, size_t size, void *user)
{
    if (!p)
        return hardened_alloc(size, user);
    if (size == 0) {
        hardened_free(p, user);
        return NULL;
    }
    check_chunk(p, "realloc");
    alloc_header *old_h =
        (alloc_header *)((uint8_t *)p - sizeof(alloc_header));
    size_t old_size = (size_t)old_h->size;

    /* malloc-new + memcpy + free-old keeps the canary placement
     * correct on shrink AND grow; realloc-in-place would leave the
     * canary at the old offset. */
    void *new_p = hardened_alloc(size, user);
    if (!new_p)
        return NULL;
    size_t copy_n = (old_size < size) ? old_size : size;
    memcpy(new_p, p, copy_n);
    hardened_free(p, user);
    heap_stats.realloc_count++;
    return new_p;
}

const CfrAllocator cfr_hardened_allocator = {
    .alloc = hardened_alloc,
    .realloc = hardened_realloc,
    .free = hardened_free,
    .user = NULL,
};

void heap_harden_init(void)
{
    static int registered = 0;
    if (registered)
        return;
    registered = 1;
    portty_bug_register_dump(heap_harden_dump);
}
