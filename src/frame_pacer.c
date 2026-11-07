/*
 * portty — Frame pacer
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "frame_pacer.h"
#include <string.h>

/* A present that returns after at least this share of a period was blocked on
 * the display. */
#define BLOCKED_NUM 1
#define BLOCKED_DEN 4

/* Presents that must block in a row before the pacer calls itself locked, and
 * presents that must not block in a row before it gives up on the display.
 * The first presents after pacing starts return at once while the swap queue
 * fills, which is why giving up takes more than a few. */
#define LOCK_AFTER_BLOCKED 4
#define FREE_AFTER_FAST    8

/* Consecutive one-period intervals before the period estimate is trusted, and
 * how far from nominal it may move. */
#define REFINE_AFTER     30
#define REFINE_LIMIT_DEN 200 /* +-0.5% */

/* A frame may be this many periods behind its refresh before the pacer skips
 * ahead. The swap queue is two deep, so a frame prepared one period late still
 * reaches the screen in order; skipping on every small overrun costs more
 * cadence than it saves. */
#define SKIP_SLACK_PERIODS 1

void pacer_init(FramePacer *p, uint64_t nominal_period_ns)
{
    memset(p, 0, sizeof(*p));
    p->nominal_ns = nominal_period_ns ? nominal_period_ns : 16666667ULL;
    p->period_ns = p->nominal_ns;
    p->mode = PACER_PROBING;
}

void pacer_reset(FramePacer *p)
{
    pacer_init(p, p->nominal_ns);
}

uint64_t pacer_begin(FramePacer *p, uint64_t now_ns)
{
    if (p->v_ns == 0) {
        p->v_ns = now_ns + p->period_ns;
        return p->v_ns;
    }
    p->v_ns += p->period_ns;
    while (p->v_ns + SKIP_SLACK_PERIODS * p->period_ns < now_ns + p->cost_ns) {
        p->v_ns += p->period_ns;
        p->misses++;
    }
    return p->v_ns;
}

void pacer_cost(FramePacer *p, uint64_t work_ns)
{
    /* Exponential moving average, 1/8 weight. */
    p->cost_ns = p->cost_ns - p->cost_ns / 8 + work_ns / 8;
}

static void drop_period_run(FramePacer *p)
{
    p->ref_t_ns = 0;
    p->ref_n = 0;
}

static uint64_t round_div(uint64_t a, uint64_t b)
{
    return (a + b / 2) / b;
}

void pacer_end(FramePacer *p, uint64_t t_before_ns, uint64_t t_after_ns)
{
    uint64_t blocked_for = t_after_ns > t_before_ns ? t_after_ns - t_before_ns : 0;
    bool blocked = blocked_for * BLOCKED_DEN >= p->period_ns * BLOCKED_NUM;

    if (!blocked) {
        p->fast_run++;
        p->blocked_run = 0;
        p->prev_return_ns = 0;
        drop_period_run(p);
        if (p->mode != PACER_FREE && p->fast_run >= FREE_AFTER_FAST)
            p->mode = PACER_FREE;
        return;
    }

    p->blocked_run++;
    p->fast_run = 0;
    if (p->mode == PACER_PROBING && p->blocked_run >= LOCK_AFTER_BLOCKED)
        p->mode = PACER_LOCKED;

    /* Refine the period from a run of returns that are exactly one period
     * apart. A long baseline is stable to far better than any single
     * interval, whose jitter is a few percent of a period. */
    if (p->prev_return_ns) {
        uint64_t n = round_div(t_after_ns - p->prev_return_ns, p->period_ns);
        if (n == 1) {
            if (p->ref_t_ns == 0) {
                p->ref_t_ns = p->prev_return_ns;
                p->ref_n = 0;
            }
            p->ref_n++;
            if (p->ref_n >= REFINE_AFTER) {
                uint64_t est = (t_after_ns - p->ref_t_ns) / p->ref_n;
                uint64_t lim = p->nominal_ns / REFINE_LIMIT_DEN;
                if (est + lim >= p->nominal_ns && est <= p->nominal_ns + lim)
                    p->period_ns = est;
            }
        } else {
            drop_period_run(p);
            p->hiccups++;
        }
    }
    p->prev_return_ns = t_after_ns;
}

PacerMode pacer_mode(const FramePacer *p)
{
    return p->mode;
}

uint64_t pacer_period(const FramePacer *p)
{
    return p->period_ns;
}

uint64_t pacer_hold_ns(const FramePacer *p)
{
    return p->period_ns;
}
