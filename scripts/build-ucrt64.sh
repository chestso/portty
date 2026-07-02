#!/bin/bash
# build-ucrt64.sh - build portty natively on Windows (MSYS2 UCRT64).
#
# MSYS2's /usr/bin/sh and /usr/bin/bash are the same binary, but when bash is
# invoked as "sh" (as libtoolize's #!/usr/bin/env sh does) its POSIX-mode
# `test -d` builtin intermittently fails to see /ucrt64 paths — a mount-table
# race that aborts autoreconf with "$pkgauxdir is not a directory". Shadowing
# sh with a bash copy earlier on PATH sidesteps the race so autoreconf works.
#
# Can be launched from any shell (git-bash, cmd, PowerShell, or an MSYS2
# shell); it re-execs into a real MSYS2 UCRT64 shell via msys2_shell.cmd.
#
# Usage:
#   ./scripts/build-ucrt64.sh            # autogen + configure + make + check
#   ./scripts/build-ucrt64.sh --gen-ico  # regenerate the Windows .ico from SVG
#   ./scripts/build-ucrt64.sh --install  # build, then make install
#
# Extra args are forwarded to configure, e.g.:
#   ./scripts/build-ucrt64.sh --enable-release

set -eu

cd "$(dirname "$0")/.."

GEN_ICO=0
DO_INSTALL=0
CONFIGURE_ARGS=()
for arg in "$@"; do
	case "$arg" in
	--gen-ico) GEN_ICO=1 ;;
	--install) DO_INSTALL=1 ;;
	*) CONFIGURE_ARGS+=("$arg") ;;
	esac
done

# If not already inside an MSYS2 UCRT64 shell, re-exec into one via
# msys2_shell.cmd so /ucrt64 is mounted and /usr/bin/pacman resolves.
if [ -z "${MSYSTEM:-}" ] || [ "${MSYSTEM:-}" != "UCRT64" ]; then
	PACMAN_PATH="$(command -v pacman 2>/dev/null || true)"
	if [ -z "$PACMAN_PATH" ]; then
		echo "ERROR: pacman not found on PATH. Install MSYS2 and add its" >&2
		echo "       /usr/bin to PATH, or run this from an MSYS2 UCRT64 shell." >&2
		exit 1
	fi
	# pacman is at <MSYS2_ROOT>/usr/bin/pacman; msys2_shell.cmd is at
	# <MSYS2_ROOT>/msys2_shell.cmd — two directories up from usr/bin.
	PACMAN_DIR="$(cd "$(dirname "$PACMAN_PATH")" && pwd)"
	MSYS2_ROOT="$(cd "$PACMAN_DIR/../.." && pwd)"
	MSYS2_SHELL="$MSYS2_ROOT/msys2_shell.cmd"
	if [ ! -f "$MSYS2_SHELL" ]; then
		echo "ERROR: msys2_shell.cmd not found at $MSYS2_SHELL" >&2
		exit 1
	fi
	REPO_WIN="$(cygpath -w "$(pwd)" 2>/dev/null || echo "$(pwd)")"
	exec cmd.exe //c "$MSYS2_SHELL" -ucrt64 -defterm -no-start -here \
		-c "cd '$REPO_WIN' && ./scripts/build-ucrt64.sh $*"
fi

# --- Inside the MSYS2 UCRT64 shell from here on ------------------------------

echo "==> MSYS2 UCRT64 shell (MSYSTEM=$MSYSTEM)"

# Workaround for the /usr/bin/sh test -d mount race (see header comment).
FIXSH_DIR="$(mktemp -d /tmp/portty-fixsh.XXXXXX)"
cp /usr/bin/bash "$FIXSH_DIR/sh"
export PATH="$FIXSH_DIR:$PATH"
trap 'rm -rf "$FIXSH_DIR"' EXIT
echo "==> Applied sh workaround (shadowed /usr/bin/sh with bash copy)"

# Sanitize ACLOCAL_PATH. When this script is re-execed into the MSYS2
# shell from a Windows parent (git-bash/cmd/PowerShell), ACLOCAL_PATH
# may arrive as a Windows-style value ("C:\...\ucrt64\share\aclocal;C:\
# ...\usr\share\aclocal") — backslashes and ';' separators. MSYS2's
# aclocal (a perl script) splits on ':' (not ';') and can't stat those
# mangled paths, so autoreconf dies with:
#   aclocal-1.18: error: file '/Users/.../usr/share/aclocal/progtest.m4'
#   does not exist
# (the drive-letter prefix gets eaten, leaving a path missing its /c/
# mount). /etc/profile would set the correct POSIX value interactively,
# but non-interactive `msys2_shell.cmd -c` shells inherit the broken
# Windows env verbatim. Convert each element to a POSIX path (cygpath)
# and join with ':'; if anything looks off, fall back to unsetting it —
# aclocal's built-in default (/usr/share/aclocal) is correct on its own.
if [ -n "${ACLOCAL_PATH:-}" ]; then
	sane=""
	IFS=';' read -ra _ac_elems <<<"$ACLOCAL_PATH"
	for _el in "${_ac_elems[@]}"; do
		[ -z "$_el" ] && continue
		if _posix="$(cygpath -u "$_el" 2>/dev/null)"; then
			sane="${sane:+$sane:}$_posix"
		fi
	done
	if [ -n "$sane" ]; then
		export ACLOCAL_PATH="$sane"
		echo "==> Sanitized ACLOCAL_PATH -> $ACLOCAL_PATH"
	else
		unset ACLOCAL_PATH
		echo "==> Unset unusable ACLOCAL_PATH (aclocal defaults apply)"
	fi
fi

# Always regenerate the autotools files. The ACLOCAL_PATH sanitization
# above is what makes this reliable (without it, aclocal scans mangled
# Windows paths and autoreconf dies).
echo "==> autogen.sh"
./autogen.sh

echo "==> configure"
rm -rf build
mkdir build
(cd build && sh ../configure "${CONFIGURE_ARGS[@]}")

if [ "$GEN_ICO" -eq 1 ]; then
	echo "==> make gen-ico"
	make -C build/data gen-ico
	echo "==> Done: $(magick identify data/icons/portty.ico | wc -l) ICO frames"
	exit 0
fi

echo "==> make -j$(nproc)"
make -C build -j"$(nproc)"

echo "==> make check"
make -C build check

if [ "$DO_INSTALL" -eq 1 ]; then
	echo "==> make install"
	make -C build install
fi

echo "==> Build complete: build/src/portty.exe"
