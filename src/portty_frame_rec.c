#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "portty_frame_rec.h"
#include "portty_script.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#endif

static int find_next_frame_index(const char *dir)
{
    int max_idx = 0;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            int idx;
            if (sscanf(ent->d_name, "frame_%d.qoi", &idx) == 1) {
                if (idx > max_idx)
                    max_idx = idx;
            }
        }
        closedir(d);
    }
    return max_idx + 1;
}

FrameRecorder *frame_recorder_new(void)
{
    FrameRecorder *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->target_fps = 30;
    return r;
}

void frame_recorder_free(FrameRecorder *r)
{
    if (!r)
        return;
    free(r->output_dir);
    free(r);
}

bool frame_recorder_start(FrameRecorder *r, const char *dir, int fps)
{
    if (!r || !dir)
        return false;
    if (r->recording)
        return false;

#ifndef _WIN32
    mkdir(dir, 0755);
#else
    mkdir(dir);
#endif

    free(r->output_dir);
    r->output_dir = strdup(dir);
    if (!r->output_dir)
        return false;

    r->target_fps = fps > 0 ? fps : 30;
    r->frame_index = find_next_frame_index(dir);
    r->recording = true;
    r->frames_written = 0;
    r->frames_skipped = 0;
    r->start_time = portty_debug_now_seconds();

    vlog("record-start: %s at %d fps, starting at frame_%06d.qoi\n",
         dir, r->target_fps, r->frame_index);
    return true;
}

void frame_recorder_stop(FrameRecorder *r)
{
    if (!r || !r->recording)
        return;

    double elapsed = portty_debug_now_seconds() - r->start_time;
    int expected = (int)(elapsed * r->target_fps);
    r->frames_skipped = expected - r->frames_written;
    if (r->frames_skipped < 0)
        r->frames_skipped = 0;

    vlog("record-stop: wrote %d frames, skipped %d (expected %d at %d fps)\n",
         r->frames_written, r->frames_skipped, expected, r->target_fps);
    r->recording = false;
}

void frame_recorder_build_path(FrameRecorder *r, char *buf, size_t len)
{
    if (!r || !buf)
        return;
    snprintf(buf, len, "%s/frame_%06d.qoi", r->output_dir, r->frame_index);
}

void frame_recorder_advance(FrameRecorder *r)
{
    if (!r)
        return;
    r->frame_index++;
    r->frames_written++;
}
