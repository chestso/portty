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
bool frame_recorder_start(FrameRecorder *r, const char *dir, int fps);
void frame_recorder_stop(FrameRecorder *r);
void frame_recorder_build_path(FrameRecorder *r, char *buf, size_t len);
void frame_recorder_advance(FrameRecorder *r);

#endif /* PORTTY_FRAME_REC_H */
