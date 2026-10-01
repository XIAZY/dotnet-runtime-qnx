// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

// QNX 6.5 has no inotify, so FileSystemWatcher (and with it PowerShell's
// Get-Content -Wait) failed with "Not supported". The managed Linux watcher
// reaches the OS only through SystemNative_INotifyInit, _INotifyAddWatch and
// _INotifyRemoveWatch, and then poll()s and read()s struct inotify_event
// records from the descriptor. Here the descriptor is the read end of a pipe
// (QNX's pipe manager; nothing here goes through io-pkt), fed by a thread
// that polls the watched directories with readdir and lstat and writes the
// differences as inotify records:
// - IN_CREATE, IN_DELETE; IN_MODIFY when a non-directory's size or mtime
//   changes; IN_ATTRIB when an entry's mode, owner or group changes;
//   IN_ISDIR on entries that are directories;
// - IN_MOVED_FROM followed directly by IN_MOVED_TO, with one cookie, when a
//   name disappears and the same inode appears under another name within
//   the same tick (in this or another watch of the same descriptor); an
//   inode reused across ticks is a delete and a create, never a move;
// - IN_IGNORED when a watch is removed or its directory is gone;
// - IN_Q_OVERFLOW when the reader falls too far behind.
// IN_ACCESS is not reported (no atime polling). Changes within one tick
// merge, and a rewrite that keeps both size and mtime is not seen.
//
// The tick is 250 ms, lengthened so that the scans stay near 300 lstat calls
// a second: with E entries in all watches of a descriptor, the tick is
// max(250 ms, E / 300 s). The thread exists only while the descriptor has
// watches: it starts with the first one and, when the last is removed,
// writes the remaining records, closes the write end (the reader then reads
// end-of-file, which the managed watcher treats as the end) and exits. It
// also exits, without a signal, when the reader has closed the read end:
// the write end is non-blocking with SIGPIPE ignored by the runtime, so a
// write fails with EPIPE, and each tick checks that the read descriptor
// still refers to this pipe. The directories are read with no lock held
// (see s_lock), so one large watch does not hold up the others.
//
// A read must never split a record. Each record is written with one
// write() of at most PIPE_BUF bytes, so it lands whole, and the thread keeps
// no more than MaxPendingBytes unread in the pipe (FIONREAD on the read
// end), which is no more than the smallest buffer the managed watcher reads
// with (InternalBufferSize is at least 4096): a read then always ends at a
// record boundary.

#include "pal_config.h"
#include "pal_io.h"
#include "pal_inotify_qnx.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

enum
{
    BaseTickMs = 250,
    LstatPerSecond = 300,
    MaxPendingBytes = 4096,
    MaxQueuedBytes = 256 * 1024,
};

typedef struct Entry
{
    char* name;
    ino_t ino;
    off_t size;
    time_t mtime;
    mode_t mode;
    uid_t uid;
    gid_t gid;
} Entry;

typedef struct Watch
{
    int32_t wd;
    uint32_t mask;
    char* path;
    dev_t dev;
    ino_t ino;
    Entry* entries; // sorted by name
    size_t count;
    struct Watch* next;
} Watch;

typedef struct Instance
{
    pthread_mutex_t lock; // the fields below; taken after s_lock where both are
    int readFd;
    int writeFd;
    dev_t pipeDev;
    ino_t pipeIno;
    Watch* watches;
    int32_t nextWd;
    uint32_t nextCookie;
    bool threadRunning;
    char* queue; // records not yet written to the pipe
    size_t queueLen;
    size_t queueCap;
    bool overflowed;
    struct Instance* next;
} Instance;

// s_lock guards the list of instances and their lifetime; each instance's
// own lock guards its watches and queue. The order is s_lock, then an
// instance's lock. The poller scans directories holding neither, so one
// large watch never holds up other watchers.
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static Instance* s_instances;

static void FreeEntries(Entry* entries, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        free(entries[i].name);
    }
    free(entries);
}

static void FreeWatch(Watch* w)
{
    FreeEntries(w->entries, w->count);
    free(w->path);
    free(w);
}

static int CompareEntries(const void* a, const void* b)
{
    return strcmp(((const Entry*)a)->name, ((const Entry*)b)->name);
}

