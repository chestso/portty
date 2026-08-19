#!/bin/sh
# add-header.sh FILE DESCRIPTION
#
# Prepend the standard portty file banner (SPDX license + copyright) to a
# single UTF-8 source file. The banner is placed as the very first text in
# the file — above existing block comments, #define directives, and include
# guards — per SPDX best practice.
#
# If the file already has a banner (contains "SPDX-License-Identifier: MIT"),
# the description line is updated in-place to match DESCRIPTION. This keeps
# descriptions in sync with descriptions.tsv across renames and edits.
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

banner="/*
 * portty — $description
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */"

if ! grep -q 'SPDX-License-Identifier: MIT' "$file"; then
	# No banner yet — prepend.
	tmp="${file}.header_tmp"
	{
		printf '%s\n' "$banner"
		printf '\n'
		# Preserve the original first line (e.g. "#define _GNU_SOURCE" or a
		# pre-existing comment). 'cat' under set -e is fine here: we only read.
		cat "$file"
	} >"$tmp"
	mv "$tmp" "$file"
	exit 0
fi

# Banner exists — update the description line if stale.
# The description lives on the second line of the banner:
#   line 1: /*
#   line 2:  * portty — <description>
#   line 3:  *
#   line 4:  * SPDX-License-Identifier: MIT
#   line 5:  * Copyright (c) 2026 Thomas Christensen
#   line 6:  */
current=$(sed -n '2p' "$file")
want=" * portty — $description"
if [ "$current" = "$want" ]; then
	exit 0
fi
# Replace line 2 in-place.
tmp="${file}.header_tmp"
sed "2s|.*|$want|" "$file" >"$tmp"
mv "$tmp" "$file"
