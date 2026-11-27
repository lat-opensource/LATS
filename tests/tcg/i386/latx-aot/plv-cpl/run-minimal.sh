#!/bin/sh
set -u

dir=/home/ng/plv-cpl
insmod "$dir/plv_tlb.ko" || /sbin/poweroff -f
echo "PLV_RING_TEST_STAGE=exec-user"
echo "PLV_RING_TEST_EXPECT=SIGSEGV-error-5"
exec "$dir/plv_user"
