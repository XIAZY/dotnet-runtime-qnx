# Copyright (c) Xia Zhongyang.
# Licensed under the MIT License.

# CMake toolchain for cross-building for QNX Neutrino with clang and lld, from
# the QNX rootfs in ROOTFS_DIR (see build-rootfs.sh), for one of:
#   x86   QNX Neutrino 6.5.0 on 32-bit x86, from the QNX SDP 6.5.0
#   arm   QNX Neutrino on 32-bit ARMv7 (little endian), as on BlackBerry 10,
#         from the BlackBerry 10 Native SDK
# The architecture comes from TARGET_BUILD_ARCH (eng/native/gen-buildsys.sh),
# and is x86 when that is not set.
#
# QNX is not a branch of eng/common/cross/toolchain.cmake because eng/common
# is Arcade's and is overwritten at every Arcade update. This file follows
# that one's conventions: the rootfs comes from ROOTFS_DIR, and the compiler
# from CC/CXX (eng/common/native/init-compiler.sh).

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 6.5.0)

set(CROSS_ROOTFS $ENV{ROOTFS_DIR})
set(_qnx_arch $ENV{TARGET_BUILD_ARCH})
if(NOT _qnx_arch)
  set(_qnx_arch x86)
endif()

if(_qnx_arch STREQUAL x86)
  set(CMAKE_SYSTEM_PROCESSOR x86)
  set(QNX_GCC_DIR ${CROSS_ROOTFS}/usr/lib/gcc/i486-pc-nto-qnx6.5.0/4.4.2)
  if(NOT EXISTS ${CROSS_ROOTFS}/usr/include/sys/neutrino.h OR NOT EXISTS ${QNX_GCC_DIR}/crtbegin.o)
    message(FATAL_ERROR "ROOTFS_DIR must name a QNX 6.5.0 x86 rootfs (see eng/native/qnx/build-rootfs.sh)")
  endif()
  # clang has no QNX 6.5 target, so this is its Linux i386 driver with the
  # Linux-specific parts taken out; the i386 SysV ABI is the same.
  set(_qnx_triple i386-pc-linux-gnu)
elseif(_qnx_arch STREQUAL arm)
  set(CMAKE_SYSTEM_PROCESSOR arm)
  set(QNX_BUILTINS ${CROSS_ROOTFS}/usr/lib/compiler-rt/libclang_rt.builtins-arm.a)
  if(NOT EXISTS ${CROSS_ROOTFS}/usr/include/sys/neutrino.h OR NOT EXISTS ${QNX_BUILTINS})
    message(FATAL_ERROR "ROOTFS_DIR must name a QNX ARMv7 rootfs (see eng/native/qnx/build-rootfs.sh)")
  endif()
  # As for x86, clang's Linux driver: the ARM EABI is the same.
  set(_qnx_triple armv7-pc-linux-gnueabi)
else()
  message(FATAL_ERROR "QNX: unsupported TARGET_BUILD_ARCH '${_qnx_arch}' (x86 or arm)")
endif()

set(CMAKE_C_COMPILER_TARGET ${_qnx_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_qnx_triple})
set(CMAKE_ASM_COMPILER_TARGET ${_qnx_triple})

if(DEFINED ENV{CC})
  set(_qnx_clang $ENV{CC})
else()
  set(_qnx_clang clang)
endif()
execute_process(COMMAND ${_qnx_clang} -print-resource-dir
  OUTPUT_VARIABLE _qnx_clang_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)

# Both: -femulated-tls, because QNX's loader has no PT_TLS (and BlackBerry
# 10's libc no __aeabi_read_tp); __thread variables go through
# __emutls_get_address, from gcc's libgcc on x86 and from compiler-rt's
# builtins on ARM. Headers: include/ (C11 additions to QNX's C99 headers),
# clang's own (its intrinsics), then QNX's.
#
# x86:
#   -march=i686 -msse2 -mfpmath=sse: as dotnet/runtime builds x86 elsewhere;
#     it also gives the 64-bit atomics (cmpxchg8b) the runtime needs.
#   -mstack-alignment=4: QNX 6.5 only guarantees 4-byte aligned stacks.
#   -fno-use-init-array: constructors in .ctors, run from DT_INIT by gcc's
#     crtbegin.o; QNX 6.5's loader may not know DT_INIT_ARRAY.
#   gcc 4.4's headers come before QNX's.
# ARM:
#   -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp: QNX's ARMv7 ABI passes
#     floating-point arguments in integer registers (softfp, as Android's
#     armeabi-v7a), with VFP instructions; every BlackBerry 10 device has
#     VFPv3 with 32 double registers.
#   -mno-unaligned-access: as BlackBerry's own toolchain builds.
#   Constructors stay in .init_array, which QNX's ARM loader runs.
if(_qnx_arch STREQUAL x86)
  add_compile_options(
    -march=i686 -msse2 -mfpmath=sse -mstack-alignment=4 -fno-use-init-array
    -D__X86__ -D__i386__)
  set(_qnx_arch_include "SHELL:-isystem ${QNX_GCC_DIR}/include")
