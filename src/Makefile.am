# src/Makefile.am - Build rules for bloom-terminal source files

bin_PROGRAMS = bloom-terminal

# Platform-specific implementations
if HOST_WINDOWS
PTY_SOURCE = pty_w32.c
FONT_RESOLVE_SOURCE = font_resolve_w32.c
else !HOST_WINDOWS
PTY_SOURCE = pty.c
if HOST_MACOS
FONT_RESOLVE_SOURCE = font_resolve_ct.c
else
FONT_RESOLVE_SOURCE = font_resolve_fc.c
endif
endif

bloom_terminal_SOURCES = main.c bloom_conf.c term.c term_bvt.c \
    rend.c rend_sdl3.c \
    rend_sdl3_atlas.c rend_sdl3_boxdraw.c font.c font_ft.c colr.c font_resolve.c \
    $(FONT_RESOLVE_SOURCE) png_writer.c png_reader.c unicode.c $(PTY_SOURCE) \
    platform.c platform_sdl3.c png_mode.c timer.c sixel.c \
    bloom_vt/term.c bloom_vt/parser.c bloom_vt/grid.c bloom_vt/style.c \
    bloom_vt/grapheme.c bloom_vt/width.c bloom_vt/print.c bloom_vt/csi.c \
    bloom_vt/osc.c bloom_vt/dcs.c bloom_vt/esc.c bloom_vt/damage.c \
    bloom_vt/scrollback.c bloom_vt/palette.c bloom_vt/keys.c bloom_vt/modes.c \
    bloom_vt/reflow.c

# Compiler flags
AM_CFLAGS = $(SDL3_CFLAGS) $(FREETYPE_CFLAGS) \
    $(HARFBUZZ_CFLAGS) $(LIBPNG_CFLAGS) -DBLOOM_DATADIR='"$(datadir)"' -Wall -Wextra

# Linker flags
bloom_terminal_LDADD = $(SDL3_LIBS) \
    $(FREETYPE_LIBS) $(HARFBUZZ_LIBS) $(LIBPNG_LIBS) -lm

if HOST_WINDOWS
bloom_terminal_LDADD += -ladvapi32 -lshell32 -ldwmapi
bloom_terminal_LDFLAGS =
else !HOST_WINDOWS
if HOST_MACOS
bloom_terminal_LDFLAGS = -framework CoreText -framework CoreFoundation \
    -framework GameController -framework CoreHaptics \
    -framework CoreMotion -framework CoreMedia
else
AM_CFLAGS += $(FONTCONFIG_CFLAGS)
bloom_terminal_LDADD += $(FONTCONFIG_LIBS)
bloom_terminal_LDADD += -lutil
bloom_terminal_LDFLAGS = -rdynamic
endif
endif

AM_CFLAGS += -DPKGLIBDIR='"$(pkglibdir)"'

# Optional GTK4/libadwaita platform backend (built as dlopen plugin)
if HAVE_GTK4
pkglib_LTLIBRARIES = bloom-terminal-gtk4.la
bloom_terminal_gtk4_la_SOURCES = platform_gtk4.c
bloom_terminal_gtk4_la_CFLAGS = $(GTK4_CFLAGS) $(SDL3_CFLAGS) \
    $(FREETYPE_CFLAGS) $(HARFBUZZ_CFLAGS) $(LIBPNG_CFLAGS) \
    $(EGL_CFLAGS) $(LIBDRM_CFLAGS) $(GBM_CFLAGS) \
    -DBLOOM_DATADIR='"$(datadir)"' -Wall -Wextra
bloom_terminal_gtk4_la_LDFLAGS = -module -avoid-version
bloom_terminal_gtk4_la_LIBADD = $(GTK4_LIBS) $(SDL3_LIBS) $(EGL_LIBS) $(GBM_LIBS)
endif
