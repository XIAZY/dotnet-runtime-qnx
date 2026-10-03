#!/usr/bin/env bash
# Copyright (c) Xia Zhongyang.
# Licensed under the MIT License.

# Makes a QNX rootfs for cross-building (ROOTFS_DIR) from QNX's own
# development kit, for an architecture of eng/native/qnx/toolchain.cmake.
# The kits are proprietary: the rootfs is for local builds only; never
# distribute or commit it.
#
#   build-rootfs.sh [--arch x86] <rootfs> <sdp>      QNX Neutrino 6.5.0 x86, from
#                                                    a local QNX SDP 6.5.0
#   build-rootfs.sh [--arch x86] <rootfs> ssh:<host> the same, from a QNX 6.5.0
#                                                    machine with the self-hosted
#                                                    SDP (/usr/qnx650)
#   build-rootfs.sh --arch arm <rootfs> <sdk>        QNX ARMv7, from a local
#                                                    BlackBerry 10 Native SDK
#                                                    (the directory with target/qnx6)
#
# Layout of the result, the paths of an installed QNX system:
#   usr/include, lib, usr/lib, opt/lib   the kit's target headers and libraries
#                                        (opt/lib only where the kit has it)
# and the compiler runtime, which QNX's kits only have for their own compilers:
#   x86: usr/lib/gcc/i486-pc-nto-qnx6.5.0/4.4.2
#        gcc's startup files and libgcc, from the SDP (GPL with the GCC
#        runtime library exception)
#   arm: usr/lib/compiler-rt/libclang_rt.builtins-arm.a
#        LLVM compiler-rt's builtins, emulated TLS included (Apache 2.0 with
#        LLVM exceptions), built here with clang for QNX's ARM ABI from
#        LLVM_PROJECT_DIR, an llvm-project checkout, or else from release
#        llvmorg-18.1.3 fetched from GitHub; this needs git, clang, cmake,
#        ninja and llvm-ar

set -euo pipefail

llvm_tag=llvmorg-18.1.3

arch=x86
if [[ ${1:-} == --arch ]]; then
    arch=${2:-}
    shift 2 || true
