/*
 * portty — Frame pacer tests
 *
 * A simulated display stands in for the compositor: a swap queue two deep
 * that retires one frame per refresh, a present that blocks only while the
 * queue is full, return jitter, and occasional late returns. The animation
 * sampler is the closed form the engine uses (frame = floor of media time at
 * the middle of the frame's refresh).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "test_helpers.h"
#include "../src/frame_pacer.h"
#include <stdlib.h>
#include <string.h>

#define MS 1000000ULL

/* ── Simulated display ── */

typedef struct
{
    uint64_t period_ns;
    uint64_t next_vblank;
    int queue; /* frames waiting to be shown */
    uint32_t rng;
    int jitter_ns;   /* +- on a blocked return */
    int stall_every; /* 0 = never; else about 1 in N blocked returns is a period late */
    bool blocks;     /* false: presents never block (no vsync) */
} Display;

static uint32_t next_rand(Display *d)
{
    d->rng = d->rng * 1664525u + 1013904223u;
    return d->rng >> 8;
}

static void display_init(Display *d, uint64_t period_ns)
{
    memset(d, 0, sizeof(*d));
    d->period_ns = period_ns;
    d->next_vblank = 5 * MS;
    d->rng = 99;
    d->blocks = true;
}

/* Retire the frames whose refresh has passed. */
static void display_advance(Display *d, uint64_t now)
{
    while (d->next_vblank <= now) {
        if (d->queue > 0)
            d->queue--;
        d->next_vblank += d->period_ns;
    }
}

/* One SDL_RenderPresent called at `now`; returns when it returns. */
static uint64_t display_present(Display *d, uint64_t now)
{
    if (!d->blocks)
        return now + 200000;
    display_advance(d, now);
    if (d->queue < 2) {
        d->queue++;
        return now + 200000;
    }
    /* The queue is full: wait for the next refresh to free a slot. */
    uint64_t vb = d->next_vblank;
    d->queue--;
    d->next_vblank += d->period_ns;
    d->queue++;
    int64_t jitter = d->jitter_ns ? (int64_t)(next_rand(d) % (2u * (unsigned)d->jitter_ns + 1u)) - d->jitter_ns : 0;
    uint64_t ret = vb + (uint64_t)jitter;
    if (d->stall_every && next_rand(d) % (unsigned)d->stall_every == 0)
        ret += d->period_ns; /* the compositor returned the slot a refresh late */
    return ret;
}

/* ── Animation run ── */

typedef struct
{
    int holds[16]; /* holds[k] = frames shown for k presents (k capped at 15) */
    int runs;
    unsigned misses;
    uint64_t period_ns;
    PacerMode mode;
} RunResult;

static RunResult run_animation(Display *d, FramePacer *p, double fps, int presents,
                               uint64_t work_ns)
{
    RunResult r;
    memset(&r, 0, sizeof(r));
    uint64_t now = 1000 * MS;
    uint64_t t0 = 0;
    int last = -1, run = 0;
    for (int i = 0; i < presents; i++) {
        uint64_t v = pacer_begin(p, now);
        if (t0 == 0)
            t0 = v;
        uint64_t sample = v + pacer_hold_ns(p) / 2;
        int frame = (int)((double)(sample - t0) * fps / 1e9);

        now += work_ns;
        pacer_cost(p, work_ns);
        uint64_t before = now;
        uint64_t ret = display_present(d, now);
        pacer_end(p, before, ret);
        now = ret + 50000;

        if (frame == last) {
            run++;
        } else {
            if (last >= 0) {
                r.holds[run > 15 ? 15 : run]++;
                r.runs++;
            }
            last = frame;
            run = 1;
        }
    }
    r.misses = p->misses;
    r.period_ns = pacer_period(p);
    r.mode = pacer_mode(p);
    return r;
}

static int holds_outside(const RunResult *r, int lo, int hi)
{
    int n = 0;
    for (int k = 0; k < 16; k++) {
        if (k < lo || k > hi)
            n += r->holds[k];
    }
    return n;
}

/* ── Cadence ── */

static void test_60fps_on_60hz_one_refresh_each(void)
{
    Display d;
    display_init(&d, 16666667);
    d.jitter_ns = 300000;
    FramePacer p;
    pacer_init(&p, 16666667);
    RunResult r = run_animation(&d, &p, 60.0, 1200, 600000);
    ASSERT_EQ(r.mode, PACER_LOCKED);
    ASSERT_EQ(r.holds[1], r.runs);
    ASSERT_EQ(r.misses, 0u);
}

static void test_30fps_on_60hz_two_refreshes_each(void)
{
    Display d;
    display_init(&d, 16666667);
    d.jitter_ns = 300000;
    FramePacer p;
    pacer_init(&p, 16666667);
    RunResult r = run_animation(&d, &p, 30.0, 1200, 600000);
    ASSERT_TRUE(r.runs > 500);
    ASSERT_EQ(r.holds[2], r.runs);
    ASSERT_EQ(r.misses, 0u);
}

