#ifndef BACKEND_SOKOL_H
#define BACKEND_SOKOL_H

#include "portty_backend.h"
#include "portty_app.h"
#include <sokol/sokol_app.h>

#ifdef __cplusplus
extern "C" {
#endif

extern PorttyBackend backend_sokol;

// Launch configuration that must outlive sokol_main() because sokol_app
// calls the init callback after sokol_main returns.  main_sokol fills this
// in and the backend callbacks consume it.
typedef struct
{
    const char *demo_text;
    const char *font_name;
    char **exec_argv;
    int ft_hint_target;
    float font_size;
    int init_cols;
    int init_rows;
} SokolLaunchConfig;

// Build the sapp_desc that sokol_main returns.  The backend stores the
// PorttyApp and PorttyBackend pointers in its global state and uses them
// from the sokol callbacks.
sapp_desc backend_sokol_desc(PorttyApp *app, PorttyBackend *backend,
                             const char *title, int width, int height);

// Stash the launch configuration so the sokol init callback can finish
// backend setup after sokol_app has created the window/GL context.
void backend_sokol_set_launch_config(const SokolLaunchConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif // BACKEND_SOKOL_H
