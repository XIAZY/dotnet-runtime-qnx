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
// unlinks).
//
// Across processes: under the in-process lock, an fcntl record lock on
// socket.lock in qnxhost-<uid>, the private per-user directory qnxhost makes
// in $TMPDIR (/tmp when it is unset or empty; a TMPDIR that already names such
// a directory is that directory), is taken shared around the same calls and
// exclusively around the unlink of a socket name, so that every process of one
// user that uses this library keeps its unlinks apart from the others' socket
// calls (a program and its child processes, for example): deleting a socket
// name in one process while another process makes socket calls can deadlock
// io-pkt, and an in-process lock cannot cover that. A pair of fcntl calls
// costs 16-20 us on QNX 6.5. Record locks belong to the process, not the
// thread: the first thread to take the shared side takes the file's read lock
// and the last one releases it; and they vanish when the process closes any
// descriptor on the file, so the file is opened once, on a descriptor kept for
// the process's lifetime. qnxhost makes the directory in every configuration;
// only if it cannot (or the directory is not this user's alone) is the file
// lock left out, with a warning. Other users' processes and foreign programs
// (sshd) are not covered.

#include "pal_config.h"
#include "pal_socklock_qnx.h"

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_rwlock_t g_qnxSocketLock = PTHREAD_RWLOCK_INITIALIZER;

// The cross-process side: the lock file's descriptor (-1 without one), and
// how many threads of this process hold the shared side.
static pthread_once_t g_qnxLockFileOnce = PTHREAD_ONCE_INIT;
static int g_qnxLockFile = -1;
static pthread_mutex_t g_qnxLockFileMutex = PTHREAD_MUTEX_INITIALIZER;
static int g_qnxLockFileShared;

static void QnxOpenLockFile(void)
{
    char dir[PATH_MAX], path[PATH_MAX], name[32];
    const char* tmp = getenv("TMPDIR");
    size_t len, nameLen;
    struct stat st;

    // The same rule as qnxhost's private_tmpdir.
    if (tmp == NULL || tmp[0] == '\0')
    {
        tmp = "/tmp";
    }
    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
    {
        len--;
    }
    nameLen = (size_t)snprintf(name, sizeof(name), "qnxhost-%u", (unsigned)getuid());
    if (len > nameLen && tmp[len - nameLen - 1] == '/' && strncmp(tmp + len - nameLen, name, nameLen) == 0)
    {
        snprintf(dir, sizeof(dir), "%.*s", (int)len, tmp);
    }
    else if (snprintf(dir, sizeof(dir), "%.*s/%s", (int)len, tmp, name) >= (int)sizeof(dir))
    {
        fprintf(stderr, "System.Native: TMPDIR is too long; socket names are locked within this process only\n");
        return;
    }
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0)
    {
        fprintf(stderr, "System.Native: no private directory %s; socket names are locked within this process only\n", dir);
        return;
    }
    snprintf(path, sizeof(path), "%s/socket.lock", dir);
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd == -1)
    {
        fprintf(stderr, "System.Native: cannot open %s (%s); socket names are locked within this process only\n", path, strerror(errno));
        return;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    g_qnxLockFile = fd;
}

static void QnxLockFile(short type)
{
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    while (fcntl(g_qnxLockFile, type == F_UNLCK ? F_SETLK : F_SETLKW, &fl) == -1 && errno == EINTR);
}

// Takes the shared side of the file lock; called with the in-process lock held shared.
static void QnxFileLockShared(void)
{
    pthread_once(&g_qnxLockFileOnce, QnxOpenLockFile);
    if (g_qnxLockFile == -1)
    {
        return;
    }
    pthread_mutex_lock(&g_qnxLockFileMutex);
    if (g_qnxLockFileShared++ == 0)
    {
        QnxLockFile(F_RDLCK);
    }
    pthread_mutex_unlock(&g_qnxLockFileMutex);
}

static void QnxFileUnlockShared(void)
{
    if (g_qnxLockFile == -1)
    {
        return;
    }
    pthread_mutex_lock(&g_qnxLockFileMutex);
    if (--g_qnxLockFileShared == 0)
    {
        QnxLockFile(F_UNLCK);
    }
    pthread_mutex_unlock(&g_qnxLockFileMutex);
}

void QnxSocketLockShared(void)
{
    int error = errno;
    pthread_rwlock_rdlock(&g_qnxSocketLock);
    QnxFileLockShared();
    errno = error;
}

void QnxSocketUnlockShared(void)
{
    int error = errno;
    QnxFileUnlockShared();
    pthread_rwlock_unlock(&g_qnxSocketLock);
    errno = error;
}

int QnxSocketLockSharedIfNonBlocking(int fd)
{
    int error = errno;
    pthread_rwlock_rdlock(&g_qnxSocketLock);
    int flags = fcntl(fd, F_GETFL);
    if (flags != -1 && (flags & O_NONBLOCK) != 0)
    {
        QnxFileLockShared();
        errno = error;
        return 1;
    }
    pthread_rwlock_unlock(&g_qnxSocketLock);
    errno = error;
    return 0;
}

int QnxUnlink(const char* path)
{
    struct stat st;

    // Looking up a socket's name is a request to io-pkt too.
    QnxSocketLockShared();
    int isSocket = lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
    QnxSocketUnlockShared();
    if (!isSocket)
    {
        return unlink(path);
    }

    pthread_rwlock_wrlock(&g_qnxSocketLock);
    pthread_once(&g_qnxLockFileOnce, QnxOpenLockFile);
    if (g_qnxLockFile != -1)
    {
        QnxLockFile(F_WRLCK);
    }
    int result = unlink(path);
    int error = errno;
    if (g_qnxLockFile != -1)
    {
        QnxLockFile(F_UNLCK);
    }
    pthread_rwlock_unlock(&g_qnxSocketLock);
    errno = error;
    return result;
}
