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
#include "latx-tu.h"
#include "translate.h"
#include "latx-static-codes.h"
#include "latx-intb-sys.h"
#include "latx-misc.h"
#include "latx-cp-counter.h"
#include "latx-shadow-htable.h"
#include "latx-sys-hamt-dmw.h"
#include "latx-shbr.h"
#include "sys-aot.h"
#include "tcg/tcg-split-tb.h"
#include "ir2.h"
#include "qemu/cacheinfo.h"

#define pc_in_same_page(a, b, pm) ((((a)^(b))&(pm))==0)
#include "qemu/cacheflush.h"
#include "mem.h"

int latxs_ir1_is_cross_page(IR1_INST *ir1)
{
    ADDRX pc_base = ir1_addr(ir1);
    ADDRX pc_next = ir1_addr_next(ir1) - 1;
    return !pc_in_same_page(pc_base, pc_next, TARGET_PAGE_MASK);
}

int latxs_ir1_is_cross_page_eip(IR1_INST *ir1, uint64_t csbase)
{
    ADDRX pc_base = ir1_addr(ir1) + csbase;
    ADDRX pc_next = ir1_addr_next(ir1) - 1 + csbase;
    return !pc_in_same_page(pc_base, pc_next, TARGET_PAGE_MASK);
}

static int latxs_is_system_eob(IR1_INST *ir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    ADDRX pc_first = td->sys.pc;
    ADDRX pc_next  = ir1_addr_next(ir1) + td->sys.cs_base;

    if (td->sys.tf) {
        td->can_link = 0;
        td->can_indir_link = 0;
        return 1;
    } else if ((inhibit_irq_hflags() &&
                td->sys.flags & HF_INHIBIT_IRQ_MASK)) {
        td->sys_eob_can_link = 1;
        return 1;
    } else if (td->sys.cflags & CF_USE_ICOUNT) {
        /* string instructions are eob in icount mode */
        if (latxs_ir1_is_string_op(ir1)) {
            /* string in/out are included */
            if (latxs_ir1_has_prefix_rep(ir1) ||
                latxs_ir1_has_prefix_repe(ir1) ||
                latxs_ir1_has_prefix_repne(ir1))
            {
                return 1;
            }
        }
        /* io instructions is eob in icount mode */
        switch (ir1_opcode(ir1)) {
        case X86_INS_RDTSC:
        case X86_INS_RDTSCP:

        case X86_INS_IN:
        case X86_INS_INSB:
        case X86_INS_INSD:
        case X86_INS_INSW:
        case X86_INS_OUT:
        case X86_INS_OUTSB:
        case X86_INS_OUTSD:
        case X86_INS_OUTSW:

            td->sys_eob_can_link = 1;
            return 1;
        default:
            break;
        }

        if (latxs_ir1_is_cross_page_eip(ir1, td->sys.cs_base)) {
            td->sys_eob_can_link = 1;
            td->sys_eob_is_indir = 1;
            return 1;
        }

        if (!pc_in_same_page(pc_first, pc_next, TARGET_PAGE_MASK)) {
            td->sys_eob_can_link = 1;
            td->sys_eob_is_indir = 1;
            return 1;
        }

        return 0;

    } else if (!pc_in_same_page(pc_first, pc_next, TARGET_PAGE_MASK)) {
#ifdef CONFIG_SOFTMMU
        td->sys_eob_use_fsht = 1;
        return 1;
#else
        return 0;
#endif
    } else if (option_split_strins && latxs_ir1_is_string_op(ir1)) {
        /* string in/out are included */
        if (latxs_ir1_has_prefix_rep(ir1) ||
            latxs_ir1_has_prefix_repe(ir1) ||
            latxs_ir1_has_prefix_repne(ir1))
        {
            return 1;
        }
    }

    return 0;
}

static int latxs_ir1_is_hit_breakpoint(IR1_INST *ir1)
{
    CPUState *cpu = current_cpu;
    TRANSLATION_DATA *td = lsenv->tr_data;

    if (current_cpu->singlestep_enabled) {
        return 1;
    }

    target_ulong pc = (target_ulong)(ir1_addr(ir1) + td->sys.cs_base);

    if (unlikely(!QTAILQ_EMPTY(&cpu->breakpoints))) {
        CPUBreakpoint *bp;
        QTAILQ_FOREACH(bp, &cpu->breakpoints, entry) {
            if (bp->pc == pc) {
                td->sys.bp_hit = 1;
                return 1;
            }
        }
    }

    if (cpu->watchpoint_hit) {
        td->sys.bp_hit = 1;
        return 1;
    }

    return 0;
}

static int latxs_ir1_is_lxx_seg(IR1_INST *ir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    switch (ir1_opcode(ir1)) {
    case X86_INS_LDS:
    case X86_INS_LES:
        if (td->sys.pe && !td->sys.vm86 && td->sys.code32) {
            return 1;
        } else {
            return 0;
        }
    case X86_INS_LSS:
        return 1;
    case X86_INS_LFS:
    case X86_INS_LGS:
    default:
        return 0;
    }
}

/* mov to es/cs/ds when PE & !vm86 & codee32 */
static bool latxs_ir1_is_mov_to_seg_eob(IR1_INST *ir1)
{
    if (!latxs_ir1_is_mov_to_seg(ir1)) return false;

    TRANSLATION_DATA *td = lsenv->tr_data;
    int seg = ir1_opnd_base_reg_num(ir1_get_opnd(ir1, 0));

    return td->sys.pe && !td->sys.vm86 && td->sys.code32 &&
            (seg == es_index ||
             seg == cs_index ||
             seg == ds_index);
}

/* pop to es/ds when PE & !vm86 & codee32 */
static bool latxs_ir1_is_pop_to_seg_eob(IR1_INST *ir1)
{
    if (!latxs_ir1_is_pop_to_seg(ir1)) return false;

    TRANSLATION_DATA *td = lsenv->tr_data;
    int seg = ir1_opnd_base_reg_num(ir1_get_opnd(ir1, 0));

    return td->sys.pe && !td->sys.vm86 && td->sys.code32 &&
            (seg == es_index ||
//             seg == cs_index ||
             seg == ds_index);
}

/* mov to ss */
bool latxs_ir1_is_mov_to_seg_ss(IR1_INST *ir1)
{
    return latxs_ir1_is_mov_to_seg(ir1) &&
        ir1_opnd_base_reg_num(ir1_get_opnd(ir1, 0)) == ss_index;
}

/* pop to ss */
bool latxs_ir1_is_pop_to_seg_ss(IR1_INST *ir1)
{
    return latxs_ir1_is_pop_to_seg(ir1) &&
        ir1_opnd_base_reg_num(ir1_get_opnd(ir1, 0)) == ss_index;
}

int latxs_ir1_is_eob_in_sys(IR1_INST *ir1)
{
    if (lsenv->tr_data->sys.cflags & CF_USE_ICOUNT) {
        if (latxs_ir1_is_mov_from_cr(ir1)) {
            lsenv->tr_data->sys_eob_can_link = 1;
            return true;
        }
    } else {
        /* mov to cr8 optimization */
        if (latxs_ir1_is_mov_to_cr8(ir1)) {
            lsenv->tr_data->sys_eob_can_link = 1;
            return true;
        }
    }

    if (latxs_ir1_is_sti(ir1)) {
        if (inhibit_irq_hflags()) { return true; }
    }

    if (latxs_ir1_is_mov_to_seg_ss(ir1) ||
        latxs_ir1_is_pop_to_seg_ss(ir1)) {
        if (inhibit_irq_hflags()) { return true; }
    }
    bool is_eob = false;
    if (!option_pause) {
        is_eob = latxs_ir1_is_repz_nop(ir1) ||
                latxs_ir1_is_pause(ir1);
    }

    return latxs_ir1_is_mov_to_cr(ir1) ||         /* mov cr, src */
           latxs_ir1_is_mov_to_dr(ir1) ||         /* mov dr, src */
           latxs_ir1_is_pop_eflags(ir1) ||        /* popf */
           latxs_ir1_is_sysenter(ir1) ||          /* sysenter */
           latxs_ir1_is_sysexit(ir1) ||           /* sysexit */
           latxs_ir1_is_syscall(ir1) ||           /* syscall */
           latxs_ir1_is_sysret(ir1) ||            /* sysret */
           latxs_ir1_is_rsm(ir1) ||               /* rsm */
//           latxs_ir1_is_sti(ir1) ||               /* sti */
           latxs_ir1_is_mov_to_seg_eob(ir1) ||    /* mov es/cs/ds, src */
           latxs_ir1_is_pop_to_seg_eob(ir1) ||    /* pop ds/es,    src */
           latxs_ir1_is_iret(ir1) ||              /* iret */
           latxs_ir1_is_lmsw(ir1) ||              /* lmsw */
           latxs_ir1_is_wait(ir1) ||              /* wait */
           latxs_ir1_is_retf(ir1) ||              /* ret far */
           latxs_ir1_is_call_far(ir1) ||          /* call far */
           latxs_ir1_is_jump_far(ir1) ||          /* jmp far */
           latxs_ir1_is_clts(ir1) ||              /* clts */
           ir1_opcode(ir1) == X86_INS_INVPCID ||
           latxs_ir1_is_invlpg(ir1) ||            /* invlpg */
           latxs_ir1_is_invlpga(ir1) ||           /* invlpga */
           latxs_ir1_is_lxx_seg(ir1) ||           /* lss, lds/les */
           /* fldenv, frstor, fxrstor, xrstor */
           /*latxs_ir1_contains_fldenv(ir1) ||*/
           /* fninit: reset all to zero */
           /*latxs_ir1_is_fninit(ir1) ||*/
           /* fnsave: reset all to zero after save */
           latxs_ir1_is_fnsave(ir1) ||
           latxs_ir1_is_xsetbv(ir1) ||            /* xsetbv */
           latxs_ir1_is_mwait(ir1) ||             /* mwait */
           latxs_ir1_is_vmrun(ir1) ||             /* vmrun */
           latxs_ir1_is_stgi(ir1) ||              /* stgi */
           latxs_ir1_is_wrmsr(ir1) ||            /* wrmsr */
           latxs_ir1_is_clac(ir1) ||
           latxs_ir1_is_stac(ir1) ||
           latxs_is_system_eob(ir1) ||
           is_eob ||
           latxs_ir1_is_hit_breakpoint(ir1) ||
           latxs_ir1_is_port_io(ir1);
}

void latxs_tr_gen_eob(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    if (!td->need_eob) {
        return;
    }

    if (inhibit_irq_hflags()) {
        if (td->inhibit_irq_in_hflags &&
            !(td->sys.flags & HF_INHIBIT_IRQ_MASK)) {
            latxs_tr_gen_hflags_set(HF_INHIBIT_IRQ_MASK);
        } else {
            latxs_tr_gen_hflags_clr(HF_INHIBIT_IRQ_MASK);
        }
    }
}

static void latxs_tr_gen_exit_tb_update_eip(
        int ignore_eip, ADDRX eip, int opnd_size)
{
    if (!ignore_eip) {
        assert(option_tb_pcrel == 0);
        assert(option_page_pcrel == 0);
        IR2_OPND eip_opnd = latxs_ra_alloc_itemp();
        if (!option_sys_aot) {
            switch (opnd_size) {
                case 32:
                    latxs_load_imm32_to_ir2(&eip_opnd, eip, EXMode_Z);
                    break;
                case 16:
                    latxs_load_imm32_to_ir2(&eip_opnd, eip & 0xffff, EXMode_Z);
                    break;
                case 64:
                    latxs_load_imm64_to_ir2(&eip_opnd, eip);
                    break;
                default:
                    lsassert(0);
                    break;
            }
        } else {
            aot_load_guest_addr_to_ir2(&eip_opnd, eip, LOAD_GUEST_ADDR);
        }

        latxs_tr_gen_store_to_curr_eip(&eip_opnd);

        latxs_ra_free_temp(&eip_opnd);
    }

    /* should correctly set env->eip */
    if (lsenv->tr_data->sys.tf) {
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_single_step,
                default_helper_cfg, HELPER_SINGLE_STEP);
    }
}

