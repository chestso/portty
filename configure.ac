AC_INIT([bloom-terminal], [0.1], [bug-report@example.com])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_CANONICAL_HOST

# Check for programs
AC_PROG_CC
LT_INIT([dlopen])
AC_PROG_INSTALL
AC_PROG_MAKE_SET

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

# Debug option
AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug],
    [enable debug symbols and disable optimizations])],
  [enable_debug=$enableval],
  [enable_debug=no])

# Set CFLAGS based on debug option
if test "x$enable_debug" = "xyes"; then
  CFLAGS="-O0 -g3 $CFLAGS"
  AC_MSG_NOTICE([Debug mode enabled: CFLAGS set to -O0 -g3])
else
  # Use default optimization if CFLAGS not set by user
  if test "x$CFLAGS" = "x"; then
    CFLAGS="-O2"
  fi
fi

# Check for libraries
PKG_CHECK_MODULES([VTERM], [vterm])
PKG_CHECK_MODULES([SDL3], [sdl3])
if test "x$host_windows" = "xno" && test "x$host_macos" = "xno"; then
  PKG_CHECK_MODULES([FONTCONFIG], [fontconfig])
fi
PKG_CHECK_MODULES([FREETYPE], [freetype2])
PKG_CHECK_MODULES([HARFBUZZ], [harfbuzz >= 2.0])
PKG_CHECK_MODULES([LIBPNG], [libpng])

# Capture dependency versions and build info for --help output
AC_DEFINE_UNQUOTED([BUILD_CC], ["$CC"], [C compiler used for build])
DEP_VTERM_VER=$($PKG_CONFIG --modversion vterm 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_VTERM_VERSION], ["$DEP_VTERM_VER"], [libvterm version])
DEP_SDL3_VER=$($PKG_CONFIG --modversion sdl3 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_SDL3_VERSION], ["$DEP_SDL3_VER"], [SDL3 version])
DEP_FREETYPE_VER=$($PKG_CONFIG --modversion freetype2 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_FREETYPE_VERSION], ["$DEP_FREETYPE_VER"], [FreeType version])
DEP_HARFBUZZ_VER=$($PKG_CONFIG --modversion harfbuzz 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_HARFBUZZ_VERSION], ["$DEP_HARFBUZZ_VER"], [HarfBuzz version])
DEP_LIBPNG_VER=$($PKG_CONFIG --modversion libpng 2>/dev/null || echo "unknown")
AC_DEFINE_UNQUOTED([DEP_LIBPNG_VERSION], ["$DEP_LIBPNG_VER"], [libpng version])
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

  # Check for EGL + libdrm + GBM for zero-copy DMA-BUF rendering
  PKG_CHECK_MODULES([EGL], [egl], [have_egl=yes], [have_egl=no])
  PKG_CHECK_MODULES([LIBDRM], [libdrm], [have_libdrm=yes], [have_libdrm=no])
  PKG_CHECK_MODULES([GBM], [gbm], [have_gbm=yes], [have_gbm=no])
  if test "x$have_egl" = "xyes" && test "x$have_libdrm" = "xyes" && test "x$have_gbm" = "xyes"; then
    AC_DEFINE([HAVE_EGL_DMABUF], [1], [Have EGL + GBM + DRM for zero-copy DMA-BUF rendering])
    AC_MSG_NOTICE([GBM/EGL DMA-BUF zero-copy rendering enabled])
  else
    AC_MSG_NOTICE([GBM/EGL DMA-BUF not available, using readback rendering])
  fi
fi

# Generate config.h header
AC_CONFIG_HEADERS([config.h])

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
