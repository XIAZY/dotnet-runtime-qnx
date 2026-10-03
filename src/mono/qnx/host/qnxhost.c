// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

/*
 * qnxhost: runs a .NET program on QNX Neutrino 6.5 with the Mono runtime,
 * in place of the dotnet host (C++), which QNX cannot run.
 *
 *     qnxhost <program.props> [program arguments...]
 *     <program> [program arguments...]
 *
 * The second form is multi-call: a copy of this executable under another
 * name (an install tree has one at <tree>/<program>/<program>, pwsh/pwsh for
 * PowerShell) reads <tree>/<program>.props, found from the executable's own
 * path as the process manager reports it, resolved (never from argv[0], so
 * that a symlink, a PATH lookup or a login shell's "-pwsh" all find it).
 * The program then sees itself as that executable: Environment.ProcessPath,
 * the process's name, and the path PowerShell restarts itself with (jobs,
 * -Login) are <tree>/pwsh/pwsh, as with the dotnet host on Linux. A symlink
 * to qnxhost resolves to qnxhost, so it must be a copy, not a link.
 *
 * program.props is written when the program is deployed. Each line is
 * KEY=VALUE; "$ROOT" in a value stands for the directory of the props file.
 * APP names the program's assembly and RUNTIME the Mono runtime library.
 * SETENV=NAME=value sets an environment variable before the runtime starts,
 * and DEFAULTENV=NAME=value sets it only if it is not set already (for what
 * a program needs on QNX). Every other key is passed to the runtime as a
 * property, as the dotnet host would pass it: TRUSTED_PLATFORM_ASSEMBLIES,
 * NATIVE_DLL_SEARCH_DIRECTORIES, APP_CONTEXT_BASE_DIRECTORY,
 * RUNTIME_IDENTIFIER and the runtimeconfig.json properties.
 *
 * What it does besides:
 *  - Selects interpreter-only execution (MONO_AOT_MODE_INTERP_ONLY) through
 *    the runtime's API: the JIT is never used. QNXHOST_MODE=aot selects
 *    AOT images with the interpreter for what they lack
 *    (MONO_AOT_MODE_INTERP, as --full-aot-interp). QNXHOST_MODE=jit leaves
 *    Mono's default instead: AOT images where they exist, the JIT for the
 *    rest (the shipped configuration, with the normal image set).
 *  - AOT images are mapped by the runtime's own loader, which charges only
 *    the pages used (mono-dl-qnx.c in the runtime). QNXHOST_AOT_LOADER=dlopen
 *    selects QNX's dlopen instead; QNXHOST_VERBOSE=1 makes the loader print
 *    a line for each image it maps or leaves to dlopen.
 *  - QNXHOST_MALLINFO=1 prints malloc's statistics (mallinfo) to standard
 *    error at exit, for diagnosing memory use: the runtime's metadata and
 *    most of its other native data live in malloc's arena.
 *  - Confines asynchronous signals (SIGINT, SIGCHLD, SIGWINCH, ...) to one
 *    thread of its own. QNX 6.5 cannot restart system calls interrupted by
 *    a signal (there is no SA_RESTART), so they are blocked in every other
 *    thread, which inherit the mask from the main thread. Synchronous
 *    signals (SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGTRAP), SIGPIPE, and the
 *    real-time signals the runtime uses to interrupt its own threads stay
 *    unblocked everywhere.
 *  - Runs the runtime on a thread with an 8 MiB stack: the main thread's
 *    is 512 KiB on QNX, and the interpreter needs more. The stack is lazy,
 *    so only the pages used take memory.
 *  - Makes a private directory, qnxhost-<uid> (mode 0700) in TMPDIR, or in
 *    /tmp when TMPDIR is unset or empty, in every configuration:
 *    System.Native keeps its cross-process socket lock file there and
 *    finds it the same way (pal_socklock_qnx.c). A TMPDIR that already
 *    names such a directory (a parent qnxhost's) is that directory. On
 *    BlackBerry 10, /tmp is /dev/shmem, which holds no directories, and
 *    applications get a TMPDIR of their own. It gives the program that
 *    directory as TMPDIR, unless TMPDIR is set already (by the environment
 *    or the props file) or QNXHOST_PRIVATE_TMPDIR=0.
 *    .NET makes the Unix sockets of named pipes in the temporary directory
 *    and deletes them when they close, and on QNX 6.5 unlinking a socket's
 *    name while io-pkt serves another request from any process deadlocks
 *    io-pkt until a reboot. A
 *    directory of their own keeps them away from anything that lists or
 *    stats /tmp. Side effect: the program's other temporary files, and the
 *    TMPDIR its child processes inherit, are in that directory too. A
 *    directory of that name that is not this user's, is a link, or is open
 *    to others is not used (a warning; TMPDIR stays unset, and the socket
 *    lock works within each process only).
 *  - Makes a POSIX rule string in TZ (EST5EDT4,M3.2.0/2,M11.1.0/2, the
 *    usual form on QNX, which has no time-zone database) a zone .NET can
 *    find. .NET reads a TZ that is not an absolute path as a file under
 *    TZDIR and knows nothing of rule strings (on Linux too: it falls back
 *    to UTC); QNX's libc knows only rule strings and parses TZ again on
 *    every mktime, so TZ itself must not change. The rule is written as a
 *    one-transition TZif file (the rule in its footer) at the relative path
 *    the rule spells out, in qnxhost-<uid>/zoneinfo-<hash>, a directory
 *    that otherwise holds symlinks to every entry of the real TZDIR (the
 *    tree's etc/zoneinfo, or the user's); TZDIR then names that directory.
 *    .NET's local zone is then the rule, its id the rule string, while libc
 *    and child programs keep the TZ the user set. With TZ unset, the
 *    system's zone (confstr(_CS_TIMEZONE)) is used and put in TZ, with the
 *    same meaning for libc: a rule on QNX 6.5, an IANA zone name on
 *    BlackBerry 10, whose libc reads both; a name is put in TZ only if .NET
 *    finds it under TZDIR (libc already reads the same name). Nothing else
 *    is done for a zone name, nor for an absolute path or an empty TZ.
 */
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/procfs.h>