// Reads the directory into a name-sorted array. Returns false if the
// directory cannot be opened (gone, or no longer a directory).
static bool ScanDirectory(const char* path, Entry** entriesOut, size_t* countOut)
{
    DIR* dir = opendir(path);
    if (dir == NULL)
    {
        return false;
    }

    size_t count = 0, cap = 16;
    Entry* entries = malloc(cap * sizeof(Entry));
    size_t pathLen = strlen(path);
    struct dirent* de;
    while (entries != NULL && (de = readdir(dir)) != NULL)
    {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
        {
            continue;
        }

        size_t nameLen = strlen(de->d_name);
        char* full = malloc(pathLen + 1 + nameLen + 1);
        if (full == NULL)
        {
            continue;
        }
        memcpy(full, path, pathLen);
        full[pathLen] = '/';
        memcpy(full + pathLen + 1, de->d_name, nameLen + 1);

        struct stat st;
        int r = lstat(full, &st);
        free(full);
        if (r != 0)
        {
            continue; // removed between readdir and lstat
        }

        if (count == cap)
        {
            Entry* grown = realloc(entries, 2 * cap * sizeof(Entry));
            if (grown == NULL)
            {
                break;
            }
            entries = grown;
            cap *= 2;
        }

        Entry* e = &entries[count];
        e->name = strdup(de->d_name);
        if (e->name == NULL)
        {
            continue;
        }
        e->ino = st.st_ino;
        e->size = st.st_size;
        e->mtime = st.st_mtime;
        e->mode = st.st_mode;
        e->uid = st.st_uid;
        e->gid = st.st_gid;
        count++;
    }
    closedir(dir);

    if (entries == NULL)
    {
        return false;
    }
    qsort(entries, count, sizeof(Entry), CompareEntries);
    *entriesOut = entries;
    *countOut = count;
    return true;
}

// Appends one inotify record to the instance's queue (the lock is held).
static void QueueEvent(Instance* inst, int32_t wd, uint32_t mask, uint32_t cookie, const char* name)
{
    size_t nameLen = name != NULL ? strlen(name) : 0;
    // As inotify: the name NUL-terminated and padded, here to 4 bytes.
    uint32_t len = nameLen > 0 ? (uint32_t)((nameLen + 1 + 3) & ~(size_t)3) : 0;
    size_t size = 16 + len;

    if (inst->overflowed)
    {
        return;
    }
    if (inst->queueLen + size > MaxQueuedBytes || size > PIPE_BUF)
    {
        // As inotify's queue overflow: one IN_Q_OVERFLOW (wd -1) after what
        // is already queued, and nothing more until it has been written.
        inst->overflowed = true;
        mask = PAL_IN_Q_OVERFLOW;
        wd = -1;
        cookie = 0;
        len = 0;
        size = 16;
        nameLen = 0;
    }

    if (inst->queueLen + size > inst->queueCap)
    {
        size_t cap = inst->queueCap == 0 ? 1024 : inst->queueCap;
        while (cap < inst->queueLen + size)
        {
            cap *= 2;
        }
        char* grown = realloc(inst->queue, cap);
        if (grown == NULL)
        {
            return;
        }
        inst->queue = grown;
        inst->queueCap = cap;
    }

    char* p = inst->queue + inst->queueLen;
    memcpy(p, &wd, 4);
    memcpy(p + 4, &mask, 4);
    memcpy(p + 8, &cookie, 4);
    memcpy(p + 12, &len, 4);
    if (len > 0)
    {
        memset(p + 16, 0, len);
        memcpy(p + 16, name, nameLen);
    }
    inst->queueLen += size;
}

// Bytes written to the pipe and not yet read; -1 if unknown.
static long PendingBytes(Instance* inst)
{
    int n;
    if (ioctl(inst->readFd, FIONREAD, &n) != 0)
    {
        return -1;
    }
    return (long)n;
}

