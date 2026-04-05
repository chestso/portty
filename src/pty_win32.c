#ifdef _WIN32

#include "bloom_pty.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct PtyContext
{
    HPCON hpc;
    HANDLE input_write;
    HANDLE output_read;
    HANDLE process;
    HANDLE thread;
    int rows;
    int cols;
};

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

    /* Create the pseudo-console */
    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;

    HRESULT hr = CreatePseudoConsole(size, input_read, output_write,
                                     0, &ctx->hpc);
    if (FAILED(hr)) {
        fprintf(stderr, "ERROR: CreatePseudoConsole failed: 0x%lx\n",
                (unsigned long)hr);
        goto fail;
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
        /* Convert argv to a single command line string */
        MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, cmdline,
                            MAX_PATH * 2);
        for (int i = 1; argv[i]; i++) {
            wcscat(cmdline, L" ");
            WCHAR arg_w[MAX_PATH];
            MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, arg_w,
                                MAX_PATH);
            wcscat(cmdline, arg_w);
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

    /* Spawn the child process */
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
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
    if (ctx->hpc != INVALID_HANDLE_VALUE)
        ClosePseudoConsole(ctx->hpc);
    free(ctx);
    return NULL;
}

void pty_destroy(PtyContext *ctx)
{
    if (!ctx)
        return;

    vlog("PTY destroy\n");

    /* Close pseudo-console first — signals EOF to child */
    if (ctx->hpc != INVALID_HANDLE_VALUE) {
        ClosePseudoConsole(ctx->hpc);
        ctx->hpc = INVALID_HANDLE_VALUE;
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

    DWORD written;
    if (!WriteFile(ctx->input_write, data, (DWORD)len, &written, NULL))
        return -1;
    return (ssize_t)written;
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

    HRESULT hr = ResizePseudoConsole(ctx->hpc, size);
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

int pty_get_child_pid(PtyContext *ctx)
{
    if (!ctx || ctx->process == INVALID_HANDLE_VALUE)
        return -1;
    return (int)GetProcessId(ctx->process);
}

void *pty_get_process_handle(PtyContext *ctx)
{
    if (!ctx)
        return NULL;
    return (void *)ctx->process;
}

#endif /* _WIN32 */
