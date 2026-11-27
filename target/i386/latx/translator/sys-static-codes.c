/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "latx-config.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "latx-ras.h"
#include "translate.h"
#include "latx-perfmap.h"
#include "latx-static-codes.h"
#include "latx-intb-sys.h"
#include "latx-tr.h"
#include "latx-sys-hamt-dmw.h"
#include "latx-hack.h"
#include "latx-helper.h"
#include "latx-shadow-htable.h"
#include "tcg/helper-tcg.h"
#include "accel/tcg/tb-jmp-cache.h"
#include "qemu/cacheinfo.h"
#include "lats/aot-atomic.h"

typedef int (*gen_cp_check_func)(void *code_ptr,
        int n, int mmu, int mode, int ptw);
int gen_latxs_cross_page_check(void *code_ptr,
        int n, int mmu, int mode, int ptw);
int gen_latxs_cross_page_check_ptw(void *code_ptr, int mmu);

latx_sc_table_t *latx_sc_table;
static latx_sc_table_t __latx_sc_table[MTCC_NR];
static void __attribute__((__constructor__)) latxs_sc_init(void)
{
    latx_sc_table = __latx_sc_table;
}

#ifdef CONFIG_LATX_PRINT
#define LATXS_DUMP_STATIC_CODES_INFO(str, ...) do {    \
    fprintf(stderr, str, __VA_ARGS__);              \
} while (0)
#else
#define LATXS_DUMP_STATIC_CODES_INFO(str, ...) do {    \
    if (option_dump) {                                  \
        fprintf(stderr, str, __VA_ARGS__);              \
    }                                                   \
} while (0)
#endif

/*
 * When generating prologue/epilogue in system-mode,
 * the vCPU thread is not created yet.
 *
 * The lsenv is initialized at the begining of vCPU thread.
 *
 * But here we need the lsenv to generate some static
 * native codes. So we use a temp lsenv.
 */

static ENV latxs_lsenv_tmp;
static TRANSLATION_DATA latxs_tr_data_tmp;
static CPUX86State latxs_env_tmp;

int latxs_tr_ready;

static void latxs_set_lsenv_tmp(uint32_t mtcc_id)
{
    memset(&latxs_lsenv_tmp, 0, sizeof(ENV));
    memset(&latxs_tr_data_tmp, 0, sizeof(TRANSLATION_DATA));
    memset(&latxs_env_tmp, 0, sizeof(CPUX86State));

    lsenv = &latxs_lsenv_tmp;
    lsenv->cpu_state = &latxs_env_tmp;
    lsenv->tr_data = &latxs_tr_data_tmp;
    lsenv->tr_data->mtcc_id = mtcc_id;

    latxs_tr_ready = 1;
}

/* LLVM direct functions may leave fixed spill frames on r3 while an
 * ordinary callee executes. Only the enclosing translator entry owns the
 * saved host context. The software-RAS miss and dispatcher exit abandon
 * these frames and resume using the ordinary guest-PC lookup. */
static bool aot_native_outer_sp_enabled(void)
{
    return aot_tu_enabled() && hamt_enabled() && option_rastoptp;
}

static void aot_native_load_outer_sp(IR2_OPND *dest)
{
    size_t off = offsetof(CPUX86State, aot_native_outer_sp);
    g_assert(!(off & 3) && off <= 32764);
    la_out2i(LISA_LDPTR_D, dest, &la_env, off >> 2);
}

static int gen_latxs_ras_tp_fallback(void *code_ptr, unsigned mtcc_id)
{
    latxs_tr_begin(code_ptr);
    IR2_OPND tp = latxs_ir2_opnd_new(IR2_OPND_GPR, 2);
    la_out2i(LISA_ADDI_D, &tp, &la_zero, 0);
    la_out2i(LISA_STPTR_W, &la_zero, &la_env,
            offsetof(CPUX86State, ras_tp.top) >> 2);
    la_out2i(LISA_STPTR_W, &la_zero, &la_env,
            offsetof(CPUX86State, ras_tp.reset_pending) >> 2);
    if (aot_native_outer_sp_enabled()) {
        aot_native_load_outer_sp(&la_sp);
    }
    if (!intb_link_enable()) latxs_tr_gen_intb_sync_gpc();
    uintptr_t target = intb_link_enable() ? GET_SC_TABLE(mtcc_id, intb_lookup) :
                                          GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0);
    if (intb_link_enable() && aot_native_outer_sp_enabled()) {
        target = GET_SC_TABLE(mtcc_id, intb_resume_lookup);
    }
    uintptr_t branch_pc = (uintptr_t)code_ptr + lsenv->tr_data->ir2_asm_nr * 4;
    int64_t delta = (int64_t)target - (int64_t)branch_pc;
    g_assert((delta & 3) == 0 && delta / 4 == sextract64(delta / 4, 0, 26));
    la_outa(LISA_B, delta / 4);
    return latxs_tr_end(code_ptr);
}

#ifdef CONFIG_SOFTMMU
typedef enum AotReturningHelper {
    AOT_HELPER_CPU,
    AOT_HELPER_X87,
    AOT_HELPER_ATOMIC,
    AOT_HELPER_SYSTEM
} AotReturningHelper;
static void gen_aot_returning_helper_body(AotReturningHelper kind);

/* The LLVM helper ABI leaves r4-r11 and r20-r21 available as temporaries.
 * Keep the descriptor outside the software TLB lookup's r4-r6 scratch set.
 * No guest output is changed until the checked atomic access succeeds. */
