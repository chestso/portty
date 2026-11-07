/*
 * portty — Event-driven timer manager tests
 *
 * The scheduler is a fake: it records arm/cancel calls and the test posts
 * "events" by hand, the way the SDL backend's timer callback does.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "test_helpers.h"
#include "../src/timer.h"
#include <string.h>

#define MAX_ARMS 128

typedef struct
{
    uint64_t delay_ns;
    uint64_t period_ns;
    uint32_t slot;
    uint32_t epoch;
    bool cancelled;
} Arm;

typedef struct
{
    Arm arms[MAX_ARMS];
    int count;
    bool refuse; /* arm() fails */
} Fake;

static uintptr_t fake_arm(void *ctx, uint64_t delay_ns, uint64_t period_ns,
                          uint32_t slot, uint32_t epoch)
{
    Fake *f = ctx;
    if (f->refuse || f->count >= MAX_ARMS)
        return 0;
    f->arms[f->count] = (Arm){ delay_ns, period_ns, slot, epoch, false };
    return (uintptr_t)++f->count; /* handle = 1-based index */
}

static void fake_cancel(void *ctx, uintptr_t handle)
{
    Fake *f = ctx;
    if (handle >= 1 && handle <= (uintptr_t)f->count)
        f->arms[handle - 1].cancelled = true;
}

static TimerManager *make(Fake *f)
{
    memset(f, 0, sizeof(*f));
    TimerScheduler s = { fake_arm, fake_cancel, f };
    return timer_manager_create(&s);
}

static void test_create_destroy(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    ASSERT_NOT_NULL(mgr);
    timer_manager_destroy(mgr);
}

static void test_create_requires_scheduler(void)
{
    ASSERT_NULL(timer_manager_create(NULL));
    TimerScheduler s = { NULL, NULL, NULL };
    ASSERT_NULL(timer_manager_create(&s));
}

static void test_add_arms_the_scheduler(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerId id = timer_add(mgr, 100, 7, NULL);
    ASSERT_NEQ(id, TIMER_INVALID);
    ASSERT_EQ(f.count, 1);
    ASSERT_EQ(f.arms[0].delay_ns, 100000000ULL);
    ASSERT_EQ(f.arms[0].period_ns, 100000000ULL);
    ASSERT_TRUE(timer_active(mgr, id));
    timer_manager_destroy(mgr);
}

static void test_add_ns_is_exact(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerId id = timer_add_ns(mgr, 16666667, 1, NULL);
    ASSERT_NEQ(id, TIMER_INVALID);
    ASSERT_EQ(f.arms[0].delay_ns, 16666667ULL);
    ASSERT_EQ(f.arms[0].period_ns, 16666667ULL);
    timer_manager_destroy(mgr);
}

static void test_zero_interval_fails(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    ASSERT_EQ(timer_add(mgr, 0, 1, NULL), TIMER_INVALID);
    ASSERT_EQ(timer_add_once_ns(mgr, 0, 1, NULL), TIMER_INVALID);
    ASSERT_EQ(f.count, 0);
    timer_manager_destroy(mgr);
}

static void test_dispatch_returns_event(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    int data = 42;
    TimerId id = timer_add(mgr, 100, 5, &data);
    ASSERT_NEQ(id, TIMER_INVALID);

    TimerEvent ev = { 0, NULL };
    ASSERT_TRUE(timer_dispatch(mgr, f.arms[0].slot, f.arms[0].epoch, &ev));
    ASSERT_EQ(ev.code, 5);
    ASSERT_TRUE(ev.data == &data);
    /* A repeating timer stays live and keeps dispatching. */
    ASSERT_TRUE(timer_active(mgr, id));
    ASSERT_TRUE(timer_dispatch(mgr, f.arms[0].slot, f.arms[0].epoch, &ev));
    timer_manager_destroy(mgr);
}

static void test_one_shot_frees_its_slot(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerId id = timer_add_once(mgr, 50, 9, NULL);
    ASSERT_NEQ(id, TIMER_INVALID);
    ASSERT_EQ(f.arms[0].period_ns, 0ULL); /* the scheduler runs it once */

    TimerEvent ev;
    ASSERT_TRUE(timer_dispatch(mgr, f.arms[0].slot, f.arms[0].epoch, &ev));
    ASSERT_EQ(ev.code, 9);
    ASSERT_FALSE(timer_active(mgr, id));
    /* A second event for the same arming is stale. */
    ASSERT_FALSE(timer_dispatch(mgr, f.arms[0].slot, f.arms[0].epoch, &ev));
    /* The real timer ended itself: nothing was cancelled. */
    ASSERT_FALSE(f.arms[0].cancelled);
    timer_manager_destroy(mgr);
}

