/*
 * portty — Poll-based event timer manager
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "timer.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>

typedef struct Timer
{
    TimerId id;
    uint32_t interval_ms;
    uint32_t remaining_ms;
    uint32_t event_code;
    void *event_data;
    bool active;
    bool one_shot;
} Timer;

struct TimerManager
{
    Timer *timers; // dense array, stable slots while active
    size_t count;
    size_t capacity;
    TimerId next_id;
};

TimerManager *timer_manager_create(void)
{
    TimerManager *mgr = calloc(1, sizeof(TimerManager));
    if (!mgr)
        return NULL;

    mgr->next_id = 1; // Start at 1 since 0 is TIMER_INVALID

    vlog("Timer manager created\n");
    return mgr;
}

void timer_manager_destroy(TimerManager *mgr)
{
    if (!mgr)
        return;

    free(mgr->timers);
    free(mgr);

    vlog("Timer manager destroyed\n");
}

static Timer *find_timer(TimerManager *mgr, TimerId id)
{
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->timers[i].id == id && mgr->timers[i].active)
            return &mgr->timers[i];
    }
    return NULL;
}

static TimerId timer_add_internal(TimerManager *mgr, uint32_t interval_ms,
                                  uint32_t event_code, void *event_data,
                                  bool one_shot)
{
    if (!mgr || interval_ms == 0)
        return TIMER_INVALID;

    // Reuse an inactive slot if possible, otherwise grow.
    Timer *timer = NULL;
    for (size_t i = 0; i < mgr->count; i++) {
        if (!mgr->timers[i].active) {
            timer = &mgr->timers[i];
            break;
        }
    }

    if (!timer) {
        if (mgr->count >= mgr->capacity) {
            size_t new_capacity = mgr->capacity == 0 ? 4 : mgr->capacity * 2;
            Timer *new_timers = realloc(mgr->timers, new_capacity * sizeof(Timer));
            if (!new_timers)
                return TIMER_INVALID;
            mgr->timers = new_timers;
            mgr->capacity = new_capacity;
        }
        timer = &mgr->timers[mgr->count++];
    }

    timer->id = mgr->next_id++;
    timer->interval_ms = interval_ms;
    timer->remaining_ms = interval_ms;
    timer->event_code = event_code;
    timer->event_data = event_data;
    timer->active = true;
    timer->one_shot = one_shot;

    vlog("Timer added: id=%u interval=%ums event_code=%u one_shot=%d\n",
         timer->id, interval_ms, event_code, one_shot);

    return timer->id;
}

TimerId timer_add(TimerManager *mgr, uint32_t interval_ms,
                  uint32_t event_code, void *event_data)
{
    return timer_add_internal(mgr, interval_ms, event_code, event_data, false);
}

TimerId timer_add_once(TimerManager *mgr, uint32_t delay_ms,
                       uint32_t event_code, void *event_data)
{
    return timer_add_internal(mgr, delay_ms, event_code, event_data, true);
}

void timer_remove(TimerManager *mgr, TimerId id)
{
    if (!mgr || id == TIMER_INVALID)
        return;

    Timer *timer = find_timer(mgr, id);
    if (!timer)
        return;

    vlog("Timer removed: id=%u\n", id);
    timer->active = false;
}

void timer_reset(TimerManager *mgr, TimerId id)
{
    if (!mgr || id == TIMER_INVALID)
        return;

    Timer *timer = find_timer(mgr, id);
    if (!timer)
        return;

    timer->remaining_ms = timer->interval_ms;

    vlog("Timer reset: id=%u interval=%ums\n", id, timer->interval_ms);
}

size_t timer_poll(TimerManager *mgr, uint32_t elapsed_ms,
                  TimerEvent *events, size_t max_events)
{
    if (!mgr || !events || max_events == 0)
        return 0;

    size_t emitted = 0;

    for (size_t i = 0; i < mgr->count; i++) {
        Timer *timer = &mgr->timers[i];
        if (!timer->active)
            continue;

        uint32_t remaining = elapsed_ms;
        while (timer->remaining_ms <= remaining) {
            remaining -= timer->remaining_ms;
            timer->remaining_ms = timer->interval_ms;

            if (emitted < max_events) {
                events[emitted].code = timer->event_code;
                events[emitted].data = timer->event_data;
                emitted++;
            }
            // Timers that overflow the output array are still consumed:
            // we advance their state so they do not fire again on the
            // next poll. The caller can detect overflow by comparing the
            // return value to max_events.

            if (timer->one_shot) {
                timer->active = false;
                break;
            }
        }
        timer->remaining_ms -= remaining;
    }

    return emitted;
}

uint32_t timer_manager_next_delay_ms(TimerManager *mgr)
{
    if (!mgr)
        return UINT32_MAX;

    uint32_t earliest = UINT32_MAX;
    for (size_t i = 0; i < mgr->count; i++) {
        Timer *timer = &mgr->timers[i];
        if (!timer->active)
            continue;
        if (timer->remaining_ms < earliest)
            earliest = timer->remaining_ms;
    }
    return earliest;
}
