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
#include "latx-sys-hamt-dmw.h"
#include "sys-aot.h"

void latxs_sys_io_register_ir1(void)
{
    latxs_register_ir1(X86_INS_IN);
    latxs_register_ir1(X86_INS_OUT);
}

void latxs_tr_gen_io_check(IR1_INST *ir1,
        IR1_OPND *opnd_io, int data_size)
{
#ifdef CONFIG_SOFTMMU
    int need_check = 0;

    TRANSLATION_DATA *td = lsenv->tr_data;
    if (td->sys.pe && (td->sys.cpl > td->sys.iopl || td->sys.vm86)) {
        need_check = 1;
    }

    if (!need_check) {
        return;
    }

    /* 0. save complete context because exception might be raised */
    helper_cfg_t cfg = default_helper_cfg;
    latxs_tr_gen_call_to_helper_prologue_cfg(cfg);

    /* 1. prepare parameters for helper */

    /* 1.1 arg0: env*/
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                      &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }

    /* 1.2 arg1: IO port address */
    if (ir1_opnd_is_imm(opnd_io)) {
        /* 8-bits imm */
        uint32 port_imm = (int32)(ir1_opnd_uimm(opnd_io) & 0xff);
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg1_ir2_opnd,
                &latxs_zero_ir2_opnd, port_imm);
    } else if (ir1_opnd_is_gpr(opnd_io)) {
        /* 16-bits DX register */
        int port_reg_num  = ir1_opnd_base_reg_num(opnd_io);
        int port_reg_size = ir1_opnd_size(opnd_io);
        lsassertm(port_reg_num == edx_index && port_reg_size == 16,
                "unknown GPR of IO_OPND in check io. IR1 = %p.\n", ir1);
        (void)port_reg_size ; /* to avoid compile warning */
        IR2_OPND port_reg = latxs_ra_alloc_gpr(port_reg_num);
        latxs_append_ir2_opnd2_(lisa_mov16z, &latxs_arg1_ir2_opnd,
                                             &port_reg);
    } else {
        lsassertm(0, "unknown type of IO_OPND in check io. IR1 = %p.\n", ir1);
    }

    /*
     * 2. call helper from target/i386/seg_helper.c
     * helper_check_iob(env, port) size = 8   , 1
     * helper_check_iow(env, port) size = 16  , 2
     * helper_check_iol(env, port) size = 32  , 4
     */
    switch (data_size) {
    case 8:
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg2_ir2_opnd,
                &latxs_zero_ir2_opnd, 1);
        latxs_tr_gen_call_to_helper((ADDR)helper_check_io, HELPER_CHECK_IO);
        break;
    case 16:
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg2_ir2_opnd,
                &latxs_zero_ir2_opnd, 2);
        latxs_tr_gen_call_to_helper((ADDR)helper_check_io, HELPER_CHECK_IO);
        break;
    case 32:
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg2_ir2_opnd,
                &latxs_zero_ir2_opnd, 4);
        latxs_tr_gen_call_to_helper((ADDR)helper_check_io, HELPER_CHECK_IO);
        break;
    default:
        lsassertm(0,
                "Unsupported check IO data size %d. IR1 = %p\n",
                data_size, ir1);
        break;
    }

    /* 3. restore native context */
    latxs_tr_gen_call_to_helper_epilogue_cfg(cfg);
#else
    lsassertm(0, "io in user mode");
#endif
}

void latxs_tr_gen_io_bpt(IR1_INST *ir1,
        IR1_OPND *port, int size)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    if (!(td->sys.flags & HF_IOBPT_MASK)) {
        return;
    }

#ifdef CONFIG_SOFTMMU
    /*
     *  target/i386/bpt_helper.c
     *  void helper_bpt_io(
     *      CPUX86State *env,
     *      uint32_t port,
     *      uint32_t size, 1:8-bit 2:16-bit 4:32-bit 8:64-bit
     *      target_ulong next_eip)
     */

    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);

    /* 1.1 arg0: env */
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                      &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }

    /*
     * 1.2 arg1 : port
     *
     * get I/O port address from opnd1
     *     > imm8 :  8-bits immediate number
     *     > dx   : 16-bits stored in dx register
     */
    if (ir1_opnd_is_imm(port)) {
        uint32_t port_imm = (uint32_t)(ir1_opnd_uimm(port) & 0xff);
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg1_ir2_opnd,
                &latxs_zero_ir2_opnd, port_imm);
    } else if (ir1_opnd_is_gpr(port)) {
        IR2_OPND port_reg = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(port));
        latxs_append_ir2_opnd2_(lisa_mov16z, &latxs_arg1_ir2_opnd,
                                             &port_reg);
    }

    /* 1.3 arg2: size */
    latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg2_ir2_opnd,
            &latxs_zero_ir2_opnd, size);

    /* 1.4 next eip = IR1.address + IR1.size */
    /* PCREL, we should avoid directly storing next_eip */
    ADDRX next_eip_offset = (option_tb_pcrel)  ? td->pc_next - td->pc_save       :
                            (option_page_pcrel)? td->pc_next & ~TARGET_PAGE_MASK : 0;
    ADDRX next_eip = ir1_addr_next(ir1);
    latxs_tr_gen_add_curr_eip_to_ir2(&latxs_arg3_ir2_opnd, next_eip_offset, next_eip, false);

    /* 1.5 call helper */
    latxs_tr_gen_call_to_helper((ADDR)helper_bpt_io, HELPER_BPT_IO);

    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
