TEST_COMMON = test_common.c
SDL_STUBS   = sdl_stubs.c
TERM_STUBS  = term_stubs.c
# term.h now includes <coffer/coffer.h>, so every test that pulls it
# in (directly or via term.c) needs the coffer include path.
TEST_CFLAGS = -I$(top_srcdir)/src $(COFFER_CFLAGS) -Wall -Wextra

EXTRA_DIST = test_helpers.h font_stubs.c

check_PROGRAMS = test_conf test_term_cfr test_osc52 test_sixel test_lottie test_lottie_cache test_diag test_clipboard_deferred test_paste_normalize test_altscreen_mouse test_path_compat test_boxdraw test_cursor_render test_scroll test_timer test_rend_common test_portty_app test_debug_script test_selection_extend test_vs15_routing test_sokol_deco test_dpi_scale test_content_scale test_panel test_link_hint

if BACKEND_SDL3
check_PROGRAMS += test_atlas
test_atlas_SOURCES = test_atlas.c $(TEST_COMMON) $(SDL_STUBS) $(TERM_STUBS) \
    ../src/rend_sdl3_atlas.c ../src/rend_common.c ../src/font.c font_stubs.c
test_atlas_CFLAGS = $(TEST_CFLAGS) $(SDL3_CFLAGS)
# rend_sdl3_atlas.c uses pow() for the sRGB->linear LUT.
test_atlas_LDADD = -lm
endif

test_conf_SOURCES = test_conf.c $(TEST_COMMON) ../src/portty_conf.c
test_conf_CFLAGS = $(TEST_CFLAGS)

# diag.c includes the generated portty_version.h (builddir/src), so add that path.
test_diag_SOURCES = test_diag.c $(TEST_COMMON) ../src/diag.c
test_diag_CFLAGS = $(TEST_CFLAGS) -I$(top_builddir)/src

# Deferred-clipboard-free linked list logic. Pure C, no SDL/coffer
# dependency — the list logic is replicated inline to test the invariant
# that entries survive CLIPBOARD_DEFERRED_MIN_AGE advances.
test_clipboard_deferred_SOURCES = test_clipboard_deferred.c $(TEST_COMMON)
test_clipboard_deferred_CFLAGS = $(TEST_CFLAGS)

# Clipboard line-ending normalization (the fix for blank lines after
# every pasted line on Windows CRLF clipboards). Pure byte transform —
# the logic is replicated inline, no SDL/coffer dependency.
test_paste_normalize_SOURCES = test_paste_normalize.c $(TEST_COMMON)
test_paste_normalize_CFLAGS = $(TEST_CFLAGS)

# Altscreen + mouse-tracking mode state through the coffer bridge.
# Regression test for the on_mouse guard: altscreen without mouse_mode
# must not allow terminal-level selection without Shift.
test_altscreen_mouse_SOURCES = test_altscreen_mouse.c $(TEST_COMMON) \
    ../src/term.c ../src/term_cfr.c ../src/base64.c ../src/path_compat.c
test_altscreen_mouse_CFLAGS = $(TEST_CFLAGS) $(COFFER_CFLAGS)
test_altscreen_mouse_LDADD = $(COFFER_LIBS)

test_term_cfr_SOURCES = test_term_cfr.c $(TEST_COMMON) \
    ../src/term.c ../src/term_cfr.c ../src/base64.c ../src/path_compat.c
test_term_cfr_CFLAGS = $(TEST_CFLAGS) $(COFFER_CFLAGS)
test_term_cfr_LDADD = $(COFFER_LIBS)

test_osc52_SOURCES = test_osc52.c $(TEST_COMMON) \
    ../src/term.c ../src/term_cfr.c ../src/base64.c ../src/path_compat.c
test_osc52_CFLAGS = $(TEST_CFLAGS) $(COFFER_CFLAGS)
test_osc52_LDADD = $(COFFER_LIBS)

# Sixel end-to-end through the host bridge: feed a DCS sequence via
# terminal_process_input and read images back via terminal_get_sixels.
test_sixel_SOURCES = test_sixel.c $(TEST_COMMON) \
    ../src/term.c ../src/term_cfr.c ../src/base64.c ../src/path_compat.c
test_sixel_CFLAGS = $(TEST_CFLAGS) $(COFFER_CFLAGS)
test_sixel_LDADD = $(COFFER_LIBS)

# Lottie end-to-end through the host bridge: feed an APC sequence via
# terminal_process_input and read animations back via terminal_get_lotties.
test_lottie_SOURCES = test_lottie.c $(TEST_COMMON) \
    ../src/term.c ../src/term_cfr.c ../src/base64.c ../src/path_compat.c
test_lottie_CFLAGS = $(TEST_CFLAGS) $(COFFER_CFLAGS)
test_lottie_LDADD = $(COFFER_LIBS)


# test_pty_pause depends on POSIX PTY APIs. It is only relevant when
# the SDL3 backend is active. Under the Sokol backend this is excluded.
if BACKEND_SDL3
if !HOST_WINDOWS
check_PROGRAMS += test_pty_pause
test_pty_pause_SOURCES = test_pty_pause.c $(TEST_COMMON) ../src/term.c
test_pty_pause_CFLAGS = $(TEST_CFLAGS)
endif
endif

# Lottie cache logic (pure C, no GPU dependency). Tests cache lookup,
# version/size change detection, and stale entry reconciliation.
test_lottie_cache_SOURCES = test_lottie_cache.c $(TEST_COMMON) $(TERM_STUBS)
test_lottie_cache_CFLAGS = $(TEST_CFLAGS)

