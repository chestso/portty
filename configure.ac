AC_INIT([portty],
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
PORTTY_VERSION=`"$srcdir/build-aux/git-version.sh" "$srcdir" 2>/dev/null`
AS_IF([test -z "$PORTTY_VERSION"],
      [PORTTY_VERSION="$PACKAGE_VERSION"])
AC_SUBST([PORTTY_VERSION])
AC_MSG_NOTICE([portty version: $PORTTY_VERSION])

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

# Windows resource compiler (windres) for embedding the app icon in the .exe.
# Automake recognizes the RC variable and auto-generates .rc → .o build rules.
AC_CHECK_TOOL([RC], [windres], [])
AM_CONDITIONAL([HAVE_RC], [test -n "$RC" && test "x$host_windows" = "xyes"])

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
  dnl ASan/UBSan is not available on Windows/MinGW. Fall back to
  dnl unsanitized debug when cross-compiling or building natively on
  dnl Windows (MinGW does not ship libasan/libubsan).
  if test "x$host_windows" = "xyes"; then
    CFLAGS="-O1 -g3 -fno-omit-frame-pointer -fno-common"
    AC_MSG_NOTICE([Debug mode (no ASan on Windows): CFLAGS=$CFLAGS])
  else
    CFLAGS="-O1 -g3 -fno-omit-frame-pointer -fno-common -fsanitize=address,undefined -fno-sanitize-recover=undefined"
    LDFLAGS="-fsanitize=address,undefined $LDFLAGS"
    AC_MSG_NOTICE([ASan+UBSan mode (default): CFLAGS=$CFLAGS])
  fi
fi

# PORTTY_HARDEN_HEAP — install the hardened CfrAllocator wrapper that
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
  AC_DEFINE([PORTTY_HARDEN_HEAP], [1],
            [Wrap the cfr allocator with magic + canary + poison-on-free])
  AC_MSG_NOTICE([Hardened heap allocator enabled (catches UAF / overflow at the source)])
fi

# Backend selection (SDL3 is the default).
AC_ARG_WITH([backend],
  [AS_HELP_STRING([--with-backend=sdl3|sokol],
    [Windowing+GPU backend @<:@default=sdl3@:>@])],
  [portty_backend="$withval"],
  [portty_backend="sdl3"])

AS_CASE([$portty_backend],
  [sdl3], [
    PKG_CHECK_MODULES([SDL3], [sdl3])
    AC_DEFINE([PORTTY_BACKEND_SDL3], [1], [Use SDL3 backend])
  ],
  [sokol], [
    # Sokol headers live in third_party/sokol/ (fetched by scripts/fetch-sokol.sh).
    # Use -I for the parent so #include <sokol/sokol_app.h> resolves.
    SOKOL_CFLAGS="-I${srcdir}/third_party"
    CFLAGS="$SOKOL_CFLAGS $CFLAGS"
    AC_SUBST([SOKOL_CFLAGS])
    AC_DEFINE([PORTTY_BACKEND_SOKOL], [1], [Use Sokol backend])
    # Auto-fetch Sokol headers if missing so --with-backend=sokol works
    # without a manual scripts/fetch-sokol.sh step.
    AS_IF([test ! -f "$srcdir/third_party/sokol/sokol_app.h"],
      [AC_MSG_NOTICE([Sokol headers not found, fetching via scripts/fetch-sokol.sh])
       AS_IF([$srcdir/scripts/fetch-sokol.sh "$srcdir/third_party/sokol"],
         [AC_MSG_NOTICE([Sokol headers fetched successfully])],
         [AC_MSG_ERROR([Failed to fetch Sokol headers. Run scripts/fetch-sokol.sh manually or check network access.])])])
    AS_CASE([$host_os],
      [*linux*], [
        AC_DEFINE([SOKOL_GLCORE], [1], [Use Sokol OpenGL core profile])
        PLATFORM_LIBS="-lX11 -lXi -lXcursor -lGL -ldl -lpthread -lm"
      ],
      [*mingw*|*msys*], [
        AC_DEFINE([SOKOL_D3D11], [1], [Use Sokol D3D11 backend])
        PLATFORM_LIBS="-ld3d11 -ldxgi -ldxguid -lkernel32 -luser32 -lshell32 -lole32 -ladvapi32 -ldwrite -lgdi32 -ldwmapi -lwinpthread"
      ],
      [*darwin*], [
        AC_DEFINE([SOKOL_METAL], [1], [Use Sokol Metal backend])
        PLATFORM_LIBS="-framework AppKit -framework QuartzCore -framework Metal -framework CoreText -framework CoreFoundation"
      ])
    AC_SUBST([PLATFORM_LIBS])
  ],
  [AC_MSG_ERROR([Unknown backend: $portty_backend])])

AM_CONDITIONAL([BACKEND_SDL3], [test "x$portty_backend" = xsdl3])
AM_CONDITIONAL([BACKEND_SOKOL], [test "x$portty_backend" = xsokol])
AC_SUBST([portty_backend])

# The QOI single-header library (used by the frame recorder's qoi_writer.c)
# lives in third_party/qoi/qoi.h. It is fetched by scripts/fetch-qoi.sh and
# is required by both backends. Auto-fetch it at configure time if missing
# so a fresh checkout builds without a manual fetch step.
AS_IF([test ! -f "$srcdir/third_party/qoi/qoi.h"],
  [AC_MSG_NOTICE([QOI header not found, fetching via scripts/fetch-qoi.sh])
   AS_IF([$srcdir/scripts/fetch-qoi.sh "$srcdir/third_party/qoi"],
     [AC_MSG_NOTICE([QOI header fetched successfully])],
     [AC_MSG_ERROR([Failed to fetch QOI header. Run scripts/fetch-qoi.sh manually or check network access.])])])

AS_IF([test "x$portty_backend" = xsdl3], [
  # Check for libraries
  PKG_CHECK_MODULES([SDL3], [sdl3])
])
if test "x$host_windows" = "xno" && test "x$host_macos" = "xno"; then
  PKG_CHECK_MODULES([FONTCONFIG], [fontconfig])
fi
PKG_CHECK_MODULES([FREETYPE], [freetype2])
PKG_CHECK_MODULES([HARFBUZZ], [harfbuzz >= 2.0])
PKG_CHECK_MODULES([LIBPNG], [libpng])
PKG_CHECK_MODULES([COFFER], [coffer >= 0.1.11])

AS_IF([test "x$portty_backend" = xsdl3], [
  DEP_SDL3_VER=$($PKG_CONFIG --modversion sdl3 2>/dev/null || echo "unknown")
  AC_DEFINE_UNQUOTED([DEP_SDL3_VERSION], ["$DEP_SDL3_VER"], [SDL3 version])
])

# Capture dependency versions and build info for --help output
BUILD_CC_VERSION=$($CC --version 2>/dev/null | head -1 || echo "$CC")
AC_DEFINE_UNQUOTED([BUILD_CC], ["$BUILD_CC_VERSION"], [C compiler version string])
DEP_FREETYPE_VER=$($PKG_CONFIG --modversion freetype2 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_FREETYPE_VERSION], ["$DEP_FREETYPE_VER"], [FreeType version])
DEP_HARFBUZZ_VER=$($PKG_CONFIG --modversion harfbuzz 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_HARFBUZZ_VERSION], ["$DEP_HARFBUZZ_VER"], [HarfBuzz version])
DEP_LIBPNG_VER=$($PKG_CONFIG --modversion libpng 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_LIBPNG_VERSION], ["$DEP_LIBPNG_VER"], [libpng version])
DEP_COFFER_VER=$($PKG_CONFIG --modversion coffer 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_COFFER_VERSION], ["$DEP_COFFER_VER"], [coffer version])
if test "x$host_windows" = "xno" && test "x$host_macos" = "xno"; then
  DEP_FONTCONFIG_VER=$($PKG_CONFIG --modversion fontconfig 2>/dev/null || echo "unknown")
  AC_DEFINE_UNQUOTED([DEP_FONTCONFIG_VERSION], ["$DEP_FONTCONFIG_VER"], [Fontconfig version])
fi

# Generate config.h header
AC_CONFIG_HEADERS([config.h])
dnl PORTTY_VERSION is intentionally NOT defined in config.h.
dnl src/Makefile.am regenerates src/portty_version.h on every `make` so the
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
