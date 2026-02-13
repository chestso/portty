# src/Makefile.am - Build rules for bloom-terminal source files

bin_PROGRAMS = bloom-terminal
bloom_terminal_SOURCES = main.c bloom_conf.c term.c term_vt.c rend.c rend_sdl3.c rend_sdl3_atlas.c rend_sdl3_boxdraw.c font.c font_ft.c colr.c font_resolve.c font_resolve_fc.c png_writer.c png_reader.c unicode.c pty.c platform.c platform_sdl3.c png_mode.c timer.c sixel.c

# Compiler flags - use the variables that configure.ac sets up via PKG_CHECK_MODULES
AM_CFLAGS = $(VTERM_CFLAGS) $(SDL3_CFLAGS) $(FONTCONFIG_CFLAGS) $(FREETYPE_CFLAGS) $(HARFBUZZ_CFLAGS) $(LIBPNG_CFLAGS) -DDATADIR='"$(datadir)"' -Wall -Wextra

# Linker flags - include all necessary libraries
bloom_terminal_LDADD = $(VTERM_LIBS) $(SDL3_LIBS) $(FONTCONFIG_LIBS) $(FREETYPE_LIBS) $(HARFBUZZ_LIBS) $(LIBPNG_LIBS) -lm -lutil -ldl
bloom_terminal_LDFLAGS = -rdynamic

AM_CFLAGS += -DPKGLIBDIR='"$(pkglibdir)"'

# Optional GTK4/libadwaita platform backend (built as dlopen plugin)
if HAVE_GTK4
pkglib_LTLIBRARIES = bloom-terminal-gtk4.la
bloom_terminal_gtk4_la_SOURCES = platform_gtk4.c
bloom_terminal_gtk4_la_CFLAGS = $(GTK4_CFLAGS) $(SDL3_CFLAGS) $(VTERM_CFLAGS) \
    $(FREETYPE_CFLAGS) $(HARFBUZZ_CFLAGS) $(LIBPNG_CFLAGS) \
    -DDATADIR='"$(datadir)"' -Wall -Wextra
bloom_terminal_gtk4_la_LDFLAGS = -module -avoid-version
bloom_terminal_gtk4_la_LIBADD = $(GTK4_LIBS) $(SDL3_LIBS)
endif
