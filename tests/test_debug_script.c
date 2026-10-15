/*
 * portty — Debug script tests (coffer engine wiring + UI extensions)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 *
 * The core grammar (escapes, wait-for options, hex payloads, unified
 * buffer search) is covered by coffer's tests/test_script.c; these
 * tests cover portty's side: extension command registration, ext arg
 * parsing (panel, mouse, record-start, dump paths), deferred flags,
 * hover asserts, and the quit/failure status flow.
 */

#include "portty_script.h"
#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Write content to a temp file and return the path (caller must free) */
static char *write_tmp_script(const char *content)
{
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmpdir) == 0)
        return NULL;
    char path[MAX_PATH];
    if (GetTempFileNameA(tmpdir, "portty", 0, path) == 0)
        return NULL;
    FILE *fp = fopen(path, "w");
#else
    char path[] = "/tmp/portty_test_script_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;
    FILE *fp = fdopen(fd, "w");
#endif
    if (!fp) {
#ifndef _WIN32
        close(fd);
#endif
        return NULL;
    }
    fputs(content, fp);
    fclose(fp);
    return strdup(path);
}

static void cleanup_tmp(char *path)
{
    if (path) {
        remove(path);
        free(path);
    }
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

/* ── Fake extension context ── */

typedef struct
{
    ScriptUIExt ext;
    /* capture state */
    bool screendump_pending;
    char screendump_path[512];
    bool renderdump_pending;
    char renderdump_path[512];
    int mouse_x, mouse_y, mouse_moves;
    int btn_x, btn_y, btn_button, btn_clicks, btn_pressed, btn_calls;
    int panel_id, panel_col, panel_row, panel_cols, panel_rows;
    int panel_level, panel_calls;
    unsigned int panel_flags;
    char panel_title[128];
    char panel_body[256];
    int panel_hide_id, panel_hide_calls;
    int win_w, win_h, winsize_calls;
    int resize_cols, resize_rows, resize_calls;
    char dump_path[512];
    int dump_calls;
    bool dump_ret;
    char emitted[1024];
    size_t emitted_len;
} FakeExt;

static void fake_mousemove(void *ud, int x, int y)
{
    FakeExt *f = ud;
    f->mouse_x = x;
    f->mouse_y = y;
    f->mouse_moves++;
}

static void fake_mouse_button(void *ud, int x, int y, int button,
                              bool pressed, int clicks)
{
    FakeExt *f = ud;
    f->btn_x = x;
    f->btn_y = y;
    f->btn_button = button;
    f->btn_pressed = pressed;
    f->btn_clicks = clicks;
    f->btn_calls++;
}

static void fake_panel(void *ud, int id, int col, int row, int cols,
                       int rows, const char *title, const char *body,
                       int level, unsigned int flags)
{
    FakeExt *f = ud;
    f->panel_id = id;
    f->panel_col = col;
    f->panel_row = row;
    f->panel_cols = cols;
    f->panel_rows = rows;
    f->panel_level = level;
    f->panel_flags = flags;
    snprintf(f->panel_title, sizeof(f->panel_title), "%s", title);
    snprintf(f->panel_body, sizeof(f->panel_body), "%s", body);
    f->panel_calls++;
}

static void fake_panel_hide(void *ud, int id)
{
    FakeExt *f = ud;
    f->panel_hide_id = id;
    f->panel_hide_calls++;
}

static void fake_resize(void *ud, int cols, int rows)
{
    FakeExt *f = ud;
    f->resize_cols = cols;
    f->resize_rows = rows;
    f->resize_calls++;
}

static void fake_winsize(void *ud, int w, int h)
{
    FakeExt *f = ud;
    f->win_w = w;
    f->win_h = h;
    f->winsize_calls++;
}

static bool fake_dump(void *ud, const char *path)
{
    FakeExt *f = ud;
    snprintf(f->dump_path, sizeof(f->dump_path), "%s",
             path ? path : "(null)");
    f->dump_calls++;
    return f->dump_ret;
}

static void fake_emit(void *app, const char *data, size_t len)
{
    FakeExt *f = app;
    if (f->emitted_len + len >= sizeof(f->emitted))
        len = sizeof(f->emitted) - f->emitted_len - 1;
    memcpy(f->emitted + f->emitted_len, data, len);
    f->emitted_len += len;
    f->emitted[f->emitted_len] = '\0';
}

static void fake_ext_init(FakeExt *f)
{
    memset(f, 0, sizeof(*f));
    f->dump_ret = true;
    f->ext = (ScriptUIExt){
        .pending_screendump = &f->screendump_pending,
        .screendump_path_buf = f->screendump_path,
        .pending_renderdump = &f->renderdump_pending,
        .renderdump_path_buf = f->renderdump_path,
        .mousemove_fn = fake_mousemove,
        .mousemove_ud = f,
        .mouse_button_fn = fake_mouse_button,
        .mouse_button_ud = f,
        .panel_fn = fake_panel,
        .panel_ud = f,
        .panel_hide_fn = fake_panel_hide,
        .panel_hide_ud = f,
        .resize_fn = fake_resize,
        .resize_ud = f,
        .winsize_fn = fake_winsize,
        .winsize_ud = f,
        .dump_fn = fake_dump,
        .dump_ud = f,
        .emit_fn = fake_emit,
        .app = f,
    };
}

/* Load + step a script to completion (immediate commands only). */
static CfrScriptStatus run_to_end(FakeExt *f, const char *text)
{
    char *path = write_tmp_script(text);
    if (!path)
        return CFR_SCRIPT_FAILED;
    char err[256] = { 0 };
    PorttyScript *s = portty_script_load(path, &f->ext, err, sizeof(err));
    cleanup_tmp(path);
    if (!s)
        return CFR_SCRIPT_FAILED;
    CfrScriptStatus st = CFR_SCRIPT_RUNNING;
    for (int i = 0; i < 100 && st == CFR_SCRIPT_RUNNING; i++)
        st = portty_script_step(s);
    portty_script_free(s);
    return st;
}

/* ── Load errors ── */

static void test_load_empty_file(void)
{
    char *path = write_tmp_script("# comment\n\n   # indented\n");
    ASSERT_NOT_NULL(path);
    FakeExt f;
    fake_ext_init(&f);
    char err[256] = { 0 };
    PorttyScript *s = portty_script_load(path, &f.ext, err, sizeof(err));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_step(s), CFR_SCRIPT_DONE);
    ASSERT_FALSE(portty_script_quit_requested(s));
    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_load_unknown_command_fails(void)
{
    char *path = write_tmp_script("frobnicate foo\n");
    ASSERT_NOT_NULL(path);
    FakeExt f;
    fake_ext_init(&f);
    char err[256] = { 0 };
    PorttyScript *s = portty_script_load(path, &f.ext, err, sizeof(err));
    ASSERT_NULL(s);
    ASSERT_TRUE(strstr(err, "unknown command") != NULL);
    cleanup_tmp(path);
}

static void test_load_missing_file(void)
{
    FakeExt f;
    fake_ext_init(&f);
    char err[256] = { 0 };
    PorttyScript *s = portty_script_load("/nonexistent/path/to/script",
                                         &f.ext, err, sizeof(err));
    ASSERT_NULL(s);
    ASSERT_TRUE(strstr(err, "cannot open") != NULL);
}

static void test_load_ext_names_registered(void)
{
    /* All portty UI commands must load without a parse error. */
    char *path = write_tmp_script(
        "screendump /tmp/a.png\n"
        "renderdump /tmp/b.png\n"
        "dumpstate /tmp/c.txt\n"
        "panel 1 0 0 40 3 \"T\" \"B\"\n"
        "panel_hide 1\n"
        "mousemove 1 2\n"
        "mousedown 1 2\n"
        "mouseup 1 2\n"
        "winsize 800 600\n"
        "assert-no-hover\n"
        "quit\n");
    ASSERT_NOT_NULL(path);
    FakeExt f;
    fake_ext_init(&f);
    char err[256] = { 0 };
    PorttyScript *s = portty_script_load(path, &f.ext, err, sizeof(err));
    if (!s)
        fprintf(stderr, "  load error: %s\n", err);
    ASSERT_NOT_NULL(s);
    portty_script_free(s);
    cleanup_tmp(path);
}

/* dumpgrid/dumprow/dumpcells were deleted (replaced by core render). */
static void test_deleted_dump_commands_rejected(void)
{
    FakeExt f;
    fake_ext_init(&f);
    char err[256] = { 0 };
    char *path = write_tmp_script("dumpgrid\n");
    ASSERT_NOT_NULL(path);
    ASSERT_NULL(portty_script_load(path, &f.ext, err, sizeof(err)));
    cleanup_tmp(path);
    path = write_tmp_script("dumprow 1\n");
    ASSERT_NOT_NULL(path);
    ASSERT_NULL(portty_script_load(path, &f.ext, err, sizeof(err)));
    cleanup_tmp(path);
    path = write_tmp_script("dumpcells 1 0 5\n");
    ASSERT_NOT_NULL(path);
    ASSERT_NULL(portty_script_load(path, &f.ext, err, sizeof(err)));
    cleanup_tmp(path);
}

/* ── Core commands through the wrapper ── */

static void test_wait_paces_steps(void)
{
    char *path = write_tmp_script("wait 0.05\n");
    ASSERT_NOT_NULL(path);
    FakeExt f;
    fake_ext_init(&f);
    char err[256] = { 0 };
    PorttyScript *s = portty_script_load(path, &f.ext, err, sizeof(err));
    ASSERT_NOT_NULL(s);

    ASSERT_EQ(portty_script_step(s), CFR_SCRIPT_RUNNING);
    uint32_t before = portty_script_wait_ms(s);
    ASSERT_TRUE(before > 0 && before <= 60);
    sleep_ms(60);
    ASSERT_EQ(portty_script_step(s), CFR_SCRIPT_DONE);

    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_wait_ms_non_wait_returns_max(void)
{
    char *path = write_tmp_script("send hi\n");
    ASSERT_NOT_NULL(path);
    FakeExt f;
    fake_ext_init(&f);
    char err[256] = { 0 };
    PorttyScript *s = portty_script_load(path, &f.ext, err, sizeof(err));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_wait_ms(s), UINT32_MAX);
    portty_script_free(s);
    cleanup_tmp(path);
}

static void test_emit_reaches_emit_fn(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "emit hello\n"), CFR_SCRIPT_DONE);
    ASSERT_STR_EQ(f.emitted, "hello");
}

