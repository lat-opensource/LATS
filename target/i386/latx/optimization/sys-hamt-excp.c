/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "host/cpuinfo.h"
#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "latx-config.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "latx-perfmap.h"
#include "latx-static-codes.h"
#include "latx-intb-sys.h"
#include "latx-sys-hamt-dmw.h"
#include "latx-sys-hamt.h"
#include "latx-tr.h"
#include "latx-hack.h"
#include "latx-ptw-address.h"
#include "loongarch-extcontext.h"
#include "hamt-data.h"
#include "hamt-internal.h"
#include "hamt-tlb.h"
#include "hamt-options.h"
#include "hamt-entry-func.h"

#ifdef CONFIG_LATX_PRINT
static void __latxs_counter(void *database, int loaddatabase,
        IR2_OPND *t_data, IR2_OPND *t_base, int index)
{
    if (loaddatabase) {
        la_imm(t_base, (uint64_t)database);
    }

    la_out2i(LISA_LD_D,   t_data, t_base, index * 8);
    la_out2i(LISA_ADDI_D, t_data, t_data, 0x1);
    la_out2i(LISA_ST_D,   t_data, t_base, index * 8);
}
#endif

#ifdef LOONGARCH_NEW_WORLD
#define HC_HAMT_SYSCALL_CODE    0x102
#else
#define HC_HAMT_SYSCALL_CODE    0x1
#endif

static void __latxs_gen_hamt_excp_syscall(void)
{
    /* KS5: syscall_parameter */
    la_out2(LISA_CSRRD, &la_t0, &la_csr_ks5);

    la_out2i(LISA_ST_D, &la_a0, &la_t0, 0 * 8);
    la_out2i(LISA_ST_D, &la_a1, &la_t0, 1 * 8);
    la_out2i(LISA_ST_D, &la_a2, &la_t0, 2 * 8);
    la_out2i(LISA_ST_D, &la_a3, &la_t0, 3 * 8);
    la_out2i(LISA_ST_D, &la_a4, &la_t0, 4 * 8);
    la_out2i(LISA_ST_D, &la_a5, &la_t0, 5 * 8);
    la_out2i(LISA_ST_D, &la_a6, &la_t0, 6 * 8);
    la_out2i(LISA_ST_D, &la_a7, &la_t0, 7 * 8);

    IR2_OPND c = la_code(HC_HAMT_SYSCALL_CODE);
    la_out1(LISA_HYPCALL, &c);

    la_out2i(LISA_LD_D, &la_v0, &la_t0, 0);
    la_out2(LISA_CSRRD, &la_t0, &la_csr_era);
    la_out2i(LISA_ADDI_D, &la_t0, &la_t0, 4);
    la_out2(LISA_CSRWR, &la_csr_era, &la_t0);
    /*0000 0110 0100 1000 0011 1000 0000 0000*/
    la_outa(LISA_DUP, 0x06483800); /* ertn */
}

static void __latxs_gen_hamt_fake_fork(void)
{
    IR2_OPND c = la_code(HC_HAMT_SYSCALL_CODE);
    la_out1(LISA_HYPCALL, &c);
    la_out0_(lisa_return);
}

static void __latxs_gen_hamt_interrupt_ipi(hamt_excp_t *d)
{
    lsassertm(hamt_enable_ipi(), "%s IPI not enabled\n", __func__);

    la_out2(LISA_CSRWR, &la_csr_ks3, &la_t0);
    la_out2(LISA_CSRWR, &la_csr_ks4, &la_t1);
    if (hamt_enable_ipi_sync()) {
        la_out2(LISA_CSRWR, &la_csr_ks2, &la_t2);
        la_out2(LISA_CSRWR, &la_csr_ks1, &la_t3);
    }

    /*0000 0110 0100 1001 1KKK KKJJ JJJO OOOO*/
    /* invtlb 0x3 : flush guest all TLB that G=0 */
    la_outa(LISA_DUP, 0x06498000 | 0x3);

    if (hamt_enable_ipi_sync()) {
        /*IOCSRRD.B rd, rj 0000 0110 0100 1000 0000 00JJ JJJD DDDD*/
        /*IOCSRRD.H rd, rj 0000 0110 0100 1000 0000 01JJ JJJD DDDD*/
        /*IOCSRRD.W rd, rj 0000 0110 0100 1000 0000 10JJ JJJD DDDD*/
        /*IOCSRRD.D rd, rj 0000 0110 0100 1000 0000 11JJ JJJD DDDD*/
        /*
         * RJ: iocsr addr = T1 : 13 = 0x1000 = IPI_STATUS
         * RD: val        = T0 : 12 = (1 << source CPU)
         */
        la_out1i(LISA_LU12I_W, &la_t1, 0x1);
        la_outa(LISA_DUP, 0x06480800 | (13<<5) | (12<<0));
        la_out3(LISA_OR, &la_t3, &la_t0, &la_zero);
        // la_t0 = 0b0001 from CPU0
        // la_t0 = 0b0010 from CPU1
        // la_t0 = 0b0100 from CPU2
        // la_t0 = 0b1000 from CPU3
        // la_t0 = 0b1010 from CPU1 and CPU3 !
        IR2_OPND ipiloop = la_new_label();
        la_label(ipiloop);
        // la_t2 = 0 from CPU0
        // la_t2 = 1 from CPU1
        // la_t2 = 2 from CPU2
        // la_t2 = 3 from CPU3
        la_out2(LISA_CTZ_D, &la_t2, &la_t0);
        la_out2i(LISA_ORI,  &la_t1, &la_zero, 0x1);
        la_out3(LISA_SLL_D, &la_t1, &la_t1, &la_t2);
        la_out3(LISA_NOR,   &la_t1, &la_t1, &la_zero);
        // la_t1 = 0b1110 for CPU0
        // la_t1 = 0b1101 for CPU1
        // la_t1 = 0b1011 for CPU2
        // la_t1 = 0b0111 for CPU3
        la_out3(LISA_AND, &la_t0, &la_t0, &la_t1); // clear the bit in ipi status
        // la_t2 = 0x0000_0000 from CPU0
        // la_t2 = 0x0100_0000 from CPU1
        // la_t2 = 0x0200_0000 from CPU2
        // la_t2 = 0x0300_0000 from CPU3
        la_out2i(LISA_SLLI_D, &la_t2, &la_t2, 24);
        // la_t1 = 0x8a_0000_0000
        la_out2_(lisa_mov, &la_t1, &la_zero);
        uint64_t lu32imm = (((uint64_t)d->database2) >> 32) & 0xfffff;
        la_out1i(LISA_LU32I_D, &la_t1, lu32imm);
        la_out2i(LISA_LU52I_D, &la_t1, &la_t1, 0x900);
        // la_t1 = 0x(7f)8a_0000_0000 from CPU0
        // la_t1 = 0x(7f)8a_1000_0000 from CPU1
        // la_t1 = 0x(7f)8a_2000_0000 from CPU2
        // la_t1 = 0x(7f)8a_3000_0000 from CPU3
        la_out3(LISA_OR, &la_t1, &la_t1, &la_t2);

        la_out2 (LISA_CSRRD,  &la_t2, &la_csr_cpuid);
        la_out2i(LISA_SLLI_D, &la_t2, &la_t2, 6);
        la_out3 (LISA_ADD_D,  &la_t1, &la_t1, &la_t2);

        la_out2i(LISA_ADDI_D, &la_t2, &la_zero, -1);
        la_out3(LISA_AMADD_D, &la_zero, &la_t1, &la_t2);

        la_out2(LISA_BNEZ, &la_t0, &ipiloop);

    //    la_out2i(LISA_ADDI_D,  &la_t0, &la_zero, -1); // ffffffff
        la_out1i(LISA_LU12I_W, &la_t1, 0x1);
        la_out2i(LISA_ORI,     &la_t1, &la_t1, 0xc);
        /*0000 0110 0100 1000 0001 00JJ JJJD DDDD IOCSRWR.B */
        /*0000 0110 0100 1000 0001 01JJ JJJD DDDD IOCSRWR.H */
        /*0000 0110 0100 1000 0001 10JJ JJJD DDDD IOCSRWR.W */
        /*0000 0110 0100 1000 0001 11JJ JJJD DDDD IOCSRWR.D */
        /*
         * RJ: iocsr addr = T1 : 13 = 0x100c = IPI_CLEAR
         * RD: val        = T3 : 15 = ~(ipi_status)
         */
        la_outa(LISA_DUP, 0x06481800 | (13<<5) | (15<<0));
        goto hamt_ipi_sync_done;
    }

    la_out2i(LISA_ADDI_D,  &la_t0, &la_zero, -1); // ffffffff
    la_out1i(LISA_LU12I_W, &la_t1, 0x1);
    la_out2i(LISA_ORI,     &la_t1, &la_t1, 0xc);
    /*0000 0110 0100 1000 0001 00JJ JJJD DDDD IOCSRWR.B */
    /*0000 0110 0100 1000 0001 01JJ JJJD DDDD IOCSRWR.H */
    /*0000 0110 0100 1000 0001 10JJ JJJD DDDD IOCSRWR.W */
    /*0000 0110 0100 1000 0001 11JJ JJJD DDDD IOCSRWR.D */
    /*
     * RJ: iocsr addr = T1 : 13 = 0x100c = IPI_CLEAR
     * RD: val        = T0 : 12 = 0xffffffffffffffff
     */
    la_outa(LISA_DUP, 0x06481800 | (13<<5) | (12<<0));

hamt_ipi_sync_done:

#ifdef CONFIG_LATX_PRINT
    la_imm(&la_t0, (uint64_t)d->database);
    la_out2i(LISA_LU52I_D,&la_t0, &la_t0, 0x900);
    la_out2i(LISA_LD_D,   &la_t1, &la_t0, HD_CNT_IPI_INDEX * 8);
    la_out2i(LISA_ADDI_D, &la_t1, &la_t1, 1);
    la_out2i(LISA_ST_D,   &la_t1, &la_t0, HD_CNT_IPI_INDEX * 8);
#endif

    la_out2(LISA_CSRRD, &la_t0, &la_csr_ks3);
    la_out2(LISA_CSRRD, &la_t1, &la_csr_ks4);
    if (hamt_enable_ipi_sync()) {
        la_out2(LISA_CSRRD, &la_t2, &la_csr_ks2);
        la_out2(LISA_CSRRD, &la_t3, &la_csr_ks1);
    }
    /*0000 0110 0100 1000 0011 1000 0000 0000*/
    la_outa(LISA_DUP, 0x06483800); /* ertn */
}

static void __latxs_gen_hamt_excp_tlbrefill_dbt(void)
{
    la_out2(LISA_CSRRD, &la_t0, &la_csr_tlbr_ehi);
    /*srli.d  \ehi,  \ehi, (TLB_PS_1G_shift + 1)*/
    /*slli.d  \ehi,  \ehi, (TLB_PS_1G_shift + 1)*/
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 31);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 31);
    /*ori     \tmp,  zero, TLB_PS_1G*/
    /*add.d   \tmp,  \tmp, \ehi*/
    /*csrwr   \tmp,  LOONGARCH_CSR_TLBREHI*/
    la_out2i(LISA_ORI,  &la_t1, &la_zero, 0x1e);
    la_out3(LISA_ADD_D, &la_t1, &la_t1, &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi, &la_t1);
    /*csrrd   \tmp,  LOONGARCH_CSR_KS6*/
    /*add.d   \tmp,  \tmp, \ehi*/
    /*csrwr   \tmp,  LOONGARCH_CSR_TLBRELO0*/
    la_out2(LISA_CSRRD, &la_t1, &la_csr_ks6);
    la_out3(LISA_ADD_D, &la_t1, &la_t1, &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_t1);
    /*csrrd   \tmp,  LOONGARCH_CSR_KS7*/
    /*add.d   \tmp,  \tmp, \ehi*/
    /*csrwr   \tmp,  LOONGARCH_CSR_TLBRELO1*/
    la_out2(LISA_CSRRD, &la_t1, &la_csr_ks7);
    la_out3(LISA_ADD_D, &la_t1, &la_t1, &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_t1);
    /*tlbfill*/
    /*0000 0110 0100 1000 0011 0100 0000 0000*/
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */
}

static void __latxs_gen_hamt_excp_tlbrefill_hamt_zero(void)
{
    la_out2(LISA_CSRRD, &la_t0, &la_csr_tlbr_ehi);
    /*srli.d  \tmp,  \tmp, 0x6*/
    /*slli.d  \tmp,  \tmp, 0x6*/
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 6);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 6);
    /*ori     \tmp,  \tmp, TLB_PS_4K*/
    la_out2i(LISA_ORI, &la_t0, &la_t0, 0xc);
    /*csrwr   \tmp,  LOONGARCH_CSR_TLBREHI*/
    /*csrwr   zero,  LOONGARCH_CSR_TLBRELO0*/
    /*csrwr   zero,  LOONGARCH_CSR_TLBRELO1*/
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi,  &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_zero);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_zero);
    /*tlbfill*/
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */
}

typedef struct HamtPtwAccessed {
    IR2_OPND update;
    IR2_OPND resume;
} HamtPtwAccessed;

/* Parent A bits travel in the existing U/S and RW permission accumulator. */
static void gen_hamt_parent_accessed(IR2_OPND *slow_path)
{
    la_out2i(LISA_ANDI, &la_t4, &la_t6, PG_ACCESSED_MASK);
    la_out2(LISA_BEQZ, &la_t4, slow_path);
}

/* The common leaf path only tests A; conditional updates are out of line. */
static void gen_hamt_leaf_accessed(HamtPtwAccessed *accessed)
{
    accessed->update = la_new_label();
    accessed->resume = la_new_label();
    la_out2i(LISA_ANDI, &la_t4, &la_t3, PG_ACCESSED_MASK);
    la_out2(LISA_BEQZ, &la_t4, &accessed->update);
    la_label(accessed->resume);
}

static void gen_hamt_leaf_write_permission(void)
{
    la_out2i(LISA_ANDI, &la_t4, &la_t3, PG_DIRTY_MASK);
    la_out3(LISA_MASKEQZ, &la_t6, &la_t6, &la_t4);
}

/*
 * t2 is the leaf PTE GPA, t3 its checked snapshot and t7 the RAM base.
 * t1 and t4 are scratch. Restore t2 and update t3 before resuming. A changed
 * entry or failed SC takes the full slow path instead of spinning here.
 */
static void gen_hamt_accessed_update(HamtPtwAccessed *accessed,
                                     IR2_OPND *slow_path)
{
    la_label(accessed->update);
    /* Leave A/D first-write handling to the access-type-aware C walker. */
    la_out2i(LISA_ANDI, &la_t4, &la_t3, PG_DIRTY_MASK);
    la_out2(LISA_BEQZ, &la_t4, slow_path);
    la_out3(LISA_ADD_D, &la_t2, &la_t2, &la_t7);
    if (cpuinfo & CPUINFO_LAMCAS) {
        la_out2i(LISA_ORI, &la_t1, &la_t3, 0);
        la_out2i(LISA_ORI, &la_t4, &la_t3, PG_ACCESSED_MASK);
        /* IR2 operands are rd, address (rj), new value (rk). */
        la_out3(LISA_AMCAS_DB_D, &la_t1, &la_t2, &la_t4);
        la_out3(LISA_BNE, &la_t1, &la_t3, slow_path);
    } else {
        la_outi(LISA_DBAR, 0);
        la_out2i(LISA_LL_D, &la_t4, &la_t2, 0);
        la_out3(LISA_BNE, &la_t4, &la_t3, slow_path);
        la_out2i(LISA_ORI, &la_t4, &la_t3, PG_ACCESSED_MASK);
        la_out2i(LISA_SC_D, &la_t4, &la_t2, 0);
        la_outi(LISA_DBAR, 0);
        la_out2(LISA_BEQZ, &la_t4, slow_path);
    }
    la_out3(LISA_SUB_D, &la_t2, &la_t2, &la_t7);
    la_out2i(LISA_ORI, &la_t3, &la_t3, PG_ACCESSED_MASK);
    la_out1(LISA_B, &accessed->resume);
}