// Writes queued records, one write() each, while the pipe holds no more than
// MaxPendingBytes. Returns false if the reader has closed the pipe.
static bool FlushQueue(Instance* inst)
{
    size_t pos = 0;
    while (pos < inst->queueLen)
    {
        uint32_t len;
        memcpy(&len, inst->queue + pos + 12, 4);
        size_t size = 16 + len;

        long pending = PendingBytes(inst);
        if (pending < 0 || pending + (long)size > MaxPendingBytes)
        {
            break; // the rest on a later tick
        }

        ssize_t n;
        while ((n = write(inst->writeFd, inst->queue + pos, size)) < 0 && errno == EINTR);
        if (n < 0)
        {
            if (errno == EPIPE)
            {
                return false;
            }
            break; // EAGAIN: full; later
        }
        assert((size_t)n == size); // at most PIPE_BUF: written whole
        pos += size;
    }

    memmove(inst->queue, inst->queue + pos, inst->queueLen - pos);
    inst->queueLen -= pos;
    if (inst->queueLen == 0)
    {
        inst->overflowed = false;
    }
    return true;
}

// Whether the read descriptor still refers to this instance's pipe.
static bool ReaderAlive(Instance* inst)
{
    struct stat st;
    return fstat(inst->readFd, &st) == 0 && st.st_dev == inst->pipeDev && st.st_ino == inst->pipeIno;
}

static void Unlink(Instance* inst)
{
    for (Instance** pp = &s_instances; *pp != NULL; pp = &(*pp)->next)
    {
        if (*pp == inst)
        {
            *pp = inst->next;
            break;
        }
    }
}

static void Destroy(Instance* inst)
{
    while (inst->watches != NULL)
    {
        Watch* w = inst->watches;
        inst->watches = w->next;
        FreeWatch(w);
    }
    if (inst->writeFd >= 0)
    {
        close(inst->writeFd);
    }
    free(inst->queue);
    pthread_mutex_destroy(&inst->lock);
    free(inst);
}

typedef struct Change
{
    Watch* watch;
    const Entry* entry;
} Change;

// A watch's directory as read outside the locks.
typedef struct Snapshot
{
    int32_t wd;
    char* path;
    dev_t dev;
    ino_t ino;
    bool ok;
    Entry* entries;
    size_t count;
} Snapshot;

// Reads each snapshot's directory (no lock held).
static void ScanSnapshots(Snapshot* snaps, size_t n)
{
    for (size_t k = 0; k < n; k++)
    {
        struct stat st;
        if (snaps[k].path == NULL)
        {
            continue; // skipped this tick (wd -1: matches no watch)
        }
        snaps[k].ok = stat(snaps[k].path, &st) == 0 && st.st_ino == snaps[k].ino && st.st_dev == snaps[k].dev &&
                      ScanDirectory(snaps[k].path, &snaps[k].entries, &snaps[k].count);
    }
}

