#ifndef COMMON_H
#define COMMON_H

#include <stdarg.h>
#include <stdbool.h>

/* Global verbose flag - defined in main.c */
extern int verbose;

/* Notification panel transparency (defined in main.c, set from config). When
 * false (default) the top notification panel is fully opaque; when true it is
 * drawn translucent (SDL3: alpha background; GTK4: a translucent .osd surface). */
extern bool portty_notification_transparent;

/* kitty-style text_composition_strategy knob (defined in main.c, set from
 * config). Applied as a uniform coverage curve to grayscale glyph alpha in
 * font_ft.c, on top of the renderer's linear-light blending. Neutral defaults
 * (gamma 1.0, contrast 0) leave coverage unchanged. */
extern float portty_text_gamma;
extern float portty_text_contrast;

/* Verbose logging implementation - use vlog() macro instead */
void vlog_impl(const char *file, const char *func, int line, const char *format, ...);

/* Verbose logging macro - captures file, function, and line number */
#define vlog(fmt, ...) vlog_impl(__FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)

/* Shared constants */
#define CURSOR_BLINK_INTERVAL_MS 1000
#define SCROLL_LINES_PER_TICK    1
#define AUTOSCROLL_INTERVAL_MS   33

#endif /* COMMON_H */