static void test_remove_cancels_and_drops_queued_event(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerId id = timer_add(mgr, 100, 1, NULL);
    uint32_t slot = f.arms[0].slot, epoch = f.arms[0].epoch;

    timer_remove(mgr, id);
    ASSERT_TRUE(f.arms[0].cancelled);
    ASSERT_FALSE(timer_active(mgr, id));
    /* The event was posted before the remove and is still queued. */
    TimerEvent ev;
    ASSERT_FALSE(timer_dispatch(mgr, slot, epoch, &ev));
    /* Removing again, or an invalid id, is harmless. */
    timer_remove(mgr, id);
    timer_remove(mgr, TIMER_INVALID);
    timer_manager_destroy(mgr);
}

static void test_reset_rearms_with_a_new_epoch(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerId id = timer_add(mgr, 500, 3, NULL);
    uint32_t slot = f.arms[0].slot, epoch0 = f.arms[0].epoch;

    timer_reset(mgr, id);
    ASSERT_EQ(f.count, 2);
    ASSERT_TRUE(f.arms[0].cancelled);
    ASSERT_FALSE(f.arms[1].cancelled);
    ASSERT_EQ(f.arms[1].slot, slot);
    ASSERT_TRUE(f.arms[1].epoch != epoch0);
    ASSERT_EQ(f.arms[1].delay_ns, 500000000ULL);

    TimerEvent ev;
    ASSERT_FALSE(timer_dispatch(mgr, slot, epoch0, &ev)); /* old arming */
    ASSERT_TRUE(timer_dispatch(mgr, slot, f.arms[1].epoch, &ev));
    ASSERT_TRUE(timer_active(mgr, id)); /* the id survives a reset */
    timer_manager_destroy(mgr);
}

static void test_reset_of_unknown_id_is_ignored(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    timer_reset(mgr, TIMER_INVALID);
    timer_reset(mgr, 12345);
    ASSERT_EQ(f.count, 0);
    timer_manager_destroy(mgr);
}

static void test_stale_id_after_slot_reuse(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerId a = timer_add(mgr, 10, 1, NULL);
    timer_remove(mgr, a);
    TimerId b = timer_add(mgr, 10, 2, NULL);
    ASSERT_NEQ(b, TIMER_INVALID);
    ASSERT_TRUE(a != b); /* same slot, new generation */
    ASSERT_FALSE(timer_active(mgr, a));
    ASSERT_TRUE(timer_active(mgr, b));
    /* The stale id can neither remove nor reset the new owner. */
    timer_remove(mgr, a);
    ASSERT_TRUE(timer_active(mgr, b));
    int before = f.count;
    timer_reset(mgr, a);
    ASSERT_EQ(f.count, before);
    /* Events from the first arming are stale for the second owner. */
    TimerEvent ev;
    ASSERT_FALSE(timer_dispatch(mgr, f.arms[0].slot, f.arms[0].epoch, &ev));
    ASSERT_TRUE(timer_dispatch(mgr, f.arms[1].slot, f.arms[1].epoch, &ev));
    ASSERT_EQ(ev.code, 2);
    timer_manager_destroy(mgr);
}

static void test_slots_are_distinct(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerId a = timer_add(mgr, 10, 1, NULL);
    TimerId b = timer_add(mgr, 20, 2, NULL);
    TimerId c = timer_add_once(mgr, 30, 3, NULL);
    ASSERT_TRUE(a != b && b != c && a != c);
    ASSERT_TRUE(f.arms[0].slot != f.arms[1].slot);
    ASSERT_TRUE(f.arms[1].slot != f.arms[2].slot);
    timer_remove(mgr, b);
    ASSERT_TRUE(timer_active(mgr, a));
    ASSERT_TRUE(timer_active(mgr, c));
    timer_manager_destroy(mgr);
}

static void test_capacity_limit(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerId ids[TIMER_MAX];
    for (int i = 0; i < TIMER_MAX; i++) {
        ids[i] = timer_add(mgr, 10, (uint32_t)i, NULL);
        ASSERT_NEQ(ids[i], TIMER_INVALID);
    }
    ASSERT_EQ(timer_add(mgr, 10, 99, NULL), TIMER_INVALID);
    timer_remove(mgr, ids[5]);
    ASSERT_NEQ(timer_add(mgr, 10, 99, NULL), TIMER_INVALID);
    timer_manager_destroy(mgr);
}

