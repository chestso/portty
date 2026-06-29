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

#endif /* PORTTY_BASE64_H */
