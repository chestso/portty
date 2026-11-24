/*
 * portty — Frame recording interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef PORTTY_FRAME_REC_H
#define PORTTY_FRAME_REC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct
{
    char *output_dir;
    int target_fps;
    bool recording;
    int frame_index;
    double start_time;
    double capture_time;
    int frames_written;
    int frames_skipped;
    FILE *manifest;
} FrameRecorder;

FrameRecorder *frame_recorder_new(void);
void frame_recorder_free(FrameRecorder *r);
/* Begin a recording in `dir`, which must be absent or empty: a recording
 * owns its directory. Frames are numbered from 1, the clock starts at 0,
 * and frames.csv is created exclusively (it doubles as the directory's
 * recording lock and is never deleted). Returns false with the reason on
 * stderr if the directory is in use, not empty, or the manifest cannot be
 * created — a used directory is never truncated or resumed. */
bool frame_recorder_start(FrameRecorder *r, const char *dir, int fps);
void frame_recorder_stop(FrameRecorder *r);
void frame_recorder_build_path(FrameRecorder *r, char *buf, size_t len);
void frame_recorder_advance(FrameRecorder *r);

#endif /* PORTTY_FRAME_REC_H */