static void test_24fps_on_60hz_is_a_2_3_pattern(void)
{
    Display d;
    display_init(&d, 16666667);
    d.jitter_ns = 300000;
    FramePacer p;
    pacer_init(&p, 16666667);
    RunResult r = run_animation(&d, &p, 24.0, 1200, 600000);
    ASSERT_EQ(holds_outside(&r, 2, 3), 0);
    ASSERT_TRUE(r.holds[2] > 200 && r.holds[3] > 200);
}

static void test_30fps_on_144hz_holds_4_or_5(void)
{
    Display d;
    display_init(&d, 6944444);
    d.jitter_ns = 150000;
    FramePacer p;
    pacer_init(&p, 6944444);
    RunResult r = run_animation(&d, &p, 30.0, 2400, 400000);
    ASSERT_EQ(r.mode, PACER_LOCKED);
    ASSERT_EQ(holds_outside(&r, 4, 5), 0);
}

static void test_jitter_alone_never_changes_the_cadence(void)
{
    /* The sample refresh is a counter: return jitter that straddles a grid
     * point must not move it. Sweep jitter to 3 ms, ten times what the
     * compositor shows; beyond about a fifth of a period the late returns
     * are real misses, not jitter. */
    for (int j = 0; j <= 3000000; j += 500000) {
        Display d;
        display_init(&d, 16666667);
        d.jitter_ns = j;
        FramePacer p;
        pacer_init(&p, 16666667);
        RunResult r = run_animation(&d, &p, 30.0, 900, 800000);
        ASSERT_EQ(r.holds[2], r.runs);
    }
}

/* ── Period estimate ── */

static void test_period_converges_to_the_real_refresh(void)
{
    /* The display reports 60 Hz but really runs at 59.96 Hz. */
    Display d;
    display_init(&d, 16678000);
    d.jitter_ns = 300000;
    FramePacer p;
    pacer_init(&p, 16666667);
    RunResult r = run_animation(&d, &p, 30.0, 3600, 600000);
    int64_t err = (int64_t)r.period_ns - 16678000;
    ASSERT_TRUE(err > -2000 && err < 2000); /* within ~1e-4 */
    /* No slip against the display over a minute: all holds are 2 except the
     * one-off while the estimate was still the nominal one. */
    ASSERT_TRUE(r.holds[2] >= r.runs - 2);
}

static void test_implausible_period_is_not_adopted(void)
{
    /* A display 5% off nominal is more likely a stalled compositor than a
     * real refresh rate: the estimate stays at nominal. */
    Display d;
    display_init(&d, 17500000);
    FramePacer p;
    pacer_init(&p, 16666667);
    RunResult r = run_animation(&d, &p, 30.0, 600, 600000);
    ASSERT_EQ(r.period_ns, 16666667ULL);
}

/* ── Stalls and late frames ── */

static void test_compositor_stalls_cost_few_frames(void)
{
    Display d;
    display_init(&d, 16666667);
    d.jitter_ns = 300000;
    d.stall_every = 40;
    FramePacer p;
    pacer_init(&p, 16666667);
    RunResult r = run_animation(&d, &p, 30.0, 3600, 600000);
    ASSERT_TRUE(r.runs > 1500);
    /* A return a refresh late is within the skip slack: the sample refresh
     * keeps counting and the cadence survives. The measured real-display rate
     * is 0.3%-1.5% off cadence; hold the simulation to 4%. */
    ASSERT_TRUE(r.holds[2] * 100 >= r.runs * 96);
}

static void test_a_late_frame_is_skipped_not_repeated(void)
{
    FramePacer p;
    pacer_init(&p, 16666667);
    uint64_t now = 1000 * MS;
    uint64_t v0 = pacer_begin(&p, now);
    /* The frame took four periods to prepare. */
    now += 4 * 16666667ULL;
    pacer_cost(&p, 4 * 16666667ULL);
    pacer_end(&p, now, now + 200000);
    uint64_t v1 = pacer_begin(&p, now + 200000);
    ASSERT_TRUE(v1 > v0 + 16666667ULL); /* skipped ahead */
    ASSERT_TRUE(p.misses >= 2);
    /* The sample refresh is a whole number of periods after the first. */
    ASSERT_EQ((v1 - v0) % 16666667ULL, 0ULL);
}

static void test_work_within_a_period_does_not_skip(void)
{
    Display d;
    display_init(&d, 16666667);
    FramePacer p;
    pacer_init(&p, 16666667);
    RunResult r = run_animation(&d, &p, 60.0, 600, 14 * MS);
    ASSERT_EQ(r.misses, 0u);
    ASSERT_EQ(r.holds[1], r.runs);
}

/* ── Mode ── */