fi
if [[ $# -ne 2 || ( $arch != x86 && $arch != arm ) ]]; then
    echo "usage: $0 [--arch x86] <rootfs> <sdp directory | ssh:host>" >&2
    echo "       $0 --arch arm <rootfs> <BlackBerry 10 Native SDK directory>" >&2
    exit 2
fi
rootfs=$1 source=$2

if [[ -e $rootfs ]]; then
    echo "$0: $rootfs exists already" >&2
    exit 2
fi

if [[ $arch == arm ]]; then
    target=$source/target/qnx6
    if [[ ! -f $target/usr/include/sys/neutrino.h || ! -f $target/armle-v7/lib/crt1.o ]]; then
        echo "$0: $source does not look like a BlackBerry 10 Native SDK (no target/qnx6/armle-v7)" >&2
        exit 1
    fi
    mkdir -p "$rootfs"/{usr/include,lib,usr/lib/compiler-rt}
    (cd "$target/usr/include" && tar cf - .) | tar xf - -C "$rootfs/usr/include"
    (cd "$target/armle-v7/lib" && tar cf - .) | tar xf - -C "$rootfs/lib"
    (cd "$target/armle-v7/usr/lib" && tar cf - .) | tar xf - -C "$rootfs/usr/lib"

    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    llvm=${LLVM_PROJECT_DIR:-}
    if [[ -z $llvm ]]; then
        llvm=$work/llvm-project
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$llvm_tag" --filter=blob:none --sparse \
            https://github.com/llvm/llvm-project "$llvm"
        git -C "$llvm" sparse-checkout set compiler-rt/lib/builtins compiler-rt/cmake compiler-rt/include cmake llvm/cmake
    fi
    # The ABI flags of toolchain.cmake's ARM branch. _QNX_SOURCE: the
    # builtins build as C11, under which QNX's headers hide POSIX.
    cflags="-march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp -mno-unaligned-access -nostdinc
        -U__linux__ -U__linux -Ulinux -U__gnu_linux__ -Uunix
        -D__QNX__ -D__QNXNTO__ -D__unix__ -D__unix -D__ELF__ -D__ARM__ -D__LITTLEENDIAN__ -D_QNX_SOURCE
        -isystem $(clang -print-resource-dir)/include -isystem $(cd "$rootfs" && pwd)/usr/include"
    cflags=$(echo $cflags)
    cmake -S "$llvm/compiler-rt/lib/builtins" -B "$work/builtins" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
        -DCMAKE_C_COMPILER=clang -DCMAKE_ASM_COMPILER=clang \
        -DCMAKE_AR="$(command -v llvm-ar)" -DCMAKE_NM="$(command -v llvm-nm)" -DCMAKE_RANLIB="$(command -v llvm-ranlib)" \
        -DCMAKE_C_COMPILER_TARGET=armv7-pc-linux-gnueabi -DCMAKE_ASM_COMPILER_TARGET=armv7-pc-linux-gnueabi \
        -DCMAKE_C_FLAGS="$cflags" -DCMAKE_ASM_FLAGS="$cflags" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
        -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BUILTINS_HIDE_SYMBOLS=ON > "$work/builtins.log" 2>&1 \
        && ninja -C "$work/builtins" >> "$work/builtins.log" 2>&1 \
        || { tail -n 40 "$work/builtins.log" >&2; echo "$0: building compiler-rt's builtins failed" >&2; exit 1; }
    cp "$work/builtins/lib/linux/libclang_rt.builtins-arm.a" "$rootfs/usr/lib/compiler-rt/"
    echo "QNX ARMv7 rootfs in $rootfs"
    exit 0
fi
# gcc's files are under the SDP's host directory, named after the host the
# SDP is installed for (linux, win32, qnx6, ...).
gccsub=x86/usr/lib/gcc/i486-pc-nto-qnx6.5.0/4.4.2

case $source in
ssh:*)
    host=${source#ssh:}
    fetch() { ssh "$host" "cd /usr/qnx650/$1 && tar cf - $2" | tar xf - -C "$3"; }
    exists() { ssh "$host" "test -d /usr/qnx650/$1"; }
    gccdir=host/qnx6/$gccsub
    ;;
*)
    fetch() { (cd "$source/$1" && tar cf - $2) | tar xf - -C "$3"; }
    exists() { [[ -d $source/$1 ]]; }
    gccdir=$(cd "$source" && ls -d host/*/$gccsub 2>/dev/null | head -n 1)
    if [[ -z $gccdir ]]; then
        echo "$0: no host/*/$gccsub in $source" >&2
        exit 2
    fi
    ;;
esac

mkdir -p "$rootfs"/{usr/include,lib,usr/lib,usr/lib/gcc/i486-pc-nto-qnx6.5.0/4.4.2}
fetch target/qnx6/usr/include . "$rootfs/usr/include"
fetch target/qnx6/x86/lib . "$rootfs/lib"
fetch target/qnx6/x86/usr/lib . "$rootfs/usr/lib"
if exists target/qnx6/x86/opt/lib; then
    mkdir -p "$rootfs/opt/lib"
    fetch target/qnx6/x86/opt/lib . "$rootfs/opt/lib"
fi
fetch "$gccdir" "--exclude=cc1 --exclude=cc1plus --exclude=collect2 ." "$rootfs/usr/lib/gcc/i486-pc-nto-qnx6.5.0/4.4.2"

if [[ ! -f $rootfs/usr/include/sys/neutrino.h || ! -f $rootfs/lib/crt1.o ]]; then
    echo "$0: $source does not look like a QNX SDP 6.5.0 installation" >&2
    exit 1
fi
echo "QNX 6.5.0 x86 rootfs in $rootfs"
