/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include <malloc.h>

#include "qemu/osdep.h"
#include "qemu/compiler.h"
#include "qemu/host-utils.h"
#include "qemu/processor.h"
#include "qemu/typedefs.h"
#include "exec/cpu-defs.h"
#include "cpu.h"
#include "exec/cpu-all.h"
#include "exec/exec-all.h"
#include "qemu/xxhash.h"
#include "exec/ram_addr.h"
#include "exec/memory.h"
#include "tcg/tcg.h"

#include "dune-info.h"

#include "hamt.h"
#include "hamt-internal.h"
#include "hamt-tlb.h"

#include "hamt-ptw.h"
#include "hamt-ptw-internal.h"

#include "loongarch-csr.h"

#include "latx-native-asm.h"
#include "latx-hamt.h"
#include "latx-options.h"
#include "latx-hack.h"
#include "latx/include/error.h"

#ifdef CONFIG_LATX_PRINT
static __thread uint64_t ptw_num;
static __thread uint64_t ptw_ok_num;
static __thread uint64_t ptw_ok_big_page_num;
static __thread uint64_t ptw_xx_num;
static __thread uint64_t ptw_xx_smc_num;
static __thread uint64_t ptw_xx_mmio_num;
static __thread uint64_t ptw_r_num;
static __thread uint64_t ptw_xx_fault_num;
static __thread uint64_t ptw_xx_rsvd_num;
static __thread uint64_t ptw_xx_not_ram_num;
static __thread uint64_t ptw_xx_protect_num;
static __thread uint64_t ptw_xx_smc_r_sp_num;
static __thread uint64_t ptw_xx_smc_w_sp_num;
static __thread uint64_t ptw_xx_smc_r_dp_num;
static __thread uint64_t ptw_xx_smc_w_dp_num;
static __thread uint64_t ptw_xx_vga_ram_num;
static __thread uint64_t ptw_xx_vga_ram_big_page_num;

typedef struct {
    struct {
        uint64_t l;
        uint64_t h;
    } vec[32];
} hamt_temp_data_t;

static __thread hamt_temp_data_t hamt_temp_data[HAMT_CPU_MAX];

static void hamt_temp_data_save(int cpuid)
{
    hamt_temp_data_t *d = &(hamt_temp_data[cpuid]);

    __vst128_native_00((unsigned char *)&(d->vec[0].l));
    __vst128_native_01((unsigned char *)&(d->vec[1].l));
    __vst128_native_02((unsigned char *)&(d->vec[2].l));
    __vst128_native_03((unsigned char *)&(d->vec[3].l));
    __vst128_native_04((unsigned char *)&(d->vec[4].l));
    __vst128_native_05((unsigned char *)&(d->vec[5].l));
    __vst128_native_06((unsigned char *)&(d->vec[6].l));
    __vst128_native_07((unsigned char *)&(d->vec[7].l));
    __vst128_native_08((unsigned char *)&(d->vec[8].l));
    __vst128_native_09((unsigned char *)&(d->vec[9].l));
    __vst128_native_10((unsigned char *)&(d->vec[10].l));
    __vst128_native_11((unsigned char *)&(d->vec[11].l));
    __vst128_native_12((unsigned char *)&(d->vec[12].l));
    __vst128_native_13((unsigned char *)&(d->vec[13].l));
    __vst128_native_14((unsigned char *)&(d->vec[14].l));
    __vst128_native_15((unsigned char *)&(d->vec[15].l));
    __vst128_native_16((unsigned char *)&(d->vec[16].l));
    __vst128_native_17((unsigned char *)&(d->vec[17].l));
    __vst128_native_18((unsigned char *)&(d->vec[18].l));
    __vst128_native_19((unsigned char *)&(d->vec[19].l));
    __vst128_native_20((unsigned char *)&(d->vec[20].l));
    __vst128_native_21((unsigned char *)&(d->vec[21].l));
    __vst128_native_22((unsigned char *)&(d->vec[22].l));
    __vst128_native_23((unsigned char *)&(d->vec[23].l));
    __vst128_native_24((unsigned char *)&(d->vec[24].l));
    __vst128_native_25((unsigned char *)&(d->vec[25].l));
    __vst128_native_26((unsigned char *)&(d->vec[26].l));
    __vst128_native_27((unsigned char *)&(d->vec[27].l));
    __vst128_native_28((unsigned char *)&(d->vec[28].l));
    __vst128_native_29((unsigned char *)&(d->vec[29].l));
    __vst128_native_30((unsigned char *)&(d->vec[30].l));
    __vst128_native_31((unsigned char *)&(d->vec[31].l));
}

