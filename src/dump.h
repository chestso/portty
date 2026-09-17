/*
 * portty — Terminal state dump interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef DUMP_H
#define DUMP_H

#include "portty_backend.h" // PorttyDiag
#include "portty_conf.h"
#include "term.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Optional context that is not part of the terminal itself but makes a dump
// self-contained for bug reports. Both pointers may be NULL — the writer then
// omits the corresponding section rather than failing.
typedef struct
{
    const PorttyDiag *diag; // renderer/GPU/host snapshot, or NULL
    const PorttyConf *conf; // effective config snapshot, or NULL
    int scroll_offset;      // current scrollback view offset (0 = live bottom)
} TerminalDumpMeta;

// Stream a JSON document describing the entire terminal state to `out`:
// geometry, cursor, all DEC modes, scrollback, selection, palette, images,
// lotties, interned hyperlink URIs, and every cell (visible grid + full
// scrollback). Returns 0 on success, -1 if the terminal is invalid.
int terminal_dump_json(FILE *out, TerminalBackend *term,
                       const TerminalDumpMeta *meta);

// Resolve the destination path for a dump. `path` NULL/empty selects the
// default location ($XDG_STATE_HOME/portty/dumps, %LOCALAPPDATA% on Windows;
// overridable by meta->conf->dump_dir or $PORTTY_DUMP_DIR). "-" means stdout.
// Writes a NUL-terminated path into `out`. Returns false on failure.
bool portty_dump_build_path(const TerminalDumpMeta *meta, const char *path,
                            char *out, size_t out_n);

// Write a dump to `path` (NULL = default, "-" = stdout). Creates the
// destination directory, writes atomically (temp file + rename) with 0600
// permissions, and copies the resolved path into `resolved` when non-NULL.
// Returns true on success.
bool portty_dump_write(TerminalBackend *term, const TerminalDumpMeta *meta,
                       const char *path, char *resolved, size_t resolved_n);

#endif // DUMP_H
