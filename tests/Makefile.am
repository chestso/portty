TEST_COMMON = test_common.c
SDL_STUBS   = sdl_stubs.c
TERM_STUBS  = term_stubs.c
TEST_CFLAGS = -I$(top_srcdir)/src -Wall -Wextra

check_PROGRAMS = test_atlas test_unicode test_conf test_bvt_parser

test_atlas_SOURCES = test_atlas.c $(TEST_COMMON) $(SDL_STUBS) ../src/rend_sdl3_atlas.c
test_atlas_CFLAGS = $(TEST_CFLAGS) $(SDL3_CFLAGS)

test_unicode_SOURCES = test_unicode.c $(TEST_COMMON) ../src/unicode.c
test_unicode_CFLAGS = $(TEST_CFLAGS)

test_conf_SOURCES = test_conf.c $(TEST_COMMON) ../src/bloom_conf.c
test_conf_CFLAGS = $(TEST_CFLAGS)

test_bvt_parser_SOURCES = test_bvt_parser.c $(TEST_COMMON) \
    ../src/bloom_vt/term.c ../src/bloom_vt/parser.c ../src/bloom_vt/grid.c \
    ../src/bloom_vt/style.c ../src/bloom_vt/grapheme.c ../src/bloom_vt/width.c \
    ../src/bloom_vt/print.c ../src/bloom_vt/csi.c ../src/bloom_vt/osc.c \
    ../src/bloom_vt/dcs.c ../src/bloom_vt/esc.c ../src/bloom_vt/damage.c \
    ../src/bloom_vt/scrollback.c ../src/bloom_vt/palette.c \
    ../src/bloom_vt/keys.c ../src/bloom_vt/modes.c ../src/bloom_vt/reflow.c
test_bvt_parser_CFLAGS = $(TEST_CFLAGS)

# test_pty_pause depends on POSIX PTY APIs
if !HOST_WINDOWS
check_PROGRAMS += test_pty_pause
test_pty_pause_SOURCES = test_pty_pause.c $(TEST_COMMON) $(TERM_STUBS) ../src/platform.c ../src/term.c ../src/unicode.c
test_pty_pause_CFLAGS = $(TEST_CFLAGS)
endif

TESTS = $(check_PROGRAMS)
