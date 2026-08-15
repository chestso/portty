/*
 * portty — MSYS2/Unix to native Windows path conversion interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef PATH_COMPAT_H
#define PATH_COMPAT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Derive the MSYS2 installation root from an executable path.
 *
 * Recognises the pattern \<env-prefix>\bin\<name>.exe where <env-prefix>
 * is one of ucrt64, mingw64, clang64, clangarm64, msys (the standard
 * MSYS2 environment directory names). Returns everything before the
 * environment prefix as the root, e.g.:
 *
 *   C:\msys64\ucrt64\bin\portty.exe  →  C:\msys64
 *   D:\msys\mingw64\bin\portty.exe   →  D:\msys
 *
 * Returns false if the pattern is not found.
 */
bool path_compat_derive_msys_root(const char *exe_path, char *out,
                                  size_t out_size);

/*
 * Convert an MSYS2/Unix-style path to a native Windows path.
 *
 * Handles three cases:
 *  1. /c/Users/foo  →  C:\Users\foo   (MSYS2 drive-letter shorthand)
 *  2. C:/Users/foo  →  C:\Users\foo   (already native, just flip slashes)
 *  3. /home/alice   →  C:\msys64\home\alice  (bare Unix → prepend MSYS root)
 *
 * Case 3 uses exe_path to derive the MSYS2 root via
 * path_compat_derive_msys_root. If the root cannot be derived, returns
 * false.
 *
 * Returns true if the output is a valid native Windows path (X:\...),
 * false otherwise.
 */
bool path_compat_msys_to_win(const char *msys_path, const char *exe_path,
                             char *out, size_t out_size);

#endif /* PATH_COMPAT_H */
