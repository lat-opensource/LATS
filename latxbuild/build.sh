#!/bin/sh

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only


make_configure=0
jobs=$(nproc)

build_enable_modules=0
build_print_config=0

LATSRR=""

LATSOPT=""
LATSOPT="${LATSOPT} --enable-latx-opt"
LATSOPT="${LATSOPT} --enable-latx-opt-nofp"
LATSOPT="${LATSOPT} --enable-latx-opt-eflags"
LATSOPT="${LATSOPT} --enable-latx-opt-tu"
LATSOPT="${LATSOPT} --enable-latx-opt-bql"
LATSOPT="${LATSOPT} --enable-latx-opt-mmio-lockless"
LATSOPT="${LATSOPT} --enable-latx-hack-mem"
LATSOPT="${LATSOPT} --enable-atomic"

LATSENABLE=""
LATSENABLE="${LATSENABLE} --enable-spice"
LATSENABLE="${LATSENABLE} --enable-sdl"
LATSENABLE="${LATSENABLE} --enable-slirp"
LATSENABLE="${LATSENABLE} --enable-libusb"
#LATSENABLE="${LATSENABLE} --enable-latx-profiler"

LATSDISABLE=""
LATSDISABLE="${LATSDISABLE} --disable-werror"
LATSDISABLE="${LATSDISABLE} --disable-docs"
LATSDISABLE="${LATSDISABLE} --disable-plugins"
LATSDISABLE="${LATSDISABLE} --disable-tools"

BUILDIR="build"
INBUILDIR=""

BUILDCFG="def"

check_host_deps() {
    missing=""

    if ! python3 -m venv -h >/dev/null 2>&1; then
        missing="$missing python3-venv"
    fi

    if ! python3 -m pip --version >/dev/null 2>&1; then
        missing="$missing python3-pip"
    fi

    if ! python3 -c 'import setuptools' >/dev/null 2>&1; then
        missing="$missing python3-setuptools"
    fi

    if [ "$make_configure" -eq 1 ]; then
        if ! pkg-config --print-errors --exists pixman-1; then
            missing="$missing libpixman-1-dev"
        fi

        if ! pkg-config --print-errors --exists spice-server; then
            missing="$missing libspice-server-dev"
        fi

        if ! pkg-config --print-errors --exists sdl2; then
            missing="$missing libsdl2-dev"
        fi
    fi

    if [ -n "$missing" ]; then
        missing="${missing# }"
        echo "missing build dependencies: $missing"
        echo "please install them first:"
        echo "  sudo apt install -y $missing"
        exit 1
    fi
}

help() {
    echo "Usage:"
    echo "  -c              [configure]"
    echo "                  rel       : release"
    echo "                  def       : default"
    echo "                  rr        : default + record replay"
    echo "                  prof      : default + latx profiler"
    echo "                  dbg       : default with debug enabled"
    echo "                  dbg-rr    : + record replay"
    echo "                  tcg       : basic lat with qemu tcg enabled"
    echo "                  tcg-rr    : + record replay"
    echo "                  user      : user mode (only for perf evaluation)"
    echo "                  user-prof : user mode + latx profiler"
    echo "                  user-dbg  : user mode with debug enabled"
    echo "  -m              build enable modules"
    echo "  -p              print configure options"
    echo "  -j              [N], 同时允许 N 个任务；无参数表明允许无限个任务。"
    echo "  -d              [name] 指定编译目录名"
    echo "                      编译目录默认为build"
    echo "                      -c def时，编译目录为build"
    echo "                      -c rr时，编译目录为build-rr"
    echo "  -h              help"
}