#include <mono/jit/details/jit-types.h> /* MonoAotMode */

#define RUNTIME_STACK_SIZE (8 * 1024 * 1024)
#define MAX_PROPERTIES 64


typedef int (*monovm_initialize_fn)(int, const char **, const char **);
typedef int (*monovm_execute_assembly_fn)(int, const char **, const char *, unsigned int *);
typedef int (*monovm_shutdown_fn)(int *);
typedef void (*mono_jit_set_aot_mode_fn)(int);

static const int async_signals[] = {
	SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGCHLD, SIGCONT, SIGTSTP, SIGTTIN, SIGTTOU,
	SIGWINCH, SIGUSR1, SIGUSR2, SIGALRM, SIGVTALRM, SIGPROF, SIGURG,
#ifdef SIGPOLL
	SIGPOLL,
#endif
};

static const char *keys[MAX_PROPERTIES], *values[MAX_PROPERTIES];
static int nproperties;
static const char *app_path, *runtime_path;
static int app_argc;
static const char **app_argv;
static int exit_code = 1;

static void fail(const char *what, const char *detail)
{
	fprintf(stderr, "qnxhost: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	exit(2);
}

/* Replaces each "$ROOT" in value with root; the result is malloc'ed. */
static char *expand(const char *value, const char *root)
{
	size_t rootlen = strlen(root), n = 0;
	const char *p;
	char *out, *q;

	for (p = value; (p = strstr(p, "$ROOT")) != NULL; p += 5)
		n++;
	out = malloc(strlen(value) + n * rootlen + 1);
	if (out == NULL)
		fail("out of memory", NULL);
	for (p = value, q = out; *p;) {
		if (strncmp(p, "$ROOT", 5) == 0) {
			memcpy(q, root, rootlen);
			q += rootlen;
			p += 5;
		} else {
			*q++ = *p++;
		}
	}
	*q = '\0';
	return out;
}

static void read_props(const char *path)
{
	char root[PATH_MAX], line[65536];
	FILE *f;

	if (realpath(path, root) == NULL)
		fail("cannot find", path);
	*strrchr(root, '/') = '\0';
	f = fopen(path, "r");
	if (f == NULL)
		fail("cannot open", path);
	while (fgets(line, sizeof line, f) != NULL) {
		char *eq, *end = line + strlen(line);

		while (end > line && (end[-1] == '\n' || end[-1] == '\r'))
			*--end = '\0';
		if (line[0] == '\0' || line[0] == '#' || (eq = strchr(line, '=')) == NULL)
			continue;
		*eq = '\0';
		if (strcmp(line, "APP") == 0) {
			app_path = expand(eq + 1, root);
		} else if (strcmp(line, "RUNTIME") == 0) {
			runtime_path = expand(eq + 1, root);
		} else if (strcmp(line, "SETENV") == 0 || strcmp(line, "DEFAULTENV") == 0) {
			char *name = expand(eq + 1, root), *value = strchr(name, '=');

			if (value == NULL || value == name)
				fail("expected NAME=value after SETENV or DEFAULTENV in", path);
			*value++ = '\0';
			if (setenv(name, value, strcmp(line, "SETENV") == 0) != 0)
				fail("cannot set", name);
			free(name);
		} else {
			if (nproperties == MAX_PROPERTIES)
				fail("too many properties in", path);
			keys[nproperties] = strdup(line);
			values[nproperties] = expand(eq + 1, root);
			nproperties++;
		}
	}
	fclose(f);
	if (app_path == NULL || runtime_path == NULL)
		fail("APP and RUNTIME must be set in", path);
}

static void async_signal_set(sigset_t *set)
{
	size_t i;

	sigemptyset(set);
	for (i = 0; i < sizeof async_signals / sizeof async_signals[0]; i++)
		sigaddset(set, async_signals[i]);
}

/* The only thread with the asynchronous signals unblocked: their handlers run here. */
static void *signal_thread(void *arg)
{
	sigset_t set;

	(void)arg;
	async_signal_set(&set);
	pthread_sigmask(SIG_UNBLOCK, &set, NULL);
	for (;;)
		pause();
	return NULL;
}

/*
 * P/Invoke override (Mono's PINVOKE_OVERRIDE host property): two entry
 * points of "libc" that PowerShell imports directly, bypassing System.Native,
 * and System.Native's process start get QNX versions; every other pair
 * resolves as usual (NULL).
 *
 * readlink: PowerShell's login code (AttemptExecPwshLogin) reads
 * /proc/self/exe, which QNX does not have; that path is answered from the
 * process manager, others go to libc.
 *
 * execv: the login code then runs /bin/sh -l -c 'exec <pwsh> "$@"' "" <args>.
 * QNX 6.5's ksh drops the positional parameters of -c when -l is given
 * (measured: `sh -l -c 'echo $#' "" a b` prints 0; without -l, 2), so pwsh
 * restarted with no arguments. That one call is rewritten to put the quoted
 * arguments in the command string itself; any other execv goes to libc.
 *
 * SystemNative_ForkAndExecProcess: without root, .NET's Ping (Test-Connection)
 * runs the ping utility with Linux's options, "-c 1 -W <seconds> [-t <ttl>]
 * [-M do|dont] -s <size> <address>". QNX's ping (NetBSD's) rejects -W
 * ("illegal option"), takes the TTL as -T (its -t is the TOS), the timeout as
 * -w, and has no -M. So a start of a program named ping whose arguments
 * include -W, a command that cannot work on QNX as written, gets -w, -T and
 * no -M; any other start goes to System.Native unchanged.
 */
static char *own_path(void);

static ssize_t qnx_readlink(const char *path, char *buf, size_t size)
{
	if (path != NULL && strcmp(path, "/proc/self/exe") == 0) {
		char *self = own_path();
		size_t n;
		if (self == NULL) {
			errno = ENOENT;
			return -1;
		}
		n = strlen(self);
		if (n > size)
			n = size;
		memcpy(buf, self, n); /* as readlink: no terminating NUL */
		free(self);
		return (ssize_t)n;
	}
	return readlink(path, buf, size);
}

/* Appends s to the buffer, single-quoted for sh. */
static void append_quoted(char **buf, size_t *len, const char *s)
{
	size_t need = *len + 3 + 4 * strlen(s);
	char *p;
	*buf = realloc(*buf, need + 1);
	if (*buf == NULL)
		fail("out of memory", NULL);
	p = *buf + *len;
	*p++ = '\'';
	for (; *s != '\0'; s++) {
		if (*s == '\'') {
			memcpy(p, "'\\''", 4);
			p += 4;
		} else {
			*p++ = *s;
		}
	}
	*p++ = '\'';
	*p = '\0';
	*len = (size_t)(p - *buf);
}

static int qnx_execv(const char *path, char *const argv[])
{
	const char *at;
	if (path != NULL && strcmp(path, "/bin/sh") == 0 && argv[0] != NULL && argv[1] != NULL &&
	    strcmp(argv[1], "-l") == 0 && argv[2] != NULL && strcmp(argv[2], "-c") == 0 && argv[3] != NULL &&
	    argv[4] != NULL && (at = strstr(argv[3], "\"$@\"")) != NULL) {
		size_t len = (size_t)(at - argv[3]);
		char *cmd = malloc(len + 1);
		const char *args[5];
		if (cmd == NULL)
			fail("out of memory", NULL);
		memcpy(cmd, argv[3], len);
		cmd[len] = '\0';
		for (int i = 5; argv[i] != NULL; i++) {
			append_quoted(&cmd, &len, argv[i]);
			cmd[len++] = ' ';
			cmd[len] = '\0';
		}
		cmd = realloc(cmd, len + strlen(at + 4) + 1);
		if (cmd == NULL)
			fail("out of memory", NULL);
		strcpy(cmd + len, at + 4); /* whatever followed "$@" */
		args[0] = argv[0];
		args[1] = "-l";
		args[2] = "-c";
		args[3] = cmd;
		args[4] = NULL;
		return execv(path, (char *const *)args);
	}
	return execv(path, argv);
}

typedef int32_t (*fork_and_exec_fn)(const char *, char *const[], char *const[], const char *, int32_t, int32_t,
				    int32_t, int32_t, uint32_t, uint32_t, uint32_t *, int32_t, int32_t *, int32_t *, int32_t *,
				    int32_t *);

/* System.Native's own SystemNative_ForkAndExecProcess, from the first
 * NATIVE_DLL_SEARCH_DIRECTORIES entry that has the library, as the runtime
 * itself finds it (dlopen of the same file returns the same handle). */
static fork_and_exec_fn real_fork_and_exec(void)
{
	static fork_and_exec_fn fn;
	const char *dirs = NULL;

	if (fn != NULL)
		return fn;
	for (int i = 0; i < nproperties; i++)
		if (strcmp(keys[i], "NATIVE_DLL_SEARCH_DIRECTORIES") == 0)
			dirs = values[i];
	while (dirs != NULL && *dirs != '\0' && fn == NULL) {
		const char *end = strchr(dirs, ':');
		size_t n = end != NULL ? (size_t)(end - dirs) : strlen(dirs);
		char path[PATH_MAX];
		void *lib;

		snprintf(path, sizeof path, "%.*s%slibSystem.Native.so", (int)n, dirs,
			 n > 0 && dirs[n - 1] == '/' ? "" : "/");
		if ((lib = dlopen(path, RTLD_NOW)) != NULL)
			fn = (fork_and_exec_fn)dlsym(lib, "SystemNative_ForkAndExecProcess");
		dirs = end != NULL ? end + 1 : NULL;
	}
	if (fn == NULL)
		fail("cannot find SystemNative_ForkAndExecProcess in NATIVE_DLL_SEARCH_DIRECTORIES", NULL);
	return fn;
}

static int32_t qnx_fork_and_exec(const char *filename, char *const argv[], char *const envp[], const char *cwd,
				 int32_t redirectStdin, int32_t redirectStdout, int32_t redirectStderr,
				 int32_t setCredentials, uint32_t userId, uint32_t groupId, uint32_t *groups,
				 int32_t groupsLength, int32_t *childPid, int32_t *stdinFd, int32_t *stdoutFd,
				 int32_t *stderrFd)
{
	const char *base = filename != NULL ? strrchr(filename, '/') : NULL;
	int has_W = 0, n = 0;

	base = base != NULL ? base + 1 : filename;
	if (base != NULL && strcmp(base, "ping") == 0 && argv != NULL)
		for (n = 0; argv[n] != NULL; n++)
			if (n > 0 && strcmp(argv[n], "-W") == 0)
				has_W = 1;
	if (has_W) {
		const char **args = calloc((size_t)n + 1, sizeof *args);
		int32_t r;
		int j = 0;

		if (args == NULL)
			return -1;
		for (int i = 0; i < n; i++) {
			if (i > 0 && strcmp(argv[i], "-M") == 0 && argv[i + 1] != NULL &&
			    (strcmp(argv[i + 1], "do") == 0 || strcmp(argv[i + 1], "dont") == 0)) {
				i++;
				continue;
			}
			if (i > 0 && strcmp(argv[i], "-W") == 0)
				args[j++] = "-w";
			else if (i > 0 && strcmp(argv[i], "-t") == 0)
				args[j++] = "-T";
			else
				args[j++] = argv[i];
		}
		r = real_fork_and_exec()(filename, (char *const *)args, envp, cwd, redirectStdin, redirectStdout, redirectStderr,
					 setCredentials, userId, groupId, groups, groupsLength, childPid, stdinFd,
					 stdoutFd, stderrFd);
		free((void *)args);
		return r;
	}
	return real_fork_and_exec()(filename, argv, envp, cwd, redirectStdin, redirectStdout, redirectStderr,
				    setCredentials, userId, groupId, groups, groupsLength, childPid, stdinFd, stdoutFd,
				    stderrFd);
}

static const void *pinvoke_override(const char *library, const char *entry)
{
	if (library != NULL && entry != NULL && strcmp(library, "libSystem.Native") == 0 &&
	    strcmp(entry, "SystemNative_ForkAndExecProcess") == 0)
		return (const void *)qnx_fork_and_exec;
	if (library == NULL || entry == NULL || strcmp(library, "libc") != 0)
		return NULL;
	if (strcmp(entry, "readlink") == 0)
		return (const void *)qnx_readlink;
	if (strcmp(entry, "execv") == 0)
		return (const void *)qnx_execv;
	return NULL;
}

/*
 * BlackBerry 10's QNX maps, dlopens and executes a file written into the inode
 * of a deleted file that was mapped or executed with the deleted file's cached
 * pages, while read() returns the new contents (measured; truncating the old
 * file, fsync and reading the new one do not clear it, and an upgrade in place
 * does exactly this). msync(MS_INVALIDATE) on a mapping of the new file makes
 * every later mapping, dlopen and exec of it see its contents, so the runtime
 * and the native libraries the program may load get that before any of them
 * is loaded. The runtime does the same for the assemblies it maps. A failure
 * only means the file is used as it is.
 */
static void invalidate_file(const char *path)
{
	struct stat st;
	void *p;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
	    (p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0)) != MAP_FAILED) {
		msync(p, (size_t)st.st_size, MS_INVALIDATE);
		munmap(p, (size_t)st.st_size);
	}
	close(fd);
}