/* t0 holds the guest VA; t1 and t4 are scratch on the large-page edges. */
static void record_hamt_large_page(bool page_1g)
{
    int offset = offsetof(CPUX86State, hamt_large_page_bits);

    QEMU_BUILD_BUG_ON(offsetof(CPUX86State, hamt_large_page_bits) > 2047);
    if (page_1g) {
        la_out2i(LISA_ADDI_D, &la_t4, &la_zero, -1);
    } else {
        la_out2i(LISA_SRLI_D, &la_t1, &la_t0, HAMT_LARGE_PAGE_SHIFT);
        la_out2i(LISA_ADDI_D, &la_t4, &la_zero, 1);
        la_out3(LISA_SLL_W, &la_t4, &la_t4, &la_t1);
        la_out2i(LISA_LD_WU, &la_t1, &la_env, offset);
        la_out3(LISA_OR, &la_t4, &la_t4, &la_t1);
    }
    la_out2i(LISA_ST_W, &la_t4, &la_env, offset);
}

static void __latxs_gen_hamt_excp_tlbrefill_hamt_ptw2m(hamt_excp_t *d)
{
    HamtPtwAccessed accessed;

    IR2_OPND ptw_no = la_new_label();
    IR2_OPND ptw_ok = la_new_label();

#ifdef CONFIG_LATX_PRINT
    IR2_OPND asdasd1 = la_new_label();
    IR2_OPND asdasd2 = la_new_label();
    la_out2i(LISA_LD_D, &la_t0, &la_env, offsetof(CPUX86State, hflags));
    la_out2i(LISA_ANDI, &la_t0, &la_t0, 0x3);
    la_out2 (LISA_BEQZ, &la_t0, &asdasd1);
    __latxs_counter(d->database, 1, &la_t1, &la_t2, HD_CNT_TLBR_X86_C3_INDEX);
    la_out1 (LISA_B, &asdasd2);
    la_label(asdasd1);
    __latxs_counter(d->database, 1, &la_t1, &la_t2, HD_CNT_TLBR_X86_C0_INDEX);
    la_label(asdasd2);
#endif

#ifdef CONFIG_LATX_DEBUG
    IR2_OPND hflagscheckok = la_new_label();
    la_out2i(LISA_LD_D, &la_t0, &la_env, offsetof(CPUX86State, hflags));
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 30);
    la_out2i(LISA_ANDI, &la_t0, &la_t0, 0x1);
    la_out2 (LISA_BNEZ, &la_t0, &hflagscheckok);
    la_outa (LISA_DUP, 0x0); // hflags not HATM_EN
    la_out1 (LISA_LABEL, &hflagscheckok);
#endif

    la_out2i(LISA_ST_D, &la_t3, &la_env, offsetof(CPUX86State, save_regs[15]));
    la_out2i(LISA_ST_D, &la_t4, &la_env, offsetof(CPUX86State, save_regs[16]));
    la_out2i(LISA_ST_D, &la_t5, &la_env, offsetof(CPUX86State, save_regs[17]));
    la_out2i(LISA_ST_D, &la_t6, &la_env, offsetof(CPUX86State, save_regs[18]));
    la_out2i(LISA_ST_D, &la_t7, &la_env, offsetof(CPUX86State, save_regs[19]));

    la_out2i(LISA_ST_D, &la_a0, &la_env, offsetof(CPUX86State, save_regs[4]));
    la_out2i(LISA_ST_D, &la_a1, &la_env, offsetof(CPUX86State, save_regs[5]));
    la_out2i(LISA_ST_D, &la_a2, &la_env, offsetof(CPUX86State, save_regs[6]));
    la_out2i(LISA_ST_D, &la_a3, &la_env, offsetof(CPUX86State, save_regs[7]));
    la_out2i(LISA_ST_D, &la_a4, &la_env, offsetof(CPUX86State, save_regs[8]));
    la_out2i(LISA_ST_D, &la_a5, &la_env, offsetof(CPUX86State, save_regs[9]));
    /*la_out2i(LISA_ST_D, &la_a6, &la_env, offsetof(CPUX86State, save_regs[10]));*/
    /*la_out2i(LISA_ST_D, &la_a7, &la_env, offsetof(CPUX86State, save_regs[11]));*/

    /*
     *   44   40   36   32   28   24   20   16   12    8    4    0
     * #### #### #### #### #### #### #### #### #### #### #### ####
     * |    L4   |    L3    |    L2    |    L1    |              |
     * 47        39         30         21         12             0
     */

    /*
     * la_t0 : badv (aka. vaddr)
     * la_t1 : cr3
     * la_t2 : pteaddr = L4: cr3 + (vaddr[39:47] << 3)
     *                   L3: pte + (vaddr[30:38] << 3)
     *                   L2: pte + (vaddr[21:29] << 3)
     *                   L1: pte + (vaddr[12:20] << 3)
     * la_t3 : pte = *(pteaddr + rambase)
     * la_t4 : temp
     * la_t5 : page size : 1G=0x40000000=(1<<30) 2M=0x200000=(1<<21) 4K=0x1000=(1<<12)
     * la_t6 : PG_USER_MASK | PG_RW_MASK
     * la_t7 : rambase = 0x90_0000_0000 ULL
     */
    la_out2(LISA_CSRRD, &la_t0, &la_csr_tlbr_badv);
    la_out2i(LISA_LD_D, &la_t1, &la_env, offsetof(CPUX86State, cr[3]));
    la_imm(&la_t7, HACK_RAMBASE);
    la_out2i(LISA_ORI, &la_t6, &la_zero,
             PG_USER_MASK | PG_RW_MASK | PG_ACCESSED_MASK);

    /* ========================== */
    /* === Page Table Level 4 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t1, &la_t2, &la_t0, 39);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & (rsvd_mask | PG_PSE_MASK)) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    la_out2i(LISA_ANDI, &la_t4, &la_t3, PG_PSE_MASK);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);

    IR2_OPND ptw_big_page_bh = la_new_label();

    /* ========================== */
    /* === Page Table Level 3 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t3, &la_t2, &la_t0, 30);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    /* if (pte & PG_PSE_MASK) {*/
    IR2_OPND ptw_l3_no_pse = la_new_label();
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x80);
    la_out2(LISA_BEQZ,  &la_t4, &ptw_l3_no_pse);
    la_out1(LISA_B, &ptw_no); /* 1G leaves use the complete walker. */
    la_label(ptw_l3_no_pse);
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);

    /* ========================== */
    /* === Page Table Level 2 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t3, &la_t2, &la_t0, 21);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    /* if (pte & PG_PSE_MASK) {*/
    IR2_OPND ptw_l2_no_pse = la_new_label();
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x80);
    la_out2(LISA_BEQZ,  &la_t4, &ptw_l2_no_pse);
    record_hamt_large_page(false);
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t4, &la_t5, HD_CNT_TLBR_X86_2M_INDEX);
#endif
    la_out2i(LISA_ORI,    &la_t5, &la_zero, 0x1);
    la_out2i(LISA_SLLI_D, &la_t5, &la_t5, 21); // pagesize = 2M = (1<<21)

    /* ====================================== */
    /* === Page Table Level 2 Big Page 2M === */
    /* ====================================== */

    la_label(ptw_big_page_bh);
    gen_hamt_parent_accessed(&ptw_no);
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);
    /*
     * la_t0 : vaddr
     * la_t2 : pteaddr
     * la_t3 : pte of big page
     * la_t5 : page size = 2M (TODO 1G)
     * la_t6 : PG_USER_MASK | PG_RW_MASK
     * la_t7 : rambase 
     */
    la_out2i(LISA_ADDI_D, &la_t5, &la_t5, -1); // page_size - 1

    /* rsvd_mask |= (page_size - 1) & PG_ADDRESS_MASK & ~PG_PSE_PAT_MASK;*/
    /* if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 13); // bit 12 : PG_PSE_PAT_MASK
    la_out2i (LISA_SLLI_D,     &la_t4, &la_t4, 13); // ptw[13:51]
    la_out3 (LISA_AND,  &la_t4, &la_t4, &la_t5);
    la_out2 (LISA_BNEZ, &la_t4, &ptw_no);

    /*if (is_user && !(ptep & PG_USER_MASK)) goto do_fault_protect;*/
    if (hamt_do_cpl_only(3)) {
        la_out2i(LISA_ANDI, &la_t4, &la_t6, PG_USER_MASK); // 4=USER1 0=USER0
    } else {
        la_out2i(LISA_LD_D,   &la_t1, &la_env, offsetof(CPUX86State, hflags));
        la_out2i(LISA_ADDI_D, &la_t1, &la_t1, 1); // 1=CPL0 4=CPL3
        la_out2i(LISA_ANDI,   &la_t1, &la_t1, 0x3); // 1=CPL0 0=CPL3
        la_out2i(LISA_ANDI,   &la_t4, &la_t6, PG_USER_MASK); // 4=USER1 0=USER0
        la_out3 (LISA_ADD_D,  &la_t4, &la_t4, &la_t1); // CPL3 && USER0
    }
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    la_out2i(LISA_ANDI, &la_t6, &la_t6, PG_RW_MASK);

    gen_hamt_leaf_accessed(&accessed);
    gen_hamt_leaf_write_permission();

    /*
     * la_t0 : vaddr
     * la_t3 : pte for big page
     * la_t5 : page size - 1
     * la_t6 : RW
     * la_t7 : rambase
     */
    la_out2i(LISA_ORI, &la_a0, &la_zero, 0x2); // D(1) in elo0
    la_out2i(LISA_ORI, &la_a1, &la_zero, 0x2); // D(1) in elo1
    la_out3(LISA_MASKEQZ, &la_a0, &la_a0, &la_t6); // if (!rw) D=0
    la_out3(LISA_MASKEQZ, &la_a1, &la_a1, &la_t6); // if (!rw) D=0

    /*
     * la_t0 : vaddr
     * la_t3 : pte for big page
     * la_t5 : page size - 1
     * la_t7 : rambase
     *
     * la_a0 : D(1) for elo0 after RW
     * la_a1 : D(1) for elo1 after RW
     */
    /*paddr = (pte & PG_ADDRESS_MASK & ~(page_size - 1)) | (addr & (page_size - 1));*/
    la_out3  (LISA_NOR,        &la_t4, &la_t5, &la_zero); // ~(page_size - 1)
    la_out2ii(LISA_BSTRPICK_D, &la_t1, &la_t3, 51, 0);
    la_out3  (LISA_AND,        &la_t1, &la_t1, &la_t4); // page 1M for elo0
    la_out2i (LISA_ORI,    &la_t2, &la_zero, 0x1);
    la_out2i (LISA_SLLI_D, &la_t2, &la_t2, 20);
    la_out3  (LISA_OR,     &la_t2, &la_t2, &la_t1); // page 1M for elo1

#ifdef CONFIG_LATX_DEBUG
    /* check GPA address
     * la_t1 : page 1M for elo0 [20]=0 [19:0]=0
     * la_t2 : page 1M for elo1 [20]=1 [19:0]=0
     * GPA should be in 0-9G (0x2_4000_0000) */
    IR2_OPND gpa1mcheckok = la_new_label();
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t1, 35, 28);
    la_out2i (LISA_ORI,   &la_t6, &la_zero, 0x24);
    la_out3  (LISA_SUB_D, &la_t4, &la_t6, &la_t4); // 0x24 - addr[35:28]
    la_out3  (LISA_BGE,   &la_t4, &la_zero, &gpa1mcheckok);
    la_outa  (LISA_DUP, 0x0); // GPA out of range
    la_out1  (LISA_LABEL, &gpa1mcheckok);

    IR2_OPND gpa1mcheckelo0 = la_new_label();
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t1, 20, 20);
    la_out2  (LISA_BEQZ,  &la_t4, &gpa1mcheckelo0);
    la_outa  (LISA_DUP, 0x0); // elo0[20] is not 0
    la_out1  (LISA_LABEL, &gpa1mcheckelo0);

    IR2_OPND gpa1mcheckelo1 = la_new_label();
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t2, 20, 20);
    la_out2  (LISA_BNEZ,  &la_t4, &gpa1mcheckelo1);
    la_outa  (LISA_DUP, 0x0); // elo1[20] is not 1
    la_out1  (LISA_LABEL, &gpa1mcheckelo1);
#endif

    /*
     * vga vram check
     *
     *        base      addr      base + size - 1
     *        |         |         |
     * +------+---------|----------+------------+
     *        |   >= 0  |  >= 2M  |
     *        +---------|---------+
     */
