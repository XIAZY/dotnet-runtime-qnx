// Copyright (c) Xia Zhongyang.
// Licensed under the MIT License.

// C11 <assert.h> on top of QNX 6.5's C99 one: C11 defines static_assert as a
// macro for _Static_assert, and QNX 6.5's header lacks it, so static_assert
// at file scope would parse as a function declaration. The QNX compiler
// wrapper searches this directory before the rootfs headers.
#include_next <assert.h>

#if !defined(__cplusplus) && !defined(static_assert) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define static_assert _Static_assert
#endif
