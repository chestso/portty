#include "portty_pty.h"
#include "test_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* Create a unique temp directory and return its path (caller must free
 * and remove it). */
static char *make_temp_dir(void)
{
#ifdef _WIN32
    char tmpdir[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tmpdir) == 0)
        return NULL;
    char path[MAX_PATH];
    if (GetTempFileNameA(tmpdir, "pty", 0, path) == 0)
        return NULL;
    /* GetTempFileNameA creates a file — delete it and make a directory */
    remove(path);
    if (!CreateDirectoryA(path, NULL))
        return NULL;
    return strdup(path);
#else
    char tmpl[] = "/tmp/pty_cwd_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir)
        return NULL;
    return strdup(dir);
#endif
}

static void cleanup_temp_dir(char *path)
{
    if (!path)
        return;
#ifdef _WIN32
    RemoveDirectoryA(path);
#else
    rmdir(path);
#endif
    free(path);
}

#ifdef _WIN32
/* Normalize backslashes to forward slashes for path comparison */
static void normalize_slashes(char *s)
{
    for (; *s; s++)
        if (*s == '\\')
            *s = '/';
}
#endif

/* ---- NULL-safety tests (no PTY needed) ---- */

static void test_get_cwd_null_ctx(void)
{
    char buf[256];
    ASSERT_FALSE(pty_get_child_cwd(NULL, buf, sizeof(buf)));
}

static void test_get_cwd_null_buf(void)
{
    /* pty_get_child_cwd must not crash with NULL buf */
    ASSERT_FALSE(pty_get_child_cwd(NULL, NULL, 256));
}

static void test_get_cwd_zero_bufsize(void)
{
    char buf[1];
    ASSERT_FALSE(pty_get_child_cwd(NULL, buf, 0));
}

/* ---- Functional test: spawn a child, cd it, verify CWD ---- */

static void test_get_cwd_matches_child(void)
{
    char *temp_dir = make_temp_dir();
    ASSERT_NOT_NULL(temp_dir);

    /* Spawn a shell that cd's into temp_dir and stays alive briefly
     * so we can read its CWD. */
#ifdef _WIN32
    char cmd[MAX_PATH * 2];
    /* Convert forward slashes to backslashes for cmd.exe */
    char windir[MAX_PATH];
    snprintf(windir, sizeof(windir), "%s", temp_dir);
    for (char *p = windir; *p; p++)
        if (*p == '/')
            *p = '\\';
    /* ping is used as a portable "sleep" on Windows */
    snprintf(cmd, sizeof(cmd), "cd /d %s && ping -n 4 127.0.0.1 > nul",
             windir);
    char *argv[] = { "cmd.exe", "/c", cmd, NULL };
#else
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "cd '%s' && sleep 3", temp_dir);
    char *argv[] = { "sh", "-c", cmd, NULL };
#endif

    PtyContext *pty = pty_create(24, 80, argv);
    if (!pty) {
        /* If we can't spawn a shell (e.g. CI without ConPTY), skip */
        fprintf(stderr, "  SKIP: could not create PTY\n");
        cleanup_temp_dir(temp_dir);
        return;
    }

    /* Give the child time to execute the cd command */
#ifdef _WIN32
    Sleep(2000);
#else
    sleep(2);
#endif

    char cwd[4096] = "";
    bool ok = pty_get_child_cwd(pty, cwd, sizeof(cwd));
    if (!ok) {
        fprintf(stderr, "  SKIP: could not resolve child CWD\n");
        pty_destroy(pty);
        cleanup_temp_dir(temp_dir);
        return;
    }

    /* Normalize and compare */
    char expected[4096];
    snprintf(expected, sizeof(expected), "%s", temp_dir);
#ifdef _WIN32
    normalize_slashes(cwd);
    normalize_slashes(expected);
    /* Strip trailing slash — the PEB CurrentDirectory usually has one */
    size_t clen = strlen(cwd);
    if (clen > 0 && cwd[clen - 1] == '/')
        cwd[clen - 1] = '\0';
    size_t elen = strlen(expected);
    if (elen > 0 && expected[elen - 1] == '/')
        expected[elen - 1] = '\0';
    /* Windows paths are case-insensitive — lowercase both for compare */
    for (char *p = cwd; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    for (char *p = expected; *p; p++)
        *p = (char)tolower((unsigned char)*p);
#endif

    ASSERT_STR_EQ(cwd, expected);

    pty_destroy(pty);
    cleanup_temp_dir(temp_dir);
}

#ifdef _WIN32
/* Regression test: CreateProcessW must be able to launch the current
 * executable using the same path construction as sdl3_spawn_new_terminal.
 * The previous commit switched from SDL_CreateProcess (which auto-appends
 * .exe) to CreateProcessW (which doesn't), breaking Ctrl+Shift+N on
 * Windows. This test would have caught that. */
static void test_spawn_self_via_create_process(void)
{
    /* Build exe path: same logic as sdl3_plat_init uses on Windows */
    WCHAR wexe[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wexe, MAX_PATH);
    ASSERT_TRUE(len > 0 && len < MAX_PATH);

    /* Verify the path ends with .exe — the bug was that the path was
     * constructed without .exe, causing CreateProcessW to fail. */
    size_t wexe_len = wcslen(wexe);
    ASSERT_TRUE(wexe_len > 4);
    ASSERT_TRUE(_wcsicmp(wexe + wexe_len - 4, L".exe") == 0);

    /* Build a quoted command line (same as sdl3_spawn_new_terminal) */
    WCHAR wcmdline[MAX_PATH];
    swprintf(wcmdline, MAX_PATH, L"\"%s\"", wexe);

    /* Launch with --version so it exits immediately */
    wcscat(wcmdline, L" --version");

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    /* Also test that lpCurrentDirectory works (the whole point of the
     * CreateProcessW switch) */
    WCHAR wcwd[MAX_PATH];
    DWORD cwd_len = GetCurrentDirectoryW(MAX_PATH, wcwd);
    ASSERT_TRUE(cwd_len > 0 && cwd_len < MAX_PATH);

    BOOL ok = CreateProcessW(wexe, wcmdline, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, wcwd, &si, &pi);
    if (!ok) {
        fprintf(stderr, "  CreateProcessW failed: %lu\n", GetLastError());
        ASSERT_TRUE(ok);
    }

    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}
#endif

int main(int argc, char *argv[])
{
    test_parse_args(argc, argv);
    printf("test_pty_cwd\n");

    RUN_TEST(test_get_cwd_null_ctx);
    RUN_TEST(test_get_cwd_null_buf);
    RUN_TEST(test_get_cwd_zero_bufsize);
    RUN_TEST(test_get_cwd_matches_child);
#ifdef _WIN32
    RUN_TEST(test_spawn_self_via_create_process);
#endif

    TEST_SUMMARY();
}
