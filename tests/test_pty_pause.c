#include "test_helpers.h"
#include "platform.h"
#include "term.h"
#include <stdlib.h>
#include <string.h>

// ---- Mock platform backend ----

static int mock_pause_count = 0;
static int mock_resume_count = 0;
static int mock_clipboard_set_count = 0;
static char *mock_clipboard_text = NULL;

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
    free(mock_clipboard_text);
    mock_clipboard_text = NULL;
}

// ---- Mock terminal backend ----

static bool mock_altscreen = false;
static int mock_term_rows = 24;
static int mock_term_cols = 80;

static bool mock_is_altscreen(TerminalBackend *term)
{
    (void)term;
    return mock_altscreen;
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
    cell->chars[0] = ' ';
    return 0;
}

static int mock_get_scrollback_cell(TerminalBackend *term, int sb_row,
                                    int col, TerminalCell *cell)
{
    (void)term;
    (void)sb_row;
    (void)col;
    memset(cell, 0, sizeof(*cell));
    cell->chars[0] = ' ';
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

// ---- Tests: pause on alt screen selection ----

// Simulate what main.c on_mouse does for left-click selection start
static void simulate_selection_start(TerminalBackend *term,
                                     PlatformBackend *plat, int row, int col,
                                     TerminalSelectMode mode)
{
    bool in_altscreen = terminal_is_altscreen(term);

    if (terminal_selection_active(term)) {
        platform_resume_pty(plat);
        terminal_selection_clear(term);
    } else {
        terminal_selection_start(term, row, col, mode);
        if (in_altscreen)
            platform_pause_pty(plat);
    }
}

// Simulate what main.c does for right-click copy
static void simulate_copy(TerminalBackend *term, PlatformBackend *plat)
{
    if (terminal_selection_active(term)) {
        char *text = terminal_selection_get_text(term);
        if (text) {
            platform_clipboard_set(plat, text);
            free(text);
        }
        platform_resume_pty(plat);
        terminal_selection_clear(term);
    }
}

// Simulate what main.c on_resize does
static void simulate_resize(TerminalBackend *term, PlatformBackend *plat)
{
    platform_resume_pty(plat);
    terminal_selection_clear(term);
}

static void test_pause_on_altscreen_char_selection(void)
{
    reset_mock_counts();
    mock_altscreen = true;
    TerminalBackend *term = create_mock_term();

    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));
    ASSERT_EQ(mock_pause_count, 1);
    ASSERT_EQ(mock_resume_count, 0);

    destroy_mock_term(term);
}

static void test_pause_on_altscreen_word_selection(void)
{
    reset_mock_counts();
    mock_altscreen = true;
    TerminalBackend *term = create_mock_term();

    terminal_selection_start(term, 5, 10, TERM_SELECT_WORD);
    if (terminal_is_altscreen(term))
        platform_pause_pty(&mock_platform);

    ASSERT_TRUE(terminal_selection_active(term));
    ASSERT_EQ(mock_pause_count, 1);

    destroy_mock_term(term);
}

static void test_pause_on_altscreen_line_selection(void)
{
    reset_mock_counts();
    mock_altscreen = true;
    TerminalBackend *term = create_mock_term();

    terminal_selection_start(term, 5, 10, TERM_SELECT_LINE);
    if (terminal_is_altscreen(term))
        platform_pause_pty(&mock_platform);

    ASSERT_TRUE(terminal_selection_active(term));
    ASSERT_EQ(mock_pause_count, 1);

    destroy_mock_term(term);
}

static void test_no_pause_on_normal_selection(void)
{
    reset_mock_counts();
    mock_altscreen = false;
    TerminalBackend *term = create_mock_term();

    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));
    ASSERT_EQ(mock_pause_count, 0);
    ASSERT_EQ(mock_resume_count, 0);

    destroy_mock_term(term);
}

// ---- Tests: resume on selection clear ----

static void test_resume_on_click_clear(void)
{
    reset_mock_counts();
    mock_altscreen = true;
    TerminalBackend *term = create_mock_term();

    // Start selection → pauses
    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 1);

    // Click again with active selection → clears + resumes
    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_resume_count, 1);
    ASSERT_TRUE(!terminal_selection_active(term));

    destroy_mock_term(term);
}