static void gen_aot_atomic_fast_path(void)
{
#ifdef CONFIG_LATX_OPT_EFLAGS
    IR2_OPND descriptor = la_new_gpr(11);
    IR2_OPND previous = la_new_gpr(7);
    IR2_OPND address = la_new_gpr(8);
    IR2_OPND value = la_new_gpr(9);
    IR2_OPND accumulator = la_new_gpr(10);
    IR2_OPND kind = la_new_gpr(20);
    IR2_OPND scratch = la_new_gpr(21);
    IR2_OPND rax = la_new_gpr(24);
    IR2_OPND slow = la_new_label();
    IR2_OPND store_result = la_new_label();
    IR2_OPND complete = la_new_label();
    IR2_OPND labels[2][3];
    static const unsigned operations[] = {
        LATS_ATOMIC_CMPXCHG, LATS_ATOMIC_XCHG, LATS_ATOMIC_XADD,
    };

    la_out3(LISA_OR, &descriptor, &la_a0, &la_zero);
    /* Paired CMPXCHG8B needs EDX:EAX and changes only ZF. Its bit-10
     * marker must not enter the scalar CMPXCHG64 returning shortcut. */
    la_out2i(LISA_ANDI, &scratch, &descriptor, 1 << 10);
    la_out3(LISA_BNE, &scratch, &la_zero, &slow);
    la_out2i(LISA_ANDI, &kind, &descriptor, 63);
    la_out2i(LISA_LDPTR_D, &address, &la_env,
            offsetof(CPUX86State, aot_native_atomic.address) >> 2);
    la_out2i(LISA_LDPTR_D, &value, &la_env,
            offsetof(CPUX86State, aot_native_atomic.value) >> 2);
    for (unsigned width = 0; width < 2; width++) {
        for (unsigned operation = 0; operation < 3; operation++) {
            labels[width][operation] = la_new_label();
            if (operation == 0 && !(cpuinfo & CPUINFO_LAMCAS)) {
                continue;
            }
            unsigned encoded = operations[operation] | ((width + 2) << 4);
            la_out2i(LISA_ADDI_D, &scratch, &la_zero, encoded);
            la_out3(LISA_BEQ, &kind, &scratch, &labels[width][operation]);
        }
    }
    la_out1(LISA_B, &slow);

    for (unsigned width = 0; width < 2; width++) {
        for (unsigned operation = 0; operation < 3; operation++) {
            if (operation == 0 && !(cpuinfo & CPUINFO_LAMCAS)) {
                continue;
            }
            la_label(labels[width][operation]);
            la_out2i(LISA_ANDI, &scratch, &address, width ? 7 : 3);
            la_out3(LISA_BNE, &scratch, &la_zero, &slow);
            if (operation == 0) {
                la_out3(LISA_OR, &previous, &rax, &la_zero);
                la_out3(LISA_OR, &accumulator, &rax, &la_zero);
            }
            IR2_OPCODE opcode = operation == 0
                ? (width ? LISA_AMCAS_DB_D : LISA_AMCAS_DB_W)
                : operation == 1
                ? (width ? LISA_AMSWAP_DB_D : LISA_AMSWAP_DB_W)
                : (width ? LISA_AMADD_DB_D : LISA_AMADD_DB_W);
            tr_gen_atomic_rmw_softmmu(opcode, &previous, &value, &address,
                                      MMU_USER_IDX, &slow);
            if (!width) {
                la_out2ii(LISA_BSTRPICK_D, &previous, &previous, 31, 0);
            }
            if (operation == 0) {
                la_out2(width ? LISA_X86SUB_D : LISA_X86SUB_W,
                        &accumulator, &previous);
                if (!width) {
                    la_out2ii(LISA_BSTRPICK_D, &accumulator, &accumulator, 31, 0);
                }
                /* A successful 32-bit CMPXCHG preserves RAX's high half. */
                la_out3(LISA_BEQ, &accumulator, &previous, &complete);
                la_out3(LISA_OR, &rax, &previous, &la_zero);
                la_out1(LISA_B, &complete);
            } else {
                if (operation == 2) {
                    la_out2(width ? LISA_X86ADD_D : LISA_X86ADD_W,
                            &previous, &value);
                }
                la_out1(LISA_B, &store_result);
            }
        }
    }

    la_label(store_result);
    la_out2ii(LISA_BSTRPICK_D, &kind, &descriptor, 9, 6);
    for (unsigned reg = 0; reg < 16; reg++) {
        IR2_OPND next = la_new_label();
        IR2_OPND guest = la_new_gpr(reg < 8 ? reg + 24 : reg + 4);
        if (reg != 15) {
            la_out2i(LISA_ADDI_D, &scratch, &la_zero, reg);
            la_out3(LISA_BNE, &kind, &scratch, &next);
        }
        la_out3(LISA_OR, &guest, &previous, &la_zero);
        la_out1(LISA_B, &complete);
        la_label(next);
    }

    la_label(complete);
    la_out2i(LISA_LDPTR_D, &la_a0, &la_env, offsetof(CPUX86State, eip) >> 2);
    la_out2ii(LISA_BSTRPICK_D, &la_a1, &descriptor, 14, 11);
    la_out3(LISA_ADD_D, &la_a0, &la_a0, &la_a1);
    la_out2i(LISA_STPTR_D, &la_a0, &la_env, offsetof(CPUX86State, eip) >> 2);
    la_out2i(LISA_LDPTR_D, &la_a0, &la_env,
            offsetof(CPUX86State, aot_native_atomic.completed) >> 2);
    la_out2i(LISA_ADDI_D, &la_a0, &la_a0, 1);
    la_out2i(LISA_STPTR_D, &la_a0, &la_env,
            offsetof(CPUX86State, aot_native_atomic.completed) >> 2);
    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);

    la_label(slow);
    la_out3(LISA_OR, &la_a0, &descriptor, &la_zero);
#endif
}

/* r4 contains the decoded instruction length; env->eip is its guest PC.
 * A kernel transition abandons every suspended LLVM frame. SYSRET/IRET
 * selects a fresh user entry from the resulting architectural state. */
static int gen_latxs_aot_native_helper(void *code_ptr, unsigned mtcc_id, bool atomic)
{
    const int frame_bytes = 16;
    latxs_tr_begin(code_ptr);
    IR2_OPND tp = la_new_gpr(2);
    IR2_OPND scratch = la_new_gpr(21);
    IR2_OPND returning = la_new_label();
    if (atomic) {
        /* Test before discarding the caller's RAS or LLVM stack. */
        la_out2i(LISA_SRLI_D, &scratch, &la_a0, 15);
        la_out2(LISA_BNEZ, &scratch, &returning);
    }
    la_out2i(LISA_ADDI_D, &tp, &la_zero, 0);
    la_out2i(LISA_STPTR_W, &la_zero, &la_env,
            offsetof(CPUX86State, ras_tp.top) >> 2);
    la_out2i(LISA_STPTR_W, &la_zero, &la_env,
            offsetof(CPUX86State, ras_tp.reset_pending) >> 2);
    aot_native_load_outer_sp(&la_sp);

    latxs_tr_dmw_in_fix_mem(&la_sp);
    /* Carve a private 16-byte frame below outer_sp without touching saved C GPRs. */
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, -frame_bytes);
    la_out2i(LISA_STPTR_D, &la_a0, &la_sp, 0);
    latxs_tr_dmw_out_fix_mem(&la_sp);
    latxs_tr_gen_call_to_helper_prologue_inline(default_helper_cfg);
