#include "common.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int verbose = 0;

void vlog_impl(const char *file, const char *func, int line, const char *format, ...)
{
    if (!verbose)
        return;
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[%s:%s:%d] ", file, func, line);
    vfprintf(stderr, format, args);
    va_end(args);
}

void test_parse_args(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
    }
}