static void invalidate_libraries(void)
{
	const char *dirs = NULL;

	invalidate_file(runtime_path);
	for (int i = 0; i < nproperties; i++)
		if (strcmp(keys[i], "NATIVE_DLL_SEARCH_DIRECTORIES") == 0)
			dirs = values[i];
	while (dirs != NULL && *dirs != '\0') {
		const char *end = strchr(dirs, ':');
		size_t n = end != NULL ? (size_t)(end - dirs) : strlen(dirs);
		char dir[PATH_MAX], path[PATH_MAX];
		DIR *d;
		struct dirent *e;

		snprintf(dir, sizeof dir, "%.*s", (int)n, dirs);
		if (n > 0 && (d = opendir(dir)) != NULL) {
			while ((e = readdir(d)) != NULL) {
				size_t len = strlen(e->d_name);
				if ((len > 3 && strcmp(e->d_name + len - 3, ".so") == 0) || strstr(e->d_name, ".so.") != NULL) {
					snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
					invalidate_file(path);
				}
			}
			closedir(d);
		}
		dirs = end != NULL ? end + 1 : NULL;
	}
}

static void *runtime_thread(void *arg)
{
	void *lib;
	monovm_initialize_fn initialize;
	monovm_execute_assembly_fn execute;
	monovm_shutdown_fn shutdown;
	mono_jit_set_aot_mode_fn set_aot_mode;
	unsigned int code = 1;
	int latched = 0;
	const char *mode;

	(void)arg;
	invalidate_libraries();
	lib = dlopen(runtime_path, RTLD_NOW | RTLD_GLOBAL);
	if (lib == NULL)
		fail("cannot load the runtime", dlerror());
	initialize = (monovm_initialize_fn)dlsym(lib, "monovm_initialize");
	execute = (monovm_execute_assembly_fn)dlsym(lib, "monovm_execute_assembly");
	shutdown = (monovm_shutdown_fn)dlsym(lib, "monovm_shutdown");
	set_aot_mode = (mono_jit_set_aot_mode_fn)dlsym(lib, "mono_jit_set_aot_mode");
	if (initialize == NULL || execute == NULL || shutdown == NULL || set_aot_mode == NULL)
		fail("the runtime lacks the hosting functions", runtime_path);

	mode = getenv("QNXHOST_MODE");
	if (mode != NULL && strcmp(mode, "aot") == 0)
		set_aot_mode(MONO_AOT_MODE_INTERP);
	else if (mode == NULL || strcmp(mode, "jit") != 0)
		set_aot_mode(MONO_AOT_MODE_INTERP_ONLY);
	{
		static char override[32];
		if (nproperties >= MAX_PROPERTIES)
			fail("no room for the PINVOKE_OVERRIDE property (MAX_PROPERTIES)", NULL);
		snprintf(override, sizeof override, "%lu", (unsigned long)(uintptr_t)pinvoke_override);
		keys[nproperties] = "PINVOKE_OVERRIDE";
		values[nproperties++] = override;
	}
	if (initialize(nproperties, keys, values) != 0)
		fail("monovm_initialize failed", NULL);
	if (execute(app_argc, app_argv, app_path, &code) != 0)
		fail("monovm_execute_assembly failed", app_path);
	shutdown(&latched);
	exit_code = latched != 0 ? latched : (int)code;
	return NULL;
}

