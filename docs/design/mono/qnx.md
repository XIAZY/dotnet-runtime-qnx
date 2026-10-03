# QNX 6.5

Mono and the native libraries run on QNX Neutrino 6.5.0 on 32-bit x86, and
on BlackBerry 10, QNX on 32-bit ARMv7 (see the last part, "BlackBerry 10").
This note records where QNX differs from Linux in ways that shaped the port:
for each, the fact, what it breaks, the solution chosen, and the
alternatives that were rejected. Unless a section says otherwise, the facts
were measured on QNX 6.5.0 x86.

The port uses the managed libraries of `linux-x86` unmodified. Everything
QNX-specific is in Mono (`HOST_QNX`), in System.Native (`__QNXNTO__`), and in
`qnxhost` (`src/mono/qnx/host`), the host that replaces the `dotnet` host on
QNX. It builds with clang and lld against the QNX SDP 6.5.0 sysroot
(`eng/native/qnx`).

## The Linux personality

**Fact.** .NET has no runtime identifier for QNX, and the managed libraries
decide most platform behaviour at build time, per identifier.

**Consequence.** A QNX identifier would mean building and maintaining every
managed library for it.

**Solution.** The runtime identifier is `linux-x86`, and the native layer
presents the Linux behaviour the managed libraries expect: Linux errno
values, the `/proc` files they read, Linux socket and signal semantics.
`OperatingSystem.IsLinux()` is true on QNX; `RuntimeInformation.OSDescription`
reports QNX.

**Rejected.** A QNX identifier with managed changes, for the reason above.

## The host

**Fact.** The `dotnet` host is C++ and needs a C++ runtime that QNX 6.5 does
not have. QNX has no `/proc/self/exe`. QNX 6.5's `/bin/sh` (ksh) drops the
positional parameters of `-c` when `-l` is given: `sh -l -c 'echo $#' "" a b`
prints 0, and 2 without `-l`.

**Consequence.** Something else has to start the runtime. Programs that
start themselves again by their own path (PowerShell does for jobs and
`-Login`) need that path to be theirs, not the host's. PowerShell's login
code reads `/proc/self/exe` with libc's `readlink`, and runs
`/bin/sh -l -c 'exec pwsh "$@"' "" <args>`, which lost every argument.

**Solution.** `qnxhost`, in C, reads a props file with the properties the
`dotnet` host would pass (trusted assemblies, search paths, runtime
settings), confines signals (below) and starts the runtime on a thread with
an 8 MiB lazy stack, since the main thread's is 516 KiB. It is multi-call: a
copy named `<program>` at `<tree>/<program>/<program>` reads
`<tree>/<program>.props`, found from its own path as the process manager
reports it (`DCMD_PROC_MAPDEBUG_BASE`), never from `argv[0]`, so
`Environment.ProcessPath` and the process name are the program's. A
P/Invoke override (the `PINVOKE_OVERRIDE` property) answers `readlink` of
`/proc/self/exe` and rewrites that one `/bin/sh -l -c` call to carry its
arguments in the command string; everything else resolves as usual.

**Rejected.** A symbolic link to `qnxhost` as the program: the process
manager reports the resolved path, `qnxhost` itself. Finding the props file
from `argv[0]`: a `PATH` lookup or a login shell's `-pwsh` does not give a
path.

## Signal handlers and FPU state

**Fact.** QNX 6.5 does not preserve FPU and SSE state across a signal
handler: when a handler writes `xmm` registers, the interrupted code finds
its own values changed. Measured on QNX 6.5.0 under QEMU: a first probe
found the interrupted code's registers changed in 4 program runs of 10; a
probe that holds a pattern in `xmm0`-`xmm7` while a timer signal arrives
every 500 µs found it changed after about 9 handler runs in 10. A handler also
starts from the interrupted code's state: its direction flag, MXCSR and x87
control word.

**Consequence.** The runtime is built with SSE2, so any C code a handler
runs, its callees included, may use `xmm` registers, and floating-point
results in the interrupted code would change at random.