#ifdef CONFIG_LATX_OPT_NOFP
    {
        /* Terminal entries may raise an exception before cs_native_to_bt_ret_0
         * or clobber caller-saved $f0-$f7 in C; flush MMX state first. */
        IR2_OPND done = la_new_label();
        la_out2i(LISA_LD_W, &scratch, &la_env, offsetof(CPUX86State, nofp));
        la_out2(LISA_BEQZ, &scratch, &done);
        for (unsigned n = 0; n < 8; n++) {
            IR2_OPND reg = latxs_ir2_opnd_new(IR2_OPND_FPR, n);
            la_out2i(LISA_FST_D, &reg, &la_env,
                    offsetof(CPUX86State, fpregs[n].d.low));
        }
        la_out2i(LISA_ST_W, &la_zero, &la_env, offsetof(CPUX86State, nofp));
        la_label(done);
    }
#endif
    la_out3(LISA_OR, &la_a0, &la_env, &la_zero);
    latxs_tr_dmw_out_fix_mem(&la_a0);
    la_out2i(LISA_LDPTR_D, &la_a1, &la_sp, 0);
    la_imm(&la_ra, atomic ? (ADDR)latx_helper_aot_atomic_request : (ADDR)helper_syscall);
    la_out2i(LISA_JIRL, &la_ra, &la_ra, 0);
    if (atomic) {
        /* Unmarked requests retain the terminal ABI used by older AOTs. */
        la_label(returning);
        gen_aot_atomic_fast_path();
        gen_aot_returning_helper_body(AOT_HELPER_ATOMIC);
        return latxs_tr_end(code_ptr);
    }
    latxs_tr_gen_call_to_helper_epilogue_inline(default_helper_cfg);
    latxs_tr_dmw_in_fix_mem(&la_sp);
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, frame_bytes);
    latxs_tr_dmw_out_fix_mem(&la_sp);

    uintptr_t target = GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0);
    uintptr_t pc = (uintptr_t)code_ptr + lsenv->tr_data->ir2_asm_nr * 4;
    int64_t delta = (int64_t)target - (int64_t)pc;
    g_assert((delta & 3) == 0 && delta / 4 == sextract64(delta / 4, 0, 26));
    la_outa(LISA_B, delta / 4);
    return latxs_tr_end(code_ptr);
}

/* A helper may return to the active LLVM frame. Its C frame preserves the
 * caller's return address and helper arguments while keeping live host FCSR. */
static void gen_aot_returning_helper_body(AotReturningHelper kind)
{
    bool operand = kind == AOT_HELPER_X87 || kind == AOT_HELPER_SYSTEM;
    const int frame_bytes = 32;

    latxs_tr_dmw_in_fix_mem(&la_sp);
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, -frame_bytes);
    la_out2i(LISA_ST_D, &la_ra, &la_sp, 8);
    la_out2i(LISA_ST_D, &la_a0, &la_sp, 0);
    if (operand) {
        la_out2i(LISA_ST_D, &la_a1, &la_sp, 16);
    }
    latxs_tr_dmw_out_fix_mem(&la_sp);
    latxs_tr_gen_call_to_helper_prologue_inline(default_helper_cfg);
#ifdef CONFIG_LATX_OPT_NOFP
    {
        IR2_OPND sync_done = la_new_label();
        la_out2i(LISA_LD_W, &la_a0, &la_env, offsetof(CPUX86State, nofp));
        la_out2i(LISA_ST_W, &la_a0, &la_sp, 24);
        la_out2(LISA_BEQZ, &la_a0, &sync_done);
        for (unsigned n = 0; n < 8; n++) {
            IR2_OPND reg = latxs_ir2_opnd_new(IR2_OPND_FPR, n);
            la_out2i(LISA_FST_D, &reg, &la_env,
                    offsetof(CPUX86State, fpregs[n].d.low));
        }
        la_out2i(LISA_ST_W, &la_zero, &la_env, offsetof(CPUX86State, nofp));
        la_label(sync_done);
    }
#endif
    la_out3(LISA_OR, &la_a0, &la_env, &la_zero);
    latxs_tr_dmw_out_fix_mem(&la_a0);
    la_out2i(LISA_LDPTR_D, &la_a1, &la_sp, operand ? 4 : 0);
    if (operand) {
        la_out2i(LISA_LDPTR_D, &la_a2, &la_sp, 0);
    }
    la_imm(&la_ra, kind == AOT_HELPER_ATOMIC ? (ADDR)latx_helper_aot_atomic
                   : kind == AOT_HELPER_X87  ? (ADDR)latx_helper_aot_x87
                   : kind == AOT_HELPER_SYSTEM
                       ? (ADDR)latx_helper_aot_system
                       : (ADDR)latx_helper_aot_cpu_query);
    la_out2i(LISA_JIRL, &la_ra, &la_ra, 0);
#ifdef CONFIG_LATX_OPT_NOFP
    {
        IR2_OPND restore_done = la_new_label();
        la_out2i(LISA_LD_W, &la_a0, &la_sp, 24);
        la_out2(LISA_BEQZ, &la_a0, &restore_done);
        for (unsigned n = 0; n < 8; n++) {
            IR2_OPND reg = latxs_ir2_opnd_new(IR2_OPND_FPR, n);
            la_out2i(LISA_FLD_D, &reg, &la_env,
                    offsetof(CPUX86State, fpregs[n].d.low));
        }
        la_out2i(LISA_ST_W, &la_a0, &la_env, offsetof(CPUX86State, nofp));
        la_label(restore_done);
    }
#endif
    latxs_tr_gen_call_to_helper_epilogue_inline(default_helper_cfg);
    latxs_tr_dmw_in_fix_mem(&la_sp);
    la_out2i(LISA_LD_D, &la_ra, &la_sp, 8);
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, frame_bytes);
    /* LLVM resumes with its private stack in the direct mapping. */
    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);
}

static int gen_latxs_aot_native_returning_helper(void *code_ptr,
                                                 AotReturningHelper kind)
{
    latxs_tr_begin(code_ptr);
    gen_aot_returning_helper_body(kind);
    return latxs_tr_end(code_ptr);
}
#endif