static void print_mallinfo(void)
{
	struct mallinfo m = mallinfo();

	fprintf(stderr, "QNXHOST mallinfo arena %d in-use %d free %d (small blocks: in use %d, free %d)\n",
		m.arena, m.uordblks + m.usmblks, m.fordblks + m.fsmblks, m.usmblks, m.fsmblks);
}

/* The private directory, resolved, once private_tmpdir has made or checked
 * it, else NULL. */
static const char *private_dir;

/* Sets TMPDIR to the private directory; see the comment at the top. */
static void private_tmpdir(void)
{
	const char *opt = getenv("QNXHOST_PRIVATE_TMPDIR");
	const char *tmp = getenv("TMPDIR");
	static char dir[PATH_MAX];
	char name[32];
	size_t len, nlen;
	struct stat st;

	/* Made in every configuration: System.Native keeps its cross-process
	 * socket lock file there, and finds it by the same rule
	 * (pal_socklock_qnx.c). */
	if (tmp == NULL || tmp[0] == '\0')
		tmp = "/tmp";
	len = strlen(tmp);
	while (len > 1 && tmp[len - 1] == '/')
		len--;
	nlen = (size_t)snprintf(name, sizeof name, "qnxhost-%u", (unsigned)getuid());
	if (len > nlen && tmp[len - nlen - 1] == '/' && strncmp(tmp + len - nlen, name, nlen) == 0)
		snprintf(dir, sizeof dir, "%.*s", (int)len, tmp);
	else if (snprintf(dir, sizeof dir, "%.*s/%s", (int)len, tmp, name) >= (int)sizeof dir) {
		fprintf(stderr, "qnxhost: warning: TMPDIR is too long\n");
		return;
	}
	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		fprintf(stderr, "qnxhost: warning: cannot make %s: %s\n", dir, strerror(errno));
		return;
	}
	if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
		fprintf(stderr, "qnxhost: warning: %s is not a private directory of this user\n", dir);
		return;
	}
	if (getenv("TMPDIR") == NULL && (opt == NULL || strcmp(opt, "0") != 0))
		setenv("TMPDIR", dir, 1);
	/* Resolved, to compare with realpath's results (the time zones). */
	private_dir = realpath(dir, NULL);
}

