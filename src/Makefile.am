# src/Makefile.am - Build rules for verm source files

bin_PROGRAMS = vterm-sdl3
vterm_sdl3_SOURCES = main.c terminal.c renderer.c font.c font_ft.c font_resolver.c png_writer.c

# Compiler flags - use the variables that configure.ac sets up via PKG_CHECK_MODULES
AM_CFLAGS = $(VTERM_CFLAGS) $(SDL3_CFLAGS) $(FONTCONFIG_CFLAGS) $(FREETYPE_CFLAGS) $(HARFBUZZ_CFLAGS) $(LIBPNG_CFLAGS) -Wall -Wextra

# Linker flags - include all necessary libraries
vterm_sdl3_LDADD = $(VTERM_LIBS) $(SDL3_LIBS) $(FONTCONFIG_LIBS) $(FREETYPE_LIBS) $(HARFBUZZ_LIBS) $(LIBPNG_LIBS) -lm
