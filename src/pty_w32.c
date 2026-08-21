/*
 * portty — Windows ConPTY pseudo-terminal (PTY) management
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifdef _WIN32

#include "common.h"
#include "portty_pty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#ifndef PSEUDOCONSOLE_PASSTHROUGH_MODE
#define PSEUDOCONSOLE_PASSTHROUGH_MODE 0x8
#endif

struct PtyContext
{
    HPCON hpc;
    HANDLE input_write;
    HANDLE output_read;
    HANDLE process;
    HANDLE thread;
    HANDLE waiter_thread;
    int rows;
    int cols;
    bool use_bundled_conpty; /* true if hpc was created via conpty.dll */
};

/* ── conpty.dll sideloading ───────────────────────────────────────────
 *
 * The system CreatePseudoConsole (kernel32.dll) always launches the
 * OS-level conhost.exe, whose legacy VtEngine strips DCS sequences
 * (used for sixel graphics).  The ConPTY rewrite (Windows Terminal
 * PR #17510) that fixes this is in conpty.dll + OpenConsole.exe from
 * the Microsoft.Windows.Console.ConPTY NuGet package.
 *
 * When conpty.dll is bundled alongside portty.exe, it automatically
 * finds OpenConsole.exe in the same directory and uses it instead of
 * the system conhost.exe.  We load CreatePseudoConsole etc. from
 * conpty.dll at runtime, falling back to kernel32.dll if the DLL
 * is not present. */

typedef HRESULT(WINAPI *CreatePseudoConsole_t)(COORD, HANDLE, HANDLE,
                                               DWORD, HPCON *);
typedef HRESULT(WINAPI *ResizePseudoConsole_t)(HPCON, COORD);
typedef void(WINAPI *ClosePseudoConsole_t)(HPCON);

static CreatePseudoConsole_t fn_CreatePseudoConsole = NULL;
static ResizePseudoConsole_t fn_ResizePseudoConsole = NULL;
static ClosePseudoConsole_t fn_ClosePseudoConsole = NULL;
static HMODULE g_conpty_dll = NULL;
static bool g_conpty_initialized = false;

static void init_conpty(void)
{
    if (g_conpty_initialized)
        return;
    g_conpty_initialized = true;

    /* Look for conpty.dll alongside portty.exe */
    wchar_t exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    if (!len || len >= MAX_PATH)
        return;

    wchar_t *slash = wcsrchr(exe_path, L'\\');
    if (!slash)
        return;
    slash[1] = L'\0';
    wcscat(exe_path, L"conpty.dll");

    g_conpty_dll = LoadLibraryW(exe_path);
    if (!g_conpty_dll)
        return;

    fn_CreatePseudoConsole = (CreatePseudoConsole_t)
        GetProcAddress(g_conpty_dll, "CreatePseudoConsole");
    fn_ResizePseudoConsole = (ResizePseudoConsole_t)
        GetProcAddress(g_conpty_dll, "ResizePseudoConsole");
    fn_ClosePseudoConsole = (ClosePseudoConsole_t)
        GetProcAddress(g_conpty_dll, "ClosePseudoConsole");

    if (!fn_CreatePseudoConsole || !fn_ResizePseudoConsole ||
        !fn_ClosePseudoConsole) {
        FreeLibrary(g_conpty_dll);
        g_conpty_dll = NULL;
        fn_CreatePseudoConsole = NULL;
        fn_ResizePseudoConsole = NULL;
        fn_ClosePseudoConsole = NULL;
        return;
    }

    vlog("ConPTY: using bundled conpty.dll + OpenConsole.exe\n");
}

/* Check if a bundled conpty.dll (with OpenConsole.exe) is available. */
static bool has_bundled_conpty(void)
{
    init_conpty();
    return g_conpty_dll != NULL;
}

/* Dispatch close to the correct implementation (conpty.dll or system). */
static void pty_close_hpc(PtyContext *ctx)
{
    if (ctx->hpc == INVALID_HANDLE_VALUE)
        return;
    if (ctx->use_bundled_conpty && fn_ClosePseudoConsole)
        fn_ClosePseudoConsole(ctx->hpc);
    else
        ClosePseudoConsole(ctx->hpc);
    ctx->hpc = INVALID_HANDLE_VALUE;
}

/* Dispatch resize to the correct implementation (conpty.dll or system). */
static HRESULT pty_resize_hpc(PtyContext *ctx, COORD size)
{
    if (ctx->use_bundled_conpty && fn_ResizePseudoConsole)
        return fn_ResizePseudoConsole(ctx->hpc, size);
    return ResizePseudoConsole(ctx->hpc, size);
}

