// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include "pal_config.h"
#include "pal_errno.h"
#include "pal_threading.h"

#include <limits.h>
#include <sched.h>
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <minipal/conditionvariable.h>
#include <minipal/mutex.h>
#include <minipal/thread.h>
#if HAVE_SCHED_GETCPU
#include <sched.h>
#endif

#if defined(TARGET_LINUX)
#include <linux/futex.h>      /* Definition of FUTEX_* constants */
#include <sys/syscall.h>      /* Definition of SYS_* constants */
#include <unistd.h>           /* Declaration of syscall */
#endif

#include <pthread.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wjump-misses-init"
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// LowLevelMonitor - Represents a non-recursive mutex and condition

struct LowLevelMonitor
{
    minipal_nonrecursive_mutex Mutex;
    minipal_condition_variable Condition;
#ifdef DEBUG
    bool IsLocked;
#endif
};

static void SetIsLocked(LowLevelMonitor* monitor, bool isLocked)
{
#ifdef DEBUG
    assert(monitor->IsLocked != isLocked);
    monitor->IsLocked = isLocked;
#else
    (void)monitor; // unused in release build
    (void)isLocked; // unused in release build
#endif
}

LowLevelMonitor* SystemNative_LowLevelMonitor_Create(void)
{
    LowLevelMonitor* monitor = (LowLevelMonitor *)malloc(sizeof(LowLevelMonitor));
    if (monitor == NULL)
    {
        return NULL;
    }

    if (!minipal_nonrecursive_mutex_init(&monitor->Mutex))
    {
        free(monitor);
        return NULL;
    }

    if (!minipal_condition_variable_init(&monitor->Condition))
    {
        minipal_nonrecursive_mutex_destroy(&monitor->Mutex);
        free(monitor);
        return NULL;
    }

#ifdef DEBUG
    monitor->IsLocked = false;
#endif

    return monitor;
}

void SystemNative_LowLevelMonitor_Destroy(LowLevelMonitor* monitor)
{
    assert(monitor != NULL);

    minipal_condition_variable_destroy(&monitor->Condition);
    minipal_nonrecursive_mutex_destroy(&monitor->Mutex);

    free(monitor);
}

void SystemNative_LowLevelMonitor_Acquire(LowLevelMonitor* monitor)
{
    assert(monitor != NULL);

    minipal_nonrecursive_mutex_enter(&monitor->Mutex);

    SetIsLocked(monitor, true);
}

void SystemNative_LowLevelMonitor_Release(LowLevelMonitor* monitor)
{
    assert(monitor != NULL);

    SetIsLocked(monitor, false);

    minipal_nonrecursive_mutex_leave(&monitor->Mutex);
}

void SystemNative_LowLevelMonitor_Wait(LowLevelMonitor* monitor)
{
    assert(monitor != NULL);

    SetIsLocked(monitor, false);

    minipal_condition_variable_result result =
        minipal_condition_variable_wait_nonrecursive(
            &monitor->Condition,
            &monitor->Mutex,
            MINIPAL_CONDITION_VARIABLE_INFINITE);
    assert(result == MINIPAL_CONDITION_VARIABLE_SIGNALED);

    SetIsLocked(monitor, true);
}

int32_t SystemNative_LowLevelMonitor_TimedWait(LowLevelMonitor *monitor, int32_t timeoutMilliseconds)
{
    assert(timeoutMilliseconds >= 0);

    SetIsLocked(monitor, false);

    minipal_condition_variable_result result =
        minipal_condition_variable_wait_nonrecursive(
            &monitor->Condition,
            &monitor->Mutex,
            (uint32_t)timeoutMilliseconds);
    assert(
        result == MINIPAL_CONDITION_VARIABLE_SIGNALED ||
        result == MINIPAL_CONDITION_VARIABLE_TIMED_OUT);

    SetIsLocked(monitor, true);

    return result == MINIPAL_CONDITION_VARIABLE_SIGNALED;
}

void SystemNative_LowLevelMonitor_Signal_Release(LowLevelMonitor* monitor)
{
    assert(monitor != NULL);

    bool result = minipal_condition_variable_signal(&monitor->Condition);
    assert(result);

    SetIsLocked(monitor, false);

    minipal_nonrecursive_mutex_leave(&monitor->Mutex);
}

