/*
 * portty — Event-driven timer manager
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

#define NS_PER_MS 1000000ULL

typedef struct Timer
{
    bool active;
    bool one_shot;
    uint32_t gen;   /* bumped each time the slot is allocated: invalidates old TimerIds */
    uint32_t epoch; /* bumped each time the real timer is (re)armed: invalidates old events */
    uint64_t period_ns;
    uint64_t first_ns;
    uint32_t event_code;
    void *event_data;
    uintptr_t handle;
} Timer;

struct TimerManager
{
    TimerScheduler sched;
    Timer slots[TIMER_MAX];
};

/* TimerId = gen << 8 | (slot + 1). TIMER_MAX < 255 keeps the slot in 8 bits
 * and the low byte non-zero, so no live id equals TIMER_INVALID. */
static TimerId make_id(uint32_t slot, uint32_t gen)
{
    return (gen << 8) | (slot + 1);
}

static Timer *lookup(TimerManager *mgr, TimerId id)
{
    if (!mgr || id == TIMER_INVALID)
        return NULL;
    uint32_t low = id & 0xFFu;
    if (low == 0 || low > TIMER_MAX)
        return NULL;
    Timer *t = &mgr->slots[low - 1];
    if (!t->active || t->gen != (id >> 8))
        return NULL;
    return t;
}

TimerManager *timer_manager_create(const TimerScheduler *sched)
{
    if (!sched || !sched->arm || !sched->cancel)
        return NULL;
    TimerManager *mgr = calloc(1, sizeof(TimerManager));
    if (!mgr)
        return NULL;
    mgr->sched = *sched;
    vlog("Timer manager created\n");
    return mgr;
}

static void cancel_handle(TimerManager *mgr, Timer *t)
{
    if (t->handle) {
        mgr->sched.cancel(mgr->sched.ctx, t->handle);
        t->handle = 0;
    }
}

void timer_manager_destroy(TimerManager *mgr)
{
    if (!mgr)
        return;
    for (int i = 0; i < TIMER_MAX; i++) {
        if (mgr->slots[i].active)
            cancel_handle(mgr, &mgr->slots[i]);
    }
    free(mgr);
    vlog("Timer manager destroyed\n");
}

/* Arm the real timer for a slot with a fresh epoch. */
static bool arm_slot(TimerManager *mgr, uint32_t slot)
{
    Timer *t = &mgr->slots[slot];
    t->epoch++;
    t->handle = mgr->sched.arm(mgr->sched.ctx, t->first_ns,
                               t->one_shot ? 0 : t->period_ns, slot, t->epoch);
    return t->handle != 0;
}

static TimerId add_internal(TimerManager *mgr, uint64_t interval_ns,
                            uint32_t event_code, void *event_data,
                            bool one_shot)
{
    if (!mgr || interval_ns == 0)
        return TIMER_INVALID;

    uint32_t slot = TIMER_MAX;
    for (uint32_t i = 0; i < TIMER_MAX; i++) {
        if (!mgr->slots[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == TIMER_MAX)
        return TIMER_INVALID;

    Timer *t = &mgr->slots[slot];
    t->active = true;
    t->one_shot = one_shot;
    t->gen++;
    t->period_ns = interval_ns;
    t->first_ns = interval_ns;
    t->event_code = event_code;
    t->event_data = event_data;
    t->handle = 0;

    if (!arm_slot(mgr, slot)) {
        t->active = false;
        return TIMER_INVALID;
    }

    vlog("Timer added: slot=%u interval=%lluns event_code=%u one_shot=%d\n",
         slot, (unsigned long long)interval_ns, event_code, one_shot);
    return make_id(slot, t->gen);
}

TimerId timer_add_ns(TimerManager *mgr, uint64_t interval_ns,
                     uint32_t event_code, void *event_data)
{
    return add_internal(mgr, interval_ns, event_code, event_data, false);
}

TimerId timer_add_once_ns(TimerManager *mgr, uint64_t delay_ns,
                          uint32_t event_code, void *event_data)
{
    return add_internal(mgr, delay_ns, event_code, event_data, true);
}

TimerId timer_add(TimerManager *mgr, uint32_t interval_ms,
                  uint32_t event_code, void *event_data)
{
    return add_internal(mgr, (uint64_t)interval_ms * NS_PER_MS, event_code,
                        event_data, false);
}

TimerId timer_add_once(TimerManager *mgr, uint32_t delay_ms,
                       uint32_t event_code, void *event_data)
{
    return add_internal(mgr, (uint64_t)delay_ms * NS_PER_MS, event_code,
                        event_data, true);
}

void timer_remove(TimerManager *mgr, TimerId id)
{
    Timer *t = lookup(mgr, id);
    if (!t)
        return;
    vlog("Timer removed: slot=%u\n", (unsigned)(id & 0xFFu) - 1);
    cancel_handle(mgr, t);
    t->epoch++; /* an event already posted is now stale */
    t->active = false;
}

void timer_reset(TimerManager *mgr, TimerId id)
{
    Timer *t = lookup(mgr, id);
    if (!t)
        return;
    uint32_t slot = (id & 0xFFu) - 1;
    cancel_handle(mgr, t);
    if (!arm_slot(mgr, slot)) {
        /* The scheduler refused: the timer cannot run, so free it. */
        t->active = false;
        return;
    }
    vlog("Timer reset: slot=%u\n", slot);
}

bool timer_active(const TimerManager *mgr, TimerId id)
{
    return lookup((TimerManager *)mgr, id) != NULL;
}

bool timer_dispatch(TimerManager *mgr, uint32_t slot, uint32_t epoch,
                    TimerEvent *out)
{
    if (!mgr || slot >= TIMER_MAX)
        return false;
    Timer *t = &mgr->slots[slot];
    if (!t->active || t->epoch != epoch)
        return false;

    if (out) {
        out->code = t->event_code;
        out->data = t->event_data;
    }
    if (t->one_shot) {
        /* The scheduler's timer ended itself; there is nothing to cancel. */
        t->handle = 0;
        t->epoch++;
        t->active = false;
    }
    return true;
}

uint64_t timer_next_deadline(uint64_t prev_deadline, uint64_t period,
                             uint64_t now)
{
    if (period == 0)
        return now + 1;
    uint64_t next = prev_deadline + period;
    if (next > now)
        return next;
    /* More than one period behind: first grid point strictly after `now`. */
    uint64_t behind = now - prev_deadline;
    return prev_deadline + (behind / period + 1) * period;
}
