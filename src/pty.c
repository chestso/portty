#define _GNU_SOURCE
#include "bloom_pty.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

PtyContext *pty_create(int rows, int cols, const char *shell)
{
    PtyContext *ctx = calloc(1, sizeof(PtyContext));
    if (!ctx) {
        fprintf(stderr, "ERROR: Failed to allocate PTY context\n");
        return NULL;
    }

    ctx->rows = rows;
    ctx->cols = cols;
    ctx->master_fd = -1;
    ctx->child_pid = -1;

    // Set up initial window size
    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    // Create PTY and fork
    pid_t pid = forkpty(&ctx->master_fd, NULL, NULL, &ws);
    if (pid < 0) {
        fprintf(stderr, "ERROR: forkpty failed: %s\n", strerror(errno));
        free(ctx);
        return NULL;
    }

    if (pid == 0) {
        // Child process - exec shell

        // Set TERM environment variable
        setenv("TERM", "xterm-256color", 1);

        // Also set COLORTERM for applications that check it
        setenv("COLORTERM", "truecolor", 1);

        // Determine which shell to run
        const char *shell_to_exec = shell;
        if (!shell_to_exec) {
            shell_to_exec = getenv("SHELL");
        }
        if (!shell_to_exec) {
            shell_to_exec = "/bin/sh";
        }

        vlog("PTY child: execing shell '%s'\n", shell_to_exec);

        // Execute the shell as a login shell
        const char *shell_basename = strrchr(shell_to_exec, '/');
        if (shell_basename) {
            shell_basename++; // Skip the '/'
        } else {
            shell_basename = shell_to_exec;
        }

        // Create login shell name (prefix with -)
        char login_shell_name[256];
        snprintf(login_shell_name, sizeof(login_shell_name), "-%s", shell_basename);

        // Exec shell - use login shell convention
        execlp(shell_to_exec, login_shell_name, (char *)NULL);

        // If exec fails, try fallback
        fprintf(stderr, "ERROR: Failed to exec '%s': %s\n", shell_to_exec, strerror(errno));
        execlp("/bin/sh", "-sh", (char *)NULL);
        _exit(127);
    }

    // Parent process
    ctx->child_pid = pid;
    vlog("PTY created: master_fd=%d, child_pid=%d, size=%dx%d\n",
         ctx->master_fd, ctx->child_pid, cols, rows);

    return ctx;
}

void pty_destroy(PtyContext *ctx)
{
    if (!ctx)
        return;

    vlog("PTY destroy: master_fd=%d, child_pid=%d\n", ctx->master_fd, ctx->child_pid);

    // Close master fd first to signal EOF to child
    if (ctx->master_fd >= 0) {
        close(ctx->master_fd);
        ctx->master_fd = -1;
    }

    // Wait for child to exit (with timeout via SIGKILL)
    if (ctx->child_pid > 0) {
        int status;
        pid_t result = waitpid(ctx->child_pid, &status, WNOHANG);
        if (result == 0) {
            // Child still running, send SIGHUP then wait briefly
            kill(ctx->child_pid, SIGHUP);
            usleep(100000); // 100ms

            result = waitpid(ctx->child_pid, &status, WNOHANG);
            if (result == 0) {
                // Still running, force kill
                kill(ctx->child_pid, SIGKILL);
                waitpid(ctx->child_pid, &status, 0);
            }
        }
        ctx->child_pid = -1;
    }

    free(ctx);
}

ssize_t pty_write(PtyContext *ctx, const char *data, size_t len)
{
    if (!ctx || ctx->master_fd < 0 || !data || len == 0)
        return -1;

    return write(ctx->master_fd, data, len);
}

ssize_t pty_read(PtyContext *ctx, char *buf, size_t bufsize)
{
    if (!ctx || ctx->master_fd < 0 || !buf || bufsize == 0)
        return -1;

    return read(ctx->master_fd, buf, bufsize);
}

int pty_resize(PtyContext *ctx, int rows, int cols)
{
    if (!ctx || ctx->master_fd < 0)
        return -1;

    struct winsize ws = {
        .ws_row = (unsigned short)rows,
        .ws_col = (unsigned short)cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0
    };

    if (ioctl(ctx->master_fd, TIOCSWINSZ, &ws) < 0) {
        fprintf(stderr, "ERROR: ioctl TIOCSWINSZ failed: %s\n", strerror(errno));
        return -1;
    }

    ctx->rows = rows;
    ctx->cols = cols;
    vlog("PTY resized to %dx%d\n", cols, rows);
    return 0;
}

bool pty_is_running(PtyContext *ctx)
{
    if (!ctx || ctx->child_pid <= 0)
        return false;

    int status;
    pid_t result = waitpid(ctx->child_pid, &status, WNOHANG);

    if (result == 0) {
        // Child still running
        return true;
    } else if (result == ctx->child_pid) {
        // Child has exited
        vlog("PTY child exited: pid=%d, status=%d\n", ctx->child_pid, status);
        ctx->child_pid = -1;
        return false;
    } else {
        // Error or no such child
        return false;
    }
}

int pty_get_master_fd(PtyContext *ctx)
{
    if (!ctx)
        return -1;
    return ctx->master_fd;
}
