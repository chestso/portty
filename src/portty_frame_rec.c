/*
 * portty — Frame recording (QOI frames to a directory)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common.h"
#include "portty_frame_rec.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

/* Monotonic clock for frame pacing (moved here from the old script
 * module — the recorder is its only user). */
static double portty_now_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* Create the manifest with O_EXCL. The atomic create is the directory's
 * recording lock: of two recorders racing on one directory, exactly one
 * wins and the other fails here instead of truncating the winner's
 * manifest. The manifest is never deleted by the recorder, so a finished
 * (or crashed) recording keeps its claim on the directory. Returns NULL
 * with errno set on failure; a file created before a failed fdopen is
 * removed again so a failed start leaves no claim behind. */
static FILE *manifest_create(const char *path)
{
#ifdef _WIN32
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, 0644);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
#endif
    if (fd < 0)
        return NULL;
#ifdef _WIN32
    FILE *f = _fdopen(fd, "wb");
#else
    FILE *f = fdopen(fd, "w");
#endif
    if (!f) {
        int err = errno;
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        remove(path);
        errno = err;
    }
    return f;
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
    if (r->manifest)
        fclose(r->manifest);
    free(r->output_dir);
    free(r);
}

bool frame_recorder_start(FrameRecorder *r, const char *dir, int fps)
{
    if (!r || !dir)
        return false;
    if (r->recording) {
        fprintf(stderr, "record-start: already recording (record-stop first)\n");
        return false;
    }

    /* A recording owns its directory: it must be absent or empty, frames
     * are numbered from 1 and the clock starts at 0. Refusing a used
     * directory keeps the frame files and the manifest in agreement by
     * construction — there is no resume state to reconcile. */
#ifdef _WIN32
    if (_mkdir(dir) != 0 && errno != EEXIST) {
#else
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
#endif
        fprintf(stderr, "record-start: cannot create directory %s: %s\n",
                dir, strerror(errno));
        return false;
    }
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "record-start: cannot open directory %s: %s\n",
                dir, strerror(errno));
        return false;
    }
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            fprintf(stderr, "record-start: %s is not empty - a recording needs "
                            "a fresh directory (empty it or choose another)\n",
                    dir);
            closedir(d);
            return false;
        }
    }
    closedir(d);

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/frames.csv", dir);
    FILE *manifest = manifest_create(manifest_path);
    if (!manifest) {
        if (errno == EEXIST)
            fprintf(stderr, "record-start: %s already exists - another "
                            "recorder claimed the directory\n",
                    manifest_path);
        else
            fprintf(stderr, "record-start: cannot create %s: %s\n",
                    manifest_path, strerror(errno));
        return false;
    }
    /* The header goes out immediately; rows are flushed one by one in
     * frame_recorder_advance() so a crash cannot leave frames on disk
     * without their manifest rows. */
    if (fprintf(manifest, "index,timestamp,filename\n") < 0 ||
        fflush(manifest) != 0) {
        fprintf(stderr, "record-start: cannot write %s: %s\n",
                manifest_path, strerror(errno));
        fclose(manifest);
        remove(manifest_path);
        return false;
    }

    char *dup = strdup(dir);
    if (!dup) {
        fclose(manifest);
        remove(manifest_path);
        return false;
    }

    free(r->output_dir);
    r->output_dir = dup;
    r->manifest = manifest;
    r->target_fps = fps > 0 ? fps : 30;
    r->frame_index = 1;
    r->frames_written = 0;
    r->frames_skipped = 0;
    r->start_time = portty_now_seconds();
    r->recording = true;

    vlog("record-start: %s at %d fps, fresh recording at frame_000001.qoi\n",
         dir, r->target_fps);
    return true;
}

void frame_recorder_stop(FrameRecorder *r)
{
    if (!r || !r->recording)
        return;

    double elapsed = portty_now_seconds() - r->start_time;
    int expected = (int)(elapsed * r->target_fps);
    r->frames_skipped = expected - r->frames_written;
    if (r->frames_skipped < 0)
        r->frames_skipped = 0;

    if (r->manifest) {
        fclose(r->manifest);
        r->manifest = NULL;
    }

    vlog("record-stop: wrote %d frames, skipped %d (expected %d at %d fps)\n",
         r->frames_written, r->frames_skipped, expected, r->target_fps);
    r->recording = false;
}

void frame_recorder_build_path(FrameRecorder *r, char *buf, size_t len)
{
    if (!r || !buf)
        return;
    r->capture_time = portty_now_seconds() - r->start_time;
    snprintf(buf, len, "%s/frame_%06d.qoi", r->output_dir, r->frame_index);
}

void frame_recorder_advance(FrameRecorder *r)
{
    if (!r)
        return;

    // Write the manifest row for the frame just written, using the
    // timestamp captured before the write, and flush it: a crash must not
    // lose the rows for frames already on disk. A failed write ends the
    // recording loudly rather than capturing frames nothing lists.
    if (r->manifest) {
        if (fprintf(r->manifest, "%d,%.3f,frame_%06d.qoi\n",
                    r->frame_index, r->capture_time, r->frame_index) < 0 ||
            fflush(r->manifest) != 0) {
            fprintf(stderr, "record: manifest write failed (%s) - "
                            "stopping recording\n",
                    strerror(errno));
            frame_recorder_stop(r);
            return;
        }
    }

    r->frame_index++;
    r->frames_written++;
}
