/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "censr_log.h"

#include <cstdio>

extern "C" {

static void censr_default_log_handler(int level, const char *msg)
{
	const char *tag = level <= LOG_ERROR	   ? "error"
			  : level <= LOG_WARNING   ? "warning"
			  : level <= LOG_INFO	   ? "info"
						   : "debug";
	fprintf(stderr, "[censr:%s] %s\n", tag, msg);
}

static censr_log_handler g_handler = censr_default_log_handler;

void censr_set_log_handler(censr_log_handler handler)
{
	g_handler = handler ? handler : censr_default_log_handler;
}

void censr_log(int level, const char *format, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	if (g_handler)
		g_handler(level, buf);
}

} // extern "C"
