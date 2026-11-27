/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _DUNE_KVM_INIT_VALUE_H_
#define _DUNE_KVM_INIT_VALUE_H_

#ifdef __ASSEMBLY__
#define _ULCAST_
#define _U64CAST_
#else
#define _ULCAST_ (unsigned long)
#define _U64CAST_ (u64)
#endif

#ifdef __ASSEMBLY__
#define _ATYPE_
#define _ATYPE32_
#define _ATYPE64_
#define _CONST64_(x) x
#else
#define _ATYPE_ __PTRDIFF_TYPE__
#define _ATYPE32_ int
#define _ATYPE64_ __s64
#ifdef CONFIG_64BIT
#define _CONST64_(x) x##L
#else
#define _CONST64_(x) x##LL
#endif
#endif

#define DMW_PABITS 48
#define CSR_DMW1_PLV0 _CONST64_(1 << 0)
#define CSR_DMW1_PLV_ALL _CONST64_(0xf << 0)
#define CSR_DMW1_MAT _CONST64_(1 << 4)
#define CSR_DMW1_VSEG _CONST64_(0x9000)
#define CSR_DMW1_BASE (CSR_DMW1_VSEG << DMW_PABITS)
#define CSR_DMW1_INIT (CSR_DMW1_BASE | CSR_DMW1_MAT | CSR_DMW1_PLV0)
#define CSR_DMW1_INIT_PLV_ALL (CSR_DMW1_BASE | CSR_DMW1_MAT | CSR_DMW1_PLV_ALL)

/* TLB exception allocated KS0 and KS1 statically */
#define TLB_EXC_KS0 LOONGARCH_CSR_KS0
#define TLB_EXC_KS1 LOONGARCH_CSR_KS1
#define TLB_KSCRATCH_MASK (1 << 0 | 1 << 1)

/* KVM allocated KS2 and KS3 statically */
#define KVM_VCPU_KS LOONGARCH_CSR_KS2
#define KVM_TEMP_KS LOONGARCH_CSR_KS3
#define KVM_KSCRATCH_MASK (1 << 2 | 1 << 3)

/* Percpu allocated KS4 */
#define PERCPU_KS4 LOONGARCH_CSR_KS4

#define CSR_TLBRELO_RPLV_SHIFT 63
#define CSR_TLBRELO_RPLV (_ULCAST_(0x1) << CSR_TLBRELO_RPLV_SHIFT)
#define CSR_TLBRELO_XI_SHIFT 62
#define CSR_TLBRELO_XI (_ULCAST_(0x1) << CSR_TLBRELO_XI_SHIFT)
#define CSR_TLBRELO_RI_SHIFT 61
#define CSR_TLBRELO_RI (_ULCAST_(0x1) << CSR_TLBRELO_RI_SHIFT)
#define CSR_TLBRELO_PFN_SHIFT 12
#define CSR_TLBRELO_PFN_WIDTH 36
#define CSR_TLBRELO_PFN (_ULCAST_(0xfffffffff) << CSR_TLBRELO_PFN_SHIFT)
#define CSR_TLBRELO_GLOBAL_SHIFT 6
#define CSR_TLBRELO_GLOBAL (_ULCAST_(0x0) << CSR_TLBRELO_GLOBAL_SHIFT)
#define CSR_TLBELO_GLOBAL  (_ULCAST_(0x0) << CSR_TLBRELO_GLOBAL_SHIFT)
#define CSR_TLBRELO_CCA_SHIFT 4
#define CSR_TLBRELO_CCA_WIDTH 2
#define CSR_TLBRELO_CCA (_ULCAST_(0x1) << CSR_TLBRELO_CCA_SHIFT)
#define CSR_TLBELO_CCA  (_ULCAST_(0x1) << CSR_TLBRELO_CCA_SHIFT)
#define CSR_TLBRELO_PLV_SHIFT 2
#define CSR_TLBRELO_PLV_WIDTH 2
#define CSR_TLBRELO_PLV (_ULCAST_(0x3) << CSR_TLBRELO_PLV_SHIFT)
#define CSR_TLBRELO_WE_SHIFT 1
#define CSR_TLBRELO_WE (_ULCAST_(0x1) << CSR_TLBRELO_WE_SHIFT)
#define CSR_TLBELO_WE  (_ULCAST_(0x1) << CSR_TLBRELO_WE_SHIFT)
#define CSR_TLBRELO_V_SHIFT 0
#define CSR_TLBRELO_V (_ULCAST_(0x1) << CSR_TLBRELO_V_SHIFT)
#define CSR_TLBELO_V  (_ULCAST_(0x1) << CSR_TLBRELO_V_SHIFT)

