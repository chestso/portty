/*
 * Placeholder translation unit.
 *
 * term.c used to call sixel_image_free(), which this file stubbed for
 * unit tests. Sixel decoding now lives entirely in bloom-vt, so term.c
 * has no such dependency and no stub is needed. Kept (empty) so existing
 * test targets that list it keep building without a Makefile change.
 */

typedef int bloom_term_stubs_placeholder;
