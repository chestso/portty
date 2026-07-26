#ifndef PORTTY_SCRIPT_H
#define PORTTY_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DBG_CMD_WAIT,
    DBG_CMD_SEND,
    DBG_CMD_SENDLN,
    DBG_CMD_RAW,
    DBG_CMD_EMIT,
    DBG_CMD_EMIT_RAW,
    DBG_CMD_ASSERT_CONTAINS,
    DBG_CMD_ASSERT_NOT_CONTAINS,
    DBG_CMD_SCREENDUMP,
    DBG_CMD_DUMPROW,
    DBG_CMD_DUMPCELLS,
    DBG_CMD_DUMPVERTS,
    DBG_CMD_VERIFYBUF,
    DBG_CMD_NOTIFY,
    DBG_CMD_MOUSEMOVE,
    DBG_CMD_RESIZE,
    DBG_CMD_WINSIZE,
    DBG_CMD_RECORD_START,
    DBG_CMD_RECORD_STOP,
    DBG_CMD_QUIT,
} DebugCmdType;

typedef struct
{
    DebugCmdType type;
    /* WAIT */
    double wait_seconds;
    /* SEND / RAW / ASSERT_* */
    char *text; /* heap-allocated, freed by script_free */
    /* DUMPROW / DUMPCELLS / DUMPVERTS / VERIFYBUF */
    int row;
    int col_start; /* -1 = all columns (for dumprow) */
    int col_end;
    /* SCREENDUMP */
    char path[512];
    /* MOUSEMOVE */
    int mouse_x;
    int mouse_y;
    /* RESIZE */
    int resize_cols;
    int resize_rows;
    /* WINSIZE — set window pixel size via SDL_SetWindowSize */
    int winsize_w;
    int winsize_h;
    /* NOTIFY */
    char notify_title[128];
    char notify_body[256];
    /* RECORD_START */
    char record_dir[512];
    int record_fps;
} DebugCmd;

typedef struct PorttyScript PorttyScript;

/* Load and parse a script file. Returns NULL on error. */
PorttyScript *portty_script_load(const char *path);

/* Free a script and all its DebugCmd text strings. Safe to call on NULL. */
void portty_script_free(PorttyScript *s);

/* Get the number of parsed commands. */
int portty_script_count(const PorttyScript *s);

/* Get a command by index (0-based). Returns NULL if out of range. */
const DebugCmd *portty_script_get(const PorttyScript *s, int index);

/* Get a human-readable error message if load() failed.
 * Returns NULL if no error or if s is NULL. */
const char *portty_script_error(const PorttyScript *s);

/* ── Shared execution helpers ── */

#include "portty_backend.h"
#include "portty_frame_rec.h"
#include "portty_pty.h"
#include "term.h"

/* Monotonic time helper (no existing utility in the codebase) */
double portty_debug_now_seconds(void); /* clock_gettime(CLOCK_MONOTONIC) */

/* Grid scan for assert-contains / assert-not-contains.
 * Searches all visible rows (and current scrollback view if scroll_offset > 0)
 * for the needle string. Returns true if found. */
bool portty_debug_grid_contains(TerminalBackend *term, int rows, int cols,
                                const char *needle);

/* Context for the shared step function. Each backend fills this with
 * its own state pointers. NULL pointers mean the backend doesn't support
 * that deferred action (e.g. SDL3 passes NULL for verifybuf). */
typedef struct
{
    PorttyBackend *backend;
    TerminalBackend *term;
    PtyContext *pty;
    int scroll_offset;
    /* Direct terminal input hook. If set, the emit/emitln/emit-raw commands
     * feed data straight to the VT engine via terminal_process_input.
     * NULL means emit is unsupported (warn + skip). */
    void (*emit_fn)(void *app, const char *data, size_t len);
    void *emit_user_data;
    /* Deferred action flags — backend points these at its own fields: */
    bool *pending_screendump;
    char *screendump_path_buf; /* backend's destination buffer */
    bool *pending_verifybuf;
    int *verify_row, *verify_col_start, *verify_col_end;
    /* Vertex dump callback (Sokol only, NULL on SDL3) */
    void (*dumpverts_fn)(int row, int col_start, int col_end);
    /* Mouse move callback (NULL if unsupported) */
    void (*mousemove_fn)(void *app, int x, int y);
    void *mousemove_user_data;
    /* Notification callback (NULL if unsupported) */
    void (*notify_fn)(void *backend, const char *title, const char *body);
    void *notify_user_data;
    /* Resize callback (NULL if unsupported) */
    void (*resize_fn)(void *user_data, int cols, int rows);
    void *resize_user_data;
    /* Window size callback — sets pixel size via SDL_SetWindowSize,
     * triggering the real compositor resize path (NULL if unsupported) */
    void (*winsize_fn)(void *user_data, int w, int h);
    void *winsize_user_data;
    /* Frame recorder context */
    FrameRecorder *recorder;
    bool *pending_record_frame;
    /* Timer lifecycle callbacks (backend-specific).
     * Called by portty_script_step() when RECORD_START/STOP
     * commands are processed. The backend creates/removes the timer
     * here, not in frame_recorder_start/stop(). */
    void (*record_start_fn)(void *user_data, int fps);
    void (*record_stop_fn)(void *user_data);
    void *record_user_data;
} DebugExecCtx;

/* Execute one script step. Called each frame by the backend's main loop.
 * Handles wait/send/raw/assert/dumpcells/dumprow.
 * Sets *pending_screendump / *pending_verifybuf for deferred commands
 * that need GL access (screendump, verifybuf).
 * When pending pointers are NULL, unsupported commands print a warning
 * and are skipped.
 * Advances *cmd_index; when all commands are done, sets *cmd_index to
 * count (caller can detect completion by comparing). */
void portty_script_step(PorttyScript *script,
                        int *cmd_index,
                        DebugExecCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PORTTY_SCRIPT_H */