**Solution.** Every handler Mono installs is entered through
`mono_qnx_signal_trampoline` (`mono/utils/mono-signal-qnx.c`), and
System.Native's through `SystemNative_QnxSignalTrampoline`. Both are written
in assembly, so that nothing runs before the state is saved. They align a
512-byte area to 16 bytes (QNX aligns the stack to 4 only), `fxsave`, give
the handler the state the ABI promises at a call (`cld`, `fninit`, MXCSR
`0x1f80`), call it, and `fxrstor`. No pattern was lost in about 10,000
handler runs behind the trampoline. The FPU state in the signal context
(`uc_mcontext.fpu`) does not hold the interrupted state, so nothing reads it:
`UCONTEXT_HAS_XMM` stays undefined on QNX.

**Rejected.** Building only the handlers without SSE: it would not cover
what they call. Restoring from the signal context: it does not hold the
interrupted values.

## No `SA_RESTART`, no `sigaltstack`

**Fact.** QNX 6.5 has neither. A signal that arrives on a thread blocked in
`read`, `write` or `pthread_cond_wait` makes the call fail with `EINTR`.

**Consequence.** The runtime, the native libraries and managed code assume
Linux restarts those calls; any thread could fail at random under signals.

**Solution.** Confinement. `qnxhost` blocks the asynchronous signals
(`SIGINT`, `SIGTERM`, `SIGCHLD`, `SIGWINCH`, `SIGUSR1` and the others) in the
main thread before any other thread exists, so that every thread inherits
the mask, and a signal thread of its own unblocks them and waits in `pause`.
Process-directed signals are delivered only there, and the handlers the
runtime and System.Native install run on that thread; System.Native's
forwards the signal to its pipe, as on Linux. Synchronous signals, `SIGPIPE`
and the real-time signals Mono sends to its own threads stay unblocked, and
Mono already copes with `EINTR` where it interrupts its own threads.
`SA_RESTART` is defined as 0 where the sources use it. A child process must
not inherit the mask, so on QNX it starts with no signal blocked. Mono's
alternate-stack handling of stack overflow (`ENABLE_SIGALTSTACK`) stays off,
as it is by default.

**Rejected.** Retrying `EINTR` at every call site: the calls are spread over
the runtime, the native libraries and the managed code, and missing one
would fail only under load.

## No ELF TLS

**Fact.** QNX 6.5's dynamic loader does not support `PT_TLS` segments.

**Consequence.** `__thread` variables, which the runtime uses, cannot be
loaded.

**Solution.** Everything is compiled with `-femulated-tls`: thread-local
variables go through `__emutls_get_address` (from the gcc 4.4 libgcc of the
QNX SDP). The AOT image loader refuses images with a `PT_TLS` segment.

## Eager commit, `MAP_LAZY` and the AOT image loader

**Fact.** QNX 6.5 commits RAM for the whole of a mapping when it is made,
unless the mapping is `MAP_LAZY`; Linux commits pages when they are first
touched. A 256 MiB `PROT_NONE` reservation takes 256 MB. A private file
mapping costs twice the file's size at map time. `dlopen` commits every
page of a library at load, and `posix_madvise(POSIX_MADV_DONTNEED)` frees
nothing. Running out of memory on a lazy page raises `SIGBUS` with
`BUS_OBJERR` at the first touch.

**Consequence.** Reservations, card tables, assemblies and AOT images would
each cost their full size in RAM, in every process.

**Solution** (`mono/utils/mono-mmap.c`, `mono/utils/mono-dl-qnx.c`):
- anonymous memory is lazy by accounting type: every `PROT_NONE`
  reservation, the SGen card tables and the interpreter's stacks. The
  nursery and the major heap stay eager, so that running out of memory
  fails at `mmap`;
- aligned allocations reserve the slack lazily and map the aligned part over
  it;
- assemblies are mapped `MAP_SHARED|MAP_LAZY`: only the pages read cost
  memory, shared between processes;
- discarding maps fresh `MAP_FIXED|MAP_LAZY` pages over the range;
- `SIGBUS` with `BUS_OBJERR` prints "Out of memory" and exits with status 1;
- AOT images (`*.dll.so`) are mapped by `mono_dl_open` itself instead of
  `dlopen`: a `PROT_NONE` reservation, the read-execute segment
  `MAP_SHARED|MAP_LAZY`, the read-write segment `MAP_PRIVATE|MAP_LAZY`, then
  the `R_386_RELATIVE` relocations. An image with anything else (TLS, other
  relocations, needed libraries) falls back to `dlopen`.

