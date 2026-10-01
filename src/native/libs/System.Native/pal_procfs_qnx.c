// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

// Linux-format process files for QNX Neutrino.
//
// The linux-x86 managed libraries read /proc/<pid>/stat, /proc/<pid>/status,
// /proc/<pid>/cmdline, /proc/<pid>/maps and the /proc/<pid>/exe link (and
// their /proc/self forms) for
// System.Diagnostics.Process, Environment.WorkingSet and PowerShell. QNX's
// /proc lists processes as numeric directories too, but each holds only an
// address-space file ("as"); the process information comes from devctl() on
// it. SystemNative_Open and SystemNative_Stat/LStat call these functions
// first: an emulated path opens a descriptor to text generated in the Linux
// format from:
//
//   DCMD_PROC_INFO           pid, parent, session, uid/gid, threads, start
//                            time, user and system time
//   DCMD_PROC_MAPDEBUG_BASE  the executable's path (the name, "comm")
//   DCMD_PROC_MAPINFO        mapping sizes (VmSize, VmData, VmStk)
//   DCMD_PROC_PAGEDATA       pages present (VmRSS, the stat rss field)
//   the "as" file itself     argv, read from the process's initial stack
//
// /proc/net/route, the IPv4 routing table System.Net.NetworkInformation reads
// for gateway addresses, comes from the routing table through
// sysctl(NET_RT_DUMP), which reads it without a routing socket.
//
// The descriptor is an unlinked shared memory object: it seeks and supports
// pread, as .NET needs for a file that is not a regular one. Times use the
// boot time of SystemNative_GetBootTimeTicks (realtime minus monotonic) and
// sysconf(_SC_CLK_TCK) ticks, as Linux does.

#include "pal_config.h"

#if defined(__QNXNTO__)

#include <devctl.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/procfs.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pal_procfs_qnx.h"

typedef enum
{
    QnxProcfsFileNone,
    QnxProcfsFileStat,
    QnxProcfsFileStatus,
    QnxProcfsFileCmdline,
    QnxProcfsFileMaps,
    QnxProcfsFileExe,
} QnxProcfsKind;

enum
{
    QnxPageSize = 4096,
    QnxCommLength = 15, // Linux truncates comm to TASK_COMM_LEN - 1
    QnxCmdlineMax = 65536,
};

// Parses "/proc/self/<file>" or "/proc/<pid>/<file>" for the emulated files.
static QnxProcfsKind QnxProcfsParse(const char* path, pid_t* pid)
{
    static const char prefix[] = "/proc/";
    const char* p;
    const char* file;

    if (path == NULL || strncmp(path, prefix, sizeof(prefix) - 1) != 0)
    {
        return QnxProcfsFileNone;
    }
    p = path + sizeof(prefix) - 1;
    if (strncmp(p, "self/", 5) == 0)
    {
        *pid = getpid();
        file = p + 5;
    }
    else
    {
        long value = 0;
        const char* start = p;
        while (*p >= '0' && *p <= '9' && p - start < 10)
        {
            value = value * 10 + (*p++ - '0');
        }
        if (p == start || *p != '/' || value <= 0)
        {
            return QnxProcfsFileNone;
        }
        *pid = (pid_t)value;
        file = p + 1;
    }

    if (strcmp(file, "stat") == 0)
        return QnxProcfsFileStat;
    if (strcmp(file, "status") == 0)
        return QnxProcfsFileStatus;
    if (strcmp(file, "cmdline") == 0)
        return QnxProcfsFileCmdline;
    if (strcmp(file, "maps") == 0)
        return QnxProcfsFileMaps;
    if (strcmp(file, "exe") == 0)
        return QnxProcfsFileExe;
    return QnxProcfsFileNone;
}

// A growable text buffer.
typedef struct
{
    char* data;
    size_t length;
    size_t capacity;
    bool failed;
} QnxText;

static void QnxAppend(QnxText* text, const char* bytes, size_t length)
{
    if (text->failed)
        return;
    if (text->length + length > text->capacity)
    {
        size_t capacity = text->capacity == 0 ? 1024 : text->capacity;
        while (capacity < text->length + length)
            capacity *= 2;
        char* grown = (char*)realloc(text->data, capacity);
        if (grown == NULL)
        {
            text->failed = true;
            return;
        }
        text->data = grown;
        text->capacity = capacity;
    }
    memcpy(text->data + text->length, bytes, length);
    text->length += length;
}

