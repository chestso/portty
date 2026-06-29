#ifndef HEAP_HARDEN_H
#define HEAP_HARDEN_H

#include <bloom-vt/bloom_vt.h>

/*
 * Hardened BvtAllocator. Wraps stdlib malloc/realloc/free with:
 *
 *   - 16-byte header before each chunk: 8-byte magic ('BLM-VT  ') +
 *     8-byte logical size.
 *   - 4-byte canary (0xDEADBEEF) trailing the user payload.
 *   - On realloc/free: verify magic + canary; on mismatch, PORTTY_BUG_ABORT
 *     with the chunk pointer, expected vs. actual values, and the
 *     heap_stats post-mortem dump.
 *   - On free: poison the user payload with 0xDD so a use-after-free
 *     read sees obvious garbage; invalidate the magic so a second free
 *     is reported as a magic mismatch instead of touching freed memory.
 *   - heap_stats counters (alloc/free/realloc, signed bytes_live to catch
 *     unbalanced frees, peak_live for triage).
 *   - 64-entry ring buffer of recent allocations with return-address
 *     site captured via backtrace(3) — dumped by heap_harden_dump on
 *     any PORTTY_BUG_ABORT path.
 *
 * Layered with ASan: ASan watches the chunk boundary; this wrapper
 * additionally catches scribbles into the user payload of an adjacent
 * chunk (within the wrapper's headroom). On a hit, both produce a
 * report and a core.
 *
 * Install once from term_bvt.c via bvt_new_with_allocator(...,
 * &bvt_hardened_allocator). Pair with heap_harden_init() at process
 * start so the dump hook is registered with portty_bug.
 */
extern const BvtAllocator bvt_hardened_allocator;

/* Register heap_stats dump with portty_bug. Idempotent. */
void heap_harden_init(void);

#endif /* HEAP_HARDEN_H */