static void test_resize_reaches_resize_fn(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "resize 120 40\n"), CFR_SCRIPT_DONE);
    ASSERT_EQ(f.resize_calls, 1);
    ASSERT_EQ(f.resize_cols, 120);
    ASSERT_EQ(f.resize_rows, 40);
}

static void test_quit_sets_flag(void)
{
    FakeExt f;
    fake_ext_init(&f);
    char *path = write_tmp_script("quit\n");
    ASSERT_NOT_NULL(path);
    char err[256] = { 0 };
    PorttyScript *s = portty_script_load(path, &f.ext, err, sizeof(err));
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(portty_script_step(s), CFR_SCRIPT_DONE);
    ASSERT_TRUE(portty_script_quit_requested(s));
    portty_script_free(s);
    cleanup_tmp(path);
}

/* ── Extension commands ── */

static void test_screendump_defers(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "screendump /tmp/shot.png\n"),
              CFR_SCRIPT_DONE);
    ASSERT_TRUE(f.screendump_pending);
    ASSERT_STR_EQ(f.screendump_path, "/tmp/shot.png");
}

static void test_screendump_quoted(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "screendump \"/tmp/my shot.png\"\n"),
              CFR_SCRIPT_DONE);
    ASSERT_STR_EQ(f.screendump_path, "/tmp/my shot.png");
}

