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
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdint.h>

#include <sys/time.h>
#include <sys/resource.h>

#include "latx-perfmap.h"
#include "latx-hamt.h"

#include "qemu/osdep.h"

#include "dune-mmap.h"
#include "dune-arch.h"
#include "dune-info.h"
#include "dune-stack.h"
#include "dune-kvm.h"
#include "dune-kvm-init.h"
#include "dune-kvm-init-value.h"
#include "loongarch-extcontext.h"
#include "hamt-enter.h"
#include "hamt-entry-func.h"
#include "hamt-exccode.h"

#ifdef LOONGARCH_NEW_WORLD
/* redefine before kvm-csr.h */
#undef KVM_REG_LOONGARCH
#define KVM_REG_LOONGARCH   0x9000000000000000ULL
#endif

#include "kvm-csr.h"
#include "loongarch-csr.h"

struct csr_reg {
    struct kvm_one_reg reg;
    char name[100];
    uint64_t v;
};

enum ACCESS_OP {
    GET = KVM_GET_ONE_REG,
    SET = KVM_SET_ONE_REG,
};

static void kvm_access_reg(const struct dune_kvm_cpu *cpu,
        struct kvm_one_reg *reg, enum ACCESS_OP op)
{
    if (ioctl(cpu->vcpu_fd, op, reg) < 0) {
        dune_die("kvm_access_reg");
    }
}

static uint64_t kvm_access_csr_reg(const struct dune_kvm_cpu *cpu,
        uint64_t id, enum ACCESS_OP op, uint64_t value)
{
    struct kvm_one_reg reg;
    uint64_t v = (op == GET) ? 0 : value;
    reg.addr = (uint64_t) & (v);
    reg.id = id;

    kvm_access_reg(cpu, &reg, op);
    return v;
}

static inline uint64_t kvm_get_csr_reg(
        const struct dune_kvm_cpu *cpu,
        uint64_t id)
{
    return kvm_access_csr_reg(cpu, id, GET, 0);
}

static inline void kvm_set_csr_reg(
        const struct dune_kvm_cpu *cpu,
        uint64_t id, uint64_t v)
{
    kvm_access_csr_reg(cpu, id, SET, v);
}