/* Waiter thread: when the child process exits, close the pseudo-console
 * so that ReadFile on the output pipe returns instead of blocking. */
static DWORD WINAPI pty_waiter_thread(LPVOID param)
{
    PtyContext *ctx = (PtyContext *)param;
    WaitForSingleObject(ctx->process, INFINITE);
    vlog("PTY waiter: child exited, closing pseudo-console\n");
    pty_close_hpc(ctx);
    return 0;
}

int pty_signal_init(void)
{
    /* No signal pipe on Windows — child exit detected via process handle */
    return 0;
}

void pty_signal_cleanup(void)
{
}

int pty_signal_get_fd(void)
{
    return -1;
}

void pty_signal_drain(void)
{
}

PtyContext *pty_create(int rows, int cols, char *const argv[])
{
    PtyContext *ctx = calloc(1, sizeof(PtyContext));
    if (!ctx) {
        fprintf(stderr, "ERROR: Failed to allocate PTY context\n");
        return NULL;
    }
    ctx->rows = rows;
    ctx->cols = cols;
    ctx->hpc = INVALID_HANDLE_VALUE;
    ctx->input_write = INVALID_HANDLE_VALUE;
    ctx->output_read = INVALID_HANDLE_VALUE;
    ctx->process = INVALID_HANDLE_VALUE;
    ctx->thread = INVALID_HANDLE_VALUE;

    /* Create pipes for ConPTY I/O */
    HANDLE input_read = INVALID_HANDLE_VALUE;
    HANDLE output_write = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&input_read, &ctx->input_write, NULL, 0)) {
        fprintf(stderr, "ERROR: CreatePipe (input) failed: %lu\n",
                GetLastError());
        goto fail;
    }

    if (!CreatePipe(&ctx->output_read, &output_write, NULL, 0)) {
        fprintf(stderr, "ERROR: CreatePipe (output) failed: %lu\n",
                GetLastError());
        goto fail;
    }

    /* Create the pseudoconsole.  Try the bundled conpty.dll first
     * (which uses OpenConsole.exe with the VtEngine rewrite that
     * passes DCS/sixel sequences through).  Fall back to the system
     * CreatePseudoConsole (kernel32.dll) which uses conhost.exe. */
    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    DWORD pty_flags = PSEUDOCONSOLE_PASSTHROUGH_MODE;

    init_conpty();
    if (fn_CreatePseudoConsole) {
        HRESULT hr = fn_CreatePseudoConsole(size, input_read,
                                            output_write, pty_flags,
                                            &ctx->hpc);
        if (SUCCEEDED(hr)) {
            ctx->use_bundled_conpty = true;
            vlog("ConPTY: conpty.dll pseudoconsole created (passthrough)\n");
        } else {
            vlog("ConPTY: conpty.dll failed (0x%lx), falling back\n",
                 (unsigned long)hr);
        }
    }

    if (!ctx->use_bundled_conpty) {
        vlog("ConPTY: requesting passthrough mode (flags=0x%lx)\n",
             (unsigned long)pty_flags);
        HRESULT hr = CreatePseudoConsole(size, input_read, output_write,
                                         pty_flags, &ctx->hpc);
        if (FAILED(hr)) {
            vlog("ConPTY: passthrough failed (0x%lx), retrying without\n",
                 (unsigned long)hr);
            hr = CreatePseudoConsole(size, input_read, output_write,
                                     0, &ctx->hpc);
        } else {
            vlog("ConPTY: passthrough mode accepted\n");
        }
        if (FAILED(hr)) {
            fprintf(stderr, "ERROR: CreatePseudoConsole failed: 0x%lx\n",
                    (unsigned long)hr);
            goto fail;
        }
    }

    /* ConPTY now owns these pipe ends — close our copies */
    CloseHandle(input_read);
    input_read = INVALID_HANDLE_VALUE;
    CloseHandle(output_write);
    output_write = INVALID_HANDLE_VALUE;

    /* Set up STARTUPINFOEX with the pseudo-console attribute */
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
    si.StartupInfo.hStdError = INVALID_HANDLE_VALUE;

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    si.lpAttributeList =
        (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_size);
    if (!si.lpAttributeList) {
        fprintf(stderr, "ERROR: Failed to allocate attribute list\n");
        goto fail;
    }

    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0,
                                           &attr_size)) {
        fprintf(stderr,
                "ERROR: InitializeProcThreadAttributeList failed: %lu\n",
                GetLastError());
        free(si.lpAttributeList);
        goto fail;
    }

    if (!UpdateProcThreadAttribute(
            si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            ctx->hpc, sizeof(HPCON), NULL, NULL)) {
        fprintf(stderr, "ERROR: UpdateProcThreadAttribute failed: %lu\n",
                GetLastError());
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
        goto fail;
    }

    /* Build command line */
    WCHAR cmdline[MAX_PATH * 2];
    if (argv && argv[0]) {
        /* .cmd/.bat scripts cannot be executed directly by
         * CreateProcessW — they must be invoked through cmd.exe.
         * Detect the extension and prepend "cmd.exe /c " so that
         * e.g. "portty -- msys2_shell.cmd -ucrt64" works. */
        const char *ext = strrchr(argv[0], '.');
        int is_cmd_script = ext &&
                            (_stricmp(ext, ".cmd") == 0 || _stricmp(ext, ".bat") == 0);

        WCHAR *p = cmdline;
        if (is_cmd_script) {
            const char *comspec = getenv("COMSPEC");
            if (!comspec)
                comspec = "cmd.exe";
            MultiByteToWideChar(CP_UTF8, 0, comspec, -1, p,
                                MAX_PATH * 2);
            p += wcslen(p);
            wcscpy(p, L" /c ");
            p += 4;
        }

        MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, p,
                            (MAX_PATH * 2) - (p - cmdline));
        p += wcslen(p);
        for (int i = 1; argv[i]; i++) {
            *p++ = L' ';
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, p,
                                (MAX_PATH * 2) - (p - cmdline));
            p += wcslen(p);
        }
    } else {
        /* Default shell: use COMSPEC (usually cmd.exe) */
        const char *comspec = getenv("COMSPEC");
        if (!comspec)
            comspec = "cmd.exe";
        MultiByteToWideChar(CP_UTF8, 0, comspec, -1, cmdline,
                            MAX_PATH * 2);
    }

    vlog("PTY: spawning '%ls'\n", cmdline);

    /* Build an explicit Unicode environment block for the child process.
     * Inheriting the parent environment (lpEnvironment=NULL) causes ConPTY
     * child processes to exit immediately on Windows 11 — the MSYS2 parent
     * environment contains Unix-style paths and variables that confuse the
     * Windows process. Instead, snapshot the current environment via
     * GetEnvironmentStringsW and append our terminal-specific overrides
     * (TERM, COLORTERM, TERM_PROGRAM), then pass the block with
     * CREATE_UNICODE_ENVIRONMENT — the same approach Windows Terminal uses.
     *
     * GetEnvironmentStringsW returns a double-null-terminated block in the
     * format KEY=VALUE\0KEY=VALUE\0\0, which is exactly what
     * CreateProcessW expects when CREATE_UNICODE_ENVIRONMENT is set. We
     * copy it and prepend our overrides so they take precedence (Windows
     * uses the first occurrence of each variable name). */
    LPWCH parent_env = GetEnvironmentStringsW();
    WCHAR envBlock[65536];
    WCHAR *ep = envBlock;

    /* Prepend terminal-specific overrides (first occurrence wins) */
    {
        static const WCHAR *overrides[] = {
            L"TERM=portty-vty-256color",
            L"COLORTERM=truecolor",
            L"TERM_PROGRAM=ghostty",
            /* Inject PROMPT_COMMAND so bash/zsh emit OSC 7 on every
             * directory change. This allows Ctrl+Shift+N to open the
             * new terminal in the shell's CWD. The PEB-walk approach
             * was removed because ReadProcessMemory fails for ConPTY
             * children (ERROR_PARTIAL_COPY). First-occurrence wins in
             * the env block, so our override shadows any parent value. */
            L"PROMPT_COMMAND=printf \"\\033]7;file://%s\\007\" \"$PWD\"",
            /* Inject PROMPT for cmd.exe so it emits OSC 9;9 on every
             * prompt. The sequence \x1b]9;9;"<path>"\x1b\\ is the
             * ConEmu CWD protocol. $P expands to the current path,
             * $e is ESC, and the trailing $S$P$G restores the default
             * path+> prompt after the OSC. Non-cmd shells ignore the
             * PROMPT variable. */
            L"PROMPT=$e]9;9;\"$P\"$e\\$S$P$G",
            /* CHERE_INVOKING tells MSYS2 bash to start in the current
             * directory instead of cd'ing to $HOME. Without this, Ctrl
             * +Shift+N spawns a new terminal that always opens in $HOME
             * regardless of the working directory passed to
             * CreateProcessW. */
            L"CHERE_INVOKING=1",
        };
        for (int i = 0; i < 6; i++) {
            size_t len = wcslen(overrides[i]);
            if (ep + len + 1 >= envBlock + 65536)
                break;
            wmemcpy(ep, overrides[i], len + 1);
            ep += len + 1;
        }
    }

