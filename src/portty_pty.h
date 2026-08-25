/*
 * portty — PTY abstraction interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

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

#ifndef _WIN32
/**
 * Get the child process current working directory (Unix only).
 *
 * Uses /proc/<pid>/cwd to resolve the CWD so that a new terminal window
 * can be spawned in the same directory. On Windows, the CWD is tracked
 * via OSC 7/OSC 9;9 escape sequences instead (ReadProcessMemory fails
 * with ERROR_PARTIAL_COPY for ConPTY children).
 *
 * @param ctx PTY context
 * @param buf Output buffer
 * @param bufsize Size of buffer
 * @return true on success, false on failure or if CWD cannot be resolved
 */
bool pty_get_child_cwd(PtyContext *ctx, char *buf, size_t bufsize);
#endif

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

/**
 * Initiate an overlapped (async) read on the PTY output pipe.
 *
 * If data is immediately available, returns the byte count (> 0).
 * If the I/O is pending, returns 0 — the caller should wait on
 * ovl->hEvent and then call pty_get_overlapped_result().
 * Returns -1 on error.
 *
 * The caller must initialize ovl->hEvent as a manual-reset event
 * before calling.
 */
ssize_t pty_read_overlapped(PtyContext *ctx, char *buf, size_t bufsize,
                            OVERLAPPED *ovl);

/**
 * Check the result of a completed overlapped read.
 *
 * Returns true if the I/O completed (bytes available in *out_bytes).
 * Returns false if still pending (ERROR_IO_INCOMPLETE) or on error
 * (out_bytes may still be non-zero if some bytes were read before error).
 */
bool pty_get_overlapped_result(PtyContext *ctx, OVERLAPPED *ovl,
                               ssize_t *out_bytes);

/**
 * Cancel any pending overlapped I/O on the PTY output pipe.
 *
 * Called from the main thread (e.g. when pausing the PTY reader)
 * to unblock a pending read so the reader thread can re-evaluate
 * the pause flag.
 */
void pty_cancel_read(PtyContext *ctx);

/**
 * Probe whether the OS-level ConPTY passes DCS sequences through.
 *
 * Creates a temporary ConPTY, sends a DCS sequence, and checks whether
 * it appears on the output pipe. Returns true if DCS passthrough works.
 */
bool pty_conpty_dcs_passthrough(void);

/**
 * Get a human-readable name for the ConPTY host in use.
 *
 * Returns "conpty.dll + OpenConsole.exe" if the bundled DLL is loaded,
 * or "system conhost.exe" if using the OS-level host.
 */
const char *pty_conpty_host_name(void);
#endif

#endif /* PTY_H */
