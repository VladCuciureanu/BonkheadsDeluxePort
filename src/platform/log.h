#pragma once

void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_fatal(const char *fmt, ...);
/* verbose trace, enabled with BONK_TRACE=1 */
void log_trace(const char *fmt, ...);
extern int g_log_trace;
extern int g_headless;   /* no message boxes (automated runs) */