**Rejected.** `dlopen` for AOT images: a program pays for every image in full
at startup, used or not. Linking the images into the executable: the
executable's pages are committed the same way. Reading the images' working
set ahead of a cold start: measured slower than faulting the pages in.

## Page faults on a cold start

**Fact.** A page fault on a file mapping fills QNX's block cache one page
per round trip to the disk driver: on a cold cache it costs about 11 times
as much per byte as a `read()`. `posix_fadvise` and `posix_madvise` with
`WILLNEED` return 0 and read nothing ahead.

**Consequence.** With lazily mapped assemblies and AOT images, the first
start after a boot reads its working set fault by fault: about 2.4 s, of
which about 0.6 s is CPU, against about 0.7 s once the files are cached.

**Solution.** None in the runtime: the lazy mappings stay, because they save
the memory of everything not used.

**Rejected.** Reading the working set (58 MB) before starting: 3.6 s, or
3.2 s in a thread beside the runtime, since the disk, not the CPU, is the
limit. A prefetch of only the pages a start touches, or AOT images laid out
in the order the methods run, could help; neither is done.

## Ahead-of-time code on x86

**Fact.** Upstream Mono lacked pieces x86 needs to run AOT code together
with the interpreter: the unbox-arbitrary trampoline, the native-to-interp
entry trampoline and its call-context helpers, and static rgctx trampolines
that find the GOT without EBX, which only AOT code sets (native code calling
an interpreted `[UnmanagedCallersOnly]` method reaches one). x86 AOT images
are also large: about three times arm64's code for the same assemblies,
because x86 Mono compiles `Vector<T>` and `Vector128<T>` as ordinary code and,
without `MONO_ARCH_DYN_CALL_SUPPORTED`, emits one runtime-invoke wrapper per
signature.

**Consequence.** Mixed mode did not work on x86, and full AOT of the
libraries a program uses costs hundreds of megabytes.

**Solution.** The missing trampolines are added for x86, with fixes found
on the way (an uncompiled method taking its neighbour's unbox trampoline;
`get_native_call_context_ret` using the wrong `CallInfo`; decoding
`UnmanagedCallConv` arguments without creating managed objects). The AOT
cross compiler gets i686 Linux and QNX targets; images are built on the
build host, never on QNX. The default configuration is normal (not full)
AOT images of the libraries used at startup, compiled with profiles and the
new `native-wrappers` option, which puts P/Invoke, icall and JIT icall
wrappers into normal images too, with the JIT for the rest: 27 MiB of images,
and 187 methods left to the JIT at startup.

**Rejected.** Full AOT of the same libraries with the interpreter for the
rest: 184 MiB of images. Interpreter only: slower, for about the same
memory.

## `poll()` and the socket event port

**Fact.** QNX 6.5 gives programs neither epoll nor kqueue; libc has `poll`,
`select` and `ionotify`.

**Consequence.** .NET's `SocketAsyncEngine` needs an event port for every
asynchronous socket and pipe, a child process's redirected output included.

**Solution.** A port on `ionotify` and pulses (`pal_networking.c`). The port
is a channel that receives pulses. Registration arms each direction with
`_NOTIFY_ACTION_POLLARM`, the action `poll` and `select` use, and posts an
event for a direction that is ready already, as epoll reports readiness at
registration. Each I/O function that can leave `EAGAIN` or `EINPROGRESS` on
a registered descriptor arms that direction again, and posts the event
itself if the descriptor is ready or cannot be armed. A pulse carries the
descriptor and a registration sequence number, so that pulses from an
earlier registration of the same number are dropped; closing a descriptor
forgets it, since .NET never unregisters before closing.

**Rejected.** `_NOTIFY_ACTION_TRANARM`: it fails with `EBUSY` when another
process is blocked in `select` on the same file, and a pending input arm
refuses every further arm on the descriptor (for a pipe, on its other end
too), so a child process that `select`s on a pipe it shares would break it.

