# src/Makefile.am - Build rules for bloom-term source files

bin_PROGRAMS = bloom-term
bloom_term_SOURCES = main.c term.c term_vt.c rend.c rend_sdl3.c rend_sdl3_atlas.c font.c font_ft.c colr.c font_resolver.c png_writer.c unicode.c pty.c event_loop.c event_loop_sdl3.c timer.c

# Compiler flags - use the variables that configure.ac sets up via PKG_CHECK_MODULES
AM_CFLAGS = $(VTERM_CFLAGS) $(SDL3_CFLAGS) $(FONTCONFIG_CFLAGS) $(FREETYPE_CFLAGS) $(HARFBUZZ_CFLAGS) $(LIBPNG_CFLAGS) -Wall -Wextra

# Linker flags - include all necessary libraries
bloom_term_LDADD = $(VTERM_LIBS) $(SDL3_LIBS) $(FONTCONFIG_LIBS) $(FREETYPE_LIBS) $(HARFBUZZ_LIBS) $(LIBPNG_LIBS) -lm -lutil
