#!/bin/sh
# autogen.sh - regenerate version, then bootstrap the autotools build system.
set -e

srcdir=$(cd "$(dirname "$0")" && pwd)
"$srcdir/build-aux/git-version.sh" "$srcdir" > "$srcdir/version"
echo "Version: $(cat "$srcdir/version")"

exec autoreconf -fi "$@"