#if defined(TARGET_LINUX)
void SystemNative_LowLevelFutex_WaitOnAddress(int32_t* address, int32_t comparand)
{
    syscall(SYS_futex, address, FUTEX_WAIT_PRIVATE, comparand, NULL, NULL, 0);
}

int32_t SystemNative_LowLevelFutex_WaitOnAddressTimeout(int32_t* address, int32_t comparand, int32_t timeoutMilliseconds)
{
    assert(timeoutMilliseconds >= 0);

    struct timespec timeoutTimeSpec;
    timeoutTimeSpec.tv_sec  = (uint32_t)timeoutMilliseconds / 1000;
    timeoutTimeSpec.tv_nsec = ((uint32_t)timeoutMilliseconds % 1000) * 1000 * 1000;

    // the timeoutTimeSpec is relative timeout with CLOCK_MONOTONIC clock by default.
    long waitResult = syscall(SYS_futex, address, FUTEX_WAIT_PRIVATE, comparand, &timeoutTimeSpec, NULL, 0);

    // possible results: woken, not blocking, interrupted, timeout
    assert(waitResult == 0 || errno == EAGAIN || errno == EINTR || errno == ETIMEDOUT);

    // normal/immediate/spurious wakes are not timeouts
    // in release treat unexpected results as spurious wakes
    return waitResult == 0 || errno != ETIMEDOUT;
}

void SystemNative_LowLevelFutex_WakeByAddressSingle(int32_t* address)
{
    syscall(SYS_futex, address, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}
#elif defined(__QNXNTO__)
// QNX has no futex, but the Linux CoreLib QNX runs waits on these, so a no-op
// would leave its waiters spinning. Waiting on an address is a mutex and
// condition variable from a small table, chosen by the address: the value is
// compared with the mutex held, and a waker takes the same mutex before it
// signals, so no wake is lost. Several addresses share a condition variable,
// so a wake wakes every waiter on it; callers treat that as a spurious wake.
#define FUTEX_TABLE_SIZE 64

typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} FutexBucket;

static FutexBucket s_futexTable[FUTEX_TABLE_SIZE];
static pthread_once_t s_futexTableOnce = PTHREAD_ONCE_INIT;

static void InitializeFutexTable(void)
{
    pthread_condattr_t condAttr;
    int error = pthread_condattr_init(&condAttr);
    assert(error == 0);
    // Timeouts are measured on the monotonic clock, as Linux's futex measures them.
    error = pthread_condattr_setclock(&condAttr, CLOCK_MONOTONIC);
    assert(error == 0);

    for (int i = 0; i < FUTEX_TABLE_SIZE; i++)
    {
        error = pthread_mutex_init(&s_futexTable[i].mutex, NULL);
        assert(error == 0);
        error = pthread_cond_init(&s_futexTable[i].cond, &condAttr);
        assert(error == 0);
    }

    pthread_condattr_destroy(&condAttr);
    (void)error;
}

static FutexBucket* GetFutexBucket(int32_t* address)
{
    pthread_once(&s_futexTableOnce, InitializeFutexTable);
    return &s_futexTable[((uintptr_t)address >> 2) % FUTEX_TABLE_SIZE];
}

void SystemNative_LowLevelFutex_WaitOnAddress(int32_t* address, int32_t comparand)
{
    FutexBucket* bucket = GetFutexBucket(address);

    pthread_mutex_lock(&bucket->mutex);
    if (*(volatile int32_t*)address == comparand)
    {
        pthread_cond_wait(&bucket->cond, &bucket->mutex);
    }
    pthread_mutex_unlock(&bucket->mutex);
}

