/*
 * portty — SIGCHLD child reaping tests
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/* The process installs a SIGCHLD handler for the PTY child, but SIGCHLD
 * arrives for every child. Only the PTY child has a waiter
 * (pty_is_running/pty_poll_exit); the short-lived helpers os_compat forks for
 * open_url() and spawn_process() have none, so pty_signal_drain() has to reap
 * them or they stay zombies for the life of the process.
 */

#include "portty_pty.h"
#include "test_helpers.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static PtyContext *spawn_sleeping_child(void)
{
    char *argv[] = { "/bin/sh", "-c", "sleep 30", NULL };
    return pty_create(24, 80, 0, 0, argv);
}

/* A child with the shape os_compat forks: it exits at once and nobody waits
 * for it. */
static pid_t spawn_helper(void)
{
    pid_t pid = fork();
    if (pid == 0)
        _exit(0);
    return pid;
}

/* Wait for the SIGCHLD self-pipe the way the reader thread does: the signal
 * itself interrupts poll() with EINTR before the handler's byte is visible,
 * so an EINTR is a retry, not a failure. */
static bool wait_signal_pipe(int timeout_ms)
{
    struct pollfd pfd = { .fd = pty_signal_get_fd(), .events = POLLIN };
    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    return rc > 0;
}

/* Drop any notification left by an earlier child (pty_destroy's SIGHUP in a
 * previous test, for instance) so the next wait sees this test's signal. */
static void flush_signal_pipe(void)
{
    pty_signal_drain(NULL);
}

static void test_helper_child_is_reaped(void)
{
    PtyContext *ctx = spawn_sleeping_child();
    ASSERT_NOT_NULL(ctx);

    flush_signal_pipe();
    pid_t helper = spawn_helper();
    ASSERT_TRUE(helper > 0);

    ASSERT_TRUE(wait_signal_pipe(5000));
    pty_signal_drain(ctx);

    int status = 0;
    errno = 0;
    ASSERT_EQ(waitpid(helper, &status, WNOHANG), -1);
    ASSERT_EQ(errno, ECHILD);

    /* Reaping the helper must not touch the PTY child. */
    ASSERT_TRUE(pty_is_running(ctx));

    pty_destroy(ctx);
}

static void test_pty_child_status_survives_drain(void)
{
    char *argv[] = { "/bin/sh", "-c", "exit 7", NULL };

    flush_signal_pipe();
    PtyContext *ctx = pty_create(24, 80, 0, 0, argv);
    ASSERT_NOT_NULL(ctx);

    ASSERT_TRUE(wait_signal_pipe(5000));
    pty_signal_drain(ctx);

    int status = -1;
    ASSERT_EQ(pty_poll_exit(ctx, &status), 1);
    ASSERT_EQ(status, 7);
    ASSERT_FALSE(pty_is_running(ctx));

    pty_destroy(ctx);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    if (pty_signal_init() != 0) {
        fprintf(stderr, "pty_signal_init failed\n");
        return 1;
    }

    printf("test_pty_reap\n");
    RUN_TEST(test_helper_child_is_reaped);
    RUN_TEST(test_pty_child_status_survives_drain);

    pty_signal_cleanup();
    TEST_SUMMARY();
}