#ifdef PORTTY_DATADIR
    /* Build TERMINFO_DIRS so MSYS2 programs can find our terminfo.
     * MSYS2's ncurses (msys-ncursesw6.dll) uses ':' as the path
     * separator and expects native Windows backslash paths.  An
     * empty trailing ':' tells ncurses to also check the
     * compiled-in default paths. */
    {
        char buf[8192];
        const char *home = getenv("HOME");
        const char *existing = getenv("TERMINFO_DIRS");

        /* Convert forward slashes to backslashes in PORTTY_DATADIR
         * (configure emits MSYS2-style /c/... paths) */
        char datadir_bs[4096];
        snprintf(datadir_bs, sizeof(datadir_bs), "%s", PORTTY_DATADIR);
        for (char *p = datadir_bs; *p; p++)
            if (*p == '/')
                *p = '\\';

        /* HOME may already have backslashes on Windows */
        if (existing) {
            snprintf(buf, sizeof(buf),
                     "%s\\terminfo:%s\\.terminfo:%s",
                     datadir_bs, home ? home : "", existing);
        } else {
            snprintf(buf, sizeof(buf),
                     "%s\\terminfo:%s\\.terminfo:",
                     datadir_bs, home ? home : "");
        }
        WCHAR wbuf[8192];
        size_t wlen = mbstowcs(wbuf, buf, sizeof(wbuf) / sizeof(wbuf[0]));
        if (wlen > 0 && wlen < sizeof(wbuf) / sizeof(wbuf[0])) {
            /* Prepend TERMINFO_DIRS= */
            static const WCHAR prefix[] = L"TERMINFO_DIRS=";
            size_t prefix_len = wcslen(prefix);
            if (ep + prefix_len + wlen + 1 < envBlock + 65536) {
                wmemcpy(ep, prefix, prefix_len);
                ep += prefix_len;
                wmemcpy(ep, wbuf, wlen + 1);
                ep += wlen + 1;
            }
        }
    }