int32_t SystemNative_LowLevelFutex_WaitOnAddressTimeout(int32_t* address, int32_t comparand, int32_t timeoutMilliseconds)
{
    assert(timeoutMilliseconds >= 0);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeoutMilliseconds / 1000;
    deadline.tv_nsec += (timeoutMilliseconds % 1000) * 1000 * 1000;
    if (deadline.tv_nsec >= 1000 * 1000 * 1000)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000 * 1000 * 1000;
    }

    FutexBucket* bucket = GetFutexBucket(address);
    int waitResult = 0;

    pthread_mutex_lock(&bucket->mutex);
    if (*(volatile int32_t*)address == comparand)
    {
        waitResult = pthread_cond_timedwait(&bucket->cond, &bucket->mutex, &deadline);
    }
    pthread_mutex_unlock(&bucket->mutex);

    // normal/immediate/spurious wakes are not timeouts
    return waitResult != ETIMEDOUT;
}

void SystemNative_LowLevelFutex_WakeByAddressSingle(int32_t* address)
{
    FutexBucket* bucket = GetFutexBucket(address);

    pthread_mutex_lock(&bucket->mutex);
    pthread_cond_broadcast(&bucket->cond);
    pthread_mutex_unlock(&bucket->mutex);
}
#else // defined(TARGET_LINUX)

// On illumos/Solaris libc's assert is not annotated noreturn, so marking these stubs noreturn would
// trigger -Winvalid-noreturn there. Only apply the attribute on other platforms.
#if defined(DEBUG) && !defined(TARGET_SUNOS)
#define DEBUGNOTRETURN __attribute__((noreturn))
#else
#define DEBUGNOTRETURN
#endif

DEBUGNOTRETURN
void SystemNative_LowLevelFutex_WaitOnAddress(int32_t* address, int32_t comparand)
{
    (void)address; // unused
    (void)comparand; // unused
    assert_msg(false, "Futex is not supported on this platform", 0);
    // trivial implementation of Wait always wakes spuriously.
}

DEBUGNOTRETURN
int32_t SystemNative_LowLevelFutex_WaitOnAddressTimeout(int32_t* address, int32_t comparand, int32_t timeoutMilliseconds)
{
    (void)address; // unused
    (void)comparand; // unused
    (void)timeoutMilliseconds; // unused
    assert_msg(false, "Futex is not supported on this platform", 0);
#if !defined(DEBUG) || defined(TARGET_SUNOS)
    // trivial implementation of Wait always wakes spuriously.
    return 1;
#endif
}

DEBUGNOTRETURN
void SystemNative_LowLevelFutex_WakeByAddressSingle(int32_t* address)
{
    (void)address; // unused
    assert_msg(false, "Futex is not supported on this platform", 0);
    // trivial implementation of Wake does nothing.
}

#undef DEBUGNOTRETURN

#endif  // defined(TARGET_LINUX), defined(__QNXNTO__)

int32_t SystemNative_CreateThread(uintptr_t stackSize, void *(*startAddress)(void*), void *parameter)
{
    bool result = false;
    pthread_attr_t attrs;

    int error = pthread_attr_init(&attrs);
    if (error != 0)
    {
        // Do not call pthread_attr_destroy
        return false;
    }

    error = pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);
    assert(error == 0);

#ifdef HOST_APPLE
    // Match Windows stack size
    if (stackSize == 0)
    {
        stackSize = 1536 * 1024;
    }
#endif

    if (stackSize > 0)
    {
        if (stackSize < (uintptr_t)PTHREAD_STACK_MIN)
        {
            stackSize = (uintptr_t)PTHREAD_STACK_MIN;
        }

        error = pthread_attr_setstacksize(&attrs, stackSize);
        if (error != 0) goto CreateThreadExit;
    }

    pthread_t threadId;
    error = pthread_create(&threadId, &attrs, startAddress, parameter);
    if (error != 0) goto CreateThreadExit;

    result = true;

CreateThreadExit:
    error = pthread_attr_destroy(&attrs);
    assert(error == 0);

    return result;
}

int32_t SystemNative_SchedGetCpu(void)
{
#if HAVE_SCHED_GETCPU
    return sched_getcpu();
#else
    return -1;
#endif
}

__attribute__((noreturn))
void SystemNative_Exit(int32_t exitCode)
{
    exit(exitCode);
}

__attribute__((noreturn))
void SystemNative_Abort(void)
{
    abort();
}

uint64_t SystemNative_GetCurrentThreadId(void)
{
    return minipal_get_current_thread_id();
}
