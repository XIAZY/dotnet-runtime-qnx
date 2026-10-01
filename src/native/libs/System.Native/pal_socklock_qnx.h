// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

#pragma once

#if defined(__QNXNTO__)

#include <errno.h>

// QNX 6.5's io-pkt stops answering for good if a Unix socket's name is
// unlinked while it serves another socket request; see pal_socklock_qnx.c.
// Socket control calls take a process-wide lock shared, and unlinking a
// socket's name takes it exclusively.

void QnxSocketLockShared(void);
void QnxSocketUnlockShared(void);

// Takes the shared lock if fd is non-blocking and returns 1; returns 0
// without the lock for a blocking descriptor, whose call may wait.
int QnxSocketLockSharedIfNonBlocking(int fd);

// unlink(), exclusive of every locked socket call if path names a socket.
int QnxUnlink(const char* path);

// Runs a socket call under the shared lock, keeping the call's errno.
#define QNX_SOCKET_LOCKED(type, call) \
    __extension__({ \
        QnxSocketLockShared(); \
        type qnxResult_ = (call); \
        int qnxErrno_ = errno; \
        QnxSocketUnlockShared(); \
        errno = qnxErrno_; \
        qnxResult_; \
    })

// The same for a call that can wait on a blocking descriptor (accept,
// connect): locked only if fd is non-blocking, so that the lock is never
// held across a wait.
#define QNX_SOCKET_LOCKED_IF_NONBLOCKING(type, fd, call) \
    __extension__({ \
        int qnxLocked_ = QnxSocketLockSharedIfNonBlocking(fd); \
        type qnxResult_ = (call); \
        int qnxErrno_ = errno; \
        if (qnxLocked_) QnxSocketUnlockShared(); \
        errno = qnxErrno_; \
        qnxResult_; \
    })

#endif