void latxs_tr_gen_popf_no_link(IR1_INST *pir1)
{
    latxs_tr_shbr_restore_zero64(pir1->shbr_restore_zero64_after);
#ifdef CONFIG_LATX_PROFILER
    TRANSLATION_DATA *td = lsenv->tr_data;

    if (td->is_tu_mode) {
        add_tu_profiler(td->curr_tb);
    } else {
        la_out1(LISA_BL, &td->tbprofile_rdtime_label);
    }
#endif
    latxs_tr_gen_eob();
    latxs_tr_gen_exit_tb_update_eip(0, ir1_addr_next(pir1), 64);
    latxs_tr_gen_exit_tb_j_context_switch(NULL, 0, 0);
}

static int cpu_hit_breakpoint(void)
{
    CPUState *cpu = env_cpu(lsenv->cpu_state);
    TRANSLATION_DATA *td = lsenv->tr_data;
    if (td->sys.bp_hit || cpu->singlestep_enabled) {
        lsassert(cpu == current_cpu);
        current_cpu->exception_index = EXCP_DEBUG;
        return 1;
    }
    return 0;
}

typedef struct SysEOBContext {
    TRANSLATION_DATA *td;
    TranslationBlock *tb;
    int irqcheck_mode;
    bool can_link;
    bool link_indir;
} SysEOBContext;

/* 0 : do not generate irq check */
#define SYSEOB_IRQCHECK_NONE    0
/* 1 : do irq check on cpu.neg.icount_decr.u32 */
#define SYSEOB_IRQCHECK_NORMAL  1
/* 2 : also do irq check on cpu.interrupt_request */
#define SYSEOB_IRQCHECK_REQ     2

static int latxs_sys_eob_check_common(IR1_INST *pir1)
{
    if (!pir1) { return 1; }
    /* If pir1 is already a normal TB-end instruction,
     * no need another exit codes. */
    if (ir1_is_tb_ending(pir1)) { return 1; }
    return 0;
}

static int latxs_sys_eob_check(SysEOBContext *ctx, IR1_INST *pir1)
{
    if (latxs_sys_eob_check_common(pir1)) return 1;
    if (option_split_strins && latxs_ir1_is_string_op(pir1)) {
        if (latxs_ir1_has_prefix_rep(pir1) ||
            latxs_ir1_has_prefix_repe(pir1) ||
            latxs_ir1_has_prefix_repne(pir1))
        {
            return 1;
        }
    }
    lsassertm(!lsenv->tr_data->tb_too_large_pir1,
            "system EOB should not exist with TB too large.\n");
    return 0;
}

static void latxs_sys_eob_prepare(SysEOBContext *ctx, IR1_INST *pir1)
{
    if (latxs_ir1_is_port_io(pir1)) {
        /* A port access is a device boundary.  Reuse the existing EOB
         * interrupt check so an async exit request (e.g. SeaBIOS writes
         * 0xCF9 and then falls through to int3) or a pending interrupt is
         * taken before the next guest instruction; a clean access still
         * links to the next TB. */
        ctx->irqcheck_mode = SYSEOB_IRQCHECK_REQ;
        return;
    }
    if (option_int_opt == IRQ_CHECK_BLOCK_END &&
        (latxs_ir1_is_mov_to_cr(pir1) || latxs_ir1_is_mov_to_dr(pir1))) {
    //if (latxs_ir1_is_mov_to_cr(pir1) || latxs_ir1_is_mov_to_dr(pir1)) {
    //    if (option_int_opt == IRQ_CHECK_BLOCK_END ||
    //        option_int_opt == IRQ_CHECK_ASYNC_NOTIFY) {
        ctx->irqcheck_mode = SYSEOB_IRQCHECK_NORMAL;
    }
    if ((inhibit_irq_hflags() && ctx->td->inhibit_irq_in_hflags)) {
        /* sti, write ss */
        if (inhibit_irq_hflags_link()) {
            ctx->can_link = true;
            ctx->link_indir = inhibit_irq_hflags_link_indir();
        } else {
            ctx->irqcheck_mode = SYSEOB_IRQCHECK_NONE;
            ctx->can_link = false;
        }
    }
    if ((inhibit_irq_hflags() && ctx->td->sys.flags & HF_INHIBIT_IRQ_MASK)) {
        /* the next inst after sti or write ss */
        if (inhibit_irq_hflags_link()) {
            ctx->irqcheck_mode = SYSEOB_IRQCHECK_REQ;
            ctx->can_link = true;
            ctx->link_indir = inhibit_irq_hflags_link_indir();
        } else {
            ctx->irqcheck_mode = SYSEOB_IRQCHECK_NONE;
            ctx->can_link = false;
        }
    }
}

static void latxs_sys_eob_update_eip(SysEOBContext *ctx, IR1_INST *pir1)
{
    ADDRX next_eip = ir1_addr_next(pir1);
    if (option_lazypc && !ctx->link_indir) {
        ctx->tb->next_pc[0] = next_eip;
    }
    /* SYSCALL applies FMASK before the single-step trap is tested. */
    if (ctx->td->sys.tf && ir1_opcode(pir1) == X86_INS_SYSCALL) {
        IR2_OPND flags = latxs_ra_alloc_itemp();
        IR2_OPND done = latxs_ir2_opnd_new_label();

        la_out2i(LISA_LD_WU, &flags, &la_env, offsetof(CPUX86State, eflags));
        la_out2i(LISA_ANDI, &flags, &flags, TF_MASK);
        la_out2(LISA_BEQZ, &flags, &done);
        latxs_ra_free_temp(&flags);
        latxs_tr_gen_call_to_helper1_cfg((ADDR)helper_single_step,
                default_helper_cfg, HELPER_SINGLE_STEP);
        latxs_append_ir2_opnd1(LISA_LABEL, &done);
        return;
    }
    latxs_tr_gen_exit_tb_update_eip(ctx->td->ignore_eip_update, next_eip, 64);
}

static void latxs_sys_eob_emit_intcheck(SysEOBContext *ctx)
{
    if (ctx->irqcheck_mode) {
        latxs_load_intcheck_to_reg(&la_a3);
        la_out3(LISA_BLT, &la_a3, &la_zero,
                latxs_ir2_interrupt_get_label(ctx->tb, 0));
        if (ctx->irqcheck_mode == SYSEOB_IRQCHECK_REQ) {
            la_out2i(LISA_LD_D, &la_a3, &la_env, offsetof(CPUX86State, interrupt_request_ptr));
            if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&la_a3);
            la_out2i(LISA_LD_WU, &la_a3, &la_a3, 0);
            la_out2 (LISA_BNEZ, &la_a3, latxs_ir2_interrupt_get_label(ctx->tb, 0));
        }
    }
}

static void latxs_sys_eob_emit_intcheck_exit(SysEOBContext *ctx,
        IR2_OPND *tbptr, int link)
{
    if (ctx->irqcheck_mode) {
        latxs_ir2_interrupt_set_label(ctx->tb, 0);
    }
    if (link) {
        latxs_tr_gen_exit_tb_load_tb_addr(tbptr, (ADDR)ctx->tb);
    }
    latxs_tr_gen_exit_tb_j_context_switch(tbptr, link, 0);
}

static void latxs_tr_emit_inline_njc_lookup(TranslationBlock *tb)
{
    bool compact;

    if (!intb_njc_inline_enabled()) {
        return;
    }
    compact = latx_jmp_cache_compact_enabled() &&
              (tb->flags & HF_CS64_MASK);
    gen_latxs_intb_lookup_njc_inline(compact);
}

static void latxs_sys_eob_emit_exit(SysEOBContext *ctx)
{
    IR2_OPND tbptr = latxs_ra_alloc_dbt_arg1(); /* a6($10) */
    bool emitted_exit_code = false;

    if (check_misc_flags(ctx->tb, MF_END_SYSCALL)) {
        /* syscall link handled in latxs_tb_set_jmp_target() */
        latxs_sys_eob_emit_intcheck_exit(ctx, &tbptr, 1);
        return;
    }

    if (ctx->can_link) {
        if (ctx->link_indir) {
            latxs_tr_gen_exit_tb_load_tb_addr(&tbptr, (ADDR)ctx->tb);
            latxs_tr_gen_load_from_curr_eip(&latxs_arg1_ir2_opnd);
            latxs_tr_emit_inline_njc_lookup(ctx->tb);
            int offset = ctx->td->ir2_asm_nr << 2;
            int fid = fsht_flags_to_id(ctx->tb->flags);
            if (option_flaged_sht && fid >= 0 && ctx->td->sys_eob_use_fsht) {
                if (tucp_fast_fsht_check() &&
                    (ctx->tb->flags & HF_CPL_MASK) == HF_CPL_MASK) {
                    tb_sc_table_offset_jmp(ctx->tb, ctx->td->mtcc_id,
                            intb_fsht_lookup_tucpfast[fid],
                            ctx->td->tcg_region_offset, offset, 0,
                            INTB_FSHT_LOOKUP_TUCPFAST);
                } else {
                    tb_sc_table_offset_jmp(ctx->tb,
                            ctx->td->mtcc_id, intb_fsht_lookup[fid],
                            ctx->td->tcg_region_offset,
                            offset, 0, INTB_FSHT_LOOKUP);
                }
            } else {
                if (!intb_njc_inline_enabled() &&
                    latx_jmp_cache_compact_enabled() &&
                    (ctx->tb->flags & HF_CS64_MASK)) {
                    tb_sc_table_offset_jmp(ctx->tb, ctx->td->mtcc_id,
                            intb_lookup_compact, ctx->td->tcg_region_offset,
                            offset, 0, INTB_LOOKUP_COMPACT);
                } else {
                    tb_sc_table_offset_jmp(ctx->tb, ctx->td->mtcc_id,
                            intb_lookup, ctx->td->tcg_region_offset,
                            offset, 0, INTB_LOOKUP);
                }
            }
        } else {
            emitted_exit_code = true;
            latxs_sys_eob_emit_intcheck_exit(ctx, &tbptr, 1);
        }
    }
    if (!emitted_exit_code) {
        latxs_sys_eob_emit_intcheck_exit(ctx, &tbptr, 0);
    }
}

void latxs_tr_gen_sys_eob(IR1_INST *pir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;

    SysEOBContext ctx = {
        .td = td,
        .tb = td->curr_tb,
        .irqcheck_mode = SYSEOB_IRQCHECK_NONE,
        .can_link = td->can_link && td->sys_eob_can_link,
        .link_indir = td->sys_eob_is_indir,
    };

    if (latxs_sys_eob_check(&ctx, pir1)) return;

#ifdef CONFIG_LATX_PROFILER
    if (!ctx.td->is_tu_mode) { la_out1(LISA_BL, &(ctx.td->tbprofile_rdtime_label)); }
#endif

    latxs_tr_gen_eob(); /* inhibit irq hflags */

    latxs_sys_eob_prepare(&ctx, pir1);

    latxs_sys_eob_update_eip(&ctx, pir1);

    latxs_sys_eob_emit_intcheck(&ctx);

    if (ctx.can_link && !ctx.link_indir) {
        latxs_tr_gen_exit_tb_j_tb_link(ctx.tb, 0);
    }

    latxs_sys_eob_emit_exit(&ctx);
}

