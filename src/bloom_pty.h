#ifndef PTY_H
#define PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct PtyContext
{
    int master_fd;
    pid_t child_pid;
    int rows;
    int cols;
} PtyContext;

/**
 * Create a new PTY and spawn a shell process.
 *
 * @param rows Initial terminal rows
 * @param cols Initial terminal columns
 * @param shell Shell to execute (NULL for default from SHELL env or /bin/sh)
 * @return PtyContext pointer on success, NULL on failure
 */
PtyContext *pty_create(int rows, int cols, const char *shell);

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

#endif /* PTY_H */
