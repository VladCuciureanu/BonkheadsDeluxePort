#include "log.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int g_log_trace;
int g_headless;

static void vlog(const char *tag, const char *fmt, va_list ap)
{
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    fprintf(stderr, "[%s] %s\n", tag, buf);
    fflush(stderr);
}

void log_info(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog("info", fmt, ap); va_end(ap); }
void log_warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog("warn", fmt, ap); va_end(ap); }

void log_trace(const char *fmt, ...)
{
    if (!g_log_trace) return;
    va_list ap; va_start(ap, fmt); vlog("trace", fmt, ap); va_end(ap);
}

void log_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[fatal] %s\n", buf);
    fflush(stderr);
    if (!g_headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Bonkheads", buf, NULL);
    exit(1);
}
