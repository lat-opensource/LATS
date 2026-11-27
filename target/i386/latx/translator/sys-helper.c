/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "latx-ras.h"
#include "translate.h"
#include "fpu/softfloat.h"
#include "latx-static-codes.h"
#include "latx-sys-hamt-dmw.h"
#include "latxs-imm-cache.h"
#include "sys-aot.h"
#include "ir2.h"

helper_cfg_t all_helper_cfg = {
    .cfg.sv_allgpr = 1, .cfg.sv_eflags = 1, .cfg.cvt_fp80 = 1 };
helper_cfg_t zero_helper_cfg = {
    .cfg.sv_allgpr = 0, .cfg.sv_eflags = 0, .cfg.cvt_fp80 = 0 };
helper_cfg_t default_helper_cfg = {
    .cfg.sv_allgpr = 1, .cfg.sv_eflags = 1, .cfg.cvt_fp80 = 0};
helper_cfg_t nofp_helper_cfg = {
    .cfg.sv_allgpr = 0, .cfg.sv_gpr = 1, .cfg.sv_fpr = 0,
    .cfg.sv_simd = 1, .cfg.sv_eflags = 1, .cfg.cvt_fp80 = 0};
helper_cfg_t fplib_helper_cfg = {
    .cfg.sv_allgpr = 0,
    .cfg.sv_gpr = 0, .cfg.sv_fpr = 1, .cfg.sv_simd = 1,
    .cfg.sv_eflags = 1, .cfg.cvt_fp80 = 0};

/*
 * return 1: cfg1 == cfg2
 * return 0: cfg1 != cfg2
 */
int cmp_helper_cfg(helper_cfg_t cfg1, helper_cfg_t cfg2)
{
    return cfg1.data == cfg2.data;
}

IR2_INST *latxs_tr_gen_call_to_helper(ADDR func_addr, int helper_kind)
{
#ifdef LATXS_IMM_CACHE_ENABLE
    if (can_do_imm() && (option_imm_rip || option_imm_pattern)) {
        imm_cache_free_all();
    }
#endif
    latxs_load_addr_to_ir2(&latxs_ra_ir2_opnd, (ADDR)func_addr, helper_kind);
    latxs_append_ir2_opnd1_(lisa_call, &latxs_ra_ir2_opnd);
    return NULL;
}

void latxs_convert_fpregs_64_to_x80(void)
{
    int i;
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    float_status s = env->fp_status;
    for (i = 0; i < 8; i++) {
        FPReg *p = &(env->fpregs[i]);
        p->d = float64_to_floatx80((float64)p->d.low, &s);
    }
}

void latxs_convert_fpregs_x80_to_64(void)
{
    int i;
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    float_status s = env->fp_status;
    for (i = 0; i < 8; i++) {
        FPReg *p = &(env->fpregs[i]);
        p->d.low = (uint64_t)floatx80_to_float64(p->d, &s);
        p->d.high = 0;
    }
}

void latxs_tr_cvt_fp64_to_80(void)
{
    lsassert(!option_soft_fpu);
    /* 1. call convert_fpregs_x64_to_80 */
    IR2_OPND func_addr_opnd = latxs_ra_alloc_itemp();
    latxs_load_addr_to_ir2(&func_addr_opnd,
                           (ADDR)latxs_convert_fpregs_64_to_x80,
                           LATXS_CONVERT_FPREGS_64_TO_X80);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_da_tmp();
        latxs_tr_dmw_out_fix_asid(NULL);
    }
    latxs_append_ir2_opnd1_(lisa_call, &func_addr_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_asid(&func_addr_opnd);
        latxs_tr_dmw_in_pg_tmp();
    }

    /* 2. call update_fp_status */
    latxs_load_addr_to_ir2(&func_addr_opnd, (ADDR)update_fp_status,
            UPDATE_FP_STATUS);
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        latxs_tr_dmw_in_da_tmp();
        latxs_tr_dmw_out_fix_asid(NULL);
    }
    latxs_append_ir2_opnd1_(lisa_call, &func_addr_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_asid(&func_addr_opnd);
        latxs_tr_dmw_in_pg_tmp();
    }
    latxs_ra_free_temp(&func_addr_opnd);
}

