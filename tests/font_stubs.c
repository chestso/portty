/*
 * portty — Test stubs for font resolution functions
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

// Stubs for font_resolve functions used by rend_common.c's font loading
// code. The atlas/rend_common tests don't actually call rend_load_fonts,
// but the linker pulls in all symbols from rend_common.o.
#include "font_resolve.h"
#include <string.h>

FontResolveBackend *font_resolve_init(FontResolveBackend *backend)
{
    (void)backend;
    return NULL;
}

void font_resolve_destroy(FontResolveBackend *resolve)
{
    (void)resolve;
}

int font_resolve_find_font(FontResolveBackend *resolve, FontType type,
                           const char *pattern, FontResolutionResult *result)
{
    (void)resolve;
    (void)type;
    (void)pattern;
    memset(result, 0, sizeof(*result));
    return -1;
}

int font_resolve_find_font_for_codepoint(FontResolveBackend *resolve,
                                         uint32_t codepoint,
                                         FontResolutionResult *result)
{
    (void)resolve;
    (void)codepoint;
    memset(result, 0, sizeof(*result));
    return -1;
}

void font_resolve_free_result(FontResolutionResult *result)
{
    (void)result;
}

void font_resolve_list_monospace(FontResolveBackend *resolve)
{
    (void)resolve;
}

// =============================================================================
// Configurable FontBackend stub for rend_common unit tests
// =============================================================================
//
// Tests configure loaded_styles via test_font_set_loaded_styles, then hand
// a FontBackend configured by make_test_font_backend to the helper under
// test. Only get_glyph_index is implemented; the rest are NULL and the
// font.c dispatch wrappers short-circuit on a missing function pointer,
// so the stubs only need to fake the data-driven branch decisions:
//   - which styles are "loaded" (loaded_styles bitmask)
//   - whether the emoji font carries the test codepoint (get_glyph_index)
//
// The stub returns non-zero from get_glyph_index for U+26A1 (lightning
// emoji) and 0 otherwise, so tests can use that codepoint to trigger
// the emoji routing branch deterministically.
#include "font.h"

static uint32_t s_test_loaded_styles = 0;
#define TEST_STUB_EMOJI_CP 0x26A1 // ⚡ — has emoji presentation + stub claims glyph

void test_font_set_loaded_styles(uint32_t mask) { s_test_loaded_styles = mask; }
uint32_t test_font_get_loaded_styles(void) { return s_test_loaded_styles; }
void test_font_reset(void) { s_test_loaded_styles = 0; }

static uint32_t stub_get_glyph_index(FontBackend *font, void *font_data,
                                     uint32_t cp)
{
    (void)font;
    (void)font_data;
    return cp == TEST_STUB_EMOJI_CP ? 1 : 0;
}

FontBackend make_test_font_backend(void)
{
    FontBackend fb = { 0 };
    fb.name = "test-font-stub";
    fb.loaded_styles = s_test_loaded_styles;
    fb.get_glyph_index = stub_get_glyph_index;
    // All other callbacks left NULL — font.c's wrapper layer short-circuits
    // when a function pointer is missing, so the unit tests only need to
    // drive decisions via loaded_styles + the emoji-codepoint branch.
    return fb;
}