/* Need fix for aot. */
void latxs_tr_gen_exit_tb_load_tb_addr(IR2_OPND *tbptr, ADDR tb_addr)
{
    if (lsenv && lsenv->tr_data && lsenv->tr_data->curr_tb) {
        TranslationBlock *tb = lsenv->tr_data->curr_tb;
        ADDR code_ptr = (ADDR)tb->tc.ptr;
        ADDR code_off = (ADDR)(lsenv->tr_data->ir2_asm_nr << 2);
        if (option_sys_aot) {
            if (split_tb_base_ok()) {
                tb_addr &= (1ULL << 32) - 1;
            }
            latxs_load_addr_to_ir2(tbptr, tb_addr, LOAD_TB_ADDR);
            return;
        }

        ADDR ins_pc = code_ptr + code_off;
        /* tbptr = ins_pc + offset => offset = tbptr - ins_pc */
        int64_t offset = ((int64_t)tb_addr - (int64_t)ins_pc) >> 2;
        if (int32_in_int20(offset)) {
            latxs_append_ir2_opnd1i(LISA_PCADDI, tbptr, offset);
        } else {
            if (split_tb_base_ok()) {
                latxs_load_imm64_to_ir2(tbptr, tb_addr & ((1ULL << 32) - 1));
            } else {
                latxs_load_imm64_to_ir2(tbptr, tb_addr);
            }
        }
    } else {
        if (split_tb_base_ok()) {
            latxs_load_imm64_to_ir2(tbptr, tb_addr & ((1ULL << 32) - 1));
        } else {
            latxs_load_imm64_to_ir2(tbptr, tb_addr);
        }
    }
}

static void latxs_tb_link_align_inside_cacheline(TranslationBlock *tb)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    uint64_t tcptr = (uint64_t)tb->tc.ptr;
    tcptr += td->ir2_asm_nr << 2;
    assert(qemu_icache_linesize);
    if ((tcptr & (qemu_icache_linesize - 1)) ==
        (qemu_icache_linesize - 4))
    {
        la_out_nop();
    }
}

void latxs_tr_gen_exit_tb_j_tb_link(TranslationBlock *tb, int succ_id)
{
    /* 0. make sure two inst not cross cacheline */
    latxs_tb_link_align_inside_cacheline(tb);

    /* 1. set a label for native code linkage */
    IR2_OPND goto_label_opnd = latxs_ir2_opnd_new_label();

    /* remember where the j instruction position */
    /* reuse jmp_reset_offset[], they will be rewritten in label_dispose */
    /* at that time we can calculate exact offset for this position */
    latxs_append_ir2_opnd1(LISA_LABEL, &goto_label_opnd);
    tb->jmp_reset_offset[succ_id] = goto_label_opnd.val;

    /* point to current j insn addr plus 8 by default, will resolve in */
    /* label_dispose, two instructions for pcaddu18i and jirl patch */
    /* patch may be pcaddu18i and jirl, this b should b to next insn of nop other than nop  */
    /* if nop was interrupt, and may just exec jirl without pcaddu18i  */
    latxs_append_ir2_opnda(LISA_B, 2);
    latxs_append_ir2_opnd0_(lisa_nop);
}

void latxs_tr_gen_exit_tb_j_context_switch(IR2_OPND *tbptr,
        int can_link, int succ_id)
{
    /* jump to context_switch_native_to_bt */
    TRANSLATION_DATA *td = lsenv->tr_data;
    TranslationBlock *tb = td->curr_tb;

    int offset = td->ir2_asm_nr << 2;

    uint32_t mtcc_id = td->mtcc_id;

    if (can_link) {
        if (succ_id) {
            tb_sc_table_offset_jmp(tb, mtcc_id, cs_native_to_bt_ret_n1,
                    td->tcg_region_offset, offset, 0, CS_NATIVE_TO_BT_RET_N1);
        } else {
            tb_sc_table_offset_jmp(tb, mtcc_id, cs_native_to_bt_ret_n0,
                    td->tcg_region_offset, offset, 0, CS_NATIVE_TO_BT_RET_N0);
        }
    } else {
        tb_sc_table_offset_jmp(tb, mtcc_id, cs_native_to_bt_ret_0,
                td->tcg_region_offset, offset, 0, CS_NATIVE_TO_BT_RET_0);
    }
}

void latxs_tr_gen_eob_if_tb_too_large(IR1_INST *pir1)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    TranslationBlock *tb = td->curr_tb;
    int can_link = option_tb_link && !(td->sys.cflags & CF_NO_GOTO_TB);
    IR2_OPND tbptr = latxs_ra_alloc_dbt_arg1();
    int succ_id = 1;

    if (latxs_sys_eob_check_common(pir1)) return ;
    lsassertm(!td->sys_eob_pir1,
            "TB too large should not exist with system EOB.\n");

#ifdef CONFIG_LATX_PROFILER
    if (!td->is_tu_mode) { la_out1(LISA_BL, &(td->tbprofile_rdtime_label)); }
#endif

    latxs_tr_gen_eob();
    if (cpu_hit_breakpoint()) { can_link = 0; }

    latxs_tr_gen_exit_tb_load_tb_addr(&tbptr, (ADDR)tb);

    if (can_link) {
        latxs_tr_gen_exit_tb_j_tb_link(tb, succ_id);
    }

    ADDRX next_eip = ir1_addr_next(pir1);
    if (option_lazypc) {
        tb->next_pc[succ_id] = next_eip;
    }

    if (!option_lazypc || !can_link) {
        latxs_tr_gen_exit_tb_update_eip(td->ignore_eip_update, next_eip, 64);
    }

    latxs_tr_gen_exit_tb_j_context_switch(&tbptr, can_link, succ_id);
}

#ifdef CONFIG_SOFTMMU
static int branch_is_cross_page(IR1_INST *branch, int n)
{
    ADDRX curr_eip = ir1_addr(branch);
    ADDRX next_eip = 0;

    if (ir1_is_call(branch) && !ir1_is_indirect_call(branch)) {
        next_eip = ir1_target_addr(branch);
        return (next_eip >> TARGET_PAGE_BITS) != (curr_eip >> TARGET_PAGE_BITS);
    }

    if (ir1_is_jump(branch) && !ir1_is_indirect_jmp(branch)) {
        next_eip = ir1_target_addr(branch);
        return (next_eip >> TARGET_PAGE_BITS) != (curr_eip >> TARGET_PAGE_BITS);
    }

    if (ir1_is_branch(branch)) {
        if (n == 0) next_eip = ir1_addr_next(branch);
        if (n == 1) next_eip = ir1_target_addr(branch);
        return (next_eip >> TARGET_PAGE_BITS) != (curr_eip >> TARGET_PAGE_BITS);
    }

    return 0;
}

static int branch_target_maybe_cross_page_tb(IR1_INST *branch, int n)
{
    /*ADDRX curr_eip = ir1_addr(branch);*/
    ADDRX next_eip = 0;
    ADDRX next_eip2 = 0;

    if (ir1_is_call(branch) && !ir1_is_indirect_call(branch)) {
        next_eip = ir1_target_addr(branch);
        next_eip2 = next_eip + 16;
        return (next_eip >> TARGET_PAGE_BITS) != (next_eip2 >> TARGET_PAGE_BITS);
    }

    if (ir1_is_jump(branch) && !ir1_is_indirect_jmp(branch)) {
        next_eip = ir1_target_addr(branch);
        next_eip2 = next_eip + 16;
        return (next_eip >> TARGET_PAGE_BITS) != (next_eip2 >> TARGET_PAGE_BITS);
    }

    if (ir1_is_branch(branch)) {
        if (n == 0) next_eip = ir1_addr_next(branch);
        if (n == 1) next_eip = ir1_target_addr(branch);
        next_eip2 = next_eip + 16;
        return (next_eip >> TARGET_PAGE_BITS) != (next_eip2 >> TARGET_PAGE_BITS);
    }

    return 0;
}
#endif

static void tr_gen_branch_save_next_eip(IR1_INST *branch, int n)
{
    int next_eip_size = latxs_ir1_addr_size(branch) << 3;

    ADDRX next_eip = 0;

    TRANSLATION_DATA *td = lsenv->tr_data;
    TranslationBlock *tb = td->curr_tb;

    int doit = 0;
    int ignore_eip = 0;

    if (ir1_is_call(branch) && !ir1_is_indirect_call(branch)) {
        next_eip = ir1_target_addr(branch);
        if (latxs_ir1_addr_size(branch) == 2 ||
            ir1_opnd_size(ir1_get_opnd(branch, 0)) == 16) {
            next_eip_size = 16;
        }
        doit = 1;
    }

    if (ir1_is_jump(branch) && !ir1_is_indirect_jmp(branch)) {
        next_eip = ir1_target_addr(branch);
        next_eip_size = ir1_opnd_size(ir1_get_opnd(branch, 0));
        ignore_eip = td->ignore_eip_update;
        doit = 1;
    }

    if (ir1_is_branch(branch)) {
        if (n == 0) next_eip = ir1_addr_next(branch);
        if (n == 1) next_eip = ir1_target_addr(branch);
        next_eip_size = ir1_opnd_size(ir1_get_opnd(branch, 0));
        doit = 1;
    }

    if (lsenv->tr_data->sys.code64) {
        next_eip_size = 64;
    }

    /* if pcrel, modify pc at end of tb*/
    if ((tb_cflags(tb) & CF_PCREL)) {
        ignore_eip = td->ignore_eip_update;
    }
    if (doit) {
        latxs_tr_gen_exit_tb_update_eip(ignore_eip, next_eip, next_eip_size);
    }
}

static void latxs_tr_pcrel_eip(IR1_INST *branch, int succ_id)
{
    int st_eip = 0;
    ADDRX next_eip = 0;
    TRANSLATION_DATA *td = lsenv->tr_data;
    int next_eip_size = latxs_ir1_addr_size(branch) << 3;
    IR1_OPCODE opcode = ir1_opcode(branch);

    if (ir1_is_branch(branch)) {
        if (succ_id == 0) next_eip = ir1_addr_next(branch);
        if (succ_id == 1) next_eip = ir1_target_addr(branch);
        next_eip_size = ir1_opnd_size(ir1_get_opnd(branch, 0));
        st_eip = 1;
    }
    if (opcode == X86_INS_RET) {
        // process at ret;
        st_eip = 2;
    }
    if (opcode == X86_INS_LJMP) {
        if (td->sys.pe && !td->sys.vm86) {
            // eip update at helper
        } else if (!ir1_opnd_is_mem(ir1_get_opnd(branch, 0))) {
            /* uptate for ljmp imm, mem seems no using */
            next_eip = ir1_opnd_uimm(ir1_get_opnd(branch, 1));
            next_eip_size = ir1_opnd_size(ir1_get_opnd(branch, 1));
            st_eip = 1;
        }
    }
    if (ir1_is_call(branch)) {
        if (ir1_is_indirect_call(branch)) {
            // process at call;
            st_eip = 2;
        } else {
            next_eip = ir1_target_addr(branch);
            if (latxs_ir1_addr_size(branch) == 2 ||
                ir1_opnd_size(ir1_get_opnd(branch, 0)) == 16) {
                next_eip_size = 16;
            }
            st_eip = 1;
        }
    }
    if (ir1_is_jump(branch)) {
        if (ir1_is_indirect_jmp(branch)) {
            // process at jmpin;
            st_eip = 2;
        } else {
            next_eip = ir1_target_addr(branch);
            next_eip_size = ir1_opnd_size(ir1_get_opnd(branch, 0));
            st_eip = 1;
        }
    }

    /*
    * update neip for test
    * for sys.32, cs_base need to be added for pc_save contains cs_base
    */
    switch (next_eip_size) {
    case 64:
        break;
    case 32:
        next_eip &= 0xffffffff;
        break;
    case 16:
        next_eip &= 0xffff;
        break;
    default:
        break;
    }

    int eip_offset = 0;
    if (option_tb_pcrel)
        eip_offset = next_eip - (td->pc_save - td->sys.cs_base);
    else if (option_page_pcrel)
        /* here assume pc_save must not cross page */
        eip_offset = next_eip - (td->curr_page - td->sys.cs_base);
    else
        assert(0);
    /* check during trans, it must be correct
     * however, during running, it might not equal
     */

    if (st_eip) {
        if (st_eip == 1) {
            IR2_OPND value_opnd = latxs_ra_alloc_itemp();
            // cal target eip
            latxs_tr_gen_add_st_curr_eip(&value_opnd, eip_offset, 0);
            latxs_ra_free_temp(&value_opnd);
        } else if (st_eip == 2){
            // do null
        }
        td->ignore_eip_update = 1;
    }
}

