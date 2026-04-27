#!/bin/bash
# build-mingw64.sh - cross-compile bloom-terminal for Windows using mingw64.
#
# Produces build-mingw64/src/.libs/bloom-terminal.exe with required DLLs
# copied alongside.

set -eu

cd "$(dirname "$0")/.."

MINGW_BUILD_DIR="build-mingw64"
MINGW_HOST="x86_64-w64-mingw32"
MINGW_CC="${MINGW_HOST}-gcc"
MINGW_PKG_CONFIG="${MINGW_HOST}-pkg-config"
MINGW_SYSROOT="/usr/x86_64-w64-mingw32/sys-root/mingw"

if ! command -v "$MINGW_CC" >/dev/null 2>&1; then
	echo "ERROR: mingw64 cross-compiler not found: $MINGW_CC" >&2
	echo "Install with: sudo dnf install mingw64-gcc" >&2
	exit 1
fi

missing=()
for pkg in mingw64-SDL3 mingw64-freetype mingw64-harfbuzz mingw64-libpng; do
	if ! rpm -q "$pkg" >/dev/null 2>&1; then
		missing+=("$pkg")
	fi
done
if [ ${#missing[@]} -gt 0 ]; then
	echo "ERROR: Missing mingw64 packages: ${missing[*]}" >&2
	echo "Install with: sudo dnf install ${missing[*]}" >&2
	exit 1
fi

[ -f configure ] || ./autogen.sh

rm -rf "$MINGW_BUILD_DIR"
mkdir -p "$MINGW_BUILD_DIR"
cd "$MINGW_BUILD_DIR"

../configure \
	--host="$MINGW_HOST" \
	--prefix="$MINGW_SYSROOT" \
	--enable-debug \
	PKG_CONFIG="$MINGW_PKG_CONFIG"

make -j"$(nproc)"

cd ..

DLL_DIR="${MINGW_SYSROOT}/bin"
EXE_DIR="${MINGW_BUILD_DIR}/src/.libs"
DLLS=(
	SDL3.dll
	libfreetype-6.dll
	libharfbuzz-0.dll
	libpng16-16.dll
	zlib1.dll
	libbz2-1.dll
	libgcc_s_seh-1.dll
	libwinpthread-1.dll
	libglib-2.0-0.dll
	libintl-8.dll
	libpcre2-8-0.dll
	iconv.dll
)

echo "Collecting runtime DLLs..."
for dll in "${DLLS[@]}"; do
	if [ -f "${DLL_DIR}/${dll}" ]; then
		cp "${DLL_DIR}/${dll}" "${EXE_DIR}/"
	else
		echo "WARNING: DLL not found: ${DLL_DIR}/${dll}" >&2
	fi
done

echo "Cross-compilation complete: ${EXE_DIR}/bloom-terminal.exe"
