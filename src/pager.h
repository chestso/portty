#ifndef PAGER_H
#define PAGER_H

#include "platform.h"
#include "rend.h"
#include "term.h"
#include <stdbool.h>
#include <stdint.h>

// General-purpose in-process pager. It displays a styled ANSI document
// (SGR/truecolor, attributes, and OSC 8 hyperlinks are all honoured) full-screen
// as a scrollable overlay, rendered by bloom-terminal itself rather than an
// external pager. Content is parsed by a dedicated, PTY-less bloom-vt terminal,
// so links become clickable cells and history pages through bloom-vt scrollback.
//
// The host wires platform input to the pager_* handlers while it is active and
// must NOT forward consumed events to the shell. The pager pauses/resumes the
// PTY across an open session so background output stays frozen behind it.
typedef struct Pager Pager;

// Create a pager bound to a renderer (overlay drawing + cell metrics) and
// platform (link opening, cursor, PTY pause). Returns NULL on allocation
// failure.
Pager *pager_create(RendererBackend *rend, PlatformBackend *plat);
void pager_destroy(Pager *p);

// Open the pager showing `ansi_text` (NUL-terminated; copied internally) over a
// `cols` x `rows` grid. Bare LF is normalised to CRLF for the VT. Opens at the
// top of the document. Returns false on failure (the pager stays closed).
bool pager_open(Pager *p, const char *ansi_text, int cols, int rows);

// Close the pager (no-op if inactive); resumes the PTY.
void pager_close(Pager *p);

bool pager_active(const Pager *p);

// Input handlers, valid while active. Each returns true if it consumed the
// event (the host must then not forward it). All are no-ops returning false
// when inactive. `key` is a TERM_KEY_* value; `codepoint` carries character
// shortcuts (q/g/G/b/j/k/space) when `key` is TERM_KEY_NONE.
bool pager_key(Pager *p, int key, int mod, uint32_t codepoint);
bool pager_scroll(Pager *p, int delta);
bool pager_mouse(Pager *p, int pixel_x, int pixel_y, int button, bool pressed, int clicks,
                 int mod);

// Rebuild the overlay at a new grid size from the retained document (call on
// window resize while active). Resets the view to the top.
void pager_resize(Pager *p, int cols, int rows);

#endif // PAGER_H
