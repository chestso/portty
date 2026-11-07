/*
 * portty — Frame pacer: the display refresh as the animation clock
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef FRAME_PACER_H
#define FRAME_PACER_H

#include <stdbool.h>
#include <stdint.h>

/*
 * While an animation plays, the loop presents once per display refresh and
 * the present blocks until the next one. FramePacer turns that into media
 * time: pacer_begin() names the refresh the next present will occupy, which is
 * the time to sample the animation for, and pacer_end() learns from how long
 * the present blocked.
 *
 * Every time is a nanosecond count on one monotonic clock, passed in by the
 * caller; the module reads no clock and links no SDL.
 *
 * The sample time is a counter, not a measurement: it advances by exactly one
 * period per present. Re-deriving "the next refresh" from the current time
 * flips between two adjacent refreshes whenever the present's return jitters
 * across a grid point, which shows up as dropped and doubled frames.
 */

typedef enum
{
    PACER_PROBING, /* too few presents seen to know whether they block */
    PACER_LOCKED,  /* presents block on the display: refresh-locked */
    PACER_FREE     /* presents do not block: a synthetic grid, not display-locked */
} PacerMode;

typedef struct
{
    uint64_t nominal_ns; /* the display's reported refresh period */
    uint64_t period_ns;  /* the estimate in use (nominal until refined) */
    uint64_t v_ns;       /* sample refresh of the last pacer_begin (0 = none yet) */
    uint64_t cost_ns;    /* smoothed time a frame needs before its refresh */
    PacerMode mode;
    unsigned blocked_run; /* consecutive presents that blocked */
    unsigned fast_run;    /* consecutive presents that did not */
    unsigned misses;      /* refreshes skipped because a frame could not make them */
    unsigned hiccups;     /* blocked presents that returned off the period grid */
    uint64_t ref_t_ns;    /* return time anchoring the current period estimate */
    unsigned ref_n;       /* refresh intervals since ref_t_ns */
    uint64_t prev_return_ns;
} FramePacer;

/* Start from the display's nominal refresh period. */
void pacer_init(FramePacer *p, uint64_t nominal_period_ns);

/* Forget everything learned about the display but keep the nominal period:
 * the next pacer_begin() starts a new run. Call when pacing stops. */
void pacer_reset(FramePacer *p);

/* The refresh the next present will occupy, in the caller's clock. `now_ns`
 * is when the frame starts being prepared. */
uint64_t pacer_begin(FramePacer *p, uint64_t now_ns);

/* Report how long preparing the frame took (everything before the present),
 * so it can be budgeted into the next sample refresh. */
void pacer_cost(FramePacer *p, uint64_t work_ns);

/* Report one present: the clock just before and just after SDL_RenderPresent. */
void pacer_end(FramePacer *p, uint64_t t_before_ns, uint64_t t_after_ns);

PacerMode pacer_mode(const FramePacer *p);
uint64_t pacer_period(const FramePacer *p);

/* How long the frame handed to pacer_begin()'s refresh stays on screen, for
 * the animation sampler: one period. It is the period of the synthetic grid
 * in PACER_FREE too, which keeps the sampler off the frame boundaries that a
 * grid commensurate with the frame rate would otherwise land on (30 fps on a
 * 60 Hz grid: every other sample, where rounding noise picks the frame). */
uint64_t pacer_hold_ns(const FramePacer *p);

#endif /* FRAME_PACER_H */
