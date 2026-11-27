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
#include "latx-perfmap.h"
#include "latx-intb-sys.h"
#include "latx-helper.h"
#include "latx-static-codes.h"
#include "latx-shadow-htable.h"
#include "latx-intb-counter.h"
#include "latx-sys-hamt-dmw.h"
#include "latx-hack.h"
#include "sys-aot.h"

#ifdef CONFIG_LATX_PRINT

uint64_t fintb_num;
uint64_t fintb_1_njc_miss;
uint64_t fintb_1_njc_miss_zero;
uint64_t fintb_1_njc_hit;
uint64_t fintb_2_fsht_miss;
uint64_t fintb_2_fsht_miss_zero;
uint64_t fintb_2_fsht_hit;
uint64_t fintb_3_helper_miss;
uint64_t fintb_3_helper_hit;

uint64_t intb_num;
uint64_t intb_3_helper_miss;
uint64_t intb_3_helper_hit;

#endif

#ifdef LATXS_INTB_LINK_ENABLE

#ifdef LATXS_INTB_LINK_OPTION_CHECK
int __intb_link_enable(void)
{
    return option_intb_link;
}
#endif

#define LISA_LOAD_PC    LISA_LD_D
#define LISA_STORE_PC   LISA_ST_D

/*
 * 0. Private Buffer Lookup
 *    0.1 lookup private buffer // => 1. if fail
 *    0.2 jump to next TB
 * 1. Native Jmp Cache Lookup
 *    1.1 lookup jmp cache // => 2. if NULL
 *    1.2 check PC // => 2. if miss
 *    1.3 check TB.cflags CF_INVALID // => 2. if miss
 *    1.4 check CS_BASE // => 2. if miss
 *    1.5 check TB.flags with env->hflags // => 2. if miss
 *    1.6 => 3.
 * 2. Helper Lookup TB
 *    2.1 call helper lookup tb // => 3. if find TB
 *    2.2 jump to epilogue //  => epilogue if no valid TB
 * 3. Jmp Glue Check
 *    3.1 (FastCS.nolink) check // => 2.2 if check fail
 *    3.2 (FastCS.jmpglue) process
 *    3.3 (FPU Rotate) process // rotate and jump to next TB
 *    3.4 (SIGINT) check // => epilogue if interrupt pending
 *    3.5 (PB) insert private buffer
 *    3.6 jump to next TB
 */
