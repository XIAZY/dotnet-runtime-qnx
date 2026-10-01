// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

// QNX 6.5's io-pkt stops answering for good, until a reboot, if a Unix
// socket's name is unlinked while it serves another socket request, from
// any process and for any protocol (reproduced on QNX 6.5.0 with a small
// test program: io-pkt deadlocked in its second round, and every process
// that sent it a request blocked). A process-wide
// read-write lock keeps this process's unlinks of socket names from
// overlapping its own socket calls:
// - the socket control calls in pal_networking.c (socket, socketpair, bind,
//   listen, connect, accept, shutdown, get/setsockopt, getsockname,
//   getpeername) and close take it shared, around the call only; accept and
//   connect only on a non-blocking descriptor, so that it is never held
//   across a wait;
// - unlink of a name that lstat reports as a socket takes it exclusively.
// Reads, writes, sends and receives stay unlocked: 700,440 of them
// overlapping 20,000 locked unlinks caused no hang, and neither did the
// socket event port's ionotify arms (about 49,000 overlapping 4,000 locked
// unlinks). Other processes are not covered.

#include "pal_config.h"
#include "pal_socklock_qnx.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_rwlock_t g_qnxSocketLock = PTHREAD_RWLOCK_INITIALIZER;

void QnxSocketLockShared(void)
{
    pthread_rwlock_rdlock(&g_qnxSocketLock);
}

void QnxSocketUnlockShared(void)
{
    pthread_rwlock_unlock(&g_qnxSocketLock);
}

int QnxSocketLockSharedIfNonBlocking(int fd)
{
    pthread_rwlock_rdlock(&g_qnxSocketLock);
    int flags = fcntl(fd, F_GETFL);
    if (flags != -1 && (flags & O_NONBLOCK) != 0)
    {
        return 1;
    }
    pthread_rwlock_unlock(&g_qnxSocketLock);
    return 0;
}

int QnxUnlink(const char* path)
{
    struct stat st;

    pthread_rwlock_rdlock(&g_qnxSocketLock);
    int isSocket = lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
    pthread_rwlock_unlock(&g_qnxSocketLock);
    if (!isSocket)
    {
        return unlink(path);
    }

    pthread_rwlock_wrlock(&g_qnxSocketLock);
    int result = unlink(path);
    int error = errno;
    pthread_rwlock_unlock(&g_qnxSocketLock);
    errno = error;
    return result;
}