static void test_renderdump_defers(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "renderdump /tmp/r.png\n"), CFR_SCRIPT_DONE);
    ASSERT_TRUE(f.renderdump_pending);
    ASSERT_STR_EQ(f.renderdump_path, "/tmp/r.png");
}

static void test_dumpstate_path(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "dumpstate /tmp/state.txt\n"),
              CFR_SCRIPT_DONE);
    ASSERT_EQ(f.dump_calls, 1);
    ASSERT_STR_EQ(f.dump_path, "/tmp/state.txt");
}

static void test_dumpstate_default_path(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "dumpstate\n"), CFR_SCRIPT_DONE);
    ASSERT_STR_EQ(f.dump_path, "(null)");
}

static void test_dumpstate_failure_fails_run(void)
{
    FakeExt f;
    fake_ext_init(&f);
    f.dump_ret = false;
    ASSERT_EQ(run_to_end(&f, "dumpstate /tmp/x\n"), CFR_SCRIPT_FAILED);
}

static void test_mousemove(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "mousemove 12 34\n"), CFR_SCRIPT_DONE);
    ASSERT_EQ(f.mouse_moves, 1);
    ASSERT_EQ(f.mouse_x, 12);
    ASSERT_EQ(f.mouse_y, 34);
}

static void test_mouse_buttons(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "mousedown 12 34\nmouseup 56 78 1 2\n"),
              CFR_SCRIPT_DONE);
    ASSERT_EQ(f.btn_calls, 2);
    ASSERT_EQ(f.btn_button, 1);
    ASSERT_EQ(f.btn_clicks, 2);
    ASSERT_FALSE(f.btn_pressed); /* last call was mouseup */
}