#ifdef CONFIG_SOFTMMU

#define CP_CHECK_PREPARE_DATA(tb, td)                    \
    int cpl = tb->flags & 0x3;                           \
    int canptw = (tb->flags & HF_HAMT_EN_MASK) ? 1 : 0;  \
    if (!option_fast_itlb_ptw) canptw = 0;               \
    int offset = td->ir2_asm_nr << 2;                    \
    int mtcc_id = td->mtcc_id;

static inline void tb_gen_cpu_neg_itlb(TranslationBlock *tb, uint64_t addr)
{
    if (option_pcid_cache) {
        int root = (tb->flags & HF_CPL_MASK) ?
            offsetof(CPUX86State, pcid_itlb_user) :
            offsetof(CPUX86State, pcid_itlb_kernel);
        la_out2i(LISA_LD_D, &la_a0, &la_env, root);
        if (option_aot_save) {
            int tc_offset = lsenv->tr_data->ir2_asm_nr << 2;
            add_curr_rel_entry(tb, LOAD_BY_GUEST_ADDR_CPU_NEG_ITLB,
                    tc_offset, 1, -1, addr - tb->pc);
        }
        la_out2i(LISA_LDPTR_D, &la_a0, &la_a0,
                 FAST_ITLB_INDEX(addr) * sizeof(fast_itlb_entry_t) / 4);
        return;
    }

    int off = 0;
    if ((tb->flags & HF_CPL_MASK)) {
        off = (int)offsetof(ArchCPU, neg.itlb_user) -
              (int)offsetof(ArchCPU, env);
    } else {
        off = (int)offsetof(ArchCPU, neg.itlb_knosmap) -
              (int)offsetof(ArchCPU, env);
    }
    off += (FAST_ITLB_INDEX(addr)) * sizeof(fast_itlb_entry_t);
    off = off >> 2;

    IR2_OPND itlb = la_a0;
    if (int32_in_int14(off)) {
        if (option_aot_save) {
            int tc_offset = lsenv->tr_data->ir2_asm_nr << 2;
            add_curr_rel_entry(tb, LOAD_BY_GUEST_ADDR_CPU_NEG_ITLB,
                    tc_offset, 1, -1, addr - tb->pc);
        }
        la_out2i(LISA_LDPTR_D, &itlb, &la_env, off);
    } else {
#ifdef CONFIG_LATX_DEBUG
        printf("%s:%d TB %p addr %lx\n", __func__, __LINE__, tb, addr);
        printf("%s:%d offset %x > 14 bits\n", __func__, __LINE__, off);
#endif
        IR2_OPND tmp = la_a1;
        la_imm(&tmp, (int64_t)off);
        la_out3(LISA_LDX_D, &itlb, &la_env, &tmp);
    }
}

static inline uint64_t latxs_tr_exit_tb_succ_pc(IR1_INST *branch, int succ_id)
{
    if (succ_id || ir1_is_call(branch)) {
        return ir1_target_addr(branch);
    }
    return ir1_addr_next(branch);
}

static inline void __latxs_tr_gen_to_cp_check_page1_id0(IR1_INST *branch,
        TranslationBlock *tb, TRANSLATION_DATA *td, bool intcheck)
{
    tb_gen_cpu_neg_itlb(tb, latxs_tr_exit_tb_succ_pc(branch, 0));
    CP_CHECK_PREPARE_DATA(tb, td);
    if (cpl == 3) {
        if (canptw) {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n0_p0_mmu_user_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N0_P0_MMU_USER_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n0_p0_mmu_user,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N0_P0_MMU_USER);
            }
        } else {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n0_p0_mmu_user_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N0_P0_MMU_USER_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n0_p0_mmu_user,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N0_P0_MMU_USER);
            }
        }
        return;
    }
    if (cpl == 0) {
        if (canptw) {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n0_p0_mmu_knosmap_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N0_P0_MMU_KNOSMAP_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n0_p0_mmu_knosmap,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N0_P0_MMU_KNOSMAP);
            }
        } else {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n0_p0_mmu_knosmap_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N0_P0_MMU_KNOSMAP_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n0_p0_mmu_knosmap,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N0_P0_MMU_KNOSMAP);
            }
        }
        return;
    }
    assert(0);
}
static inline void __latxs_tr_gen_to_cp_check_page1_id1(IR1_INST *branch,
        TranslationBlock *tb, TRANSLATION_DATA *td, bool intcheck)
{
    tb_gen_cpu_neg_itlb(tb, latxs_tr_exit_tb_succ_pc(branch, 1));
    CP_CHECK_PREPARE_DATA(tb, td);
    if (cpl == 3) {
        if (canptw) {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n1_p0_mmu_user_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N1_P0_MMU_USER_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n1_p0_mmu_user,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N1_P0_MMU_USER);
            }
        } else {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n1_p0_mmu_user_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N1_P0_MMU_USER_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n1_p0_mmu_user,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N1_P0_MMU_USER);
            }
        }
        return;
    }
    if (cpl == 0) {
        if (canptw) {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n1_p0_mmu_knosmap_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N1_P0_MMU_KNOSMAP_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n1_p0_mmu_knosmap,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N1_P0_MMU_KNOSMAP);
            }
        } else {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n1_p0_mmu_knosmap_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N1_P0_MMU_KNOSMAP_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n1_p0_mmu_knosmap,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N1_P0_MMU_KNOSMAP);
            }
        }
        return;
    }
    assert(0);
}
static inline void __latxs_tr_gen_to_cp_check_page2_id0(IR1_INST *branch,
        TranslationBlock *tb, TRANSLATION_DATA *td, bool intcheck)
{
    tb_gen_cpu_neg_itlb(tb, latxs_tr_exit_tb_succ_pc(branch, 0));
    CP_CHECK_PREPARE_DATA(tb, td);
    if (cpl == 3) {
        if (canptw) {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n0_p01_mmu_user_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N0_P01_MMU_USER_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n0_p01_mmu_user,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N0_P01_MMU_USER);
            }
        } else {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n0_p01_mmu_user_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N0_P01_MMU_USER_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n0_p01_mmu_user,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N0_P01_MMU_USER);
            }
        }
        return;
    }
    if (cpl == 0) {
        if (canptw) {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n0_p01_mmu_knosmap_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N0_P01_MMU_KNOSMAP_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n0_p01_mmu_knosmap,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N0_P01_MMU_KNOSMAP);
            }
        } else {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n0_p01_mmu_knosmap_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N0_P01_MMU_KNOSMAP_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n0_p01_mmu_knosmap,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N0_P01_MMU_KNOSMAP);
            }
        }
        return;
    }
    assert(0);
}
static inline void __latxs_tr_gen_to_cp_check_page2_id1(IR1_INST *branch,
        TranslationBlock *tb, TRANSLATION_DATA *td, bool intcheck)
{
    tb_gen_cpu_neg_itlb(tb, latxs_tr_exit_tb_succ_pc(branch, 1));
    CP_CHECK_PREPARE_DATA(tb, td);
    if (cpl == 3) {
        if (canptw) {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n1_p01_mmu_user_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N1_P01_MMU_USER_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n1_p01_mmu_user,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N1_P01_MMU_USER);
            }
        } else {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n1_p01_mmu_user_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N1_P01_MMU_USER_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n1_p01_mmu_user,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N1_P01_MMU_USER);
            }
        }
        return;
    }
    if (cpl == 0) {
        if (canptw) {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n1_p01_mmu_knosmap_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N1_P01_MMU_KNOSMAP_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_ptw_n1_p01_mmu_knosmap,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_PTW_N1_P01_MMU_KNOSMAP);
            }
        } else {
            if (intcheck) {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n1_p01_mmu_knosmap_intcheck,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N1_P01_MMU_KNOSMAP_INTCHECK);
            } else {
                tb_sc_table_offset_jmp(tb, mtcc_id,
                        cp_check_n1_p01_mmu_knosmap,
                        td->tcg_region_offset, offset, 1,
                        CP_CHECK_N1_P01_MMU_KNOSMAP);
            }
        }
        return;
    }
    assert(0);
}
static inline void __latxs_tr_gen_to_cp_check_page1(IR1_INST *branch,
        TranslationBlock *tb, TRANSLATION_DATA *td, int succ_id, bool intcheck)
{
    if (!succ_id)   __latxs_tr_gen_to_cp_check_page1_id0(branch, tb, td, intcheck);
    else            __latxs_tr_gen_to_cp_check_page1_id1(branch, tb, td, intcheck);
}
static inline void __latxs_tr_gen_to_cp_check_page2(IR1_INST *branch,
        TranslationBlock *tb, TRANSLATION_DATA *td, int succ_id, bool intcheck)
{
    if (!succ_id)   __latxs_tr_gen_to_cp_check_page2_id0(branch, tb, td, intcheck);
    else            __latxs_tr_gen_to_cp_check_page2_id1(branch, tb, td, intcheck);
}
static inline void latxs_tr_gen_to_cp_check(IR1_INST *branch,
        TranslationBlock *tb, TRANSLATION_DATA *td, int succ_id, int check2page, bool intcheck)
{
    if (check2page) __latxs_tr_gen_to_cp_check_page2(branch, tb, td, succ_id, intcheck);
    else            __latxs_tr_gen_to_cp_check_page1(branch, tb, td, succ_id, intcheck);
}
#endif

/*
 * Exit-TB design
 *
 * This code builds the exit sequence for one TB successor.
 *
 * The exit is classified in two steps:
 *
 *   1. direct vs indirect
 *      - direct   : target is known at translation time
 *      - indirect : target is known only at runtime
 *
 *   2. optional exit modifiers
 *      - cross-page handling
 *      - interrupt handling
 *      - TB-link / lookup availability
 *
 * Main split:
 *
 *   - direct path
 *     Used by direct jmp / direct call / jcc successors.
 *     The code may:
 *       * record next_pc or save next EIP
 *       * emit a TB-link stub
 *       * fall back to cross-page lookup
 *       * finally return through context switch
 *
 *   - indirect path
 *     Used by ret / indirect jmp / indirect call.
 *     The code must save next EIP first, then:
 *       * try runtime lookup if enabled
 *       * otherwise return through context switch
 *
 * Cross-page handling:
 *
 *   Cross-page logic only refines the direct path.
 *   It selects one link-stub shape:
 *       * normal TB-link stub
 *       * cross-page check stub
 *       * save-EIP stub
 *       * inline cross-page check stub
 *       * no stub, use lookup instead
 *
 * Interrupt handling:
 *
 *   - IRQ_CHECK_BLOCK_END
 *       Insert explicit interrupt check around the exit.
 *
 *   - IRQ_CHECK_ASYNC_NOTIFY
 *       Do not insert an explicit check here.
 *       Only mark the exit with tb->int_opt[succ_id].
 */
