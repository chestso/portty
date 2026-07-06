/*
 * path_compat.c — MSYS2/Unix to native Windows path conversion
 *
 * No dependency on MSYS2 runtime or SDL. Pure C99 + Win32 API for
 * toupper/tolower only (via ctype.h, which is standard C).
 */
#include "path_compat.h"

#include <ctype.h>
#include <string.h>

/* Standard MSYS2 environment directory prefixes.
 * Each appears as \<prefix>\bin\ in the exe path. */
static const char *msys_env_prefixes[] = {
    "ucrt64",
    "mingw64",
    "clang64",
    "clangarm64",
    "msys",
};

static size_t count_env_prefixes(void)
{
    return sizeof(msys_env_prefixes) / sizeof(msys_env_prefixes[0]);
}

/* Check whether the string at 's' starts with \<prefix>\bin\.
 * 's' must be a backslash-separated path. Returns the length of the
 * matched prefix segment (including the leading backslash), or 0. */
static size_t match_env_bin(const char *s, const char *prefix)
{
    size_t plen = strlen(prefix);
    /* Expect: \<prefix>\bin\ */
    if (s[0] != '\\')
        return 0;
    if (strncmp(s + 1, prefix, plen) != 0)
        return 0;
    if (s[1 + plen] != '\\')
        return 0;
    if (strncmp(s + 1 + plen + 1, "bin\\", 4) != 0)
        return 0;
    return 1 + plen; /* length of "\<prefix>" */
}

bool path_compat_derive_msys_root(const char *exe_path, char *out,
                                  size_t out_size)
{
    if (!exe_path || !out || out_size == 0)
        return false;

    /* Walk through exe_path looking for \<env>\bin\ */
    for (const char *p = exe_path; *p; p++) {
        if (*p != '\\')
            continue;
        for (size_t i = 0; i < count_env_prefixes(); i++) {
            size_t seg_len = match_env_bin(p, msys_env_prefixes[i]);
            if (seg_len > 0) {
                size_t root_len = (size_t)(p - exe_path);
                if (root_len == 0 || root_len >= out_size)
                    return false;
                memcpy(out, exe_path, root_len);
                out[root_len] = '\0';
                return true;
            }
        }
    }
    return false;
}

/* Flip forward slashes to backslashes in-place. */
static void flip_slashes(char *s)
{
    for (; *s; s++)
        if (*s == '/')
            *s = '\\';
}

/* Check whether a path looks like a native Windows path: X:\... */
static bool is_native_win_path(const char *p)
{
    if (!p || !p[0])
        return false;
    return ((p[0] >= 'A' && p[0] <= 'Z') ||
            (p[0] >= 'a' && p[0] <= 'z')) &&
           p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}

bool path_compat_msys_to_win(const char *msys_path, const char *exe_path,
                             char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return false;
    if (!msys_path || !msys_path[0])
        return false;

    /* Case 2: already a native Windows path (C:/... or C:\...) */
    if (is_native_win_path(msys_path)) {
        size_t len = strlen(msys_path);
        if (len >= out_size)
            return false;
        memcpy(out, msys_path, len + 1);
        flip_slashes(out);
        return true;
    }

    /* Must start with / to be an MSYS2 path */
    if (msys_path[0] != '/')
        return false;

    /* Case 1: MSYS2 drive-letter shorthand /c/Users/foo → C:\Users\foo */
    if (msys_path[1] && msys_path[2] == '/' &&
        ((msys_path[1] >= 'a' && msys_path[1] <= 'z') ||
         (msys_path[1] >= 'A' && msys_path[1] <= 'Z'))) {
        size_t len = strlen(msys_path);
        if (len >= out_size)
            return false;
        out[0] = (char)toupper((unsigned char)msys_path[1]);
        out[1] = ':';
        /* Copy the rest starting after /c, i.e. from msys_path[2] (the slash) */
        memcpy(out + 2, msys_path + 2, len - 2 + 1);
        flip_slashes(out);
        return true;
    }

    /* Case 3: bare Unix path like /home/alice — prepend MSYS root */
    char root[1024];
    if (!path_compat_derive_msys_root(exe_path, root, sizeof(root)))
        return false;

    size_t root_len = strlen(root);
    size_t path_len = strlen(msys_path); /* includes leading / */

    /* Special case: "/" → just the root */
    if (path_len == 1) {
        if (root_len >= out_size)
            return false;
        memcpy(out, root, root_len + 1);
        return true;
    }

    /* root + \ + rest-of-path (skip leading /) */
    size_t needed = root_len + 1 + (path_len - 1);
    if (needed >= out_size)
        return false;

    memcpy(out, root, root_len);
    out[root_len] = '\\';
    memcpy(out + root_len + 1, msys_path + 1, path_len - 1 + 1);
    flip_slashes(out);
    return true;
}