## The io-pkt unlink deadlock and the socket locks

**Fact.** On QNX 6.5, unlinking a Unix socket's name while the network stack
(io-pkt) serves another socket request, from any process, deadlocks io-pkt
until a reboot. An unlink with no other request in progress can do it too, less
often: measurements by the QNX port of Go saw it in a tight loop of one
program, after close, somewhere between hundreds and tens of thousands of
removals, and, more rarely, with the socket still open.

**Consequence.** .NET unlinks socket names in `Socket.Dispose` of a bound
Unix socket, in every `NamedPipeServerStream`, and in `File.Delete` of a
socket.

**Solution** (`pal_socklock_qnx.c`). A process-wide read-write lock is taken
shared around the socket control calls (`socket`, `bind`, `listen`,
`connect` and `accept` on non-blocking descriptors, the option and name
calls) and `close`, and exclusively around the unlink of a name that `lstat`
reports as a socket. Under it, an `fcntl` record lock on `socket.lock` in
`qnxhost-<uid>`, a private directory in `$TMPDIR` (`/tmp` when unset), does
the same between the processes of one user; record locks belong to the
process, so the first thread to take the shared side takes the read lock and
the last releases it. A pair of `fcntl` calls costs 16-20 µs. Reads, writes
and the event port's arms stay unlocked: 700,440 transfers overlapping
20,000 locked unlinks, and about 49,000 arms overlapping 4,000, caused no
deadlock. `qnxhost` also gives programs a private temporary directory, where
.NET creates its named pipes, so that programs listing `/tmp` don't touch
those names.

The locks cover only the concurrent form; nothing can make a lone unlink
safe. What protects a program is creating no names in ordinary use: Mono
creates no diagnostics server socket on QNX by default, and PowerShell, for
example, turns its host IPC listener off.

**Rejected.** The `fcntl` lock alone: it does not exclude threads of the same
process. A lock across all users: other users' processes and programs not
built on .NET (`sshd`) cannot be made to take it, so they are not covered.
Unlinking a bound socket's name in `SystemNative_Close`, before the close
(the order that made the lone-unlink hang rarer): an accepted socket reports
its listener's path, so closing a connection would delete the listener's
name, and the name may by then belong to another socket.

## `/proc` emulation

**Fact.** QNX's `/proc` holds, per process, only an address-space file,
`as`; the information comes from `devctl` on it. There is no
`/proc/self/exe`, and no `/proc/net`.

**Consequence.** `Process`, `Environment.ProcessPath` and
`System.Net.NetworkInformation` read Linux's files.

**Solution** (`pal_procfs_qnx.c`). `SystemNative_Open`, `Stat`, `LStat` and
`ReadLink` recognise `/proc/<pid>/{stat,status,cmdline,maps,exe}` and
`/proc/net/route`, and serve Linux-format text built at open from
`DCMD_PROC_INFO`, `DCMD_PROC_MAPINFO`, `DCMD_PROC_PAGEDATA`,
`DCMD_PROC_MAPDEBUG` and `sysctl(NET_RT_DUMP)`, through an unlinked shared
memory object, which seeks and supports `pread` as .NET requires. There is
no counterpart for the thread list, handle counts or peak memory, so they
read as empty, 0 and the current values. `minipal_getexepath` uses
`DCMD_PROC_MAPDEBUG_BASE`.

**Rejected.** Changing the managed readers: see the Linux personality.

## `fork` and `vfork`

**Fact.** `fork` fails with `ENOSYS` in a multithreaded process. `vfork`
works but fails spuriously with `EBADF` while other threads open and close
descriptors (1,783 times in 2,000).

**Consequence.** `Process.Start` failed in any program with more than one
thread, which every .NET program has.

**Solution.** `ForkAndExecProcess` always uses `vfork` on QNX, retried on
`EBADF`, with every signal blocked around it; the child resets handlers and
starts with an empty signal mask, kept in its own `sigset_t` since it shares
the parent's memory until `execve`.

## The SMP kernel freeze under process creation and disk flushes