// Queues the differences between the watches and their new snapshots, and
// makes the snapshots current (the instance's lock is held). A watch added
// after the snapshot was taken has none and is left for the next tick.
static void Apply(Instance* inst, Snapshot* snaps, size_t n)
{
    size_t removedCount = 0, addedCount = 0;
    Change* removed = NULL;
    Change* added = NULL;
    Watch* oldLists = NULL; // the previous entry arrays, freed after the moves are paired

    Watch** pw = &inst->watches;
    while (*pw != NULL)
    {
        Watch* w = *pw;
        Snapshot* snap = NULL;
        for (size_t k = 0; k < n && snap == NULL; k++)
        {
            if (snaps[k].wd == w->wd)
            {
                snap = &snaps[k];
            }
        }
        if (snap == NULL)
        {
            pw = &w->next;
            continue;
        }
        Entry* entries = snap->entries;
        size_t count = snap->count;
        snap->entries = NULL; // the watch's now
        snap->count = 0;
        if (!snap->ok)
        {
            // The directory is gone (or replaced): as inotify, IN_IGNORED.
            QueueEvent(inst, w->wd, PAL_IN_IGNORED, 0, NULL);
            *pw = w->next;
            FreeWatch(w);
            continue;
        }

        // Diff the sorted arrays.
        size_t i = 0, j = 0;
        while (i < w->count || j < count)
        {
            int c = i == w->count ? 1 : j == count ? -1 : strcmp(w->entries[i].name, entries[j].name);
            if (c < 0)
            {
                Change* grown = realloc(removed, (removedCount + 1) * sizeof(Change));
                if (grown != NULL)
                {
                    removed = grown;
                    removed[removedCount].watch = w;
                    removed[removedCount].entry = &w->entries[i];
                    removedCount++;
                }
                i++;
            }
            else if (c > 0)
            {
                Change* grown = realloc(added, (addedCount + 1) * sizeof(Change));
                if (grown != NULL)
                {
                    added = grown;
                    added[addedCount].watch = w;
                    added[addedCount].entry = &entries[j];
                    addedCount++;
                }
                j++;
            }
            else
            {
                const Entry* o = &w->entries[i];
                const Entry* e = &entries[j];
                uint32_t isDir = S_ISDIR(e->mode) ? PAL_IN_ISDIR : 0;
                if (o->ino != e->ino)
                {
                    // Replaced under the same name: a delete and a create.
                    if (w->mask & PAL_IN_DELETE)
                        QueueEvent(inst, w->wd, PAL_IN_DELETE | (S_ISDIR(o->mode) ? PAL_IN_ISDIR : 0), 0, o->name);
                    if (w->mask & PAL_IN_CREATE)
                        QueueEvent(inst, w->wd, PAL_IN_CREATE | isDir, 0, e->name);
                }
                else
                {
                    if (!S_ISDIR(e->mode) && (o->size != e->size || o->mtime != e->mtime) && (w->mask & PAL_IN_MODIFY))
                        QueueEvent(inst, w->wd, PAL_IN_MODIFY, 0, e->name);
                    if ((o->mode != e->mode || o->uid != e->uid || o->gid != e->gid) && (w->mask & PAL_IN_ATTRIB))
                        QueueEvent(inst, w->wd, PAL_IN_ATTRIB | isDir, 0, e->name);
                }
                i++;
                j++;
            }
        }

        // Keep the old array until the moves are paired; the new one is current.
        Watch* old = calloc(1, sizeof(Watch));
        if (old != NULL)
        {
            old->entries = w->entries;
            old->count = w->count;
            old->next = oldLists;
            oldLists = old;
        }
        else
        {
            // No memory to keep it: report its removals as deletes now.
            for (size_t k = 0; k < removedCount; k++)
            {
                if (removed[k].watch == w)
                {
                    if (w->mask & PAL_IN_DELETE)
                        QueueEvent(inst, w->wd, PAL_IN_DELETE | (S_ISDIR(removed[k].entry->mode) ? PAL_IN_ISDIR : 0), 0, removed[k].entry->name);
                    removed[k].watch = NULL;
                }
            }
            FreeEntries(w->entries, w->count);
        }
        w->entries = entries;
        w->count = count;
        pw = &w->next;
    }

    // Moves: a removed and an added entry with the same inode, in this tick.
    for (size_t r = 0; r < removedCount; r++)
    {
        if (removed[r].watch == NULL)
        {
            continue;
        }
        const Entry* from = removed[r].entry;
        uint32_t fromDir = S_ISDIR(from->mode) ? PAL_IN_ISDIR : 0;
        size_t a = 0;
        while (a < addedCount && (added[a].watch == NULL || added[a].entry->ino != from->ino))
        {
            a++;
        }
        if (a < addedCount)
        {
            Watch* fw = removed[r].watch;
            Watch* tw = added[a].watch;
            uint32_t cookie = ++inst->nextCookie;
            bool fromWanted = (fw->mask & PAL_IN_MOVED_FROM) != 0;
            bool toWanted = (tw->mask & PAL_IN_MOVED_TO) != 0;
            if (fromWanted)
                QueueEvent(inst, fw->wd, PAL_IN_MOVED_FROM | fromDir, cookie, from->name);
            if (toWanted)
                QueueEvent(inst, tw->wd, PAL_IN_MOVED_TO | fromDir, cookie, added[a].entry->name);
            if (!fromWanted && (fw->mask & PAL_IN_DELETE))
                QueueEvent(inst, fw->wd, PAL_IN_DELETE | fromDir, 0, from->name);
            if (!toWanted && (tw->mask & PAL_IN_CREATE))
                QueueEvent(inst, tw->wd, PAL_IN_CREATE | fromDir, 0, added[a].entry->name);
            added[a].watch = NULL;
        }
        else if (removed[r].watch->mask & PAL_IN_DELETE)
        {
            QueueEvent(inst, removed[r].watch->wd, PAL_IN_DELETE | fromDir, 0, from->name);
        }
    }
    for (size_t a = 0; a < addedCount; a++)
    {
        if (added[a].watch != NULL && (added[a].watch->mask & PAL_IN_CREATE))
        {
            QueueEvent(inst, added[a].watch->wd, PAL_IN_CREATE | (S_ISDIR(added[a].entry->mode) ? PAL_IN_ISDIR : 0), 0,
                       added[a].entry->name);
        }
    }

    while (oldLists != NULL)
    {
        Watch* old = oldLists;
        oldLists = old->next;
        FreeEntries(old->entries, old->count);
        free(old);
    }
    free(removed);
    free(added);
}

