/*
 * portty — Cross-platform OS helpers interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef OS_COMPAT_H
#define OS_COMPAT_H

#include <stddef.h>
#include <stdbool.h>

/*
 * os_compat.h — cross-platform OS abstraction for exe path resolution
 * and detached process spawning.
 *
 * Used by the SDL3 backend to avoid duplicating
 * platform-specific #ifdef blocks.
 */

/*
 * Resolve the absolute path of the current executable.
 *
 * @param buf     Output buffer
 * @param bufsize Size of buf
 * @return true on success, false if the path could not be resolved
 */
bool os_compat_get_exe_path(char *buf, size_t bufsize);

/*
 * Spawn a new detached process running the given executable.
 *
 * On Unix: fork + setsid + chdir + execl.
 * On Windows: CreateProcessW with DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP.
 *
 * @param exe_path  Path to the executable to run
 * @param cwd       Working directory for the new process, or NULL to inherit
 * @return true on success, false on failure
 */
bool os_compat_spawn_process(const char *exe_path, const char *cwd);

/*
 * Open a URL in the user's default browser / application.
 *
 * On Windows: ShellExecuteW with "open" verb.
 * On macOS:    open(1) via fork+execl.
 * On Linux:    xdg-open(1) via fork+execl.
 *
 * @param url     NUL-terminated URL to open
 * @param err     Buffer for error message on failure
 * @param errlen  Size of err buffer
 * @return true on success, false on failure
 */
bool os_compat_open_url(const char *url, char *err, size_t errlen);

#endif /* OS_COMPAT_H */