**Fact.** On a KVM-based cloud VM with 2 vCPUs, QNX 6.5's SMP kernel
(`procnto-smp-instr`) froze the whole machine, until a power cycle, under
process creation beside periodic disk flushes. No .NET code is needed: four
parallel ssh login loops (each login a `fork` in sshd), beside a file write
and `sync` every 2 s, froze it in both runs, after about 2.5 and 4.5 minutes
(the cleanly measured one with both vCPUs at 100% and no disk or network I/O
in the host's metrics). The same load ran 8 minutes twice on the
uniprocessor kernel (`procnto-instr`) booted from the same disk, and on SMP
it also ran 8 minutes with the writes but without the `sync`.

**Consequence.** A program that starts processes often while the disk is
being flushed can stop such a machine; PowerShell starting `ssh` children in
a loop beside small writes did within minutes. Ordinary interactive use does
not come near it, and on the uniprocessor kernel, PowerShell's full upstream
test suite (over 12,000 tests, two runs of about 37 minutes) ran without a
stop.

**Solution.** None in the runtime. The workaround is the uniprocessor
kernel: QNX 6.5's stock install ships `qnxbasedma.ifs` (`procnto-instr`, disk
DMA on) beside the SMP image, selectable at the boot loader's menu; setting
the VM to one vCPU works as well.

**Rejected.** Changing how the runtime creates processes: the machine froze
with no .NET process running at all. Starting processes with QNX's `spawn()`
instead of `vfork` was also tried, and the machine also stopped.

**Not known.** Which kernel path spins, and where else it shows. It did not
appear in two runs (8 and 30 minutes) of the same load under QEMU's full
emulation (TCG, 2 CPUs, the same kernel with a patched local-APIC startup
that QEMU requires, an emulated IDE disk), about three times as many logins
as the KVM VM needed to freeze, by an estimate from login rates. So whether
it shows depends on the virtual machine or on that boot-time APIC setup,
which differed too; the QNX kernel may still hold the underlying bug. The
APIC startup, which the QEMU runs used, does not boot on the KVM VM: it
cannot find the IOAPIC pin of the timer, so the two startups cannot be
compared there. Real SMP hardware and other hypervisors have not been
tested.

## `inotify` by polling

**Fact.** QNX 6.5 has no inotify. (BlackBerry 10's libc has it, without
`inotify_init1`; there System.Native uses it, chosen by the `HAVE_INOTIFY`
configure check, with `inotify_init` and `FD_CLOEXEC`.)

**Consequence.** `FileSystemWatcher` failed with "Not supported".

**Solution** (`pal_inotify_qnx.c`, where configure finds no inotify). `SystemNative_INotifyInit` returns the
read end of a pipe, fed by a thread that scans the watched directories with
`readdir` and `lstat` and writes the differences as `inotify_event` records:
create, delete, modify, attribute changes, moves paired by inode within one
scan, `IN_IGNORED` and `IN_Q_OVERFLOW`. The interval is 250 ms, lengthened to
keep near 300 `lstat` calls a second. Each record is one `write` of at most
`PIPE_BUF` bytes, and no more than 4,096 bytes are left unread, so that a
read never splits a record. The directories are scanned with no lock held.
Changes within one interval merge, a rewrite that keeps both size and
modification time (whole seconds) is not seen, and there are no access
events. The managed watcher is unchanged.

## Static constructor order (not QNX-specific)

**Fact.** ECMA-335 lets a `beforefieldinit` type initializer run at any time
before the first access to one of its static fields. Mono's JIT and AOT code
run it when the method that accesses the field is compiled or loaded;
CoreCLR and Mono's interpreter run it at the access.

**Consequence.** Code written against CoreCLR can depend on the order. In
PowerShell, `PSVersionInfo`'s static constructor sets `PSVersion` and then
reads `RemotingConstants`, whose initializer reads `PSVersion`. With the JIT
or AOT code, `RemotingConstants` was initialized first, its host version
stayed null, and every out-of-process runspace (`Start-Job`) failed.

**Solution.** In a static constructor, an access to a static field of
another `beforefieldinit` class initializes that class at the access, as on
CoreCLR. Other methods keep the current behaviour.

## Other differences

