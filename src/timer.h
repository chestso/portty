#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t TimerId;
#define TIMER_INVALID 0

typedef struct TimerManager TimerManager;

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
 * Add a new timer.
 *
 * When the timer fires, it pushes an SDL_EVENT_USER event with the specified
 * event_code in the user.code field and event_data in user.data1.
 *
 * @param mgr Timer manager
 * @param interval_ms Timer interval in milliseconds
 * @param repeat If true, timer repeats; if false, fires once
 * @param event_code Event code to use in SDL_Event.user.code
 * @param event_data Optional data to pass in SDL_Event.user.data1
 * @return Timer ID on success, TIMER_INVALID on failure
 */
TimerId timer_add(TimerManager *mgr, uint32_t interval_ms, bool repeat,
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

#endif /* TIMER_H */
