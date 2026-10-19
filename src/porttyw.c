/*
 * portty — Windowed launcher: starts portty.exe with its console hidden
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * porttyw.exe is the windowed entry point on Windows: a GUI-subsystem binary
 * whose whole job is to start the console-subsystem portty.exe with the same
 * command line, hiding the console Windows creates for it. Explorer
 * double-clicks, Start Menu shortcuts and file associations point here;
 * shells, scripts and pipelines call portty.exe directly, where the console
 * subsystem is what makes cmd wait for the process and report its exit status.
 * The "w" postfix is the established convention (emacsclientw.exe, runemacs).
 *
 * Three properties carry the contract:
 *
 *  - It never replaces a standard handle. It hands the ones it was given to the
 *    child unchanged, so `porttyw -V | grep` and `porttyw -V > f.txt` behave
 *    exactly like portty.exe does.
 *  - It always waits, and exits with the child's status, so a batch script
 *    sees the real %errorlevel%.
 *  - It holds the child in a kill-on-close job object, assigned before the
 *    child's main thread is resumed, so killing the launcher cannot leave an
 *    orphaned terminal behind.
 *
 * It carries no application logic — no config, no fonts, no terminal, no link
 * against anything but Win32.
 */

#include <windows.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>

/* Long enough for a \\?\ extended-length module path. */
#define PORTTY_PATH_MAX 32768

#define PORTTY_CHILD_EXE L"portty.exe"

/* Taskbar grouping: the app and its launcher are one entry. The explicit ID is
 * inherited by child processes, so the terminal portty.exe creates below
 * carries it too. */
#define PORTTY_APP_USER_MODEL_ID L"chestso.portty"

/* A GUI-subsystem process has no stderr to complain into, so a failed launch
 * has to say so in a dialog. */
static int launcher_fail(void)
{
    MessageBoxW(NULL, L"Could not start portty.", L"portty", MB_ICONSTOP | MB_SETFOREGROUND);
    return 1;
}

/* The launcher's own command line with the program name removed, so every
 * portty flag reaches the app verbatim through lpCommandLine. */
static const wchar_t *args_after_program_name(const wchar_t *cmdline)
{
    const wchar_t *p = cmdline;

    while (*p == L' ' || *p == L'\t')
        p++;
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"')
            p++;
        if (*p == L'"')
            p++;
    } else {
        while (*p && *p != L' ' && *p != L'\t')
            p++;
    }
    while (*p == L' ' || *p == L'\t')
        p++;
    return p;
}

/* KILL_ON_JOB_CLOSE so the launcher's death takes the terminal with it;
 * SILENT_BREAKAWAY_OK so portty's own child (the shell) is not trapped in the
 * job — portty tears that one down itself. */
static HANDLE create_kill_on_close_job(void)
{
    HANDLE job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;

    if (!job)
        return NULL;

    memset(&info, 0, sizeof(info));
    info.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

/* Resolve a handle the child can actually inherit: NULL when the launcher has
 * none (a double-click leaves the standard handles unset), the handle itself
 * when it carries the inherit flag, and an inheritable duplicate otherwise
 * (recorded in *owned so the caller can close it after the spawn). */
static HANDLE inheritable_handle(HANDLE h, HANDLE *owned)
{
    DWORD flags = 0;

    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return NULL;
    if (!GetHandleInformation(h, &flags))
        return NULL;
    if (flags & HANDLE_FLAG_INHERIT)
        return h;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), owned, 0, TRUE,
                         DUPLICATE_SAME_ACCESS))
        return NULL;
    return *owned;
}

