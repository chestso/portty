#ifndef PTY_H
#define PTY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif

/* Opaque — defined in platform-specific pty.c / pty_w32.c */
typedef struct PtyContext PtyContext;

/**
 * Create a new PTY and spawn a process.
 *
 * @param rows Initial terminal rows
 * @param cols Initial terminal columns
 * @param argv NULL-terminated argument array (argv[0] is program to execute).
 *             If NULL, spawns the default shell from $SHELL or /bin/sh.
 * @return PtyContext pointer on success, NULL on failure
 */
PtyContext *pty_create(int rows, int cols, char *const argv[]);

/**
 * Destroy PTY context and terminate child process.
 *
 * @param ctx PTY context to destroy
 */
void pty_destroy(PtyContext *ctx);

/**
 * Write data to the PTY (send to shell).
 *
 * @param ctx PTY context
 * @param data Data to write
 * @param len Length of data
 * @return Number of bytes written, or -1 on error
 */
ssize_t pty_write(PtyContext *ctx, const char *data, size_t len);

/**
 * Read data from the PTY (receive from shell).
 *
 * @param ctx PTY context
 * @param buf Buffer to read into
 * @param bufsize Size of buffer
 * @return Number of bytes read, 0 on EOF, or -1 on error
 */
ssize_t pty_read(PtyContext *ctx, char *buf, size_t bufsize);

/**
 * Resize the PTY window size.
 *
 * @param ctx PTY context
 * @param rows New number of rows
 * @param cols New number of columns
 * @return 0 on success, -1 on error
 */
int pty_resize(PtyContext *ctx, int rows, int cols);

/**
 * Check if the child process is still running.
 *
 * @param ctx PTY context
 * @return true if child is still running, false otherwise
 */
bool pty_is_running(PtyContext *ctx);

/**
 * Get the master file descriptor for poll/select.
 *
 * @param ctx PTY context
 * @return Master file descriptor
 */
int pty_get_master_fd(PtyContext *ctx);

/**
 * Initialize SIGCHLD signal handling.
 *
 * Sets up a self-pipe for async-signal-safe notification of child exit.
 * Must be called before pty_create().
 *
 * @return 0 on success, -1 on failure
 */
int pty_signal_init(void);

/**
 * Cleanup SIGCHLD signal handling.
 *
 * Closes the signal pipe and restores default signal handling.
 */
void pty_signal_cleanup(void);

/**
 * Get the read end of the signal pipe for poll/select.
 *
 * When this fd becomes readable, a SIGCHLD was received.
 * After reading, call pty_signal_drain() to clear the pipe.
 *
 * @return File descriptor, or -1 if signal handling not initialized
 */
int pty_signal_get_fd(void);

/**
 * Drain the signal pipe after it becomes readable.
 *
 * Call this after poll/select indicates the signal pipe is readable.
 */
void pty_signal_drain(void);

/**
 * Get the child process PID (Unix only, for /proc queries).
 *
 * @param ctx PTY context
 * @return Child PID, or -1 if not running
 */
int pty_get_child_pid(PtyContext *ctx);

#ifdef _WIN32
/**
 * Get the child process handle for WaitForMultipleObjects.
 *
 * @param ctx PTY context
 * @return Process HANDLE cast to void*, or NULL
 */
void *pty_get_process_handle(PtyContext *ctx);

/**
 * Close the pseudo-console to unblock any pending ReadFile.
 *
 * Call this before waiting for the reader thread to exit on shutdown.
 * Safe to call multiple times.
 */
void pty_close_console(PtyContext *ctx);
#endif

#endif /* PTY_H */
