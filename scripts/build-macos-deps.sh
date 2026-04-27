#!/bin/bash
# Cross-compile bloom-terminal dependencies for macOS using osxcross
#
# Required environment variables:
#   OSXCROSS_HOST  - cross-compilation host triple (e.g., x86_64-apple-darwin23)
#   MACOS_PREFIX   - installation prefix for built libraries
#
# Each dependency is built only if its .pc file is missing from MACOS_PREFIX.

set -e
set -u

if [ -z "${OSXCROSS_HOST:-}" ]; then
	echo "ERROR: OSXCROSS_HOST not set" >&2
	exit 1
fi
if [ -z "${MACOS_PREFIX:-}" ]; then
	echo "ERROR: MACOS_PREFIX not set" >&2
	exit 1
fi

CC="${OSXCROSS_HOST}-clang"
CXX="${OSXCROSS_HOST}-clang++"
AR="${OSXCROSS_HOST}-ar"
RANLIB="${OSXCROSS_HOST}-ranlib"
STRIP="${OSXCROSS_HOST}-strip"

# Fall back to llvm tools if host-prefixed versions don't exist
if ! command -v "$AR" &>/dev/null; then
	AR="$(dirname "$(command -v "$CC")")/llvm-ar"
fi
if ! command -v "$RANLIB" &>/dev/null; then
	RANLIB="$(dirname "$(command -v "$CC")")/llvm-ranlib"
fi

# Resolve to absolute paths (cmake needs this)
CC="$(command -v "$CC")"
CXX="$(command -v "$CXX")"
AR="$(command -v "$AR")"
RANLIB="$(command -v "$RANLIB")"

JOBS=$(nproc)
SCRIPT_DIR="$(pwd)"
DEPS_SRC="deps/macos-src"
mkdir -p "$DEPS_SRC" "$MACOS_PREFIX"

export PKG_CONFIG_PATH="${MACOS_PREFIX}/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="${MACOS_PREFIX}/lib/pkgconfig"
export CC="$CC"
export CXX="$CXX"
export AR="$AR"
export RANLIB="$RANLIB"
export CFLAGS="-I${MACOS_PREFIX}/include"
export CXXFLAGS="-I${MACOS_PREFIX}/include"
export LDFLAGS="-L${MACOS_PREFIX}/lib"
export CPPFLAGS="-I${MACOS_PREFIX}/include"

configure_common=(
	--host="$OSXCROSS_HOST"
	--prefix="$MACOS_PREFIX"
	--enable-static
	--disable-shared
)

cmake_common=(
	-DCMAKE_SYSTEM_NAME=Darwin
	-DCMAKE_C_COMPILER="$CC"
	-DCMAKE_CXX_COMPILER="$CXX"
	-DCMAKE_AR="$AR"
	-DCMAKE_RANLIB="$RANLIB"
	-DCMAKE_INSTALL_PREFIX="$MACOS_PREFIX"
	-DCMAKE_PREFIX_PATH="$MACOS_PREFIX"
	-DBUILD_SHARED_LIBS=OFF
)

download() {
	local url="$1" dest="$2"
	if [ ! -f "$dest" ]; then
		echo "Downloading $(basename "$dest")..."
		curl -L -o "$dest" "$url"
	fi
}

# --- zlib ---
build_zlib() {
	local version="1.3.1"
	local tarball="${DEPS_SRC}/zlib-${version}.tar.gz"
	local srcdir="${DEPS_SRC}/zlib-${version}"

	if [ -f "${MACOS_PREFIX}/lib/pkgconfig/zlib.pc" ]; then
		echo "zlib: already built"
		return
	fi

	download "https://github.com/madler/zlib/releases/download/v${version}/zlib-${version}.tar.gz" "$tarball"
	[ -d "$srcdir" ] || tar -xzf "$tarball" -C "$DEPS_SRC"

	cd "$srcdir"
	CC="$CC" AR="$AR" RANLIB="$RANLIB" \
		./configure --prefix="$MACOS_PREFIX" --static
	make -j"$JOBS"
	make install
	cd "$SCRIPT_DIR"
	echo "zlib: done"
}