int main(void)
{
    wchar_t module[PORTTY_PATH_MAX];
    wchar_t *child_path;
    wchar_t *cmdline;
    const wchar_t *rest;
    wchar_t *sep = NULL;
    size_t child_len, rest_len, i;
    DWORD status = 0;
    HANDLE job;
    HANDLE owned[3] = { NULL, NULL, NULL };
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    (void)SetCurrentProcessExplicitAppUserModelID(PORTTY_APP_USER_MODEL_ID);

    DWORD n = GetModuleFileNameW(NULL, module, PORTTY_PATH_MAX);
    if (n == 0 || n >= PORTTY_PATH_MAX)
        return launcher_fail();

    /* The app binary is this launcher's sibling. */
    for (wchar_t *p = module; *p; p++) {
        if (*p == L'\\' || *p == L'/')
            sep = p;
    }
    if (!sep)
        return launcher_fail();
    sep[1] = L'\0';

    child_path = malloc((wcslen(module) + wcslen(PORTTY_CHILD_EXE) + 1) * sizeof(wchar_t));
    if (!child_path)
        return launcher_fail();
    wcscpy(child_path, module);
    wcscat(child_path, PORTTY_CHILD_EXE);
    child_len = wcslen(child_path);

    /* "<dir>\portty.exe" <original args> — the path is quoted in case it has
     * spaces; the args keep whatever quoting they arrived with. */
    rest = args_after_program_name(GetCommandLineW());
    rest_len = wcslen(rest);
    cmdline = malloc((child_len + rest_len + 4) * sizeof(wchar_t));
    if (!cmdline) {
        free(child_path);
        return launcher_fail();
    }
    i = 0;
    cmdline[i++] = L'"';
    wcscpy(cmdline + i, child_path);
    i = child_len + 1;
    cmdline[i++] = L'"';
    if (rest_len) {
        cmdline[i++] = L' ';
        wcscpy(cmdline + i, rest);
        i += rest_len;
    }
    cmdline[i] = L'\0';

    job = create_kill_on_close_job();

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    /* SW_HIDE: the console Windows creates for the console-subsystem child is
     * never shown. The character counts keep that hidden console's screen
     * buffer small instead of the user's (possibly huge) default. */
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USECOUNTCHARS | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.dwXCountChars = 80;
    si.dwYCountChars = 25;
    /* STARTF_USESTDHANDLES with the launcher's own handles is what keeps the
     * caller's pipe, redirect or console attached to the child. Inheriting them
     * is not enough on its own: Windows creates a fresh console for a
     * console-subsystem child whose parent has none, and installs that
     * console's handles in place of the caller's — so `porttyw -V | grep` and
     * `porttyw -V > f.txt` would come up empty. Handing the same handles over
     * by value is a no-op replacement plus the guarantee that routing is kept.
     * A double-click leaves all three unset, and the child then gets the hidden
     * console described above. */
    si.hStdInput = inheritable_handle(GetStdHandle(STD_INPUT_HANDLE), &owned[0]);
    si.hStdOutput = inheritable_handle(GetStdHandle(STD_OUTPUT_HANDLE), &owned[1]);
    si.hStdError = inheritable_handle(GetStdHandle(STD_ERROR_HANDLE), &owned[2]);

    /* bInheritHandles: the handles set above have to be inherited to stay valid
     * in the child. CREATE_SUSPENDED: the child goes into the job before it can
     * spawn or run anything. */
    if (!CreateProcessW(child_path, cmdline, NULL, NULL, TRUE,
                        NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        if (job)
            CloseHandle(job);
        for (i = 0; i < 3; i++) {
            if (owned[i])
                CloseHandle(owned[i]);
        }
        free(cmdline);
        free(child_path);
        return launcher_fail();
    }
    for (i = 0; i < 3; i++) {
        if (owned[i])
            CloseHandle(owned[i]);
    }
    free(cmdline);
    free(child_path);

    /* A sandbox or a job hierarchy can refuse the assignment; the terminal is
     * still worth running, so only the kill-on-close guarantee is lost. Nothing
     * is assigned to the job in that case, so closing it here is safe. */
    if (job && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = NULL;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    /* Always wait: the launcher's lifetime is the terminal's, and the wait is
     * what makes %errorlevel% real for whoever started it. */
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &status))
        status = 1;
    CloseHandle(pi.hProcess);
    if (job)
        CloseHandle(job);

    /* The CRT turns this int back into the exit code word, so the child's
     * status survives the round trip unchanged. */
    return (int)status;
}