typedef struct ExitTBContext {
    IR1_OPCODE branch_opcode;
    bool target_is_indirect;
    TRANSLATION_DATA *tr_ctx;
    TranslationBlock *curr_tb;
    bool allow_tb_link;
    uint32_t curr_mtcc_id;
    bool target_crosses_page;
    bool target_maybe_cross_page_block;
    bool target_in_current_tu;
    bool cross_page_intcheck;
} ExitTBContext;

typedef struct ExitTBEmitState {
    bool emitted_interrupt_check;
    int tblink_stub_mode;
    bool loaded_tb_addr;
    bool saved_next_eip;
#ifdef CONFIG_SOFTMMU
    bool cp_inline_p0;
    IR2_OPND cp_inline_fallback;
    IR2_OPND cp_inline_link_entry;
#ifdef CONFIG_LATX_TU
    bool tu_generation_inline;
    IR2_OPND tu_generation_fallback;
    IR2_OPND tu_generation_link_entry;
#endif
#endif
    bool deferred_tu_cold_exit;
} ExitTBEmitState;

static bool latxs_tr_exit_tb_uses_tu_check(const ExitTBContext *ctx)
{
#if defined(CONFIG_SOFTMMU) && defined(CONFIG_LATX_TU)
    return tucp_fast_direct_check() &&
           (tb_is_tu(ctx->curr_tb) || !option_external_tu_stub) &&
           (ctx->curr_tb->flags & HF_CPL_MASK) == HF_CPL_MASK &&
           !ctx->target_is_indirect && !ctx->target_in_current_tu;
#else
    return false;
#endif
}

static void latxs_tr_exit_tb_prepare_direct_interrupt(ExitTBContext *ctx,
        ExitTBEmitState *state, IR1_INST *branch, int succ_id)
{
    if (ctx->target_is_indirect || !succ_id) {
        return;
    }

    if (option_int_opt == IRQ_CHECK_BLOCK_END &&
        (ctx->target_crosses_page ||
        ctx->target_maybe_cross_page_block) &&
        !latxs_tr_exit_tb_uses_tu_check(ctx)) {
        /* do interrupt check in cross-page checking codes */
        ctx->cross_page_intcheck = true;
        return;
    }

    if (option_int_opt == IRQ_CHECK_BLOCK_END) {
        latxs_load_intcheck_to_reg(&la_a3);
        latxs_append_ir2_opnd3(LISA_BLT, &la_a3, &la_zero,
                latxs_ir2_interrupt_get_label(ctx->curr_tb, succ_id));
        state->emitted_interrupt_check = 1;
    } else if (option_int_opt == IRQ_CHECK_ASYNC_NOTIFY) {
        ctx->curr_tb->int_opt[succ_id] = 1;
    }
}

static int latxs_tr_exit_tb_select_link_stub_mode(ExitTBContext *ctx)
{
    int link_stub_mode = TBLINK_STUB_DIRECT;

#ifdef CONFIG_SOFTMMU
    if (latxs_tr_exit_tb_uses_tu_check(ctx)) {
        /* Every direct entry from outside the current TU validates all pages
         * described by the target TB, including ordinary non-TU TBs. */
        if (!ctx->target_in_current_tu) {
            link_stub_mode = TBLINK_STUB_TUCHECK;
        } else {
            if (!ctx->allow_tb_link) {
                link_stub_mode = TBLINK_STUB_NONE;
            }
        }
        return link_stub_mode;
    }
    if (ctx->target_maybe_cross_page_block) {
        if (!cp_tb_enabled()) link_stub_mode = TBLINK_STUB_DIRECT;
        if (cp_tb_link())   link_stub_mode = TBLINK_STUB_DIRECT;
        if (cp_tb_check())  link_stub_mode = TBLINK_STUB_CPCHECK | TBLINK_STUB_CPLABEL;
    } else if (ctx->target_crosses_page) {
        if (!cp_enabled()) ctx->allow_tb_link = 0;
        if (cp_link())   link_stub_mode = TBLINK_STUB_DIRECT;
        if (cp_check())  link_stub_mode = TBLINK_STUB_CPCHECK;
    }
#endif

    if (!ctx->allow_tb_link) {
        link_stub_mode = TBLINK_STUB_NONE;
    }

    return link_stub_mode;
}

#ifdef CONFIG_SOFTMMU
static void latxs_tr_exit_tb_set_cp_label(TranslationBlock *tb, int succ_id,
        const char *name)
{
    IR2_OPND cp_label_opnd = la_new_label();

    la_label_name(cp_label_opnd, name);
    tb->jmp_cp_data[succ_id] = cp_label_opnd.val;
}

static void latxs_tr_exit_tb_emit_cp_inline_p0_check(const ExitTBContext *ctx,
        ExitTBEmitState *state, IR2_OPND *tbptr, IR1_INST *branch, int succ_id)
{
    IR2_OPND expected_tag = la_a2;
    IR2_OPND int_pending = la_a3;
    bool merge_check = ctx->cross_page_intcheck && cp_inline_p0_merged_check();
    int page0_offset = TB_NEXT_DATA_OFF(page_addr_cache[0], succ_id);

    state->cp_inline_p0 = true;
    state->cp_inline_fallback = la_new_label();
    state->cp_inline_link_entry = la_new_label();

    if (ctx->cross_page_intcheck) {
        if (merge_check) {
            latxs_load_intcheck_to_reg(&int_pending);
        } else {
            latxs_load_intcheck_to_reg(&expected_tag);
            la_out3(LISA_BLT, &expected_tag, &la_zero,
                    &state->cp_inline_fallback);
        }
    }

    tb_gen_cpu_neg_itlb(ctx->curr_tb,
            latxs_tr_exit_tb_succ_pc(branch, succ_id));
    if (split_tb_base_ok()) {
        la_out1i(LISA_LU32I_D, tbptr, SPLIT_TB_BASE >> 32);
    }
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(tbptr);
    }
    la_out2i(LISA_LD_D, &expected_tag, tbptr, page0_offset);
    if (merge_check) {
        /*
         * int_pending is either 0 or -1.  In the supported <= 8 GiB guest
         * RAM layout, the 28-bit mixed physical page number cannot be all
         * ones, so the complete page-address mix cannot be UINT64_MAX.
         * Thus -1 is a safe mismatch sentinel while 0 leaves the cached tag
         * unchanged.
         */
        la_out3(LISA_OR, &expected_tag, &expected_tag, &int_pending);
        la_out3(LISA_BNE, &expected_tag, &la_a0,
                &state->cp_inline_fallback);
    } else {
        la_out3(LISA_BNE, &expected_tag, &la_a0, &state->cp_inline_fallback);
    }
    la_label_name(state->cp_inline_link_entry, "cross-page p0 inline hit");
}

static void latxs_tr_exit_tb_emit_cp_inline_p0_fallback(const ExitTBContext *ctx,
        const ExitTBEmitState *state, IR1_INST *branch, int succ_id)
{
    if (!state->cp_inline_p0) {
        return;
    }

    IR2_OPND fallback = state->cp_inline_fallback;
    IR2_OPND link_entry = state->cp_inline_link_entry;

    la_label_name(fallback, "cross-page p0 inline fallback");
    latxs_tr_gen_to_cp_check(branch, ctx->curr_tb, ctx->tr_ctx, succ_id, 0,
            ctx->cross_page_intcheck);
    la_out1(LISA_B, &link_entry);
}

static void latxs_tr_exit_tb_emit_cp_check_stub(const ExitTBContext *ctx,
        ExitTBEmitState *state, IR2_OPND *tbptr, ADDR tb_addr,
        IR1_INST *branch, int succ_id)
{
    int check2page = ctx->target_maybe_cross_page_block;
    if (!check2page) { assert(ctx->target_crosses_page); }

    latxs_tr_gen_exit_tb_load_tb_addr(tbptr, tb_addr);
    if (!check2page && option_link_opt && cp_inline_p0()) {
        latxs_tr_exit_tb_emit_cp_inline_p0_check(ctx, state, tbptr, branch, succ_id);
    } else {
        latxs_tr_gen_to_cp_check(branch, ctx->curr_tb, ctx->tr_ctx, succ_id, check2page,
                ctx->cross_page_intcheck);
    }

    if (option_link_opt) {
        /*
         * (1) bl cross-page check codes
         * (2) nop // to make (3) (4) not span in two cacheline
         * (3) nop === link opt ===
         * (4) j context switch
         */
        latxs_tb_link_align_inside_cacheline(ctx->curr_tb); // (2)
        IR2_OPND label = la_new_label();
        la_label_name(label, "tb link stub opt for cross page");
        ctx->curr_tb->jmp_reset_offset[succ_id] = label.val;
        ctx->curr_tb->link_opt[succ_id] = 1;
        la_out_nop(); // (3)
    } else {
        /*
         * (1) bl cross-page check codes
         * (2) nop // to make (3) (4) not span in two cacheline
         * (3) b 2 // tb link stub
         * (4) nop
         */
        latxs_tr_gen_exit_tb_j_tb_link(ctx->curr_tb, succ_id);
    }
}

static void latxs_tr_gen_to_tu_cp_check(TranslationBlock *tb,
        TRANSLATION_DATA *td, int succ_id, bool intcheck)
{
    int mtcc_id = td->mtcc_id;
    int offset = td->ir2_asm_nr << 2;

    lsassert((tb->flags & HF_CPL_MASK) == HF_CPL_MASK);
    if (succ_id) {
        if (intcheck) {
            tb_sc_table_offset_jmp(tb, mtcc_id,
                    tu_cp_check_n1_mmu_user_intcheck,
                    td->tcg_region_offset, offset, 1,
                    TU_CP_CHECK_N1_MMU_USER_INTCHECK);
        } else {
            tb_sc_table_offset_jmp(tb, mtcc_id,
                    tu_cp_check_n1_mmu_user,
                    td->tcg_region_offset, offset, 1,
                    TU_CP_CHECK_N1_MMU_USER);
        }
    } else {
        if (intcheck) {
            tb_sc_table_offset_jmp(tb, mtcc_id,
                    tu_cp_check_n0_mmu_user_intcheck,
                    td->tcg_region_offset, offset, 1,
                    TU_CP_CHECK_N0_MMU_USER_INTCHECK);
        } else {
            tb_sc_table_offset_jmp(tb, mtcc_id,
                    tu_cp_check_n0_mmu_user,
                    td->tcg_region_offset, offset, 1,
                    TU_CP_CHECK_N0_MMU_USER);
        }
    }
}

static void latxs_tr_exit_tb_emit_checked_link_slot(const ExitTBContext *ctx,
        ExitTBEmitState *state, int succ_id, const char *name)
{
    if (option_link_opt && !state->emitted_interrupt_check) {
        IR2_OPND label = la_new_label();

        latxs_tb_link_align_inside_cacheline(ctx->curr_tb);
        la_label_name(label, name);
        ctx->curr_tb->jmp_reset_offset[succ_id] = label.val;
        ctx->curr_tb->link_opt[succ_id] = 1;
        la_out_nop();
    } else {
        latxs_tr_gen_exit_tb_j_tb_link(ctx->curr_tb, succ_id);
    }
}

#ifdef CONFIG_LATX_TU
static void latxs_tr_exit_tb_emit_tu_memset_cache_check(
        ExitTBEmitState *state, IR2_OPND *offset,
        IR2_OPND *array, IR2_OPND *valid)
{
    la_out2i(LISA_LD_D, array, &la_env,
             offsetof(CPUX86State, tu_generation_array));
    la_out3(LISA_LDX_BU, valid, array, offset);
    la_out2(LISA_BEQZ, valid, &state->tu_generation_fallback);
}