static int gen_latxs_intb_lookup_impl(void *code_ptr, bool compact, bool resume)
{
    int code_nr = 0;

    latxs_tr_init(NULL);

    TRANSLATION_DATA *td = lsenv->tr_data;

    uint32_t mtcc_id = td->mtcc_id;

    int start = (td->ir2_asm_nr << 2);
    int offset = 0;
    int64_t ins_offset = 0;

    int off_tb_tc_ptr = 0;
    off_tb_tc_ptr += offsetof(TranslationBlock, tc);
    off_tb_tc_ptr += offsetof(struct tb_tc, ptr);

    IR2_OPND *ret0 = &latxs_ret0_ir2_opnd;    /* $4   a0/v0 */
    IR2_OPND *zero = &latxs_zero_ir2_opnd;    /* $0   zero  */
    IR2_OPND *env  = &latxs_env_ir2_opnd;     /* $23  s0    */
                                                      
    IR2_OPND *arg0 = &latxs_arg0_ir2_opnd;    /* $4   a0/v0 */

    IR2_OPND tmp  = latxs_ra_alloc_itemp();
    IR2_OPND tmp0 = latxs_ra_alloc_itemp();
    IR2_OPND tmp1 = latxs_ra_alloc_itemp();
                                                      
#ifdef CONFIG_LATX_PRINT
    intb_counter_la_out_inc(tmp, tmp0, intb_num);
#endif

    /*
     * $a6: previous TB
     *
     * $a0/v0: next TB (lookup result)       @ret0
     *         > valid after TB lookup
     */

    /* ======== 0. Private Buffer ========  */

    /* ======== 1. NJC Lookup TB ======== */
    /* Native fallback has released every frame. Its cold C lookup must
     * check pending work before selecting a fresh native ABI entry. */
    if (intb_njc_enabled() && (resume ? compact : !intb_njc_inline_enabled())) {
        IR2_OPND label_njc_miss = latxs_ir2_opnd_new_label();
        if (resume) {
            latxs_append_ir2_opnd2i(LISA_LDPTR_D, &tmp0, env,
                offsetof(CPUX86State, aot_native_fault_active) >> 2);
            latxs_append_ir2_opnd3(LISA_BNE, &tmp0, zero, &label_njc_miss);
            latxs_load_intcheck_to_reg(&tmp0);
            latxs_append_ir2_opnd3(LISA_BLT, &tmp0, zero, &label_njc_miss);
        }
        if (compact) {
            gen_latxs_intb_lookup_njc_compact(code_ptr,
                &tmp, &tmp0, &tmp1, &label_njc_miss, NULL, option_table_addr);
        } else {
            gen_latxs_intb_lookup_njc_legacy(code_ptr,
                &tmp, &tmp0, &tmp1, &label_njc_miss, NULL);
        }

        /* Compact tcptr is stored pre-windowed and remains in ret0. */
        if (!compact && hamt_enabled()) {
            latxs_tr_dmw_in_fix_mem(&tmp);
        }
        if (hamt_enabled()) {
            latxs_tr_dmw_in_plv_env_break(&tmp0, env);
        }
        latxs_append_ir2_opnd2i(LISA_JIRL, zero,
                compact ? ret0 : &tmp, 0);

        /* lookup miss */
        latxs_append_ir2_opnd1(LISA_LABEL, &label_njc_miss);
    }

    latxs_tr_gen_intb_sync_gpc();
#ifdef CONFIG_SOFTMMU
    /* ======== 2. fast path htable lookup ======== */
#endif

#if 0
    offset = (td->ir2_asm_nr << 2) - start;
    ins_offset = (GET_SC_TABLE(mtcc_id, cs_native_to_bt_ret_0) - (ADDR)code_ptr - offset);
    ins_offset = ins_offset >> 2;
    latxs_append_ir2_jmp_far(ins_offset, 0);
#else
    /* ======== 3. helper_lookup_tb ======== */

    /* 3.1 call helper lookup tb */
    offset = (td->ir2_asm_nr << 2) - start;
 #ifdef CONFIG_LATX_OPT_NOFP
    ins_offset = ((resume ? GET_SC_TABLE(mtcc_id, scs_prologue) :
                  GET_SC_TABLE(mtcc_id, scs_prologue_nofp)) - (ADDR)code_ptr - offset) >> 2;
 #else
    ins_offset = (GET_SC_TABLE(mtcc_id, scs_prologue) - (ADDR)code_ptr - offset) >> 2;
 #endif
    latxs_append_ir2_jmp_far(ins_offset, 1);

    latxs_append_ir2_opnd2_(lisa_mov, arg0, env);
    if (resume) {
        /* Runtime static code is not serialized into an AOT file. */
        la_imm(&la_ra, (ADDR)latx_helper_aot_resume);
        la_out1_(lisa_call, &la_ra);
    } else {
        latxs_tr_gen_call_to_helper((ADDR)latx_helper_lookup_tb_hashtable,
                LATX_HELPER_LOOKUP_TB_HASHTABLE);
    }

    offset = (td->ir2_asm_nr << 2) - start;
 #ifdef CONFIG_LATX_OPT_NOFP
    ins_offset = ((resume ? GET_SC_TABLE(mtcc_id, scs_epilogue) :
                  GET_SC_TABLE(mtcc_id, scs_epilogue_nofp)) - (ADDR)code_ptr - offset) >> 2;
 #else
    ins_offset = (GET_SC_TABLE(mtcc_id, scs_epilogue) - (ADDR)code_ptr - offset) >> 2;
 #endif
    latxs_append_ir2_jmp_far(ins_offset, 1);

    /* return tb.tc.ptr or cs_native_to_bt */
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(ret0);
        latxs_tr_dmw_in_plv_env_break(&tmp0, env);
    }
    latxs_append_ir2_opnd2i(LISA_JIRL, zero, ret0, 0);
#endif

    code_nr = latxs_tr_ir2_assemble(code_ptr);

    latxs_tr_fini();

    return code_nr;
}

int gen_latxs_intb_lookup(void *code_ptr)
{
    return gen_latxs_intb_lookup_impl(code_ptr, false, false);
}

int gen_latxs_intb_lookup_compact(void *code_ptr)
{
    return gen_latxs_intb_lookup_impl(code_ptr, true, false);
}

int gen_latxs_intb_resume_lookup(void *code_ptr)
{
    return gen_latxs_intb_lookup_impl(code_ptr, option_intb_njc == 1, true);
}

#endif