// 512M 从 INIT_VALUE_PRCFG2 中获取
#define TLB_PS_4K 0xc
#define TLB_PS_1G 0x1e
#define TLB_PS_1G_shift 30
#define TLB_MASK (1 << TLB_PS - 1)

#define TLBRELO0_STANDARD_BITS                                                 \
    (CSR_TLBRELO_V | CSR_TLBRELO_WE | CSR_TLBRELO_CCA | CSR_TLBRELO_GLOBAL)
#define TLBRELO1_STANDARD_BITS (TLBRELO0_STANDARD_BITS | (1 << TLB_PS_1G_shift))

#define TLBRELO0_HAMTDMW_BITS \
    (CSR_TLBRELO_V | CSR_TLBRELO_WE | CSR_TLBRELO_CCA )
#define TLBRELO1_HAMTDMW_BITS (TLBRELO0_HAMTDMW_BITS | (1 << TLB_PS_1G_shift))

#define TLBELO_STANDARD_BITS                                                   \
    (CSR_TLBELO_V | CSR_TLBELO_WE | CSR_TLBELO_CCA | CSR_TLBELO_GLOBAL)

// ring 0, disable interrupt, mapping
#define CRMD_DA 3
#define CRMD_PG 4
#define CRMD_IE 2
#define __INIT_VALUE_CRMD    ((1<<CRMD_PG) | (1<<CRMD_IE) | (1<<5) | (1<<7))
#define __INIT_VALUE_CRMD_DA ((1<<CRMD_DA) | (1<<CRMD_IE) | (1<<5) | (1<<7))

#define INIT_VALUE_EUEN 0xf

#define INIT_VALUE_MISC 0x0

#define ECFGB_SIP0      0
#define ECFGF_SIP0      (_ULCAST_(1) << ECFGB_SIP0)
#define ECFGB_SIP1      1
#define ECFGF_SIP1      (_ULCAST_(1) << ECFGB_SIP1)
#define ECFGB_IP0       2
#define ECFGF_IP0       (_ULCAST_(1) << ECFGB_IP0)
#define ECFGB_IP1       3
#define ECFGF_IP1       (_ULCAST_(1) << ECFGB_IP1)
#define ECFGB_IP2       4
#define ECFGF_IP2       (_ULCAST_(1) << ECFGB_IP2)
#define ECFGB_IP3       5
#define ECFGF_IP3       (_ULCAST_(1) << ECFGB_IP3)
#define ECFGB_IP4       6
#define ECFGF_IP4       (_ULCAST_(1) << ECFGB_IP4)
#define ECFGB_IP5       7
#define ECFGF_IP5       (_ULCAST_(1) << ECFGB_IP5)
#define ECFGB_IP6       8
#define ECFGF_IP6       (_ULCAST_(1) << ECFGB_IP6)
#define ECFGB_IP7       9
#define ECFGF_IP7       (_ULCAST_(1) << ECFGB_IP7)
#define ECFGB_PC        10
#define ECFGF_PC        (_ULCAST_(1) << ECFGB_PC)
#define ECFGB_TIMER     11
#define ECFGF_TIMER     (_ULCAST_(1) << ECFGB_TIMER)
#define ECFGB_IPI       12
#define ECFGF_IPI       (_ULCAST_(1) << ECFGB_IPI)
// VS 指令间距是 2 ** 7,  打开IPI/时钟，屏蔽性能计数器 和 硬中断
#define INIT_VALUE_ECFG (0x70000 | ECFGF_IPI)
// 无需配置缩减虚拟地址
#define INIT_VALUE_RVACFG 0x0
// 读取的 LLBCTL 总是 0, 猜测是因为多数情况下，LLBit 都不会被其他人清零，所以总是 0
#define INIT_VALUE_LLBCTL 0x1

#define INIT_VALUE_STLBPS 0xc

// kvm_vz_vcpu_setup 将 timerid 初始化为 `vcpu->vcpu_id`
// 而 ioctl(vcpu->vm->vm_fd, KVM_CREATE_VCPU, vcpu->cpu_id);
#define INIT_VALUE_TIMERCFG 0x0

// CPUCFG 知道一共存在 4 个，从内核的定义来看，也是如此的
#define INIT_VALUE_PERF0_EVENT 0x0
#define INIT_VALUE_PERF1_EVENT 0x0
#define INIT_VALUE_PERF2_EVENT 0x0
#define INIT_VALUE_PERF3_EVENT 0x0

#define INIT_VALUE_DEBUG 0x0

#endif