static void hamt_temp_data_restore(int cpuid)
{
    hamt_temp_data_t *d = &(hamt_temp_data[cpuid]);

    __vld128_native_00((unsigned char *)&(d->vec[0].l));
    __vld128_native_01((unsigned char *)&(d->vec[1].l));
    __vld128_native_02((unsigned char *)&(d->vec[2].l));
    __vld128_native_03((unsigned char *)&(d->vec[3].l));
    __vld128_native_04((unsigned char *)&(d->vec[4].l));
    __vld128_native_05((unsigned char *)&(d->vec[5].l));
    __vld128_native_06((unsigned char *)&(d->vec[6].l));
    __vld128_native_07((unsigned char *)&(d->vec[7].l));
    __vld128_native_08((unsigned char *)&(d->vec[8].l));
    __vld128_native_09((unsigned char *)&(d->vec[9].l));
    __vld128_native_10((unsigned char *)&(d->vec[10].l));
    __vld128_native_11((unsigned char *)&(d->vec[11].l));
    __vld128_native_12((unsigned char *)&(d->vec[12].l));
    __vld128_native_13((unsigned char *)&(d->vec[13].l));
    __vld128_native_14((unsigned char *)&(d->vec[14].l));
    __vld128_native_15((unsigned char *)&(d->vec[15].l));
    __vld128_native_16((unsigned char *)&(d->vec[16].l));
    __vld128_native_17((unsigned char *)&(d->vec[17].l));
    __vld128_native_18((unsigned char *)&(d->vec[18].l));
    __vld128_native_19((unsigned char *)&(d->vec[19].l));
    __vld128_native_20((unsigned char *)&(d->vec[20].l));
    __vld128_native_21((unsigned char *)&(d->vec[21].l));
    __vld128_native_22((unsigned char *)&(d->vec[22].l));
    __vld128_native_23((unsigned char *)&(d->vec[23].l));
    __vld128_native_24((unsigned char *)&(d->vec[24].l));
    __vld128_native_25((unsigned char *)&(d->vec[25].l));
    __vld128_native_26((unsigned char *)&(d->vec[26].l));
    __vld128_native_27((unsigned char *)&(d->vec[27].l));
    __vld128_native_28((unsigned char *)&(d->vec[28].l));
    __vld128_native_29((unsigned char *)&(d->vec[29].l));
    __vld128_native_30((unsigned char *)&(d->vec[30].l));
    __vld128_native_31((unsigned char *)&(d->vec[31].l));
}

#define print_and_clear_num(cpuid) do { \
    if (hamt_do_print_ptw())    \
    if (!(ptw_num & 0xffff)) { \
    hamt_temp_data_save(cpuid); \
    printf("%s CPU %d %-5ld %-5ld %-5ld ", __func__, \
            cpuid, \
            ptw_num, \
            ptw_ok_num, \
            ptw_xx_num); \
    printf("BP(%-5ld) R(%-5ld) SMC(%-5ld SP %-5ld %-5ld DP %-5ld %-5ld) MMIO(%-5ld) ", \
            ptw_ok_big_page_num, \
            ptw_r_num, \
            ptw_xx_smc_num, \
            ptw_xx_smc_r_sp_num, \
            ptw_xx_smc_w_sp_num, \
            ptw_xx_smc_r_dp_num, \
            ptw_xx_smc_w_dp_num, \
            ptw_xx_mmio_num); \
    printf("X(%-5ld %-5ld %-5ld) ", \
            ptw_xx_fault_num, \
            ptw_xx_protect_num, \
            ptw_xx_rsvd_num); \
    printf("VGA(%-5ld BP-%-5ld)", \
            ptw_xx_vga_ram_num, \
            ptw_xx_vga_ram_big_page_num \
            ); \
    printf(" NotRAM(%-5ld)\n", ptw_xx_not_ram_num); \
    hamt_temp_data_restore(cpuid); \
    ptw_num=0; \
    ptw_ok_num=0; \
    ptw_ok_big_page_num=0; \
    ptw_xx_num=0; \
    ptw_xx_smc_num=0; \
    ptw_xx_smc_r_sp_num=0; \
    ptw_xx_smc_w_sp_num=0; \
    ptw_xx_smc_r_dp_num=0; \
    ptw_xx_smc_w_dp_num=0; \
    ptw_xx_mmio_num=0; \
    ptw_r_num=0; \
    ptw_xx_fault_num=0; \
    ptw_xx_rsvd_num=0; \
    ptw_xx_not_ram_num=0; \
    ptw_xx_protect_num=0; \
    ptw_xx_vga_ram_num=0; \
    ptw_xx_vga_ram_big_page_num=0; \
    } \
} while (0)
#endif