static void latxs_tr_exit_tb_emit_tu_counter_cache_check(
        ExitTBEmitState *state, IR2_OPND *offset,
        IR2_OPND *array, IR2_OPND *generation)
{
    la_out2i(LISA_LD_D, array, &la_env,
             offsetof(CPUX86State, tu_generation_array));
    la_out3(LISA_LDX_HU, generation, array, offset);
    la_out2i(LISA_LD_HU, array, &la_env,
             offsetof(CPUX86State, tu_generation));
    la_out3(LISA_BNE, array, generation,
            &state->tu_generation_fallback);
}

static void latxs_tr_exit_tb_emit_tu_offset_cache_check(
        const ExitTBContext *ctx, ExitTBEmitState *state,
        IR2_OPND *offset, IR2_OPND *array, IR2_OPND *valid, int succ_id)
{
    IR2_OPND patch_slot = la_new_label();

    la_label_name(patch_slot, "TU generation offset patch slot");
    ctx->curr_tb->s_data->tu_generation_offset_patch[succ_id] = patch_slot.val;
    la_out1i(LISA_LU12I_W, offset, 0);
    la_out2i(LISA_ORI, offset, offset, 0);
    latxs_tr_exit_tb_emit_tu_memset_cache_check(
            state, offset, array, valid);
}

static void latxs_tr_exit_tb_emit_tu_generation_check(
        const ExitTBContext *ctx, ExitTBEmitState *state, IR2_OPND *tbptr,
        int succ_id)
{
    IR2_OPND offset = la_a0;
    IR2_OPND array = la_a1;
    IR2_OPND valid = la_a2;
    int offset_offset = TB_TU_LINK_DATA_OFFSET(
                            next_tu_generation_offset[0]) +
                        sizeof(uint64_t) * succ_id;

    state->tu_generation_inline = true;
    state->tu_generation_fallback = la_new_label();
    state->tu_generation_link_entry = la_new_label();

    if (ctx->cross_page_intcheck) {
        latxs_load_intcheck_to_reg(&offset);
        la_out3(LISA_BLT, &offset, &la_zero,
                &state->tu_generation_fallback);
    }

    if (tucp_generation_offset()) {
        latxs_tr_exit_tb_emit_tu_offset_cache_check(
                ctx, state, &offset, &array, &valid, succ_id);
    } else {
        if (split_tb_base_ok()) {
            la_out1i(LISA_LU32I_D, tbptr, SPLIT_TB_BASE >> 32);
        }
        if (hamt_enabled()) {
            latxs_tr_dmw_in_fix_mem(tbptr);
        }
        la_out2i(LISA_LD_D, &offset, tbptr, offset_offset);
        if (tucp_generation_counter()) {
            latxs_tr_exit_tb_emit_tu_counter_cache_check(
                    state, &offset, &array, &valid);
        } else {
            latxs_tr_exit_tb_emit_tu_memset_cache_check(
                    state, &offset, &array, &valid);
        }
    }

    la_label_name(state->tu_generation_link_entry,
                  "TU generation inline hit");
}

static void latxs_tr_exit_tb_emit_tu_generation_fallback(
        const ExitTBContext *ctx, const ExitTBEmitState *state,
        IR2_OPND *tbptr, ADDR tb_addr, int succ_id)
{
    if (!state->tu_generation_inline) {
        return;
    }

    IR2_OPND fallback = state->tu_generation_fallback;
    IR2_OPND link_entry = state->tu_generation_link_entry;

    la_label_name(fallback, "TU generation inline fallback");
    if (tucp_generation_offset()) {
        latxs_tr_gen_exit_tb_load_tb_addr(tbptr, tb_addr);
    }
    latxs_tr_gen_to_tu_cp_check(ctx->curr_tb, ctx->tr_ctx,
            succ_id, ctx->cross_page_intcheck);
    la_out1(LISA_B, &link_entry);
}
#endif

static void latxs_tr_exit_tb_emit_tu_check_stub(const ExitTBContext *ctx,
        ExitTBEmitState *state, IR2_OPND *tbptr, ADDR tb_addr,
        int succ_id)
{
    /* The CP label is after the normal interrupt check, so the TU static
     * checker always uses its no-intcheck entry. */
    assert(!ctx->cross_page_intcheck);

    /* Offset mode has no successor side-data dependency on the hit path. */
    if (!tucp_generation_offset()) {
        latxs_tr_gen_exit_tb_load_tb_addr(tbptr, tb_addr);
        state->loaded_tb_addr = 1;
    }

#ifdef CONFIG_LATX_TU
    if (tucp_generation_enabled()) {
        latxs_tr_exit_tb_emit_tu_generation_check(ctx, state, tbptr,
                                                  succ_id);
    } else
#endif
    {
        latxs_tr_gen_to_tu_cp_check(ctx->curr_tb, ctx->tr_ctx,
                succ_id, ctx->cross_page_intcheck);
    }

    latxs_tr_exit_tb_emit_checked_link_slot(ctx, state, succ_id,
            "tb link stub for TU check");
}

#ifdef CONFIG_LATX_TU
/* Stubs are independent region allocations, retired with their MTCC.
 * Linking runs under the existing TB jump lock. */
typedef struct ExternalTUStub {
    uint32_t code[192];
    uint64_t target;
    uint64_t executions;
    uint64_t successes;
    bool trace;
    struct ExternalTUStub *next;
} ExternalTUStub;

static ExternalTUStub *external_tu_stubs[MTCC_NR];
static pthread_mutex_t external_tu_lock = PTHREAD_MUTEX_INITIALIZER;

void latxs_external_tu_stub_summary(void)
{
    if (!getenv("LATS_EXTERNAL_TU_TRACE")) {
        return;
    }
    uint64_t executed = 0, checked = 0, stubs = 0;
    for (unsigned mtcc = 0; mtcc < MTCC_NR; mtcc++) {
        for (ExternalTUStub *stub = external_tu_stubs[mtcc]; stub;
             stub = stub->next) {
            executed += stub->executions;
            checked += stub->successes;
            stubs++;
        }
    }
    fprintf(stderr, "[External TU] summary stubs=%" PRIu64
            " executed=%" PRIu64 " checked=%" PRIu64 "\n",
            stubs, executed, checked);
}

static ExternalTUStub *external_tu_alloc_locked(unsigned mtcc, const void *source)
{
    if (getenv("LATS_EXTERNAL_TU_FAIL_ALLOC")) {
        return NULL;
    }
    TCGContext *s = tcg_ctx_mtcc[mtcc];
    ExternalTUStub *stub = tcg_region_alloc_data(s, sizeof(*stub));
    if (!stub) {
        return NULL;
    }
    int64_t distance = (intptr_t)stub - (intptr_t)source;
    if ((distance >> 2) != sextract64(distance >> 2, 0, 36)) {
        return NULL;
    }
    memset(stub, 0, sizeof(*stub));
    stub->next = external_tu_stubs[mtcc];
    external_tu_stubs[mtcc] = stub;
    return stub;
}

void latxs_external_tu_stub_reset(unsigned mtcc)
{
    uint64_t executions = 0, successes = 0;
    bool trace = false;
    pthread_mutex_lock(&external_tu_lock);
    for (ExternalTUStub *stub = external_tu_stubs[mtcc]; stub;
         stub = stub->next) {
        executions += stub->executions;
        successes += stub->successes;
        trace |= stub->trace;
    }
    external_tu_stubs[mtcc] = NULL;
    pthread_mutex_unlock(&external_tu_lock);
    if (trace) {
        fprintf(stderr, "[External TU] mtcc=%u executed=%" PRIu64
                " checked=%" PRIu64 " retired\n", mtcc, executions, successes);
    }
}

void latxs_external_tu_stub_unlink(TranslationBlock *tb, int n)
{
    ExternalTUStub *stub = tb->jit_tucheck_external[n];
    if (stub) {
        if (stub->trace && stub->executions) {
            fprintf(stderr, "[External TU] unlink source=%" PRIx64
                    " exit=%d executed=%" PRIu64 " checked=%" PRIu64 "\n",
                    (uint64_t)tb->pc, n, stub->executions, stub->successes);
        }
        /* Old readers may still finish: never reuse this stub before reset. */
        tb->jit_tucheck_external[n] = NULL;
    }
}

static void external_tu_count(uint64_t *counter)
{
    la_imm(&la_a0, (uintptr_t)counter);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(&la_a0);
    }
    la_out2i(LISA_ADDI_D, &la_a1, &la_zero, 1);
    la_out3(LISA_AMADD_D, &la_zero, &la_a0, &la_a1);
}

void *latxs_external_tu_stub(TranslationBlock *tb, int n, void *target)
{
    TRANSLATION_DATA *saved = lsenv->tr_data;
    TRANSLATION_DATA scratch = { 0 };
    unsigned mtcc = curr_mtcc(lsenv->cpu_state);
    pthread_mutex_lock(&external_tu_lock);
    ExternalTUStub *stub = external_tu_alloc_locked(mtcc, tb->tc.ptr);
    pthread_mutex_unlock(&external_tu_lock);
    IR2_OPND done, tbptr;
    uint64_t check;
    int nr;

    if (!stub) {
        return NULL;
    }
    stub->target = (uintptr_t)target;
    stub->trace = getenv("LATS_EXTERNAL_TU_TRACE") != NULL;
    scratch.mtcc_id = mtcc;
    latxs_tr_sync_region_offset(mtcc);
    memcpy(scratch.tcg_region_offset, saved->tcg_region_offset,
           sizeof(scratch.tcg_region_offset));
    lsenv->tr_data = &scratch;
    latxs_tr_init(NULL);
    tbptr = latxs_ra_alloc_dbt_arg1();
    done = la_new_label();
    if (stub->trace) {
        external_tu_count(&stub->executions);
    }
    /* Load the full source pointer: an external PC cannot use TB-relative
     * address materialization or AOT relocation records. */
    la_imm(&tbptr, (uintptr_t)tb);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(&tbptr);
    }
    if (tucp_generation_enabled()) {
        ExitTBEmitState state = { 0 };
        IR2_OPND slow = la_new_label();
        state.tu_generation_fallback = slow;
        la_out2i(LISA_LD_D, &la_a0, &tbptr,
                 TB_TU_LINK_DATA_OFFSET(next_tu_generation_offset[0]) + 8 * n);
        if (tucp_generation_counter()) {
            latxs_tr_exit_tb_emit_tu_counter_cache_check(
                    &state, &la_a0, &la_a1, &la_a2);
        } else {
            latxs_tr_exit_tb_emit_tu_memset_cache_check(
                    &state, &la_a0, &la_a1, &la_a2);
        }
        la_out1(LISA_B, &done);
        la_label_name(slow, "external TU generation miss");
    }
    check = n ? GET_SC_TABLE_OFFSET(mtcc, tu_cp_check_n1_mmu_user,
                                    scratch.tcg_region_offset) :
                GET_SC_TABLE_OFFSET(mtcc, tu_cp_check_n0_mmu_user,
                                    scratch.tcg_region_offset);
    la_imm(&la_a0, check);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(&la_a0);
    }
    la_out2i(LISA_JIRL, &la_ra, &la_a0, 0);
    la_label_name(done, "external TU validated");
    if (stub->trace) {
        external_tu_count(&stub->successes);
    }
    la_imm(&la_a0, (uintptr_t)&stub->target);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(&la_a0);
    }
    la_out2i(LISA_LD_D, &la_a0, &la_a0, 0);
    if (hamt_enabled()) {
        latxs_tr_dmw_in_fix_mem(&la_a0);
    }
    la_out2i(LISA_JIRL, &la_zero, &la_a0, 0);
    lsassert(scratch.ir2_asm_nr <= ARRAY_SIZE(stub->code));
    nr = latxs_tr_ir2_assemble(stub->code);
    latxs_tr_fini();
    mm_free(scratch.ir2_array);
    lsenv->tr_data = saved;
    if (nr < 0) {
        return NULL;
    }
    flush_idcache_range((uintptr_t)stub->code, (uintptr_t)stub->code, nr * 4);
    tb->jit_tucheck_external[n] = stub;
    if (stub->trace) {
        fprintf(stderr, "[External TU] linked source=%" PRIx64
                " exit=%d target=%p stub=%p instructions=%d\n",
                (uint64_t)tb->pc, n, target, stub->code, nr);
    }
    return stub->code;
}
#endif
#endif