static int gen_latxs_sc_prologue(void *code_ptr)
{
    int i = 0;
    const int extra_space = 40;

    latxs_tr_begin(code_ptr);

    if (hamt_enabled()) { /* hamt swtich into DMW mode */
        /* The caller can still be at guest PLV3; CSR operations need PLV0. */
        latxs_tr_dmw_in_plv0_break();
        latxs_tr_dmw_in_fix_mem(&la_sp);
        latxs_tr_dmw_in_fix_mem(&la_a0);
        latxs_tr_dmw_in_fix_mem(&la_a1);
        latxs_tr_dmw_in_fix_asid_tmp(&la_a1);
        latxs_tr_dmw_in_pg_tmp();
        latxs_tr_dmw_in_plv_env_tmp(&la_a1);
    }

    /* 1. save DBT's context in stack, so allocate space on the stack */
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, -256);

    /* 1.1 save callee-saved registers. s0-s7 ($23-$31) */
    IR2_OPND reg = la_inv;
    for (i = 0; i <= 8; ++i) {
        reg = la_new_gpr(i + 23);
        la_out2i(LISA_ST_D, &reg, &la_sp, extra_space + (i << 3));
    }

    /* 1.2 save fp($22), and ra($1) */
    la_out2i(LISA_ST_D, &la_fp, &la_sp, extra_space + 72);
    la_out2i(LISA_ST_D, &la_ra, &la_sp, extra_space + 80);

    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&la_sp);
    }

    /* 2. set up native context */
    la_out3(LISA_OR, &la_env, &la_a1, &la_zero);
    if (aot_native_outer_sp_enabled()) {
        size_t off = offsetof(CPUX86State, aot_native_outer_sp);
        g_assert(!(off & 3) && off <= 32764);
        la_out2i(LISA_STPTR_D, &la_sp, &la_env, off >> 2);
    }
    if (option_rastoptp) {
        IR2_OPND tp = latxs_ir2_opnd_new(IR2_OPND_GPR, 2);
        la_out2i(LISA_STPTR_D, &tp, &la_env,
                offsetof(CPUX86State, ras_tp.native_tp) >> 2);
    }

    /* 2.1 load x86 mapping registers */
    int load_top = !option_soft_fpu;
#ifdef CONFIG_LATX_OPT_NOFP
    latxs_tr_load_registers_from_env(0xffffffff, 0x00, load_top,
                                     0x0, 0x00);
#else
    if (option_soft_fpu_fast) {
        latxs_tr_load_registers_from_env(0xffffffff, 0, load_top,
                                         0xffffffff, 0x00);

        IR2_OPND mode_fpu = la_new_itmp();
        IR2_OPND label_fpu = la_new_label();

        la_out2i(LISA_LD_BU, &mode_fpu, &la_env, lsenv_offset_of_mode_fpu(lsenv));
        la_out3(LISA_BNE, &mode_fpu, &la_zero, &label_fpu);

        latxs_tr_load_fprs_from_env(0xff, load_top);

        la_label(label_fpu);
        la_tmp_free(mode_fpu);
    } else {
        latxs_tr_load_registers_from_env(0xffffffff, 0xff, load_top,
                                         0xffffffff, 0x00);
    }
#endif

#ifndef CONFIG_LATX_OPT_EFLAGS
    latxs_tr_load_eflags();
#endif
 
    latxs_ras_tp_leave_c();

    /* 3. jump to native code address (saved in a0) */
    la_out2i(LISA_JIRL, &la_zero, &la_a0, 0);

    la_out_nop();

    i = latxs_tr_end(code_ptr);
    return i;
}

static int gen_latxs_sc_epilogue(void *code_ptr)
{
    int i = 0;
    const int extra_space = 40;

    latxs_tr_begin(code_ptr);

    /*
     * by default set v0 to zero.
     * sc_table.cs_native_to_bt_ret_0 points here
     */
    IR2_OPND label_epi = la_new_label();
    IR2_OPND jump_cold = la_new_label();
    bool keep_native_frame = false;
#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
    keep_native_frame = aot_native_outer_sp_enabled() && option_soft_ras &&
        hamt_mode_dmw() && option_int_opt == IRQ_CHECK_ASYNC_NOTIFY;
#endif
    la_out3(LISA_OR, &la_v0, &la_zero, &la_zero);
    la_out1(LISA_B, &label_epi);

    /*
     * 1. set return value from a6
     * sc_table.cs_native_to_bt_ret_n3
     * sc_table.cs_native_to_bt_ret_n1
     * sc_table.cs_native_to_bt_ret_n0
     */
    IR2_OPND tb_ptr_opnd = latxs_ra_alloc_dbt_arg1();
    la_out2i(LISA_ORI, &la_v0, &tb_ptr_opnd, 3); // n3
    la_out1(LISA_B, &label_epi);
    la_out2i(LISA_ORI, &la_v0, &tb_ptr_opnd, 1); // n1
    la_out1(LISA_B, keep_native_frame ? &jump_cold : &label_epi);
    la_out2i(LISA_ORI, &la_v0, &tb_ptr_opnd, 0); // n0
    /*
     *  0:  or   a0, zero, zero  // ret_0
     *  4:  b    6
     *  8:  ori  a0, tb, 3     // ret_n3
     * 12:  b    4
     * 16:  ori  a0, tb, 1     // ret_n1
     * 20:  b    2
     * 24:  ori  a0, tb, 0     // ret_n0
     * ------------------------
     * epilogue context switch
     * ------------------------
     */

    la_label_name(label_epi, "epilogue : native to bt");
    latxs_ras_tp_enter_c();
    if (aot_native_outer_sp_enabled()) {
        IR2_OPND saved_sp = la_new_gpr(21);
        IR2_OPND done = la_new_label();
        aot_native_load_outer_sp(&saved_sp);
        la_out3(LISA_BEQ, &la_sp, &saved_sp, &done);
        /* A pending LLVM continuation cannot survive abandoning its frame. */
        la_out2i(LISA_STPTR_W, &la_zero, &la_env,
                offsetof(CPUX86State, ras_tp.top) >> 2);
        la_out3(LISA_OR, &la_sp, &saved_sp, &la_zero);
        la_label(done);
    }
    if (hamt_enabled()) latxs_tr_dmw_out_fix_mem(&la_v0);
    /* 3. save x86 mapping registers */
    int save_top = (!option_soft_fpu) ? 1 : 0;
#ifdef CONFIG_LATX_OPT_NOFP
    latxs_tr_save_registers_to_env(0xffffffff, 0x00, save_top,
                                   0x0, 0x00);
    /* Save physical MMX values before any C dispatcher code can use FPRs. */
    latxs_gen_nofp_sync_fprs_to_env();
#else
    if (option_soft_fpu_fast) {
        latxs_tr_save_registers_to_env(0xffffffff, 0, save_top,
                                       0xffffffff, 0x00);

        IR2_OPND mode_fpu = la_new_itmp();
        IR2_OPND label_fpu = la_new_label();

        la_out2i(LISA_LD_BU, &mode_fpu, &la_env, lsenv_offset_of_mode_fpu(lsenv));
        la_out3(LISA_BNE, &mode_fpu, &la_zero, &label_fpu);

        latxs_tr_save_fprs_to_env(0xff, save_top);

        la_label(label_fpu);
        la_tmp_free(mode_fpu);
    } else {
        latxs_tr_save_registers_to_env(0xffffffff, 0xff, save_top,
                                       0xffffffff, 0x00);
    }
#endif

#ifndef CONFIG_LATX_OPT_EFLAGS
    latxs_tr_save_eflags();
#endif

    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(&la_sp);
        latxs_tr_dmw_in_plv0_break();
    }

    /* 4. restore bt's context */
    /* 4.1 restore fp($22) and ra($1) */
    la_out2i(LISA_LD_D, &la_ra, &la_sp, extra_space + 80);
    la_out2i(LISA_LD_D, &la_fp, &la_sp, extra_space + 72);
    /* 4.2 restore callee-saved registers. s0-s7 ($16-$23) */
    IR2_OPND reg = la_inv;
    for (i = 0; i <= 8; ++i) {
        reg = la_new_gpr(i + 23);
        la_out2i(LISA_LD_D, &reg, &la_sp, extra_space + (i << 3));
    }
    /* 4.4 restore sp */
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, 256);

    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&la_sp);
        latxs_tr_dmw_in_da_tmp();
        latxs_tr_dmw_out_fix_asid(NULL);
    }

    /* 5 return */
    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);