static void test_mouse_button_defaults(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "mousedown 12 34\n"), CFR_SCRIPT_DONE);
    ASSERT_EQ(f.btn_button, 1);
    ASSERT_EQ(f.btn_clicks, 1);
    ASSERT_TRUE(f.btn_pressed);
}

static void test_mouse_bad_args_fail(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "mousedown 12\n"), CFR_SCRIPT_FAILED);
}

static void test_panel(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "panel 1 2 3 40 5 \"Build\" \"All good\"\n"),
              CFR_SCRIPT_DONE);
    ASSERT_EQ(f.panel_calls, 1);
    ASSERT_EQ(f.panel_id, 1);
    ASSERT_EQ(f.panel_col, 2);
    ASSERT_EQ(f.panel_row, 3);
    ASSERT_EQ(f.panel_cols, 40);
    ASSERT_EQ(f.panel_rows, 5);
    ASSERT_STR_EQ(f.panel_title, "Build");
    ASSERT_STR_EQ(f.panel_body, "All good");
    ASSERT_EQ(f.panel_level, 0);
}

static void test_panel_with_level_and_flags(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "panel 2 10 5 30 2 \"Warn\" \"Low disk\" 1 4\n"),
              CFR_SCRIPT_DONE);
    ASSERT_EQ(f.panel_level, 1);
    ASSERT_EQ(f.panel_flags, 4);
}

static void test_panel_escapes_expanded(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "panel 1 0 0 10 2 \"T\" \"line1\\nline2\"\n"),
              CFR_SCRIPT_DONE);
    ASSERT_STR_EQ(f.panel_body, "line1\nline2");
}

static void test_panel_hide(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "panel_hide 7\n"), CFR_SCRIPT_DONE);
    ASSERT_EQ(f.panel_hide_calls, 1);
    ASSERT_EQ(f.panel_hide_id, 7);
}

static void test_winsize(void)
{
    FakeExt f;
    fake_ext_init(&f);
    ASSERT_EQ(run_to_end(&f, "winsize 1024 768\n"), CFR_SCRIPT_DONE);
    ASSERT_EQ(f.win_w, 1024);
    ASSERT_EQ(f.win_h, 768);
}

