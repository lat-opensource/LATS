/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdarg.h>
#include <stddef.h>
#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "qemu/osdep.h"
#include "qemu/compiler.h"
#include "qemu/host-utils.h"
#include "qemu/typedefs.h"
#include "exec/cpu-defs.h"
#include "cpu.h"
#include "exec/cpu-all.h"
#include "exec/exec-all.h"
#include "qemu/xxhash.h"
#include "exec/ram_addr.h"

#include "dune-arch.h"
#include "dune-kvm.h"
#include "dune-info.h"

#include "hamt-internal.h"
#include "hamt-init.h"
#include "hamt-enter.h"
#include "hamt.h"
#include "hamt-iocsr.h"
#include "hamt-interrupt.h"
#include "hamt-entry-func.h"
#include "hamt-exccode.h"
#include "latx-sys-hamt.h"

_Static_assert((HAMT_CPU_MAX - 1) * 8 < HAMT_IPI_SEQ_INDEX,
               "IPI acknowledgement slots overlap the sequence");
_Static_assert(HAMT_IPI_MASK_INDEX < 4096 / sizeof(uint64_t),
               "IPI snapshot scratch exceeds its data page");

uint64_t hamt_prepare_ipi(int cpuid, int n)
{
    uint64_t *d = (void *)(hamt_ipi_data_area[cpuid]);
    uint64_t sequence = qatomic_read(&d[HAMT_IPI_SEQ_INDEX]) + 1;

    (void)n;
    /* Zero is reserved for never acknowledged; wraparound is unsupported. */
    g_assert(sequence != 0);
    qatomic_set(&d[HAMT_IPI_SEQ_INDEX], sequence);
    smp_mb();
    return sequence;
}

int hamt_finish_ipi(int cpuid, uint64_t *r,
        int n, int mask, int waitcpus, uint64_t sequence)
{
    uint64_t *d = (void *)(hamt_ipi_data_area[cpuid]);
    int res = 0;
    for (int i = 0; i < n; ++i) {
        if (i == cpuid || !(mask & (1<<i))) continue;
        if (qatomic_read(&d[i << 3]) == sequence) {
            res += 1;
        }
    }
    *r = res;
    return res == waitcpus;
}

void hamt_send_ipi(int cpuid, void *target_cpu)
{
    if (!(hamt_enabled() && hamt_enable_ipi() && hamt_is_running())) {
        return;
    }

    CPUState *cs = target_cpu;
    struct dune_kvm_cpu *kvmcpu = cs->hamt_kvmcpu_ptr;
    if (!kvmcpu) return;

    int irq = cpuid;
    int c = cs->cpu_index;
    assert(c == kvmcpu->vcpu_id);

    uint32_t val = IOCSR_IPI_SEND_BLOCKING;
    val |= irq;
    val |= (c << IOCSR_IPI_SEND_CPU_SHIFT);
    iocsr_write32(val, LOONGARCH_IOCSR_IPI_SEND);
}

void hamt_enable_local_ipi(void)
{
    if (!(hamt_enabled() && hamt_enable_ipi() && hamt_is_running())) {
        return;
    }

    /*
     * before enter kvm :
     * CRMD.IE and ECFG.IPI is initialized in dune_kvm_init_csr()
     */

    iocsr_write32(0xffffffff, LOONGARCH_IOCSR_IPI_EN); 
#ifdef CONFIG_LATX_PRINT
    uint32_t ipien = iocsr_read32(LOONGARCH_IOCSR_IPI_EN); 
    printf("%s:%d IPIEN 0x%x\n", __func__, __LINE__, ipien);
#endif
    iocsr_write32(0xffffffff, LOONGARCH_IOCSR_IPI_CLEAR);
}