static void test_scheduler_refusal_frees_the_slot(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    f.refuse = true;
    ASSERT_EQ(timer_add(mgr, 10, 1, NULL), TIMER_INVALID);
    f.refuse = false;
    for (int i = 0; i < TIMER_MAX; i++)
        ASSERT_NEQ(timer_add(mgr, 10, 1, NULL), TIMER_INVALID);
    timer_manager_destroy(mgr);
}

static void test_reset_refused_frees_the_timer(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerId id = timer_add(mgr, 10, 1, NULL);
    f.refuse = true;
    timer_reset(mgr, id);
    ASSERT_FALSE(timer_active(mgr, id));
    timer_manager_destroy(mgr);
}

static void test_destroy_cancels_live_timers(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    timer_add(mgr, 10, 1, NULL);
    timer_add_once(mgr, 10, 2, NULL);
    TimerId c = timer_add(mgr, 10, 3, NULL);
    timer_remove(mgr, c);
    timer_manager_destroy(mgr);
    ASSERT_TRUE(f.arms[0].cancelled);
    ASSERT_TRUE(f.arms[1].cancelled);
    ASSERT_TRUE(f.arms[2].cancelled); /* removed earlier */
}

static void test_dispatch_rejects_bad_slot(void)
{
    Fake f;
    TimerManager *mgr = make(&f);
    TimerEvent ev;
    ASSERT_FALSE(timer_dispatch(mgr, TIMER_MAX, 1, &ev));
    ASSERT_FALSE(timer_dispatch(mgr, 0, 1, &ev)); /* nothing armed */
    ASSERT_FALSE(timer_dispatch(NULL, 0, 1, &ev));
    timer_manager_destroy(mgr);
}

static void test_next_deadline_is_a_fixed_grid(void)
{
    const uint64_t P = 16666667;
    /* On time: exactly one period after the deadline that fired. */
    ASSERT_EQ(timer_next_deadline(1000, P, 1000), 1000 + P);
    /* Slightly late: still the grid point, not "now + period". */
    ASSERT_EQ(timer_next_deadline(1000, P, 1000 + 3000000), 1000 + P);
    /* Just before the next grid point is reached: the grid point. */
    ASSERT_EQ(timer_next_deadline(1000, P, 1000 + P - 1), 1000 + P);
    /* Exactly on the next grid point: skip to the one after. */
    ASSERT_EQ(timer_next_deadline(1000, P, 1000 + P), 1000 + 2 * P);
    /* Many periods behind: first grid point after now, no burst. */
    ASSERT_EQ(timer_next_deadline(1000, P, 1000 + 5 * P + 7), 1000 + 6 * P);
    /* The result is always in the future. */
    for (uint64_t now = 1000; now < 1000 + 7 * P; now += 1234567)
        ASSERT_TRUE(timer_next_deadline(1000, P, now) > now);
    /* No drift over many ticks on a fixed grid. */
    uint64_t d = 1000 + P;
    for (int i = 0; i < 10000; i++)
        d = timer_next_deadline(d, P, d + 1500000);
    ASSERT_EQ(d, 1000 + P + 10000 * P);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_create_destroy);
    RUN_TEST(test_create_requires_scheduler);
    RUN_TEST(test_add_arms_the_scheduler);
    RUN_TEST(test_add_ns_is_exact);
    RUN_TEST(test_zero_interval_fails);
    RUN_TEST(test_dispatch_returns_event);
    RUN_TEST(test_one_shot_frees_its_slot);
    RUN_TEST(test_remove_cancels_and_drops_queued_event);
    RUN_TEST(test_reset_rearms_with_a_new_epoch);
    RUN_TEST(test_reset_of_unknown_id_is_ignored);
    RUN_TEST(test_stale_id_after_slot_reuse);
    RUN_TEST(test_slots_are_distinct);
    RUN_TEST(test_capacity_limit);
    RUN_TEST(test_scheduler_refusal_frees_the_slot);
    RUN_TEST(test_reset_refused_frees_the_timer);
    RUN_TEST(test_destroy_cancels_live_timers);
    RUN_TEST(test_dispatch_rejects_bad_slot);
    RUN_TEST(test_next_deadline_is_a_fixed_grid);

    TEST_SUMMARY();
}
