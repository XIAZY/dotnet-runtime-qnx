#!/usr/bin/env bash
# Copyright (c) Xia Zhongyang.
# Licensed under the MIT License.

# Makes a QNX Neutrino 6.5.0 x86 rootfs for cross-building (ROOTFS_DIR) from a
# QNX Software Development Platform (SDP) 6.5.0 installation. The SDP is
# proprietary: the rootfs is for local builds only; never distribute or
# commit it.
#
#   build-rootfs.sh <rootfs> <sdp>          from a local SDP installation
#   build-rootfs.sh <rootfs> ssh:<host>     from a QNX 6.5.0 machine with the
#                                           self-hosted SDP (/usr/qnx650)
#
# Layout of the result, the paths of an installed QNX system:
#   usr/include, lib, usr/lib, opt/lib   the SDP's x86 target headers and libraries
#                                        (opt/lib only where the SDP has it)
#   usr/lib/gcc/i486-pc-nto-qnx6.5.0/4.4.2
#                                        gcc's startup files and libgcc (GPL with
#                                        the GCC runtime library exception)

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <rootfs> <sdp directory | ssh:host>" >&2
    exit 2
fi
rootfs=$1 source=$2
# gcc's files are under the SDP's host directory, named after the host the
# SDP is installed for (linux, win32, qnx6, ...).
gccsub=x86/usr/lib/gcc/i486-pc-nto-qnx6.5.0/4.4.2

if [[ -e $rootfs ]]; then
    echo "$0: $rootfs exists already" >&2
    exit 2
fi

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