# --- libpng ---
build_libpng() {
	local version="1.6.43"
	local tarball="${DEPS_SRC}/libpng-${version}.tar.gz"
	local srcdir="${DEPS_SRC}/libpng-${version}"

	if [ -f "${MACOS_PREFIX}/lib/pkgconfig/libpng16.pc" ]; then
		echo "libpng: already built"
		return
	fi

	download "https://download.sourceforge.net/libpng/libpng-${version}.tar.gz" "$tarball"
	[ -d "$srcdir" ] || tar -xzf "$tarball" -C "$DEPS_SRC"

	cd "$srcdir"
	./configure "${configure_common[@]}"
	make -j"$JOBS"
	make install
	cd "$SCRIPT_DIR"
	echo "libpng: done"
}

# --- FreeType ---
build_freetype() {
	local version="2.13.3"
	local tarball="${DEPS_SRC}/freetype-${version}.tar.gz"
	local srcdir="${DEPS_SRC}/freetype-${version}"

	if [ -f "${MACOS_PREFIX}/lib/pkgconfig/freetype2.pc" ]; then
		echo "FreeType: already built"
		return
	fi

	download "https://download.savannah.gnu.org/releases/freetype/freetype-${version}.tar.gz" "$tarball"
	[ -d "$srcdir" ] || tar -xzf "$tarball" -C "$DEPS_SRC"

	cd "$srcdir"
	./configure "${configure_common[@]}" \
		--with-png=yes --with-zlib=yes --with-harfbuzz=no --with-bzip2=no --with-brotli=no \
		ZLIB_CFLAGS="-I${MACOS_PREFIX}/include" \
		ZLIB_LIBS="-L${MACOS_PREFIX}/lib -lz" \
		LIBPNG_CFLAGS="-I${MACOS_PREFIX}/include" \
		LIBPNG_LIBS="-L${MACOS_PREFIX}/lib -lpng16 -lz"
	make -j"$JOBS"
	make install
	cd "$SCRIPT_DIR"
	echo "FreeType: done"
}

# --- HarfBuzz ---
build_harfbuzz() {
	local version="10.1.0"
	local tarball="${DEPS_SRC}/harfbuzz-${version}.tar.xz"
	local srcdir="${DEPS_SRC}/harfbuzz-${version}"

	if [ -f "${MACOS_PREFIX}/lib/pkgconfig/harfbuzz.pc" ]; then
		echo "HarfBuzz: already built"
		return
	fi

	download "https://github.com/harfbuzz/harfbuzz/releases/download/${version}/harfbuzz-${version}.tar.xz" "$tarball"
	[ -d "$srcdir" ] || tar -xJf "$tarball" -C "$DEPS_SRC"

	cd "$srcdir"
	# HarfBuzz uses Meson; fall back to cmake if available
	mkdir -p build-osxcross && cd build-osxcross
	cmake .. "${cmake_common[@]}" \
		-DHB_HAVE_FREETYPE=ON \
		-DHB_HAVE_GLIB=OFF \
		-DHB_HAVE_ICU=OFF \
		-DHB_HAVE_GOBJECT=OFF \
		-DHB_BUILD_SUBSET=OFF \
		-DFREETYPE_INCLUDE_DIRS="${MACOS_PREFIX}/include/freetype2" \
		-DFREETYPE_LIBRARY="${MACOS_PREFIX}/lib/libfreetype.a"
	make -j"$JOBS"
	make install
	cd "$SCRIPT_DIR"
	echo "HarfBuzz: done"
}

# --- SDL3 ---
build_sdl3() {
	local version="3.2.8"
	local tarball="${DEPS_SRC}/SDL3-${version}.tar.gz"
	local srcdir="${DEPS_SRC}/SDL3-${version}"

	if [ -f "${MACOS_PREFIX}/lib/pkgconfig/sdl3.pc" ]; then
		echo "SDL3: already built"
		return
	fi

	download "https://github.com/libsdl-org/SDL/releases/download/release-${version}/SDL3-${version}.tar.gz" "$tarball"
	[ -d "$srcdir" ] || tar -xzf "$tarball" -C "$DEPS_SRC"

	cd "$srcdir"
	mkdir -p build-osxcross && cd build-osxcross
	cmake .. "${cmake_common[@]}" \
		-DSDL_SHARED=OFF \
		-DSDL_STATIC=ON \
		-DSDL_TEST=OFF
	make -j"$JOBS"
	make install
	cd "$SCRIPT_DIR"
	echo "SDL3: done"
}

# Build in dependency order
build_zlib
build_libpng
build_freetype
build_harfbuzz
build_sdl3

echo "All macOS dependencies built in: ${MACOS_PREFIX}/"
