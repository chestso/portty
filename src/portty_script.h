/*
 * portty — Debug/automation script interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 *
 * The script engine (parser, executor, core vocabulary: wait, wait-for,
 * send/sendln, raw, emit/emit-raw, assert-contains/not-contains,
 * await-exit/assert-exit, resize, render [cells|styles], cursor,
 * dump-sixel, quit) lives in coffer — see coffer's
 * docs/script-engine-design.md. This module wires portty's
 * UI-specific extension commands into it:
 *
 *   screendump, renderdump, dumpstate, record-start, record-stop,
 *   panel, panel_hide, mousemove, mousedown, mouseup, winsize,
 *   display-scale, assert-hover, assert-no-hover, diag
 */

#ifndef PORTTY_SCRIPT_H
#define PORTTY_SCRIPT_H

#include <coffer/script.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "portty_frame_rec.h"
#include "portty_pty.h"
#include "term.h"

#ifdef __cplusplus
extern "C" {
#endif

/* UI extension context. The backend fills this once before
 * portty_script_load() (fields point at backend state); the exec_ext
 * dispatch reads it when a UI command runs. Must outlive the script. */
typedef struct
{
    TerminalBackend *term; /* hover asserts (terminal_hovered_hyperlink) */
    PtyContext *pty;       /* send/sendln/raw + await-exit */
    CfrTerm *cfr_term;     /* the engine grid (term_cfr_get_cfr_term) */

    /* Direct terminal input hook for emit/emit-raw. */
    void (*emit_fn)(void *app, const char *data, size_t len);
    void *app;

    /* Deferred actions the backend executes in its render phase. */
    bool *pending_screendump;
    char *screendump_path_buf; /* 512 bytes */
    bool *pending_renderdump;
    char *renderdump_path_buf; /* 512 bytes */

    /* Synthetic mouse input (NULL = unsupported). */
    void (*mousemove_fn)(void *ud, int x, int y);
    void *mousemove_ud;
    void (*mouse_button_fn)(void *ud, int x, int y, int button,
                            bool pressed, int clicks);
    void *mouse_button_ud;

    /* Notification panels (NULL = unsupported). */
    void (*panel_fn)(void *ud, int id, int col, int row, int cols,
                     int rows, const char *title, const char *body,
                     int level, unsigned int flags);
    void *panel_ud;
    void (*panel_hide_fn)(void *ud, int id);
    void *panel_hide_ud;

    /* resize (PTY + grid) and winsize (window pixels) hooks. */
    void (*resize_fn)(void *ud, int cols, int rows);
    void *resize_ud;
    void (*winsize_fn)(void *ud, int w, int h);
    void *winsize_ud;

    /* Display-scale change (the window moved to a display with another
     * scale factor). */
    void (*display_scale_fn)(void *ud, float scale);
    void *display_scale_ud;

    /* Frame recorder. */
    FrameRecorder *recorder;
    void (*record_start_fn)(void *ud, int fps);
    void (*record_stop_fn)(void *ud);
    void *record_ud;

    /* Terminal-state dump (dumpstate PATH; NULL path = default). */
    bool (*dump_fn)(void *ud, const char *path);
    void *dump_ud;

    /* Diagnostics report in the internal pager (diag). */
    void (*diag_fn)(void *ud);
    void *diag_ud;
} ScriptUIExt;

typedef struct PorttyScript PorttyScript;

/* Load a script and start a run wired to `ext`. portty's extension
 * command names are registered automatically; unknown commands fail
 * the load. Returns NULL on error with "line N: ..." in `err`. */
PorttyScript *portty_script_load(const char *path, ScriptUIExt *ext,
                                 char *err, size_t errlen);

void portty_script_free(PorttyScript *s);

/* One step per frame. Returns the engine status; when it is not
 * CFR_SCRIPT_RUNNING the run is over (done or failed). */
CfrScriptStatus portty_script_step(PorttyScript *s);

/* Milliseconds until the current wait can advance (UINT32_MAX when
 * not waiting), for the event loop's deadline fold. */
uint32_t portty_script_wait_ms(PorttyScript *s);

/* True when the run executed `quit` — the backend should request_quit.
 * (A failed run should also quit: the engine stops at the first
 * failure, so nothing will advance anymore.) */
bool portty_script_quit_requested(const PorttyScript *s);

#ifdef __cplusplus
}
#endif

#endif /* PORTTY_SCRIPT_H */
