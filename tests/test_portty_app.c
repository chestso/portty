#include "test_helpers.h"
#include "../src/portty_app.h"
#include "../src/portty_backend.h"
#include "../src/common.h"

float portty_text_gamma = 1.0f;
float portty_text_contrast = 0.0f;
bool portty_notification_transparent = false;

// Stub implementations for the backend functions called by the extracted
// callbacks. Avoids linking platform.c/pty.c and their heavy dependencies.
typedef struct
{
    bool pause_called;
    bool resume_called;
    char clipboard_text[64];
    const char *working_dir;
} StubPlatState;

static StubPlatState g_state = { 0 };
static PorttyBackend g_stub_backend = { 0 };

static void backend_pause_pty(PorttyBackend *self)
{
    (void)self;
    g_state.pause_called = true;
}

static void backend_resume_pty(PorttyBackend *self)
{
    (void)self;
    g_state.resume_called = true;
}

static bool backend_clipboard_set(PorttyBackend *self, const char *text)
{
    (void)self;
    strncpy(g_state.clipboard_text, text, sizeof(g_state.clipboard_text) - 1);
    g_state.clipboard_text[sizeof(g_state.clipboard_text) - 1] = '\0';
    return true;
}

static void backend_set_working_dir(PorttyBackend *self, const char *dir)
{
    (void)self;
    g_state.working_dir = dir;
}

static void reset_state(void)
{
    g_state = (StubPlatState){ 0 };
    g_stub_backend = (PorttyBackend){
        .pause_pty = backend_pause_pty,
        .resume_pty = backend_resume_pty,
        .clipboard_set = backend_clipboard_set,
        .set_working_dir = backend_set_working_dir,
    };
}

static void test_selection_change_pauses_and_resumes(void)
{
    reset_state();
    PorttyApp app = { 0 };
    app.backend = &g_stub_backend;

    portty_app_selection_change(true, &app);
    ASSERT_TRUE(g_state.pause_called);
    ASSERT_FALSE(g_state.resume_called);

    portty_app_selection_change(false, &app);
    ASSERT_TRUE(g_state.resume_called);
}

static void test_clipboard_set_forwards_text(void)
{
    reset_state();
    PorttyApp app = { 0 };
    app.backend = &g_stub_backend;

    portty_app_clipboard_set("hello", 5, &app);
    ASSERT_STR_EQ(g_state.clipboard_text, "hello");
}

static void test_cwd_change_forwards_dir(void)
{
    reset_state();
    PorttyApp app = { 0 };
    app.backend = &g_stub_backend;

    portty_app_cwd_change("/tmp", &app);
    ASSERT_STR_EQ(g_state.working_dir, "/tmp");
}

static void test_term_output_to_pty_writes(void)
{
    reset_state();
    // NULL app/pty should not crash.
    portty_app_term_output_to_pty("x", 1, NULL);

    PorttyApp app = { 0 };
    portty_app_term_output_to_pty("x", 1, &app);
    ASSERT_TRUE(true);
}

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);

    RUN_TEST(test_selection_change_pauses_and_resumes);
    RUN_TEST(test_clipboard_set_forwards_text);
    RUN_TEST(test_cwd_change_forwards_dir);
    RUN_TEST(test_term_output_to_pty_writes);

    TEST_SUMMARY();
}
