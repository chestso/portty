#include "timer.h"
#include "common.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

typedef struct Timer
{
    TimerId id;
    uint32_t interval_ms;
    bool repeat;
    uint32_t event_code;
    void *event_data;
    SDL_TimerID sdl_timer_id;
} Timer;

struct TimerManager
{
    Timer **timers; // array of pointers (stable across realloc)
    size_t count;
    size_t capacity;
    TimerId next_id;
};

// SDL timer callback - runs in timer thread, just pushes event
static Uint32 timer_sdl_callback(void *param, SDL_TimerID sdl_id, Uint32 interval)
{
    (void)sdl_id;
    Timer *timer = (Timer *)param;

    SDL_Event event = { 0 };
    event.type = SDL_EVENT_USER;
    event.user.code = (Sint32)timer->event_code;
    event.user.data1 = timer->event_data;
    SDL_PushEvent(&event);

    return timer->repeat ? interval : 0;
}

TimerManager *timer_manager_create(void)
{
    TimerManager *mgr = calloc(1, sizeof(TimerManager));
    if (!mgr)
        return NULL;

    mgr->timers = NULL;
    mgr->count = 0;
    mgr->capacity = 0;
    mgr->next_id = 1; // Start at 1 since 0 is TIMER_INVALID

    vlog("Timer manager created\n");
    return mgr;
}

void timer_manager_destroy(TimerManager *mgr)
{
    if (!mgr)
        return;

    // Remove all active timers
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->timers[i]->sdl_timer_id != 0) {
            SDL_RemoveTimer(mgr->timers[i]->sdl_timer_id);
        }
        free(mgr->timers[i]);
    }

    free(mgr->timers);
    free(mgr);

    vlog("Timer manager destroyed\n");
}

static Timer *find_timer(TimerManager *mgr, TimerId id)
{
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->timers[i]->id == id) {
            return mgr->timers[i];
        }
    }
    return NULL;
}

TimerId timer_add(TimerManager *mgr, uint32_t interval_ms, bool repeat,
                  uint32_t event_code, void *event_data)
{
    if (!mgr)
        return TIMER_INVALID;

    // Grow pointer array if needed
    if (mgr->count >= mgr->capacity) {
        size_t new_capacity = mgr->capacity == 0 ? 4 : mgr->capacity * 2;
        Timer **new_timers = realloc(mgr->timers, new_capacity * sizeof(Timer *));
        if (!new_timers)
            return TIMER_INVALID;
        mgr->timers = new_timers;
        mgr->capacity = new_capacity;
    }

    // Allocate timer on the heap so its address is stable
    Timer *timer = malloc(sizeof(Timer));
    if (!timer)
        return TIMER_INVALID;

    timer->id = mgr->next_id++;
    timer->interval_ms = interval_ms;
    timer->repeat = repeat;
    timer->event_code = event_code;
    timer->event_data = event_data;

    // Start SDL timer
    timer->sdl_timer_id = SDL_AddTimer(interval_ms, timer_sdl_callback, timer);
    if (timer->sdl_timer_id == 0) {
        vlog("Failed to create SDL timer: %s\n", SDL_GetError());
        free(timer);
        return TIMER_INVALID;
    }

    mgr->timers[mgr->count] = timer;
    mgr->count++;

    vlog("Timer added: id=%u interval=%ums repeat=%s event_code=%u\n",
         timer->id, interval_ms, repeat ? "yes" : "no", event_code);

    return timer->id;
}

void timer_remove(TimerManager *mgr, TimerId id)
{
    if (!mgr || id == TIMER_INVALID)
        return;

    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->timers[i]->id == id) {
            // Stop SDL timer
            if (mgr->timers[i]->sdl_timer_id != 0) {
                SDL_RemoveTimer(mgr->timers[i]->sdl_timer_id);
            }

            vlog("Timer removed: id=%u\n", id);

            free(mgr->timers[i]);

            // Remove from array by swapping with last element
            if (i < mgr->count - 1) {
                mgr->timers[i] = mgr->timers[mgr->count - 1];
            }
            mgr->count--;
            return;
        }
    }
}

void timer_reset(TimerManager *mgr, TimerId id)
{
    if (!mgr || id == TIMER_INVALID)
        return;

    Timer *timer = find_timer(mgr, id);
    if (!timer)
        return;

    // Stop old SDL timer
    if (timer->sdl_timer_id != 0) {
        SDL_RemoveTimer(timer->sdl_timer_id);
    }

    // Start new SDL timer
    timer->sdl_timer_id = SDL_AddTimer(timer->interval_ms, timer_sdl_callback, timer);
    if (timer->sdl_timer_id == 0) {
        vlog("Failed to reset timer %u: %s\n", id, SDL_GetError());
        return;
    }

    vlog("Timer reset: id=%u interval=%ums\n", id, timer->interval_ms);
}