/* Time zones; see the comment at the top. */

static uint64_t fnv1a(const char *s)
{
	uint64_t h = 14695981039346656037ULL;
	for (; *s != '\0'; s++)
		h = (h ^ (unsigned char)*s) * 1099511628211ULL;
	return h;
}

/* Reads the standard-time part of a POSIX TZ rule, a name ("EST", or
 * "<+0530>" quoted) and its offset ("5", "-5:30", hours west of UTC).
 * Returns 0 if the string does not start like a rule. */
static int rule_std(const char *rule, char *name, size_t size, int32_t *utoff)
{
	const char *p = rule, *start;
	long h, m = 0, sec = 0;
	int west = 1;
	size_t n;

	if (*p == '<') {
		start = ++p;
		while (*p != '\0' && *p != '>')
			p++;
		if (*p != '>')
			return 0;
		n = (size_t)(p++ - start);
	} else {
		start = p;
		while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
			p++;
		n = (size_t)(p - start);
	}
	if (n < 3 || n >= size)
		return 0;
	memcpy(name, start, n);
	name[n] = '\0';
	if (*p == '+' || *p == '-')
		west = *p++ == '+';
	if (*p < '0' || *p > '9')
		return 0;
	h = strtol(p, (char **)&p, 10);
	if (*p == ':') {
		m = strtol(p + 1, (char **)&p, 10);
		if (*p == ':')
			sec = strtol(p + 1, (char **)&p, 10);
	}
	if (h > 167 || m > 59 || sec > 59)
		return 0;
	*utoff = (int32_t)((h * 3600 + m * 60 + sec) * (west ? -1 : 1));
	return 1;
}

