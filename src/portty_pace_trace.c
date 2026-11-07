/*
 * portty — Present-pacing trace
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "portty_pace_trace.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static FILE *g_trace;

int pace_trace_format(char *buf, size_t cap, const PaceTraceRec *rec)
{
    int n = snprintf(buf, cap,
                     "P %" PRIu64 " %" PRIu64 " %" PRIu64 " %d %u %d %d",
                     rec->t_before_ns, rec->t_after_ns, rec->sample_ns,
                     rec->locked, rec->misses, rec->held, rec->nframes);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    for (int i = 0; i < rec->nframes && i < PACE_TRACE_MAX_FRAMES; i++) {
        int m = snprintf(buf + n, cap - (size_t)n, " %d", rec->frames[i]);
        if (m < 0 || (size_t)m >= cap - (size_t)n)
            return -1;
        n += m;
    }
    return n;
}

bool pace_trace_open(const char *path)
{
    pace_trace_close();
    if (!path || !path[0])
        return false;
    g_trace = fopen(path, "w");
    if (!g_trace) {
        fprintf(stderr, "ERROR: cannot open pace trace %s\n", path);
        return false;
    }
    fputs("# portty pace trace v1\n"
          "# P t_before_ns t_after_ns sample_ns locked misses held nframes frame...\n",
          g_trace);
    return true;
}

bool pace_trace_open_from_env(void)
{
    return pace_trace_open(getenv("PORTTY_PACE_TRACE"));
}

bool pace_trace_active(void)
{
    return g_trace != NULL;
}

void pace_trace_record(const PaceTraceRec *rec)
{
    if (!g_trace || !rec)
        return;
    char line[256];
    if (pace_trace_format(line, sizeof(line), rec) > 0) {
        fputs(line, g_trace);
        fputc('\n', g_trace);
    }
}

void pace_trace_close(void)
{
    if (g_trace) {
        fclose(g_trace);
        g_trace = NULL;
    }
}