#else
    if (!(td->sys.flags & HF_IOBPT_MASK)) {
        lsassertm(0, "bpt io in user mode");
    }
#endif
}

void latxs_tr_gen_io_start(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    TranslationBlock *tb = td->curr_tb;

    uint32_t cflags = tb->cflags;
    if (cflags & CF_USE_ICOUNT) {
        IR2_OPND *env = &latxs_env_ir2_opnd;
        IR2_OPND *zero = &latxs_zero_ir2_opnd;
        IR2_OPND tmp = latxs_ra_alloc_itemp();

        int offset = (int)offsetof(X86CPU, parent_obj.can_do_io)
                   - (int)offsetof(X86CPU, env);

        assert(!(offset & 0x3));
        latxs_append_ir2_opnd2i(LISA_ORI, &tmp, zero, 1);
        latxs_append_ir2_opnd2i(LISA_STPTR_W, &tmp, env, offset >> 2);

        latxs_ra_free_temp(&tmp);
    }
}

void latxs_tr_gen_io_end(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    TranslationBlock *tb = td->curr_tb;

    uint32_t cflags = tb->cflags;
    if (cflags & CF_USE_ICOUNT) {
        IR2_OPND *env = &latxs_env_ir2_opnd;
        IR2_OPND *zero = &latxs_zero_ir2_opnd;

        int offset = (int)offsetof(X86CPU, parent_obj.can_do_io)
                   - (int)offsetof(X86CPU, env);

        assert(!(offset & 0x3));
        latxs_append_ir2_opnd2i(LISA_STPTR_W, zero, env, offset >> 2);
    }
}

bool latxs_translate_in(IR1_INST *pir1)
{
#ifdef CONFIG_SOFTMMU
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0); /* dest reg : GPR */
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1); /* I/O port : GPR/IMM */

    /*return b & 1 ? (ot == MO_16 ? MO_16 : MO_32) : MO_8;*/
    uint8_t *b = latxs_ir1_inst_opbytes(pir1);
    int ds = latxs_ir1_data_size(pir1);
    int data_size = (b[0] & 1) ? (ds == 16 ? 16 : 32) : 8;

    lsassertm_illop(ir1_addr(pir1),
            ir1_opnd_is_gpr(opnd0) &&
            (ir1_opnd_is_imm(opnd1) || ir1_opnd_is_gpr(opnd1)),
            "Unrecognized X86_INS_IN %p\n", (void *)pir1);

    IR2_OPND port_reg;
    IR2_OPND label_hack_379 = latxs_ir2_opnd_new_label();
    if (option_risk_in_379 && ir1_opnd_is_gpr(opnd1)) {
        IR2_OPND tmp_reg = latxs_ra_alloc_itemp();
        port_reg = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd1));
        latxs_append_ir2_opnd2_(lisa_mov16z, &tmp_reg,
                                             &port_reg);
        latxs_append_ir2_opnd2i(LISA_XORI, &tmp_reg, &tmp_reg, 0x379);
        latxs_append_ir2_opnd3(LISA_BEQ, &tmp_reg, &latxs_zero_ir2_opnd,  &label_hack_379);
    }

    /* Always check IO before any IO operation */
    latxs_tr_gen_io_check(pir1, opnd1, data_size);

    latxs_tr_gen_io_start();

    /* 0. save native context */
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);

    /* 1. prepare parameters for helper  */
    /*    > arg0 : CPUX86State   *env    */
    /*    > arg1 ; uint32_t      port    */

    /* 1.1 arg0: env */
    latxs_append_ir2_opnd2_(lisa_mov, &latxs_arg0_ir2_opnd,
                                      &latxs_env_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }

    /*
     * 1.2 arg1 : port
     *
     * get I/O port address from opnd1
     *     > imm8 :  8-bits immediate number
     *     > dx   : 16-bits stored in dx register
     */
    uint32   port_imm;

    if (ir1_opnd_is_imm(opnd1)) {
        port_imm = (int32)(ir1_opnd_uimm(opnd1) & 0xff);
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg1_ir2_opnd,
                &latxs_zero_ir2_opnd, port_imm);
    } else if (ir1_opnd_is_gpr(opnd1)) {
        port_reg = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd1));
        latxs_append_ir2_opnd2_(lisa_mov16z, &latxs_arg1_ir2_opnd,
                                             &port_reg);
    }

    /* 2. call the helper_inb/inw/inl */
    switch (data_size) {
    /* target/i386/misc_helper.c */
    case 8:
        latxs_tr_gen_call_to_helper((ADDR)helper_inb, HELPER_INB);
        break;
    case 16:
        latxs_tr_gen_call_to_helper((ADDR)helper_inw, HELPER_INW);
        break;
    case 32:
        latxs_tr_gen_call_to_helper((ADDR)helper_inl, HELPER_INL);
        break;
    default:
        lsassertm(0, "Unsupported X86_INS_OUT data size %d.\n", data_size);
        break;
    }

    /* 3. restore native context */
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);

    /* 4. save $v0 to the correct destination */
    latxs_store_ir2_to_ir1_gpr(&latxs_ret0_ir2_opnd, opnd0);

    /* 5. breakpoint IO */
    latxs_tr_gen_io_bpt(pir1, opnd1, data_size >> 3);

    /* tr_gen_io_end(); */
    if (option_risk_in_379) {
        latxs_append_ir2_opnd1(LISA_LABEL, &label_hack_379);
    }

    return true;