else()
  add_compile_options(
    -march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp -mno-unaligned-access
    -D__ARM__)
  set(_qnx_arch_include)
endif()
add_compile_options(
  -fno-omit-frame-pointer -femulated-tls -nostdinc
  -U__linux__ -U__linux -Ulinux -U__gnu_linux__ -Uunix -U__FLOAT128__ -U__SIZEOF_FLOAT128__
  -D__QNX__ -D__QNXNTO__ -D__unix__ -D__unix -D__ELF__
  -D__LITTLEENDIAN__ -D__LANGUAGE_C -D_LANGUAGE_C
  "SHELL:-isystem ${CMAKE_CURRENT_LIST_DIR}/include"
  "SHELL:-isystem ${_qnx_clang_resource_dir}/include"
  ${_qnx_arch_include}
  "SHELL:-isystem ${CROSS_ROOTFS}/usr/include"
  -Wno-unused-command-line-argument)

# Linking: the output has the shape of QNX's own toolchain's, SysV hash, two
# PT_LOADs (read-only data folded into text), DT_RPATH, and no RELRO or
# GNU_STACK segments, which QNX 6.5's loader predates. The startup files and
# the compiler runtime come from the rootfs, so the link rules name them.
set(_r ${CROSS_ROOTFS})
string(JOIN " " _qnx_ldflags
  -fuse-ld=lld -nostdlib
  -Wl,--hash-style=sysv -Wl,--disable-new-dtags -Wl,-z,norelro -Wl,-z,nognustack -Wl,--no-rosegment
  -Wl,--build-id=none
  -L${_r}/lib -L${_r}/usr/lib -L${_r}/opt/lib
  -Wl,-rpath-link=${_r}/lib:${_r}/usr/lib:${_r}/opt/lib)

# Executables are not position independent (QNX's crt1.o is not PIC); they
# use /usr/lib/ldqnx.so.2 as the interpreter and start where QNX's own
# linker starts them. The static libc follows the shared one: some libc
# functions (popen, glob, ...) are only in libc.a.
# Shared libraries: libcS holds the parts of libc that shared objects link
# statically. The compiler runtime's symbols stay out of the exports.
if(_qnx_arch STREQUAL x86)
  # gcc's crtbegin/crtend run the .ctors; PIC ones for shared libraries.
  # gcc 4.4's libgcc.a does not hide its symbols, hence --exclude-libs.
  set(_g ${QNX_GCC_DIR})
  set(_qnx_link_exe
    "${_qnx_ldflags} -no-pie -Wl,--dynamic-linker=/usr/lib/ldqnx.so.2 -Wl,--image-base=0x08048000 \
${_r}/lib/crt1.o ${_r}/lib/crti.o ${_g}/crtbegin.o <OBJECTS> -o <TARGET> <LINK_LIBRARIES> \
-x none ${_g}/libgcc.a -lc -Wl,-Bstatic -lc -Wl,-Bdynamic ${_g}/libgcc.a ${_g}/crtend.o ${_r}/lib/crtn.o")
  set(_qnx_link_shared
    "${_qnx_ldflags} -shared -Wl,--exclude-libs,libgcc.a <SONAME_FLAG><TARGET_SONAME> -o <TARGET> \
${_r}/lib/crti.o ${_g}/pic/crtbegin.o <OBJECTS> <LINK_LIBRARIES> \
-x none ${_g}/pic/libgcc.a -lc -Wl,-Bstatic -lcS -Wl,-Bdynamic ${_g}/pic/libgcc.a ${_g}/pic/crtend.o ${_r}/lib/crtn.o")
else()
  # No crtbegin/crtend: the loader runs .init_array. compiler-rt's emulated
  # TLS has default visibility, hence --exclude-libs. 4 KiB pages, as QNX's
  # ARM linker uses.
  set(_b ${QNX_BUILTINS})
  set(_qnx_link_exe
    "${_qnx_ldflags} -Wl,-z,max-page-size=0x1000 -no-pie -Wl,--dynamic-linker=/usr/lib/ldqnx.so.2 -Wl,--image-base=0x00100000 \
${_r}/lib/crt1.o ${_r}/lib/crti.o <OBJECTS> -o <TARGET> <LINK_LIBRARIES> \
-x none ${_b} -lc -Wl,-Bstatic -lc -Wl,-Bdynamic ${_b} ${_r}/lib/crtn.o")
  set(_qnx_link_shared
    "${_qnx_ldflags} -Wl,-z,max-page-size=0x1000 -shared -Wl,--exclude-libs,libclang_rt.builtins-arm.a <SONAME_FLAG><TARGET_SONAME> -o <TARGET> \
${_r}/lib/crti.o <OBJECTS> <LINK_LIBRARIES> \
-x none ${_b} -lc -Wl,-Bstatic -lcS -Wl,-Bdynamic ${_b} ${_r}/lib/crtn.o")
endif()

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
