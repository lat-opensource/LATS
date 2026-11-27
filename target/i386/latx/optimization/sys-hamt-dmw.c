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
#include "translate.h"
#include "sys-excp.h"
#include <string.h>

#include "latx-sys-hamt.h"
#include "latx-sys-hamt-dmw.h"

void latxs_tr_dmw_in_fix_mem(IR2_OPND *mem)
{
    if (hamt_mode_dmw()) {
        latxs_append_ir2_opnd2i(LISA_LU52I_D, mem, mem, 0x900);
    }
}

void latxs_tr_dmw_out_fix_mem(IR2_OPND *mem)
{
    if (hamt_mode_dmw()) {
        latxs_append_ir2_opnd2i(LISA_LU52I_D, mem, mem, 0x000);
    }
}

/* Legacy DMW mode switches ASID between translator and guest mappings. */
static void enter_guest_asid(IR2_OPND *tmp, IR2_OPND *env)
{
    if (hamt_mode_dmw() && !hamt_dmw_mode_da() && !hamt_disable_asid()) {
        IR2_OPND *zero = &latxs_zero_ir2_opnd;
        if (option_pcid_asid) {
            QEMU_BUILD_BUG_ON(offsetof(CPUX86State, hamt_guest_asid) >= 2048);
            latxs_append_ir2_opnd2i(LISA_LD_HU, tmp, env,
                                    offsetof(CPUX86State, hamt_guest_asid));
        } else {
            latxs_append_ir2_opnd2i(LISA_ORI, tmp, zero, 0xf);
        }
        latxs_append_ir2_opnd2(LISA_CSRWR, &la_csr_asid, tmp);
    }
}

void latxs_tr_dmw_in_fix_asid(IR2_OPND *tmp)
{
    enter_guest_asid(tmp, &latxs_env_ir2_opnd);
}

void latxs_tr_dmw_out_fix_asid(IR2_OPND *tmp)
{
    if (hamt_mode_dmw() && !hamt_dmw_mode_da() && !hamt_disable_asid()) {
        IR2_OPND *zero = &latxs_zero_ir2_opnd;
        latxs_append_ir2_opnd2(LISA_CSRWR, &la_csr_asid, zero);
    }
}

void latxs_tr_dmw_in_fix_asid_tmp(IR2_OPND *env)
{
    if (hamt_mode_dmw() && !hamt_dmw_mode_da() && !hamt_disable_asid()) {
        IR2_OPND tmp = latxs_ra_alloc_itemp();
        enter_guest_asid(&tmp, env);
        latxs_ra_free_temp(&tmp);
    }
}

void latxs_tr_dmw_out_fix_asid_tmp(void)
{
    if (hamt_mode_dmw() && !hamt_dmw_mode_da() && !hamt_disable_asid()) {
        IR2_OPND tmp = latxs_ra_alloc_itemp();
        latxs_tr_dmw_out_fix_asid(&tmp);
        latxs_ra_free_temp(&tmp);
    }
}

void latxs_tr_dmw_in_pg(IR2_OPND *tmp0, IR2_OPND *tmp1)
{
    if (hamt_dmw_mode_da()) {
        la_out2i(LISA_ORI, tmp0, &la_zero, (0 << 3) | (1 << 4));
        la_out2i(LISA_ORI, tmp1, &la_zero, (1 << 3) | (1 << 4));
        la_out3(LISA_CSRXCHG, &la_csr_crmd, tmp0, tmp1);
        la_comment("HAMT: enter PG mode");
    }
}

void latxs_tr_dmw_in_da(IR2_OPND *tmp0, IR2_OPND *tmp1)
{
    if (hamt_dmw_mode_da()) {
        la_out2i(LISA_ORI, tmp0, &la_zero, (1 << 3) | (0 << 4));
        la_out2i(LISA_ORI, tmp1, &la_zero, (1 << 3) | (1 << 4));
        la_out3(LISA_CSRXCHG, &la_csr_crmd, tmp0, tmp1);
        la_comment("HAMT: enter DA mode");
    }
}

void latxs_tr_dmw_in_pg_tmp(void)
{
    if (hamt_dmw_mode_da()) {
        IR2_OPND t0 = latxs_ra_alloc_itemp();
        IR2_OPND t1 = latxs_ra_alloc_itemp();
        latxs_tr_dmw_in_pg(&t0, &t1);
        latxs_ra_free_temp(&t0);
        latxs_ra_free_temp(&t1);
    }
}