| Fact | Solution |
|---|---|
| No `mkstemps` | The fallback in `SystemNative_MksTemps` fills the X's before the suffix itself and opens with `O_CREAT \| O_EXCL`; before, it created the file without the suffix. Not QNX-specific: bionic lacks `mkstemps` too |
| `strerror_r` returns `EINVAL` for an unknown error and leaves the buffer as it was | "Unknown error *n*" is written instead of returning uninitialized text. Not QNX-specific |
| `getnameinfo` fails with `EAI_FAIL` unless `sin_len`/`sin6_len` is set | The addresses System.Native builds set it |
| io-pkt reports `AF_UNIX` address lengths at nearly the full `sockaddr_un` size | `Accept`, `GetPeerName` and `GetSockName` trim them to Linux's lengths |
| `int64_t` is 8-aligned inside structs (as Mono's managed `long` on QNX), `long long` 4-aligned | Structs shared with managed code use `int64_t` only |
| `clock_getres(CLOCK_MONOTONIC)` fails with `EINVAL`, while `clock_gettime` works | Mono probes the resolution once on `CLOCK_REALTIME` and keeps `errno` |
| `ftruncate` of a special file fails with `ENOSYS` where Linux gives `EINVAL` | Mapped to `EINVAL` for files that are not regular |
| `printf` crashes on a `NULL` `%s` | Mono's logging guards the arguments that can be `NULL` |
| No `O_NOFOLLOW`, `mkdtemp` or `futimes`; file times are whole seconds | Emulated (`futimes` with `futime`, to the second) |
| No mount table API | The root is reported as the only mount point |
| `EALREADY` equals `EBUSY` | Errno 16 maps to `EBUSY` |

## Raw sockets and ping

**Fact.** Raw IPv4 sockets ignore `IP_TTL` when sending and have no option
for the don't-fragment bit, but honour an IP header supplied with
`IP_HDRINCL`. A connected raw socket receives only its peer's packets, so it
never sees a router's Time Exceeded, and `sendto` on a connected socket
fails with `EISCONN`. Raw sockets need root, and there are no unprivileged
ICMP sockets.

**Solution.** For raw ICMP sockets, System.Native remembers `IP_TTL` and
`DontFragment` per descriptor and, once either is set, sends its own IP
header. `Connect` records the peer instead of connecting, and `Send`,
`SendMessage` and `GetPeerName` use it, as .NET's `Ping` uses the socket on
FreeBSD and macOS. Without root, .NET runs the `ping` utility with Linux's
options; QNX's `ping` takes the timeout as `-w` and the TTL as `-T`, and has
no `-M`, so `qnxhost`'s P/Invoke override rewrites that one command.

**Rejected.** Emulating Linux's `IP_RECVERR` to report each router's address
during a traceroute: hop addresses are reported as on FreeBSD and macOS.

## Thread priorities

**Fact.** `sched_get_priority_min..max` is 1..255 for root and 1..63 for
other users; threads start at 10, and system services run between: io-pkt
at 21.

**Consequence.** `mono_thread_internal_set_priority` spreads .NET's five
levels over that range, so Normal was 128 (32 for other users), above every
service, and a busy managed thread starved the machine: with two CPU-bound
managed threads on two processors, ssh stopped answering for the whole
minute they ran.

**Solution.** On QNX, Normal is the priority of the thread that starts the
runtime, read once in `mono_thread_init`, and each level adds or subtracts
one: 8 to 12 from the default 10, for root and other users alike.

**Rejected.** Taking the base from the first thread whose priority is set:
a thread created at another level would shift every later one.

## Time zones

**Fact.** QNX 6.5 has no time-zone database. The local zone is a POSIX rule
string in `TZ` (`EST5EDT4,M3.2.0/2,M11.1.0/2`) or `confstr(_CS_TIMEZONE)`,
and libc reads `TZ` again on every `mktime`. .NET reads a `TZ` that is not
an absolute path as a file under `TZDIR`, and knows nothing of rule strings.

**Consequence.** .NET's local zone was UTC.

**Solution.** `qnxhost` writes the rule as a one-transition TZif file, with
the rule in its footer, under the rule's own name in a private `TZDIR` whose
other entries link to the real zone directory. `TZ` is unchanged, so libc
and child processes keep its meaning; with `TZ` unset, the system's rule is
used and put in `TZ`. On BlackBerry 10 the system's zone is an IANA name
(`Europe/Amsterdam`), which its libc reads from `TZ` as well; it is put in
`TZ` when .NET finds it under `TZDIR`. Programs ship the IANA zones and point
`TZDIR` at them.

**Rejected.** Rewriting `TZ` for .NET: libc would lose the zone. Turning a
rule into a zone name in `TZ`: QNX 6.5's libc understands only rule strings
and would use UTC.

# BlackBerry 10

BlackBerry 10 runs BlackBerry's fork of QNX Neutrino (6.5/6.6 era, reporting
itself as 8.0.0) on 32-bit ARMv7. It is the same operating-system target as
QNX 6.5 (`-os qnx`), with the ARM architecture (`-arch arm`): where the two
systems differ, the code chooses by configure checks (`HAVE_*`) or by
architecture, never by an OS version. The managed libraries are those of
`linux-arm`, unmodified. The facts in this part were measured on a
BlackBerry 10 phone (4 Krait cores, VFPv4 and NEON).