//    la_out2i(LISA_LD_D, &la_a2, &la_env, offsetof(CPUX86State, vram_base));
//    la_out2i(LISA_LD_D, &la_a3, &la_env, offsetof(CPUX86State, vram_size));
//    la_out3 (LISA_ADD_D,  &la_t5, &la_a2, &la_a3);
//    la_out2i(LISA_ADDI_D, &la_t5, &la_t5, -1);     // t5 = addr + size - 1
//    la_out3 (LISA_SUB_D,  &la_t4, &la_t1, &la_a2); // t4 = addr - base
//    la_out3 (LISA_SUB_D,  &la_t5, &la_t5, &la_t1); // t5 = base + size - 1 - addr
//    la_out2i(LISA_ORI,    &la_t6, &la_zero, 0x1);
//    la_out2i(LISA_SLLI_D, &la_t6, &la_t6, 21);
//    la_out3(LISA_SUB_D,   &la_t5, &la_t5, &la_t6); // t5 = t5 - 2M
//    la_out2i(LISA_SRLI_D, &la_t4, &la_t4, 32);
//    la_out2i(LISA_SRLI_D, &la_t5, &la_t5, 32);
//    la_out3 (LISA_OR,   &la_t4, &la_t4, &la_t5);
//    la_out2 (LISA_BNEZ, &la_t4, &ptw_no);

    /*0000000000000000-00000000BFFFFFFF : ok */
    /*00000000C0000000-00000000FFFFFFFF : not ok */
    /*0000000100000000-000000023fffffff : ok */
    la_out2i(LISA_SRLI_D, &la_t4, &la_t1, 28);
    la_out2i(LISA_ADDI_D, &la_t4, &la_t4, -0xc);
    la_out2i(LISA_SRLI_D, &la_t4, &la_t4, 2);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);

    /*
     * la_t0 : vaddr
     * la_t1 : page 1M for elo0 [20]=0 [19:0]=0
     * la_t2 : page 1M for elo1 [20]=1 [19:0]=0
     * la_t3 : pte for big page
     * la_t5 : page size - 1
     * la_t7 : rambase
     *
     * la_a0 : D(1) for elo0 after RW
     * la_a1 : D(1) for elo1 after RW
     */

    IR2_OPND big_page_do_4k = la_new_label();
    /* check ptwdata1m[paddr>>20] */
    la_out2i(LISA_SRLI_D, &la_t4, &la_t1, 19); // (paddr0 >> 20) * 2
    la_out2i(LISA_SRLI_D, &la_t6, &la_t2, 19); // (paddr1 >> 20) * 2
    la_out3 (LISA_ADD_D,  &la_t1, &la_t1, &la_t7);
    la_out3 (LISA_ADD_D,  &la_t2, &la_t2, &la_t7);
    la_out2i(LISA_LD_D,   &la_a3, &la_env, offsetof(CPUX86State, ptw1m));
    la_out3 (LISA_LDX_HU, &la_t4, &la_a3, &la_t4); // ptwdata1m[paddr0>>20]
    la_out3 (LISA_LDX_HU, &la_t6, &la_a3, &la_t6); // ptwdata1m[paddr1>>20]
    la_out3 (LISA_OR,   &la_a3, &la_t4, &la_t6);
    la_out2 (LISA_BNEZ, &la_a3, &big_page_do_4k);
    la_out3(LISA_MASKNEZ, &la_a0, &la_a0, &la_t4); // if (smc) D=0
    la_out3(LISA_MASKNEZ, &la_a1, &la_a1, &la_t6); // if (smc) D=0

    /*
     * la_t0 : vaddr
     * la_t1 : haddr 1M page for elo0
     * la_t2 : haddr 1M page for elo1
     *
     * la_a0 : D(1) for elo0 after RW and SMC
     * la_a1 : D(1) for elo1 after RW and SMC
     */
    la_out2i(LISA_ORI, &la_t1, &la_t1, 0x11); // MAT(5:4)=1 V(0) = 1 for elo0
    la_out2i(LISA_ORI, &la_t2, &la_t2, 0x11); // MAT(5:4)=1 V(0) = 1 for elo1
    la_out3 (LISA_OR,  &la_t1, &la_t1, &la_a0); // D(1) = 1 for elo0
    la_out3 (LISA_OR,  &la_t2, &la_t2, &la_a1); // D(1) = 1 for elo1

    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 21);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 21);
    la_out2i(LISA_ORI,    &la_t0, &la_t0, 0x14); // PS 1M
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi,  &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_t1);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_t2);
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t0, &la_t1, HD_CNT_TLBR_X86_OK1M_INDEX);
#endif
    la_out1(LISA_B, &ptw_ok);












    la_label(big_page_do_4k);
    /*
     * la_t0 : vaddr
     * la_t3 : pte for big page
     * la_t5 : page size - 1
     * la_t7 : rambase
     *
     * la_a0 : D(1) for elo0 after RW
     * la_a1 : D(1) for elo1 after RW
     */
    /*paddr = (pte & PG_ADDRESS_MASK & ~(page_size - 1)) | (addr & (page_size - 1));*/
    la_out3  (LISA_NOR,        &la_t4, &la_t5, &la_zero); // ~(page_size - 1)
    la_out2ii(LISA_BSTRPICK_D, &la_t1, &la_t3, 51, 0);
    la_out3  (LISA_AND,        &la_t1, &la_t1, &la_t4);

    la_out3 (LISA_AND,    &la_t4, &la_t0, &la_t5);
    la_out2i(LISA_SRLI_D, &la_t4, &la_t4, 12);
    la_out2i(LISA_ORI,    &la_t4, &la_t4, 0x1);
    la_out2i(LISA_ADDI_D, &la_t5, &la_t4, -1);
    la_out2i(LISA_SLLI_D, &la_t4, &la_t4, 12);
    la_out2i(LISA_SLLI_D, &la_t5, &la_t5, 12);
    la_out3 (LISA_OR,     &la_t2, &la_t4, &la_t1); // paddr 4K page for elo1
    la_out3 (LISA_OR,     &la_t1, &la_t5, &la_t1); // paddr 4K page for elo0

    /*
     * la_t0 : vaddr
     * la_t1 : page 4K for elo0
     * la_t2 : page 4K for elo1
     * la_t7 : rambase
     *
     * la_a0 : D(1) for elo0 after RW
     * la_a1 : D(1) for elo1 after RW
     */
    /* check ptwdata[paddr>>12] */
    la_out2i(LISA_SRLI_D, &la_t4, &la_t1, 12); // paddr0 >> 12
    la_out2i(LISA_SRLI_D, &la_t6, &la_t2, 12); // paddr1 >> 12
    la_out3 (LISA_ADD_D,  &la_t1, &la_t1, &la_t7);
    la_out3 (LISA_ADD_D,  &la_t2, &la_t2, &la_t7);
    la_out2i(LISA_LD_D,   &la_a3, &la_env, offsetof(CPUX86State, ptw4k));
    la_out3 (LISA_LDX_B,  &la_t4, &la_a3, &la_t4); // ptwdata[paddr0>>12]
    la_out3 (LISA_LDX_B,  &la_t6, &la_a3, &la_t6); // ptwdata[paddr1>>12]
    la_out3(LISA_MASKNEZ, &la_a0, &la_a0, &la_t4); // if (smc) D=0
    la_out3(LISA_MASKNEZ, &la_a1, &la_a1, &la_t6); // if (smc) D=0

    /*
     * la_t0 : vaddr
     * la_t1 : haddr 4K page for elo0
     * la_t2 : haddr 4K page for elo1
     *
     * la_a0 : D(1) for elo0 after RW and SMC
     * la_a1 : D(1) for elo1 after RW and SMC
     */
    la_out2i(LISA_ORI, &la_t1, &la_t1, 0x11); // MAT(5:4)=1 V(0) = 1 for elo0
    la_out2i(LISA_ORI, &la_t2, &la_t2, 0x11); // MAT(5:4)=1 V(0) = 1 for elo1
    la_out3 (LISA_OR,  &la_t1, &la_t1, &la_a0); // D(1) = 1 for elo0
    la_out3 (LISA_OR,  &la_t2, &la_t2, &la_a1); // D(1) = 1 for elo1
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_ORI,    &la_t0, &la_t0, 0xc); // PS 4K
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi,  &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_t1);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_t2);
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t0, &la_t1, HD_CNT_TLBR_X86_OK1M4K_INDEX);
#endif

    la_out1(LISA_B, &ptw_ok);












    la_label(ptw_l2_no_pse);
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);
    gen_hamt_parent_accessed(&ptw_no);
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t4, &la_t5, HD_CNT_TLBR_X86_4K_INDEX);
#endif

    /*
     * la_t0 : vaddr
     * la_t3 : L2 pte
     * la_t6 : (USER|RW) from L4/L3/L2 pte
     * la_t7 : rambase
     */
    /* ========================== */
    /* === Page Table Level 1 === */
    /* ========================== */
    latxs_ptw_pair_address(&la_a4, &la_a5, &la_t3, &la_t1, &la_t2, &la_t0);
    la_out3(LISA_LDX_D, &la_t1, &la_a4, &la_t7); // pte = *(pteaddr + rambase)
    la_out3(LISA_LDX_D, &la_t2, &la_a5, &la_t7); // pte = *(pteaddr + rambase)
    /*
     * la_t0 : vaddr
     * la_t1 : L1 pte elo0
     * la_t2 : L1 pte elo1
     * la_t6 : (USER|RW) from L4/L3/L2 pte
     * la_t7 : rambase
     *
     * to la_a2 : valid for elo0
     * to la_a3 : valid for elo1
     * la_a4 : pteaddr0 for L1 pte elo0
     * la_a5 : pteaddr1 for L1 pte elo1
     */

    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t1, 0x1);
    la_out2i(LISA_ANDI, &la_t5, &la_t2, 0x1);
    la_out3(LISA_MASKEQZ, &la_a2, &la_t4, &la_t4);
    la_out3(LISA_MASKEQZ, &la_a3, &la_t5, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);

    /*if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t1, 51, 40);
    la_out2ii(LISA_BSTRPICK_D, &la_t5, &la_t2, 51, 40);
    la_out3(LISA_MASKNEZ, &la_a2, &la_a2, &la_t4);
    la_out3(LISA_MASKNEZ, &la_a3, &la_a3, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);

    /*ptep &= pte;*/
    la_out3(LISA_AND, &la_a0, &la_t1, &la_t6); // (USER|RW) for elo0
    la_out3(LISA_AND, &la_a1, &la_t2, &la_t6); // (USER|RW) for elo1
    /*
     * la_t0 : vaddr
     * la_t1 : L1 pte elo0
     * la_t2 : L1 pte elo1
     * la_t7 : rambase
     *
     * la_a0 : (USER|RW) from L4/L3/L2/L1 pte for elo0
     * la_a1 : (USER|RW) from L4/L3/L2/L1 pte for elo1
     * la_a2 : valid for elo0 after PRESEND and rsvd
     * la_a3 : valid for elo1 after PRESEND and rsvd
     * la_a4 : pteaddr0 for L1 pte elo0
     * la_a5 : pteaddr1 for L1 pte elo1
     */

    /*if (is_user && !(ptep & PG_USER_MASK)) goto do_fault_protect;*/
    la_out2i(LISA_ANDI, &la_t4, &la_a0, PG_USER_MASK); // 4=USER1 0=USER0
    la_out2i(LISA_ANDI, &la_t5, &la_a1, PG_USER_MASK); // 4=USER1 0=USER0
    if (!hamt_do_cpl_only(3)) {
        la_out2i(LISA_LD_D,   &la_t3, &la_env, offsetof(CPUX86State, hflags));
        la_out2i(LISA_ADDI_D, &la_t3, &la_t3, 1);   // 1=CPL0 4=CPL3
        la_out2i(LISA_ANDI,   &la_t3, &la_t3, 0x3); // 1=CPL0 0=CPL3
        la_out3 (LISA_ADD_D,  &la_t4, &la_t4, &la_t3); // CPL3 && USER0
        la_out3 (LISA_ADD_D,  &la_t5, &la_t5, &la_t3); // CPL3 && USER0
    }
    la_out3(LISA_MASKEQZ, &la_a2, &la_a2, &la_t4);
    la_out3(LISA_MASKEQZ, &la_a3, &la_a3, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);
    la_out2i(LISA_ANDI, &la_a0, &la_a0, PG_RW_MASK);
    la_out2i(LISA_ANDI, &la_a1, &la_a1, PG_RW_MASK);
    /*
     * la_t0 : vaddr
     * la_t1 : L1 pte elo0
     * la_t2 : L1 pte elo1
     * la_t7 : rambase
     *
     * la_a0 : (RW) from L4/L3/L2/L1 pte for elo0
     * la_a1 : (RW) from L4/L3/L2/L1 pte for elo1
     * la_a2 : valid for elo0 after PRESEND, rsvd and USER
     * la_a3 : valid for elo1 after PRESEND, rsvd and USER
     * la_a4 : pteaddr0 for L1 pte elo0
     * la_a5 : pteaddr1 for L1 pte elo1
     */

    // check pte ACCESSED
    la_out2i(LISA_ANDI, &la_t4, &la_t1, 0x20);
    la_out2i(LISA_ANDI, &la_t5, &la_t2, 0x20);
    la_out3(LISA_MASKEQZ, &la_a2, &la_a2, &la_t4);
    la_out3(LISA_MASKEQZ, &la_a3, &la_a3, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);
    // check DIRTY
    la_out2i(LISA_ANDI, &la_t4, &la_t1, 0x40);
    la_out2i(LISA_ANDI, &la_t5, &la_t2, 0x40);
    la_out3(LISA_MASKEQZ, &la_a0, &la_a0, &la_t4);
    la_out3(LISA_MASKEQZ, &la_a1, &la_a1, &la_t5);

    /**((uint32_t*)haddr) = (uint32_t)pte | PG_ACCESSED_MASK;*/
//    IR2_OPND l1_pte0_is_not_valid = la_new_label();
//    IR2_OPND l1_pte1_is_not_valid = la_new_label();
//    la_out2(LISA_BEQZ, &la_a2, &l1_pte0_is_not_valid);
//    la_out2i(LISA_ORI,  &la_t4, &la_t1, 0x20);
//    la_out3(LISA_STX_W, &la_t4, &la_a4, &la_t7);
//    la_label(l1_pte0_is_not_valid);
//    la_out2(LISA_BEQZ, &la_a3, &l1_pte1_is_not_valid);
//    la_out2i(LISA_ORI,  &la_t5, &la_t2, 0x20);
//    la_out3(LISA_STX_W, &la_t5, &la_a5, &la_t7);
//    la_label(l1_pte1_is_not_valid);

    /*
     * la_t0 : vaddr
     * la_t1 : L1 pte elo0
     * la_t2 : L1 pte elo1
     * la_t7 : rambase
     *
     * la_a0 : (RW) from L4/L3/L2/L1 pte for elo0
     * la_a1 : (RW) from L4/L3/L2/L1 pte for elo1
     * la_a2 : valid for elo0 after PRESEND, rsvd and USER
     * la_a3 : valid for elo1 after PRESEND, rsvd and USER
     */

    /*paddr = (pte & PG_ADDRESS_MASK & ~(page_size - 1)) | (addr & (page_size - 1));*/
    la_out2ii(LISA_BSTRPICK_D, &la_t1, &la_t1, 51, 12);
    la_out2ii(LISA_BSTRPICK_D, &la_t2, &la_t2, 51, 12);
    la_out2i(LISA_SLLI_D, &la_t1, &la_t1, 12); // paddr 4K page elo0
    la_out2i(LISA_SLLI_D, &la_t2, &la_t2, 12); // paddr 4K page elo1

#ifdef CONFIG_LATX_DEBUG
    /* check GPA address
     * la_t1 : paddr 4K page for elo0
     * la_t2 : paddr 4K page for elo1
     * GPA should be in 0-9G (0x2_4000_0000) */
    IR2_OPND gpa4k0checkok = la_new_label();
    la_out2  (LISA_BEQZ, &la_a2, &gpa4k0checkok); // no check if not valid
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t1, 35, 28);
    la_out2i (LISA_ORI,   &la_t6, &la_zero, 0x24);
    la_out3  (LISA_SUB_D, &la_t4, &la_t6, &la_t4); // 0x24 - addr[35:28]
    la_out3  (LISA_BGE,   &la_t4, &la_zero, &gpa4k0checkok);
    la_outa  (LISA_DUP, 0x0); // elo0 GPA out of range
    la_out1  (LISA_LABEL, &gpa4k0checkok);

    IR2_OPND gpa4k1checkok = la_new_label();
    la_out2  (LISA_BEQZ, &la_a3, &gpa4k1checkok); // no check if not valid
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t2, 35, 28);
    la_out2i (LISA_ORI,   &la_t6, &la_zero, 0x24);
    la_out3  (LISA_SUB_D, &la_t4, &la_t6, &la_t4); // 0x24 - addr[35:28]
    la_out3  (LISA_BGE,   &la_t4, &la_zero, &gpa4k1checkok);
    la_outa  (LISA_DUP, 0x0); // elo0 GPA out of range
    la_out1  (LISA_LABEL, &gpa4k1checkok);