void latxs_tr_cvt_fp80_to_64(void)
{
    lsassert(!option_soft_fpu);
    /* 1. save return value */
    latxs_append_ir2_opnd2i(LISA_ST_D, &latxs_ret0_ir2_opnd,
                            &latxs_env_ir2_opnd,
                            lsenv_offset_of_save_regs(lsenv, 0x2));

    /* 2. call convert_fpregs_x80_to_64 */
    IR2_OPND func_addr_opnd = latxs_ra_alloc_itemp();
    latxs_load_addr_to_ir2(&func_addr_opnd,
                           (ADDR)latxs_convert_fpregs_x80_to_64,
                           LATXS_CONVERT_FPREGS_X80_TO_64);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_da_tmp();
        latxs_tr_dmw_out_fix_asid(NULL);
    }
    latxs_append_ir2_opnd1_(lisa_call, &func_addr_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_asid(&func_addr_opnd);
        latxs_tr_dmw_in_pg_tmp();
    }
    latxs_ra_free_temp(&func_addr_opnd);

    /* 3. restore return value */
    latxs_append_ir2_opnd2i(LISA_LD_D, &latxs_ret0_ir2_opnd,
                            &latxs_env_ir2_opnd,
                            lsenv_offset_of_save_regs(lsenv, 0x2));
}

/*
 * Prologue and Epilogue for helper function in system-mode
 *
 * If the helper requires complete ENV context, set @all_gpr to save
 * all the mapping registers.
 *
 * In the following situations, complete ENV is required:
 *  > helper that reads/writes all the GPRs...
 *      > helper_rsm
 *  > helper that might not return
 *      > helper_raise_exception: always no return
 *      > helper_raise_interrupt: always no return
 *      > helper that might triger interrupt/exception
 *          > many helper for system instruction
 *            - softmmu's helper : load/store
 *            - helper_ltr   : ltr
 *            - helper_lldt  : lldt
 *            - helper_pause : nop
 */
static void gen_helper_prologue(helper_cfg_t cfg, bool use_static)
{
    /* Use static helper prologue for default */
    if (use_static && scs_enabled() && cmp_helper_cfg(cfg, default_helper_cfg)) {
        TRANSLATION_DATA *td = lsenv->tr_data;
        TranslationBlock *tb = td->curr_tb;
        int offset = td->ir2_asm_nr << 2;

        uint32_t mtcc_id = td->mtcc_id;
        tb_sc_table_offset_jmp(tb, mtcc_id, scs_prologue,
                td->tcg_region_offset, offset, 1, SCS_PROLOGUE);
        return;
    }
    if (use_static && scs_enabled() && cmp_helper_cfg(cfg, nofp_helper_cfg)) {
        TRANSLATION_DATA *td = lsenv->tr_data;
        TranslationBlock *tb = td->curr_tb;
        int offset = td->ir2_asm_nr << 2;

        uint32_t mtcc_id = td->mtcc_id;
        tb_sc_table_offset_jmp(tb, mtcc_id, scs_prologue_nofp,
                td->tcg_region_offset, offset, 1, SCS_PROLOGUE_NOFP);
        return;
    }

    latxs_ras_tp_enter_c();

    if (hamt_enabled()) {
        latxs_tr_dmw_in_plv0_break();
    }

    /* Hardware $fcsr0 carries live guest MXCSR/FPU state across C helpers
     * (matching gen_latxs_scs_prologue_cfg); sc_prologue never populates
     * sp+128, so inline prologues/epilogues must not reload $fcsr0. */
    int save_top = !option_soft_fpu;
    if (likely(cfg.cfg.sv_allgpr)) {
        latxs_tr_save_registers_to_env(0xffffffff, 0xff, save_top,
                                       0xffffffff, 0x2);
    } else {
        if (cfg.cfg.sv_gpr) {
            latxs_tr_save_gprs_to_env(0xffffffff);
        }
        if (cfg.cfg.sv_fpr) {
            latxs_tr_save_fprs_to_env(0xff, save_top);
        }
        if (cfg.cfg.sv_simd) {
            latxs_tr_save_xmms_to_env(0xffffffff);
        }
    }

    if (likely(cfg.cfg.sv_eflags)) {
        latxs_tr_save_eflags();
    }

    if ((!option_soft_fpu) && unlikely(cfg.cfg.cvt_fp80)) {
        latxs_tr_save_temp_register();
        latxs_tr_cvt_fp64_to_80();
        latxs_tr_restore_temp_register();
    }

    if (hamt_enabled()) {
        latxs_tr_dmw_in_da_tmp();
        latxs_tr_dmw_out_fix_asid(NULL);
    }
}

