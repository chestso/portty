#define _GNU_SOURCE
#include "bloom_pty.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

struct PtyContext
{
    int master_fd;
    pid_t child_pid;
    int rows;
    int cols;
};

// Self-pipe for SIGCHLD notification
static int sigchld_pipe[2] = { -1, -1 };
static struct sigaction old_sigchld_action;
static bool sigchld_handler_installed = false;

// SIGCHLD handler - writes to pipe (async-signal-safe)
static void sigchld_handler(int sig)
{
    (void)sig;
    // write() is async-signal-safe
    char c = 1;
    int saved_errno = errno;
    write(sigchld_pipe[1], &c, 1);
    errno = saved_errno;
}

int pty_signal_init(void)
{
    // Create self-pipe
    if (pipe(sigchld_pipe) < 0) {
        fprintf(stderr, "ERROR: pipe() failed: %s\n", strerror(errno));
        return -1;
    }

    // Set both ends to non-blocking
    int flags = fcntl(sigchld_pipe[0], F_GETFL);
    if (flags >= 0)
        fcntl(sigchld_pipe[0], F_SETFL, flags | O_NONBLOCK);

    flags = fcntl(sigchld_pipe[1], F_GETFL);
    if (flags >= 0)
        fcntl(sigchld_pipe[1], F_SETFL, flags | O_NONBLOCK);

    // Set close-on-exec to avoid leaking to child
    fcntl(sigchld_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(sigchld_pipe[1], F_SETFD, FD_CLOEXEC);

    // Install SIGCHLD handler
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, &old_sigchld_action) < 0) {
        fprintf(stderr, "ERROR: sigaction() failed: %s\n", strerror(errno));
        close(sigchld_pipe[0]);
        close(sigchld_pipe[1]);
        sigchld_pipe[0] = sigchld_pipe[1] = -1;
        return -1;
    }

    sigchld_handler_installed = true;
    vlog("SIGCHLD handler installed, signal pipe fd=%d\n", sigchld_pipe[0]);
    return 0;
}

void pty_signal_cleanup(void)
{
    // Restore old signal handler
    if (sigchld_handler_installed) {
        sigaction(SIGCHLD, &old_sigchld_action, NULL);
        sigchld_handler_installed = false;
    }

    // Close pipe
    if (sigchld_pipe[0] >= 0) {
        close(sigchld_pipe[0]);
        sigchld_pipe[0] = -1;
    }
    if (sigchld_pipe[1] >= 0) {
        close(sigchld_pipe[1]);
        sigchld_pipe[1] = -1;
    }

    vlog("SIGCHLD handler cleaned up\n");
}

int pty_signal_get_fd(void)
{
    return sigchld_pipe[0];
}

void pty_signal_drain(void)
{
    if (sigchld_pipe[0] < 0)
        return;

    // Drain all bytes from the pipe
    char buf[64];
    while (read(sigchld_pipe[0], buf, sizeof(buf)) > 0) {
        // Keep draining
    }
}

PtyContext *pty_create(int rows, int cols, char *const argv[])
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
        // Child process - exec command or default shell

        // Ensure erase character is DEL (0x7F) to match terminfo kbs=^?
        // macOS defaults to ^H which causes visual backspace issues
        struct termios tios;
        if (tcgetattr(STDIN_FILENO, &tios) == 0) {
            tios.c_cc[VERASE] = 0x7f;
            tcsetattr(STDIN_FILENO, TCSANOW, &tios);
        }

        // Set TERM environment variable.
        // Default to the vty-compatible entry: it inherits setaf/setab
        // from xterm-256color, so Haskell TUIs using vty-unix (brick,
        // matterhorn, etc.) can parse it. 24-bit colour still works via
        // Tc/RGB flags and COLORTERM=truecolor for apps using modern
        // detection. Apps that want the full ncurses-parseable entry
        // with 24-bit setaf/setab/Setulc can set TERM=bloom-terminal-256color.
        setenv("TERM", "bloom-terminal-vty-256color", 1);

        // Point to our installed terminfo database
#ifdef BLOOM_DATADIR
        {
            char buf[4096];
            const char *home = getenv("HOME");
            const char *existing = getenv("TERMINFO_DIRS");
            if (existing) {
                snprintf(buf, sizeof(buf),
                         BLOOM_DATADIR "/terminfo:%s/.terminfo:%s",
                         home ? home : "", existing);
            } else {
                snprintf(buf, sizeof(buf),
                         BLOOM_DATADIR "/terminfo:%s/.terminfo:",
                         home ? home : "");
            }
            setenv("TERMINFO_DIRS", buf, 1);
        }
#endif

        // Help emacs find our term/*.el file
#ifdef BLOOM_DATADIR
        {
            char buf[4096];
            const char *existing = getenv("EMACSLOADPATH");
            if (existing) {
                snprintf(buf, sizeof(buf),
                         BLOOM_DATADIR "/emacs/site-lisp:%s", existing);
            } else {
                snprintf(buf, sizeof(buf),
                         BLOOM_DATADIR "/emacs/site-lisp:");
            }
            setenv("EMACSLOADPATH", buf, 1);
        }
#endif

        // Also set COLORTERM for applications that check it
        setenv("COLORTERM", "truecolor", 1);

        // Advertise kitty keyboard protocol support to TUIs that
        // gate progressive enhancement on TERM_PROGRAM rather than
        // probing via XTVERSION. Claude Code, Helix, and Neovim all
        // allowlist a small set of TERM_PROGRAM values; bloom-terminal
        // isn't in that list, so we identify as `ghostty` — the
        // architecturally closest peer (in-process VT engine, no
        // libvterm) — to opt into the kitty kb push that Shift+Enter,
        // Ctrl+Tab and friends rely on. terminfo lookup is unaffected
        // because that uses TERM, not TERM_PROGRAM.
        setenv("TERM_PROGRAM", "ghostty", 1);

        // Set window title via PROMPT_COMMAND for bash.
        // Fedora's /etc/bashrc guards its PROMPT_COMMAND setup with
        // [ -z "$PROMPT_COMMAND" ], so pre-setting it here causes bash
        // to skip its xterm*/screen* case and use our value instead.
        // User's .bashrc can still override this.
        setenv("PROMPT_COMMAND",
               "printf '\\033]0;%s@%s:%s\\007' "
               "\"${USER}\" \"${HOSTNAME%%.*}\" \"${PWD/#$HOME/\\~}\"",
               0);

        if (argv && argv[0]) {
            // Command provided - execute directly
            vlog("PTY child: execing '%s'\n", argv[0]);
            execvp(argv[0], argv);

            // If exec fails
            fprintf(stderr, "ERROR: Failed to exec '%s': %s\n", argv[0], strerror(errno));
            _exit(127);
        }

        // No command - run default shell
        const char *shell_to_exec = getenv("SHELL");
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

int pty_get_child_pid(PtyContext *ctx)
{
    if (!ctx)
        return -1;
    return (int)ctx->child_pid;
}