static unsigned TickMs(Instance* inst)
{
    size_t entries = 0;
    for (Watch* w = inst->watches; w != NULL; w = w->next)
    {
        entries += w->count + 1;
    }
    size_t ms = entries * 1000 / LstatPerSecond;
    return ms < BaseTickMs ? BaseTickMs : (unsigned)ms;
}

static void* PollThread(void* arg)
{
    Instance* inst = (Instance*)arg;
    unsigned tick = BaseTickMs;

    for (;;)
    {
        struct timespec ts = { (time_t)(tick / 1000), (long)(tick % 1000) * 1000000L };
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR);

        pthread_mutex_lock(&s_lock);
        pthread_mutex_lock(&inst->lock);
        bool alive = ReaderAlive(inst) && FlushQueue(inst);
        if (!alive || (inst->watches == NULL && inst->queueLen == 0))
        {
            // The reader is gone, or the last watch was removed and its
            // records are written: end-of-file for the reader, and done.
            Unlink(inst);
            inst->threadRunning = false;
            pthread_mutex_unlock(&inst->lock);
            pthread_mutex_unlock(&s_lock);
            Destroy(inst);
            return NULL;
        }

        // Snapshot the watch list, then read the directories unlocked.
        size_t n = 0;
        for (Watch* w = inst->watches; w != NULL; w = w->next)
        {
            n++;
        }
        Snapshot* snaps = calloc(n == 0 ? 1 : n, sizeof(Snapshot));
        size_t k = 0;
        for (Watch* w = inst->watches; w != NULL && snaps != NULL; w = w->next, k++)
        {
            snaps[k].wd = w->wd;
            snaps[k].path = strdup(w->path);
            snaps[k].dev = w->dev;
            snaps[k].ino = w->ino;
        }
        pthread_mutex_unlock(&inst->lock);
        pthread_mutex_unlock(&s_lock);

        if (snaps != NULL)
        {
            for (k = 0; k < n; k++)
            {
                if (snaps[k].path == NULL)
                {
                    snaps[k].wd = -1; // no memory for the path: skip this tick
                }
            }
            ScanSnapshots(snaps, n);
        }

        pthread_mutex_lock(&inst->lock);
        if (snaps != NULL)
        {
            Apply(inst, snaps, n);
        }
        FlushQueue(inst);
        tick = TickMs(inst);
        pthread_mutex_unlock(&inst->lock);

        for (k = 0; snaps != NULL && k < n; k++)
        {
            free(snaps[k].path);
            FreeEntries(snaps[k].entries, snaps[k].count);
        }
        free(snaps);
    }
}

static Instance* Find(intptr_t fd)
{
    for (Instance* inst = s_instances; inst != NULL; inst = inst->next)
    {
        if (inst->readFd == (int)fd && inst->writeFd >= 0 && ReaderAlive(inst))
        {
            return inst;
        }
    }
    return NULL;
}

