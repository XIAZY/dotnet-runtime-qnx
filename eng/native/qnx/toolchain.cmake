# Copyright (c) Xia Zhongyang.
# Licensed under the MIT License.

# CMake toolchain for cross-building for QNX Neutrino 6.5.0 on x86 with clang
# and lld, from the QNX rootfs in ROOTFS_DIR (see build-rootfs.sh).
#
# QNX is not a branch of eng/common/cross/toolchain.cmake because eng/common
# is Arcade's and is overwritten at every Arcade update. This file follows
# that one's conventions: the rootfs comes from ROOTFS_DIR, and the compiler
# from CC/CXX (eng/common/native/init-compiler.sh).

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 6.5.0)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CROSS_ROOTFS $ENV{ROOTFS_DIR})
set(QNX_GCC_DIR ${CROSS_ROOTFS}/usr/lib/gcc/i486-pc-nto-qnx6.5.0/4.4.2)
if(NOT EXISTS ${CROSS_ROOTFS}/usr/include/sys/neutrino.h OR NOT EXISTS ${QNX_GCC_DIR}/crtbegin.o)
  message(FATAL_ERROR "ROOTFS_DIR must name a QNX 6.5.0 x86 rootfs (see eng/native/qnx/build-rootfs.sh)")
endif()

# clang has no QNX 6.5 target, so this is its Linux i386 driver with the
# Linux-specific parts taken out; the i386 SysV ABI is the same.
set(CMAKE_C_COMPILER_TARGET i386-pc-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET i386-pc-linux-gnu)
set(CMAKE_ASM_COMPILER_TARGET i386-pc-linux-gnu)

if(DEFINED ENV{CC})
  set(_qnx_clang $ENV{CC})
else()
  set(_qnx_clang clang)
endif()
execute_process(COMMAND ${_qnx_clang} -print-resource-dir
  OUTPUT_VARIABLE _qnx_clang_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)

#   -march=i686 -msse2 -mfpmath=sse: as dotnet/runtime builds x86 elsewhere;
#     it also gives the 64-bit atomics (cmpxchg8b) the runtime needs.
#   -mstack-alignment=4: QNX 6.5 only guarantees 4-byte aligned stacks.
#   -femulated-tls: QNX 6.5's loader has no PT_TLS; __thread variables go
#     through __emutls_get_address, which gcc 4.4.2's libgcc provides.
#   -fno-use-init-array: constructors in .ctors, run from DT_INIT by gcc's
#     crtbegin.o; QNX 6.5's loader may not know DT_INIT_ARRAY.
#   Headers: include/ (C11 additions to QNX 6.5's C99 headers), clang's own
#     (its intrinsics), gcc 4.4's, then QNX's.
add_compile_options(
  -march=i686 -msse2 -mfpmath=sse -mstack-alignment=4
  -fno-omit-frame-pointer -femulated-tls -fno-use-init-array -nostdinc
  -U__linux__ -U__linux -Ulinux -U__gnu_linux__ -Uunix -U__FLOAT128__ -U__SIZEOF_FLOAT128__
  -D__QNX__ -D__QNXNTO__ -D__unix__ -D__unix -D__ELF__ -D__X86__ -D__i386__
  -D__LITTLEENDIAN__ -D__LANGUAGE_C -D_LANGUAGE_C
  "SHELL:-isystem ${CMAKE_CURRENT_LIST_DIR}/include"
  "SHELL:-isystem ${_qnx_clang_resource_dir}/include"
  "SHELL:-isystem ${QNX_GCC_DIR}/include"
  "SHELL:-isystem ${CROSS_ROOTFS}/usr/include"
  -Wno-unused-command-line-argument)

# Linking: the output has the shape of QNX's own toolchain's, SysV hash, two
# PT_LOADs (read-only data folded into text), DT_RPATH, and no RELRO or
# GNU_STACK segments, which QNX 6.5's loader predates. The startup files and
# libgcc come from the rootfs, so the link rules name them.
set(_r ${CROSS_ROOTFS})
set(_g ${QNX_GCC_DIR})
string(JOIN " " _qnx_ldflags
  -fuse-ld=lld -nostdlib
  -Wl,--hash-style=sysv -Wl,--disable-new-dtags -Wl,-z,norelro -Wl,-z,nognustack -Wl,--no-rosegment
  -Wl,--build-id=none
  -L${_r}/lib -L${_r}/usr/lib -L${_r}/opt/lib
  -Wl,-rpath-link=${_r}/lib:${_r}/usr/lib:${_r}/opt/lib)

# Executables are not position independent (QNX's crt1.o is not PIC); they
# use /usr/lib/ldqnx.so.2 as the interpreter and start at 0x08048000. The
# static libc follows the shared one: some libc functions (popen, glob,
# crypt, ...) are only in libc.a.
set(_qnx_link_exe
  "${_qnx_ldflags} -no-pie -Wl,--dynamic-linker=/usr/lib/ldqnx.so.2 -Wl,--image-base=0x08048000 \
${_r}/lib/crt1.o ${_r}/lib/crti.o ${_g}/crtbegin.o <OBJECTS> -o <TARGET> <LINK_LIBRARIES> \
-x none ${_g}/libgcc.a -lc -Wl,-Bstatic -lc -Wl,-Bdynamic ${_g}/libgcc.a ${_g}/crtend.o ${_r}/lib/crtn.o")
# Shared libraries: PIC crtbegin/crtend and libgcc; libcS holds the parts of
# libc that shared objects link statically. gcc 4.4's libgcc.a does not hide
# its symbols, so --exclude-libs keeps them out of the exports.
set(_qnx_link_shared
  "${_qnx_ldflags} -shared -Wl,--exclude-libs,libgcc.a <SONAME_FLAG><TARGET_SONAME> -o <TARGET> \
${_r}/lib/crti.o ${_g}/pic/crtbegin.o <OBJECTS> <LINK_LIBRARIES> \
-x none ${_g}/pic/libgcc.a -lc -Wl,-Bstatic -lcS -Wl,-Bdynamic ${_g}/pic/libgcc.a ${_g}/pic/crtend.o ${_r}/lib/crtn.o")

foreach(lang C CXX)
  set(CMAKE_${lang}_LINK_EXECUTABLE
    "<CMAKE_${lang}_COMPILER> <FLAGS> <CMAKE_${lang}_LINK_FLAGS> <LINK_FLAGS> ${_qnx_link_exe}")
  set(CMAKE_${lang}_CREATE_SHARED_LIBRARY
    "<CMAKE_${lang}_COMPILER> <CMAKE_SHARED_LIBRARY_${lang}_FLAGS> <LANGUAGE_COMPILE_FLAGS> <LINK_FLAGS> ${_qnx_link_shared}")
  set(CMAKE_${lang}_CREATE_SHARED_MODULE ${CMAKE_${lang}_CREATE_SHARED_LIBRARY})
endforeach()

# try_compile projects re-read this file; they get the rootfs from the
# environment, like the main project.
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_NM llvm-nm)
set(CMAKE_OBJCOPY llvm-objcopy)
set(CMAKE_STRIP llvm-strip)

set(CMAKE_FIND_ROOT_PATH ${CROSS_ROOTFS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
