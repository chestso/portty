/*
 * portty — Shell quoting of dropped file paths
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#include "shell_quote.h"

#include <stdlib.h>
#include <string.h>

char *shell_quote_posix(const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s);
    size_t quotes = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'')
            quotes++;
    }
    /* Each ' expands to '\'' (4 bytes for 1), plus the two wrappers
     * and the NUL. */
    char *out = malloc(len + 3 * quotes + 3);
    if (!out)
        return NULL;
    size_t j = 0;
    out[j++] = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            memcpy(out + j, "'\\''", 4);
            j += 4;
        } else {
            out[j++] = s[i];
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
}

char *shell_quote_win(const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s);
    size_t quotes = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"')
            quotes++;
    }
    /* Each " doubles, plus the two wrappers and the NUL. */
    char *out = malloc(len + quotes + 3);
    if (!out)
        return NULL;
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '"') {
            memcpy(out + j, "\"\"", 2);
            j += 2;
        } else {
            out[j++] = s[i];
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    return out;
}

char *shell_quote(const char *s)
{
#ifdef _WIN32
    return shell_quote_win(s);
#else
    return shell_quote_posix(s);
#endif
}