#else
    return false;
#endif
}

bool latxs_translate_out(IR1_INST *pir1)
{
#ifdef CONFIG_SOFTMMU
    IR1_OPND *opnd0 = ir1_get_opnd(pir1, 0); /* I/O port : GPR/IMM */
    IR1_OPND *opnd1 = ir1_get_opnd(pir1, 1); /* src reg  : GPR     */

    /*return b & 1 ? (ot == MO_16 ? MO_16 : MO_32) : MO_8;*/
    uint8_t *b = latxs_ir1_inst_opbytes(pir1);
    int ds = latxs_ir1_data_size(pir1);
    int data_size = (b[0] & 1) ? (ds == 16 ? 16 : 32) : 8;

    lsassertm_illop(ir1_addr(pir1),
            ir1_opnd_is_gpr(opnd1) &&
            (ir1_opnd_is_imm(opnd0) || ir1_opnd_is_gpr(opnd0)),
             "Unrecognized X86_INS_OUT %p\n", (void *)pir1);

    /* Always check IO before any IO operation */
    latxs_tr_gen_io_check(pir1, opnd0, data_size);

    latxs_tr_gen_io_start();

    /* 0. save native context */
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);

    /* 1. prepare parameters for helper */
    /*    > arg0 : CPUX86State *env */
    /*    > arg1 ; uint32_t    port */
    /*    > arg2 : uint32_t    data */

    /*
     * 1.0 arg2 : data
     *
     * convert function may use temp registers, so do this first
     *
     * get data from opnd0
     *    > al  :  8-bits from  al register (low)
     *    > ax  : 16-bits from  ax register
     *    > eax : 32-bits from eax regiser
     */
    IR2_OPND data_reg = latxs_ra_alloc_itemp();
    latxs_load_ir1_gpr_to_ir2(&data_reg, opnd1, EXMode_Z);
    latxs_append_ir2_opnd3(LISA_OR, &latxs_arg2_ir2_opnd,
            &data_reg, &latxs_zero_ir2_opnd);

    /* 1.1 arg0: env */
    latxs_append_ir2_opnd3(LISA_OR, &latxs_arg0_ir2_opnd,
            &latxs_env_ir2_opnd, &latxs_zero_ir2_opnd);
    if (hamt_enabled()) {
        latxs_tr_dmw_out_fix_mem(&latxs_arg0_ir2_opnd);
    }

    /*
     * 1.2 arg1 : port
     *
     * get I/O port address from opnd1
     *     > imm8 :  8-bits immediate number
     *     > dx   : 16-bits stored in dx register
     */
    int32    port_imm;
    IR2_OPND port_reg;

    if (ir1_opnd_is_imm(opnd0)) {
        port_imm = (int32)(ir1_opnd_uimm(opnd0) & 0xff);
        latxs_append_ir2_opnd2i(LISA_ORI, &latxs_arg1_ir2_opnd,
                &latxs_zero_ir2_opnd, port_imm);
    } else if (ir1_opnd_is_gpr(opnd0)) {
        port_reg = latxs_ra_alloc_gpr(ir1_opnd_base_reg_num(opnd0));
        latxs_append_ir2_opnd2_(lisa_mov16z, &latxs_arg1_ir2_opnd,
                                             &port_reg);
    }

    /* 2. call the helper_outb/outw/outl */
    switch (data_size) {
    /* target/i386/misc_helper.c */
    case 8:
        latxs_tr_gen_call_to_helper((ADDR)helper_outb, HELPER_OUTB);
        break;
    case 16:
        latxs_tr_gen_call_to_helper((ADDR)helper_outw, HELPER_OUTW);
        break;
    case 32:
        latxs_tr_gen_call_to_helper((ADDR)helper_outl, HELPER_OUTL);
        break;
    default:
        lsassertm(0, "Unsupported X86_INS_OUT data size %d.\n", data_size);
        break;
    }

    /* 3. restore native context */
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);

    /* 4. breakpoint IO */
    latxs_tr_gen_io_bpt(pir1, opnd0, data_size >> 3);

    /* tr_gen_io_end(); */

    return true;
#else
    return false;
#endif
}
