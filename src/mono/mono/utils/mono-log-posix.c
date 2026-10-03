/**
 * \file
 * POSIX interface to the logger
 *
 * This module contains the POSIX syslog logger routines
 *
 * Author:
 *    Neale Ferguson <neale@sinenomine.net>
 *
 */
#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#if defined(_POSIX_VERSION) && !defined(HOST_WASI)

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <glib.h>
#ifdef HAVE_SYSLOG_H
#include <syslog.h>
#endif
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include "mono-logger-internals.h"

static void *logUserData = NULL;

#ifdef HAVE_SYSLOG_H
/**
 * mapSyslogLevel:
 *
 * 	@level - GLogLevelFlags value
 * 	@returns The equivalent syslog priority value
 */
static __inline__ int
mapSyslogLevel(GLogLevelFlags level)
{
	if (level & G_LOG_LEVEL_ERROR)
		return (LOG_ERR);
	if (level & G_LOG_LEVEL_CRITICAL)
		return (LOG_CRIT);
	if (level & G_LOG_LEVEL_WARNING)
		return (LOG_WARNING);
	if (level & G_LOG_LEVEL_MESSAGE)
		return (LOG_NOTICE);
	if (level & G_LOG_LEVEL_INFO)
		return (LOG_INFO);
	if (level & G_LOG_LEVEL_DEBUG)
		return (LOG_DEBUG);
	return (LOG_INFO);
}
#endif

/**
 * mono_log_open_syslog:
 * \param ident Identifier: ignored
 * \param userData Not used
 * Open the syslog interface specifying that we want our PID recorded
 * and that we're using the \c LOG_USER facility.
 */
void
mono_log_open_syslog(const char *ident, void *userData)
{
#if defined(HAVE_SYSLOG_H) && defined(HAVE_OPENLOG)
	openlog("mono", LOG_PID, LOG_USER);
#endif
	logUserData = userData;
}

/**
 * mono_log_write_syslog:
 * \param domain Identifier string
 * \param level Logging level flags
 * \param format \c printf format string
 * \param vargs Variable argument list
 * Write data to the log file.
 */
void
mono_log_write_syslog(const char *domain, GLogLevelFlags level, mono_bool hdr, const char *message)
{
#ifdef HAVE_SYSLOG_H
	syslog (mapSyslogLevel(level), "%s", message);
#else
	/* No <syslog.h>: the messages go where they go with no log destination. */
	mono_log_write_logfile (domain, level, hdr, message);
#endif

	if (level & G_LOG_LEVEL_ERROR)
		g_assert_abort ();
}

/**
 * mono_log_close_syslog:
 * Close the log file
 */
void
mono_log_close_syslog(void)
{
#if defined(HAVE_SYSLOG_H) && defined(HAVE_CLOSELOG)
	closelog();
#endif
}

#else

#include <mono/utils/mono-compiler.h>

MONO_EMPTY_SOURCE_FILE (mono_log_posix);

#endif