static void test_resume_on_copy(void)
{
    reset_mock_counts();
    mock_altscreen = true;
    TerminalBackend *term = create_mock_term();

    // Start selection → pauses
    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 1);

    // Right-click copy → resumes
    simulate_copy(term, &mock_platform);
    ASSERT_EQ(mock_resume_count, 1);
    ASSERT_TRUE(!terminal_selection_active(term));

    destroy_mock_term(term);
}

static void test_resume_on_resize(void)
{
    reset_mock_counts();
    mock_altscreen = true;
    TerminalBackend *term = create_mock_term();

    // Start selection → pauses
    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 1);

    // Resize → resumes
    simulate_resize(term, &mock_platform);
    ASSERT_EQ(mock_resume_count, 1);
    ASSERT_TRUE(!terminal_selection_active(term));

    destroy_mock_term(term);
}

// ---- Tests: resume is safe without prior pause ----

static void test_resume_without_pause(void)
{
    reset_mock_counts();
    mock_altscreen = false;
    TerminalBackend *term = create_mock_term();

    // Start selection on normal screen (no pause)
    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 0);

    // Resize → resume called, but backend handles idempotency
    simulate_resize(term, &mock_platform);
    ASSERT_EQ(mock_resume_count, 1);

    destroy_mock_term(term);
}

// ---- Tests: full cycle ----

static void test_full_select_copy_cycle(void)
{
    reset_mock_counts();
    mock_altscreen = true;
    TerminalBackend *term = create_mock_term();

    // Start selection → pause
    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 1);
    ASSERT_EQ(mock_resume_count, 0);
    ASSERT_TRUE(terminal_selection_active(term));

    // Copy → resume
    simulate_copy(term, &mock_platform);
    ASSERT_EQ(mock_resume_count, 1);
    ASSERT_TRUE(!terminal_selection_active(term));

    // Start another selection → pause again
    simulate_selection_start(term, &mock_platform, 8, 3, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 2);

    // Resize → resume
    simulate_resize(term, &mock_platform);
    ASSERT_EQ(mock_resume_count, 2);
    ASSERT_TRUE(!terminal_selection_active(term));

    destroy_mock_term(term);
}

static void test_altscreen_toggle_between_selections(void)
{
    reset_mock_counts();
    TerminalBackend *term = create_mock_term();

    // Select on normal screen → no pause
    mock_altscreen = false;
    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 0);

    // Clear (click dismiss)
    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_resume_count, 1);

    // Switch to alt screen, select → pause
    mock_altscreen = true;
    simulate_selection_start(term, &mock_platform, 5, 10, TERM_SELECT_CHAR);
    ASSERT_EQ(mock_pause_count, 1);

    // Copy to clear
    simulate_copy(term, &mock_platform);
    ASSERT_EQ(mock_resume_count, 2);

    destroy_mock_term(term);
}

// ---- Tests: terminal_process_input clears selection ----

static void test_process_input_clears_selection(void)
{
    TerminalBackend *term = create_mock_term();

    terminal_selection_start(term, 5, 10, TERM_SELECT_CHAR);
    ASSERT_TRUE(terminal_selection_active(term));

    // terminal_process_input clears selection (as implemented in term.c)
    terminal_process_input(term, "hello", 5);
    ASSERT_TRUE(!terminal_selection_active(term));

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

    // Alt screen pause tests
    RUN_TEST(test_pause_on_altscreen_char_selection);
    RUN_TEST(test_pause_on_altscreen_word_selection);
    RUN_TEST(test_pause_on_altscreen_line_selection);
    RUN_TEST(test_no_pause_on_normal_selection);

    // Resume tests
    RUN_TEST(test_resume_on_click_clear);
    RUN_TEST(test_resume_on_copy);
    RUN_TEST(test_resume_on_resize);
    RUN_TEST(test_resume_without_pause);

    // Full cycle tests
    RUN_TEST(test_full_select_copy_cycle);
    RUN_TEST(test_altscreen_toggle_between_selections);

    // Selection cleared by PTY input
    RUN_TEST(test_process_input_clears_selection);

    TEST_SUMMARY();
}
