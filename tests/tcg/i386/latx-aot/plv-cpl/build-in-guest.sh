#!/bin/sh
set -u

dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
kernel_build=/lib/modules/$(uname -r)/build
PATH=/usr/lib/gcc/x86_64-linux-gnu/5:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
rc=0

echo "PLV_RING_BUILD_STAGE=module"
make -C "$kernel_build" M="$dir" modules || rc=$?

if [ "$rc" -eq 0 ]; then
    echo "PLV_RING_BUILD_STAGE=user"
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -nostdlib -static \
        -fno-stack-protector -fno-pie -no-pie -Wl,-e,_start \
        -o "$dir/plv_user" "$dir/plv_user.c" || rc=$?
fi

if [ "$rc" -eq 0 ]; then
    echo "PLV_FORK_BUILD_STAGE=user"
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -static \
        -fno-stack-protector -fno-pie -no-pie \
        -o "$dir/plv_fork" "$dir/plv_fork.c" || rc=$?
fi


echo "PLV_RING_BUILD_RESULT=$rc"
sync
/sbin/poweroff -f
exit "$rc"
