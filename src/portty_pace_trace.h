/*
 * portty — Present-pacing trace
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef PORTTY_PACE_TRACE_H
#define PORTTY_PACE_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PACE_TRACE_MAX_FRAMES 4

/* One presented frame. Times are SDL ticks in nanoseconds. */
typedef struct
{
    uint64_t t_before_ns; /* just before SDL_RenderPresent */
    uint64_t t_after_ns;  /* just after it returned */
    uint64_t sample_ns;   /* presentation time the animation was sampled for (0 = none) */
    int locked;           /* the pacer is phase-locked to the display */
    unsigned misses;      /* frames the pacer had to skip so far */
    int held;             /* the frame re-presented the cached scene */
    int nframes;
    int frames[PACE_TRACE_MAX_FRAMES]; /* current_frame of each Lottie */
} PaceTraceRec;

/* Open the trace file named by $PORTTY_PACE_TRACE. Returns true when tracing
 * is on; does nothing (and returns false) when the variable is unset. */
bool pace_trace_open_from_env(void);
bool pace_trace_open(const char *path);
bool pace_trace_active(void);
void pace_trace_record(const PaceTraceRec *rec);
void pace_trace_close(void);

/* Format one record as a trace line (no newline). Returns the length, or -1
 * when the buffer is too small. Exposed for tests. */
int pace_trace_format(char *buf, size_t cap, const PaceTraceRec *rec);

#endif /* PORTTY_PACE_TRACE_H */
