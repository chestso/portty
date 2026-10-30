/*
 * portty — Base64 encoder and decoder interface
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Thomas Christensen
 */

#ifndef PORTTY_BASE64_H
#define PORTTY_BASE64_H

#include <stddef.h>
#include <stdint.h>

/* Decode `len` bytes of base64 from `src` into a freshly malloc'd buffer.
 * Tolerates whitespace inside the payload and missing padding (some OSC 52
 * producers in the wild omit it). Returns NULL on malformed input or
 * allocation failure; the caller must free() a non-NULL result. On success
 * `*out_len` receives the decoded byte count. */
uint8_t *base64_decode(const char *src, size_t len, size_t *out_len);

/* Encode `len` bytes of `src` as standard base64 (RFC 4648 alphabet, '='
 * padding emitted). Returns a freshly malloc'd, NUL-terminated string the
 * caller must free(), or NULL on allocation failure. A zero-length input
 * encodes to the empty string. */
char *base64_encode(const uint8_t *src, size_t len);

#endif /* PORTTY_BASE64_H */