#endif

    /*
     * la_t0 : vaddr
     * la_t1 : paddr 4K page for elo0
     * la_t2 : paddr 4K page for elo1
     * la_t7 : rambase
     * la_a0 : RW for elo0
     * la_a1 : RW for elo1
     * la_a2 : valid for elo0
     * la_a3 : valid for elo1
     * assert(la_a2 || la_a3) : at least one page is valid
     */
    /* vga vram check */
//    la_out2i(LISA_LD_D,  &la_t3, &la_env, offsetof(CPUX86State, vram_base));
//    la_out3 (LISA_SUB_D, &la_t4, &la_t1, &la_t3);
//    la_out3 (LISA_SUB_D, &la_t5, &la_t2, &la_t3);
//    la_out2i(LISA_LD_D,   &la_t6, &la_env, offsetof(CPUX86State, vram_size));
//    la_out3 (LISA_ADD_D,  &la_t3, &la_t3, &la_t6);
//    la_out2i(LISA_ADDI_D, &la_t3, &la_t3, -1);
//    la_out3 (LISA_SUB_D,  &la_a4, &la_t3, &la_t1);
//    la_out3 (LISA_SUB_D,  &la_a5, &la_t3, &la_t2);
//    la_out2i(LISA_SRLI_D, &la_t4, &la_t4, 32);
//    la_out2i(LISA_SRLI_D, &la_t5, &la_t5, 32);
//    la_out2i(LISA_SRLI_D, &la_a4, &la_a4, 32);
//    la_out2i(LISA_SRLI_D, &la_a5, &la_a5, 32);
//    la_out3 (LISA_OR, &la_t4, &la_t4, &la_a4);
//    la_out3 (LISA_OR, &la_t5, &la_t5, &la_a5);
//    la_out3 (LISA_MASKNEZ, &la_a2, &la_a2, &la_t4);
//    la_out3 (LISA_MASKNEZ, &la_a3, &la_a3, &la_t5);
//    la_out3 (LISA_OR,   &la_t4, &la_a2, &la_a3);
//    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*0000000000000000-00000000BFFFFFFF : ok */
    /*00000000C0000000-00000000FFFFFFFF : not ok */
    /*0000000100000000-000000023fffffff : ok */
    la_out2i(LISA_SRLI_D, &la_t4, &la_t1, 28);
    la_out2i(LISA_SRLI_D, &la_t5, &la_t2, 28);
    la_out2i(LISA_ADDI_D, &la_t4, &la_t4, -0xc);
    la_out2i(LISA_ADDI_D, &la_t5, &la_t5, -0xc);
    la_out2i(LISA_SRLI_D, &la_t4, &la_t4, 2);
    la_out2i(LISA_SRLI_D, &la_t5, &la_t5, 2);
    la_out3 (LISA_MASKEQZ, &la_a2, &la_a2, &la_t4);
    la_out3 (LISA_MASKEQZ, &la_a3, &la_a3, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);

    la_out2i(LISA_SRLI_D, &la_t4, &la_t1, 12);     // paddr0 >> 12
    la_out2i(LISA_SRLI_D, &la_t5, &la_t2, 12);     // paddr1 >> 12
    la_out3 (LISA_ADD_D,  &la_t1, &la_t1, &la_t7);
    la_out3 (LISA_ADD_D,  &la_t2, &la_t2, &la_t7);
    la_out2i(LISA_LD_D,   &la_t3, &la_env, offsetof(CPUX86State, ptw4k));
    la_out3 (LISA_MASKEQZ, &la_t4, &la_t4, &la_a2);
    la_out3 (LISA_MASKEQZ, &la_t5, &la_t5, &la_a3);
    la_out3 (LISA_LDX_B,  &la_t4, &la_t3, &la_t4); // ptwdata[paddr0]
    la_out3 (LISA_LDX_B,  &la_t5, &la_t3, &la_t5); // ptwdata[paddr1]
    /*
     * la_t0 : vaddr
     * la_t1 : haddr 4K page for elo0
     * la_t2 : haddr 4K page for elo1
     * la_t4 : ptwdata[paddr0>>12] for elo0
     * la_t5 : ptwdata[paddr1>>12] for elo1
     * la_a0 : RW for elo0
     * la_a1 : RW for elo1
     * la_a2 : valid for elo0
     * la_a3 : valid for elo1
     */
    la_out2i(LISA_ORI, &la_t6, &la_zero, 0x2); // D(1) in elo0
    la_out2i(LISA_ORI, &la_t7, &la_zero, 0x2); // D(1) in elo1
    la_out3(LISA_MASKEQZ, &la_t6, &la_t6, &la_a0); // if (!rw) D=0
    la_out3(LISA_MASKEQZ, &la_t7, &la_t7, &la_a1);
    la_out3(LISA_MASKNEZ, &la_t6, &la_t6, &la_t4); // if (!ptwdata) D=0
    la_out3(LISA_MASKNEZ, &la_t7, &la_t7, &la_t5);
    /*
     * la_t0 : vaddr
     * la_t1 : haddr 4K page for elo0
     * la_t2 : haddr 4K page for elo1
     * la_t4 : ptwdata[paddr0>>12] for elo0
     * la_t5 : ptwdata[paddr1>>12] for elo1
     * la_t6 : D(1) for elo0
     * la_t7 : D(1) for elo1
     * la_a2 : valid for elo0
     * la_a3 : valid for elo1
     */
    la_out2i(LISA_ORI, &la_t3, &la_zero, 0x1); // v(0) in elo
    la_out2i(LISA_ANDI, &la_t4, &la_t4, 0x1);
    la_out2i(LISA_ANDI, &la_t5, &la_t5, 0x1);
    la_out3(LISA_MASKNEZ, &la_t4, &la_t3, &la_t4); // elo0 valid after mmio
    la_out3(LISA_MASKNEZ, &la_t5, &la_t3, &la_t5); // elo1 valid after mmio
    la_out3(LISA_MASKEQZ, &la_t4, &la_t4, &la_a2); // elo0 valid after PTW
    la_out3(LISA_MASKEQZ, &la_t5, &la_t5, &la_a3); // elo1 valid after PTW
    la_out3(LISA_OR,   &la_a2, &la_t4, &la_t5);
    la_out2(LISA_BEQZ, &la_a2, &ptw_no);

    /*
     * la_t0 : vaddr
     * la_t1 : haddr 4K page for 0
     * la_t2 : haddr 4K page for 1
     * la_t4 : elo0 valid
     * la_t5 : elo1 valid
     * la_t6 : D(1) for 0
     * la_t7 : D(1) for 1
     */
    la_out3 (LISA_OR,  &la_t1, &la_t1, &la_t4); // V(0) in elo0
    la_out3 (LISA_OR,  &la_t2, &la_t2, &la_t5); // V(0) in elo1
    la_out2i(LISA_ORI, &la_t1, &la_t1, 0x10); // MAT(5:4)=1, PLV(3:2)=0, D(1)=0
    la_out2i(LISA_ORI, &la_t2, &la_t2, 0x10); // MAT(5:4)=1, PLV(3:2)=0, D(1)=0
    la_out3 (LISA_OR,  &la_t1, &la_t1, &la_t6); // D(1) in elo0
    la_out3 (LISA_OR,  &la_t2, &la_t2, &la_t7); // D(1) in elo1

    /*
     * register status at here
     * la_t0 : vaddr
     * la_t1 : elo0
     * la_t2 : elo1
     */
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_ORI,    &la_t0, &la_t0, 0xc); // PS 4K
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi,  &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_t1);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_t2);
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t0, &la_t1, HD_CNT_TLBR_X86_OK_INDEX);
#endif

    la_out1(LISA_B, &ptw_ok);
    la_label(ptw_no);
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_ORI,    &la_t0, &la_t0, 0xc); // PS 4K
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi,  &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_zero);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_zero);
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t0, &la_t1, HD_CNT_TLBR_X86_XX_INDEX);
#endif
    la_label(ptw_ok);

    la_out2i(LISA_LD_D, &la_t3, &la_env, offsetof(CPUX86State, save_regs[15]));
    la_out2i(LISA_LD_D, &la_t4, &la_env, offsetof(CPUX86State, save_regs[16]));
    la_out2i(LISA_LD_D, &la_t5, &la_env, offsetof(CPUX86State, save_regs[17]));
    la_out2i(LISA_LD_D, &la_t6, &la_env, offsetof(CPUX86State, save_regs[18]));
    la_out2i(LISA_LD_D, &la_t7, &la_env, offsetof(CPUX86State, save_regs[19]));

    la_out2i(LISA_LD_D, &la_a0, &la_env, offsetof(CPUX86State, save_regs[4]));
    la_out2i(LISA_LD_D, &la_a1, &la_env, offsetof(CPUX86State, save_regs[5]));
    la_out2i(LISA_LD_D, &la_a2, &la_env, offsetof(CPUX86State, save_regs[6]));
    la_out2i(LISA_LD_D, &la_a3, &la_env, offsetof(CPUX86State, save_regs[7]));
    la_out2i(LISA_LD_D, &la_a4, &la_env, offsetof(CPUX86State, save_regs[8]));
    la_out2i(LISA_LD_D, &la_a5, &la_env, offsetof(CPUX86State, save_regs[9]));
    /*la_out2i(LISA_LD_D, &la_a6, &la_env, offsetof(CPUX86State, save_regs[10]));*/
    /*la_out2i(LISA_LD_D, &la_a7, &la_env, offsetof(CPUX86State, save_regs[11]));*/

    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);
    gen_hamt_accessed_update(&accessed, &ptw_no);
}

static void __latxs_gen_hamt_excp_tlbrefill_hamt_ptw2(hamt_excp_t *d)
{
    HamtPtwAccessed accessed;

    IR2_OPND ptw_no = la_new_label();
    IR2_OPND ptw_ok = la_new_label();

#ifdef CONFIG_LATX_PRINT
    IR2_OPND asdasd1 = la_new_label();
    IR2_OPND asdasd2 = la_new_label();
    la_out2i(LISA_LD_D, &la_t0, &la_env, offsetof(CPUX86State, hflags));
    la_out2i(LISA_ANDI, &la_t0, &la_t0, 0x3);
    la_out2 (LISA_BEQZ, &la_t0, &asdasd1);
    __latxs_counter(d->database, 1, &la_t1, &la_t2, HD_CNT_TLBR_X86_C3_INDEX);
    la_out1 (LISA_B, &asdasd2);
    la_label(asdasd1);
    __latxs_counter(d->database, 1, &la_t1, &la_t2, HD_CNT_TLBR_X86_C0_INDEX);
    la_label(asdasd2);
#endif

#ifdef CONFIG_LATX_DEBUG
    IR2_OPND hflagscheckok = la_new_label();
    la_out2i(LISA_LD_D, &la_t0, &la_env, offsetof(CPUX86State, hflags));
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 30);
    la_out2i(LISA_ANDI, &la_t0, &la_t0, 0x1);
    la_out2 (LISA_BNEZ, &la_t0, &hflagscheckok);
    la_outa (LISA_DUP, 0x0); // hflags not HATM_EN
    la_out1 (LISA_LABEL, &hflagscheckok);