static void gen_helper_epilogue(helper_cfg_t cfg, bool use_static)
{
    /*
     * X86 GPRs will be store and read again for helper:
     *   > After st_w and ld_w, GPRs' EM need to be fixed
     *   > For softmmu helper, it will fix the EM through
     *      another way in tr_softmmu.c
     */
    TRANSLATION_DATA *td = lsenv->tr_data;
    uint32_t mtcc_id = td->mtcc_id;

    /* Use static helper epilogue for default */
    if (use_static && scs_enabled() && cmp_helper_cfg(cfg, default_helper_cfg)) {
        TranslationBlock *tb = td->curr_tb;
        int offset = td->ir2_asm_nr << 2;
        tb_sc_table_offset_jmp(tb, mtcc_id, scs_epilogue,
                td->tcg_region_offset, offset, 1, SCS_EPILOGUE);
        return;
    }
    if (use_static && scs_enabled() && cmp_helper_cfg(cfg, nofp_helper_cfg)) {
        TranslationBlock *tb = td->curr_tb;
        int offset = td->ir2_asm_nr << 2;
        tb_sc_table_offset_jmp(tb, mtcc_id, scs_epilogue_nofp,
                td->tcg_region_offset, offset, 1, SCS_EPILOGUE_NOFP);
        return;
    }

    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_asid_tmp(&latxs_env_ir2_opnd);
        latxs_tr_dmw_in_pg_tmp();
        latxs_tr_dmw_in_plv_env_tmp(&latxs_env_ir2_opnd);
    }

    if ((!option_soft_fpu) && unlikely(cfg.cfg.cvt_fp80)) {
        latxs_tr_cvt_fp80_to_64();
    }

#ifndef CONFIG_LATX_OPT_EFLAGS
    if (likely(cfg.cfg.sv_eflags)) {
        latxs_tr_load_eflags();
    }
#endif

    int load_top = !option_soft_fpu;
    if (likely(cfg.cfg.sv_allgpr)) {
        latxs_tr_load_registers_from_env(0xffffffff, 0xff, load_top,
                                         0xffffffff, 0x2);
    } else {
        if (cfg.cfg.sv_simd) {
            latxs_tr_load_xmms_from_env(0xffffffff);
        }
        if (cfg.cfg.sv_fpr) {
            latxs_tr_load_fprs_from_env(0xff, load_top);
        }
        if (cfg.cfg.sv_gpr) {
            latxs_tr_load_gprs_from_env(0xffffffff);
        }
    }

    latxs_ras_tp_leave_c();
}

void latxs_tr_gen_call_to_helper_prologue_cfg(helper_cfg_t cfg)
{
    gen_helper_prologue(cfg, true);
}

void latxs_tr_gen_call_to_helper_epilogue_cfg(helper_cfg_t cfg)
{
    gen_helper_epilogue(cfg, true);
}

/* Static code has no source TB from which to emit a relocated SCS call. */
void latxs_tr_gen_call_to_helper_prologue_inline(helper_cfg_t cfg)
{
    gen_helper_prologue(cfg, false);
}

void latxs_tr_gen_call_to_helper_epilogue_inline(helper_cfg_t cfg)
{
    gen_helper_epilogue(cfg, false);
}

/* helper with zero arg */
void latxs_tr_gen_call_to_helper0_cfg(ADDR func, helper_cfg_t cfg, int helper_kind)
{
    latxs_tr_gen_call_to_helper_prologue_cfg(cfg);

    latxs_tr_gen_call_to_helper(func, helper_kind);

    latxs_tr_gen_call_to_helper_epilogue_cfg(cfg);
}