#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
    if (keep_native_frame) {
        const int jump_frame_bytes = 48;
        IR2_OPND tp = la_new_gpr(2);
        IR2_OPND scratch = la_new_gpr(21);
        IR2_OPND pending = la_new_gpr(20);
        IR2_OPND failed = la_new_label();

        /* Only an unlinked ordinary edge inside an LLVM call needs this
         * cold lookup. Linked branches and the fixed entry offsets stay
         * unchanged; all other dispatcher exits keep their old behavior. */
        la_label(jump_cold);
        aot_native_load_outer_sp(&scratch);
        la_out3(LISA_BEQ, &la_sp, &scratch, &label_epi);

        latxs_tr_dmw_in_fix_mem(&la_sp);
        la_out2i(LISA_ADDI_D, &la_sp, &la_sp, -jump_frame_bytes);
        la_out2i(LISA_STPTR_D, &la_ra, &la_sp, 0);
        la_out2i(LISA_STPTR_D, &la_v0, &la_sp, 2);
        la_out2i(LISA_STPTR_D, &tp, &la_sp, 4);
        latxs_tr_dmw_out_fix_mem(&la_sp);

        /* A JMP preserves flags as well as all mapped GPRs and XMMs.
         * Save ra separately: unlike a CALL, this edge must not link r1. */
        latxs_tr_gen_call_to_helper_prologue_inline(default_helper_cfg);
#ifdef CONFIG_LATX_OPT_NOFP
        {
            IR2_OPND sync_done = la_new_label();
            la_out2i(LISA_LD_W, &la_a0, &la_env, offsetof(CPUX86State, nofp));
            la_out2i(LISA_ST_W, &la_a0, &la_sp, 32);
            la_out2(LISA_BEQZ, &la_a0, &sync_done);
            for (unsigned n = 0; n < 8; n++) {
                IR2_OPND fpr = latxs_ir2_opnd_new(IR2_OPND_FPR, n);
                la_out2i(LISA_FST_D, &fpr, &la_env,
                        offsetof(CPUX86State, fpregs[n].d.low));
            }
            la_out2i(LISA_ST_W, &la_zero, &la_env, offsetof(CPUX86State, nofp));
            la_label(sync_done);
        }
#endif
        la_out3(LISA_OR, &la_a0, &la_env, &la_zero);
        latxs_tr_dmw_out_fix_mem(&la_a0);
        la_out2i(LISA_LDPTR_D, &la_a1, &la_sp, 2);
        la_imm(&la_ra, (ADDR)latx_helper_aot_ordinary_jump);
        la_out2i(LISA_JIRL, &la_ra, &la_ra, 0);
        la_out2i(LISA_STPTR_D, &la_a0, &la_sp, 6);
#ifdef CONFIG_LATX_OPT_NOFP
        {
            IR2_OPND restore_done = la_new_label();
            la_out2i(LISA_LD_W, &la_a0, &la_sp, 32);
            la_out2(LISA_BEQZ, &la_a0, &restore_done);
            for (unsigned n = 0; n < 8; n++) {
                IR2_OPND fpr = latxs_ir2_opnd_new(IR2_OPND_FPR, n);
                la_out2i(LISA_FLD_D, &fpr, &la_env,
                        offsetof(CPUX86State, fpregs[n].d.low));
            }
            la_out2i(LISA_ST_W, &la_a0, &la_env, offsetof(CPUX86State, nofp));
            la_label(restore_done);
        }
#endif
        latxs_tr_gen_call_to_helper_epilogue_inline(default_helper_cfg);

        latxs_tr_dmw_in_fix_mem(&la_sp);
        la_out2i(LISA_LDPTR_D, &la_ra, &la_sp, 0);
        la_out2i(LISA_LDPTR_D, &la_a0, &la_sp, 6);
        la_out2i(LISA_LDPTR_D, &scratch, &la_sp, 4);
        la_out2(LISA_BEQZ, &la_a0, &failed);
        la_out3(LISA_BNE, &tp, &scratch, &failed);
        la_out2i(LISA_LDPTR_W, &pending, &la_env,
                offsetof(CPUX86State, ras_tp.reset_pending) >> 2);
        la_out2(LISA_BNEZ, &pending, &failed);
        la_out2i(LISA_ADDI_D, &la_sp, &la_sp, jump_frame_bytes);
        latxs_tr_dmw_out_fix_mem(&la_sp);
        latxs_tr_dmw_in_fix_mem(&la_a0);
        la_out2i(LISA_JIRL, &la_zero, &la_a0, 0);

        la_label(failed);
        la_out2i(LISA_LDPTR_D, &la_v0, &la_sp, 2);
        la_out2i(LISA_ADDI_D, &la_sp, &la_sp, jump_frame_bytes);
        latxs_tr_dmw_out_fix_mem(&la_sp);
        la_out1(LISA_B, &label_epi);
    }
#endif

    i = latxs_tr_end(code_ptr);
    return i;
}