#include "exec/ram_addr.h"

static bool hamt_ptw_set_ad(uint64_t haddr, uint64_t pte, uint32_t set)
{
    if (!(set & ~pte)) {
        return true;
    }
    /* Do not restore an entry another vCPU has unmapped or replaced. */
    return qatomic_cmpxchg((uint64_t *)(uintptr_t)haddr, pte, pte | set) == pte;
}

static bool hamt_do_ptw(CPUX86State *env,
                        uint64_t addr,
                        void *epc,
                        int mmu_index,
                        MMUAccessType access_type,
                        hamt_ptw_error_t *ptwerror)
{
#ifdef CONFIG_LATX_PRINT
    ptw_num += 1;
    ptw_r_num += access_type == MMU_DATA_LOAD ? 1 : 0;
    int is_big_page = 0;
    CPUState *cpu = env_cpu(env);
    int cpuid = cpu->cpu_index;
#endif

    int64_t sext = (int64_t)addr >> 47;
    if (sext != 0 && sext != -1) {
        /* avoid ptw with illegal address */
        ptwerror->error = HAMT_PTW_ERROR_PF;
        return false;
    }

    int is_user = mmu_index == MMU_USER_IDX;
    uint64_t ptep, pte;
    int page_size;
    uint64_t rsvd_mask;
    hwaddr pte_addr, haddr, paddr;

restart:
    rsvd_mask = 0x000fff0000000000LL;
    pte = env->cr[3];
    ptep = PG_USER_MASK | PG_RW_MASK;

    uint64_t rambase =  HACK_RAMBASE;

    /* Page table level 4 */
    pte_addr = ((pte & PG_ADDRESS_MASK) + (((addr >> 39) & 0x1ff) << 3));
    haddr = pte_addr + rambase; /* HVA = GPA + rambase */
    pte = *((uint64_t *)haddr);
    if (!(pte & PG_PRESENT_MASK)) goto do_fault;
    if (pte & (rsvd_mask | PG_PSE_MASK)) goto do_fault_rsvd;
    if (!hamt_ptw_set_ad(haddr, pte, PG_ACCESSED_MASK)) {
        goto restart;
    }
    pte |= PG_ACCESSED_MASK;
    ptep &= pte;

    /* Page table level 3 */
    pte_addr = ((pte & PG_ADDRESS_MASK) + (((addr >> 30) & 0x1ff) << 3));
    haddr = pte_addr + rambase; /* HVA = GPA + rambase */
    pte = *((uint64_t *)haddr);
    if (!(pte & PG_PRESENT_MASK)) goto do_fault;
    if (pte & rsvd_mask) goto do_fault_rsvd;
    if (!hamt_ptw_set_ad(haddr, pte, PG_ACCESSED_MASK)) {
        goto restart;
    }
    pte |= PG_ACCESSED_MASK;
    ptep &= pte;
    if (pte & PG_PSE_MASK) {
        page_size = 1024 * 1024 * 1024; /* 1 GB page */
#ifdef CONFIG_LATX_PRINT
        is_big_page = 3;
#endif
        goto do_check_protect;
    }

    /* Page table level 2 */
    pte_addr = ((pte & PG_ADDRESS_MASK) + (((addr >> 21) & 0x1ff) << 3));
    haddr = pte_addr + rambase; /* HVA = GPA + rambase */
    pte = *((uint64_t *)haddr);
    if (!(pte & PG_PRESENT_MASK)) goto do_fault;
    if (pte & rsvd_mask) goto do_fault_rsvd;
    if (!hamt_ptw_set_ad(haddr, pte, PG_ACCESSED_MASK)) {
        goto restart;
    }
    pte |= PG_ACCESSED_MASK;
    ptep &= pte;
    if (pte & PG_PSE_MASK) {
        page_size = 2048 * 1024; /* 2 MB page */
#ifdef CONFIG_LATX_PRINT
        is_big_page = 2;
#endif
        goto do_check_protect;
    }

    /* Page table level 1 */
    pte_addr = ((pte & PG_ADDRESS_MASK) + (((addr >> 12) & 0x1ff) << 3));
    haddr = pte_addr + rambase; /* HVA = GPA + rambase */
    pte = *((uint64_t *)haddr);
    if (!(pte & PG_PRESENT_MASK)) goto do_fault;
    if (pte & rsvd_mask) goto do_fault_rsvd;
    ptep &= pte;
    page_size = 4096;

do_check_protect:
    rsvd_mask |= (page_size - 1) & PG_ADDRESS_MASK & ~PG_PSE_PAT_MASK;
    if (pte & rsvd_mask) goto do_fault_rsvd;

    /* Guest page permissions are independent of the host PLV encoding. */
    if (is_user && !(ptep & PG_USER_MASK)) {
        goto do_fault_protect;
    }

    int plv = 0;
    if (hamt_enable_plv()) {
        plv = (ptep & PG_USER_MASK) ? 3 : 0;
    }

    int prot = PAGE_READ;
    if ((ptep & PG_RW_MASK)) prot |= PAGE_WRITE;

    if ((prot & (1 << access_type)) == 0) {
        goto do_fault_protect;
    }

    /* yes, it can! */
    {
        uint32_t set = PG_ACCESSED_MASK;
        if (access_type == MMU_DATA_STORE) {
            set |= PG_DIRTY_MASK;
        } else if (!(pte & PG_DIRTY_MASK)) {
            prot &= ~PAGE_WRITE;
        }
        if (!hamt_ptw_set_ad(haddr, pte, set)) {
            goto restart;
        }
    }

    /* align to page_size */
    paddr = (pte & PG_ADDRESS_MASK & ~(page_size - 1)) | (addr & (page_size - 1));
    haddr = paddr + rambase;

    uint8_t reasons;
    uint8_t ptwflags = hamt_ptwflags_get_page_state(paddr, &reasons);

    if (ptwflags & HAMT_PTWFLAGS_NOACCESS_MASK) {
#ifdef CONFIG_LATX_PRINT
        ptw_xx_mmio_num += 1;
#endif
        ptwerror->error = HAMT_PTW_ERROR_MMIO;
        ptwerror->paddr = paddr;
        goto returnfalse;
    }

    if (ptwflags & HAMT_PTWFLAGS_NOWRITE) {
        ptwerror->error = HAMT_PTW_ERROR_NOWRITE;
        if (reasons & PTW_REASON_VGA) {
            ptwerror->error = HAMT_PTW_ERROR_VGA;
        }
        if (reasons & PTW_REASON_SMC) {
            ptwerror->error = HAMT_PTW_ERROR_SMC;
        }
#ifdef CONFIG_LATX_PRINT
        if (ptwerror->error == HAMT_PTW_ERROR_VGA) {
            ptw_xx_vga_ram_num += 1;
            if (is_big_page) {
                ptw_xx_vga_ram_big_page_num += 1;
            }
        } else if (ptwerror->error == HAMT_PTW_ERROR_SMC) {
            ptw_xx_smc_num += 1;
            TranslationBlock *tb = tcg_tb_lookup((uintptr_t)epc);
            if (tb) {
                uint64_t pc_phys_page = tb_page_addr0(tb);
                if ((pc_phys_page >> 12) == (paddr >> 12)) {
                    if (access_type == MMU_DATA_LOAD)  ptw_xx_smc_r_sp_num += 1;
                    if (access_type == MMU_DATA_STORE) ptw_xx_smc_w_sp_num += 1;
                } else {
                    if (access_type == MMU_DATA_LOAD)  ptw_xx_smc_r_dp_num += 1;
                    if (access_type == MMU_DATA_STORE) ptw_xx_smc_w_dp_num += 1;
                }
            }
        }
#endif
        goto returnfalse;
    }

    if (page_size > TARGET_PAGE_SIZE &&
        hamt_ptwflags_large_page_blocked(paddr, page_size)) {
        ptwerror->error = HAMT_PTW_ERROR_MMIO;
        ptwerror->paddr = paddr;
        goto returnfalse;
    }

#ifdef CONFIG_LATX_HACK_MEM
    if (!latx_hack_gpa_in_ram(paddr)) {
#ifdef CONFIG_LATX_PRINT
        ptw_xx_not_ram_num += 1;
#endif
        ptwerror->error = HAMT_PTW_ERROR_NOTRAM;
        goto returnfalse;
    }
#endif

#ifdef CONFIG_LATX_PRINT
    ptw_ok_num += 1;
    if (is_big_page) ptw_ok_big_page_num += 1;
    print_and_clear_num(cpuid);
#endif

    ptwerror->error = HAMT_PTW_ERROR_NONE;
    if (hamt_enable_plv()) HAMT_PROT_ADD_PLV(prot, plv);
    if (!option_pcid_asid && page_size > TARGET_PAGE_SIZE) {
        env->hamt_large_page_bits |= page_size == (1U << HAMT_LARGE_PAGE_SHIFT)
                                     ? hamt_large_page_bit(addr) : UINT32_MAX;
    }
    hamt_set_hardware_tlb_nocheck(env, addr, haddr, prot, 0);
    return true;

do_fault_rsvd:
#ifdef CONFIG_LATX_PRINT
    ptw_xx_rsvd_num += 1;
#endif
    ptwerror->error = HAMT_PTW_ERROR_PF;
    ptwerror->error_code = PG_ERROR_RSVD_MASK;
    goto returnfalse;

do_fault_protect:
#ifdef CONFIG_LATX_PRINT
    ptw_xx_protect_num += 1;
#endif
    ptwerror->error = HAMT_PTW_ERROR_PF;
    ptwerror->error_code = PG_ERROR_P_MASK;
    goto returnfalse;

do_fault:
#ifdef CONFIG_LATX_PRINT
    ptw_xx_fault_num += 1;
#endif
    ptwerror->error = HAMT_PTW_ERROR_PF;
    ptwerror->error_code = 0;

returnfalse:
    if (is_user) {
        ptwerror->error_code |= PG_ERROR_U_MASK;
    }
    switch (access_type) {
    case MMU_DATA_LOAD:
        break;
    case MMU_DATA_STORE:
        ptwerror->error_code |= PG_ERROR_W_MASK;
        break;
    default:
        break;
    }
#ifdef CONFIG_LATX_PRINT
    ptw_xx_num += 1;
    print_and_clear_num(cpuid);
#endif
    return false;
}

bool hamt_try_do_ptw(CPUX86State *env,
                     uint64_t badv,
                     void *epc,
                     int w,
                     hamt_ptw_error_t *ptwerror)
{
    lsassertm(get_pg_mode(env) == 0x1000f,
            "HAMT page mode not 0x1000f : epc %p\n", epc);

    int mmu_index = cpu_mmu_index(env, false);
    if (!hamt_enabled() ||
        !hamt_use_ptw() ||
        get_pg_mode(env) != 0x1000f ||
        (mmu_index != MMU_USER_IDX && mmu_index != MMU_KNOSMAP_IDX))
    {
        ptwerror->error = HAMT_PTW_ERROR_DISABLE;
        return false;
    }

    MMUAccessType access_type = w ? MMU_DATA_STORE : MMU_DATA_LOAD;
    return hamt_do_ptw(env, badv, epc, mmu_index, access_type, ptwerror);
}