/* helper with 1 default arg(CPUArchState*) */
void latxs_tr_gen_call_to_helper1_cfg(ADDR func, helper_cfg_t cfg, int helper_kind)
{
    latxs_tr_gen_call_to_helper_prologue_cfg(cfg);

    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }

    latxs_tr_gen_call_to_helper(func, helper_kind);

    latxs_tr_gen_call_to_helper_epilogue_cfg(cfg);
}

/* helper with 2 arg(CPUArchState*, int) */
void latxs_tr_gen_call_to_helper2_cfg(ADDR func, int arg2, helper_cfg_t cfg, int helper_kind)
{
    latxs_tr_gen_call_to_helper_prologue_cfg(cfg);

    latxs_load_imm32_to_ir2(&latxs_arg1_ir2_opnd, arg2, EXMode_S);

    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }

    latxs_tr_gen_call_to_helper(func, helper_kind);

    latxs_tr_gen_call_to_helper_epilogue_cfg(cfg);
}

/* helper with 3 arg(CPUArchState*, int, int) */
void latxs_tr_gen_call_to_helper3_cfg(ADDR func, int arg2, int arg3,
                                      helper_cfg_t cfg, int helper_kind)
{
    latxs_tr_gen_call_to_helper_prologue_cfg(cfg);

    latxs_load_imm32_to_ir2(&latxs_arg1_ir2_opnd, arg2, EXMode_S);
    latxs_load_imm32_to_ir2(&latxs_arg2_ir2_opnd, arg3, EXMode_S);

    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }

    latxs_tr_gen_call_to_helper(func, helper_kind);

    latxs_tr_gen_call_to_helper_epilogue_cfg(cfg);
}

/* helper with 3 arg(CPUArchState*, uint64, uint64) */
void latxs_tr_gen_call_to_helper3_u64_cfg(ADDR func, uint64_t arg2,
        uint64_t arg3, helper_cfg_t cfg, int helper_kind)
{
    latxs_tr_gen_call_to_helper_prologue_cfg(cfg);

    latxs_load_imm64_to_ir2(&latxs_arg1_ir2_opnd, arg2);
    latxs_load_imm64_to_ir2(&latxs_arg2_ir2_opnd, arg3);

    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }

    latxs_tr_gen_call_to_helper(func, helper_kind);

    latxs_tr_gen_call_to_helper_epilogue_cfg(cfg);
}

void latxs_tr_gen_call_to_helper4_u64_cfg(ADDR func, uint64_t arg2,
        uint64_t arg3, uint64_t arg4, helper_cfg_t cfg, int helper_kind)
{
    latxs_tr_gen_call_to_helper_prologue_cfg(cfg);

    latxs_load_imm64_to_ir2(&latxs_arg1_ir2_opnd, arg2);
    latxs_load_imm64_to_ir2(&latxs_arg2_ir2_opnd, arg3);
    latxs_load_imm64_to_ir2(&latxs_arg3_ir2_opnd, arg4);

    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                            &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }

    latxs_tr_gen_call_to_helper(func, helper_kind);

    latxs_tr_gen_call_to_helper_epilogue_cfg(cfg);
}

void latxs_tr_gen_call_to_helper_pclmulqdq(ADDR func, int dest_xmm_num,
        int v_xmm_num, int src_xmm_num, int ctrl, int helper_kind)
{
    /* prologue */
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
    
    /* set arguments */
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd, &latxs_env_ir2_opnd);
    IR2_OPND tmp_env_opnd = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp_env_opnd, &latxs_env_ir2_opnd, 0x7f0);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg1_ir2_opnd, &tmp_env_opnd, lsenv_offset_of_xmm(lsenv, dest_xmm_num) - 0x7f0);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg2_ir2_opnd, &tmp_env_opnd, lsenv_offset_of_xmm(lsenv, v_xmm_num) - 0x7f0);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg3_ir2_opnd, &tmp_env_opnd, lsenv_offset_of_xmm(lsenv, src_xmm_num) - 0x7f0);
    latxs_ra_free_temp(&tmp_env_opnd);
    latxs_load_imm32_to_ir2(&latxs_arg4_ir2_opnd, ctrl, EXMode_N);

    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg1_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg2_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg3_ir2_opnd);
    }
    /* load func_addr and jmp */                  
    latxs_tr_gen_call_to_helper(func, helper_kind);

    /* restore context */
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
}

