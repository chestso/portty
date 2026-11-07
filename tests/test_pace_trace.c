/*
 * portty — Pace trace tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "test_helpers.h"
#include "../src/portty_pace_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_format_fields(void)
{
    PaceTraceRec r = { 0 };
    r.t_before_ns = 100;
    r.t_after_ns = 250;
    r.sample_ns = 300;
    r.locked = 1;
    r.misses = 4;
    r.held = 1;
    r.nframes = 2;
    r.frames[0] = 7;
    r.frames[1] = 9;
    char buf[128];
    int n = pace_trace_format(buf, sizeof(buf), &r);
    ASSERT_TRUE(n > 0);
    ASSERT_STR_EQ(buf, "P 100 250 300 1 4 1 2 7 9");
}

static void test_format_no_animation(void)
{
    PaceTraceRec r = { 0 };
    r.t_before_ns = 1;
    r.t_after_ns = 2;
    char buf[64];
    ASSERT_TRUE(pace_trace_format(buf, sizeof(buf), &r) > 0);
    ASSERT_STR_EQ(buf, "P 1 2 0 0 0 0 0");
}

static void test_format_small_buffer(void)
{
    PaceTraceRec r = { 0 };
    r.nframes = 1;
    char buf[8];
    ASSERT_EQ(pace_trace_format(buf, sizeof(buf), &r), -1);
}

static void test_file_roundtrip(void)
{
    char path[] = "/tmp/portty-pace-trace-XXXXXX";
    int fd = mkstemp(path);
    ASSERT_TRUE(fd >= 0);
    close(fd);

    ASSERT_FALSE(pace_trace_active());
    ASSERT_TRUE(pace_trace_open(path));
    ASSERT_TRUE(pace_trace_active());

    PaceTraceRec r = { 0 };
    r.t_before_ns = 10;
    r.t_after_ns = 20;
    r.nframes = 1;
    r.frames[0] = 3;
    pace_trace_record(&r);
    pace_trace_close();
    ASSERT_FALSE(pace_trace_active());

    FILE *f = fopen(path, "r");
    ASSERT_NOT_NULL(f);
    char line[256];
    int data_lines = 0;
    int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#')
            continue;
        data_lines++;
        ok = strcmp(line, "P 10 20 0 0 0 0 1 3\n") == 0;
    }
    fclose(f);
    unlink(path);
    ASSERT_EQ(data_lines, 1);
    ASSERT_TRUE(ok);
}

static void test_unset_env_is_off(void)
{
#ifdef _WIN32
    /* MinGW does not declare unsetenv; _putenv with an empty value leaves
     * getenv returning "" (or NULL), and pace_trace_open treats both as
     * off, which is all this check needs. */
    _putenv("PORTTY_PACE_TRACE=");
#else
    unsetenv("PORTTY_PACE_TRACE");
#endif
    ASSERT_FALSE(pace_trace_open_from_env());
    ASSERT_FALSE(pace_trace_active());
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_format_fields);
    RUN_TEST(test_format_no_animation);
    RUN_TEST(test_format_small_buffer);
    RUN_TEST(test_file_roundtrip);
    RUN_TEST(test_unset_env_is_off);

    TEST_SUMMARY();
}