static int gen_latxs_intb_fsht_direct_tail_lookup_tucpfast(void *code_ptr,
                                                           int fid)
{
    latxs_tr_begin(code_ptr);
    IR2_OPND hit_softras = la_new_label();
    IR2_OPND hit_jump = la_new_label();
    IR2_OPND fallback = la_new_label();
    IR2_OPND skip_push = la_new_label();
    IR2_OPND tp = la_new_gpr(2);

    if (hamt_enabled()) {
        la_out2i(LISA_LU52I_D, &la_sp, &la_sp, 0);
    }
    if (intb_njc_enabled()) {
        latxs_out_jmp_cache_hash(&la_a1, &la_a5, &la_a4);
        la_out2i(LISA_LD_D, &la_a3, &la_env,
                 lsenv_offset_of_latx_njc_cache_ptr(lsenv));
        la_out2i(LISA_LD_WU, &la_a6, &la_env, offsetof(CPUX86State, hflags));
        la_out2i(LISA_SLLI_D, &la_a4, &la_a4, 5);
        la_out3(LISA_ADD_D, &la_a3, &la_a4, &la_a3);
        la_out2i(LISA_LD_D, &la_a2, &la_a3,
                 offsetof(LatxNJCEntry, call_tcptr));
        la_out2i(LISA_LD_D, &la_a5, &la_a3, offsetof(LatxNJCEntry, pc));
        la_out2i(LISA_LD_D, &la_a4, &la_a3, offsetof(LatxNJCEntry, flags));
        la_out2ii(LISA_BSTRPICK_D, &la_a7, &la_a4, 31, 0);
        la_out2i(LISA_SRLI_D, &la_a4, &la_a4, 48);
        la_out3(LISA_XOR, &la_a5, &la_a5, &la_a1);
        la_out3(LISA_XOR, &la_a7, &la_a7, &la_a6);
        la_out3(LISA_OR, &la_a5, &la_a5, &la_a7);
        la_out3(LISA_MASKNEZ, &la_a5, &la_a2, &la_a5);
        la_out2(LISA_BEQZ, &la_a5, &fallback);
        la_out2(LISA_BEQZ, &la_a4, &hit_softras);
        la_out2i(LISA_JIRL, &la_zero, &la_a2, 8);

        la_label(hit_softras);
        la_out2(LISA_BEQZ, &la_a0, &hit_jump);
        la_out2i(LISA_ANDI, &la_a5, &tp, LATX_RAS_TP_SIZE - 1);
        la_out3i(LISA_ALSL_D, &la_a3, &la_a5, &la_env, 3);
        la_out2i(LISA_STPTR_D, &la_a0, &la_a3,
                 offsetof(CPUX86State, ras_tp.stack) >> 2);
        la_out2i(LISA_STPTR_D, &la_ra, &la_a3,
                 (offsetof(CPUX86State, ras_tp.stack) + 8) >> 2);
        la_out2i(LISA_ADDI_D, &tp, &la_a5, 1);
        la_label(hit_jump);
        la_out2i(LISA_JIRL, &la_zero, &la_a2, 0);
    }

    la_label(fallback);
    la_out2(LISA_BEQZ, &la_a0, &skip_push);
    la_out2i(LISA_ANDI, &la_a2, &tp, LATX_RAS_TP_SIZE - 1);
    la_out3i(LISA_ALSL_D, &la_a3, &la_a2, &la_env, 3);
    la_out2i(LISA_STPTR_D, &la_a0, &la_a3,
             offsetof(CPUX86State, ras_tp.stack) >> 2);
    la_out2i(LISA_STPTR_D, &la_ra, &la_a3,
             (offsetof(CPUX86State, ras_tp.stack) + 8) >> 2);
    la_out2i(LISA_ADDI_D, &tp, &la_a2, 1);

    la_label(skip_push);
    la_out_nop();
    int nr = latxs_tr_end(code_ptr);
    uint32_t *insns = (uint32_t *)code_ptr;
    uintptr_t target = GET_SC_TABLE(lsenv->tr_data->mtcc_id,
                                    intb_fsht_call_lookup_tucpfast[fid]);
    int64_t delta = (int64_t)target - (int64_t)(uintptr_t)&insns[nr - 1];
    int64_t off = delta >> 2;
    g_assert((delta & 3) == 0 && off == sextract64(off, 0, 26));
    insns[nr - 1] = 0x50000000U |
                    (((uint32_t)(off >> 16)) & 0x3ffU) |
                    ((((uint32_t)off) << 10) & 0x3fffc00U);
    return nr;
}

