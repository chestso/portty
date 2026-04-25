#include "test_helpers.h"
#include "platform.h"
#include "term.h"
#include <stdlib.h>
#include <string.h>

// ---- Mock platform backend ----

static int mock_pause_count = 0;
static int mock_resume_count = 0;
static int mock_clipboard_set_count = 0;

static void mock_pause_pty(PlatformBackend *plat)
{
    (void)plat;
    mock_pause_count++;
}

static void mock_resume_pty(PlatformBackend *plat)
{
    (void)plat;
    mock_resume_count++;
}

static bool mock_clipboard_set(PlatformBackend *plat, const char *text)
{
    (void)plat;
    (void)text;
    mock_clipboard_set_count++;
    return true;
}

static PlatformBackend mock_platform = {
    .name = "mock",
    .pause_pty = mock_pause_pty,
    .resume_pty = mock_resume_pty,
    .clipboard_set = mock_clipboard_set,
};

// Platform backend with no pause/resume (NULL function pointers)
static PlatformBackend mock_platform_no_pause = {
    .name = "mock_no_pause",
};

static void reset_mock_counts(void)
{
    mock_pause_count = 0;
    mock_resume_count = 0;
    mock_clipboard_set_count = 0;
}

// ---- Mock terminal backend ----

static int mock_term_rows = 24;
static int mock_term_cols = 80;

static bool mock_is_altscreen(TerminalBackend *term)
{
    (void)term;
    return false;
}

static int mock_get_dimensions(TerminalBackend *term, int *rows, int *cols)
{
    (void)term;
    *rows = mock_term_rows;
    *cols = mock_term_cols;
    return 0;
}

static int mock_get_cell(TerminalBackend *term, int row, int col,
                         TerminalCell *cell)
{
    (void)term;
    (void)row;
    (void)col;
    memset(cell, 0, sizeof(*cell));
    cell->cp = ' ';
    return 0;
}

static int mock_get_scrollback_cell(TerminalBackend *term, int sb_row,
                                    int col, TerminalCell *cell)
{
    (void)term;
    (void)sb_row;
    (void)col;
    memset(cell, 0, sizeof(*cell));
    cell->cp = ' ';
    return 0;
}

static int mock_get_scrollback_lines(TerminalBackend *term)
{
    (void)term;
    return 0;
}

static int mock_process_input(TerminalBackend *term, const char *input,
                              size_t len)
{
    (void)term;
    (void)input;
    return (int)len;
}

static TerminalBackend mock_term_backend = {
    .name = "mock_term",
    .is_altscreen = mock_is_altscreen,
    .get_dimensions = mock_get_dimensions,
    .get_cell = mock_get_cell,
    .get_scrollback_cell = mock_get_scrollback_cell,
    .get_scrollback_lines = mock_get_scrollback_lines,
    .process_input = mock_process_input,
};

static TerminalBackend *create_mock_term(void)
{
    TerminalBackend *term = calloc(1, sizeof(TerminalBackend));
    *term = mock_term_backend;
    return term;
}

static void destroy_mock_term(TerminalBackend *term)
{
    free(term->selection.word_chars);
    free(term);
}

// ---- Selection change callback tracking ----

static int cb_active_count = 0;
static int cb_inactive_count = 0;
static bool cb_last_active = false;

static void tracking_selection_cb(bool active, void *user_data)
{
    (void)user_data;
    cb_last_active = active;
    if (active)
        cb_active_count++;
    else
        cb_inactive_count++;
}

// Mock callback that mirrors main.c on_selection_change
static void mock_selection_cb(bool active, void *user_data)
{
    (void)user_data;
    if (active)
        mock_pause_count++;
    else
        mock_resume_count++;
}

static void reset_cb_counts(void)
{
    cb_active_count = 0;
    cb_inactive_count = 0;
    cb_last_active = false;
}

// ---- Tests: platform.c wrapper delegation ----

static void test_wrapper_pause_delegates(void)
{
    reset_mock_counts();
    platform_pause_pty(&mock_platform);
    ASSERT_EQ(mock_pause_count, 1);
    platform_pause_pty(&mock_platform);
    ASSERT_EQ(mock_pause_count, 2);
}

static void test_wrapper_resume_delegates(void)
{
    reset_mock_counts();
    platform_resume_pty(&mock_platform);
    ASSERT_EQ(mock_resume_count, 1);
    platform_resume_pty(&mock_platform);
    ASSERT_EQ(mock_resume_count, 2);
}

static void test_wrapper_null_plat(void)
{
    // Should not crash
    platform_pause_pty(NULL);
    platform_resume_pty(NULL);
}

static void test_wrapper_null_fn_ptr(void)
{
    // Backend with NULL pause_pty/resume_pty — should not crash
    platform_pause_pty(&mock_platform_no_pause);
    platform_resume_pty(&mock_platform_no_pause);
}

// ---- Tests: process_input selection behavior ----

static void test_process_input_clears_selection(void)
{
    TerminalBackend *term = create_mock_term();

    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));

    terminal_process_input(term, "hello", 5);
    ASSERT_TRUE(!terminal_selection_active(term));

    destroy_mock_term(term);
}

// ---- Tests: selection change callback ----

static void test_callback_fires_on_selection_start(void)
{
    reset_cb_counts();
    TerminalBackend *term = create_mock_term();
    terminal_set_selection_callback(term, tracking_selection_cb, NULL);

    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(cb_active_count, 1);
    ASSERT_EQ(cb_last_active, true);

    destroy_mock_term(term);
}

static void test_callback_fires_on_selection_clear(void)
{
    reset_cb_counts();
    TerminalBackend *term = create_mock_term();
    terminal_set_selection_callback(term, tracking_selection_cb, NULL);

    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    terminal_selection_clear(term);
    ASSERT_EQ(cb_inactive_count, 1);
    ASSERT_EQ(cb_last_active, false);

    destroy_mock_term(term);
}

static void test_callback_not_fired_on_redundant_clear(void)
{
    reset_cb_counts();
    TerminalBackend *term = create_mock_term();
    terminal_set_selection_callback(term, tracking_selection_cb, NULL);

    // No selection active — clear should be a no-op
    terminal_selection_clear(term);
    ASSERT_EQ(cb_inactive_count, 0);

    destroy_mock_term(term);
}

// ---- Tests: callback-based pause/resume integration ----

static void test_callback_pause_resume_integration(void)
{
    reset_mock_counts();
    TerminalBackend *term = create_mock_term();
    terminal_set_selection_callback(term, mock_selection_cb, NULL);

    // Start selection → callback pauses PTY
    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 1);

    // Clear selection → callback resumes PTY
    terminal_selection_clear(term);
    ASSERT_EQ(mock_resume_count, 1);

    destroy_mock_term(term);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    printf("test_pty_pause\n");

    // Wrapper delegation tests
    RUN_TEST(test_wrapper_pause_delegates);
    RUN_TEST(test_wrapper_resume_delegates);
    RUN_TEST(test_wrapper_null_plat);
    RUN_TEST(test_wrapper_null_fn_ptr);

    // process_input selection behavior
    RUN_TEST(test_process_input_clears_selection);

    // Selection change callback
    RUN_TEST(test_callback_fires_on_selection_start);
    RUN_TEST(test_callback_fires_on_selection_clear);
    RUN_TEST(test_callback_not_fired_on_redundant_clear);

    // Callback-based pause/resume integration
    RUN_TEST(test_callback_pause_resume_integration);

    TEST_SUMMARY();
}