# Shift+Click selection extension. Uses a mock TerminalBackend (no coffer
# or SDL dependency). Compiles term.c for the selection logic.
test_selection_extend_SOURCES = test_selection_extend.c $(TEST_COMMON) ../src/term.c
test_selection_extend_CFLAGS = $(TEST_CFLAGS)

# Sokol decoration coalescing logic. Pure C — replicates the underline and
# strikethrough run coalescing algorithms without GPU dependencies.
test_sokol_deco_SOURCES = test_sokol_deco.c $(TEST_COMMON)
test_sokol_deco_CFLAGS = $(TEST_CFLAGS)

test_dpi_scale_SOURCES = test_dpi_scale.c $(TEST_COMMON)
test_dpi_scale_CFLAGS = $(TEST_CFLAGS)

test_content_scale_SOURCES = test_content_scale.c $(TEST_COMMON)
test_content_scale_CFLAGS = $(TEST_CFLAGS)

# Panel state management. Pure C, no SDL/coffer dependency.
test_panel_SOURCES = test_panel.c $(TEST_COMMON) ../src/portty_panel.c
test_panel_CFLAGS = $(TEST_CFLAGS)

# MSYS2/Unix → Windows path conversion. Pure C, no SDL/coffer dependency.
test_path_compat_SOURCES = test_path_compat.c $(TEST_COMMON) ../src/path_compat.c
test_path_compat_CFLAGS = $(TEST_CFLAGS)

# Procedural box-drawing alignment test.  Renders to a CPU pixel buffer
# (GlyphBitmap) and inspects pixels directly — no SDL or GPU required.
test_boxdraw_SOURCES = test_boxdraw.c $(TEST_COMMON) $(TERM_STUBS) ../src/rend_boxdraw.c ../src/rend_common.c ../src/font.c font_stubs.c
test_boxdraw_CFLAGS = $(TEST_CFLAGS)
test_boxdraw_LDADD = -lm

# Cursor-only render regression test. Pure C — replicates the clip-rect
# and blit-rect computation from sdl3_draw_cursor inline, verifying that
# the blit rect matches the cursor cell (not the full screen) and that
# no full-screen clear is performed.
test_cursor_render_SOURCES = test_cursor_render.c $(TEST_COMMON)
test_cursor_render_CFLAGS = $(TEST_CFLAGS)

# Mouse-wheel scroll logic: sub-tick accumulation and consolidated
# dispatch decision tree. Pure C, no SDL/coffer dependency.
test_scroll_SOURCES = test_scroll.c $(TEST_COMMON)
test_scroll_CFLAGS = $(TEST_CFLAGS)
test_scroll_LDADD = -lm

# Poll-based timer manager. Pure C, no SDL/coffer dependency.
test_timer_SOURCES = test_timer.c $(TEST_COMMON) ../src/timer.c
test_timer_CFLAGS = $(TEST_CFLAGS)
test_timer_LDADD =

# Shared GPU-agnostic renderer helpers (Nerd Font map, sRGB helpers).
test_rend_common_SOURCES = test_rend_common.c $(TEST_COMMON) $(TERM_STUBS) ../src/rend_common.c ../src/font.c font_stubs.c
test_rend_common_CFLAGS = $(TEST_CFLAGS)
test_rend_common_LDADD = -lm

# PorttyApp terminal callbacks. Uses a stub PorttyBackend to verify the
# callback wiring without SDL or a real PTY. It still needs term.c and
# coffer for the full handler implementations.
test_portty_app_SOURCES = test_portty_app.c $(TEST_COMMON) \
    ../src/portty_app.c ../src/term.c ../src/term_cfr.c ../src/base64.c \
    ../src/path_compat.c ../src/pager.c ../src/rend_common.c \
    ../src/portty_conf.c ../src/diag.c ../src/font.c font_stubs.c
if HOST_WINDOWS
test_portty_app_SOURCES += ../src/pty_w32.c
else !HOST_WINDOWS
test_portty_app_SOURCES += ../src/pty.c
endif
test_portty_app_CFLAGS = $(TEST_CFLAGS) $(COFFER_CFLAGS) -I$(top_builddir)/src
test_portty_app_LDADD = $(COFFER_LIBS)

# Script parser. Pure string parsing — no SDL/coffer dependency for
# the parser itself, but portty_script.h includes term.h which pulls
# in coffer, and the step helper calls rend_display_row_to_unified.
test_debug_script_SOURCES = test_debug_script.c $(TEST_COMMON) $(TERM_STUBS) \
    ../src/portty_script.c ../src/portty_frame_rec.c ../src/rend_common.c ../src/font.c font_stubs.c
test_debug_script_CFLAGS = $(TEST_CFLAGS) $(COFFER_CFLAGS)
test_debug_script_LDADD = $(COFFER_LIBS) -lm

# VS15 emoji routing regression test. Tests rend_should_use_emoji() and
# unicode_cell_has_vs15() — pure logic, no SDL/coffer dependency, but
# rend_common.c pulls in font.c (needs font_stubs.c).
test_vs15_routing_SOURCES = test_vs15_routing.c $(TEST_COMMON) $(TERM_STUBS) \
    ../src/rend_common.c ../src/font.c font_stubs.c
test_vs15_routing_CFLAGS = $(TEST_CFLAGS)
test_vs15_routing_LDADD = -lm

# Link hint panel positioning tests. Pure logic, no SDL/coffer dependency.
test_link_hint_SOURCES = test_link_hint.c $(TEST_COMMON)
test_link_hint_CFLAGS = $(TEST_CFLAGS) $(COFFER_CFLAGS)
test_link_hint_LDADD = $(COFFER_LIBS)

TESTS = $(check_PROGRAMS)
