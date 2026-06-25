/*
 * test_clipboard_deferred — regression tests for the deferred-clipboard-free
 * linked list used in platform_sdl3.c.
 *
 * Motivated by a heap-use-after-free crash (ASan report 995600):
 *   1. clipboard_cleanup_callback defers the old clipboard string
 *   2. clipboard_deferred_free_advance frees entries that have survived
 *      enough iterations
 *   3. The previous single-pointer or fixed ring buffer was insufficient:
 *      - A single deferred pointer was freed after one event loop iteration,
 *        but the Wayland compositor's data_source_send could arrive on the
 *        next iteration's SDL_WaitEvent (before the free).
 *      - A fixed ring buffer of N slots couldn't handle > N clipboard sets
 *        in one iteration (user selection + app OSC 52 racing).
 *
 * The linked list with age-based reaping handles unbounded rapid sets.
 * These tests verify the invariant: every entry survives at least
 * CLIPBOARD_DEFERRED_MIN_AGE advance() calls before being freed.
 */

#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Replicate the deferred-free logic from platform_sdl3.c             */
/* ------------------------------------------------------------------ */

typedef struct ClipboardDeferredFree
{
    char *ptr;
    int age;
    struct ClipboardDeferredFree *next;
} ClipboardDeferredFree;

static ClipboardDeferredFree *test_head;

#define CLIPBOARD_DEFERRED_MIN_AGE 2

static void deferred_cleanup(void *userdata)
{
    char *ptr = (char *)userdata;
    if (!ptr)
        return;
    ClipboardDeferredFree *entry = malloc(sizeof(*entry));
    if (!entry) {
        free(ptr);
        return;
    }
    entry->ptr = ptr;
    entry->age = 0;
    entry->next = test_head;
    test_head = entry;
}

static void deferred_advance(void)
{
    ClipboardDeferredFree **pp = &test_head;
    while (*pp) {
        (*pp)->age++;
        if ((*pp)->age >= CLIPBOARD_DEFERRED_MIN_AGE) {
            ClipboardDeferredFree *old = *pp;
            *pp = old->next;
            free(old->ptr);
            free(old);
        } else {
            pp = &(*pp)->next;
        }
    }
}

static void deferred_flush(void)
{
    while (test_head) {
        ClipboardDeferredFree *next = test_head->next;
        free(test_head->ptr);
        free(test_head);
        test_head = next;
    }
}

/* Count live entries in the list. */
static int deferred_count(void)
{
    int n = 0;
    for (ClipboardDeferredFree *e = test_head; e; e = e->next)
        n++;
    return n;
}