## Building for ARM

`eng/native/qnx/build-rootfs.sh --arch arm <rootfs> <sdk>` makes the rootfs
from a BlackBerry 10 Native SDK (the directory with `target/qnx6`), and
builds LLVM compiler-rt's builtins into it, since the SDK has no libgcc;
the SDK is proprietary, and the rootfs is for local builds only. Then
`./build.sh -os qnx -arch arm -cross` with `ROOTFS_DIR` set builds the
runtime and the native libraries.

- clang's Linux ARM EABI driver with QNX's predefines; ARMv7-A, VFPv3 and
  QNX's softfp ABI (floating-point arguments in integer registers), which
  Mono's ARM code uses unless told otherwise (`mono.proj` asks for hard
  float only on Linux).
- lld with two load segments at 4 KiB alignment: QNX's loader fails to load
  a large library in lld's default layout (three segments at 64 KiB).
- Constructors stay in `.init_array`, which the ARM loader runs.
- The AOT cross compiler's triple is `armv7-unknown-nto-qnx6.5.0eabi`.

## What differs from QNX 6.5

| | QNX 6.5 x86 | BlackBerry 10 | In the port |
|---|---|---|---|
| ELF TLS | none | none, and no `__aeabi_read_tp` | emulated TLS on both; compiler-rt's on ARM |
| Stack protector runtime | none | in libc | `HAVE_STACK_PROTECTOR_RUNTIME` |
| inotify | none | in libc, without `inotify_init1` | `HAVE_INOTIFY` |
| `<syslog.h>` | yes | no (libc has `syslog`) | `HAVE_SYSLOG_H`; without it, `SysLog` does nothing |
| Memory commit | eager unless `MAP_LAZY` | lazy, `dlopen` included | the same `MAP_LAZY` code serves both |
| System time zone | a POSIX rule | an IANA name | `qnxhost` accepts both |
| Floating-point state in signal handlers | not preserved | not preserved | a trampoline per architecture |
| CPU features | `cpuid` | no auxiliary vector, no `/proc/cpuinfo` | the system page |
| Managed libraries | `linux-x86` | `linux-arm` | unmodified on both |

## Signal handlers and VFP state

**Fact.** As QNX 6.5 does with the FPU/SSE registers, BlackBerry 10 does not
preserve the interrupted code's VFP registers around a signal handler that
uses them (lost in 4,455 of 4,462 handler runs in a probe), and the handler
starts with the interrupted code's FPSCR.

**Solution.** Mono's and System.Native's handlers are entered through an ARM
trampoline beside the x86 one: it saves d0-d31 and FPSCR on an 8-byte
aligned stack, runs the handler with the default FPSCR, and restores them.
The trampolines are naked functions with `target("arm")`: written as
top-level assembly in a Thumb-compiled file, the assembler gave the symbol
the Thumb bit, and the kernel entered ARM code in Thumb state.

## The instruction cache

**Fact.** compiler-rt's `__clear_cache` has no QNX implementation; it calls
`abort ()`.