void latxs_tr_gen_call_to_helper_pcmpxstrx(ADDR func, int dest_xmm_num, int src_xmm_num, int ctrl, int helper_kind)
{
    /* prologue */
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
    
    /* set arguments */
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd, &latxs_env_ir2_opnd);
    IR2_OPND tmp_env_opnd = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp_env_opnd, &latxs_env_ir2_opnd, 0x7f0);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg1_ir2_opnd, &tmp_env_opnd, lsenv_offset_of_xmm(lsenv, dest_xmm_num) - 0x7f0);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg2_ir2_opnd, &tmp_env_opnd, lsenv_offset_of_xmm(lsenv, src_xmm_num) - 0x7f0);
    latxs_ra_free_temp(&tmp_env_opnd);
    latxs_load_imm32_to_ir2(&latxs_arg3_ir2_opnd, ctrl, EXMode_N);

    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg1_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg2_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg3_ir2_opnd);
    }
    /* load func_addr and jmp */                  
    latxs_tr_gen_call_to_helper(func, helper_kind);

    /* restore context */
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
}

void latxs_tr_gen_call_to_helper_aes(ADDR func, int dest_xmm_num, int src1_xmm_num, int src2_xmm_num, int helper_kind)
{
    /* prologue */
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);

    /* set arguments */
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd, &latxs_env_ir2_opnd);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg1_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_xmm(lsenv, dest_xmm_num));
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg2_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_xmm(lsenv, src1_xmm_num));
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg3_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_xmm(lsenv, src2_xmm_num));

    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg1_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg2_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg3_ir2_opnd);
    }
    /* load func_addr and jmp */
    latxs_tr_gen_call_to_helper(func, helper_kind);

    /* prologue, jmp and epilogue */
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
}

void latxs_tr_gen_call_to_helper_sha(ADDR func, int dest_xmm_num, int src1_xmm_num, int src2_xmm_num, int helper_kind)
{
    /* prologue */
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);

    /* set arguments */
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg0_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_xmm(lsenv, dest_xmm_num));
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg1_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_xmm(lsenv, src1_xmm_num));
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg2_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_xmm(lsenv, src2_xmm_num));
    latxs_tr_gen_call_to_helper(func, helper_kind);

    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg1_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg2_ir2_opnd);
    }
    /* prologue, jmp and epilogue */
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
}

void latxs_tr_gen_call_to_helper_sha256rnds2(ADDR func, int dest_xmm_num, int src1_xmm_num, int src2_xmm_num, int helper_kind)
{
    /* prologue */
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);

    /* set arguments */
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg0_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_xmm(lsenv, dest_xmm_num));
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg1_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_xmm(lsenv, src1_xmm_num));
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &latxs_arg2_ir2_opnd, &latxs_env_ir2_opnd, lsenv_offset_of_xmm(lsenv, src2_xmm_num));

    IR2_OPND xmm0 = latxs_ra_alloc_xmm(0);
    latxs_append_ir2_opnd2i(LISA_VPICKVE2GR_WU, &latxs_arg3_ir2_opnd, &xmm0, 0);
    latxs_append_ir2_opnd2i(LISA_VPICKVE2GR_WU, &latxs_arg4_ir2_opnd, &xmm0, 1);

    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg1_ir2_opnd);
        latxs_tr_dmw_out_fix_mem(&latxs_arg2_ir2_opnd);
    }
    latxs_tr_gen_call_to_helper(func, helper_kind);
    /* prologue, jmp and epilogue */
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
}

void latxs_tr_gen_infinite_loop(void)
{
    IR2_OPND label_opnd = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd1(LISA_LABEL, &label_opnd);
    latxs_append_ir2_opnd1(LISA_B, &label_opnd);
    latxs_append_ir2_opnd0_(lisa_nop);
}