int target_latxs_static_codes(void *code_base, uint32_t mtcc_id)
{
    int code_nr = 0;
    int code_nr_all = 0;
    void *code_ptr = code_base;
    uint64_t code_aln = 0;

    latxs_set_lsenv_tmp(mtcc_id);

#ifdef CONFIG_LATX_PRINT
    printf("%s code %p mtcc %d\n", __func__, code_base, mtcc_id);
#endif

#define LATXS_GEN_STATIC_CODES(name, genfn, ...) do {   \
    code_nr = genfn(__VA_ARGS__);                       \
    code_nr_all += code_nr;                             \
    latx_perfmap_insert(code_ptr, code_nr << 2, \
            mtcc_id ? name "_R1" : name "_R0");  \
    code_ptr = code_base + (code_nr_all << 2);          \
    code_aln = (uint64_t)code_ptr;  \
    if (code_aln & 0x3f) {  \
        code_aln += 64;     \
        code_aln &= ~0x3f;  \
        code_nr_all += (code_aln - (uint64_t)code_ptr) >> 2;    \
        code_ptr = (void*)code_aln; \
    }   \
} while (0)

    /* epilogue */
    SET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0, (ADDR)code_ptr);
    SET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n3, (ADDR)code_ptr + 8);
    SET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n1, (ADDR)code_ptr + 16);
    SET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n0, (ADDR)code_ptr + 24);
    /*
     *  0:  or   a0, zero, zero  // ret_0
     *  4:  b    3
     *  8:  ori  a0, tb, 3     // ret_n3
     * 12:  b    3
     * 16:  ori  a0, tb, 1     // ret_n1
     * 20:  b    3
     * 24:  ori  a0, tb, 0     // ret_n0
     * ------------------------
     * epilogue context switch
     * ------------------------
     */
    LATXS_GEN_STATIC_CODES("latxs_epilogue",
            gen_latxs_sc_epilogue, code_ptr);
    LATXS_DUMP_STATIC_CODES_INFO("latxs epilogue ret_0: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0));
    LATXS_DUMP_STATIC_CODES_INFO("latxs epilogue ret_n3: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n3));
    LATXS_DUMP_STATIC_CODES_INFO("latxs epilogue ret_n1: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n1));
    LATXS_DUMP_STATIC_CODES_INFO("latxs epilogue ret_n0: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_n0));

    /* prologue */
    /*
     * For signal interrupt optimization, we need to check interrupt
     * pending before jump to TB's native codes. If there is an
     * interrupt pending, we should jump to epilogue to return
     * back to BT context. So we need to know the address of
     * context switch native to bt here.
     */
    SET_SC_TABLE(mtcc_id, cs_bt_to_native, (ADDR)code_ptr);
    LATXS_GEN_STATIC_CODES("latxs_prologue",
            gen_latxs_sc_prologue, code_ptr);
    LATXS_DUMP_STATIC_CODES_INFO("latxs prologue: %p\n",
            (void *)GET_SC_TABLE(mtcc_id, cs_bt_to_native));

    if (option_rastoptp && !intb_link_enable()) {
        SET_SC_TABLE(mtcc_id, ras_tp_fallback, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_ras_tp_fallback",
                gen_latxs_ras_tp_fallback, code_ptr, mtcc_id);
    }

#ifdef CONFIG_SOFTMMU
    if (aot_native_outer_sp_enabled()) {
        SET_SC_TABLE(mtcc_id, aot_native_syscall, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_aot_native_syscall",
                gen_latxs_aot_native_helper, code_ptr, mtcc_id, false);
        SET_SC_TABLE(mtcc_id, aot_native_atomic, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_aot_native_atomic",
                gen_latxs_aot_native_helper, code_ptr, mtcc_id, true);
        SET_SC_TABLE(mtcc_id, aot_native_cpu_query, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_aot_native_cpu_query",
                               gen_latxs_aot_native_returning_helper, code_ptr,
                               AOT_HELPER_CPU);
        SET_SC_TABLE(mtcc_id, aot_native_x87, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_aot_native_x87",
                               gen_latxs_aot_native_returning_helper, code_ptr,
                               AOT_HELPER_X87);
        SET_SC_TABLE(mtcc_id, aot_native_system, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_aot_native_system",
                               gen_latxs_aot_native_returning_helper, code_ptr,
                               AOT_HELPER_SYSTEM);
    }
#endif

    if (scs_enabled()) {
        SET_SC_TABLE(mtcc_id, scs_prologue, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_static_prologue",
                gen_latxs_scs_prologue_cfg, code_ptr, default_helper_cfg);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs SCS: static CS prologue %p\n",
                (void *)GET_SC_TABLE(mtcc_id, scs_prologue));
        
        SET_SC_TABLE(mtcc_id, scs_prologue_nofp, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_static_prologue_nofp",
                gen_latxs_scs_prologue_cfg_nofp, code_ptr, nofp_helper_cfg);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs SCS: static CS prologue nofp %p\n",
                (void *)GET_SC_TABLE(mtcc_id, scs_prologue_nofp));

        SET_SC_TABLE(mtcc_id, scs_epilogue, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_static_epilogue",
                gen_latxs_scs_epilogue_cfg, code_ptr, default_helper_cfg);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs SCS: static CS epilogue %p\n",
                (void *)GET_SC_TABLE(mtcc_id, scs_epilogue));
        
        SET_SC_TABLE(mtcc_id, scs_epilogue_nofp, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_static_epilogue_nofp",
                gen_latxs_scs_epilogue_cfg_nofp, code_ptr, nofp_helper_cfg);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs SCS: static CS epilogue nofp %p\n",
                (void *)GET_SC_TABLE(mtcc_id, scs_epilogue_nofp));
    }

    if (option_rastoptp) {
        SET_SC_TABLE(mtcc_id, ras_tp_repair, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_ras_tp_repair",
                gen_latxs_ras_tp_repair, code_ptr);
    }

    if (intb_link_enable()) {
        SET_SC_TABLE(mtcc_id, intb_lookup, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_intb_lookup",
                gen_latxs_intb_lookup, code_ptr);
        LATXS_DUMP_STATIC_CODES_INFO(
                "latxs intb lookup %p\n",
                (void *)GET_SC_TABLE(mtcc_id, intb_lookup));

        if (option_rastoptp) {
            if (aot_native_outer_sp_enabled()) {
                SET_SC_TABLE(mtcc_id, intb_resume_lookup, (ADDR)code_ptr);
                LATXS_GEN_STATIC_CODES("latxs_intb_resume_lookup",
                        gen_latxs_intb_resume_lookup, code_ptr);
            }
            SET_SC_TABLE(mtcc_id, ras_tp_fallback, (ADDR)code_ptr);
            LATXS_GEN_STATIC_CODES("latxs_ras_tp_fallback",
                    gen_latxs_ras_tp_fallback, code_ptr, mtcc_id);
        }

        if (latx_jmp_cache_compact_enabled() &&
            !intb_njc_inline_enabled()) {
            SET_SC_TABLE(mtcc_id, intb_lookup_compact, (ADDR)code_ptr);
            LATXS_GEN_STATIC_CODES("latxs_intb_lookup_compact",
                    gen_latxs_intb_lookup_compact, code_ptr);
            LATXS_DUMP_STATIC_CODES_INFO(
                    "latxs compact intb lookup %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, intb_lookup_compact));
        } else {
            SET_SC_TABLE(mtcc_id, intb_lookup_compact,
                    GET_SC_TABLE(mtcc_id, intb_lookup));
        }

        if (option_flaged_sht) {
            int index = 0;
            uint32_t flags = 0;
            FSHT_FOREACH_FLAGS(index, flags) {
                int do_gen_fsht = 0;

                if (qemu_tcg_mtcc_enabled()) {
                    if ((mtcc_id == 1 && (flags & 0x3) == 3) ||
                        (mtcc_id == 0 && (flags & 0x3) == 0))
                    {
                        do_gen_fsht = 1;
                    }
                } else {
                    do_gen_fsht = 1;
                }

                if (do_gen_fsht) {
#if defined(CONFIG_LATX_PRINT)
                    printf("%s:%d mtcc %d intb fsht %d lookup for flags %x\n",
                            __func__, __LINE__,
                            mtcc_id, index, flags);
#endif
                    SET_SC_TABLE(mtcc_id, intb_fsht_lookup[index], (ADDR)code_ptr);
                    LATXS_GEN_STATIC_CODES("latxs_intb_fsht_lookup",
                            gen_latxs_intb_fsht_lookup, code_ptr, index);
                    LATXS_DUMP_STATIC_CODES_INFO(
                            "latxs intb fsht %d lookup %p\n",
                            index,
                            (void *)GET_SC_TABLE(mtcc_id, intb_fsht_lookup[index]));

                }
            }
        }
    }

#ifdef CONFIG_SOFTMMU
    if (cp_enabled() || cp_tb_enabled()) {
        int have_user = 0;
        int have_knosmap = 0;
        if (qemu_tcg_mtcc_enabled()) {
            if (mtcc_id == 0) have_knosmap = 1;
            if (mtcc_id == 1) have_user = 1;
        } else {
            have_knosmap = 1;
            have_user = 1;
        }

        gen_cp_check_func cp_fn = gen_latxs_cross_page_check;

#define CP_INTCHECK_SIZE    8

        if (have_user) {
            SET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n0_p01_mmu_user",
                    cp_fn, code_ptr, 0, MMU_USER_IDX, 3, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n0_p01_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n1_p01_mmu_user",
                    cp_fn, code_ptr, 1, MMU_USER_IDX, 3, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n1_p01_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n0_p0_mmu_user",
                    cp_fn, code_ptr, 0, MMU_USER_IDX, 1, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n0_p0_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n1_p0_mmu_user",
                    cp_fn, code_ptr, 1, MMU_USER_IDX, 1, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n1_p0_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_user));
        }

        if (have_user && option_fast_itlb_ptw) {
            SET_SC_TABLE(mtcc_id, cp_check_ptw_user, (ADDR)code_ptr);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_user",
                    gen_latxs_cross_page_check_ptw, code_ptr, MMU_USER_IDX);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_user));

            if (tucp_fast_direct_check() || tucp_fast_fsht_check()) {
                SET_SC_TABLE(mtcc_id, tu_cp_check_n0_mmu_user_intcheck,
                             (ADDR)code_ptr);
                SET_SC_TABLE(mtcc_id, tu_cp_check_n0_mmu_user,
                             (ADDR)code_ptr + CP_INTCHECK_SIZE);
                LATXS_GEN_STATIC_CODES("tu_cp_check_n0_mmu_user",
                        gen_latxs_tu_cross_page_check, code_ptr, 0);

                SET_SC_TABLE(mtcc_id, tu_cp_check_n1_mmu_user_intcheck,
                             (ADDR)code_ptr);
                SET_SC_TABLE(mtcc_id, tu_cp_check_n1_mmu_user,
                             (ADDR)code_ptr + CP_INTCHECK_SIZE);
                LATXS_GEN_STATIC_CODES("tu_cp_check_n1_mmu_user",
                        gen_latxs_tu_cross_page_check, code_ptr, 1);

                SET_SC_TABLE(mtcc_id, tu_cp_check_ptr_mmu_user,
                             (ADDR)code_ptr);
                LATXS_GEN_STATIC_CODES("tu_cp_check_ptr_mmu_user",
                        gen_latxs_tu_cross_page_check_ptr, code_ptr);
            }

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n0_p01_mmu_user",
                    cp_fn, code_ptr, 0, MMU_USER_IDX, 3, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n0_p01_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n1_p01_mmu_user",
                    cp_fn, code_ptr, 1, MMU_USER_IDX, 3, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n1_p01_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n0_p0_mmu_user",
                    cp_fn, code_ptr, 0, MMU_USER_IDX, 1, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n0_p0_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_user));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_user_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_user, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n1_p0_mmu_user",
                    cp_fn, code_ptr, 1, MMU_USER_IDX, 1, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n1_p0_mmu_user %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_user));
        }

        if (have_knosmap) {
            SET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n0_p01_mmu_knosmap",
                    cp_fn, code_ptr, 0, MMU_KNOSMAP_IDX, 3, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n0_p01_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n0_p01_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n1_p01_mmu_knosmap",
                    cp_fn, code_ptr, 1, MMU_KNOSMAP_IDX, 3, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n1_p01_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n1_p01_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n0_p0_mmu_knosmap",
                    cp_fn, code_ptr, 0, MMU_KNOSMAP_IDX, 1, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n0_p0_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n0_p0_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_n1_p0_mmu_knosmap",
                    cp_fn, code_ptr, 1, MMU_KNOSMAP_IDX, 1, 0);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_n1_p0_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_n1_p0_mmu_knosmap));
        }

        if (have_knosmap && option_fast_itlb_ptw) {
            SET_SC_TABLE(mtcc_id, cp_check_ptw_knosmap, (ADDR)code_ptr);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_knosmap",
                    gen_latxs_cross_page_check_ptw, code_ptr, MMU_KNOSMAP_IDX);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n0_p01_mmu_knosmap",
                    cp_fn, code_ptr, 0, MMU_KNOSMAP_IDX, 3, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n0_p01_mmu_knosmap%p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p01_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n1_p01_mmu_knosmap",
                    cp_fn, code_ptr, 1, MMU_KNOSMAP_IDX, 3, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n1_p01_mmu_knosmap%p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p01_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n0_p0_mmu_knosmap",
                    cp_fn, code_ptr, 0, MMU_KNOSMAP_IDX, 1, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n0_p0_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n0_p0_mmu_knosmap));

            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_knosmap_intcheck, (ADDR)code_ptr);
            SET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_knosmap, (ADDR)code_ptr + CP_INTCHECK_SIZE);
            LATXS_GEN_STATIC_CODES("cp_check_ptw_n1_p0_mmu_knosmap",
                    cp_fn, code_ptr, 1, MMU_KNOSMAP_IDX, 1, 1);
            LATXS_DUMP_STATIC_CODES_INFO("cp_check_ptw_n1_p0_mmu_knosmap %p\n",
                    (void *)GET_SC_TABLE(mtcc_id, cp_check_ptw_n1_p0_mmu_knosmap));
        }
    }
