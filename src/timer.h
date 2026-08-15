/*
 * portty — Poll-based event timer manager interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef uint32_t TimerId;
#define TIMER_INVALID 0

typedef struct TimerManager TimerManager;

/**
 * Event fired by a timer. Backends poll these out of the manager and
 * dispatch them however they like (SDL_EVENT_USER, direct handler call,
 * etc.). The event_data pointer is owned by the timer creator and must
 * remain valid for the lifetime of the timer.
 */
typedef struct
{
    uint32_t code;
    void *data;
} TimerEvent;

/**
 * Create a new timer manager.
 *
 * @return TimerManager pointer on success, NULL on failure
 */
TimerManager *timer_manager_create(void);

/**
 * Destroy a timer manager and all its timers.
 *
 * @param mgr Timer manager to destroy
 */
void timer_manager_destroy(TimerManager *mgr);

/**
 * Add a new repeating timer.
 *
 * The timer fires every `interval_ms` milliseconds. Each time it fires,
 * `timer_poll()` reports an event with the given `event_code` and the
 * optional `event_data` pointer.
 *
 * @param mgr Timer manager
 * @param interval_ms Timer interval in milliseconds
 * @param event_code Event code returned by timer_poll()
 * @param event_data Optional data pointer returned by timer_poll()
 * @return Timer ID on success, TIMER_INVALID on failure
 */
TimerId timer_add(TimerManager *mgr, uint32_t interval_ms,
                  uint32_t event_code, void *event_data);

/**
 * Add a one-shot timer.
 *
 * Fires exactly once after `delay_ms`, then removes itself. Identical to
 * timer_add() except the timer is not repeating.
 *
 * @param mgr Timer manager
 * @param delay_ms Delay in milliseconds before firing
 * @param event_code Event code returned by timer_poll()
 * @param event_data Optional data pointer returned by timer_poll()
 * @return Timer ID on success, TIMER_INVALID on failure
 */
TimerId timer_add_once(TimerManager *mgr, uint32_t delay_ms,
                       uint32_t event_code, void *event_data);

/**
 * Remove a timer.
 *
 * @param mgr Timer manager
 * @param id Timer ID to remove
 */
void timer_remove(TimerManager *mgr, TimerId id);

/**
 * Reset a timer, restarting its interval from now.
 *
 * @param mgr Timer manager
 * @param id Timer ID to reset
 */
void timer_reset(TimerManager *mgr, TimerId id);

/**
 * Milliseconds until the next active timer fires.
 *
 * Returns UINT32_MAX when no timers are active. Backends use this to arm
 * a blocking wait (SDL_AddTimer, poll timeout, etc.) so the event loop
 * can sleep instead of busy-polling. Does not modify timer state.
 *
 * @param mgr Timer manager
 * @return Delay to the earliest deadline in milliseconds, or UINT32_MAX
 */
uint32_t timer_manager_next_delay_ms(TimerManager *mgr);

/**
 * Advance time by `elapsed_ms` and fill `events` with timers that fired.
 *
 * Call this once per frame/event-loop iteration. `events` must be large
 * enough to hold all firing timers; in practice a small fixed-size array
 * (e.g. 8-16 entries) is sufficient. Events are emitted in the order the
 * timers fire; timers with the same deadline are ordered by stable timer
 * slot order.
 *
 * @param mgr Timer manager
 * @param elapsed_ms Milliseconds since the last call
 * @param events Output array of fired timer events
 * @param max_events Maximum number of events to write
 * @return Number of events written (may be more than max_events if
 *         timers overflowed; compare return to max to detect overflow)
 */
size_t timer_poll(TimerManager *mgr, uint32_t elapsed_ms,
                  TimerEvent *events, size_t max_events);

#endif /* TIMER_H */