is_safe_build_dir() {
    case "$1" in
    ""|"."|".."|-*|*/*|*\\*)
        return 1
        ;;
    esac
    return 0
}

parseArgs() {
    while getopts "c:mpj:hd:e:" opt; do
        case ${opt} in
        c)
            BUILDCFG="$OPTARG"
            ;;
        d)
            INBUILDIR="$OPTARG"
            ;;
        m)
            build_enable_modules=1
            ;;
        p)
            build_print_config=1
            ;;
        j)
            jobs="$OPTARG"
            ;;
        h)
            help
            exit
            ;;
        # 若选项需要参数但未收到，则走冒号分支
        :)
            help
            exit
            ;;
        # 若遇到未指定的选项，会走问号分支
        \?)
            help
            exit
            ;;
        esac
    done
}

afterparseArgs() {
    if [ "$BUILDCFG" = "def" ] ; then
        make_configure=1
        BUILDIR="build"
    elif [ "$BUILDCFG" = "rel" ] ; then
        make_configure=1
        LATSENABLE="${LATSENABLE} --enable-latx-ndebug"
        LATSDISABLE="${LATSDISABLE} --disable-werror"
        BUILDIR="build-release"
    elif [ "$BUILDCFG" = "vgpu" ] ; then
        make_configure=1
        LATSENABLE="${LATSENABLE} --enable-latx-ndebug"
        LATSDISABLE="${LATSDISABLE} --disable-werror"
        BUILDIR="build-vgpu"
    elif [ "$BUILDCFG" = "vgpu-dbg" ] ; then
        make_configure=1
        LATSENABLE="${LATSENABLE} --enable-debug"
        LATSENABLE="${LATSENABLE} --enable-latx-debug"
        LATSDISABLE="${LATSDISABLE} --disable-capstone-diet"
        BUILDIR="build-vgpu-dbg"
    elif [ "$BUILDCFG" = "vgpu-dbg-opt" ] ; then
        make_configure=1
        LATSENABLE="${LATSENABLE} --enable-latx-debug"
        LATSDISABLE="${LATSDISABLE} --disable-capstone-diet"
        BUILDIR="build-vgpu-dbg-opt"
    elif [ "$BUILDCFG" = "prof" ] ; then
        make_configure=1
        LATSENABLE="${LATSENABLE} --enable-latx-profiler"
        BUILDIR="build-prof"
    elif [ "$BUILDCFG" = "rr" ] ; then
        make_configure=1
        LATSRR="--enable-latx-rr"
        LATSDISABLE="${LATSDISABLE} --disable-capstone-diet"
        BUILDIR="build-rr"
    elif [ "$BUILDCFG" = "dbg-opt" ] ; then
        make_configure=1
        LATSENABLE="${LATSENABLE} --enable-latx-debug"
        LATSDISABLE="${LATSDISABLE} --disable-capstone-diet"
        BUILDIR="build-dbg-opt"
    elif [ "$BUILDCFG" = "dbg" ] ; then
        make_configure=1
        LATSENABLE="${LATSENABLE} --enable-debug"
        LATSENABLE="${LATSENABLE} --enable-latx-debug"
        LATSDISABLE="${LATSDISABLE} --disable-capstone-diet"
        BUILDIR="build-dbg"
    elif [ "$BUILDCFG" = "dbg-rr" ] ; then
        make_configure=1
        LATSRR="--enable-latx-rr"
        LATSENABLE="${LATSENABLE} --enable-debug"
        LATSENABLE="${LATSENABLE} --enable-latx-debug"
        LATSDISABLE="${LATSDISABLE} --disable-capstone-diet"
        BUILDIR="build-dbg-rr"
    elif [ "$BUILDCFG" = "tcg" ] ; then
        make_configure=1
        LATSOPT="--enable-latx-opt-tu --enable-latx-tcg-mode"
        BUILDIR="build-tcg"
    elif [ "$BUILDCFG" = "tcg-rr" ] ; then
        make_configure=1
        LATSOPT="--enable-latx-opt-tu --enable-latx-tcg-mode"
        LATSRR="--enable-latx-rr"
        LATSDISABLE="${LATSDISABLE} --disable-capstone-diet"
        BUILDIR="build-tcg-rr"
    elif [ "$BUILDCFG" = "user" ] ; then
        make_configure=2
        LATSOPT="--enable-latx-opt"
        LATSOPT="${LATSOPT} --enable-latx-opt-tu"
        LATSOPT="${LATSOPT} --enable-latx-opt-nofp"
        LATSOPT="${LATSOPT} --enable-latx-opt-eflags"
        LATSOPT="${LATSOPT} --enable-atomic"
        LATSENABLE=""
        BUILDIR="build-user"
    elif [ "$BUILDCFG" = "user-prof" ] ; then
        make_configure=2
        LATSOPT="--enable-latx-opt"
        LATSOPT="${LATSOPT} --enable-latx-opt-tu"
        LATSOPT="${LATSOPT} --enable-latx-opt-nofp"
        LATSOPT="${LATSOPT} --enable-latx-opt-eflags"
        LATSOPT="${LATSOPT} --enable-atomic"
        LATSENABLE="--enable-latx-profiler"
        BUILDIR="build-user-prof"
    elif [ "$BUILDCFG" = "user-dbg" ] ; then
        make_configure=2
        LATSOPT="--enable-latx-opt"
        LATSOPT="${LATSOPT} --enable-latx-opt-tu"
        LATSOPT="${LATSOPT} --enable-latx-opt-nofp"
        LATSOPT="${LATSOPT} --enable-latx-opt-eflags"
        LATSOPT="${LATSOPT} --enable-atomic"
        LATSENABLE="--enable-debug --enable-latx-debug"
        LATSDISABLE="${LATSDISABLE} --disable-capstone-diet"
        BUILDIR="build-user-dbg"
    elif [ "$BUILDCFG" = "user-dbg-opt" ] ; then
        make_configure=2
        LATSOPT="--enable-latx-opt"
        LATSOPT="${LATSOPT} --enable-latx-opt-tu"
        LATSOPT="${LATSOPT} --enable-latx-opt-nofp"
        LATSOPT="${LATSOPT} --enable-latx-opt-eflags"
        LATSOPT="${LATSOPT} --enable-atomic"
        LATSENABLE="--enable-latx-debug"
        LATSDISABLE="${LATSDISABLE} --disable-capstone-diet"
        BUILDIR="build-user-dbg-opt"
    else
        echo "invalid configure options!"
        help
        exit
    fi

    if [ $build_enable_modules -eq 1 ] ; then
        LATSOPT="${LATSOPT} --enable-modules"
    fi

    if [ "$INBUILDIR" != "" ] ; then
        if ! is_safe_build_dir "$INBUILDIR"; then
            echo "invalid build directory name: $INBUILDIR"
            echo "use a simple name relative to the source tree"
            exit 1
        fi
        BUILDIR="$INBUILDIR"
    fi

    if [ $build_print_config -eq 1 ] ; then
        if [ $make_configure -eq 1 ] ; then
            echo ../configure --target-list=x86_64-softmmu ${LATSENABLE} ${LATSDISABLE} ${LATSOPT} ${LATSRR}
        fi
        if [ $make_configure -eq 2 ] ; then
            echo ../configure --target-list=x86_64-linux-user ${LATSENABLE} ${LATSDISABLE} ${LATSOPT}
        fi
        exit
    fi

    echo "build at [$BUILDIR]"
}

make_cmd() {
    if [ $make_configure -eq 1 ] || [ $make_configure -eq 2 ] ; then
        rm -rf -- "$BUILDIR"
    fi
    mkdir -p -- "$BUILDIR"
    cd "./$BUILDIR"

    if [ $make_configure -eq 1 ] ; then
        ../configure --target-list=x86_64-softmmu \
            ${LATSENABLE} ${LATSDISABLE} ${LATSOPT} ${LATSRR}
    fi

    if [ $make_configure -eq 2 ] ; then
        ../configure --target-list=x86_64-linux-user \
            ${LATSENABLE} ${LATSDISABLE} ${LATSOPT}
    fi

    if [ ! -f "/usr/bin/ninja" ]; then
        make -j $jobs
    else
        ninja -j $jobs
    fi
}

parseArgs "$@"
afterparseArgs
check_host_deps
make_cmd