#endif

    /* Copy parent environment, skipping overrides we already set */
    if (parent_env) {
        LPWCH p = parent_env;
        while (*p) {
            size_t len = wcslen(p);
            if (ep + len + 1 >= envBlock + 65536)
                break;
            if (wcsncmp(p, L"TERMINFO_DIRS=", 14) == 0 ||
                wcsncmp(p, L"PROMPT_COMMAND=", 15) == 0 ||
                wcsncmp(p, L"PROMPT=", 7) == 0 ||
                wcsncmp(p, L"CHERE_INVOKING=", 15) == 0 ||
                wcsncmp(p, L"_PORTTY_DETACHED=", 17) == 0) {
                p += len + 1;
                continue;
            }
            wmemcpy(ep, p, len + 1);
            ep += len + 1;
            p += len + 1;
        }
        FreeEnvironmentStringsW(parent_env);
    }
    *ep = L'\0'; /* double-null terminator */

    /* Spawn the child process */
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT |
                            CREATE_UNICODE_ENVIRONMENT,
                        envBlock, NULL,
                        &si.StartupInfo, &pi)) {
        fprintf(stderr, "ERROR: CreateProcessW failed: %lu\n",
                GetLastError());
        DeleteProcThreadAttributeList(si.lpAttributeList);
        free(si.lpAttributeList);
        goto fail;
    }

    ctx->process = pi.hProcess;
    ctx->thread = pi.hThread;

    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);

    /* Start waiter thread to close console when child exits */
    ctx->waiter_thread =
        CreateThread(NULL, 0, pty_waiter_thread, ctx, 0, NULL);

    vlog("PTY created: pid=%lu, size=%dx%d\n",
         (unsigned long)pi.dwProcessId, cols, rows);

    return ctx;

fail:
    if (input_read != INVALID_HANDLE_VALUE)
        CloseHandle(input_read);
    if (output_write != INVALID_HANDLE_VALUE)
        CloseHandle(output_write);
    if (ctx->input_write != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->input_write);
    if (ctx->output_read != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->output_read);
    pty_close_hpc(ctx);
    free(ctx);
    return NULL;
}

