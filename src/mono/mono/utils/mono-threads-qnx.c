/**
 * \file
 * Thread support for QNX Neutrino.
 *
 * Copyright (c) Xia Zhongyang.
 * Licensed under the MIT License.
 */

#include <config.h>

#if defined(HOST_QNX)

#include <mono/utils/mono-threads.h>
#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/procfs.h>
#include <unistd.h>

/*
 * QNX has no pthread_getattr_np, but the process manager reports the stack of
 * any thread of the process, the main thread included, through /proc.
 */
void
mono_threads_platform_get_stack_bounds (guint8 **staddr, size_t *stsize)
{
	procfs_status status;
	int fd;

	*staddr = NULL;
	*stsize = (size_t)-1;

	fd = open ("/proc/self/as", O_RDONLY);
	if (fd < 0)
		return;
	memset (&status, 0, sizeof (status));
	status.tid = pthread_self ();
	if (devctl (fd, DCMD_PROC_TIDSTATUS, &status, sizeof (status), NULL) == EOK && status.tid == pthread_self ()) {
		*staddr = (guint8 *)(gsize)status.stkbase;
		*stsize = status.stksize;
	}
	close (fd);
}

guint64
mono_native_thread_os_id_get (void)
{
	/* Thread ids are small integers, unique within the process. */
	return (guint64)pthread_self ();
}

#else

#include <mono/utils/mono-compiler.h>

MONO_EMPTY_SOURCE_FILE (mono_threads_qnx);

#endif
