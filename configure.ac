AC_INIT([bloom-terminal], [0.1], [bug-report@example.com])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign subdir-objects])

# Check for programs
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_MAKE_SET

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
PKG_CHECK_MODULES([FONTCONFIG], [fontconfig])
PKG_CHECK_MODULES([FREETYPE], [freetype2])
PKG_CHECK_MODULES([HARFBUZZ], [harfbuzz >= 2.0])
PKG_CHECK_MODULES([LIBPNG], [libpng])

# Optional GTK4/libadwaita support
AC_ARG_ENABLE([gtk4],
  [AS_HELP_STRING([--enable-gtk4],
    [enable GTK4/libadwaita platform backend (auto-detected)])],
  [enable_gtk4=$enableval],
  [enable_gtk4=auto])

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
  AC_MSG_NOTICE([GTK4/libadwaita platform backend enabled])
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
