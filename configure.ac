AC_INIT([bloom-terminal],
        m4_esyscmd_s([cat version 2>/dev/null || echo "0.0.0-unknown"]))
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([1.14 foreign subdir-objects -Wall -Werror])
AM_SILENT_RULES([yes])
AC_CANONICAL_HOST

dnl Recompute the version at configure time so a rebuild after new commits
dnl picks up the current version without needing to re-run autoreconf. Falls
dnl back to PACKAGE_VERSION (set at autoreconf time via m4_esyscmd_s above)
dnl for tarball builds where build-aux/git-version.sh has nothing to read.
BLOOM_TERMINAL_VERSION=`"$srcdir/build-aux/git-version.sh" "$srcdir" 2>/dev/null`
AS_IF([test -z "$BLOOM_TERMINAL_VERSION"],
      [BLOOM_TERMINAL_VERSION="$PACKAGE_VERSION"])
AC_SUBST([BLOOM_TERMINAL_VERSION])
AC_MSG_NOTICE([bloom-terminal version: $BLOOM_TERMINAL_VERSION])

# Check for programs
user_cflags="$CFLAGS"
AC_PROG_CC
AM_PROG_AR
LT_INIT([dlopen])
AC_PROG_INSTALL
AC_PROG_MAKE_SET

# glslangValidator is only needed to regenerate the committed GPU glyph-shader
# header (src/shaders/glyph_coverage_frag_spv.h) after editing the .glsl via
# `make regen-shaders`. Normal builds compile the committed header, so this is
# an optional maintainer tool, not a build dependency.
AC_PATH_PROG([GLSLANGVALIDATOR], [glslangValidator])
AM_CONDITIONAL([HAVE_GLSLANGVALIDATOR], [test -n "$GLSLANGVALIDATOR"])

# Detect cross-compilation target
case "$host_os" in
  mingw*|msys*) host_windows=yes; host_macos=no ;;
  darwin*)      host_windows=no;  host_macos=yes ;;
  *)            host_windows=no;  host_macos=no ;;
esac
AM_CONDITIONAL([HOST_WINDOWS], [test "x$host_windows" = "xyes"])
AM_CONDITIONAL([HOST_MACOS], [test "x$host_macos" = "xyes"])

# dlopen support (POSIX only)
if test "x$host_windows" = "xno"; then
  AC_SEARCH_LIBS([dlopen], [dl])
fi

# Build mode selection (--enable-release > --enable-debug > default ASan)
AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug],
    [unsanitized debug build (-O0 -g3, no ASan)])],
  [enable_debug=$enableval],
  [enable_debug=no])

AC_ARG_ENABLE([release],
  [AS_HELP_STRING([--enable-release],
    [optimized release build (-O3 -DNDEBUG)])],
  [enable_release=$enableval],
  [enable_release=no])

if test "x$enable_release" = "xyes"; then
  CFLAGS="-O3 -DNDEBUG"
  AC_MSG_NOTICE([Release mode: CFLAGS set to -O3 -DNDEBUG])
elif test "x$enable_debug" = "xyes"; then
  CFLAGS="-O0 -g3"
  AC_MSG_NOTICE([Debug mode: CFLAGS set to -O0 -g3])
elif test "x$user_cflags" = "x"; then
  CFLAGS="-O1 -g3 -fno-omit-frame-pointer -fno-common -fsanitize=address,undefined -fno-sanitize-recover=undefined"
  LDFLAGS="-fsanitize=address,undefined $LDFLAGS"
  AC_MSG_NOTICE([ASan+UBSan mode (default): CFLAGS=$CFLAGS])
fi

# BLOOM_HARDEN_HEAP — install the hardened BvtAllocator wrapper that
# catches heap corruption at the corrupting allocation rather than at
# the next unrelated calloc. Auto-on under --enable-debug or when the
# user has passed -fsanitize=address (the wrapper layers usefully with
# ASan and is cheap), off in plain release builds.
have_hardened_heap=no
if test "x$enable_debug" = "xyes"; then
  have_hardened_heap=yes
fi
case "$CFLAGS" in
  *-fsanitize=address*) have_hardened_heap=yes ;;
esac
if test "x$have_hardened_heap" = "xyes"; then
  AC_DEFINE([BLOOM_HARDEN_HEAP], [1],
            [Wrap the bvt allocator with magic + canary + poison-on-free])
  AC_MSG_NOTICE([Hardened heap allocator enabled (catches UAF / overflow at the source)])
fi

# Check for libraries
PKG_CHECK_MODULES([SDL3], [sdl3])
if test "x$host_windows" = "xno" && test "x$host_macos" = "xno"; then
  PKG_CHECK_MODULES([FONTCONFIG], [fontconfig])
fi
PKG_CHECK_MODULES([FREETYPE], [freetype2])
PKG_CHECK_MODULES([HARFBUZZ], [harfbuzz >= 2.0])
PKG_CHECK_MODULES([LIBPNG], [libpng])
PKG_CHECK_MODULES([BLOOM_VT], [bloom-vt])

