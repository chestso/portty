#ifndef BLOOM_BUG_H
#define BLOOM_BUG_H

#include <stdio.h>
#include <stdlib.h>

/*
 * Crash policy for bug indicators.
 *
 * Throughout bloom-terminal, fragile paths distinguish two failure
 * classes:
 *
 *   - Expected lifecycle (e.g. DMA-BUF unsupported at init, resize
 *     triggers GBM recreate) -- count and continue.
 *
 *   - Bug indicator (e.g. eglMakeCurrent fails mid-session, allocator
 *     canary mismatch, invariant violation) -- BLOOM_BUG_ABORT.
 *
 * BLOOM_BUG_ABORT writes a one-line "BLOOM BUG: <reason> at <file:line>"
 * to stderr, calls every registered dump function (gl_stats, heap_stats,
 * etc.), then abort()s. The abort triggers systemd-coredump (and the
 * ASan/UBSan log_path under sanitised builds), so the bug surfaces at
 * the site instead of silently corrupting state for someone else to
 * trip on later.
 */

/*
 * Register a dump function. Called in registration order from
 * bloom_bug_dump_state(). Up to BLOOM_BUG_MAX_DUMPERS registrations
 * are supported; further calls are silently dropped.
 *
 * The plugin (bloom-terminal-gtk4.so) registers via this symbol
 * exported from the main binary (-rdynamic), so both binary and
 * plugin can contribute dumps to the same post-mortem.
 */
void bloom_bug_register_dump(void (*fn)(void));

/*
 * Call every registered dump function. Invoked by BLOOM_BUG_ABORT
 * before abort(), and can be called manually from a signal handler
 * if desired (stdio in a signal handler is technically UB but
 * works in practice on glibc -- the alternative is losing the
 * post-mortem entirely).
 */
void bloom_bug_dump_state(void);

#define BLOOM_BUG_ABORT(...)                                \
    do {                                                    \
        fprintf(stderr, "BLOOM BUG: " __VA_ARGS__);         \
        fprintf(stderr, " at %s:%d\n", __FILE__, __LINE__); \
        bloom_bug_dump_state();                             \
        fflush(stderr);                                     \
        abort();                                            \
    } while (0)

#endif /* BLOOM_BUG_H */