/*  @opnd0: load target ir2 opnd

    load curr eip, for pcrel, we use reg la_sp to calculate eip during guset execution
    if pcrel, opnd0 & opnd1 is not used
*/
void latxs_tr_gen_load_from_curr_eip(IR2_OPND  *opnd0)
{
    /* usually, use pcrep as default, set with pcrel */
    if(option_page_pcrel) {
        /* page_pcrel, sp is just page_num, load from env */
        latxs_append_ir2_opnd2i(LISA_LD_D , opnd0, &latxs_env_ir2_opnd,
            lsenv_offset_of_eip(lsenv));
    } else {
        latxs_append_ir2_opnd2i(LISA_LD_D , opnd0, &latxs_env_ir2_opnd,
            lsenv_offset_of_eip(lsenv));
    }
}

/*  @opnd0: load target ir2 opnd

    store curr eip, for pcrel, we use reg la_sp to calculate eip during guset execution
    if pcrel, opnd0 & opnd1 is not used
    however, in pcrel, don't directly update eip except for jmpin/callin/ljmp_mem
*/
void latxs_tr_gen_store_to_curr_eip(IR2_OPND  *opnd0)
{
    if (option_page_pcrel) {
        latxs_append_ir2_opnd2i(LISA_ST_D, opnd0, &latxs_env_ir2_opnd,
            lsenv_offset_of_eip(lsenv));
    } else {
        latxs_append_ir2_opnd2i(LISA_ST_D, opnd0, &latxs_env_ir2_opnd,
                                lsenv_offset_of_eip(lsenv));
    }
}

