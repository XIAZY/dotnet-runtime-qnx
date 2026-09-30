/**
 * \file
 * Signal handlers that preserve the FPU and SSE state on QNX Neutrino.
 *
 * QNX 6.5 does not reliably save the interrupted code's FPU/SSE registers
 * around a signal handler: when the handler uses them, the interrupted code
 * can find them changed (measured: 4
 * runs in 10 under QEMU). The runtime is built with SSE2, so any C code,
 * handlers included, may use xmm registers. Every handler is therefore
 * entered through a trampoline, written in assembly so that no compiler
 * generated code runs first, which saves the state with fxsave, calls the
 * handler, and restores the state with fxrstor.
 *
 * Copyright (c) Xia Zhongyang.
 * Licensed under the MIT License.
 */

#include <config.h>

#if defined(HOST_QNX) && defined(TARGET_X86)

#include <mono/utils/mono-signal-qnx.h>
#include <glib.h>

typedef void (*MonoQnxSignalHandler) (int, siginfo_t *, void *);

/* The real handler of each signal; read by the trampoline. */
/* Hidden, like every symbol of the runtime not marked for export. */
MonoQnxSignalHandler mono_qnx_signal_handlers [_SIGMAX + 1];

void mono_qnx_signal_trampoline (int signo, siginfo_t *info, void *context);

/*
 * The fxsave area is 512 bytes and must be 16-byte aligned, and the stack is
 * only 4-byte aligned on QNX. The handler table is reached through the GOT,
 * since this is position-independent code.
 */
__asm__ (
	".text\n"
	".p2align 4\n"
	".globl mono_qnx_signal_trampoline\n"
	".hidden mono_qnx_signal_trampoline\n"
	".type mono_qnx_signal_trampoline, @function\n"
	"mono_qnx_signal_trampoline:\n"
	"	pushl %ebp\n"
	"	movl %esp, %ebp\n"
	"	pushl %ebx\n"
	"	subl $524, %esp\n"		/* the fxsave area, and room to align it */
	"	andl $-16, %esp\n"
	"	fxsave (%esp)\n"
	"	call 1f\n"
	"1:	popl %ebx\n"
	"	addl $_GLOBAL_OFFSET_TABLE_+(.-1b), %ebx\n"
	"	movl 8(%ebp), %eax\n"		/* signo */
	"	movl mono_qnx_signal_handlers@GOT(%ebx), %ecx\n"
	"	movl (%ecx,%eax,4), %ecx\n"
	"	subl $16, %esp\n"		/* three arguments, keeping the alignment */
	"	movl %eax, 0(%esp)\n"
	"	movl 12(%ebp), %edx\n"		/* info */
	"	movl %edx, 4(%esp)\n"
	"	movl 16(%ebp), %edx\n"		/* context */
	"	movl %edx, 8(%esp)\n"
	"	testl %ecx, %ecx\n"
	"	jz 2f\n"
	"	call *%ecx\n"
	"2:	addl $16, %esp\n"
	"	fxrstor (%esp)\n"
	"	movl -4(%ebp), %ebx\n"
	"	movl %ebp, %esp\n"
	"	popl %ebp\n"
	"	ret\n"
	".size mono_qnx_signal_trampoline, .-mono_qnx_signal_trampoline\n"
);

void
mono_qnx_wrap_signal_handler (int signo, struct sigaction *sa)
{
	if (signo <= 0 || signo > _SIGMAX)
		return;
	if (sa->sa_handler == SIG_DFL || sa->sa_handler == SIG_IGN)
		return;
	if ((void (*) (int, siginfo_t *, void *))sa->sa_sigaction == mono_qnx_signal_trampoline)
		return;
	/* sa_handler and sa_sigaction share storage; the trampoline passes all three arguments. */
	mono_qnx_signal_handlers [signo] = (MonoQnxSignalHandler)sa->sa_sigaction;
	sa->sa_sigaction = mono_qnx_signal_trampoline;
}

#else

#include <mono/utils/mono-compiler.h>

MONO_EMPTY_SOURCE_FILE (mono_signal_qnx);

#endif
