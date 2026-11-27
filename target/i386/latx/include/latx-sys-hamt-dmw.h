/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_HAMT_DMW_H_
#define _LATX_SYS_HAMT_DMW_H_

void latxs_tr_dmw_in_fix_mem(IR2_OPND *mem);
void latxs_tr_dmw_out_fix_mem(IR2_OPND *mem);

void latxs_tr_dmw_in_fix_asid(IR2_OPND *tmp);
void latxs_tr_dmw_out_fix_asid(IR2_OPND *tmp);

void latxs_tr_dmw_in_fix_asid_tmp(IR2_OPND *env);
void latxs_tr_dmw_out_fix_asid_tmp(void);

void latxs_tr_dmw_in_pg(IR2_OPND *tmp0, IR2_OPND *tmp1);
void latxs_tr_dmw_in_da(IR2_OPND *tmp0, IR2_OPND *tmp1);
void latxs_tr_dmw_in_pg_tmp(void);
void latxs_tr_dmw_in_da_tmp(void);

void latxs_tr_dmw_in_plv0(IR2_OPND *tmp);
void latxs_tr_dmw_in_plv3(IR2_OPND *tmp);
void latxs_tr_dmw_in_plv0_tmp(void);
void latxs_tr_dmw_in_plv3_tmp(void);
void latxs_tr_dmw_in_plv0_break(void);
void latxs_tr_dmw_in_plv3_break(void);
void latxs_tr_dmw_in_plv_cpl(IR2_OPND *tmp, IR2_OPND *cpl);
void latxs_tr_dmw_in_plv_env(IR2_OPND *t0, IR2_OPND *t1, IR2_OPND *env);
void latxs_tr_dmw_in_plv_env_tmp(IR2_OPND *env);
void latxs_tr_dmw_in_plv_env_break(IR2_OPND *t0, IR2_OPND *env);
void latxs_tr_dmw_in_plv_env_break_tmp(IR2_OPND *env);

#endif