static void test_unblocked_presents_leave_the_display_clock(void)
{
    Display d;
    display_init(&d, 16666667);
    d.blocks = false;
    FramePacer p;
    pacer_init(&p, 16666667);
    RunResult r = run_animation(&d, &p, 30.0, 7, 600000);
    ASSERT_EQ(r.mode, PACER_PROBING); /* 7 fast presents is not enough */
    r = run_animation(&d, &p, 30.0, 1, 600000);
    ASSERT_EQ(pacer_mode(&p), PACER_FREE);
    /* The synthetic grid is sampled mid-interval like a display's. */
    ASSERT_EQ(pacer_hold_ns(&p), 16666667ULL);
}

static void test_a_queue_filling_start_still_locks(void)
{
    /* The first presents return at once while the swap queue fills. */
    Display d;
    display_init(&d, 16666667);
    FramePacer p;
    pacer_init(&p, 16666667);
    RunResult r = run_animation(&d, &p, 60.0, 30, 600000);
    ASSERT_EQ(r.mode, PACER_LOCKED);
    ASSERT_EQ(pacer_hold_ns(&p), 16666667ULL);
}

static void test_free_is_sticky_until_reset(void)
{
    FramePacer p;
    pacer_init(&p, 16666667);
    for (int i = 0; i < 8; i++)
        pacer_end(&p, 1000 + (uint64_t)i * 20 * MS, 1000 + (uint64_t)i * 20 * MS + 100000);
    ASSERT_EQ(pacer_mode(&p), PACER_FREE);
    for (int i = 0; i < 10; i++) {
        uint64_t t = 500 * MS + (uint64_t)i * 17 * MS;
        pacer_end(&p, t, t + 16 * MS); /* blocked */
    }
    ASSERT_EQ(pacer_mode(&p), PACER_FREE);
    pacer_reset(&p);
    ASSERT_EQ(pacer_mode(&p), PACER_PROBING);
    ASSERT_EQ(pacer_period(&p), 16666667ULL);
}

static void test_locked_gives_up_after_eight_fast_presents(void)
{
    FramePacer p;
    pacer_init(&p, 16666667);
    for (int i = 0; i < 4; i++) {
        uint64_t t = 100 * MS + (uint64_t)i * 17 * MS;
        pacer_end(&p, t, t + 16 * MS);
    }
    ASSERT_EQ(pacer_mode(&p), PACER_LOCKED);
    for (int i = 0; i < 7; i++)
        pacer_end(&p, 1000 + (uint64_t)i, 1000 + (uint64_t)i + 100000);
    ASSERT_EQ(pacer_mode(&p), PACER_LOCKED); /* one more is needed */
    pacer_end(&p, 9000, 9000 + 100000);
    ASSERT_EQ(pacer_mode(&p), PACER_FREE);
}

static void test_a_blocked_present_resets_the_fast_count(void)
{
    FramePacer p;
    pacer_init(&p, 16666667);
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 7; i++)
            pacer_end(&p, 1000, 1000 + 100000);
        pacer_end(&p, 1000, 1000 + 16 * MS); /* blocked: the run starts over */
    }
    ASSERT_TRUE(pacer_mode(&p) != PACER_FREE);
}

static void test_reset_forgets_the_run(void)
{
    FramePacer p;
    pacer_init(&p, 16666667);
    uint64_t v0 = pacer_begin(&p, 1000 * MS);
    pacer_begin(&p, 1020 * MS);
    pacer_reset(&p);
    uint64_t v1 = pacer_begin(&p, 5000 * MS);
    ASSERT_EQ(v1, 5000 * MS + 16666667ULL);
    ASSERT_TRUE(v1 > v0);
    ASSERT_EQ(p.misses, 0u);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_60fps_on_60hz_one_refresh_each);
    RUN_TEST(test_30fps_on_60hz_two_refreshes_each);
    RUN_TEST(test_24fps_on_60hz_is_a_2_3_pattern);
    RUN_TEST(test_30fps_on_144hz_holds_4_or_5);
    RUN_TEST(test_jitter_alone_never_changes_the_cadence);
    RUN_TEST(test_period_converges_to_the_real_refresh);
    RUN_TEST(test_implausible_period_is_not_adopted);
    RUN_TEST(test_compositor_stalls_cost_few_frames);
    RUN_TEST(test_a_late_frame_is_skipped_not_repeated);
    RUN_TEST(test_work_within_a_period_does_not_skip);
    RUN_TEST(test_unblocked_presents_leave_the_display_clock);
    RUN_TEST(test_a_queue_filling_start_still_locks);
    RUN_TEST(test_free_is_sticky_until_reset);
    RUN_TEST(test_locked_gives_up_after_eight_fast_presents);
    RUN_TEST(test_a_blocked_present_resets_the_fast_count);
    RUN_TEST(test_reset_forgets_the_run);

    TEST_SUMMARY();
}