# Capture dependency versions and build info for --help output
BUILD_CC_VERSION=$($CC --version 2>/dev/null | head -1 || echo "$CC")
AC_DEFINE_UNQUOTED([BUILD_CC], ["$BUILD_CC_VERSION"], [C compiler version string])
DEP_SDL3_VER=$($PKG_CONFIG --modversion sdl3 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_SDL3_VERSION], ["$DEP_SDL3_VER"], [SDL3 version])
DEP_FREETYPE_VER=$($PKG_CONFIG --modversion freetype2 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_FREETYPE_VERSION], ["$DEP_FREETYPE_VER"], [FreeType version])
DEP_HARFBUZZ_VER=$($PKG_CONFIG --modversion harfbuzz 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_HARFBUZZ_VERSION], ["$DEP_HARFBUZZ_VER"], [HarfBuzz version])
DEP_LIBPNG_VER=$($PKG_CONFIG --modversion libpng 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_LIBPNG_VERSION], ["$DEP_LIBPNG_VER"], [libpng version])
DEP_BLOOM_VT_VER=$($PKG_CONFIG --modversion bloom-vt 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_BLOOM_VT_VERSION], ["$DEP_BLOOM_VT_VER"], [bloom-vt version])
if test "x$host_windows" = "xno" && test "x$host_macos" = "xno"; then
  DEP_FONTCONFIG_VER=$($PKG_CONFIG --modversion fontconfig 2>/dev/null || echo "unknown")
  AC_DEFINE_UNQUOTED([DEP_FONTCONFIG_VERSION], ["$DEP_FONTCONFIG_VER"], [Fontconfig version])
fi

# Optional GTK4/libadwaita support (not on Windows)
AC_ARG_ENABLE([gtk4],
  [AS_HELP_STRING([--enable-gtk4],
    [enable GTK4/libadwaita platform backend (auto-detected)])],
  [enable_gtk4=$enableval],
  [enable_gtk4=auto])

# Force-disable GTK4 on Windows and macOS
if test "x$host_windows" = "xyes" || test "x$host_macos" = "xyes"; then
  enable_gtk4=no
fi

have_gtk4=no
if test "x$enable_gtk4" != "xno"; then
  PKG_CHECK_MODULES([GTK4], [gtk4 libadwaita-1],
    [have_gtk4=yes],
    [have_gtk4=no])
  if test "x$enable_gtk4" = "xyes" && test "x$have_gtk4" = "xno"; then
    AC_MSG_ERROR([GTK4/libadwaita requested but not found])
  fi
fi
AM_CONDITIONAL([HAVE_GTK4], [test "x$have_gtk4" = "xyes"])
if test "x$have_gtk4" = "xyes"; then
  AC_DEFINE([HAVE_GTK4], [1], [Define if GTK4/libadwaita is available])
  DEP_GTK4_VER=$($PKG_CONFIG --modversion gtk4 2>/dev/null || echo "unknown")
  AC_DEFINE_UNQUOTED([DEP_GTK4_VERSION], ["$DEP_GTK4_VER"], [GTK4 version])
  DEP_LIBADWAITA_VER=$($PKG_CONFIG --modversion libadwaita-1 2>/dev/null || echo "unknown")
  AC_DEFINE_UNQUOTED([DEP_LIBADWAITA_VERSION], ["$DEP_LIBADWAITA_VER"], [libadwaita version])
  AC_MSG_NOTICE([GTK4/libadwaita platform backend enabled])

  # libdrm provides <drm_fourcc.h> (DRM format constants used when describing
  # the exported DMA-BUF to GTK). Header-only here; no extra link required.
  PKG_CHECK_MODULES([LIBDRM], [libdrm], [have_libdrm=yes], [have_libdrm=no])

  # Vulkan: the GTK4 backend renders with SDL's Vulkan renderer (gamma-correct
  # linear-light blending) and exports the render target as a DMA-BUF via
  # Vulkan external memory for zero-copy display. bloom owns Vulkan device
  # creation (SDL's own device does not enable the external-memory extensions).
  PKG_CHECK_MODULES([VULKAN], [vulkan], [have_vulkan=yes], [have_vulkan=no])
  if test "x$have_vulkan" = "xyes" && test "x$have_libdrm" = "xyes"; then
    AC_DEFINE([HAVE_VULKAN_DMABUF], [1], [Have Vulkan for zero-copy DMA-BUF export])
    AC_MSG_NOTICE([Vulkan DMA-BUF zero-copy rendering enabled])
  else
    AC_MSG_NOTICE([Vulkan not available, GTK4 falls back to readback rendering])
  fi
fi

# Generate config.h header
AC_CONFIG_HEADERS([config.h])
dnl BLOOM_TERMINAL_VERSION is intentionally NOT defined in config.h.
dnl src/Makefile.am regenerates src/bloom_version.h on every `make` so the
dnl version reflects the current git HEAD without needing reconfigure.

# Check for header files
AC_CHECK_HEADERS([stdio.h stdlib.h string.h])

# Check for functions
AC_CHECK_FUNCS([memset])

# Configuration files
AC_CONFIG_FILES([
Makefile
src/Makefile
data/Makefile
tests/Makefile
])

AC_OUTPUT