static void put32(unsigned char **p, uint32_t v)
{
	for (int i = 3; i >= 0; i--)
		*(*p)++ = (unsigned char)(v >> (8 * i));
}

/* A TZif version 2 file for the rule: one type (standard time), one
 * transition, in 1901, and the rule in the footer. .NET applies the footer
 * only after the last transition, so a file without one would be ignored.
 * Returns its length, 0 if it does not fit. */
static size_t make_tzif(const char *rule, const char *name, int32_t utoff, unsigned char *buf, size_t size)
{
	size_t chars = strlen(name) + 1;
	unsigned char *p = buf;

	if (2 * (44 + 6 + chars) + 9 + strlen(rule) + 2 > size)
		return 0;
	for (int v = 1; v <= 2; v++) {
		memcpy(p, "TZif2", 5);
		memset(p + 5, 0, 15);
		p += 20;
		put32(&p, 0); /* isutcnt */
		put32(&p, 0); /* isstdcnt */
		put32(&p, 0); /* leapcnt */
		put32(&p, v == 2); /* timecnt */
		put32(&p, 1); /* typecnt */
		put32(&p, (uint32_t)chars);
		if (v == 2) {
			put32(&p, 0xffffffffu); /* -2^31 as a 64-bit time */
			put32(&p, 0x80000000u);
			*p++ = 0; /* its type */
		}
		put32(&p, (uint32_t)utoff);
		*p++ = 0; /* isdst */
		*p++ = 0; /* desigidx */
		memcpy(p, name, chars);
		p += chars;
	}
	p += sprintf((char *)p, "\n%s\n", rule);
	return (size_t)(p - buf);
}

