# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

if host_arch == 'loongarch64'
  i386_system_ss.add(files(
    'dune.c',
    'dune-info.c',
    'dune-kvm.c',
    'dune-kvm-init-csr.c',
    'dune-kvm-init-ebase.c',
    'dune-kvm-init-fpu.c',
    'dune-inject-ade.c',
    'dune-loop.c',
    'dune-stack.c',
    'hamt.c',
    'hamt-init.c',
    'hamt-ptw.c',
    'hamt-tlb.c',
    'hamt-error.c',
    'hamt-interrupt.c',
    'entry-fpu.S',
    'entry-stack.S',
    'timer.c',
  ))
else
  i386_system_ss.add(files('hamt-stub.c'))
endif