void latxs_tr_dmw_in_da_tmp(void)
{
    if (hamt_dmw_mode_da()) {
        IR2_OPND t0 = latxs_ra_alloc_itemp();
        IR2_OPND t1 = latxs_ra_alloc_itemp();
        latxs_tr_dmw_in_da(&t0, &t1);
        latxs_ra_free_temp(&t0);
        latxs_ra_free_temp(&t1);
    }
}

void latxs_tr_dmw_in_plv0_break(void)
{
    if (hamt_enable_plv()) {
        IR2_OPND c = la_code(HAMT_BREAK_CODE_TOPLV0);
        la_out1(LISA_BREAK, &c);
        la_comment("break to PLV0");
    }
}

void latxs_tr_dmw_in_plv3_break(void)
{
    if (hamt_enable_plv()) {
        IR2_OPND c = la_code(HAMT_BREAK_CODE_TOPLV3);
        la_out1(LISA_BREAK, &c);
        la_comment("break to PLV3");
    }
}

void latxs_tr_dmw_in_plv0(IR2_OPND *tmp)
{
    if (hamt_enable_plv()) {
        latxs_tr_dmw_in_plv0_break();
    }
}

void latxs_tr_dmw_in_plv3(IR2_OPND *tmp)
{
    if (hamt_enable_plv()) {
        la_out2(LISA_CSRRD, tmp, &la_csr_crmd);
        la_out2i(LISA_ORI, tmp, tmp, 0x3);
        la_out2(LISA_CSRWR, &la_csr_crmd, tmp);
    }
}

void latxs_tr_dmw_in_plv0_tmp(void)
{
    if (hamt_enable_plv()) {
        IR2_OPND t0 = latxs_ra_alloc_itemp();
        latxs_tr_dmw_in_plv0(&t0);
        latxs_ra_free_temp(&t0);
    }
}

void latxs_tr_dmw_in_plv3_tmp(void)
{
    if (hamt_enable_plv()) {
        IR2_OPND t0 = latxs_ra_alloc_itemp();
        latxs_tr_dmw_in_plv3(&t0);
        latxs_ra_free_temp(&t0);
    }
}

void latxs_tr_dmw_in_plv_cpl(IR2_OPND *tmp, IR2_OPND *cpl)
{
    if (hamt_enable_plv()) {
        la_out2(LISA_CSRRD, tmp, &la_csr_crmd);
        la_out2ii(LISA_BSTRINS_D, tmp, &la_zero, 1, 0);
        la_out3(LISA_OR, tmp, tmp, cpl);
        la_out2(LISA_CSRWR, &la_csr_crmd, tmp);
    }
}

void latxs_tr_dmw_in_plv_env(IR2_OPND *t0, IR2_OPND *t1, IR2_OPND *env)
{
    if (hamt_enable_plv()) {
        la_out2i(LISA_LD_WU, t1, env, offsetof(CPUX86State, hflags));
        la_out2i(LISA_ANDI, t1, t1, 0x3);
        la_comment("read current CPL");
        latxs_tr_dmw_in_plv_cpl(t0, t1);
        la_comment("switch PLV according to CPL");
    }
}

void latxs_tr_dmw_in_plv_env_tmp(IR2_OPND *env)
{
    if (hamt_enable_plv()) {
        IR2_OPND tmp = latxs_ra_alloc_itemp();
        IR2_OPND cpl = latxs_ra_alloc_itemp();
        latxs_tr_dmw_in_plv_env(&tmp, &cpl, env);
        latxs_ra_free_temp(&tmp);
        latxs_ra_free_temp(&cpl);
    }
}

void latxs_tr_dmw_in_plv_env_break(IR2_OPND *t0, IR2_OPND *env)
{
    if (!hamt_enable_plv()) return;

    IR2_OPND cpl_is_zero = la_new_label();
    IR2_OPND finish = la_new_label();

    la_out2i(LISA_LD_WU, t0, env, offsetof(CPUX86State, hflags));
    la_out2i(LISA_ANDI, t0, t0, 0x3);
    la_comment("read current CPL");
    la_out2(LISA_BEQZ, t0, &cpl_is_zero);
    latxs_tr_dmw_in_plv3_break();
    la_out1(LISA_B, &finish);

    la_label_name(cpl_is_zero, "CPL = 0");
    latxs_tr_dmw_in_plv0_break();
    la_label_name(finish, "finish: switch PLV by CPL");
}

void latxs_tr_dmw_in_plv_env_break_tmp(IR2_OPND *env)
{
    if (!hamt_enable_plv()) return;

    IR2_OPND tmp = latxs_ra_alloc_itemp();
    latxs_tr_dmw_in_plv_env_break(&tmp, env);
    latxs_ra_free_temp(&tmp);
}