intptr_t QnxINotifyInit(void)
{
    int fds[2];
    if (pipe(fds) != 0)
    {
        return -1;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);

    struct stat st;
    Instance* inst = calloc(1, sizeof(Instance));
    if (inst == NULL || fstat(fds[0], &st) != 0)
    {
        free(inst);
        close(fds[0]);
        close(fds[1]);
        errno = ENOMEM;
        return -1;
    }
    pthread_mutex_init(&inst->lock, NULL);
    inst->readFd = fds[0];
    inst->writeFd = fds[1];
    inst->pipeDev = st.st_dev;
    inst->pipeIno = st.st_ino;
    inst->nextWd = 1;

    pthread_mutex_lock(&s_lock);
    // Drop instances whose reader closed before any watch was added (no
    // thread to notice): their read descriptor may be this one's number.
    Instance** pp = &s_instances;
    while (*pp != NULL)
    {
        Instance* old = *pp;
        if (!old->threadRunning && !ReaderAlive(old))
        {
            *pp = old->next;
            Destroy(old);
            continue;
        }
        pp = &old->next;
    }
    inst->next = s_instances;
    s_instances = inst;
    pthread_mutex_unlock(&s_lock);
    return fds[0];
}

int32_t QnxINotifyAddWatch(intptr_t fd, const char* pathName, uint32_t mask)
{
    struct stat st;
    int r = (mask & PAL_IN_DONT_FOLLOW) ? lstat(pathName, &st) : stat(pathName, &st);
    if (r != 0)
    {
        return -1;
    }
    if (!S_ISDIR(st.st_mode))
    {
        // The managed watcher watches directories only (IN_ONLYDIR).
        errno = ENOTDIR;
        return -1;
    }

    // Read the directory before taking any lock.
    Watch* w = calloc(1, sizeof(Watch));
    if (w == NULL || (w->path = strdup(pathName)) == NULL || !ScanDirectory(pathName, &w->entries, &w->count))
    {
        int e = w != NULL && w->path != NULL ? errno : ENOMEM;
        if (w != NULL)
        {
            free(w->path);
            free(w);
        }
        errno = e;
        return -1;
    }

    pthread_mutex_lock(&s_lock);
    Instance* inst = Find(fd);
    if (inst == NULL)
    {
        pthread_mutex_unlock(&s_lock);
        FreeWatch(w);
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&inst->lock);

    for (Watch* x = inst->watches; x != NULL; x = x->next)
    {
        if (x->dev == st.st_dev && x->ino == st.st_ino)
        {
            // As inotify: the same directory keeps its descriptor; new mask.
            x->mask = mask;
            int32_t wd = x->wd;
            pthread_mutex_unlock(&inst->lock);
            pthread_mutex_unlock(&s_lock);
            FreeWatch(w);
            return wd;
        }
    }

    w->wd = inst->nextWd++;
    w->mask = mask;
    w->dev = st.st_dev;
    w->ino = st.st_ino;
    w->next = inst->watches;
    inst->watches = w;

    if (!inst->threadRunning)
    {
        pthread_attr_t attr;
        pthread_t thread;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        int err = pthread_create(&thread, &attr, PollThread, inst);
        pthread_attr_destroy(&attr);
        if (err != 0)
        {
            inst->watches = w->next;
            pthread_mutex_unlock(&inst->lock);
            pthread_mutex_unlock(&s_lock);
            FreeWatch(w);
            errno = err;
            return -1;
        }
        inst->threadRunning = true;
    }
    int32_t wd = w->wd;
    pthread_mutex_unlock(&inst->lock);
    pthread_mutex_unlock(&s_lock);
    return wd;
}

int32_t QnxINotifyRemoveWatch(intptr_t fd, int32_t wd)
{
    pthread_mutex_lock(&s_lock);
    Instance* inst = Find(fd);
    if (inst != NULL)
    {
        pthread_mutex_lock(&inst->lock);
        for (Watch** pw = &inst->watches; *pw != NULL; pw = &(*pw)->next)
        {
            Watch* w = *pw;
            if (w->wd == wd)
            {
                *pw = w->next;
                FreeWatch(w);
                // As inotify: IN_IGNORED for the removed watch. The thread
                // writes it and, after the last watch, closes the write end.
                QueueEvent(inst, wd, PAL_IN_IGNORED, 0, NULL);
                pthread_mutex_unlock(&inst->lock);
                pthread_mutex_unlock(&s_lock);
                return 0;
            }
        }
        pthread_mutex_unlock(&inst->lock);
    }
    pthread_mutex_unlock(&s_lock);
    errno = EINVAL;
    return -1;
}
