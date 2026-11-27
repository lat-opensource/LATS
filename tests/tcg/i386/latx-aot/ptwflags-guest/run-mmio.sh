#!/bin/sh
/sbin/insmod /home/ng/ptwflags-guest/ptwflags_tlb.ko || /sbin/poweroff -f
exec /home/ng/ptwflags-guest/ptwflags_user_mmio