#ifdef CONFIG_LATX_TU
static bool latxs_tr_exit_tb_tu_cold_exit_eligible(
        const ExitTBContext *ctx,
        const ExitTBEmitState *state, int succ_id)
{
    TranslationBlock *tb = ctx->curr_tb;

    return tb_is_tu(tb) &&
        tb->s_data->tu_cold_exit_offset[succ_id] == TU_COLD_EXIT_DIRECT_PENDING &&
        TBLINK_STUB_MODE(state->tblink_stub_mode) == TBLINK_STUB_DIRECT &&
        ctx->allow_tb_link && !ctx->target_is_indirect &&
        !ctx->target_crosses_page &&
        !ctx->target_maybe_cross_page_block;
}

static void latxs_tr_gen_exit_tb_j_tb_link_cold(TranslationBlock *tb,
        int succ_id, IR2_OPND *cold_exit_label)
{
    latxs_tb_link_align_inside_cacheline(tb);

    IR2_OPND goto_label_opnd = latxs_ir2_opnd_new_label();
    latxs_append_ir2_opnd1(LISA_LABEL, &goto_label_opnd);
    tb->jmp_reset_offset[succ_id] = goto_label_opnd.val;
    tb->link_opt[succ_id] = 1;

    latxs_append_ir2_opnd1(LISA_B, cold_exit_label);
    latxs_append_ir2_opnd0_(lisa_nop);
}
#endif

static void latxs_tr_exit_tb_emit_link_stub(const ExitTBContext *ctx,
        ExitTBEmitState *state, IR2_OPND *tbptr, ADDR tb_addr,
        IR1_INST *branch, int succ_id)
{
    if (!ctx->allow_tb_link || ctx->target_is_indirect) {
        return;
    }

    switch (TBLINK_STUB_MODE(state->tblink_stub_mode)) {
    case TBLINK_STUB_DIRECT:
#ifdef CONFIG_LATX_TU
        if (latxs_tr_exit_tb_tu_cold_exit_eligible(
                ctx, state, succ_id)) {
            IR2_OPND cold_exit_label = latxs_ir2_opnd_new_label();
            la_label_name(cold_exit_label, "TU direct cold exit");
            ctx->curr_tb->s_data->tu_cold_exit_offset[succ_id] = cold_exit_label.val;
            state->deferred_tu_cold_exit = true;
            latxs_tr_gen_exit_tb_j_tb_link_cold(
                    ctx->curr_tb, succ_id, &cold_exit_label);
            if (state->emitted_interrupt_check) {
                latxs_ir2_interrupt_set_label(ctx->curr_tb, succ_id);
                latxs_append_ir2_opnd1(LISA_B, &cold_exit_label);
            }
            break;
        }
#endif
        if (option_link_opt && !state->emitted_interrupt_check) {
            IR2_OPND label = la_new_label();

            latxs_tb_link_align_inside_cacheline(ctx->curr_tb);
            la_label_name(label, "tb link stub opt");
            ctx->curr_tb->jmp_reset_offset[succ_id] = label.val;
            ctx->curr_tb->link_opt[succ_id] = 1;
        } else {
            latxs_tr_gen_exit_tb_j_tb_link(ctx->curr_tb, succ_id);
        }
        break;
#ifdef CONFIG_SOFTMMU
    case TBLINK_STUB_CPCHECK:
        if (TBLINK_STUB_NEED_CPLABEL(state->tblink_stub_mode)) {
            latxs_tr_exit_tb_set_cp_label(ctx->curr_tb, succ_id,
                    "label for cross-page tb (mode cpcheck)");
        }
        state->loaded_tb_addr = 1;
        latxs_tr_exit_tb_emit_cp_check_stub(ctx, state, tbptr, tb_addr,
                                            branch, succ_id);
        break;
    case TBLINK_STUB_TUCHECK:
        latxs_tr_exit_tb_set_cp_label(ctx->curr_tb, succ_id,
                "label for TU check bypass");
        latxs_tr_exit_tb_emit_tu_check_stub(ctx, state, tbptr, tb_addr,
                                            succ_id);
        break;
#endif
    default:
        break;
    }
}

static void latxs_tr_exit_tb_save_direct_next_pc(const ExitTBContext *ctx,
        const ExitTBEmitState *state, IR1_INST *branch, int succ_id)
{
    if (option_lazypc) {
        if (succ_id || ir1_is_call(branch)) {
            ctx->curr_tb->next_pc[succ_id] = ir1_target_addr(branch);
        } else {
            ctx->curr_tb->next_pc[succ_id] = ir1_addr_next(branch);
        }
        int neips = ir1_opnd_size(ir1_get_opnd(branch, 0));
        if (!ctx->tr_ctx->sys.code64 && neips != 64) {
            ctx->curr_tb->next_pc[succ_id] &= ((1ULL << neips) - 1ULL);
        }
        return;
    }

    if (!state->saved_next_eip || state->emitted_interrupt_check) {
        tr_gen_branch_save_next_eip(branch, succ_id);
    }
}

static void latxs_tr_exit_tb_emit_direct_action(const ExitTBContext *ctx,
        const ExitTBEmitState *state, IR2_OPND *tbptr, ADDR tb_addr,
        IR1_INST *branch, int succ_id)
{
    if (state->deferred_tu_cold_exit) {
        if (option_lazypc) {
            latxs_tr_exit_tb_save_direct_next_pc(ctx, state, branch, succ_id);
        }
        return;
    }

    latxs_tr_shbr_restore_zero64(
        branch->shbr_restore_zero64_exit[succ_id]);
    latxs_tr_exit_tb_save_direct_next_pc(ctx, state, branch, succ_id);

    if ((ctx->allow_tb_link && !state->loaded_tb_addr) || state->emitted_interrupt_check) {
        latxs_tr_gen_exit_tb_load_tb_addr(tbptr, tb_addr);
    }
    if (option_lazypc && !ctx->allow_tb_link) {
        tr_gen_branch_save_next_eip(branch, succ_id);
    }
    latxs_tr_gen_exit_tb_j_context_switch(tbptr, ctx->allow_tb_link, succ_id);
#ifdef CONFIG_SOFTMMU
    latxs_tr_exit_tb_emit_cp_inline_p0_fallback(ctx, state, branch, succ_id);
#ifdef CONFIG_LATX_TU
    latxs_tr_exit_tb_emit_tu_generation_fallback(ctx, state, tbptr, tb_addr,
                                                 succ_id);
#endif
#endif
}

typedef enum ExitTBContextSwitchAction {
    EXIT_TB_CS_ACTION_UNDEF = 0,
    EXIT_TB_CS_ACTION_DEFAULT,
    EXIT_TB_CS_ACTION_INDIR,
} ExitTBContextSwitchAction;

static ExitTBContextSwitchAction latxs_tr_exit_tb_get_cs_action(
        const ExitTBContext *ctx, IR1_INST *branch)
{
    if (ir1_is_branch(branch)) {
        return EXIT_TB_CS_ACTION_DEFAULT;
    }
    if (ctx->branch_opcode == X86_INS_RET) {
        return EXIT_TB_CS_ACTION_INDIR;
    }
    if (ir1_is_call(branch)) {
        return ir1_is_indirect_call(branch) ?
                EXIT_TB_CS_ACTION_INDIR :
                EXIT_TB_CS_ACTION_DEFAULT;
    }
    if (ir1_is_jump(branch)) {
        return ir1_is_indirect_jmp(branch) ?
                EXIT_TB_CS_ACTION_INDIR :
                EXIT_TB_CS_ACTION_DEFAULT;
    }
    return EXIT_TB_CS_ACTION_UNDEF;
}

#ifdef CONFIG_LATX_TU
static bool latxs_tr_exit_tb_use_tu_indirect_inline_exit(
        const ExitTBContext *ctx, IR1_INST *branch, int succ_id)
{
    TranslationBlock *tb = ctx->curr_tb;
    bool supported = ir1_is_indirect_call(branch) ||
        ir1_is_indirect_jmp(branch);

    if (!(tb_is_tu(tb) &&
          tb->s_data->tu_cold_exit_offset[succ_id] ==
              TU_INDIRECT_EXIT_INLINE_PENDING &&
          option_int_opt == IRQ_CHECK_BLOCK_END &&
          ctx->tr_ctx->opt_can_link && ctx->tr_ctx->opt_can_indir_link &&
          supported)) {
        return false;
    }

    tb->s_data->tu_cold_exit_offset[succ_id] = TU_COLD_EXIT_INVALID;
    return true;
}
#endif

static void latxs_tr_exit_tb_emit_indirect_context_switch(
        const ExitTBContext *ctx, int succ_id)
{
    (void)ctx;
    latxs_tr_gen_intb_sync_gpc();
    latxs_tr_gen_exit_tb_j_context_switch(NULL, 0, succ_id);
}

static void latxs_tr_exit_tb_emit_indirect_interrupt(const ExitTBContext *ctx,
        int succ_id, bool inline_tu_exit)
{
    if (ctx->branch_opcode == X86_INS_RET) {
        return;
    }

    if (option_int_opt == IRQ_CHECK_BLOCK_END) {
        IR2_OPND intop = latxs_ra_alloc_itemp();

        latxs_load_intcheck_to_reg(&intop);
        if (inline_tu_exit) {
            IR2_OPND lookup_label = latxs_ir2_opnd_new_label();

            latxs_append_ir2_opnd3(LISA_BGE, &intop, &la_zero,
                    &lookup_label);
            latxs_ra_free_temp(&intop);
            latxs_tr_exit_tb_emit_indirect_context_switch(ctx, succ_id);
            latxs_append_ir2_opnd1(LISA_LABEL, &lookup_label);
        } else {
            latxs_append_ir2_opnd3(LISA_BLT, &intop, &la_zero,
                    latxs_ir2_interrupt_get_label(ctx->curr_tb, succ_id));
            latxs_ra_free_temp(&intop);
        }
    } else if (option_int_opt == IRQ_CHECK_ASYNC_NOTIFY) {
        ctx->curr_tb->int_opt[succ_id] = 1;
    }
}

static void latxs_tr_exit_tb_emit_indirect_lookup(const ExitTBContext *ctx)
{
    int offset;
    int fid = fsht_flags_to_id(ctx->curr_tb->flags);

    latxs_tr_emit_inline_njc_lookup(ctx->curr_tb);
    offset = ctx->tr_ctx->ir2_asm_nr << 2;

    if (option_flaged_sht && fid >= 0) {
        if (tucp_fast_fsht_check() &&
            (ctx->curr_tb->flags & HF_CPL_MASK) == HF_CPL_MASK) {
            tb_sc_table_offset_jmp(ctx->curr_tb, ctx->curr_mtcc_id,
                    intb_fsht_lookup_tucpfast[fid],
                    ctx->tr_ctx->tcg_region_offset, offset, 0,
                    INTB_FSHT_LOOKUP_TUCPFAST);
        } else {
            tb_sc_table_offset_jmp(ctx->curr_tb, ctx->curr_mtcc_id,
                    intb_fsht_lookup[fid], ctx->tr_ctx->tcg_region_offset,
                    offset, 0, INTB_FSHT_LOOKUP);
        }
    } else {
        if (!intb_njc_inline_enabled() &&
            latx_jmp_cache_compact_enabled() &&
            (ctx->curr_tb->flags & HF_CS64_MASK)) {
            tb_sc_table_offset_jmp(ctx->curr_tb, ctx->curr_mtcc_id,
                    intb_lookup_compact, ctx->tr_ctx->tcg_region_offset,
                    offset, 0, INTB_LOOKUP_COMPACT);
        } else {
            tb_sc_table_offset_jmp(ctx->curr_tb, ctx->curr_mtcc_id,
                    intb_lookup, ctx->tr_ctx->tcg_region_offset,
                    offset, 0, INTB_LOOKUP);
        }
    }
}