static void QnxPrintf(QnxText* text, const char* format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (n > 0)
        QnxAppend(text, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

// Everything the three files are made of, for one process.
typedef struct
{
    procfs_info info;
    char comm[QnxCommLength + 1];
    uint64_t vmSize, vmData, vmStack, vmResident;
} QnxProcess;

// The executable's name: the last component of its path, at most 15 characters.
static void QnxReadComm(int as, QnxProcess* process)
{
    struct
    {
        procfs_debuginfo info;
        char path[1024];
    } map;
    memset(&map, 0, sizeof(map));
    process->comm[0] = '\0';
    if (devctl(as, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof(map), NULL) == EOK)
    {
        const char* name = strrchr(map.info.path, '/');
        name = name != NULL ? name + 1 : map.info.path;
        strncpy(process->comm, name, QnxCommLength);
        process->comm[QnxCommLength] = '\0';
    }
    if (process->comm[0] == '\0')
    {
        snprintf(process->comm, sizeof(process->comm), "%d", (int)process->info.pid);
    }
}

// Reads a DCMD_PROC_MAPINFO or DCMD_PROC_PAGEDATA list; free() the result.
static procfs_mapinfo* QnxReadMaps(int as, int dcmd, int* count)
{
    int n = 0;
    *count = 0;
    if (devctl(as, dcmd, NULL, 0, &n) != EOK || n <= 0)
        return NULL;
    n += 16; // it may grow meanwhile
    procfs_mapinfo* maps = (procfs_mapinfo*)calloc((size_t)n, sizeof(procfs_mapinfo));
    if (maps == NULL)
        return NULL;
    int got = 0;
    if (devctl(as, dcmd, maps, (size_t)n * sizeof(procfs_mapinfo), &got) != EOK)
    {
        free(maps);
        return NULL;
    }
    *count = got < n ? got : n;
    return maps;
}

static void QnxReadMemory(int as, QnxProcess* process)
{
    int count;
    procfs_mapinfo* maps = QnxReadMaps(as, DCMD_PROC_MAPINFO, &count);
    for (int i = 0; i < count; i++)
    {
        process->vmSize += maps[i].size;
        if ((maps[i].flags & MAP_STACK) != 0)
            process->vmStack += maps[i].size;
        else if ((maps[i].flags & MAP_ANON) != 0 && (maps[i].flags & MAP_TYPE) == MAP_PRIVATE)
            process->vmData += maps[i].size;
    }
    free(maps);

    maps = QnxReadMaps(as, DCMD_PROC_PAGEDATA, &count);
    for (int i = 0; i < count; i++)
    {
        if ((maps[i].flags & PG_HWMAPPED) != 0)
            process->vmResident += maps[i].size;
    }
    free(maps);
}

// Opens the process's address space file and reads what the files need.
static int QnxReadProcess(pid_t pid, QnxProcess* process, int* asOut)
{
    char path[64];
    memset(process, 0, sizeof(*process));
    snprintf(path, sizeof(path), "/proc/%d/as", (int)pid);
    int as = open(path, O_RDONLY | O_CLOEXEC);
    if (as < 0)
        return -1; // errno: ENOENT for a process that does not exist
    int err = devctl(as, DCMD_PROC_INFO, &process->info, sizeof(process->info), NULL);
    if (err != EOK)
    {
        close(as);
        errno = err == ESRCH ? ENOENT : err;
        return -1;
    }
    QnxReadComm(as, process);
    QnxReadMemory(as, process);
    *asOut = as;
    return 0;
}

static uint64_t QnxBootTimeNanoseconds(void)
{
    struct timespec realtime, monotonic;
    clock_gettime(CLOCK_REALTIME, &realtime);
    clock_gettime(CLOCK_MONOTONIC, &monotonic);
    return ((uint64_t)realtime.tv_sec * 1000000000u + (uint64_t)realtime.tv_nsec) -
           ((uint64_t)monotonic.tv_sec * 1000000000u + (uint64_t)monotonic.tv_nsec);
}

static uint64_t QnxToTicks(uint64_t nanoseconds)
{
    long ticks = sysconf(_SC_CLK_TCK);
    return nanoseconds / (1000000000u / (uint64_t)(ticks > 0 ? ticks : 100));
}

static char QnxState(const QnxProcess* process)
{
    return (process->info.flags & _NTO_PF_ZOMBIE) != 0 ? 'Z' : 'S';
}

static void QnxRenderStat(const QnxProcess* process, QnxText* text)
{
    const procfs_info* info = &process->info;
    uint64_t boot = QnxBootTimeNanoseconds();
    uint64_t start = info->start_time > boot ? info->start_time - boot : 0;

    // All 52 fields of Linux's /proc/<pid>/stat, space-separated, ending in a newline.
    QnxPrintf(text, "%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 ", (int)info->pid, process->comm, QnxState(process),
              (int)info->parent, (int)info->pgrp, (int)info->sid);
    QnxPrintf(text, "%llu %llu %llu %llu 20 0 %u 0 %llu %llu %llu 4294967295 ",
              (unsigned long long)QnxToTicks(info->utime), (unsigned long long)QnxToTicks(info->stime),
              (unsigned long long)QnxToTicks(info->cutime), (unsigned long long)QnxToTicks(info->cstime),
              (unsigned)info->num_threads, (unsigned long long)QnxToTicks(start),
              (unsigned long long)process->vmSize, (unsigned long long)(process->vmResident / QnxPageSize));
    QnxPrintf(text, "0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
}

static void QnxRenderStatus(const QnxProcess* process, QnxText* text)
{
    const procfs_info* info = &process->info;
    QnxPrintf(text, "Name:\t%s\n", process->comm);
    QnxPrintf(text, "State:\t%c (%s)\n", QnxState(process), QnxState(process) == 'Z' ? "zombie" : "sleeping");
    QnxPrintf(text, "Tgid:\t%d\nPid:\t%d\nPPid:\t%d\n", (int)info->pid, (int)info->pid, (int)info->parent);
    QnxPrintf(text, "Uid:\t%d\t%d\t%d\t%d\n", (int)info->uid, (int)info->euid, (int)info->suid, (int)info->euid);
    QnxPrintf(text, "Gid:\t%d\t%d\t%d\t%d\n", (int)info->gid, (int)info->egid, (int)info->sgid, (int)info->egid);
    // QNX keeps no peak values, and does not swap.
    QnxPrintf(text, "VmPeak:\t%8llu kB\nVmSize:\t%8llu kB\n", (unsigned long long)(process->vmSize / 1024),
              (unsigned long long)(process->vmSize / 1024));
    QnxPrintf(text, "VmHWM:\t%8llu kB\nVmRSS:\t%8llu kB\n", (unsigned long long)(process->vmResident / 1024),
              (unsigned long long)(process->vmResident / 1024));
    QnxPrintf(text, "VmData:\t%8llu kB\nVmStk:\t%8llu kB\nVmSwap:\t%8u kB\n",
              (unsigned long long)(process->vmData / 1024), (unsigned long long)(process->vmStack / 1024), 0u);
    QnxPrintf(text, "Threads:\t%u\n", (unsigned)info->num_threads);
}

// argv, NUL-separated with a trailing NUL, read from the process's initial
// stack (argc, then the argv pointers) through its address space file.
static void QnxRenderCmdline(int as, const QnxProcess* process, QnxText* text)
{
    uint32_t argc = 0;
    off64_t stack = (off64_t)process->info.initial_stack;

    if (stack != 0 && pread64(as, &argc, sizeof(argc), stack) == (ssize_t)sizeof(argc) && argc > 0 && argc < 4096)
    {
        uint32_t* argv = (uint32_t*)calloc(argc, sizeof(uint32_t));
        if (argv != NULL && pread64(as, argv, argc * sizeof(uint32_t), stack + (off64_t)sizeof(uint32_t)) ==
                                (ssize_t)(argc * sizeof(uint32_t)))
        {
            char chunk[256];
            for (uint32_t i = 0; i < argc && text->length < QnxCmdlineMax; i++)
            {
                off64_t at = argv[i];
                for (;;)
                {
                    ssize_t n = pread64(as, chunk, sizeof(chunk), at);
                    if (n <= 0)
                        break;
                    char* end = memchr(chunk, '\0', (size_t)n);
                    QnxAppend(text, chunk, end != NULL ? (size_t)(end - chunk) : (size_t)n);
                    if (end != NULL || text->length >= QnxCmdlineMax)
                        break;
                    at += n;
                }
                QnxAppend(text, "", 1);
            }
        }
        free(argv);
    }

    if (text->length == 0)
    {
        // As Linux shows for a process whose arguments cannot be read.
        QnxAppend(text, process->comm, strlen(process->comm));
        QnxAppend(text, "", 1);
    }
}

// The path of the object mapped at vaddr (the executable with
// DCMD_PROC_MAPDEBUG_BASE), as an absolute path. procnto reports these
// paths without their leading '/' ("proc/boot/libc.so.3"), and anonymous
// memory as "/dev/zero". Returns false if there is no file behind it.
static bool QnxMappedPath(int as, unsigned dcmd, uint64_t vaddr, bool self, char* out, size_t size)
{
    struct
    {
        procfs_debuginfo info;
        char path[1024];
    } map;
    memset(&map, 0, sizeof(map));
    map.info.vaddr = vaddr;
    if (devctl(as, (int)dcmd, &map, sizeof(map), NULL) != EOK || map.info.path[0] == '\0' ||
        strcmp(map.info.path, "/dev/zero") == 0)
    {
        return false;
    }
    if (map.info.path[0] == '.')
    {
        // A program started by a relative path is reported as it was given
        // ("./bin/qnxhost"), which only resolves in the process's own
        // directory: resolve it for this process, leave it for others.
        char* resolved = self ? realpath(map.info.path, NULL) : NULL;
        snprintf(out, size, "%s", resolved != NULL ? resolved : map.info.path);
        free(resolved);
        return true;
    }
    snprintf(out, size, "%s%s", map.info.path[0] == '/' ? "" : "/", map.info.path);
    return true;
}

// Linux's maps lines: address range, permissions, offset, device, inode and
// path. Only file mappings get a path; .NET builds Process.Modules from the
// readable and executable ones.
static void QnxRenderMaps(int as, bool self, QnxText* text)
{
    int count;
    procfs_mapinfo* maps = QnxReadMaps(as, DCMD_PROC_MAPINFO, &count);
    for (int i = 0; i < count; i++)
    {
        char path[1100];
        uint32_t flags = maps[i].flags;
        if (!QnxMappedPath(as, DCMD_PROC_MAPDEBUG, maps[i].vaddr, self, path, sizeof(path)))
        {
            path[0] = '\0';
        }
        QnxPrintf(text, "%08llx-%08llx %c%c%c%c %08llx 00:00 %llu%s%s\n", (unsigned long long)maps[i].vaddr,
                  (unsigned long long)(maps[i].vaddr + maps[i].size), (flags & PROT_READ) != 0 ? 'r' : '-',
                  (flags & PROT_WRITE) != 0 ? 'w' : '-', (flags & PROT_EXEC) != 0 ? 'x' : '-',
                  (flags & MAP_TYPE) == MAP_SHARED ? 's' : 'p', (unsigned long long)maps[i].offset,
                  (unsigned long long)(path[0] != '\0' ? maps[i].ino : 0), path[0] != '\0' ? "    " : "", path);
    }
    free(maps);
}

// Linux's /proc/net/route: the main table's IPv4 routes, one line each, the
// addresses as the hexadecimal of the in_addr's 32 bits as stored, every line
// padded to 127 characters. From BSD's routing table: ARP and cloned entries
// are left out, and so are routes through loopback interfaces, which Linux
// keeps in its local table. RefCnt, Metric, MTU, Window and IRTT have no
// counterpart and are 0.
static void QnxRenderNetRoute(QnxText* text)
{
    char line[256];
    snprintf(line, sizeof(line), "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT");
    QnxPrintf(text, "%-127s\n", line);

    int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_DUMP, 0 };
    size_t needed = 0;
    if (sysctl(mib, 6, NULL, &needed, NULL, 0) != 0 || needed == 0)
        return;
    char* buffer = (char*)malloc(needed);
    if (buffer == NULL || sysctl(mib, 6, buffer, &needed, NULL, 0) != 0)
    {
        free(buffer);
        return;
    }

    struct ifaddrs* interfaces = NULL;
    getifaddrs(&interfaces);

    for (char* next = buffer; next + sizeof(struct rt_msghdr) <= buffer + needed;)
    {
        struct rt_msghdr* message = (struct rt_msghdr*)next;
        if (message->rtm_msglen == 0)
            break;
        next += message->rtm_msglen;
        if (message->rtm_version != RTM_VERSION || (message->rtm_flags & RTF_UP) == 0)
            continue;
#ifdef RTF_LLINFO
        if (message->rtm_flags & RTF_LLINFO)
            continue;
#endif
#ifdef RTF_CLONED
        if (message->rtm_flags & RTF_CLONED)
            continue;
#endif

        // The addresses follow the header, each rounded up to a long.
        struct sockaddr* addresses[RTAX_MAX] = { 0 };
        char* sa = (char*)(message + 1);
        for (int i = 0; i < RTAX_MAX; i++)
        {
            if ((message->rtm_addrs & (1 << i)) == 0)
                continue;
            struct sockaddr* address = (struct sockaddr*)sa;
            if (sa >= next)
                break;
            addresses[i] = address;
            size_t length = address->sa_len > 0 ? (size_t)address->sa_len : sizeof(long);
            sa += (length + sizeof(long) - 1) & ~(sizeof(long) - 1);
        }

        struct sockaddr_in destination, gateway, mask;
        memset(&destination, 0, sizeof(destination));
        memset(&gateway, 0, sizeof(gateway));
        memset(&mask, 0, sizeof(mask));
        if (addresses[RTAX_DST] == NULL || addresses[RTAX_DST]->sa_family != AF_INET)
            continue;
        memcpy(&destination, addresses[RTAX_DST], addresses[RTAX_DST]->sa_len < sizeof(destination) ? addresses[RTAX_DST]->sa_len : sizeof(destination));
        if (addresses[RTAX_GATEWAY] != NULL && addresses[RTAX_GATEWAY]->sa_family == AF_INET)
            memcpy(&gateway, addresses[RTAX_GATEWAY], sizeof(gateway));
        if (addresses[RTAX_NETMASK] != NULL)
        {
            // A netmask may be shortened to its significant bytes.
            size_t length = addresses[RTAX_NETMASK]->sa_len;
            memcpy(&mask, addresses[RTAX_NETMASK], length < sizeof(mask) ? length : sizeof(mask));
        }
        else if (message->rtm_flags & RTF_HOST)
        {
            mask.sin_addr.s_addr = 0xffffffffu;
        }

        char name[IF_NAMESIZE] = "";
        if (if_indextoname(message->rtm_index, name) == NULL)
            continue;
        bool loopback = false;
        for (struct ifaddrs* i = interfaces; i != NULL; i = i->ifa_next)
        {
            if (strcmp(i->ifa_name, name) == 0 && (i->ifa_flags & IFF_LOOPBACK))
                loopback = true;
        }
        if (loopback)
            continue;

        unsigned flags = 0x0001; // RTF_UP
        if (message->rtm_flags & RTF_GATEWAY)
            flags |= 0x0002;
        if (message->rtm_flags & RTF_HOST)
            flags |= 0x0004;
        if (message->rtm_flags & RTF_DYNAMIC)
            flags |= 0x0010;
        if (message->rtm_flags & RTF_MODIFIED)
            flags |= 0x0020;
        if (message->rtm_flags & RTF_REJECT)
            flags |= 0x0200;

        snprintf(line, sizeof(line), "%s\t%08X\t%08X\t%04X\t%d\t%u\t%d\t%08X\t%d\t%u\t%u", name,
                 (unsigned)destination.sin_addr.s_addr, (unsigned)gateway.sin_addr.s_addr, flags, 0,
                 (unsigned)message->rtm_use, 0, (unsigned)mask.sin_addr.s_addr, 0, 0u, 0u);
        QnxPrintf(text, "%-127s\n", line);
    }

    if (interfaces != NULL)
        freeifaddrs(interfaces);
    free(buffer);
}

static const char QnxNetRoutePath[] = "/proc/net/route";

// Returns an unlinked shared memory object holding the text, positioned at 0.
static int QnxDescriptorFor(const QnxText* text)
{
    static unsigned counter;
    char name[64];
    snprintf(name, sizeof(name), "/dotnet-procfs-%d-%u", (int)getpid(), __atomic_fetch_add(&counter, 1u, __ATOMIC_RELAXED));
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0400);
    if (fd < 0)
        return -1;
    shm_unlink(name);
    size_t done = 0;
    while (done < text->length)
    {
        ssize_t n = write(fd, text->data + done, text->length - done);
        if (n <= 0)
        {
            int error = errno;
            close(fd);
            errno = error;
            return -1;
        }
        done += (size_t)n;
    }
    lseek(fd, 0, SEEK_SET);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int32_t QnxProcfsOpen(const char* path, int32_t* fd)
{
    if (path != NULL && strcmp(path, QnxNetRoutePath) == 0)
    {
        QnxText route = { 0 };
        QnxRenderNetRoute(&route);
        if (route.failed)
        {
            errno = ENOMEM;
            *fd = -1;
        }
        else
        {
            *fd = QnxDescriptorFor(&route);
        }
        free(route.data);
        return 1;
    }

    pid_t pid;
    QnxProcfsKind kind = QnxProcfsParse(path, &pid);
    if (kind == QnxProcfsFileNone || kind == QnxProcfsFileExe)
        return 0;

    QnxProcess process;
    int as;
    *fd = -1;
    if (QnxReadProcess(pid, &process, &as) != 0)
        return 1; // errno set

    QnxText text = { 0 };
    switch (kind)
    {
        case QnxProcfsFileStat:
            QnxRenderStat(&process, &text);
            break;
        case QnxProcfsFileStatus:
            QnxRenderStatus(&process, &text);
            break;
        case QnxProcfsFileMaps:
            QnxRenderMaps(as, pid == getpid(), &text);
            break;
        case QnxProcfsFileCmdline:
        case QnxProcfsFileExe:
        case QnxProcfsFileNone:
            QnxRenderCmdline(as, &process, &text);
            break;
    }
    close(as);

    if (text.failed)
    {
        errno = ENOMEM;
    }
    else
    {
        *fd = QnxDescriptorFor(&text);
    }
    free(text.data);
    return 1;
}

int32_t QnxProcfsStat(const char* path, struct stat64* st)
{
    if (path != NULL && strcmp(path, QnxNetRoutePath) == 0)
    {
        // As Linux reports it: regular, read-only, empty, root's.
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFREG | 0444;
        st->st_nlink = 1;
        st->st_blksize = QnxPageSize;
        st->st_atime = st->st_mtime = st->st_ctime = time(NULL);
        return 1;
    }

    pid_t pid;
    QnxProcfsKind kind = QnxProcfsParse(path, &pid);
    if (kind == QnxProcfsFileNone)
        return 0;

    char as[64];
    struct stat64 asStat;
    snprintf(as, sizeof(as), "/proc/%d/as", (int)pid);
    if (stat64(as, &asStat) != 0)
        return -1; // errno: the process does not exist

    // As Linux reports its process files: regular, read-only, empty; exe is a link.
    memset(st, 0, sizeof(*st));
    st->st_mode = kind == QnxProcfsFileExe ? (S_IFLNK | 0777) : (S_IFREG | 0444);
    st->st_nlink = 1;
    st->st_uid = asStat.st_uid;
    st->st_gid = asStat.st_gid;
    st->st_dev = asStat.st_dev;
    st->st_ino = asStat.st_ino;
    st->st_atime = st->st_mtime = st->st_ctime = asStat.st_mtime;
    st->st_blksize = QnxPageSize;
    return 1;
}

int32_t QnxProcfsReadLink(const char* path, char* buffer, int32_t bufferSize, int32_t* length)
{
    pid_t pid;
    if (QnxProcfsParse(path, &pid) != QnxProcfsFileExe)
        return 0;

    char as[64], exe[1100];
    snprintf(as, sizeof(as), "/proc/%d/as", (int)pid);
    int fd = open(as, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        *length = -1;
        return 1;
    }
    bool found = QnxMappedPath(fd, DCMD_PROC_MAPDEBUG_BASE, 0, pid == getpid(), exe, sizeof(exe));
    close(fd);
    if (!found)
    {
        errno = ENOENT;
        *length = -1;
        return 1;
    }
    // readlink semantics: no terminating NUL, truncated to the buffer.
    size_t n = strlen(exe);
    if (n > (size_t)bufferSize)
        n = (size_t)bufferSize;
    memcpy(buffer, exe, n);
    *length = (int32_t)n;
    return 1;
}

#endif // __QNXNTO__
