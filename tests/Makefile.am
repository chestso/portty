TEST_COMMON = test_common.c
SDL_STUBS   = sdl_stubs.c
TERM_STUBS  = term_stubs.c
# term.h now includes <bloom-vt/bloom_vt.h>, so every test that pulls it
# in (directly or via term.c) needs the bloom-vt include path.
TEST_CFLAGS = -I$(top_srcdir)/src $(BLOOM_VT_CFLAGS) -Wall -Wextra

check_PROGRAMS = test_atlas test_unicode test_conf test_term_bvt test_osc52 test_sixel test_diag

test_atlas_SOURCES = test_atlas.c $(TEST_COMMON) $(SDL_STUBS) ../src/rend_sdl3_atlas.c
test_atlas_CFLAGS = $(TEST_CFLAGS) $(SDL3_CFLAGS)
# rend_sdl3_atlas.c uses pow() for the sRGB->linear LUT.
test_atlas_LDADD = -lm

test_unicode_SOURCES = test_unicode.c $(TEST_COMMON) ../src/unicode.c
test_unicode_CFLAGS = $(TEST_CFLAGS)

test_conf_SOURCES = test_conf.c $(TEST_COMMON) ../src/bloom_conf.c
test_conf_CFLAGS = $(TEST_CFLAGS)

# diag.c includes the generated bloom_version.h (builddir/src), so add that path.
test_diag_SOURCES = test_diag.c $(TEST_COMMON) ../src/diag.c
test_diag_CFLAGS = $(TEST_CFLAGS) -I$(top_builddir)/src

test_term_bvt_SOURCES = test_term_bvt.c $(TEST_COMMON) $(TERM_STUBS) \
    ../src/term.c ../src/term_bvt.c ../src/base64.c ../src/unicode.c
test_term_bvt_CFLAGS = $(TEST_CFLAGS) $(BLOOM_VT_CFLAGS)
test_term_bvt_LDADD = $(BLOOM_VT_LIBS)

test_osc52_SOURCES = test_osc52.c $(TEST_COMMON) $(TERM_STUBS) \
    ../src/term.c ../src/term_bvt.c ../src/base64.c ../src/unicode.c
test_osc52_CFLAGS = $(TEST_CFLAGS) $(BLOOM_VT_CFLAGS)
test_osc52_LDADD = $(BLOOM_VT_LIBS)

# Sixel end-to-end through the host bridge: feed a DCS sequence via
# terminal_process_input and read images back via terminal_get_sixels.
test_sixel_SOURCES = test_sixel.c $(TEST_COMMON) \
    ../src/term.c ../src/term_bvt.c ../src/base64.c ../src/unicode.c
test_sixel_CFLAGS = $(TEST_CFLAGS) $(BLOOM_VT_CFLAGS)
test_sixel_LDADD = $(BLOOM_VT_LIBS)

# test_pty_pause depends on POSIX PTY APIs
if !HOST_WINDOWS
check_PROGRAMS += test_pty_pause
test_pty_pause_SOURCES = test_pty_pause.c $(TEST_COMMON) $(TERM_STUBS) ../src/platform.c ../src/term.c ../src/unicode.c
test_pty_pause_CFLAGS = $(TEST_CFLAGS)
endif

TESTS = $(check_PROGRAMS)