**Solution.** On QNX, `mono_arch_flush_icache` uses QNX's `msync` on whole
pages: `MS_SYNC | MS_CACHE_ONLY` to clean the data cache, then
`MS_INVALIDATE_ICACHE`.

## CPU features

**Fact.** QNX has neither the auxiliary vector nor `/proc/cpuinfo`, so Mono
found neither ARMv7 nor VFP.

**Solution.** `mono-hwcap-arm.c` reads the system page
(`SYSPAGE_ENTRY (cpuinfo)->flags`: `ARM_CPU_FLAG_V7`, `CPU_FLAG_FPU`), and
Mono's CMake sets `HAVE_ARMV7` from what the compiler targets, so that the
runtime, the JIT and the AOT compiler agree.

## Stale pages after an inode is reused

**Fact.** BlackBerry 10 maps, `dlopen`s and executes a file written into the
inode of a deleted file that was mapped or executed with the deleted file's
cached pages, while `read ()` returns the new contents. Truncating the old
file, `fsync` and reading the new one do not clear it;
`msync (MS_INVALIDATE)` on a mapping of the new file does, for every later
mapping, `dlopen` and `exec` of it. An upgrade in place, which deletes the
old files and writes new ones, does exactly this.

**Consequence.** After an upgrade, assemblies mapped as the files they
replaced (the runtime found no assembly in them), libraries loaded without
their symbols, and programs failed to start ("Can't access shared library").

**Solution.**
- Mono's file mapping on QNX compares the first page of each new mapping with
  `read ()` and invalidates the mapping when they differ: the files mapped
  there begin with content-specific headers (an assembly's PE header carries
  a hash of its content).
- The AOT image loader does the same before anything can refuse an image, so
  the `dlopen` fallback gets the right pages too. Two builds of an image can
  share their first page; the AOT runtime then refuses the image on its
  assembly GUID, and the JIT compiles the methods.
- `qnxhost` invalidates the runtime and every shared library in
  `NATIVE_DLL_SEARCH_DIRECTORIES` before loading the runtime.

On the phone, the guards cost about 0.1 s at startup each.

**Not covered.** The launcher itself (a copy of `qnxhost`) cannot invalidate
its own executable: if it fails to start right after an upgrade, a reboot
clears the stale pages. The names of loaded modules other than the main
module, as the process manager reports them, can be a deleted file's after a
reinstall; `/proc/<pid>/exe` (and so `Environment.ProcessPath`) and the main
module's name come from `DCMD_PROC_MAPDEBUG_BASE`, which is right.

## Ahead-of-time code on ARM

**Fact.** The ARM backend takes the EABI calling convention (64-bit
arguments in aligned register pairs) from `__ARM_EABI__` when it runs as the
JIT, but in a cross compiler built for another host only from the `mtriple`
AOT option, and only for "gnueabi" triples.

**Consequence.** Images compiled without it used the old ARM convention, and
calls between them and JIT code passed 64-bit arguments in the wrong
registers.

**Solution.** Any triple containing "eabi" selects the EABI convention, and
the images are compiled with `mtriple=armv7-unknown-nto-qnx6.5.0eabi`. They
have the same shape as on x86 (two load segments, only `R_ARM_RELATIVE`
relocations in the writable one), and the lazy image loader takes the host's
machine and relocation type. With the hot set precompiled, PowerShell starts
in about 4.4 s on the phone instead of 15.6 s.

## Limitations

- Programs cannot run from the SD card (FAT, mounted without execute
  permission), and code mapped from it faults; the AOT image loader leaves
  images on such a filesystem to `dlopen`.
- A process that is not root sees only its own processes (`/proc/<pid>/as`
  is root-only), so `Process.GetProcesses` returns only the user's own.
- Pinging (`System.Net.NetworkInformation.Ping`) is not available: without
  root, .NET runs the system's `ping`, which ordinary users cannot execute on
  BlackBerry 10, and programs from outside the system cannot run as root
  (root's loader refuses them). The raw-socket ICMP emulation of QNX 6.5
  (see "Raw sockets and ping") is therefore never used there.
- The system's ICU (49) is older than .NET's minimum, so globalization is
  invariant, as on QNX 6.5.

