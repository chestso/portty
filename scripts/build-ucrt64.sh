#!/bin/bash
# build-ucrt64.sh - build bloom-terminal natively on Windows (MSYS2 UCRT64).
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
FIXSH_DIR="$(mktemp -d /tmp/bloom-fixsh.XXXXXX)"
cp /usr/bin/bash "$FIXSH_DIR/sh"
export PATH="$FIXSH_DIR:$PATH"
trap 'rm -rf "$FIXSH_DIR"' EXIT
echo "==> Applied sh workaround (shadowed /usr/bin/sh with bash copy)"

# Ensure build dependencies are installed. bloom-vt is NOT in the official
# pacman repo — it must be built/installed separately, so only require it via
# pkg-config and omit it from the pacman list.
echo "==> Ensuring build dependencies (pacman -S --needed)"
pacman -S --noconfirm --needed \
	mingw-w64-ucrt-x86_64-gcc \
	mingw-w64-ucrt-x86_64-sdl3 \
	mingw-w64-ucrt-x86_64-freetype \
	mingw-w64-ucrt-x86_64-harfbuzz \
	mingw-w64-ucrt-x86_64-libpng \
	mingw-w64-ucrt-x86_64-autotools 2>&1 | tail -3 || true

if [ "$GEN_ICO" -eq 1 ]; then
	echo "==> Installing icon tools (ImageMagick + librsvg for SVG input)"
	pacman -S --noconfirm --needed \
		mingw-w64-ucrt-x86_64-imagemagick \
		mingw-w64-ucrt-x86_64-librsvg 2>&1 | tail -3 || true
fi

# Always regenerate the autotools files so maintainer-mode rebuilds don't
# trigger aclocal with a broken m4 path mid-build.
echo "==> autoreconf -fi"
autoreconf -fi

echo "==> configure"
rm -rf build
mkdir build
(cd build && sh ../configure --disable-gtk4 "${CONFIGURE_ARGS[@]}")

if [ "$GEN_ICO" -eq 1 ]; then
	echo "==> make gen-ico"
	make -C build/data gen-ico
	echo "==> Done: $(magick identify data/icons/bloom-terminal.ico | wc -l) ICO frames"
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

echo "==> Build complete: build/src/bloom-terminal.exe"
