/*
 * portty — Shell quoting of dropped file paths interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef PORTTY_SHELL_QUOTE_H
#define PORTTY_SHELL_QUOTE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Quote a string so a shell reads it as one literal word. The result is
 * malloc'd and NUL-terminated; the caller free()s it. NULL input or
 * allocation failure returns NULL. The string is always quoted, even
 * when it contains no special characters, so callers never have to
 * decide whether quoting is needed.
 *
 * Two styles, both testable on any platform:
 *
 * - POSIX: single quotes, with each embedded ' emitted as '\''. Inside
 *   single quotes every byte is literal (backslash, $, `, ;, newline),
 *   so the quoted word survives sh/bash/zsh unchanged.
 * - Windows: double quotes, with each embedded " doubled (""). cmd.exe
 *   and PowerShell both read "" inside a double-quoted string as one
 *   literal quote. This targets those two shells; an MSYS2 bash child
 *   would still expand $ inside double quotes.
 *
 * shell_quote() picks the style for the platform portty is built on. */
char *shell_quote_posix(const char *s);
char *shell_quote_win(const char *s);
char *shell_quote(const char *s);

/* Quote each path with shell_quote() and join the words with single
 * spaces — the paste payload for a (multi-)file drop. malloc'd and
 * NUL-terminated; NULL when n == 0, a path is NULL, or allocation
 * fails. */
char *shell_quote_join(const char *const *paths, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* PORTTY_SHELL_QUOTE_H */