#endif

    la_out2i(LISA_ST_D, &la_t3, &la_env, offsetof(CPUX86State, save_regs[15]));
    la_out2i(LISA_ST_D, &la_t4, &la_env, offsetof(CPUX86State, save_regs[16]));
    la_out2i(LISA_ST_D, &la_t5, &la_env, offsetof(CPUX86State, save_regs[17]));
    la_out2i(LISA_ST_D, &la_t6, &la_env, offsetof(CPUX86State, save_regs[18]));
    la_out2i(LISA_ST_D, &la_t7, &la_env, offsetof(CPUX86State, save_regs[19]));

    la_out2i(LISA_ST_D, &la_a0, &la_env, offsetof(CPUX86State, save_regs[4]));
    la_out2i(LISA_ST_D, &la_a1, &la_env, offsetof(CPUX86State, save_regs[5]));
    la_out2i(LISA_ST_D, &la_a2, &la_env, offsetof(CPUX86State, save_regs[6]));
    la_out2i(LISA_ST_D, &la_a3, &la_env, offsetof(CPUX86State, save_regs[7]));
    la_out2i(LISA_ST_D, &la_a4, &la_env, offsetof(CPUX86State, save_regs[8]));
    la_out2i(LISA_ST_D, &la_a5, &la_env, offsetof(CPUX86State, save_regs[9]));
    /*la_out2i(LISA_ST_D, &la_a6, &la_env, offsetof(CPUX86State, save_regs[10]));*/
    /*la_out2i(LISA_ST_D, &la_a7, &la_env, offsetof(CPUX86State, save_regs[11]));*/

    /*
     *   44   40   36   32   28   24   20   16   12    8    4    0
     * #### #### #### #### #### #### #### #### #### #### #### ####
     * |    L4   |    L3    |    L2    |    L1    |              |
     * 47        39         30         21         12             0
     */

    /*
     * la_t0 : badv (aka. vaddr)
     * la_t1 : cr3
     * la_t2 : pteaddr = L4: cr3 + (vaddr[39:47] << 3)
     *                   L3: pte + (vaddr[30:38] << 3)
     *                   L2: pte + (vaddr[21:29] << 3)
     *                   L1: pte + (vaddr[12:20] << 3)
     * la_t3 : pte = *(pteaddr + rambase)
     * la_t4 : temp
     * la_t5 : page size : 1G=0x40000000=(1<<30) 2M=0x200000=(1<<21) 4K=0x1000=(1<<12)
     * la_t6 : PG_USER_MASK | PG_RW_MASK
     * la_t7 : rambase = 0x90_0000_0000 ULL
     */
    la_out2(LISA_CSRRD, &la_t0, &la_csr_tlbr_badv);
    la_out2i(LISA_LD_D, &la_t1, &la_env, offsetof(CPUX86State, cr[3]));
    la_imm(&la_t7, HACK_RAMBASE);
    la_out2i(LISA_ORI, &la_t6, &la_zero,
             PG_USER_MASK | PG_RW_MASK | PG_ACCESSED_MASK);

    /* ========================== */
    /* === Page Table Level 4 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t1, &la_t2, &la_t0, 39);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & (rsvd_mask | PG_PSE_MASK)) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    la_out2i(LISA_ANDI, &la_t4, &la_t3, PG_PSE_MASK);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);

    IR2_OPND ptw_big_page_bh = la_new_label();

    /* ========================== */
    /* === Page Table Level 3 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t3, &la_t2, &la_t0, 30);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    /* if (pte & PG_PSE_MASK) {*/
    IR2_OPND ptw_l3_no_pse = la_new_label();
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x80);
    la_out2(LISA_BEQZ,  &la_t4, &ptw_l3_no_pse);
    record_hamt_large_page(true);
    la_out2i(LISA_ORI,    &la_t5, &la_zero, 0x1);
    la_out2i(LISA_SLLI_D, &la_t5, &la_t5, 30); // pagesize = 1G = (1<<30)
    la_out1 (LISA_B, &ptw_big_page_bh);
    la_label(ptw_l3_no_pse);
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);

    /* ========================== */
    /* === Page Table Level 2 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t3, &la_t2, &la_t0, 21);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    /* if (pte & PG_PSE_MASK) {*/
    IR2_OPND ptw_l2_no_pse = la_new_label();
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x80);
    la_out2(LISA_BEQZ,  &la_t4, &ptw_l2_no_pse);
    record_hamt_large_page(false);
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t4, &la_t5, HD_CNT_TLBR_X86_2M_INDEX);
#endif
    la_out2i(LISA_ORI,    &la_t5, &la_zero, 0x1);
    la_out2i(LISA_SLLI_D, &la_t5, &la_t5, 21); // pagesize = 2M = (1<<21)
    la_out1 (LISA_B, &ptw_big_page_bh);
    la_label(ptw_l2_no_pse);
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);
    gen_hamt_parent_accessed(&ptw_no);
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t4, &la_t5, HD_CNT_TLBR_X86_4K_INDEX);
#endif











    /*
     * la_t0 : vaddr
     * la_t3 : L2 pte
     * la_t6 : (USER|RW) from L4/L3/L2 pte
     * la_t7 : rambase
     */
    /* ========================== */
    /* === Page Table Level 1 === */
    /* ========================== */
    latxs_ptw_pair_address(&la_a4, &la_a5, &la_t3, &la_t1, &la_t2, &la_t0);
    la_out3(LISA_LDX_D, &la_t1, &la_a4, &la_t7); // pte = *(pteaddr + rambase)
    la_out3(LISA_LDX_D, &la_t2, &la_a5, &la_t7); // pte = *(pteaddr + rambase)
    /*
     * la_t0 : vaddr
     * la_t1 : L1 pte elo0
     * la_t2 : L1 pte elo1
     * la_t6 : (USER|RW) from L4/L3/L2 pte
     * la_t7 : rambase
     *
     * to la_a2 : valid for elo0
     * to la_a3 : valid for elo1
     * la_a4 : pteaddr0 for L1 pte elo0
     * la_a5 : pteaddr1 for L1 pte elo1
     */

    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t1, 0x1);
    la_out2i(LISA_ANDI, &la_t5, &la_t2, 0x1);
    la_out3(LISA_MASKEQZ, &la_a2, &la_t4, &la_t4);
    la_out3(LISA_MASKEQZ, &la_a3, &la_t5, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);

    /*if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t1, 51, 40);
    la_out2ii(LISA_BSTRPICK_D, &la_t5, &la_t2, 51, 40);
    la_out3(LISA_MASKNEZ, &la_a2, &la_a2, &la_t4);
    la_out3(LISA_MASKNEZ, &la_a3, &la_a3, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);

    /*ptep &= pte;*/
    la_out3(LISA_AND, &la_a0, &la_t1, &la_t6); // (USER|RW) for elo0
    la_out3(LISA_AND, &la_a1, &la_t2, &la_t6); // (USER|RW) for elo1
    /*
     * la_t0 : vaddr
     * la_t1 : L1 pte elo0
     * la_t2 : L1 pte elo1
     * la_t7 : rambase
     *
     * la_a0 : (USER|RW) from L4/L3/L2/L1 pte for elo0
     * la_a1 : (USER|RW) from L4/L3/L2/L1 pte for elo1
     * la_a2 : valid for elo0 after PRESEND and rsvd
     * la_a3 : valid for elo1 after PRESEND and rsvd
     * la_a4 : pteaddr0 for L1 pte elo0
     * la_a5 : pteaddr1 for L1 pte elo1
     */

    /*if (is_user && !(ptep & PG_USER_MASK)) goto do_fault_protect;*/
    la_out2i(LISA_ANDI, &la_t4, &la_a0, PG_USER_MASK); // 4=USER1 0=USER0
    la_out2i(LISA_ANDI, &la_t5, &la_a1, PG_USER_MASK); // 4=USER1 0=USER0
    if (!hamt_do_cpl_only(3)) {
        la_out2i(LISA_LD_D,   &la_t3, &la_env, offsetof(CPUX86State, hflags));
        la_out2i(LISA_ADDI_D, &la_t3, &la_t3, 1);   // 1=CPL0 4=CPL3
        la_out2i(LISA_ANDI,   &la_t3, &la_t3, 0x3); // 1=CPL0 0=CPL3
        la_out3 (LISA_ADD_D,  &la_t4, &la_t4, &la_t3); // CPL3 && USER0
        la_out3 (LISA_ADD_D,  &la_t5, &la_t5, &la_t3); // CPL3 && USER0
    }
    la_out3(LISA_MASKEQZ, &la_a2, &la_a2, &la_t4);
    la_out3(LISA_MASKEQZ, &la_a3, &la_a3, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);
    la_out2i(LISA_ANDI, &la_a0, &la_a0, PG_RW_MASK);
    la_out2i(LISA_ANDI, &la_a1, &la_a1, PG_RW_MASK);
    /*
     * la_t0 : vaddr
     * la_t1 : L1 pte elo0
     * la_t2 : L1 pte elo1
     * la_t7 : rambase
     *
     * la_a0 : (RW) from L4/L3/L2/L1 pte for elo0
     * la_a1 : (RW) from L4/L3/L2/L1 pte for elo1
     * la_a2 : valid for elo0 after PRESEND, rsvd and USER
     * la_a3 : valid for elo1 after PRESEND, rsvd and USER
     * la_a4 : pteaddr0 for L1 pte elo0
     * la_a5 : pteaddr1 for L1 pte elo1
     */

    // check pte ACCESSED
    la_out2i(LISA_ANDI, &la_t4, &la_t1, 0x20);
    la_out2i(LISA_ANDI, &la_t5, &la_t2, 0x20);
    la_out3(LISA_MASKEQZ, &la_a2, &la_a2, &la_t4);
    la_out3(LISA_MASKEQZ, &la_a3, &la_a3, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);
    // check DIRTY
    la_out2i(LISA_ANDI, &la_t4, &la_t1, 0x40);
    la_out2i(LISA_ANDI, &la_t5, &la_t2, 0x40);
    la_out3(LISA_MASKEQZ, &la_a0, &la_a0, &la_t4);
    la_out3(LISA_MASKEQZ, &la_a1, &la_a1, &la_t5);

    /**((uint32_t*)haddr) = (uint32_t)pte | PG_ACCESSED_MASK;*/
//    IR2_OPND l1_pte0_is_not_valid = la_new_label();
//    IR2_OPND l1_pte1_is_not_valid = la_new_label();
//    la_out2(LISA_BEQZ, &la_a2, &l1_pte0_is_not_valid);
//    la_out2i(LISA_ORI,  &la_t4, &la_t1, 0x20);
//    la_out3(LISA_STX_W, &la_t4, &la_a4, &la_t7);
//    la_label(l1_pte0_is_not_valid);
//    la_out2(LISA_BEQZ, &la_a3, &l1_pte1_is_not_valid);
//    la_out2i(LISA_ORI,  &la_t5, &la_t2, 0x20);
//    la_out3(LISA_STX_W, &la_t5, &la_a5, &la_t7);
//    la_label(l1_pte1_is_not_valid);

    /*
     * la_t0 : vaddr
     * la_t1 : L1 pte elo0
     * la_t2 : L1 pte elo1
     * la_t7 : rambase
     *
     * la_a0 : (RW) from L4/L3/L2/L1 pte for elo0
     * la_a1 : (RW) from L4/L3/L2/L1 pte for elo1
     * la_a2 : valid for elo0 after PRESEND, rsvd and USER
     * la_a3 : valid for elo1 after PRESEND, rsvd and USER
     */

    /*paddr = (pte & PG_ADDRESS_MASK & ~(page_size - 1)) | (addr & (page_size - 1));*/
    la_out2ii(LISA_BSTRPICK_D, &la_t1, &la_t1, 51, 12);
    la_out2ii(LISA_BSTRPICK_D, &la_t2, &la_t2, 51, 12);
    la_out2i(LISA_SLLI_D, &la_t1, &la_t1, 12); // paddr 4K page elo0
    la_out2i(LISA_SLLI_D, &la_t2, &la_t2, 12); // paddr 4K page elo1

    IR2_OPND ptw_to_check_4K = la_new_label();
    /*
     * la_t0 : vaddr
     * la_t1 : paddr 4K page for elo0
     * la_t2 : paddr 4K page for elo1
     * la_t7 : rambase
     * la_a0 : RW for elo0
     * la_a1 : RW for elo1
     * la_a2 : valid for elo0 after PRESEND, rsvd and USER
     * la_a3 : valid for elo1 after PRESEND, rsvd and USER
     */
    la_out1(LISA_B, &ptw_to_check_4K);










    /*
     * la_t0 : vaddr
     * la_t2 : pteaddr
     * la_t3 : pte of big page
     * la_t5 : page size : 1G or 2M
     * la_t6 : PG_USER_MASK | PG_RW_MASK
     * la_t7 : rambase 
     */
    la_label(ptw_big_page_bh);
    gen_hamt_parent_accessed(&ptw_no);
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);
    la_out2i(LISA_ADDI_D, &la_t5, &la_t5, -1); // page_size - 1

    /* rsvd_mask |= (page_size - 1) & PG_ADDRESS_MASK & ~PG_PSE_PAT_MASK;*/
    /* if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 13); // bit 12 : PG_PSE_PAT_MASK
    la_out2i (LISA_SLLI_D,     &la_t4, &la_t4, 13); // ptw[13:51]
    la_out3 (LISA_AND,  &la_t4, &la_t4, &la_t5);
    la_out2 (LISA_BNEZ, &la_t4, &ptw_no);

    /*if (is_user && !(ptep & PG_USER_MASK)) goto do_fault_protect;*/
    if (hamt_do_cpl_only(3)) {
        la_out2i(LISA_ANDI, &la_t4, &la_t6, PG_USER_MASK); // 4=USER1 0=USER0
    } else {
        la_out2i(LISA_LD_D,   &la_t1, &la_env, offsetof(CPUX86State, hflags));
        la_out2i(LISA_ADDI_D, &la_t1, &la_t1, 1); // 1=CPL0 4=CPL3
        la_out2i(LISA_ANDI,   &la_t1, &la_t1, 0x3); // 1=CPL0 0=CPL3
        la_out2i(LISA_ANDI,   &la_t4, &la_t6, PG_USER_MASK); // 4=USER1 0=USER0
        la_out3 (LISA_ADD_D,  &la_t4, &la_t4, &la_t1); // CPL3 && USER0
    }
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    la_out2i(LISA_ANDI, &la_t6, &la_t6, PG_RW_MASK);

    gen_hamt_leaf_accessed(&accessed);
    gen_hamt_leaf_write_permission();

    /*
     * la_t0 : vaddr
     * la_t3 : pte for big page
     * la_t5 : page size - 1
     * la_t6 : RW
     * la_t7 : rambase
     */
    /*paddr = (pte & PG_ADDRESS_MASK & ~(page_size - 1)) | (addr & (page_size - 1));*/
    la_out3 (LISA_NOR, &la_t4, &la_t5, &la_zero); // ~(page_size - 1)
    la_out2ii(LISA_BSTRPICK_D, &la_t1, &la_t3, 51, 0);
    la_out3  (LISA_AND,        &la_t1, &la_t1, &la_t4);

    la_out3 (LISA_AND,    &la_t4, &la_t0, &la_t5);
    la_out2i(LISA_SRLI_D, &la_t4, &la_t4, 12);
    la_out2i(LISA_ORI,    &la_t4, &la_t4, 0x1);
    la_out2i(LISA_SLLI_D, &la_t2, &la_t4, 12);
    la_out3 (LISA_OR,     &la_t2, &la_t2, &la_t1); // paddr 4K page for elo1
    la_out2i(LISA_ADDI_D, &la_t4, &la_t4, -1);
    la_out2i(LISA_SLLI_D, &la_t4, &la_t4, 12);
    la_out3 (LISA_OR,     &la_t1, &la_t4, &la_t1); // paddr 4K page for elo0

    la_out2_(lisa_mov, &la_a0, &la_t6); // RW for elo0
    la_out2_(lisa_mov, &la_a1, &la_t6); // RW for elo1
    
    la_out2i(LISA_ORI, &la_a2, &la_zero, 0x1); // valid for elo0
    la_out2i(LISA_ORI, &la_a3, &la_zero, 0x1); // valid for elo1










    la_label(ptw_to_check_4K);
    /*
     * la_t0 : vaddr
     * la_t1 : paddr 4K page for elo0
     * la_t2 : paddr 4K page for elo1
     * la_t7 : rambase
     * la_a0 : RW for elo0
     * la_a1 : RW for elo1
     * la_a2 : valid for elo0
     * la_a3 : valid for elo1
     * assert(la_a2 || la_a3) : at least one page is valid
     */
    /* vga vram check */