void pty_destroy(PtyContext *ctx)
{
    if (!ctx)
        return;

    vlog("PTY destroy\n");

    /* Close pseudo-console (waiter thread may have already done this) */
    pty_close_hpc(ctx);

    /* Wait for waiter thread to finish */
    if (ctx->waiter_thread != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(ctx->waiter_thread, 2000);
        CloseHandle(ctx->waiter_thread);
    }

    /* Wait briefly for child to exit */
    if (ctx->process != INVALID_HANDLE_VALUE) {
        if (WaitForSingleObject(ctx->process, 500) != WAIT_OBJECT_0) {
            TerminateProcess(ctx->process, 1);
            WaitForSingleObject(ctx->process, 1000);
        }
        CloseHandle(ctx->process);
    }
    if (ctx->thread != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->thread);
    if (ctx->input_write != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->input_write);
    if (ctx->output_read != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->output_read);

    free(ctx);
}

ssize_t pty_write(PtyContext *ctx, const char *data, size_t len)
{
    if (!ctx || ctx->input_write == INVALID_HANDLE_VALUE || !data ||
        len == 0)
        return -1;

    // Loop in case WriteFile returns a short count (matches the POSIX
    // path's behavior — pastes larger than the pipe buffer need to be
    // delivered completely).
    size_t total = 0;
    while (total < len) {
        DWORD written = 0;
        DWORD chunk = (DWORD)((len - total) > 0x7FFFFFFFu ? 0x7FFFFFFFu : (len - total));
        if (!WriteFile(ctx->input_write, data + total, chunk, &written, NULL))
            return total > 0 ? (ssize_t)total : -1;
        if (written == 0)
            break;
        total += (size_t)written;
    }
    return (ssize_t)total;
}

ssize_t pty_read(PtyContext *ctx, char *buf, size_t bufsize)
{
    if (!ctx || ctx->output_read == INVALID_HANDLE_VALUE || !buf ||
        bufsize == 0)
        return -1;

    DWORD bytes_read;
    if (!ReadFile(ctx->output_read, buf, (DWORD)bufsize, &bytes_read,
                  NULL))
        return bytes_read > 0 ? (ssize_t)bytes_read : -1;
    return (ssize_t)bytes_read;
}

int pty_resize(PtyContext *ctx, int rows, int cols)
{
    if (!ctx || ctx->hpc == INVALID_HANDLE_VALUE)
        return -1;

    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;

    HRESULT hr = pty_resize_hpc(ctx, size);
    if (FAILED(hr)) {
        /* Wine returns E_NOTIMPL (0x80004001) — not fatal */
        vlog("ResizePseudoConsole returned 0x%lx (may be unimplemented)\n",
             (unsigned long)hr);
        ctx->rows = rows;
        ctx->cols = cols;
        return 0;
    }

    ctx->rows = rows;
    ctx->cols = cols;
    vlog("PTY resized to %dx%d\n", cols, rows);
    return 0;
}

bool pty_is_running(PtyContext *ctx)
{
    if (!ctx || ctx->process == INVALID_HANDLE_VALUE)
        return false;

    DWORD result = WaitForSingleObject(ctx->process, 0);
    if (result == WAIT_OBJECT_0) {
        vlog("PTY child exited\n");
        return false;
    }
    return true;
}

int pty_get_master_fd(PtyContext *ctx)
{
    (void)ctx;
    return -1;
}

void *pty_get_process_handle(PtyContext *ctx)
{
    if (!ctx)
        return NULL;
    return (void *)ctx->process;
}

void pty_close_console(PtyContext *ctx)
{
    if (!ctx)
        return;
    pty_close_hpc(ctx);
}

/* Check whether the ConPTY host passes DCS (Device Control String)
 * sequences through unmodified.  The system conhost.exe strips DCS
 * (used for sixel graphics) even with PSEUDOCONSOLE_PASSTHROUGH_MODE.
 * The ConPTY rewrite (PR #17510) that fixes this is in the bundled
 * conpty.dll + OpenConsole.exe from the Microsoft.Windows.Console.ConPTY
 * NuGet package.
 *
 * Returns true if a bundled conpty.dll is loaded (which uses
 * OpenConsole.exe with the fix), false if only the system conhost.exe
 * is used. */
bool pty_conpty_dcs_passthrough(void)
{
    return has_bundled_conpty();
}

const char *pty_conpty_host_name(void)
{
    return has_bundled_conpty()
               ? "conpty.dll + OpenConsole.exe"
               : "system conhost.exe";
}

#endif /* _WIN32 */
