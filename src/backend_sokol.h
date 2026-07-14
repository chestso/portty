#ifndef BACKEND_SOKOL_H
#define BACKEND_SOKOL_H

#include "portty_backend.h"
#include "portty_app.h"
#include <sokol/sokol_app.h>

#ifdef __cplusplus
extern "C" {
#endif

extern PorttyBackend backend_sokol;

// Build the sapp_desc that sokol_main returns.  The backend stores the
// PorttyApp and PorttyBackend pointers in its global state and uses them
// from the sokol callbacks.  All config values are read from PorttyApp.
sapp_desc backend_sokol_desc(PorttyApp *app, PorttyBackend *backend,
                             const char *title, int width, int height);

#ifdef __cplusplus
}
#endif

#endif // BACKEND_SOKOL_H