//    la_out2i(LISA_LD_D,  &la_t3, &la_env, offsetof(CPUX86State, vram_base));
//    la_out3 (LISA_SUB_D, &la_t4, &la_t1, &la_t3);
//    la_out3 (LISA_SUB_D, &la_t5, &la_t2, &la_t3);
//    la_out2i(LISA_LD_D,  &la_t6, &la_env, offsetof(CPUX86State, vram_size));
//    la_out3 (LISA_ADD_D,  &la_t3, &la_t3, &la_t6);
//    la_out2i(LISA_ADDI_D, &la_t3, &la_t3, -1);
//    la_out3 (LISA_SUB_D,  &la_a4, &la_t3, &la_t1);
//    la_out3 (LISA_SUB_D,  &la_a5, &la_t3, &la_t2);
//    la_out2i(LISA_SRLI_D, &la_t4, &la_t4, 32);
//    la_out2i(LISA_SRLI_D, &la_t5, &la_t5, 32);
//    la_out2i(LISA_SRLI_D, &la_a4, &la_a4, 32);
//    la_out2i(LISA_SRLI_D, &la_a5, &la_a5, 32);
//    la_out3 (LISA_OR, &la_t4, &la_t4, &la_a4);
//    la_out3 (LISA_OR, &la_t5, &la_t5, &la_a5);
//    la_out3 (LISA_MASKNEZ, &la_a2, &la_a2, &la_t4);
//    la_out3 (LISA_MASKNEZ, &la_a3, &la_a3, &la_t5);
//    la_out3 (LISA_OR,   &la_t4, &la_a2, &la_a3);
//    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*0000000000000000-00000000BFFFFFFF : ok */
    /*00000000C0000000-00000000FFFFFFFF : not ok */
    /*0000000100000000-000000023fffffff : ok */
    la_out2i(LISA_SRLI_D, &la_t4, &la_t1, 28);
    la_out2i(LISA_SRLI_D, &la_t5, &la_t2, 28);
    la_out2i(LISA_ADDI_D, &la_t4, &la_t4, -0xc);
    la_out2i(LISA_ADDI_D, &la_t5, &la_t5, -0xc);
    la_out2i(LISA_SRLI_D, &la_t4, &la_t4, 2);
    la_out2i(LISA_SRLI_D, &la_t5, &la_t5, 2);
    la_out3 (LISA_MASKEQZ, &la_a2, &la_a2, &la_t4);
    la_out3 (LISA_MASKEQZ, &la_a3, &la_a3, &la_t5);
    la_out3(LISA_OR,   &la_t4, &la_a2, &la_a3);
    la_out2(LISA_BEQZ, &la_t4, &ptw_no);

    la_out2i(LISA_SRLI_D, &la_t4, &la_t1, 12);     // paddr0 >> 12
    la_out2i(LISA_SRLI_D, &la_t5, &la_t2, 12);     // paddr1 >> 12
    la_out3 (LISA_ADD_D,  &la_t1, &la_t1, &la_t7);
    la_out3 (LISA_ADD_D,  &la_t2, &la_t2, &la_t7);
    la_out2i(LISA_LD_D,   &la_t3, &la_env, offsetof(CPUX86State, ptw4k));
    la_out3 (LISA_MASKEQZ, &la_t4, &la_t4, &la_a2);
    la_out3 (LISA_MASKEQZ, &la_t5, &la_t5, &la_a3);
    la_out3 (LISA_LDX_B,  &la_t4, &la_t3, &la_t4); // ptwdata[paddr0]
    la_out3 (LISA_LDX_B,  &la_t5, &la_t3, &la_t5); // ptwdata[paddr1]
    /*
     * la_t0 : vaddr
     * la_t1 : haddr 4K page for elo0
     * la_t2 : haddr 4K page for elo1
     * la_t4 : ptwdata[paddr0>>12] for elo0
     * la_t5 : ptwdata[paddr1>>12] for elo1
     * la_a0 : RW for elo0
     * la_a1 : RW for elo1
     * la_a2 : valid for elo0
     * la_a3 : valid for elo1
     */
    la_out2i(LISA_ORI, &la_t6, &la_zero, 0x2); // D(1) in elo0
    la_out2i(LISA_ORI, &la_t7, &la_zero, 0x2); // D(1) in elo1
    la_out3(LISA_MASKEQZ, &la_t6, &la_t6, &la_a0); // if (!rw) D=0
    la_out3(LISA_MASKEQZ, &la_t7, &la_t7, &la_a1);
    la_out3(LISA_MASKNEZ, &la_t6, &la_t6, &la_t4); // if (!ptwdata) D=0
    la_out3(LISA_MASKNEZ, &la_t7, &la_t7, &la_t5);
    /*
     * la_t0 : vaddr
     * la_t1 : haddr 4K page for elo0
     * la_t2 : haddr 4K page for elo1
     * la_t4 : ptwdata[paddr0>>12] for elo0
     * la_t5 : ptwdata[paddr1>>12] for elo1
     * la_t6 : D(1) for elo0
     * la_t7 : D(1) for elo1
     * la_a2 : valid for elo0
     * la_a3 : valid for elo1
     */
    la_out2i(LISA_ORI, &la_t3, &la_zero, 0x1); // v(0) in elo
    la_out2i(LISA_ANDI, &la_t4, &la_t4, 0x1);
    la_out2i(LISA_ANDI, &la_t5, &la_t5, 0x1);
    la_out3(LISA_MASKNEZ, &la_t4, &la_t3, &la_t4); // elo0 valid after mmio
    la_out3(LISA_MASKNEZ, &la_t5, &la_t3, &la_t5); // elo1 valid after mmio
    la_out3(LISA_MASKEQZ, &la_t4, &la_t4, &la_a2); // elo0 valid after PTW
    la_out3(LISA_MASKEQZ, &la_t5, &la_t5, &la_a3); // elo1 valid after PTW
    la_out3(LISA_OR,   &la_a2, &la_t4, &la_t5);
    la_out2(LISA_BEQZ, &la_a2, &ptw_no);

    /*
     * la_t0 : vaddr
     * la_t1 : haddr 4K page for 0
     * la_t2 : haddr 4K page for 1
     * la_t4 : elo0 valid
     * la_t5 : elo1 valid
     * la_t6 : D(1) for 0
     * la_t7 : D(1) for 1
     */
    la_out3 (LISA_OR,  &la_t1, &la_t1, &la_t4); // V(0) in elo0
    la_out3 (LISA_OR,  &la_t2, &la_t2, &la_t5); // V(0) in elo1
    la_out2i(LISA_ORI, &la_t1, &la_t1, 0x10); // MAT(5:4)=1, PLV(3:2)=0, D(1)=0
    la_out2i(LISA_ORI, &la_t2, &la_t2, 0x10); // MAT(5:4)=1, PLV(3:2)=0, D(1)=0
    la_out3 (LISA_OR,  &la_t1, &la_t1, &la_t6); // D(1) in elo0
    la_out3 (LISA_OR,  &la_t2, &la_t2, &la_t7); // D(1) in elo1

    /*
     * register status at here
     * la_t0 : vaddr
     * la_t1 : elo0
     * la_t2 : elo1
     */
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_ORI,    &la_t0, &la_t0, 0xc); // PS 4K
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi,  &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_t1);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_t2);
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t0, &la_t1, HD_CNT_TLBR_X86_OK_INDEX);
#endif

    la_out1(LISA_B, &ptw_ok);
    la_label(ptw_no);
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_ORI,    &la_t0, &la_t0, 0xc); // PS 4K
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi,  &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_zero);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_zero);
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t0, &la_t1, HD_CNT_TLBR_X86_XX_INDEX);
#endif
    la_label(ptw_ok);

    la_out2i(LISA_LD_D, &la_t3, &la_env, offsetof(CPUX86State, save_regs[15]));
    la_out2i(LISA_LD_D, &la_t4, &la_env, offsetof(CPUX86State, save_regs[16]));
    la_out2i(LISA_LD_D, &la_t5, &la_env, offsetof(CPUX86State, save_regs[17]));
    la_out2i(LISA_LD_D, &la_t6, &la_env, offsetof(CPUX86State, save_regs[18]));
    la_out2i(LISA_LD_D, &la_t7, &la_env, offsetof(CPUX86State, save_regs[19]));

    la_out2i(LISA_LD_D, &la_a0, &la_env, offsetof(CPUX86State, save_regs[4]));
    la_out2i(LISA_LD_D, &la_a1, &la_env, offsetof(CPUX86State, save_regs[5]));
    la_out2i(LISA_LD_D, &la_a2, &la_env, offsetof(CPUX86State, save_regs[6]));
    la_out2i(LISA_LD_D, &la_a3, &la_env, offsetof(CPUX86State, save_regs[7]));
    la_out2i(LISA_LD_D, &la_a4, &la_env, offsetof(CPUX86State, save_regs[8]));
    la_out2i(LISA_LD_D, &la_a5, &la_env, offsetof(CPUX86State, save_regs[9]));
    /*la_out2i(LISA_LD_D, &la_a6, &la_env, offsetof(CPUX86State, save_regs[10]));*/
    /*la_out2i(LISA_LD_D, &la_a7, &la_env, offsetof(CPUX86State, save_regs[11]));*/

    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);
    gen_hamt_accessed_update(&accessed, &ptw_no);
}

static void __latxs_gen_hamt_excp_tlbrefill_hamt_ptw(void)
{
    HamtPtwAccessed accessed;

#ifdef CONFIG_LATX_DEBUG
    IR2_OPND hflagscheckok = la_new_label();
    la_out2i(LISA_LD_D, &la_t0, &la_env, offsetof(CPUX86State, hflags));
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 30);
    la_out2i(LISA_ANDI, &la_t0, &la_t0, 0x1);
    la_out2 (LISA_BNEZ, &la_t0, &hflagscheckok);
    la_outa (LISA_DUP, 0x0); // hflags not HATM_EN
    la_out1 (LISA_LABEL, &hflagscheckok);
#endif

    /*
     * la_t0 : badv (aka. vaddr)
     * la_t1 : cr3
     * la_t2 : pteaddr = L4: cr3 + (vaddr[39:47] << 3)
     *                   L3: pte + (vaddr[30:38] << 3)
     *                   L2: pte + (vaddr[21:29] << 3)
     *                   L1: pte + (vaddr[12:20] << 3)
     * la_t3 : pte = *(pteaddr + rambase)
     * la_t4 : temp
     * la_t5 : page size : 1G=0x40000000=(1<<30) 2M=0x200000=(1<<21) 4K=0x1000=(1<<12)
     * la_t6 : PG_USER_MASK | PG_RW_MASK
     * la_t7 : rambase = 0x90_0000_0000 ULL
     */
    IR2_OPND ptw_no = la_new_label();
    IR2_OPND ptw_ok = la_new_label();

    la_out2i(LISA_ST_D, &la_t3, &la_env, offsetof(CPUX86State, save_regs[15]));
    la_out2i(LISA_ST_D, &la_t4, &la_env, offsetof(CPUX86State, save_regs[16]));
    la_out2i(LISA_ST_D, &la_t5, &la_env, offsetof(CPUX86State, save_regs[17]));
    la_out2i(LISA_ST_D, &la_t6, &la_env, offsetof(CPUX86State, save_regs[18]));
    la_out2i(LISA_ST_D, &la_t7, &la_env, offsetof(CPUX86State, save_regs[19]));

    /*
     *   44   40   36   32   28   24   20   16   12    8    4    0
     * #### #### #### #### #### #### #### #### #### #### #### ####
     * |    L4   |    L3    |    L2    |    L1    |              |
     * 47        39         30         21         12             0
     */

    la_out2(LISA_CSRRD, &la_t0, &la_csr_tlbr_badv);
    la_out2i(LISA_LD_D, &la_t1, &la_env, offsetof(CPUX86State, cr[3]));
    la_out2_(lisa_mov,  &la_t7, &la_zero);
    la_imm(&la_t7, HACK_RAMBASE);
    la_out2i(LISA_ORI, &la_t6, &la_zero,
             PG_USER_MASK | PG_RW_MASK | PG_ACCESSED_MASK);

    /* ========================== */
    /* === Page Table Level 4 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t1, &la_t2, &la_t0, 39);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & (rsvd_mask | PG_PSE_MASK)) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    la_out2i(LISA_ANDI, &la_t4, &la_t3, PG_PSE_MASK);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);

    IR2_OPND ptw_bh = la_new_label();

    /* ========================== */
    /* === Page Table Level 3 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t3, &la_t2, &la_t0, 30);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    /* if (pte & PG_PSE_MASK) {*/
    IR2_OPND ptw_l3_no_pse = la_new_label();
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x80);
    la_out2(LISA_BEQZ,  &la_t4, &ptw_l3_no_pse);
    record_hamt_large_page(true);
    la_out2i(LISA_ORI,    &la_t5, &la_zero, 0x1);
    la_out2i(LISA_SLLI_D, &la_t5, &la_t5, 30); // pagesize = 1G = (1<<30)
    la_out1 (LISA_B, &ptw_bh);
    la_label(ptw_l3_no_pse);
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);

    /* ========================== */
    /* === Page Table Level 2 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t3, &la_t2, &la_t0, 21);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    /* if (pte & PG_PSE_MASK) {*/
    IR2_OPND ptw_l2_no_pse = la_new_label();
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x80);
    la_out2(LISA_BEQZ,  &la_t4, &ptw_l2_no_pse);
    record_hamt_large_page(false);
    la_out2i(LISA_ORI,    &la_t5, &la_zero, 0x1);
    la_out2i(LISA_SLLI_D, &la_t5, &la_t5, 21); // pagesize = 2M = (1<<21)
    la_out1 (LISA_B, &ptw_bh);
    la_label(ptw_l2_no_pse);
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);

    /* ========================== */
    /* === Page Table Level 1 === */
    /* ========================== */
    latxs_ptw_address(&la_t2, &la_t3, &la_t2, &la_t0, 12);
    la_out3  (LISA_LDX_D,      &la_t3, &la_t2, &la_t7); // pte = *(pteaddr + rambase)
    /*if (!(pte & PG_PRESENT_MASK)) goto do_fault;*/
    la_out2i(LISA_ANDI, &la_t4, &la_t3, 0x1);
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    /*if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 40);
    la_out2  (LISA_BNEZ,       &la_t4, &ptw_no);
    /*ptep &= pte ^ PG_NX_MASK;*/
    la_out2i(LISA_ORI,    &la_t5, &la_zero, 0x1);
    la_out2i(LISA_SLLI_D, &la_t5, &la_t5, 12); // pagesize = 4K = (1<<12)


    la_label(ptw_bh);
    gen_hamt_parent_accessed(&ptw_no);
    la_out3(LISA_AND, &la_t6, &la_t6, &la_t3);
    /*
     * registers status at here
     * la_t0 : vaddr
     * la_t1 : cr3 : free to use as temp from here
     * la_t2 : pteaddr : to update pte ACCESSED/DIRTY bit
     * la_t3 : pte
     * la_t4 : temp
     * la_t5 : page size
     * la_t6 : PG_USER_MASK | PG_RW_MASK : to check USER bit
     * la_t7 : rambase 
     */
    la_out2i(LISA_ADDI_D, &la_t5, &la_t5, -1); // page_size - 1

    /* rsvd_mask |= (page_size - 1) & PG_ADDRESS_MASK & ~PG_PSE_PAT_MASK;*/
    /* if (pte & rsvd_mask) goto do_fault_rsvd;*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 13); // bit 12 : PG_PSE_PAT_MASK
    la_out2i (LISA_SLLI_D,     &la_t4, &la_t4, 13); // ptw[13:51]
    la_out3 (LISA_AND,  &la_t4, &la_t4, &la_t5);
    la_out2 (LISA_BNEZ, &la_t4, &ptw_no);

    /*if (is_user && !(ptep & PG_USER_MASK)) goto do_fault_protect;*/
    if (hamt_do_cpl_only(3)) {
        la_out2i(LISA_ANDI, &la_t4, &la_t6, PG_USER_MASK); // 4=USER1 0=USER0
    } else {
        la_out2i(LISA_LD_D, &la_t1, &la_env, offsetof(CPUX86State, hflags));
        la_out2i(LISA_ADDI_D, &la_t1, &la_t1, 1); // 1=CPL0 4=CPL3
        la_out2i(LISA_ANDI, &la_t1, &la_t1, 0x3); // 1=CPL0 0=CPL3
        la_out2i(LISA_ANDI, &la_t4, &la_t6, PG_USER_MASK); // 4=USER1 0=USER0
        la_out3(LISA_ADD_D, &la_t4, &la_t4, &la_t1); // CPL3 && USER0
    }
    la_out2 (LISA_BEQZ, &la_t4, &ptw_no);
    la_out2i(LISA_ANDI, &la_t6, &la_t6, PG_RW_MASK);

    /* Read-only refill entries defer first-write Dirty updates to C. */
    gen_hamt_leaf_accessed(&accessed);
    gen_hamt_leaf_write_permission();

    /*
     * registers status at here
     * la_t0 : vaddr
     * la_t1 : temp
     * la_t2 : temp
     * la_t3 : pte : to get paddr
     * la_t4 : temp
     * la_t5 : pagesize - 1 : to get paddr
     * la_t6 : PG_RW_MASK : to fill hardware tlb
     * la_t7 : rambase : to get haddr
     */

    /*paddr = (pte & PG_ADDRESS_MASK & ~(page_size - 1)) | (addr & (page_size - 1));*/
    la_out2ii(LISA_BSTRPICK_D, &la_t4, &la_t3, 51, 0);
    la_out3 (LISA_NOR, &la_t1, &la_t5, &la_zero); // ~(page_size - 1)
    la_out3 (LISA_AND, &la_t4, &la_t4, &la_t1);
    la_out3 (LISA_AND, &la_t1, &la_t0, &la_t5);
    la_out3 (LISA_OR,     &la_t2, &la_t4, &la_t1); // paddr
    la_out2i(LISA_SRLI_D, &la_t4, &la_t2, 12);    // paddr >> 12
    la_out3 (LISA_ADD_D,  &la_t2, &la_t2, &la_t7); // haddr = paddr + rambase

    /*0000000000000000-00000000BFFFFFFF : ok */
    /*00000000C0000000-00000000FFFFFFFF : not ok */
    /*0000000100000000-000000023fffffff : ok */
    la_out2i(LISA_SRLI_D, &la_t1, &la_t4, 28 - 12);
    la_out2i(LISA_ADDI_D, &la_t1, &la_t1, -0xc);
    la_out2i(LISA_SRLI_D, &la_t1, &la_t1, 2);
    la_out2 (LISA_BEQZ,   &la_t1, &ptw_no);

    /* check ptwdata[paddr >> 12] */
    la_out2i(LISA_LD_D,  &la_t1, &la_env, offsetof(CPUX86State, ptw4k));
    la_out3 (LISA_LDX_B, &la_t7, &la_t1, &la_t4);
    la_out2 (LISA_BNEZ,  &la_t7, &ptw_no);

    /*
     * la_t0 : vaddr
     * la_t2 : haddr
     * la_t6 : RW
     *
     * la_t4 : bit12 : 0=elo0 1=elo1
     */
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 12);
    la_out2i(LISA_ANDI,   &la_t4, &la_t0, 0x1);
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 1);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_ORI,    &la_t0, &la_t0, 0xc); // PS 4K

    la_out2i(LISA_SRLI_D, &la_t2, &la_t2, 12);
    la_out2i(LISA_SLLI_D, &la_t2, &la_t2, 12);
    // MAT(5:4)=1, PLV(3:2)=0, D(1)=0, V(0)=1
    la_out2i(LISA_ORI, &la_t2, &la_t2, 0x11);
    // if (rw==0) D(1)=0; else D(1)=0x1;
    la_out2i(LISA_ORI,     &la_t3, &la_zero, 0x2);
    la_out3 (LISA_MASKEQZ, &la_t3, &la_t3, &la_t6);
    la_out3 (LISA_OR,      &la_t2, &la_t2, &la_t3);

    // elo0 = (bit12 == 0) ? haddr : zero
    // elo1 = (bti12 == 0) ? zero  : haddr
    la_out3(LISA_MASKNEZ, &la_t3, &la_t2, &la_t4); // elo0
    la_out3(LISA_MASKEQZ, &la_t6, &la_t2, &la_t4); // elo1
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi,  &la_t0); // PS 4K
    la_out2i(LISA_ORI, &la_t0, &la_zero, 0xc);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 24);
    la_out2(LISA_CSRWR, &la_csr_tlbidx, &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_t3);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_t6);
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */

    la_out1(LISA_B, &ptw_ok);
    la_label(ptw_no);
    la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 13);
    la_out2i(LISA_ORI,    &la_t0, &la_t0, 0xc); // PS 4K
    la_out2(LISA_CSRWR, &la_csr_tlbr_ehi,  &la_t0);
    la_out2i(LISA_ORI, &la_t0, &la_zero, 0xc);
    la_out2i(LISA_SLLI_D, &la_t0, &la_t0, 24);
    la_out2(LISA_CSRWR, &la_csr_tlbidx, &la_t0);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo0, &la_zero);
    la_out2(LISA_CSRWR, &la_csr_tlbr_elo1, &la_zero);
    la_outa(LISA_DUP, 0x06483400); /* tlbfill */
    la_label(ptw_ok);

    /*la_out2i(LISA_LD_D, &la_t0, &la_env, offsetof(CPUX86State, save_regs[12]));*/
    /*la_out2i(LISA_LD_D, &la_t1, &la_env, offsetof(CPUX86State, save_regs[13]));*/
    /*la_out2i(LISA_LD_D, &la_t2, &la_env, offsetof(CPUX86State, save_regs[14]));*/
    la_out2i(LISA_LD_D, &la_t3, &la_env, offsetof(CPUX86State, save_regs[15]));
    la_out2i(LISA_LD_D, &la_t4, &la_env, offsetof(CPUX86State, save_regs[16]));
    la_out2i(LISA_LD_D, &la_t5, &la_env, offsetof(CPUX86State, save_regs[17]));
    la_out2i(LISA_LD_D, &la_t6, &la_env, offsetof(CPUX86State, save_regs[18]));
    la_out2i(LISA_LD_D, &la_t7, &la_env, offsetof(CPUX86State, save_regs[19]));

    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);
    gen_hamt_accessed_update(&accessed, &ptw_no);
}

