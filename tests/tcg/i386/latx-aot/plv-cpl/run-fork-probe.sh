#!/bin/sh
set -u

dir=/home/ng/plv-cpl

echo "PLV_FORK_RUNNER=start"
"$dir/plv_fork"
rc=$?
echo "PLV_FORK_RUNNER_RESULT=$rc"

sync
/sbin/poweroff -f
exit "$rc"
