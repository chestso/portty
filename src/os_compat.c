/*
 * portty — Cross-platform OS helpers: exe path lookup and detached processes
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

/*
 * os_compat.c — cross-platform OS abstraction for exe path resolution
 * and detached process spawning.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "os_compat.h"
#include "common.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#endif

bool os_compat_get_exe_path(char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0)
        return false;

#ifdef _WIN32
    WCHAR wpath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wpath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, buf, (int)bufsize, NULL, NULL);
    return true;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)bufsize;
    if (_NSGetExecutablePath(buf, &size) != 0)
        return false;
    return true;
#else
    ssize_t len = readlink("/proc/self/exe", buf, bufsize - 1);
    if (len <= 0)
        return false;
    buf[len] = '\0';
    return true;
#endif
}

bool os_compat_open_url(const char *url, char *err, size_t errlen)
{
    if (!url || !url[0]) {
        if (err && errlen > 0)
            snprintf(err, errlen, "empty URL");
        return false;
    }

#ifdef _WIN32
    WCHAR wurl[4096];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 4096);
    HINSTANCE r = ShellExecuteW(NULL, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32)
        return true;
    if (err && errlen > 0)
        snprintf(err, errlen, "ShellExecuteW failed");
    return false;
#else
    pid_t pid = fork();
    if (pid < 0) {
        if (err && errlen > 0)
            snprintf(err, errlen, "fork failed: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        setsid();
#if defined(__APPLE__)
        execlp("open", "open", url, (char *)NULL);
#else
        execlp("xdg-open", "xdg-open", url, (char *)NULL);
#endif
        _exit(1);
    }
    return true;
#endif
}

bool os_compat_spawn_process(const char *exe_path, const char *cwd)
{
    if (!exe_path || !exe_path[0])
        return false;

#ifdef _WIN32
    WCHAR wexe[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, exe_path, -1, wexe, MAX_PATH);
    WCHAR wcmdline[MAX_PATH];
    swprintf(wcmdline, MAX_PATH, L"\"%s\"", wexe);
    WCHAR wcwd[MAX_PATH] = L"";
    LPWSTR lpcwd = NULL;
    if (cwd && cwd[0]) {
        char norm[PATH_MAX];
        snprintf(norm, sizeof(norm), "%s", cwd);
        for (char *p = norm; *p; p++)
            if (*p == '/')
                *p = '\\';
        MultiByteToWideChar(CP_UTF8, 0, norm, -1, wcwd, MAX_PATH);
        lpcwd = wcwd;
    }
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessW(wexe, wcmdline, NULL, NULL, FALSE,
                       DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                       NULL, lpcwd, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    vlog("Failed to spawn process: %lu\n", GetLastError());
    return false;
#else
    pid_t pid = fork();
    if (pid < 0) {
        vlog("Failed to fork for new process: %s\n", strerror(errno));
        return false;
    }
    if (pid == 0) {
        setsid();
        if (cwd && cwd[0]) {
            int cr = chdir(cwd);
            (void)cr;
        }
        execl(exe_path, exe_path, NULL);
        _exit(1);
    }
    return true;
#endif
}
