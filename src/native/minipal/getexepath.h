// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#ifndef HAVE_MINIPAL_GETEXEPATH_H
#define HAVE_MINIPAL_GETEXEPATH_H

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__FreeBSD__)
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__HAIKU__)
#include <FindDirectory.h>
#include <StorageDefs.h>
#elif defined(__QNXNTO__)
#include <devctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/procfs.h>
#include <unistd.h>
#elif HAVE_GETAUXVAL
#include <sys/auxv.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get the full path to the executable for the current process.
 * Resolves symbolic links. The caller is responsible for releasing the buffer.
 *
 * @return A pointer to a null-terminated string containing the executable path, 
 *         or NULL if an error occurs.
 */
static inline char* minipal_getexepath(void)
{
#if defined(__APPLE__)
    uint32_t len = PATH_MAX;
    char pathBuf[PATH_MAX];
    if (_NSGetExecutablePath(pathBuf, &len) != 0)
    {
        errno = EINVAL;
        return NULL;
    }

    return realpath(pathBuf, NULL);
#elif defined(__FreeBSD__)
    static const int name[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    char path[PATH_MAX];
    size_t len = sizeof(path);
    if (sysctl(name, 4, path, &len, NULL, 0) != 0)
    {
        return NULL;
    }

    return strdup(path);
#elif defined(__sun)
    const char* path = getexecname();
    if (path == NULL)
    {
        return NULL;
    }

    return realpath(path, NULL);
#elif defined(__HAIKU__)
    char path[B_PATH_NAME_LENGTH];
    status_t status = find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_IMAGE_PATH, NULL, path, B_PATH_NAME_LENGTH);
    if (status != B_OK)
    {
        errno = status;
        return NULL;
    }

    return realpath(path, NULL);
#elif defined(_WIN32)
    char path[MAX_PATH];
    if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0)
    {
        return NULL;
    }

    return strdup(path);
#elif defined(TARGET_WASM)
    // This is a packaging convention that our tooling should enforce.
    return strdup("/managed");
#elif defined(__QNXNTO__)
    // QNX has no /proc/self/exe; the process manager reports the path of the
    // executable (without its leading '/').
    struct
    {
        procfs_debuginfo info;
        char path[PATH_MAX];
    } map;
    char path[PATH_MAX + 1];
    int fd = open("/proc/self/as", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return NULL;
    }
    memset(&map, 0, sizeof(map));
    int err = devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof(map), NULL);
    close(fd);
    if (err != EOK || map.info.path[0] == '\0')
    {
        return NULL;
    }
    // A program started by a relative path is reported as it was given
    // ("./bin/app"), relative to the working directory at startup.
    snprintf(path, sizeof(path), "%s%s", map.info.path[0] == '/' || map.info.path[0] == '.' ? "" : "/", map.info.path);
    return realpath(path, NULL);
#else
#ifdef __linux__
    const char* symlinkEntrypointExecutable = "/proc/self/exe";
#else
    const char* symlinkEntrypointExecutable = "/proc/curproc/exe";
#endif

    // Resolve the symlink to the executable from /proc
    char* path = realpath(symlinkEntrypointExecutable, NULL);
    if (path)
    {
        return path;
    }

#if HAVE_GETAUXVAL && defined(AT_EXECFN)
    // fallback to AT_EXECFN, which does not work properly in rare cases
    // when .NET process is set as interpreter (shebang).
    const char* exePath = (const char *)(getauxval(AT_EXECFN));
    if (exePath)
    {
        return realpath(exePath, NULL);
    }
#endif // HAVE_GETAUXVAL && defined(AT_EXECFN)

    return NULL;
#endif // defined(__APPLE__)
}

#ifdef __cplusplus
}
#endif // extern "C"

#endif // HAVE_MINIPAL_GETEXEPATH_H
