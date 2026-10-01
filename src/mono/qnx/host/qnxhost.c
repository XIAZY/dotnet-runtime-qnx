// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

/*
 * qnxhost: runs a .NET program on QNX Neutrino 6.5 with the Mono runtime,
 * in place of the dotnet host (C++), which QNX cannot run.
 *
 *     qnxhost <program.props> [program arguments...]
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
 *  - Makes a private directory, /tmp/qnxhost-<uid> (mode 0700), in every
 *    configuration: System.Native keeps its cross-process socket lock file
 *    there. It gives the program that directory as TMPDIR, unless
 *    TMPDIR is set already (by the environment or the props file) or
 *    QNXHOST_PRIVATE_TMPDIR=0.
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
 */
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* Sets TMPDIR to /tmp/qnxhost-<uid>; see the comment at the top. */
static void private_tmpdir(void)
{
	const char *opt = getenv("QNXHOST_PRIVATE_TMPDIR");
	char dir[64];
	struct stat st;

	/* Made in every configuration: System.Native keeps its cross-process
	 * socket lock file there (pal_socklock_qnx.c). */
	snprintf(dir, sizeof dir, "/tmp/qnxhost-%u", (unsigned)getuid());
	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		fprintf(stderr, "qnxhost: warning: cannot make %s: %s\n", dir, strerror(errno));
		return;
	}
	if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
		fprintf(stderr, "qnxhost: warning: %s is not a private directory of this user\n", dir);
		return;
	}
	if (getenv("TMPDIR") != NULL || (opt != NULL && strcmp(opt, "0") == 0))
		return;
	setenv("TMPDIR", dir, 1);
}

int main(int argc, char **argv)
{
	sigset_t set;
	pthread_attr_t attr;
	pthread_t signals, runtime;
	int err;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <program.props> [arguments...]\n", argv[0]);
		return 2;
	}
	read_props(argv[1]);
	if (getenv("QNXHOST_MALLINFO") != NULL)
		atexit(print_mallinfo);
	/* The runtime's AOT image loader reads its settings from these. */
	if (getenv("QNXHOST_AOT_LOADER") != NULL)
		setenv("MONO_QNX_AOT_LOADER", getenv("QNXHOST_AOT_LOADER"), 1);
	if (getenv("QNXHOST_VERBOSE") != NULL)
		setenv("MONO_QNX_AOT_LOADER_VERBOSE", "1", 1);
	private_tmpdir();
	app_argc = argc - 2;
	app_argv = (const char **)argv + 2;

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
