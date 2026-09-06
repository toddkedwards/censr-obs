/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

/* Tiny logging shim for censr-core. The engine has no libobs dependency, so it
 * cannot call obs_log directly; instead it calls censr_log(), and each host
 * installs a sink:
 *   - the OBS plugin forwards to obs_log (see plugin-main.c);
 *   - the standalone virtual-camera host can log to a file / stderr.
 * The default sink (before any handler is installed) writes to stderr. */

#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log levels mirror libobs util/base.h so a handler can forward them verbatim.
 * Guarded so including this alongside an obs header (which also defines them) is
 * harmless. */
#ifndef LOG_ERROR
#define LOG_ERROR 100
#define LOG_WARNING 200
#define LOG_INFO 300
#define LOG_DEBUG 400
#endif

/* Receives an already-formatted message. */
typedef void (*censr_log_handler)(int level, const char *msg);

/* Install the log sink; pass NULL to restore the default (stderr). */
void censr_set_log_handler(censr_log_handler handler);

/* printf-style; formats and dispatches to the installed handler. */
void censr_log(int level, const char *format, ...);

#ifdef __cplusplus
}
#endif
