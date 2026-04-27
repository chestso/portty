#!/bin/sh
# git-version.sh - print the bloom-terminal version string.
#
# Usage: git-version.sh [SRCDIR]
#
# Resolution order:
#   1. git describe --tags --match 'v*' in SRCDIR (v0.1-5-gabc1234 -> 0.1.5-abc1234)
#   2. 0.0.<rev-count>-<short-sha>  if SRCDIR is a git checkout with no tags
#   3. contents of SRCDIR/version   for tarball builds with no .git
#   4. literal "0.0.0-unknown"
set -e

srcdir=${1:-.}

if git -C "$srcdir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if git -C "$srcdir" describe --tags --match 'v*' HEAD >/dev/null 2>&1; then
        git -C "$srcdir" describe --tags --match 'v*' HEAD \
            | sed 's/^v//;s/-\([0-9]*\)-g/.\1-/'
        exit 0
    fi
    count=$(git -C "$srcdir" rev-list --count HEAD 2>/dev/null || echo 0)
    hash=$(git -C "$srcdir" rev-parse --short HEAD 2>/dev/null || echo unknown)
    echo "0.0.${count}-${hash}"
    exit 0
fi

if [ -s "$srcdir/version" ]; then
    cat "$srcdir/version"
    exit 0
fi

echo "0.0.0-unknown"