static void __latxs_gen_hamt_excp_tlbrefill(hamt_excp_t *d, int *need_ptw)
{
    la_out2(LISA_CSRWR, &la_csr_tlbr_save, &la_t0);
    la_out2(LISA_CSRWR, &la_csr_ks0,       &la_t1);
    la_out2(LISA_CSRWR, &la_csr_ks1,       &la_t2);

#ifdef CONFIG_LATX_PROFILER
    la_imm(&la_t0, (uint64_t)d->database);
    la_out2 (LISA_RDTIME_D, &la_t1, &la_zero); // t1 = start time
    la_out2i(LISA_ST_D, &la_t1, &la_t0, HD_TLBR_RDTIME_ST * 8);
#endif

    IR2_OPND ereturn = la_new_label();

    IR2_OPND hamtspace = la_new_label();
    if (hamt_mode_dmw()) {
        la_out2(LISA_CSRRD, &la_t0, &la_csr_asid);
        la_out2i(LISA_ANDI, &la_t0, &la_t0, 0x3ff);
        la_out2(LISA_BNEZ,  &la_t0, &hamtspace);
    } else if (hamt_mode_hole()) {
        la_out2(LISA_CSRRD, &la_t0, &la_csr_tlbr_badv);
        if (hamt_hole_mode_48_7f()) {
            la_out2i(LISA_SRLI_D, &la_t1, &la_t0, 40);
            la_out2i(LISA_ORI,    &la_t0, &la_zero, 0x7f);
            la_out3 (LISA_BNE,    &la_t0, &la_t1, &hamtspace);
        } else if (hamt_hole_mode_40_8()) {
            la_out2i(LISA_SRLI_D, &la_t0, &la_t0, 39);
            la_out2i(LISA_ADDI_D, &la_t0, &la_t0, -1);
            la_out2(LISA_BNEZ, &la_t0, &hamtspace);
        } else {
            printf("%s:%d hamt hole error : mode %d submode %d\n", __func__, __LINE__,
                    option_hamt_mode, option_hamt_submode);
        }
    } else {
        printf("%s:%d hamt mode error %d\n", __func__, __LINE__,
                option_hamt_mode);
        exit(-1);
    }

    /* ====================================== */
    /* === dbt space, 1G entry and return === */
    /* ====================================== */
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t0, &la_t1, HD_CNT_TLBR_DBT_INDEX);
#endif
    __latxs_gen_hamt_excp_tlbrefill_dbt();
    la_out1(LISA_B, &ereturn);

    /* ================================== */
    /* === hamt space, need GVA - HVA === */
    /* ================================== */
    la_label(hamtspace);
    if (option_pcid_stats) {
        la_out2(LISA_RDTIME_D, &la_t0, &la_zero);
        la_out2i(LISA_ST_D, &la_t0, &la_env,
                 offsetof(CPUX86State, pcid_refill_start));
        la_out2i(LISA_LD_D, &la_t0, &la_env,
                 offsetof(CPUX86State, pcid_refills));
        la_out2i(LISA_ADDI_D, &la_t0, &la_t0, 1);
        la_out2i(LISA_ST_D, &la_t0, &la_env,
                 offsetof(CPUX86State, pcid_refills));
    }
#ifdef CONFIG_LATX_PRINT
    __latxs_counter(d->database, 1, &la_t0, &la_t1, HD_CNT_TLBR_X86_INDEX);
#endif

    if (hamt_use_pgd()) {
        printf("%s:%d hamt tlbrefill pgd not implement\n", __func__, __LINE__);
        __latxs_gen_hamt_excp_tlbrefill_hamt_zero();
    } else if (hamt_use_ptw_inrefill()) {
        if (hamt_do_cpl_any()) {
            la_imm(&la_t0, (uint64_t)d->subcodebase);
            la_out2i(LISA_ST_D, &la_ra, &la_env, offsetof(CPUX86State, save_regs[3]));
            la_out2i(LISA_JIRL, &la_ra, &la_t0, 0);
            la_out2i(LISA_LD_D, &la_ra, &la_env, offsetof(CPUX86State, save_regs[3]));
            *need_ptw = 1;
        } else {
            __latxs_gen_hamt_excp_tlbrefill_hamt_zero();
        }
    } else {
        __latxs_gen_hamt_excp_tlbrefill_hamt_zero();
    }

    if (option_pcid_stats) {
        la_out2(LISA_RDTIME_D, &la_t0, &la_zero);
        la_out2i(LISA_LD_D, &la_t1, &la_env,
                 offsetof(CPUX86State, pcid_refill_start));
        la_out3(LISA_SUB_D, &la_t0, &la_t0, &la_t1);
        la_out2i(LISA_LD_D, &la_t1, &la_env,
                 offsetof(CPUX86State, pcid_refill_ticks));
        la_out3(LISA_ADD_D, &la_t0, &la_t0, &la_t1);
        la_out2i(LISA_ST_D, &la_t0, &la_env,
                 offsetof(CPUX86State, pcid_refill_ticks));
    }
#ifdef CONFIG_LATX_PROFILER
    la_imm(&la_t0, (uint64_t)d->database);
    la_out2 (LISA_RDTIME_D, &la_t2, &la_zero); // t2 = end time
    la_out2i(LISA_LD_D,  &la_t1, &la_t0, HD_TLBR_RDTIME_ST * 8);
    la_out3 (LISA_SUB_D, &la_t2, &la_t2, &la_t1); // t2 = end - start
    la_out2i(LISA_LD_D,  &la_t1, &la_t0, HD_TLBR_RDTIME_ALL * 8);
    la_out3 (LISA_ADD_D, &la_t1, &la_t1, &la_t2); // ALL += t2
    la_out2i(LISA_ST_D,  &la_t1, &la_t0, HD_TLBR_RDTIME_ALL * 8);
#endif

    la_label(ereturn);
    la_out2(LISA_CSRRD, &la_t0, &la_csr_tlbr_save);
    la_out2(LISA_CSRRD, &la_t1, &la_csr_ks0);
    la_out2(LISA_CSRRD, &la_t2, &la_csr_ks1);
    la_outa(LISA_DUP, 0x06483800); /* ertn */
}

void latxs_gen_hamt_excp(hamt_excp_t *d)
{
    void *code = d->codebase;

    int need_ptw = 0;

    if (hamt_dump_enabled()) {
        latx_enable_dump();
        d->verbose = 1;
    }

    latxs_tr_begin(code);

    switch (d->type) {
    case HAMT_EXCP_ENTRY_SYSCALL:
        if (d->verbose) printf("build hamt syscall at %p\n", code);
        __latxs_gen_hamt_excp_syscall();
        break;
    case HAMT_EXCP_ENTRY_TLBREFILL:
        if (d->verbose) printf("build hamt tlbrefill at %p\n", code);
        __latxs_gen_hamt_excp_tlbrefill(d, &need_ptw);
        break;
    case HAMT_EXCP_INTERRUPT_IPI:
        if (d->verbose) printf("build hamt interrupt ipi at %p\n", code);
        __latxs_gen_hamt_interrupt_ipi(d);
        break;
    case HAMT_EXCP_ENTRY_FAKE_FORK:
        if (d->verbose) printf("build hamt fake fork at %p\n", code);
        __latxs_gen_hamt_fake_fork();
        break;
    default:
        break;
    }

    int nr = latxs_tr_end(code);

    uint64_t *begin = d->begin;
    uint64_t *end   = d->end;
    int *size = d->size;

    *size  = nr * 4;
    *begin = (uint64_t)d->codebase;
    *end   = (uint64_t)d->codebase + (nr * 4);

    if (need_ptw && hamt_use_ptw_inrefill()) {
        code = d->subcodebase;
        latxs_tr_begin(code);
        if (hamt_use_ptw_inrefill_4K1()) {
            if (d->verbose) printf("build hamt tlbrefill ptw 4k1 at %p\n", code);
            __latxs_gen_hamt_excp_tlbrefill_hamt_ptw();
        } else if (hamt_use_ptw_inrefill_4K2()) {
            if (d->verbose) printf("build hamt tlbrefill ptw 4k2 at %p\n", code);
            __latxs_gen_hamt_excp_tlbrefill_hamt_ptw2(d);
        } else if (hamt_use_ptw_inrefill_4K2M()) {
            if (d->verbose) printf("build hamt tlbrefill ptw 4k2M at %p\n", code);
            __latxs_gen_hamt_excp_tlbrefill_hamt_ptw2m(d);
        } else {
            printf("%s:%d hamt tlbrefill PTW unknown mode\n", __func__, __LINE__);
            __latxs_gen_hamt_excp_tlbrefill_hamt_zero();
        }
        nr = latxs_tr_end(code);
        g_assert(nr * 4 <= HAMT_PTW_CODE_SIZE);
        if (d->verbose) {
            printf("hamt refill PTW: %d/%d bytes\n",
                   nr * 4, HAMT_PTW_CODE_SIZE);
        }
    }

    if (hamt_dump_enabled()) latx_disable_dump();
}

static void latxs_gen_save_t0t1(hamt_excp_t *d,
                                IR2_OPND  *kscratch)
{
    la_out2(LISA_CSRWR, kscratch, &la_t0);
    la_imm(&la_t0, (uint64_t)d->database);
    la_out2i(LISA_LU52I_D, &la_t0, &la_t0, 0x900);
    la_out2i(LISA_ST_D, &la_t1, &la_t0, PT_R13);
    la_out2(LISA_CSRRD, &la_t1, kscratch);
    la_out2i(LISA_ST_D, &la_t1, &la_t0, PT_R12);
}

static void latxs_gen_restore_t0t1(void)
{
    la_out2i(LISA_LD_D, &la_t1, &la_t0, PT_R13);
    la_out2i(LISA_LD_D, &la_t0, &la_t0, PT_R12);
}

