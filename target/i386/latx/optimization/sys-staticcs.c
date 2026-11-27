/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <string.h>
#include "common.h"
#include "cpu.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "translate.h"
#include "latx-ras.h"
#include "sys-excp.h"
#include "latx-sys-hamt-dmw.h"

int scs_enabled(void)
{
    return option_staticcs;
}

/*
 * All temp registers should not be touch by static
 * generated prologue and epilogue codes.
 * Use stmp1_ir2_opnd & stmp2_ir2_opnd to mov data
 */

int gen_latxs_scs_prologue_cfg(
        void *code_ptr,
        helper_cfg_t cfg)
{
    if (!scs_enabled()) {
        lsassertm(0, "StaticCS not enabled but generated.\n");
        return 0;
    }

    latxs_tr_init(NULL);
    latxs_ras_tp_enter_c();
    if (hamt_enabled()) {
        latxs_tr_dmw_in_plv0_break();
    }

//    int ra_reg_num = latxs_ir2_opnd_reg(&latxs_ra_ir2_opnd);
//    latxs_append_ir2_opnd2i(LISA_ST_D, &latxs_ra_ir2_opnd,
//            &latxs_env_ir2_opnd,
//            lsenv_offset_of_save_regs(lsenv, ra_reg_num));


#if defined(LATX_SYS_FCSR)
//    latxs_save_fcsr_scs_prologue();
#endif

    /* FPU TOP will be stored outside */
    if ((cfg.cfg.sv_allgpr)) {
        latxs_tr_gen_static_save_registers_to_env(
                0xffffffff, 0xff, 0xffffffff, 0x0);
    } else {
        lsassert(0);
    }

#ifdef CONFIG_LATX_OPT_NOFP
    latxs_gen_nofp_sync_fprs_to_env_static();
#endif

#ifndef CONFIG_LATX_OPT_EFLAGS
    if (cfg.cfg.sv_eflags) {
        latxs_tr_gen_static_save_eflags();
    }
#endif

    if (cfg.cfg.cvt_fp80) {
        lsassertm(0, "cvtfp not supported in staticcs.\n");
    }

#if defined(LATX_SYS_FCSR)
    //latxs_load_dbt_fcsr(&latxs_stmp1_ir2_opnd);
#endif

//    latxs_append_ir2_opnd2i(LISA_LD_D, &latxs_ra_ir2_opnd,
//            &latxs_env_ir2_opnd,
//            lsenv_offset_of_save_regs(lsenv, ra_reg_num));

    if (hamt_enabled()) {
        latxs_tr_dmw_in_da(&la_a2, &la_a3);
        latxs_tr_dmw_out_fix_asid(NULL);
    }

    latxs_append_ir2_opnd0_(lisa_return);

    /* */
    int code_nr  = latxs_tr_ir2_assemble(code_ptr);
    latxs_tr_fini();
    return code_nr;
}

int gen_latxs_scs_prologue_cfg_nofp(
        void *code_ptr,
        helper_cfg_t cfg)
{
    if (!scs_enabled()) {
        lsassertm(0, "StaticCS not enabled but generated.\n");
        return 0;
    }

    latxs_tr_init(NULL);
    latxs_ras_tp_enter_c();
    if (hamt_enabled()) {
        latxs_tr_dmw_in_plv0_break();
    }

//    int ra_reg_num = latxs_ir2_opnd_reg(&latxs_ra_ir2_opnd);
//    latxs_append_ir2_opnd2i(LISA_ST_D, &latxs_ra_ir2_opnd,
//            &latxs_env_ir2_opnd,
//            lsenv_offset_of_save_regs(lsenv, ra_reg_num));

    /* FPU TOP will be stored outside */
#ifdef CONFIG_LATX_OPT_NOFP
    latxs_tr_gen_static_save_registers_to_env(
            0xffffffff, 0x00, 0x0, 0x0);
    /* A cold lookup may load AOT code and use host floating registers. */
    latxs_gen_nofp_sync_fprs_to_env_static();
#else
    latxs_tr_gen_static_save_registers_to_env(
            0xffffffff, 0xff, 0xffffffff, 0x0);
#endif

#ifndef CONFIG_LATX_OPT_EFLAGS
    if (cfg.cfg.sv_eflags) {
        latxs_tr_gen_static_save_eflags();
    }
#endif

    if (cfg.cfg.cvt_fp80) {
        lsassertm(0, "cvtfp not supported in staticcs.\n");
    }

//    latxs_append_ir2_opnd2i(LISA_LD_D, &latxs_ra_ir2_opnd,
//            &latxs_env_ir2_opnd,
//            lsenv_offset_of_save_regs(lsenv, ra_reg_num));

    if (hamt_enabled()) {
        latxs_tr_dmw_in_da(&la_a2, &la_a3);
        latxs_tr_dmw_out_fix_asid(NULL);
    }

    latxs_append_ir2_opnd0_(lisa_return);

    /* */
    int code_nr  = latxs_tr_ir2_assemble(code_ptr);
    latxs_tr_fini();
    return code_nr;
}

