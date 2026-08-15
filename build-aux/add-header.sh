#!/bin/sh
# add-header.sh FILE DESCRIPTION
#
# Prepend the standard portty file banner (SPDX license + copyright) to a
# single UTF-8 source file. The banner is placed as the very first text in
# the file — above existing block comments, #define directives, and include
# guards — per SPDX best practice.
#
# Idempotent: if the file already contains "SPDX-License-Identifier: MIT"
# (anywhere), it is left untouched.
#
# Usage is intentionally bare (no flags): the description passed on the
# command line is embedded verbatim, so a helper can wrap this script with
# its own per-file description table.

set -eu

file=$1
description=$2

if [ -z "$file" ] || [ -z "$description" ]; then
	echo "usage: $0 FILE DESCRIPTION" >&2
	exit 2
fi

if [ ! -f "$file" ]; then
	echo "add-header: no such file: $file" >&2
	exit 1
fi

if grep -q 'SPDX-License-Identifier: MIT' "$file"; then
	exit 0
fi

banner="/*
 * portty — $description
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */"

tmp="${file}.header_tmp"
{
	printf '%s\n' "$banner"
	printf '\n'
	# Preserve the original first line (e.g. "#define _GNU_SOURCE" or a
	# pre-existing comment). 'cat' under set -e is fine here: we only read.
	cat "$file"
} >"$tmp"

mv "$tmp" "$file"
