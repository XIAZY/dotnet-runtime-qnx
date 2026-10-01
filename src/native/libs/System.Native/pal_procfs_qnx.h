// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

#pragma once

#if defined(__QNXNTO__)

#include <stdint.h>
#include <sys/stat.h>

// Linux-format /proc/<pid>/{stat,status,cmdline,maps,exe} and /proc/net/route
// for QNX; see pal_procfs_qnx.c.

// Returns 0 if path is not an emulated file. Otherwise returns 1 and sets *fd
// to a descriptor holding the file's text, or to -1 with errno set.
int32_t QnxProcfsOpen(const char* path, int32_t* fd);

// Returns 0 if path is not an emulated file, 1 with *st filled in, or -1
// with errno set (the process does not exist).
int32_t QnxProcfsStat(const char* path, struct stat64* st);

// Returns 0 if path is not /proc/<pid>/exe or /proc/self/exe. Otherwise
// returns 1 and sets *length as readlink() returns (-1 with errno set).
int32_t QnxProcfsReadLink(const char* path, char* buffer, int32_t bufferSize, int32_t* length);

#endif
