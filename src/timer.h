/*
 * portty — Event-driven timer manager interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A timer is an OS/SDL timer that posts an event when its deadline passes.
 * Nothing here counts time down and nothing polls a clock to decide whether a
 * timer is due: the manager is a slot table owned by the main thread, and a
 * TimerScheduler supplied by the backend arms and cancels the real timers.
 *
 * Threading: every function in this header runs on the main (event loop)
 * thread. The scheduler's timers fire on whatever thread the backend uses and
 * only post an event carrying (slot, epoch); the main thread hands that pair
 * to timer_dispatch(), which drops the event when the timer has been removed,
 * reset or replaced since it was armed.
 */

typedef uint32_t TimerId;
#define TIMER_INVALID 0

/* Upper bound on concurrently live timers. */
#define TIMER_MAX 32

typedef struct TimerManager TimerManager;

/**
 * Event fired by a timer. The event_data pointer is owned by the timer
 * creator and must remain valid for the lifetime of the timer.
 */
typedef struct
{
    uint32_t code;
    void *data;
} TimerEvent;

/**
 * How the backend arms real timers.
 *
 * arm() starts a timer that fires after `delay_ns` and then every
 * `period_ns` (0 = once), and for each firing posts an event that the main
 * thread passes to timer_dispatch() as (slot, epoch). It returns an opaque
 * non-zero handle, or 0 on failure. cancel() stops a timer by handle; an
 * event it already posted is rejected later by the epoch check.
 */
typedef struct
{
    uintptr_t (*arm)(void *ctx, uint64_t delay_ns, uint64_t period_ns,
                     uint32_t slot, uint32_t epoch);
    void (*cancel)(void *ctx, uintptr_t handle);
    void *ctx;
} TimerScheduler;

/**
 * Create a timer manager that arms its timers through `sched`.
 * The scheduler is copied.
 *
 * @return TimerManager pointer on success, NULL on failure
 */
TimerManager *timer_manager_create(const TimerScheduler *sched);

/**
 * Destroy a timer manager, cancelling every timer still armed.
 */
void timer_manager_destroy(TimerManager *mgr);

/**
 * Add a repeating timer: fires every `interval_ns`, the first time one
 * interval from now. A zero interval fails.
 *
 * @return Timer ID on success, TIMER_INVALID on failure (no free slot, the
 *         scheduler refused, or a zero interval)
 */
TimerId timer_add_ns(TimerManager *mgr, uint64_t interval_ns,
                     uint32_t event_code, void *event_data);

/** Add a one-shot timer: fires once after `delay_ns`, then frees its slot. */
TimerId timer_add_once_ns(TimerManager *mgr, uint64_t delay_ns,
                          uint32_t event_code, void *event_data);

/** Millisecond conveniences for the _ns calls. */
TimerId timer_add(TimerManager *mgr, uint32_t interval_ms,
                  uint32_t event_code, void *event_data);
TimerId timer_add_once(TimerManager *mgr, uint32_t delay_ms,
                       uint32_t event_code, void *event_data);

/**
 * Remove a timer and cancel its real timer. An event it already posted is
 * dropped by timer_dispatch(). Unknown and stale ids are ignored.
 */
void timer_remove(TimerManager *mgr, TimerId id);

/**
 * Restart a timer's interval (or one-shot delay) from now. The pending event
 * of the previous arming, if any, is dropped.
 */
void timer_reset(TimerManager *mgr, TimerId id);

/**
 * Whether `id` names a live timer. A one-shot is live until its event has
 * been dispatched.
 */
bool timer_active(const TimerManager *mgr, TimerId id);

/**
 * Handle an event posted by the scheduler's timer for (slot, epoch).
 *
 * @return true and fill `out` when the timer is still live and the epoch is
 *         current; false for a stale event, which the caller drops. A
 *         one-shot's slot is freed.
 */
bool timer_dispatch(TimerManager *mgr, uint32_t slot, uint32_t epoch,
                    TimerEvent *out);

/**
 * Next deadline of a repeating timer given the one that just fired.
 *
 * `prev_deadline` is the deadline that fired, `now` the current time, both on
 * the same clock. The result is `prev_deadline + period`, a fixed grid so a
 * late wake never pushes later ticks back. When more than a period behind it
 * skips to the first grid point after `now` instead of firing a burst. The
 * result is always greater than `now`.
 */
uint64_t timer_next_deadline(uint64_t prev_deadline, uint64_t period,
                             uint64_t now);

#endif /* TIMER_H */
