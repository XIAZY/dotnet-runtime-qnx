// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

#pragma once

// QNX 6.5 can freeze as a whole when a resource manager copies a reply into
// a page of the calling process that has not been touched yet, while a
// page-in for the same process waits on the same resource manager: the
// manager faults on the caller's page and needs the address-space lock that
// the page-in holds (QNX SDP 6.5.0 release notes, known issue Ref# 62483;
// reproduced with devb-eide, and with a page-in of a different file than the
// one being read). Memory that is committed but not yet touched counts as
// untouched, so a fresh managed array, a malloc'd buffer or a deep stack page
// qualify. Touching the caller's memory before the call, a store for memory
// the call writes and a load for memory it reads, makes the manager's copy
// find the pages present. It is done on every QNX target, since it costs one
// access per page and the bug is not known to be absent elsewhere.
//
// Only bytes inside [buf, buf + len) are accessed: the first, the first of
// each page that starts inside the range, and the last. A byte of the same
// page outside the range may belong to another object that another thread is
// writing. The accesses are volatile so that the compiler keeps them.

#include <stddef.h>
#include <stdint.h>

#if defined(__QNXNTO__)
#include <sys/uio.h>
#include <unistd.h>

static inline size_t QnxTouchPageSize(void)
{
    static size_t pageSize;
    if (pageSize == 0)
    {
        long size = sysconf(_SC_PAGESIZE);
        pageSize = size > 0 ? (size_t)size : 4096;
    }
    return pageSize;
}

// Before a call that writes into [buf, buf + len).
static inline void QnxTouchToWrite(void* buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return;
    }
    size_t page = QnxTouchPageSize();
    volatile char* first = (volatile char*)buf;
    volatile char* last = first + (len - 1);
    *first = *first;
    uintptr_t end = (uintptr_t)last;
    // The first page start after `first`, then one per page; stops before
    // stepping past `last`, so the address never wraps.
    for (uintptr_t p = ((uintptr_t)first | (uintptr_t)(page - 1)) + 1; p != 0 && p <= end; p += page)
    {
        volatile char* q = (volatile char*)p;
        *q = *q;
        if (end - p < page)
        {
            break;
        }
    }
    *last = *last;
}

// Before a call that reads from [buf, buf + len).
static inline void QnxTouchToRead(const void* buf, size_t len)
{
    if (buf == NULL || len == 0)
    {
        return;
    }
    size_t page = QnxTouchPageSize();
    const volatile char* first = (const volatile char*)buf;
    const volatile char* last = first + (len - 1);
    (void)*first;
    uintptr_t end = (uintptr_t)last;
    for (uintptr_t p = ((uintptr_t)first | (uintptr_t)(page - 1)) + 1; p != 0 && p <= end; p += page)
    {
        (void)*(const volatile char*)p;
        if (end - p < page)
        {
            break;
        }
    }
    (void)*last;
}

static inline void QnxTouchIovToWrite(const struct iovec* iov, int count)
{
    for (int i = 0; i < count; i++)
    {
        QnxTouchToWrite(iov[i].iov_base, iov[i].iov_len);
    }
}

static inline void QnxTouchIovToRead(const struct iovec* iov, int count)
{
    for (int i = 0; i < count; i++)
    {
        QnxTouchToRead(iov[i].iov_base, iov[i].iov_len);
    }
}
#else
#define QnxTouchToWrite(buf, len) ((void)0)
#define QnxTouchToRead(buf, len) ((void)0)
#define QnxTouchIovToWrite(iov, count) ((void)0)
#define QnxTouchIovToRead(iov, count) ((void)0)
#endif