static void latxs_tr_exit_tb_emit_indirect_slowpath(const ExitTBContext *ctx,
        int succ_id)
{
    if (option_int_opt != IRQ_CHECK_BLOCK_END) {
        return;
    }

    if (ctx->branch_opcode != X86_INS_RET) {
        latxs_ir2_interrupt_set_label(ctx->curr_tb, succ_id);
        latxs_tr_exit_tb_emit_indirect_context_switch(ctx, succ_id);
    }
#ifndef CONFIG_RAS_DISABLE_CPL0
    {
        TranslationBlock *tb = ctx->tr_ctx->curr_tb;

        if (!(tb->flags & HF_CPL_MASK) && ctx->branch_opcode == X86_INS_RET) {
            latxs_ir2_interrupt_set_label(tb, succ_id);
            latxs_tr_exit_tb_emit_indirect_context_switch(ctx, succ_id);
        }
    }
#endif
}

static void latxs_tr_exit_tb_emit_indirect_action(const ExitTBContext *ctx,
        const ExitTBEmitState *state, IR1_INST *branch, int succ_id)
{
    if (!state->saved_next_eip) {
        tr_gen_branch_save_next_eip(branch, succ_id);
    }

    if (!ctx->tr_ctx->opt_can_link || !ctx->tr_ctx->opt_can_indir_link) {
        latxs_tr_exit_tb_emit_indirect_context_switch(ctx, succ_id);
        return;
    }

#ifdef CONFIG_LATX_TU
    bool inline_tu_exit =
        latxs_tr_exit_tb_use_tu_indirect_inline_exit(
            ctx, branch, succ_id);
#else
    bool inline_tu_exit = false;
#endif

    latxs_tr_exit_tb_emit_indirect_interrupt(
            ctx, succ_id, inline_tu_exit);
    latxs_tr_exit_tb_emit_indirect_lookup(ctx);
    if (!inline_tu_exit) {
        latxs_tr_exit_tb_emit_indirect_slowpath(ctx, succ_id);
    }
}

static void latxs_tr_exit_tb_prepare(ExitTBContext *ctx,
        ExitTBEmitState *state, IR2_OPND *tbptr, ADDR tb_addr,
        IR1_INST *branch, int succ_id)
{
    /* debug support : when hit break point, do NOT TB-link */
    if (cpu_hit_breakpoint()) {
        ctx->allow_tb_link = 0;
    }

#ifdef CONFIG_LATX_PROFILER
    if (!ctx->tr_ctx->is_tu_mode)
    {
        la_out1(LISA_BL, &(ctx->tr_ctx->tbprofile_rdtime_label));
    }
#endif

    /*
     * <A> EOB end-of-tb worker
     *
     * for branch instructions, EOB will be called before branch
     * since it contains two exit ports.
     */
    if (!ir1_is_branch(branch)) {
        latxs_tr_gen_eob();
    }

    /* situation for generate update pcrel
     *        | branch| jmp  | call | ret | ljmp|
     * ---------------------------------------------
     *   dir  |   1   |  1   |   1  | 0   |  1  |
     *  indir |   0   |  1   |   1  | 1   |  0  |
     *   next |              |   1  |
     * for direct, get target here and st to neip
     * for indirect, mov target to sp in tr_in
     */
    if ((tb_cflags(ctx->curr_tb) & CF_PCREL)) {
        latxs_tr_pcrel_eip(branch, succ_id);
    }

    ctx->target_is_indirect = ir1_is_indirect_inst(branch);

#ifdef CONFIG_SOFTMMU
    ctx->target_crosses_page = branch_is_cross_page(branch, succ_id);
    ctx->target_maybe_cross_page_block = branch_target_maybe_cross_page_tb(branch, succ_id);
    if (ctx->target_in_current_tu) {
        ctx->target_crosses_page = false;
        ctx->target_maybe_cross_page_block = false;
    }
#endif

    if (ctx->target_crosses_page) {
        add_misc_flags(ctx->curr_tb, succ_id ? MF_NTB_CROSSPAGE_1 : MF_NTB_CROSSPAGE_0);
    }

    latxs_tr_exit_tb_prepare_direct_interrupt(ctx, state, branch, succ_id);

    state->tblink_stub_mode = latxs_tr_exit_tb_select_link_stub_mode(ctx);
    latxs_tr_exit_tb_emit_link_stub(ctx, state, tbptr, tb_addr, branch, succ_id);

    if (state->emitted_interrupt_check &&
        !state->deferred_tu_cold_exit) {
        latxs_ir2_interrupt_set_label(ctx->curr_tb, succ_id);
    }
}

static void latxs_tr_exit_tb_finish(const ExitTBContext *ctx,
        ExitTBEmitState *state, IR2_OPND *tbptr, ADDR tb_addr,
        IR1_INST *branch, int succ_id)
{
    switch (latxs_tr_exit_tb_get_cs_action(ctx, branch)) {
    case EXIT_TB_CS_ACTION_DEFAULT: /* 直接跳转 */
        latxs_tr_exit_tb_emit_direct_action(ctx, state, tbptr, tb_addr,
                                            branch, succ_id);
        break;
    case EXIT_TB_CS_ACTION_INDIR:
        latxs_tr_exit_tb_emit_indirect_action(ctx, state, branch, succ_id);
        break;
    default:
        lsassertm(0, "not implement.\n");
        break;
    }
}

#ifdef CONFIG_LATX_TU
void latxs_tr_gen_tu_cold_exits(uint32_t tb_num_in_tu,
                                  TranslationBlock **tb_list)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    TranslationBlock *saved_tb = td->curr_tb;
    IR1_INST *saved_ir1 = td->ir1_inst_array;
    int saved_ignore_eip = td->ignore_eip_update;
    int saved_code64 = td->sys.code64;
    TranslationBlock **cold_order;
    uint32_t i;

    cold_order = g_new(TranslationBlock *, tb_num_in_tu);
    memcpy(cold_order, tb_list, sizeof(*cold_order) * tb_num_in_tu);
    for (i = 1; i < tb_num_in_tu; i++) {
        TranslationBlock *key = cold_order[i];
        uint32_t j = i;

        while (j > 0 && cold_order[j - 1]->pc > key->pc) {
            cold_order[j] = cold_order[j - 1];
            j--;
        }
        cold_order[j] = key;
    }

    for (i = 0; i < tb_num_in_tu; i++) {
        TranslationBlock *tb = cold_order[i];
        IR1_INST *branch = (IR1_INST *)tb->s_data->ir1 + tb->icount - 1;
        int succ_id;

        td->curr_tb = tb;
        td->ir1_inst_array = tb->s_data->ir1;
        td->ignore_eip_update = tb->s_data->ignore_eip_update;
        td->sys.code64 = (tb->flags >> HF_CS64_SHIFT) & 1;

        for (succ_id = 0; succ_id < 2; succ_id++) {
            uint32_t cold_exit = tb->s_data->tu_cold_exit_offset[succ_id];
            IR2_OPND cold_exit_label;
            IR2_OPND tbptr;

            if (cold_exit == TU_COLD_EXIT_INVALID ||
                cold_exit == TU_COLD_EXIT_DIRECT_PENDING ||
                cold_exit == TU_INDIRECT_EXIT_INLINE_PENDING) {
                continue;
            }

            if (cold_exit == TU_COLD_EXIT_JMP_INTERRUPT_DEFERRED) {
                latxs_ir2_interrupt_set_label(tb, succ_id);
                latxs_tr_shbr_restore_zero64(
                    branch->shbr_restore_zero64_exit[succ_id]);
                tr_gen_branch_save_next_eip(branch, succ_id);
                latxs_tr_gen_exit_tb_j_context_switch(NULL, 0, succ_id);
                tb->s_data->tu_cold_exit_offset[succ_id] = TU_COLD_EXIT_INVALID;
                continue;
            }

            cold_exit_label = latxs_ir2_opnd_new(
                    IR2_OPND_LABEL, cold_exit);
            latxs_append_ir2_opnd1(LISA_LABEL, &cold_exit_label);
            latxs_tr_shbr_restore_zero64(
                branch->shbr_restore_zero64_exit[succ_id]);
            if (!option_lazypc) {
                tr_gen_branch_save_next_eip(branch, succ_id);
            }
            tbptr = latxs_ra_alloc_dbt_arg1();
            latxs_tr_gen_exit_tb_load_tb_addr(&tbptr, (ADDR)tb);
            latxs_tr_gen_exit_tb_j_context_switch(&tbptr, 1, succ_id);
        }
    }

    td->curr_tb = saved_tb;
    td->ir1_inst_array = saved_ir1;
    td->ignore_eip_update = saved_ignore_eip;
    td->sys.code64 = saved_code64;
    g_free(cold_order);
}
#endif

void latxs_tr_generate_exit_tb(IR1_INST *branch, int succ_id)
{
    ExitTBContext ctx = {
        .branch_opcode = ir1_opcode(branch),
        .target_is_indirect = false,
        .tr_ctx = lsenv->tr_data,
        .curr_tb = lsenv->tr_data->curr_tb,
        .allow_tb_link = lsenv->tr_data->can_link,
        .curr_mtcc_id = lsenv->tr_data->mtcc_id,
        .target_crosses_page = false,
        .target_maybe_cross_page_block = false,
        .target_in_current_tu = false,
        .cross_page_intcheck = false,
    };
#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
    if (tucp_fast_tu_direct() && ctx.tr_ctx->is_tu_mode &&
        !ir1_is_indirect_inst(branch))
    {
        TranslationBlock *target_tb;

        if (ir1_is_call(branch)) {
            target_tb = tu_tree_lookup(ir1_target_addr(branch));
            ctx.target_in_current_tu = target_tb != NULL;
        } else {
            target_tb = ctx.curr_tb->s_data->next_tb[succ_id];
            ctx.target_in_current_tu = target_tb &&
                                       tb_is_in_tb_list(target_tb);
        }
    }
#endif
    ExitTBEmitState state = { 0 };
    IR2_OPND tbptr = latxs_ra_alloc_dbt_arg1(); /* t8($24) */
    ADDR tb_addr = (ADDR)ctx.curr_tb;

    latxs_tr_exit_tb_prepare(&ctx, &state, &tbptr, tb_addr, branch, succ_id);
    latxs_tr_exit_tb_finish(&ctx, &state, &tbptr, tb_addr, branch, succ_id);
#ifdef CONFIG_LATX_TU
    if (ctx.curr_tb->s_data->tu_cold_exit_offset[succ_id] ==
            TU_COLD_EXIT_DIRECT_PENDING ||
        ctx.curr_tb->s_data->tu_cold_exit_offset[succ_id] ==
            TU_INDIRECT_EXIT_INLINE_PENDING) {
        ctx.curr_tb->s_data->tu_cold_exit_offset[succ_id] = TU_COLD_EXIT_INVALID;
    }
#endif
}
