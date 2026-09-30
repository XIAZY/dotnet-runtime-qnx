/**
 * \file
 * Signal handlers that preserve the FPU and SSE state on QNX Neutrino.
 *
 * Copyright (c) Xia Zhongyang.
 * Licensed under the MIT License.
 */

#ifndef __MONO_SIGNAL_QNX_H__
#define __MONO_SIGNAL_QNX_H__

#include <config.h>

#ifdef HOST_QNX
#include <signal.h>

/*
 * Routes the handler in sa through a trampoline that saves the FPU/SSE state
 * before the handler runs and restores it afterwards. Call before
 * sigaction (signo, sa, ...). Leaves SIG_DFL and SIG_IGN alone.
 */
void
mono_qnx_wrap_signal_handler (int signo, struct sigaction *sa);
#endif

#endif /* __MONO_SIGNAL_QNX_H__ */
