#ifndef COMMON_H
#define COMMON_H

#include <stdarg.h>

/* Global verbose flag - defined in main.c */
extern int verbose;

/* Verbose logging implementation - use vlog() macro instead */
void vlog_impl(const char *file, const char *func, int line, const char *format, ...);

/* Verbose logging macro - captures file, function, and line number */
#define vlog(fmt, ...) vlog_impl(__FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)

/* Shared constants */
#define CURSOR_BLINK_INTERVAL_MS 1000
#define SCROLL_LINES_PER_TICK    3

#endif /* COMMON_H */
