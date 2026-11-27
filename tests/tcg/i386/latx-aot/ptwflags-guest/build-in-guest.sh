#!/bin/sh
set -u

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
kernel_build=/lib/modules/$(uname -r)/build
PATH=/usr/lib/gcc/x86_64-linux-gnu/5:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
rc=0

build_user()
{
    mode=$1
    output=$2
    echo "PTWFLAGS_BUILD_USER=$output"
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -nostdlib -static \
        -fno-stack-protector -fno-pie -no-pie -Wl,-e,_start \
        -DTEST_MODE="$mode" -o "$dir/ptwflags_user_$output" \
        "$dir/ptwflags_user.c" || rc=$?
}

echo "PTWFLAGS_BUILD_STAGE=module"
make -C "$kernel_build" M="$dir" modules || rc=$?

if [ "$rc" -eq 0 ]; then
    echo "PTWFLAGS_BUILD_STAGE=user"
    build_user 1 baseline
    build_user 2 smc
    build_user 3 vga
    build_user 4 mmio
    build_user 5 super_read
    build_user 6 super_write
    build_user 7 super_exec
    build_user 8 super_write_cold
    build_user 9 readonly_write
    build_user 10 user_present_readonly_write
fi

echo "PTWFLAGS_BUILD_RESULT=$rc"
sync
/sbin/poweroff -f
exit "$rc"