#endif

    if (intb_link_enable() && option_flaged_sht && tucp_fast_fsht_check() &&
        (!qemu_tcg_mtcc_enabled() || mtcc_id == 1)) {
        int index = 0;
        uint32_t flags = 0;

        FSHT_FOREACH_FLAGS(index, flags) {
            if ((flags & HF_CPL_MASK) == HF_CPL_MASK) {
                SET_SC_TABLE(mtcc_id, intb_fsht_lookup_tucpfast[index],
                             (ADDR)code_ptr);
                LATXS_GEN_STATIC_CODES("latxs_intb_fsht_lookup_tucpfast",
                        gen_latxs_intb_fsht_lookup_tucpfast, code_ptr, index);
                if (latx_jmp_cache_compact_enabled() && (flags & HF_CS64_MASK)) {
                    SET_SC_TABLE(mtcc_id, intb_fsht_call_lookup_tucpfast[index],
                                 (ADDR)code_ptr);
                    LATXS_GEN_STATIC_CODES("latxs_intb_fsht_call_lookup_tucpfast",
                            gen_latxs_intb_fsht_call_lookup_tucpfast, code_ptr, index);
                    SET_SC_TABLE(mtcc_id,
                                 intb_fsht_direct_tail_lookup_tucpfast[index],
                                 (ADDR)code_ptr);
                    LATXS_GEN_STATIC_CODES(
                            "latxs_intb_fsht_direct_tail_lookup_tucpfast",
                            gen_latxs_intb_fsht_direct_tail_lookup_tucpfast,
                            code_ptr, index);
                }
            }
        }
    }

    if (option_njc_source_jump) {
        SET_SC_TABLE(mtcc_id, intb_njc_return, (ADDR)code_ptr);
        LATXS_GEN_STATIC_CODES("latxs_intb_njc_return",
                gen_latxs_intb_njc_return, code_ptr);
    }

    latx_perfmap_flush();

#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    /* leave space for TBMini for the 1st TB */
    if (option_split_tb) {
        code_nr_all += qemu_icache_linesize / 4;
    }
#endif
    return code_nr_all;
}
