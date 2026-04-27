#!/bin/bash
# build-osxcross.sh - cross-compile bloom-terminal for macOS using osxcross.
#
# Produces build-osxcross/src/bloom-terminal (Mach-O 64-bit). Builds macOS
# dependencies (zlib, libpng, FreeType, HarfBuzz, SDL3) into deps/macos-prefix
# on first run.

set -eu

cd "$(dirname "$0")/.."

OSXCROSS_BUILD_DIR="build-osxcross"
DEPS_DIR="deps"
MACOS_PREFIX="$(pwd)/${DEPS_DIR}/macos-prefix"

# Add local osxcross to PATH if present.
if [ -d "osxcross/target/bin" ]; then
	OSXCROSS_ROOT="$(pwd)/osxcross/target"
	export PATH="${OSXCROSS_ROOT}/bin:$PATH"
	export LD_LIBRARY_PATH="${OSXCROSS_ROOT}/lib:${LD_LIBRARY_PATH:-}"
fi

if ! command -v o64-clang >/dev/null 2>&1; then
	echo "ERROR: osxcross cross-compiler not found." >&2
	echo "Run: ./scripts/setup-osxcross.sh /path/to/Command_Line_Tools.dmg" >&2
	exit 1
fi

OSXCROSS_CC="o64-clang"
OSXCROSS_HOST=$(o64-clang -dumpmachine 2>/dev/null || echo "x86_64-apple-darwin23")
OSXCROSS_AR="${OSXCROSS_HOST}-ar"
if ! command -v "$OSXCROSS_AR" >/dev/null 2>&1; then
	OSXCROSS_AR="$(dirname "$(command -v "$OSXCROSS_CC")")/llvm-ar"
fi

echo "Using cross-compiler: $OSXCROSS_CC (host: $OSXCROSS_HOST)"

if [ ! -f "${MACOS_PREFIX}/lib/pkgconfig/sdl3.pc" ]; then
	echo "Building macOS dependencies (first run, this may take a while)..."
	if [ ! -x "scripts/build-macos-deps.sh" ]; then
		echo "ERROR: scripts/build-macos-deps.sh not found" >&2
		exit 1
	fi
	OSXCROSS_HOST="$OSXCROSS_HOST" MACOS_PREFIX="$MACOS_PREFIX" \
		scripts/build-macos-deps.sh
else
	echo "Using cached macOS dependencies: ${MACOS_PREFIX}/"
fi

[ -f configure ] || ./autogen.sh

rm -rf "$OSXCROSS_BUILD_DIR"
mkdir -p "$OSXCROSS_BUILD_DIR"
cd "$OSXCROSS_BUILD_DIR"

OSXCROSS_CC_ABS=$(command -v "$OSXCROSS_CC")
OSXCROSS_ROOT_DETECTED=$(dirname "$(dirname "$OSXCROSS_CC_ABS")")
RT_LIB="${OSXCROSS_ROOT_DETECTED}/lib/darwin/libclang_rt.osx.a"
if [ ! -f "$RT_LIB" ]; then
	echo "ERROR: compiler-rt not found: $RT_LIB" >&2
	echo "Run: ./scripts/setup-osxcross.sh (it builds compiler-rt automatically)" >&2
	exit 1
fi

../configure \
	--host="$OSXCROSS_HOST" \
	--prefix="$MACOS_PREFIX" \
	--enable-debug \
	CC="$OSXCROSS_CC_ABS" \
	PKG_CONFIG=pkg-config \
	PKG_CONFIG_PATH="${MACOS_PREFIX}/lib/pkgconfig" \
	PKG_CONFIG_LIBDIR="${MACOS_PREFIX}/lib/pkgconfig" \
	LIBS="$RT_LIB"

make -j"$(nproc)"

echo "Cross-compilation complete: ${OSXCROSS_BUILD_DIR}/src/bloom-terminal"