int gen_latxs_scs_epilogue_cfg(
        void *code_ptr,
        helper_cfg_t cfg)
{
    if (!scs_enabled()) {
        lsassertm(0, "StaticCS not enabled but generated.\n");
        return 0;
    }

    latxs_tr_init(NULL);

//    int ra_reg_num = latxs_ir2_opnd_reg(&latxs_ra_ir2_opnd);
//    latxs_append_ir2_opnd2i(LISA_ST_D, &latxs_ra_ir2_opnd,
//            &latxs_env_ir2_opnd,
//            lsenv_offset_of_save_regs(lsenv, ra_reg_num));

    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_asid(&latxs_arg2_ir2_opnd);
        latxs_tr_dmw_in_pg(&la_a2, &la_a3);
        latxs_tr_dmw_in_plv_env(&la_a2, &la_a3, &la_env);
    }


    if (cfg.cfg.cvt_fp80) {
        lsassertm(0, "cvtfp not supported in staticcs.\n");
    }

#ifndef CONFIG_LATX_OPT_EFLAGS
    if (cfg.cfg.sv_eflags) {
        latxs_tr_gen_static_load_eflags(0);
    }
#endif

#ifdef CONFIG_LATX_OPT_NOFP
    latxs_gen_nofp_restore_fprs_static();
#endif

    /* FPU TOP will be loaded outside */
    if (cfg.cfg.sv_allgpr) {
        latxs_tr_gen_static_load_registers_from_env(
                0xffffffff, 0xff, 0xffffffff, 0x0);
    } else {
        lsassert(0);
    }

//#if defined(LATX_SYS_FCSR)
////    latxs_load_fcsr_scs_epilogue();
//#endif

//    latxs_append_ir2_opnd2i(LISA_LD_D, &latxs_ra_ir2_opnd,
//            &latxs_env_ir2_opnd,
//            lsenv_offset_of_save_regs(lsenv, ra_reg_num));
    latxs_ras_tp_leave_c();
    latxs_append_ir2_opnd0_(lisa_return);

    int code_nr = latxs_tr_ir2_assemble(code_ptr);
    latxs_tr_fini();
    return code_nr;
}

int gen_latxs_scs_epilogue_cfg_nofp(
        void *code_ptr,
        helper_cfg_t cfg)
{
    if (!scs_enabled()) {
        lsassertm(0, "StaticCS not enabled but generated.\n");
        return 0;
    }

    latxs_tr_init(NULL);

//    int ra_reg_num = latxs_ir2_opnd_reg(&latxs_ra_ir2_opnd);
//    latxs_append_ir2_opnd2i(LISA_ST_D, &latxs_ra_ir2_opnd,
//            &latxs_env_ir2_opnd,
//            lsenv_offset_of_save_regs(lsenv, ra_reg_num));

    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_asid(&latxs_arg2_ir2_opnd);
        latxs_tr_dmw_in_pg(&la_a2, &la_a3);
        latxs_tr_dmw_in_plv_env(&la_a2, &la_a3, &la_env);
    }


    if (cfg.cfg.cvt_fp80) {
        lsassertm(0, "cvtfp not supported in staticcs.\n");
    }

#ifndef CONFIG_LATX_OPT_EFLAGS
    if (cfg.cfg.sv_eflags) {
        latxs_tr_gen_static_load_eflags(0);
    }
#endif

    /* FPU TOP will be loaded outside */
#ifdef CONFIG_LATX_OPT_NOFP
    latxs_gen_nofp_restore_fprs_static();
    latxs_tr_gen_static_load_registers_from_env(
            0xffffffff, 0x00, 0x0, 0x0);
#else
    latxs_tr_gen_static_load_registers_from_env(
            0xffffffff, 0xff, 0xffffffff, 0x0);
#endif

//    latxs_append_ir2_opnd2i(LISA_LD_D, &latxs_ra_ir2_opnd,
//            &latxs_env_ir2_opnd,
//            lsenv_offset_of_save_regs(lsenv, ra_reg_num));
    latxs_ras_tp_leave_c();
    latxs_append_ir2_opnd0_(lisa_return);

    int code_nr = latxs_tr_ir2_assemble(code_ptr);
    latxs_tr_fini();
    return code_nr;
}

void latxs_tr_gen_static_save_registers_to_env(
        uint32_t gpr_to_save,
        uint8_t fpr_to_save,
        uint32_t xmm_to_save,
        uint8_t vreg_to_save)
{
    latxs_tr_save_gprs_to_env(gpr_to_save);
    if (option_lsfpu && !option_soft_fpu) {
        latxs_tr_save_lstop_to_env(&latxs_stmp1_ir2_opnd);
        latxs_tr_fpu_disable_top_mode();
    }
    latxs_tr_save_fprs_to_env(fpr_to_save, 0);
    latxs_tr_save_xmms_to_env(xmm_to_save);
    latxs_tr_save_vreg_to_env(vreg_to_save);
}

void latxs_tr_gen_static_load_registers_from_env(
        uint32_t gpr_to_load,
        uint8_t fpr_to_load,
        uint32_t xmm_to_load,
        uint8_t vreg_to_load)
{
    latxs_tr_load_vreg_from_env(vreg_to_load);
    latxs_tr_load_xmms_from_env(xmm_to_load);
    latxs_tr_load_fprs_from_env(fpr_to_load, 0);
    if (option_lsfpu && !option_soft_fpu) {
        latxs_tr_fpu_enable_top_mode();
        latxs_tr_load_lstop_from_env(&latxs_stmp1_ir2_opnd);
    }
    latxs_tr_load_gprs_from_env(gpr_to_load);
}
