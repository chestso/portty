#ifndef TERM_CFR_H
#define TERM_CFR_H

#include "term.h"

/* coffer terminal backend — selected via env var PORTTY_VT=coffer. */
extern TerminalBackend terminal_backend_cfr;

/* Allocate a fresh, independent coffer-backed terminal on the heap and
 * initialise it to (cols x rows). Unlike using the shared terminal_backend_cfr
 * global directly, this yields a distinct instance suitable for a second,
 * PTY-less terminal (e.g. the pager overlay). Returns NULL on failure.
 * Free with terminal_destroy() followed by free(). */
TerminalBackend *term_cfr_new(const CfrConfig *cfg);

#endif /* TERM_CFR_H */