static void latxs_gen_save_some(void)
{
    IR2_OPND la_reg;

    la_out2i(LISA_ST_D, &la_zero, &la_t0, PT_R0);
    la_out2i(LISA_ST_D, &la_ra, &la_t0, PT_R1);
    la_reg = la_new_gpr(2);
    la_out2i(LISA_ST_D, &la_reg, &la_t0, PT_R2);
    la_out2i(LISA_ST_D, &la_sp, &la_t0, PT_R3);
    la_out2i(LISA_ST_D, &la_a0, &la_t0, PT_R4);
    la_out2i(LISA_ST_D, &la_a1, &la_t0, PT_R5);
    la_out2i(LISA_ST_D, &la_a2, &la_t0, PT_R6);
    la_out2i(LISA_ST_D, &la_a3, &la_t0, PT_R7);
    la_out2i(LISA_ST_D, &la_a4, &la_t0, PT_R8);
    la_out2i(LISA_ST_D, &la_a5, &la_t0, PT_R9);
    la_out2i(LISA_ST_D, &la_a6, &la_t0, PT_R10);
    la_out2i(LISA_ST_D, &la_a7, &la_t0, PT_R11);
    la_reg = la_new_gpr(21);
    la_out2i(LISA_ST_D, &la_reg, &la_t0, PT_R21);
    la_out2i(LISA_ST_D, &la_fp, &la_t0, PT_R22);
    /* save ERA */
    la_out2 (LISA_CSRRD, &la_t1, &la_csr_era);
    la_out2i(LISA_ST_D,  &la_t1, &la_t0, PT_ERA);
    /* save PRMD */
    la_out2 (LISA_CSRRD, &la_t1, &la_csr_prmd);
    la_out2i(LISA_ST_D,  &la_t1, &la_t0, PT_PRMD);
    if (hamt_mode_dmw()) {
        la_out2 (LISA_CSRRD, &la_t1, &la_csr_asid);
        la_out2i(LISA_ST_D,  &la_t1, &la_t0, PT_ASID);
        la_out2(LISA_CSRWR, &la_csr_asid, &la_zero);
    } else {
        assert(hamt_mode_hole());
    }
}

static void latxs_gen_save_temp(void)
{
    IR2_OPND la_t8 = la_new_gpr(20);
    la_out2i(LISA_ST_D, &la_t2, &la_t0, PT_R14);
    la_out2i(LISA_ST_D, &la_t3, &la_t0, PT_R15);
    la_out2i(LISA_ST_D, &la_t4, &la_t0, PT_R16);
    la_out2i(LISA_ST_D, &la_t5, &la_t0, PT_R17);
    la_out2i(LISA_ST_D, &la_t6, &la_t0, PT_R18);
    la_out2i(LISA_ST_D, &la_t7, &la_t0, PT_R19);
    la_out2i(LISA_ST_D, &la_t8, &la_t0, PT_R20);
}

static void latxs_gen_save_static(void)
{
    la_out2i(LISA_ST_D, &la_s0, &la_t0, PT_R23);
    la_out2i(LISA_ST_D, &la_s1, &la_t0, PT_R24);
    la_out2i(LISA_ST_D, &la_s2, &la_t0, PT_R25);
    la_out2i(LISA_ST_D, &la_s3, &la_t0, PT_R26);
    la_out2i(LISA_ST_D, &la_s4, &la_t0, PT_R27);
    la_out2i(LISA_ST_D, &la_s5, &la_t0, PT_R28);
    la_out2i(LISA_ST_D, &la_s6, &la_t0, PT_R29);
    la_out2i(LISA_ST_D, &la_s7, &la_t0, PT_R30);
    la_out2i(LISA_ST_D, &la_s8, &la_t0, PT_R31);
}

static void latxs_gen_restore_some(void)
{
    IR2_OPND la_reg;

    la_out2i(LISA_LD_D, &la_ra, &la_t0, PT_R1);
    la_reg = la_new_gpr(2);
    la_out2i(LISA_LD_D, &la_reg, &la_t0, PT_R2);
    la_out2i(LISA_LD_D, &la_sp, &la_t0, PT_R3);
    la_out2i(LISA_LD_D, &la_a0, &la_t0, PT_R4);
    la_out2i(LISA_LD_D, &la_a1, &la_t0, PT_R5);
    la_out2i(LISA_LD_D, &la_a2, &la_t0, PT_R6);
    la_out2i(LISA_LD_D, &la_a3, &la_t0, PT_R7);
    la_out2i(LISA_LD_D, &la_a4, &la_t0, PT_R8);
    la_out2i(LISA_LD_D, &la_a5, &la_t0, PT_R9);
    la_out2i(LISA_LD_D, &la_a6, &la_t0, PT_R10);
    la_out2i(LISA_LD_D, &la_a7, &la_t0, PT_R11);
    la_reg = la_new_gpr(21);
    la_out2i(LISA_LD_D, &la_reg, &la_t0, PT_R21);
    la_out2i(LISA_LD_D, &la_fp, &la_t0, PT_R22);
    /* restore ERA */
    la_out2i(LISA_LD_D,  &la_t1, &la_t0, PT_ERA);
    la_out2 (LISA_CSRWR, &la_csr_era, &la_t1);
    /* restore PRMD */
    la_out2i(LISA_LD_D,  &la_t1, &la_t0, PT_PRMD);
    la_out2 (LISA_CSRWR, &la_csr_prmd, &la_t1);
    if (hamt_mode_dmw()) {
        la_out2i(LISA_LD_D,  &la_t1, &la_t0, PT_ASID);
        la_out2 (LISA_CSRWR, &la_csr_asid, &la_t1);
    } else {
        assert(hamt_mode_hole());
    }
}

static void latxs_gen_restore_temp(void)
{
    IR2_OPND la_t8 = la_new_gpr(20);
    la_out2i(LISA_LD_D, &la_t2, &la_t0, PT_R14);
    la_out2i(LISA_LD_D, &la_t3, &la_t0, PT_R15);
    la_out2i(LISA_LD_D, &la_t4, &la_t0, PT_R16);
    la_out2i(LISA_LD_D, &la_t5, &la_t0, PT_R17);
    la_out2i(LISA_LD_D, &la_t6, &la_t0, PT_R18);
    la_out2i(LISA_LD_D, &la_t7, &la_t0, PT_R19);
    la_out2i(LISA_LD_D, &la_t8, &la_t0, PT_R20);
}

static void latxs_gen_restore_static(void)
{
    la_out2i(LISA_LD_D, &la_s0, &la_t0, PT_R23);
    la_out2i(LISA_LD_D, &la_s1, &la_t0, PT_R24);
    la_out2i(LISA_LD_D, &la_s2, &la_t0, PT_R25);
    la_out2i(LISA_LD_D, &la_s3, &la_t0, PT_R26);
    la_out2i(LISA_LD_D, &la_s4, &la_t0, PT_R27);
    la_out2i(LISA_LD_D, &la_s5, &la_t0, PT_R28);
    la_out2i(LISA_LD_D, &la_s6, &la_t0, PT_R29);
    la_out2i(LISA_LD_D, &la_s7, &la_t0, PT_R30);
    la_out2i(LISA_LD_D, &la_s8, &la_t0, PT_R31);
}
static void latxs_gen_ras_native_tp(void)
{
    if (option_rastoptp) {
        IR2_OPND tp = la_new_gpr(2);
        /* t0 is the fixed per-vCPU database; neither TLS nor env is needed. */
        la_out2i(LISA_LD_D, &tp, &la_t0, HD_RAS_NATIVE_TP_INDEX * 8);
    }
}

void latxs_gen_hamt_int(hamt_excp_t *d)
{
    void *code = d->codebase;

    if (hamt_dump_enabled()) {
        latx_enable_dump();
        d->verbose = 1;
    }

    latxs_tr_begin(code);

    /* save t0, t1 */
    latxs_gen_save_t0t1(d, &la_csr_ks0);
    /* load database to t0 */
    la_imm(&la_t0, (uint64_t)d->database);
    la_out2i(LISA_LU52I_D, &la_t0, &la_t0, 0x900);
    latxs_gen_save_some();
    latxs_gen_save_temp();
    latxs_gen_save_static();
    latxs_gen_ras_native_tp();

    /* hardirq context */
    switch (d->type) {
    case HAMT_EXCP_ENTRY_TI:
    {
        if (d->verbose) printf("build ti at %p\n", code);
        /*
         * In one-shot mode, clear En bit after interrupt;
         */
        IR2_OPND clear_ti = la_new_label();
        la_out2(LISA_CSRRD, &la_t0, &la_csr_tcfg);
        la_out2ii(LISA_BSTRPICK_D, &la_t1, &la_t0,
            CSR_TCFG_PERIOD_SHIFT, CSR_TCFG_PERIOD_SHIFT);
        la_out2(LISA_BNEZ, &la_t1, &clear_ti);
        la_out2(LISA_CSRWR, &la_csr_tcfg, &la_zero);

        /* Clear Timer Interrupt */
        la_label(clear_ti);
        la_out2i(LISA_ORI,  &la_t1, &la_zero, 0x1);
        la_out2(LISA_CSRWR, &la_csr_tintclear, &la_t1);
        break;
    }
    default:
        break;
    }

    /* Enable interrupts before entering bottom half handling. */
    la_out2i(LISA_ORI,  &la_t1, &la_zero, 0x7);
    la_out2i(LISA_ORI,  &la_t0, &la_zero, CSR_CRMD_IE);
    la_out3(LISA_CSRXCHG, &la_csr_crmd, &la_t0, &la_t1);
    /* subcodebase: bottom half */
    la_imm(&la_t0, (uint64_t)d->subcodebase);
    la_out1_(lisa_call, &la_t0);

    la_imm(&la_t0, (uint64_t)d->database);
    la_out2i(LISA_LU52I_D, &la_t0, &la_t0, 0x900);
    latxs_gen_restore_temp();
    latxs_gen_restore_static();
    latxs_gen_restore_some();
    /* retore t0, t1 */
    latxs_gen_restore_t0t1();
    /* ertn */
    la_outa(LISA_DUP, 0x06483800);

    int nr = latxs_tr_end(code);
    uint64_t *begin = d->begin;
    uint64_t *end   = d->end;
    int *size = d->size;

    *size  = nr * 4;
    *begin = (uint64_t)d->codebase;
    *end   = (uint64_t)d->codebase + (nr * 4);

    if (hamt_dump_enabled()) latx_disable_dump();
}

extern void hamt_native_tlb_entry(void);

void latxs_gen_hamt_exception(hamt_excp_t *d)
{
    void *code = d->codebase;

    if (hamt_dump_enabled()) {
        latx_enable_dump();
        d->verbose = 1;
    }

    latxs_tr_begin(code);

    /* save t0, t1 */
    latxs_gen_save_t0t1(d, &la_csr_ks0);
    /* load database to t0 */
    la_imm(&la_t0, (uint64_t)d->database);
    la_out2i(LISA_LU52I_D, &la_t0, &la_t0, 0x900);
    latxs_gen_save_some();
    latxs_gen_save_temp();
    latxs_gen_save_static();
    latxs_gen_ras_native_tp();

    switch (d->type) {
    case HAMT_EXCP_ENTRY_INE:
        la_out2(LISA_CSRRD, &la_t1, &la_csr_badv);
        la_out2i(LISA_ST_D, &la_t1, &la_t0, PT_BVADDR);
        la_out2(LISA_CSRRD, &la_a0, &la_csr_estat);
        la_out2i(LISA_ST_D, &la_a0, &la_t0, PT_ESTAT);
        la_out3(LISA_OR, &la_a1, &la_env, &la_zero);
        la_out2i(LISA_LD_D, &la_a2, &la_t0, PT_ERA);
        break;
    case HAMT_EXCP_ENTRY_FPE:
        la_out3(LISA_OR, &la_a0, &la_env, &la_zero);
        la_out2i(LISA_LD_D, &la_a1, &la_t0, PT_ERA);
        la_out2(LISA_MOVFCSR2GR, &la_a2, &la_fcsr);
        break;
    case HAMT_EXCP_ENTRY_ALE:
        la_out2 (LISA_CSRRD, &la_a0, &la_csr_badv);
        la_out3(LISA_OR, &la_a1, &la_env, &la_zero);
        la_out2i(LISA_LD_D, &la_a2, &la_t0, PT_ERA);
        break;
    case HAMT_EXCP_ENTRY_TLBLDSTMY:
    case HAMT_EXCP_ENTRY_ERROR_TLB:
        la_out2 (LISA_CSRRD, &la_t1, &la_csr_estat);
        la_out2i(LISA_ST_D,  &la_t1, &la_t0, PT_ESTAT);
        la_out2 (LISA_CSRRD, &la_a0, &la_csr_badv);
        la_out2i(LISA_ST_D,  &la_a0, &la_t0, PT_BVADDR);
        la_out3(LISA_OR, &la_a1, &la_env, &la_zero);
        la_out2i(LISA_LD_D, &la_a2, &la_t0, PT_ERA);
        break;
    default:
        break;
    }

    /* subcodebase: bottom half */
    if ((d->type == HAMT_EXCP_ENTRY_TLBLDSTMY ||
         d->type == HAMT_EXCP_ENTRY_ERROR_TLB) &&
        aot_tu_enabled() && option_aot_load && (cpuinfo & CPUINFO_LASX)) {
        la_imm(&la_a3, (uint64_t)d->database);
        la_out2i(LISA_LU52I_D, &la_a3, &la_a3, 0x900);
        la_imm(&la_t0, (uint64_t)hamt_native_tlb_entry);
    } else {
        la_imm(&la_t0, (uint64_t)d->subcodebase);
    }
    la_out1_(lisa_call, &la_t0);

    la_imm(&la_t0, (uint64_t)d->database);
    la_out2i(LISA_LU52I_D, &la_t0, &la_t0, 0x900);
    latxs_gen_restore_temp();
    latxs_gen_restore_static();
    latxs_gen_restore_some();
    /* retore t0, t1 */
    latxs_gen_restore_t0t1();
    /* ertn */
    la_outa(LISA_DUP, 0x06483800);

    int nr = latxs_tr_end(code);
    uint64_t *begin = d->begin;
    uint64_t *end   = d->end;
    int *size = d->size;

    *size  = nr * 4;
    *begin = (uint64_t)d->codebase;
    *end   = (uint64_t)d->codebase + (nr * 4);

    if (hamt_dump_enabled()) latx_disable_dump();
}

void latxs_gen_hamt_exception_error_stop(hamt_excp_t *d)
{
    void *code = d->codebase;

    if (hamt_dump_enabled()) {
        latx_enable_dump();
        d->verbose = 1;
    }

    latxs_tr_begin(code);

    latxs_gen_save_t0t1(d, &la_csr_ks1);
    latxs_gen_save_some();
    latxs_gen_save_temp();
    latxs_gen_save_static();
    latxs_gen_ras_native_tp();
    la_out2 (LISA_CSRRD, &la_t1, &la_csr_badv);
    la_out2i(LISA_ST_D,  &la_t1, &la_t0, PT_BVADDR);
    la_out2 (LISA_CSRRD,  &la_t1, &la_csr_tlbr_era);
    la_out2i(LISA_ORI,    &la_t1, &la_t1, 0x1);
    la_out2i(LISA_ADDI_D, &la_t1, &la_t1, -1);
    la_out2 (LISA_CSRWR,  &la_csr_tlbr_era, &la_t1);

    la_out2 (LISA_CSRRD, &la_a0, &la_csr_estat);
    la_out2i(LISA_ST_D,  &la_a0, &la_t0, PT_ESTAT);
    la_out3(LISA_OR, &la_a1, &la_env, &la_zero);
    la_out2i(LISA_LD_D, &la_a2, &la_t0, PT_ERA);

    /* no return */
    la_imm(&la_t0, (uint64_t)d->subcodebase);
    la_out1_(lisa_call, &la_t0);
    int nr = latxs_tr_end(code);
    uint64_t *begin = d->begin;
    uint64_t *end   = d->end;
    int *size = d->size;

    *size  = nr * 4;
    *begin = (uint64_t)d->codebase;
    *end   = (uint64_t)d->codebase + (nr * 4);

    if (hamt_dump_enabled()) latx_disable_dump();
}