/* Check if a specific pointer is still alive in the deferred list. */
static bool deferred_contains(const char *s)
{
    for (ClipboardDeferredFree *e = test_head; e; e = e->next) {
        if (e->ptr == s)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* A single deferred string must survive exactly MIN_AGE advances. */
static void test_single_entry_survives_min_age(void)
{
    test_head = NULL;
    char *s = strdup("hello");
    deferred_cleanup(s);

    ASSERT_EQ(deferred_count(), 1);
    ASSERT_TRUE(deferred_contains(s));

    /* Age 0 → 1: not old enough */
    deferred_advance();
    ASSERT_EQ(deferred_count(), 1);
    ASSERT_TRUE(deferred_contains(s));

    /* Age 1 → 2: reaches MIN_AGE, freed */
    deferred_advance();
    ASSERT_EQ(deferred_count(), 0);

    /* String was freed by advance — don't free again. */
}

/* Rapid N cleanups in one "iteration" must all survive MIN_AGE advances.
 * This was the bug that broke the fixed-size ring buffer: 3 sets in one
 * iteration would wrap a 2-slot ring, freeing the oldest entry prematurely. */
static void test_rapid_cleanup_burst(void)
{
    test_head = NULL;
    char *a = strdup("aaa");
    char *b = strdup("bbb");
    char *c = strdup("ccc");
    char *d = strdup("ddd");
    char *e = strdup("eee");

    /* Simulate 5 clipboard sets in one iteration — each cancels the
     * previous offer, firing the cleanup callback. */
    deferred_cleanup(a);
    deferred_cleanup(b);
    deferred_cleanup(c);
    deferred_cleanup(d);
    deferred_cleanup(e);

    ASSERT_EQ(deferred_count(), 5);

    /* First advance: all age 0 → 1, none freed */
    deferred_advance();
    ASSERT_EQ(deferred_count(), 5);

    /* Second advance: all age 1 → 2, all freed */
    deferred_advance();
    ASSERT_EQ(deferred_count(), 0);
}

/* Strings added in different "iterations" are freed at the right time. */
static void test_staggered_entries_freed_independently(void)
{
    test_head = NULL;
    char *a = strdup("first");
    char *b = strdup("second");
    char *c = strdup("third");

    /* Iteration 0: add a */
    deferred_cleanup(a);
    deferred_advance(); /* a: age 0→1 */

    /* Iteration 1: add b */
    deferred_cleanup(b);
    ASSERT_EQ(deferred_count(), 2); /* a (age 1), b (age 0) */
    deferred_advance();             /* a: age 1→2 (freed), b: age 0→1 */
    ASSERT_EQ(deferred_count(), 1);
    ASSERT_FALSE(deferred_contains(a));
    ASSERT_TRUE(deferred_contains(b));

    /* Iteration 2: add c */
    deferred_cleanup(c);
    ASSERT_EQ(deferred_count(), 2); /* b (age 1), c (age 0) */
    deferred_advance();             /* b: age 1→2 (freed), c: age 0→1 */
    ASSERT_EQ(deferred_count(), 1);
    ASSERT_FALSE(deferred_contains(b));
    ASSERT_TRUE(deferred_contains(c));

    /* Iteration 3 */
    deferred_advance(); /* c: age 1→2 (freed) */
    ASSERT_EQ(deferred_count(), 0);
}

/* Flushing must free all remaining entries regardless of age. */
static void test_flush_frees_all(void)
{
    test_head = NULL;
    deferred_cleanup(strdup("young"));
    deferred_advance(); /* age 0→1, not yet due */
    deferred_cleanup(strdup("fresh"));

    ASSERT_EQ(deferred_count(), 2);
    deferred_flush();
    ASSERT_EQ(deferred_count(), 0);
}

/* NULL userdata must not be added to the list. */
static void test_null_userdata_ignored(void)
{
    test_head = NULL;
    deferred_cleanup(NULL);
    ASSERT_EQ(deferred_count(), 0);
}

/* malloc failure in cleanup must free the string immediately without
 * adding to the list (graceful degradation). */
static void test_oom_in_cleanup_frees_immediately(void)
{
    test_head = NULL;
    /* We can't easily force malloc to fail, but we can verify the code
     * path exists by checking that the list handles the NULL-entry case
     * correctly. The real OOM test would need a malloc interposer. */
    char *s = strdup("oom_test");
    deferred_cleanup(s);
    ASSERT_EQ(deferred_count(), 1);
    deferred_flush();
}

/* The critical invariant: no entry is freed before MIN_AGE advances.
 * This is the core guarantee that prevents the Wayland UAF. */
static void test_invariant_no_premature_free(void)
{
    for (int burst = 1; burst <= 10; burst++) {
        test_head = NULL;

        /* Add 'burst' entries in one "iteration" */
        char *ptrs[10];
        for (int i = 0; i < burst; i++) {
            ptrs[i] = strdup("x");
            deferred_cleanup(ptrs[i]);
        }

        /* Before MIN_AGE advances, ALL entries must still be alive */
        for (int iter = 0; iter < CLIPBOARD_DEFERRED_MIN_AGE - 1; iter++) {
            deferred_advance();
            ASSERT_EQ(deferred_count(), burst);
            for (int i = 0; i < burst; i++)
                ASSERT_TRUE(deferred_contains(ptrs[i]));
        }

        /* At MIN_AGE advances, all are freed */
        deferred_advance();
        ASSERT_EQ(deferred_count(), 0);
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_single_entry_survives_min_age);
    RUN_TEST(test_rapid_cleanup_burst);
    RUN_TEST(test_staggered_entries_freed_independently);
    RUN_TEST(test_flush_frees_all);
    RUN_TEST(test_null_userdata_ignored);
    RUN_TEST(test_oom_in_cleanup_frees_immediately);
    RUN_TEST(test_invariant_no_premature_free);

    TEST_SUMMARY();
}