/*  @opnd0   : load target ir2 opnd
    @offset  : offset for update eip, it may be a small number that can trans by addi
    @next_eip: next_ir1 addr, for ~pcrel

    store next eip, for pcrel, we use reg la_sp to calculate eip during guset execution
    if pcrel, opnd0 & opnd1 is still used
    for pcrel, using addi or add depends on range of imm (12 bits sign imm)
*/
void latxs_tr_gen_add_st_curr_eip(IR2_OPND  *opnd0, int64_t offset, int64_t next_eip)
{
    int use_addi_w = 0;
    if (offset >> 11 == -1 || offset >> 11 == 0)
        use_addi_w = 1;
    IR2_OPND tmp = latxs_ra_alloc_itemp();
    /* only in pcrel, offset is offset for next_eip */
    /* tb_pcrel: sp is curr_save_eip, offset is next eip offset to saved*/
    if (option_tb_pcrel) {
        if(use_addi_w) {
            latxs_append_ir2_opnd2i(LISA_LD_D, &tmp, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp, &tmp, offset);
            latxs_append_ir2_opnd2i(LISA_ST_D, &tmp, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
        } else {
            /* offset is too large*/
            assert(opnd0 != NULL);
            // printf("load offset to ir2, offset = %lx\n", offset);
            latxs_load_imm64_to_ir2(opnd0, offset);
            latxs_append_ir2_opnd2i(LISA_LD_D, &tmp, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
            latxs_append_ir2_opnd3(LISA_ADD_D, &tmp, &tmp, opnd0);
            latxs_append_ir2_opnd2i(LISA_ST_D, &tmp, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
        }
    /* page_pcrel: sp is curr_page_num, offset is next eip offset to page*/
    } else if (option_page_pcrel) {
        if (use_addi_w) {
            latxs_append_ir2_opnd2i(LISA_LD_D, &tmp, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &tmp, &latxs_zero_ir2_opnd, 11, 0);
            latxs_append_ir2_opnd2i(LISA_ADDI_D, &tmp, &tmp, offset);
            latxs_append_ir2_opnd2i(LISA_ST_D, &tmp, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
        } else {
            /* offset is too large*/
            // printf("load offset to ir2, offset = %lx\n", offset);
            assert(opnd0 != NULL);
            latxs_load_imm64_to_ir2(opnd0, offset);
            latxs_append_ir2_opnd2i(LISA_LD_D, &tmp, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, &tmp, &latxs_zero_ir2_opnd, 11, 0);
            latxs_append_ir2_opnd3(LISA_ADD_D, &tmp, &tmp, opnd0);
            latxs_append_ir2_opnd2i(LISA_ST_D, &tmp, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
        }
    /* for not pcrel, load next_eip to opnd1 */
    } else {
        // assert(0);
        /*assert(opnd0 != NULL && next_eip != 0);*/
         assert(opnd0 != NULL);
        latxs_load_eip_to_ir2(opnd0, next_eip);
        latxs_append_ir2_opnd2i(LISA_ST_D, opnd0, &latxs_env_ir2_opnd,
                lsenv_offset_of_eip(lsenv));

    }
    latxs_ra_free_temp(&tmp);

}

/*  @opnd0   : calculate target ir2 opnd
    @offset  : offset for update eip, it may be a small number that can trans by addi
    @next_eip: next_ir1 addr, for ~pcrel
    @use_sp  : for helper arg calculate, it is after helper context switch, cannot use la_sp

    used for eip calculation, but write result to a ir2_opnd, such as for helper arg
    for pcrel, we use reg la_sp to calculate eip during guset execution
    for pcrel, using addi or add depends on range of imm (12 bits sign imm)
*/
void latxs_tr_gen_add_curr_eip_to_ir2(IR2_OPND  *opnd0, int64_t offset, int64_t next_eip, bool use_sp)
{
    int use_addi_w = 0;
    if (offset >> 11 == -1 || offset >> 11 == 0)
        use_addi_w = 1;
    /* only in pcrel, imm is offset for next_eip */
    if (option_tb_pcrel) {
        /* if use_sp is false. load eip from env, reg_sp is switch to host sp now */
        if (use_addi_w && !use_sp) {
            /* if not use_sp, cannot replace mem env->eip with reg sp */
            latxs_append_ir2_opnd2i(LISA_LD_D, opnd0, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
            latxs_append_ir2_opnd2i(LISA_ADDI_D, opnd0, opnd0, offset);
        } else if (!use_addi_w) {
            /* offset is too large, emm may not happen  */
            assert(0);
        } else if (use_sp) {
            latxs_append_ir2_opnd2i(LISA_LD_D, opnd0, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
            latxs_append_ir2_opnd2i(LISA_ADDI_D, opnd0, opnd0, offset);
        }
    } else if (option_page_pcrel) {
        /* !use_sp, cannot use mapped la_sp */
        if (use_addi_w) {
            /* eip = page_num + page_offset */
            latxs_append_ir2_opnd2i(LISA_LD_D, opnd0, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
            /* clear eip_offset */
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, opnd0, &latxs_zero_ir2_opnd, 11, 0);
            latxs_append_ir2_opnd2i(LISA_ADDI_D, opnd0, opnd0, offset);
        } else if (!use_addi_w) {
            /* offset is too large  */
            IR2_OPND tmp = latxs_ra_alloc_itemp();
            latxs_load_imm64_to_ir2(&tmp, offset);
            latxs_append_ir2_opnd2i(LISA_LD_D, opnd0, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_eip(lsenv));
            latxs_append_ir2_opnd2ii(LISA_BSTRINS_D, opnd0, &latxs_zero_ir2_opnd, 11, 0);
            latxs_append_ir2_opnd3(LISA_ADD_D, opnd0, opnd0, &tmp);
        }
    } else {
        /* for no_pcrel, imm is directly next_eip, load next_eip to opnd1 */
        assert(next_eip != 0);
        latxs_load_eip_to_ir2(opnd0, next_eip);
    }
}

void latxs_tr_gen_save_curr_eip(void)
{
    lsassert(lsenv->tr_data->curr_ir1_inst);
    TRANSLATION_DATA *td = lsenv->tr_data;

    IR1_INST *pir1 = lsenv->tr_data->curr_ir1_inst;
    ADDRX curr_eip = ir1_addr(pir1);
    int eip_offset = 0;
    /* PCREL */
    if (option_tb_pcrel)
        eip_offset =  td->pc_curr - td->pc_save;
    else if (option_page_pcrel)
        eip_offset =  td->pc_curr - td->curr_page;
    IR2_OPND eip_opnd = latxs_ra_alloc_itemp();
    latxs_tr_gen_add_st_curr_eip(&eip_opnd, eip_offset, curr_eip);
    latxs_ra_free_temp(&eip_opnd);

    td->pc_save = td->pc_curr;
}

void latxs_tr_gen_save_next_eip(void)
{
    lsassert(lsenv->tr_data->curr_ir1_inst);
    TRANSLATION_DATA *td = lsenv->tr_data;

    IR1_INST *pir1 = lsenv->tr_data->curr_ir1_inst;
    ADDRX next_eip = ir1_addr_next(pir1);
    int eip_offset = 0;
    /* PCREL */
    if (option_tb_pcrel)
        eip_offset =  td->pc_next - td->pc_save;
    else if (option_page_pcrel)
        eip_offset =  td->pc_next - td->curr_page;
    IR2_OPND eip_opnd = latxs_ra_alloc_itemp();
    latxs_tr_gen_add_st_curr_eip(&eip_opnd, eip_offset, next_eip);
    latxs_ra_free_temp(&eip_opnd);

    td->pc_save = td->pc_next;
}

void latxs_tr_save_temp_register_mask(int mask)
{
    const TEMP_REG_STATUS *p = latxs_itemp_status_default;
    int i;
    for (i = 0; i < latxs_itemp_status_num; ++i) {
        if ((mask >> i) & 0x1) {
            int ireg = p[i].physical_id;
            IR2_OPND gpr = latxs_ir2_opnd_new(IR2_OPND_GPR, ireg);
            latxs_append_ir2_opnd2i(LISA_ST_D, &gpr, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_save_regs(lsenv, ireg));
        }
    }
}

void latxs_tr_restore_temp_register_mask(int mask)
{
    const TEMP_REG_STATUS *p = latxs_itemp_status_default;
    int i;
    for (i = 0; i < latxs_itemp_status_num; ++i) {
        if ((mask >> i) & 0x1) {
            int ireg = p[i].physical_id;
            IR2_OPND gpr = latxs_ir2_opnd_new(IR2_OPND_GPR, ireg);
            latxs_append_ir2_opnd2i(LISA_LD_D, &gpr, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_save_regs(lsenv, ireg));
        }
    }
}

/* Only need to save current used temp registers */
void latxs_tr_save_temp_register(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    const TEMP_REG_STATUS *p = latxs_itemp_status_default;

    int mask = td->itemp_mask;
    td->itemp_mask_bk = mask;

    lsassertm(!(td->itemp_saved), "Can not save temp again after save temp.\n");

    int i;
    for (i = 0; i < latxs_itemp_status_num; ++i) {
        /* if ((mask >> i) & 0x1) { */
        int ireg = p[i].physical_id;
        IR2_OPND gpr = latxs_ir2_opnd_new(IR2_OPND_GPR, ireg);
        latxs_append_ir2_opnd2i(LISA_ST_D, &gpr, &latxs_env_ir2_opnd,
                                lsenv_offset_of_save_regs(lsenv, ireg));
        /* pb[i].virtual_id = p[i].virtual_id; */
        /* } */
    }

    td->itemp_saved = 1;
}

/* Only need to restore saved temp registers */
void latxs_tr_restore_temp_register(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    const TEMP_REG_STATUS *p = latxs_itemp_status_default;

    int mask_bk = td->itemp_mask_bk;
    td->itemp_mask = mask_bk;

    lsassertm(td->itemp_saved,
              "Can not resoter temp again if not save temp.\n");

    int i;
    for (i = 0; i < latxs_itemp_status_num; ++i) {
        if ((mask_bk >> i) & 0x1) {
            int ireg = p[i].physical_id;
            IR2_OPND gpr = latxs_ir2_opnd_new(IR2_OPND_GPR, ireg);
            latxs_append_ir2_opnd2i(LISA_LD_D, &gpr, &latxs_env_ir2_opnd,
                                    lsenv_offset_of_save_regs(lsenv, ireg));
        }
    }

    td->itemp_saved = 0;
}
