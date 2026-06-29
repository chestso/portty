#ifndef TERM_BVT_H
#define TERM_BVT_H

#include "term.h"

/* bloom-vt terminal backend — selected via env var PORTTY_VT=bloomvt. */
extern TerminalBackend terminal_backend_bvt;

/* Allocate a fresh, independent bloom-vt-backed terminal on the heap and
 * initialise it to (cols x rows). Unlike using the shared terminal_backend_bvt
 * global directly, this yields a distinct instance suitable for a second,
 * PTY-less terminal (e.g. the pager overlay). Returns NULL on failure.
 * Free with terminal_destroy() followed by free(). */
TerminalBackend *term_bvt_new(const BvtConfig *cfg);

#endif /* TERM_BVT_H */