/* Writes data to path unless the file already holds exactly that: through
 * a temporary file and rename, so that a reader never sees part of it. */
static int write_file(const char *path, const unsigned char *data, size_t n)
{
	unsigned char old[1024];
	char tmp[PATH_MAX];
	int fd = open(path, O_RDONLY);

	if (fd >= 0) {
		ssize_t got = read(fd, old, sizeof old);
		close(fd);
		if (got == (ssize_t)n && memcmp(old, data, n) == 0)
			return 0;
	}
	if (snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid()) >= (int)sizeof tmp)
		return -1;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	if (write(fd, data, n) != (ssize_t)n || close(fd) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* Makes dir/<every directory component of rel>, refusing symlinks, so that
 * nothing is written into the real zone directory through one. */
static int make_parents(const char *dir, const char *rel)
{
	char path[PATH_MAX];
	const char *slash;
	struct stat st;

	for (slash = strchr(rel, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
		if (snprintf(path, sizeof path, "%s/%.*s", dir, (int)(slash - rel), rel) >= (int)sizeof path)
			return -1;
		if (mkdir(path, 0700) != 0 && errno != EEXIST)
			return -1;
		if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
			return -1;
	}
	return 0;
}

static void rule_zone(void)
{
	const char *tz = getenv("TZ"), *tzdir = getenv("TZDIR"), *c;
	char rule[256], name[64], path[PATH_MAX], zdir[PATH_MAX] = "", *src = NULL;
	unsigned char data[1024];
	int32_t utoff;
	size_t n, len;
	DIR *d;
	struct dirent *e;

	if (tz != NULL) {
		if (snprintf(rule, sizeof rule, "%s", tz) >= (int)sizeof rule)
			return;
	} else if ((n = confstr(_CS_TIMEZONE, rule, sizeof rule)) == 0 || n > sizeof rule) {
		return;
	}
	if (rule[0] == '\0' || rule[0] == ':' || rule[0] == '/')
		return;
	/* Each component must be a plain file name. */
	for (c = rule; c != NULL; c = strchr(c, '/') != NULL ? strchr(c, '/') + 1 : NULL)
		if (*c == '/' || *c == '\0' || strncmp(c, "./", 2) == 0 || strncmp(c, "../", 3) == 0 ||
		    strcmp(c, ".") == 0 || strcmp(c, "..") == 0)
			return;
	if (tzdir == NULL || tzdir[0] == '\0')
		tzdir = "/usr/share/zoneinfo"; /* .NET's default */
	snprintf(path, sizeof path, "%s/%s", tzdir, rule);
	if (access(path, R_OK) == 0)
		goto done; /* a zone of that name exists ("EST5EDT", "America/New_York"), or ours from a parent process */
	if (!rule_std(rule, name, sizeof name, &utoff))
		return; /* a zone name .NET cannot find, or not a rule */
	if (private_dir == NULL)
		return;
	src = realpath(tzdir, NULL);
	len = strlen(private_dir);
	if (src != NULL && strncmp(src, private_dir, len) == 0 && strncmp(src + len, "/zoneinfo-", 10) == 0) {
		snprintf(zdir, sizeof zdir, "%s", src); /* a parent's: already linked */
	} else {
		snprintf(zdir, sizeof zdir, "%s/zoneinfo-%016llx", private_dir,
			 (unsigned long long)fnv1a(src != NULL ? src : ""));
		if (mkdir(zdir, 0700) != 0 && errno != EEXIST)
			goto fail;
		if (src != NULL && (d = opendir(src)) != NULL) {
			char target[PATH_MAX];
			struct stat st;

			while ((e = readdir(d)) != NULL) {
				if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
					continue;
				snprintf(path, sizeof path, "%s/%s", zdir, e->d_name);
				snprintf(target, sizeof target, "%s/%s", src, e->d_name);
				if (lstat(path, &st) != 0 && symlink(target, path) != 0 && errno != EEXIST) {
					closedir(d);
					goto fail;
				}
			}
			closedir(d);
		}
	}
	free(src);
	src = NULL;
	n = make_tzif(rule, name, utoff, data, sizeof data);
	snprintf(path, sizeof path, "%s/%s", zdir, rule);
	if (n == 0 || make_parents(zdir, rule) != 0 || write_file(path, data, n) != 0)
		goto fail;
	setenv("TZDIR", zdir, 1);
done:
	if (tz == NULL)
		setenv("TZ", rule, 1); /* libc's meaning already: it reads the same rule */
	return;
fail:
	fprintf(stderr, "qnxhost: warning: cannot make a zone for TZ rule %s in %s: %s\n", rule, zdir, strerror(errno));
	free(src);
}

/* The executable's resolved path, as the process manager reports it
 * (QNX has no /proc/self/exe); as minipal_getexepath does in the runtime.
 * NULL if it cannot be found. */
static char *own_path(void)
{
	struct {
		procfs_debuginfo info;
		char path[PATH_MAX];
	} map;
	char path[PATH_MAX + 1];
	int fd = open("/proc/self/as", O_RDONLY);

	if (fd < 0)
		return NULL;
	memset(&map, 0, sizeof map);
	if (devctl(fd, DCMD_PROC_MAPDEBUG_BASE, &map, sizeof map, NULL) != EOK || map.info.path[0] == '\0') {
		close(fd);
		return NULL;
	}
	close(fd);
	/* Reported without the leading '/', or as given if relative ("./bin/x"). */
	snprintf(path, sizeof path, "%s%s", map.info.path[0] == '/' || map.info.path[0] == '.' ? "" : "/", map.info.path);
	return realpath(path, NULL);
}

int main(int argc, char **argv)
{
	sigset_t set;
	pthread_attr_t attr;
	pthread_t signals, runtime;
	int err;

	char *self = own_path(), props[PATH_MAX + 16];
	int first = 2; /* index of the program's first argument */

	if (self == NULL && argv[0] != NULL && strcmp(basename(argv[0]), "qnxhost") != 0)
		fail("cannot find this executable's path, so not its props file", argv[0]);
	if (self != NULL && strcmp(basename(self), "qnxhost") != 0) {
		/* Multi-call: <tree>/<name>/<name> reads <tree>/<name>.props. */
		char *name = strdup(basename(self)), *dir = dirname(self);
		snprintf(props, sizeof props, "%s/../%s.props", dir, name);
		first = 1;
	} else if (argc < 2) {
		fprintf(stderr, "usage: %s <program.props> [arguments...]\n", argv[0]);
		return 2;
	} else {
		snprintf(props, sizeof props, "%s", argv[1]);
	}
	read_props(props);
	if (getenv("QNXHOST_MALLINFO") != NULL)
		atexit(print_mallinfo);
	/* The runtime's AOT image loader reads its settings from these. */
	if (getenv("QNXHOST_AOT_LOADER") != NULL)
		setenv("MONO_QNX_AOT_LOADER", getenv("QNXHOST_AOT_LOADER"), 1);
	if (getenv("QNXHOST_VERBOSE") != NULL)
		setenv("MONO_QNX_AOT_LOADER_VERBOSE", "1", 1);
	private_tmpdir();
	rule_zone();
	app_argc = argc - first;
	app_argv = (const char **)argv + first;

	/* Block before any thread exists, so that every thread inherits the mask. */
	async_signal_set(&set);
	pthread_sigmask(SIG_BLOCK, &set, NULL);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);
	err = pthread_create(&signals, &attr, signal_thread, NULL);
	if (err != 0)
		fail("cannot create the signal thread", strerror(err));
	pthread_detach(signals);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, RUNTIME_STACK_SIZE);
#ifdef PTHREAD_STACK_LAZY /* QNX */
	pthread_attr_setstacklazy(&attr, PTHREAD_STACK_LAZY);
#endif
	err = pthread_create(&runtime, &attr, runtime_thread, NULL);
	if (err != 0)
		fail("cannot create the runtime thread", strerror(err));
	pthread_join(runtime, NULL);
	return exit_code;
}
