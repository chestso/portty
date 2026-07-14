#ifndef DISPLAY_INFO_H
#define DISPLAY_INFO_H

// Collect physical screen specs for the diagnostics report.
// Returns a human-readable string like "1920x1080, 309x174 mm (eDP-1)"
// or NULL if the information could not be determined.
// The returned pointer is valid until the next call and must not be freed.
const char *display_info_get_physical(void);

#endif // DISPLAY_INFO_H