#define CSR_INIT_REG(X)                                             \
{                                                                   \
    .reg = { .id = KVM_CSR_##X }, .name = #X, .v = INIT_VALUE_##X   \
}

#ifdef LOONGARCH_NEW_WORLD
#define CSR_INIT_REG_CPUCFG(n)          \
{                                       \
    .reg = { .id = KVM_CPUCFG##n },     \
    .name = "CPUCFG"#n,                 \
    .v = INIT_VALUE_CPUCFG##n           \
}
#endif

void dune_kvm_init_csr(struct dune_kvm_cpu *cpu, int mode, int cpuid)
{
#ifdef CONFIG_LATX_PRINT
    dune_info("--------------------------------");
    dune_info("%s cpu %d mode %d", __func__, cpuid, mode);
#endif

    if (!cpu->info.ebase) {
        dune_die("You forget to init ebase");
    }

    uint64_t INIT_VALUE_DMWIN1 = CSR_DMW1_INIT;
    if (hamt_enable_plv()) {
        INIT_VALUE_DMWIN1 = CSR_DMW1_INIT_PLV_ALL;
    }
    uint64_t INIT_VALUE_KSCRATCH5 = (uint64_t)cpu->syscall_parameter + CSR_DMW1_BASE;
    uint64_t INIT_VALUE_KSCRATCH6 = TLBRELO0_STANDARD_BITS;
    uint64_t INIT_VALUE_KSCRATCH7 = TLBRELO1_STANDARD_BITS;

    /* for hamt tlb refill */
    INIT_VALUE_KSCRATCH6 = TLBRELO0_HAMTDMW_BITS;
    INIT_VALUE_KSCRATCH7 = TLBRELO1_HAMTDMW_BITS;

    uint64_t INIT_VALUE_TLBREBASE = (uint64_t)cpu->info.ebase;
    uint64_t INIT_VALUE_EBASE = (uint64_t)cpu->info.ebase + CSR_DMW1_BASE;

    uint64_t INIT_VALUE_CPUNUM = cpu->cpu_id;
    int host_cpu = sched_getcpu();
    if (host_cpu < 0) {
        dune_die("sched_getcpu");
    }
    uint64_t INIT_VALUE_TIMERID = host_cpu;

    uint64_t INIT_VALUE_ASID = hamt_dmw_mode_da() ? 0xf : 0x0;
    /*
     * 16 KB / 8 B = 2K = 2048 = 2^11
     * @0 - 10 =  47 ... 38 D3
     * @1 -  7 =  37 ... 31 D2
     * @2 -  9 =  30 ... 22 D1
     * @3 - 10 =  21 ... 12 PT
     * @4 - entry 11 ... 0
     */
    uint64_t INIT_VALUE_PWCTL0 = 0xff4d94c;
    uint64_t INIT_VALUE_PWCTL1 = 0x2a6;

#ifdef LOONGARCH_NEW_WORLD
    struct kvm_device_attr kvm_attr;
    uint64_t kvm_attr_value = 0;

    /* read and set CPUCFG2 to enable Guest's FP/LSX/LASX/LBT in KVM */
    kvm_attr.group = KVM_LOONGARCH_VCPU_CPUCFG;
    kvm_attr.attr = 0x2; // LOONGARCH_CPUCFG2;
    kvm_attr.addr = (uint64_t)&kvm_attr_value;
    if (ioctl(cpu->vcpu_fd, KVM_GET_DEVICE_ATTR, &kvm_attr) < 0) {
        dune_die("KVM_GET_DEVICE_ATTR failed");
    }
    dune_info("read kvm cpucfg2 0x%016lx", kvm_attr_value);
    uint64_t INIT_VALUE_CPUCFG2 = kvm_attr_value;

    /* enable KVM features */
    kvm_attr.group = KVM_LOONGARCH_VCPU_CPUCFG;
    kvm_attr.attr = 0x40000000 + 4; // kvm_attr.attr = CPUCFG_KVM_FEATURE;
    kvm_attr.addr = (uint64_t)&kvm_attr_value;
    if (ioctl(cpu->vcpu_fd, KVM_GET_DEVICE_ATTR, &kvm_attr) < 0) {
        dune_die("KVM_GET_DEVICE_ATTR failed");
    }
    dune_info("read kvm attr feature 0x%016lx", kvm_attr_value);
//    kvm_attr_value |= 1ULL << 24;
    kvm_attr_value |= 1ULL << 25; // KVM_FEATURE_USER_HCALL
    if (ioctl(cpu->vcpu_fd, KVM_SET_DEVICE_ATTR, &kvm_attr) < 0) {
        dune_die("KVM_SET_DEVICE_ATTR failed");
    }
    kvm_attr_value = 0;
    if (ioctl(cpu->vcpu_fd, KVM_GET_DEVICE_ATTR, &kvm_attr) < 0) {
        dune_die("KVM_GET_DEVICE_ATTR failed");
    }
    dune_info("set kvm attr feature 0x%016lx", kvm_attr_value);
#endif

    uint64_t INIT_VALUE_CRMD = __INIT_VALUE_CRMD;
    if (hamt_dmw_mode_da()) {
        INIT_VALUE_CRMD = __INIT_VALUE_CRMD_DA;
    }

    struct csr_reg one_regs[] = {
        CSR_INIT_REG(CRMD),
        // CSR_INIT_REG(PRMD),
        CSR_INIT_REG(EUEN), CSR_INIT_REG(MISC), CSR_INIT_REG(ECFG),
        // CSR_INIT_REG(ESTAT),
        // CSR_INIT_REG(EPC),
        // CSR_INIT_REG(BADV),
        // CSR_INIT_REG(BADI),
        CSR_INIT_REG(EBASE),
        // CSR_INIT_REG(TLBIDX),
        // CSR_INIT_REG(TLBHI),
        // CSR_INIT_REG(TLBLO0),
        // CSR_INIT_REG(TLBLO1),
        // CSR_INIT_REG(GTLBC),
        // CSR_INIT_REG(TRGP),
        CSR_INIT_REG(ASID),
        // CSR_INIT_REG(PGDL),
        // CSR_INIT_REG(PGDH),
        // CSR_INIT_REG(PGD),
        CSR_INIT_REG(PWCTL0), CSR_INIT_REG(PWCTL1),
        CSR_INIT_REG(STLBPS), CSR_INIT_REG(RVACFG),
        CSR_INIT_REG(CPUNUM),
        // CSR_INIT_REG(PRCFG1),
        // CSR_INIT_REG(PRCFG2),
        // CSR_INIT_REG(PRCFG3),
        // CSR_INIT_REG(KSCRATCH0),
        // CSR_INIT_REG(KSCRATCH1),
        // CSR_INIT_REG(KSCRATCH2),
        // CSR_INIT_REG(KSCRATCH3),
        // CSR_INIT_REG(KSCRATCH4),
        CSR_INIT_REG(KSCRATCH5), CSR_INIT_REG(KSCRATCH6),
        CSR_INIT_REG(KSCRATCH7),
        CSR_INIT_REG(TIMERID),
        CSR_INIT_REG(TIMERCFG),
        // CSR_INIT_REG(TIMERTICK),
        // CSR_INIT_REG(TIMEROFFSET),
        // CSR_INIT_REG(GSTAT), gcsr
        // CSR_INIT_REG(GCFG),  gcsr
        // CSR_INIT_REG(GINTC), gcsr
        // CSR_INIT_REG(GCNTC), gcsr
        // CSR_INIT_REG(LLBCTL),
        // CSR_INIT_REG(IMPCTL1),
        // CSR_INIT_REG(IMPCTL2),
        // CSR_INIT_REG(GNMI),
        CSR_INIT_REG(TLBREBASE),
        // CSR_INIT_REG(TLBRBADV),
        // CSR_INIT_REG(TLBREPC),
        // CSR_INIT_REG(TLBRSAVE),
        // CSR_INIT_REG(TLBRELO0),
        // CSR_INIT_REG(TLBRELO1),
        // CSR_INIT_REG(TLBREHI),
        // CSR_INIT_REG(TLBRPRMD),
        // CSR_INIT_REG(ERRCTL),
        // CSR_INIT_REG(ERRINFO1),
        // CSR_INIT_REG(ERRINFO2),
        // CSR_INIT_REG(ERREBASE),
        // CSR_INIT_REG(ERREPC),
        // CSR_INIT_REG(ERRSAVE),
        // CSR_INIT_REG(CTAG),
        // CSR_INIT_REG(MCSR0),
        // CSR_INIT_REG(MCSR1),
        // CSR_INIT_REG(MCSR2),
        // CSR_INIT_REG(MCSR3),
        // CSR_INIT_REG(MCSR8),
        // CSR_INIT_REG(MCSR9),
        // CSR_INIT_REG(MCSR10),
        // CSR_INIT_REG(MCSR24),
        // CSR_INIT_REG(UCWIN),
        // CSR_INIT_REG(UCWIN0_LO),
        // CSR_INIT_REG(UCWIN0_HI),
        // CSR_INIT_REG(UCWIN1_LO),
        // CSR_INIT_REG(UCWIN1_HI),
        // CSR_INIT_REG(UCWIN2_LO),
        // CSR_INIT_REG(UCWIN2_HI),
        // CSR_INIT_REG(UCWIN3_LO),
        // CSR_INIT_REG(UCWIN3_HI),
        // CSR_INIT_REG(DMWIN0),
        CSR_INIT_REG(DMWIN1),
        // CSR_INIT_REG(DMWIN2),
        // CSR_INIT_REG(DMWIN3),
        // CSR_INIT_REG(PERF0_EVENT),
        // CSR_INIT_REG(PERF0_COUNT),
        // CSR_INIT_REG(PERF1_EVENT),
        // CSR_INIT_REG(PERF1_COUNT),
        // CSR_INIT_REG(PERF2_EVENT),
        // CSR_INIT_REG(PERF2_COUNT),
        // CSR_INIT_REG(PERF3_EVENT),
        // CSR_INIT_REG(PERF3_COUNT),
        // CSR_INIT_REG(DEBUG),
        // CSR_INIT_REG(DEPC),
        // CSR_INIT_REG(DESAVE),
#ifdef LOONGARCH_NEW_WORLD
        CSR_INIT_REG_CPUCFG(2),
#endif
    };

    for (int i = 0; i < sizeof(one_regs) / sizeof(struct csr_reg); ++i) {
        one_regs[i].reg.addr = (uint64_t) & (one_regs[i].v);
    }

    for (int i = 0; i < sizeof(one_regs) / sizeof(struct csr_reg); ++i) {
        kvm_set_csr_reg(cpu, one_regs[i].reg.id, one_regs[i].v);
#ifdef CONFIG_LATX_PRINT
        dune_info("Init CSR 0x%02x %-10s 0x%llx",
                ((one_regs[i].reg.id) >> 3) & 0xff,
                one_regs[i].name, one_regs[i].v);
#endif
    }
#ifdef CONFIG_LATX_PRINT
    uint64_t t40 = kvm_get_csr_reg(cpu, KVM_CSR_TIMERID);
    uint64_t t41 = kvm_get_csr_reg(cpu, KVM_CSR_TIMERCFG);
    uint64_t t42 = kvm_get_csr_reg(cpu, KVM_CSR_TIMERTICK);
    uint64_t t43 = kvm_get_csr_reg(cpu, KVM_CSR_TIMEROFFSET);
    dune_info("read CSR_TIMERID 0x%lx", t40);
    dune_info("read CSR_TIMERCFG 0x%lx", t41);
    dune_info("read CSR_TIMERTICK 0x%lx", t42);
    dune_info("read CSR_TIMEROFFSET 0x%lx", t43);
#endif
}