static void test_record_start_stop(void)
{
    FakeExt f;
    fake_ext_init(&f);
    FrameRecorder *rec = frame_recorder_new();
    ASSERT_NOT_NULL(rec);
    f.ext.recorder = rec;

#ifdef _WIN32
    char dir[MAX_PATH];
    ASSERT_TRUE(GetTempPathA(MAX_PATH, dir) != 0);
    strncat(dir, "portty-rec-test", MAX_PATH - strlen(dir) - 1);
#else
    char dir[] = "/tmp/portty_rec_test_XXXXXX";
    ASSERT_NOT_NULL(mkdtemp(dir));
#endif

    char text[1200];
    snprintf(text, sizeof(text), "record-start %s 12\nrecord-stop\n", dir);
    ASSERT_EQ(run_to_end(&f, text), CFR_SCRIPT_DONE);
    ASSERT_FALSE(rec->recording);
    ASSERT_EQ(rec->target_fps, 12);

    frame_recorder_free(rec);
#ifdef _WIN32
    RemoveDirectoryA(dir);
#else
    char manifest[1200];
    snprintf(manifest, sizeof(manifest), "%s/frames.csv", dir);
    remove(manifest);
    rmdir(dir);
#endif
}

static void test_record_without_recorder_warns_but_passes(void)
{
    FakeExt f;
    fake_ext_init(&f);
    /* recorder NULL: warn + skip, run still completes */
    ASSERT_EQ(run_to_end(&f, "record-start /tmp/x 10\nrecord-stop\n"),
              CFR_SCRIPT_DONE);
}

static void test_assert_no_hover_passes(void)
{
    /* term_stubs' terminal_hovered_hyperlink returns 0 (no hover). */
    FakeExt f;
    fake_ext_init(&f);
    TerminalBackend *term = (TerminalBackend *)&f; /* stub ignores it */
    f.ext.term = term;
    ASSERT_EQ(run_to_end(&f, "assert-no-hover\n"), CFR_SCRIPT_DONE);
}

static void test_assert_hover_fails_run(void)
{
    FakeExt f;
    fake_ext_init(&f);
    f.ext.term = (TerminalBackend *)&f;
    ASSERT_EQ(run_to_end(&f, "assert-hover\n"), CFR_SCRIPT_FAILED);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_load_empty_file);
    RUN_TEST(test_load_unknown_command_fails);
    RUN_TEST(test_load_missing_file);
    RUN_TEST(test_load_ext_names_registered);
    RUN_TEST(test_deleted_dump_commands_rejected);

    RUN_TEST(test_wait_paces_steps);
    RUN_TEST(test_wait_ms_non_wait_returns_max);
    RUN_TEST(test_emit_reaches_emit_fn);
    RUN_TEST(test_resize_reaches_resize_fn);
    RUN_TEST(test_quit_sets_flag);

    RUN_TEST(test_screendump_defers);
    RUN_TEST(test_screendump_quoted);
    RUN_TEST(test_renderdump_defers);
    RUN_TEST(test_dumpstate_path);
    RUN_TEST(test_dumpstate_default_path);
    RUN_TEST(test_dumpstate_failure_fails_run);
    RUN_TEST(test_mousemove);
    RUN_TEST(test_mouse_buttons);
    RUN_TEST(test_mouse_button_defaults);
    RUN_TEST(test_mouse_bad_args_fail);
    RUN_TEST(test_panel);
    RUN_TEST(test_panel_with_level_and_flags);
    RUN_TEST(test_panel_escapes_expanded);
    RUN_TEST(test_panel_hide);
    RUN_TEST(test_winsize);
    RUN_TEST(test_record_start_stop);
    RUN_TEST(test_record_without_recorder_warns_but_passes);
    RUN_TEST(test_assert_no_hover_passes);
    RUN_TEST(test_assert_hover_fails_run);

    TEST_SUMMARY();
}
