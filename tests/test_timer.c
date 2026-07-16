#include "test_helpers.h"
#include "../src/timer.h"
#include <string.h>

static void test_create_destroy(void)
{
    TimerManager *mgr = timer_manager_create();
    ASSERT_NOT_NULL(mgr);
    timer_manager_destroy(mgr);
}

static void test_add_fires_after_interval(void)
{
    TimerManager *mgr = timer_manager_create();
    ASSERT_NOT_NULL(mgr);

    int data = 42;
    TimerId id = timer_add(mgr, 100, 1, &data);
    ASSERT_NEQ(id, TIMER_INVALID);

    TimerEvent events[4];
    size_t n;

    n = timer_poll(mgr, 50, events, 4);
    ASSERT_EQ(n, 0);

    n = timer_poll(mgr, 50, events, 4);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(events[0].code, 1);
    ASSERT_EQ(events[0].data, &data);

    timer_manager_destroy(mgr);
}

static void test_repeating_timer_fires_multiple_times(void)
{
    TimerManager *mgr = timer_manager_create();
    ASSERT_NOT_NULL(mgr);

    TimerId id = timer_add(mgr, 10, 2, NULL);
    ASSERT_NEQ(id, TIMER_INVALID);

    TimerEvent events[8];
    size_t n = timer_poll(mgr, 35, events, 8);
    ASSERT_EQ(n, 3);
    for (size_t i = 0; i < n; i++)
        ASSERT_EQ(events[i].code, 2);

    timer_manager_destroy(mgr);
}

static void test_remove_stops_firing(void)
{
    TimerManager *mgr = timer_manager_create();
    ASSERT_NOT_NULL(mgr);

    TimerId id = timer_add(mgr, 10, 3, NULL);
    timer_remove(mgr, id);

    TimerEvent events[4];
    size_t n = timer_poll(mgr, 100, events, 4);
    ASSERT_EQ(n, 0);

    timer_manager_destroy(mgr);
}

static void test_reset_restarts_interval(void)
{
    TimerManager *mgr = timer_manager_create();
    ASSERT_NOT_NULL(mgr);

    TimerId id = timer_add(mgr, 100, 4, NULL);

    TimerEvent events[4];
    size_t n = timer_poll(mgr, 90, events, 4);
    ASSERT_EQ(n, 0);

    timer_reset(mgr, id);

    n = timer_poll(mgr, 50, events, 4);
    ASSERT_EQ(n, 0);

    n = timer_poll(mgr, 60, events, 4);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(events[0].code, 4);

    timer_manager_destroy(mgr);
}

static void test_multiple_timers_sorted_by_slot(void)
{
    TimerManager *mgr = timer_manager_create();
    ASSERT_NOT_NULL(mgr);

    TimerId a = timer_add(mgr, 50, 10, NULL);
    TimerId b = timer_add(mgr, 50, 11, NULL);
    (void)a;
    (void)b;

    TimerEvent events[4];
    size_t n = timer_poll(mgr, 50, events, 4);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(events[0].code, 10);
    ASSERT_EQ(events[1].code, 11);

    timer_manager_destroy(mgr);
}

static void test_overflow_event_count(void)
{
    TimerManager *mgr = timer_manager_create();
    ASSERT_NOT_NULL(mgr);

    timer_add(mgr, 10, 20, NULL);
    timer_add(mgr, 10, 21, NULL);

    TimerEvent events[1];
    size_t n = timer_poll(mgr, 30, events, 1);
    ASSERT_EQ(n, 1);
    // The second timer should have fired too but was dropped from the
    // output array; it should still be consumed (not leak into next poll).
    n = timer_poll(mgr, 0, events, 1);
    ASSERT_EQ(n, 0);

    timer_manager_destroy(mgr);
}

static void test_zero_elapsed_no_fire(void)
{
    TimerManager *mgr = timer_manager_create();
    ASSERT_NOT_NULL(mgr);

    timer_add(mgr, 0, 99, NULL);
    TimerEvent events[4];
    size_t n = timer_poll(mgr, 0, events, 4);
    ASSERT_EQ(n, 0);

    timer_manager_destroy(mgr);
}

static void test_reuse_slot_after_remove(void)
{
    TimerManager *mgr = timer_manager_create();
    ASSERT_NOT_NULL(mgr);

    TimerId id = timer_add(mgr, 10, 30, NULL);
    timer_remove(mgr, id);

    TimerId id2 = timer_add(mgr, 10, 31, NULL);
    ASSERT_NEQ(id2, TIMER_INVALID);

    TimerEvent events[4];
    size_t n = timer_poll(mgr, 15, events, 4);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(events[0].code, 31);

    timer_manager_destroy(mgr);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_create_destroy);
    RUN_TEST(test_add_fires_after_interval);
    RUN_TEST(test_repeating_timer_fires_multiple_times);
    RUN_TEST(test_remove_stops_firing);
    RUN_TEST(test_reset_restarts_interval);
    RUN_TEST(test_multiple_timers_sorted_by_slot);
    RUN_TEST(test_overflow_event_count);
    RUN_TEST(test_zero_elapsed_no_fire);
    RUN_TEST(test_reuse_slot_after_remove);

    TEST_SUMMARY();
}
