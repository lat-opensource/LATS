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
#include "sys-excp.h"
#include "latx-ras.h"
#include "latx-tr.h"
#include "latx-misc.h"
#include "latx-native-asm.h"
#include "loongarch-extcontext.h"
#include "latx-sys-hamt-dmw.h"
#include "sys-aot.h"
#include "latx-static-codes.h"
#include "hamt.h"
#include "qemu/cacheflush.h"
#include "qemu/log.h"
#include "accel/tcg/tb-context.h"
#include "exec/exec-all.h"
#ifdef CONFIG_LATX_TU
#include "latx-tu.h"
#endif

int top_mask;

#define RAS_SLOW_UNLINK_BRANCH_INSNS 5

inline uint32_t get_ras_cpuindex(TranslationBlock *tb)
{
    return get_mask_from_miscflags(tb, MF_RAS_CPU_MASK, MF_RAS_CPU_SHIFT);
}

inline void set_ras_cpuindex(TranslationBlock *tb, uint32_t index)
{
    set_mask_to_miscflags(tb, index, MF_RAS_CPU_MASK,
            MF_RAS_CPU_SHIFT);
}

void latxs_ras_init(CPUState *cpu)
{
    if (option_rastoptp) {
        G_STATIC_ASSERT(LATX_RAS_TP_SIZE == RAS_SIZE);
        G_STATIC_ASSERT(offsetof(LatxRasTPState, top) == offsetof(Soft_RAS, top));
        G_STATIC_ASSERT(offsetof(LatxRasTPState, patching_tb) ==
                        offsetof(Soft_RAS, patching_tb));
        G_STATIC_ASSERT(offsetof(CPUX86State, ras_tp.stack) + 8 <= 32764);
        G_STATIC_ASSERT((offsetof(CPUX86State, ras_tp) & 15) == 0);
        CPUX86State *env = cpu->env_ptr;
        memset(&env->ras_tp, 0, sizeof(env->ras_tp));
        cpu->s_ras = (Soft_RAS *)&env->ras_tp;
        top_mask = LATX_RAS_TP_SIZE - 1;
        return;
    }
    void *addr;
    void *guardpage;
    size_t page_size = getpagesize();
    int pagenum = 2;

    addr = mmap(NULL, pagenum * page_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        option_soft_ras = 0;
    }
    memset(addr, 0, pagenum * page_size);

    guardpage = addr + (page_size * (pagenum - 1));
    *(uint64_t *)guardpage = 0xdeadbeef;
    if (mprotect(guardpage, page_size, PROT_READ) == -1) {
        munmap(addr, page_size * pagenum);
        option_soft_ras = 0;
        cpu->s_ras = NULL;
        return;
    }

    cpu->s_ras = addr;

    if (RAS_SIZE & (RAS_SIZE - 1)) {
        fprintf(stderr, "[RAS-WARNING] RAS_SIZE must be a power of 2.\n");
    }
    if (RAS_SIZE > 512) {
        fprintf(stderr, "[RAS-WARNING] RAS_SIZE must not exceed 4096.\n");
    }
    top_mask = RAS_SIZE - 1;
}

static inline uint32_t *latxs_ras_inst_ptr(TranslationBlock *tb)
{
    uintptr_t inst_ptr_offset = tb->inst_ptr_offset;

#ifdef CONFIG_LATX_TU
    if (tb_is_tu(tb)) {
        return (uint32_t *)((uintptr_t)tb->tc.ptr +
               inst_ptr_offset - tb->tc.offset_in_tu);
    }
#endif
    return (uint32_t *)((uintptr_t)tb->tc.ptr + inst_ptr_offset);
}

static inline void latxs_ras_ibar(void)
{
    asm volatile("\tibar 0\n" : : : "memory");
}

static inline bool latxs_ras_flag_is_set(TranslationBlock *tb, uint32_t flag)
{
    return qatomic_read(&tb->misc_flags) & flag;
}

static inline void latxs_ras_set_flag(TranslationBlock *tb, uint32_t flag)
{
    qatomic_or(&tb->misc_flags, flag);
}

static inline void latxs_ras_clear_flag(TranslationBlock *tb, uint32_t flag)
{
    qatomic_and(&tb->misc_flags, ~flag);
}

static inline TranslationBlock *latxs_ras_first_source(TranslationBlock *target)
{
    if (!target->preorder_tb_offset) {
        return NULL;
    }
    return (TranslationBlock *)((intptr_t)target +
                                target->preorder_tb_offset);
}

static inline void latxs_ras_set_first_source(TranslationBlock *target,
                                              TranslationBlock *source)
{
    intptr_t offset;

    if (!source) {
        target->preorder_tb_offset = 0;
        return;
    }

    offset = (intptr_t)source - (intptr_t)target;
    g_assert(offset != 0 && (intptr_t)(int)offset == offset);
    target->preorder_tb_offset = offset;
}

/* The caller holds target->jmp_lock. */
static bool latxs_ras_remove_source_locked(TranslationBlock *target,
                                           TranslationBlock *source)
{
    TranslationBlock *prev = NULL;
    TranslationBlock *curr = latxs_ras_first_source(target);

    while (curr) {
        if (curr == source) {
            if (prev) {
                prev->preorder_next = curr->preorder_next;
            } else {
                latxs_ras_set_first_source(target, curr->preorder_next);
            }
            curr->preorder_next = NULL;
            return true;
        }
        prev = curr;
        curr = curr->preorder_next;
    }
    return false;
}

static void latxs_ras_patch_link(TranslationBlock *tb,
                                 TranslationBlock *tb_next)
{
    ptrdiff_t offset_high, offset_low;
    uintptr_t target_ptr = (uintptr_t)tb_next->tc.ptr;
    uint32_t inst0, inst1, inst2, inst3;
    uint32_t *inst_ptr = latxs_ras_inst_ptr(tb);

#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
    if (option_rastoptp &&
        (tb_next->bool_flags & (IS_AOT_TB | IS_TU_TB)) ==
        (IS_AOT_TB | IS_TU_TB) &&
        (qatomic_load_acquire(&tb_next->s_data->aot_native_call_entry) ||
         qatomic_load_acquire(&tb_next->s_data->aot_direct_native_entry))) {
        unsigned mtcc_id = get_mask_from_miscflags(tb, MF_MTCC_ID_MASK,
                                                 MF_MTCC_ID_SHIFT);
        /* Native returns need the full resume protocol, including outer SP. */
        target_ptr = GET_SC_TABLE(mtcc_id, ras_tp_fallback);
    }
#endif
    offset_high = ((target_ptr >> 12) -
            ((uintptr_t)inst_ptr >> 12)) & 0xfffff;
    offset_low = target_ptr & 0xfff;
    if (option_rastoptp) {
        /* Only an unpublished source or an exclusive callback gets here. */
        inst2 = inst_ptr[2];
        inst0 = 0x1a000000 | (inst2 & 0x1f) | (offset_high << 5);
        inst1 = 0x03800000 | (offset_low << 10) |
                ((inst2 & 0x1f) << 5) | (inst2 & 0x1f);
        qatomic_set(&inst_ptr[1], inst1);
        qatomic_set(&inst_ptr[3], inst_ptr[3] | (1 << 10));
        flush_idcache_range((uintptr_t)inst_ptr, (uintptr_t)inst_ptr, 16);
        qatomic_set(&inst_ptr[0], inst0);
        flush_idcache_range((uintptr_t)inst_ptr, (uintptr_t)inst_ptr, 4);
        return;
    }

#ifdef CONFIG_RAS_SLOW
    /*
     * patch insts
     * 0: nop                   => pcalau12i
     * 1: nop                   => ori
     * 2: st.d hva, entry, off
     * 3: addi.d top, top, 0    => addi.d top, top, 1
     */
    inst2 = inst_ptr[2];
    inst3 = inst_ptr[3];
    inst0 = 0x1a000000 | (inst2 & 0x1f) | (offset_high << 5);
    inst1 = 0x03800000 | (offset_low << 10) |
            ((inst2 & 0x1f) << 5) | (inst2 & 0x1f);
    inst_ptr[1] = inst1;
    inst_ptr[3] = inst3 | (0x1 << 10);
    latxs_ras_ibar();
    inst_ptr[0] = inst0;
    latxs_ras_ibar();
#else
    uint32_t inst4;

    /*
     * patch insts
     * 0: addi.d new, top, size
     * 1: b 4                   => pcalau12i
     * 2: nop                   => ori
     * 3: st.d hva, top, off
     * 4: gr2scr scr0, top      => gr2scr scr0, new
     */
    inst0 = inst_ptr[0];
    inst3 = inst_ptr[3];
    inst1 = 0x1a000000 | (inst3 & 0x1f) | (offset_high << 5);
    inst2 = 0x03800000 | (offset_low << 10) |
            ((inst3 & 0x1f) << 5) | (inst3 & 0x1f);
    inst4 = 0x800 |((inst0 & 0x1f) << 5);
    /*
     * Close the gate first, then update the payload.  The final pcalau12i
     * publishes the executable link; inst0 and inst3 remain unchanged.
     */
    inst_ptr[1] = 0x50000000 | (0x4 << 10);
    latxs_ras_ibar();
    inst_ptr[2] = inst2;
    inst_ptr[4] = inst4;
    latxs_ras_ibar();
    inst_ptr[1] = inst1;
    latxs_ras_ibar();
#endif
}

/* Explicit positions also cover repairs shared by a multi-member TU. */
uint32_t latxs_ras_tp_closed_gate(TranslationBlock *tb)
{
    g_assert(!(tb->inst_ptr_offset & 3));
    g_assert(!(LATXS_RAS_TP_COLD_OFFSET(tb) & 3));
    int64_t off = ((int64_t)LATXS_RAS_TP_COLD_OFFSET(tb) -
                  tb->inst_ptr_offset) >> 2;
    g_assert(off > 0 && off < (1 << 25));
    return 0x50000000 | (((uint32_t)off & 0xffff) << 10) |
           (((uint32_t)off >> 16) & 0x3ff);
}

static void latxs_ras_patch_detach(TranslationBlock *tb)
{
    uint32_t *inst_ptr = latxs_ras_inst_ptr(tb);
    if (option_rastoptp) {
        qatomic_set(inst_ptr, latxs_ras_tp_closed_gate(tb));
        flush_idcache_range((uintptr_t)inst_ptr, (uintptr_t)inst_ptr, 4);
        latxs_ras_clear_flag(tb, MF_RAS_LINKED);
        return;
    }

#ifdef CONFIG_RAS_SLOW
    /* Skip the complete slow-RAS sequence, but keep it relinkable. */
    int branch_insns = RAS_SLOW_UNLINK_BRANCH_INSNS;

    qatomic_set(&inst_ptr[0], 0x50000000 | (branch_insns << 10));
#else
    /*
     * Keep the payload intact for a later relink.  Closing inst1 prevents
     * execution from observing a partially updated target.
     */
    inst_ptr[1] = 0x50000000 | (0x4 << 10);
#endif
    latxs_ras_ibar();
    latxs_ras_clear_flag(tb, MF_RAS_LINKED);
}

static void latxs_ras_patch_permanent(TranslationBlock *tb)
{
    if (option_rastoptp) {
        if (tb->inst_ptr_offset && tb->tc.ptr) latxs_ras_patch_detach(tb);
        latxs_ras_set_flag(tb, MF_RAS_CLEAR_B0);
        if (tb->inst_ptr_offset && tb->tc.ptr) {
            uint32_t *start = latxs_ras_inst_ptr(tb) - 3;
            /* An old entrant past this instruction retains the complete
             * closed gate/cold path; C sees the permanent flag and returns.
             * New entrants skip the push, as in the legacy disabled site. */
            qatomic_set(start, LATX_RAS_TP_CALL_SKIP);
            flush_idcache_range((uintptr_t)start, (uintptr_t)start, 4);
        }
        return;
    }
    if (tb->inst_ptr_offset && tb->tc.ptr) {
        uint32_t *inst_ptr = latxs_ras_inst_ptr(tb);

#ifdef CONFIG_RAS_SLOW
        int branch_insns = RAS_SLOW_UNLINK_BRANCH_INSNS;

        qatomic_set(&inst_ptr[0], 0x50000000 | (branch_insns << 10));
#else
        uint32_t nop = 0x03400000;

        /*
         * Close the fast-RAS gate before destructively rewriting its
         * payload; granular TB invalidation does not quiesce other vCPUs.
         */
        inst_ptr[1] = 0x50000000 | (0x4 << 10);
        latxs_ras_ibar();

        /*
         * -2: scr2gr top, scr0         => b 7
         * -1: st.d gva, top, off       => nop
         *  0: addi.d new, top, size    => nop
         *  1: pcalau12i                => nop
         *  2: ori                      => nop
         *  3: st.d hva, top, off       => nop
         *  4: gr2scr scr0, new         => nop
         */
        inst_ptr[4] = nop;
        inst_ptr[3] = nop;
        inst_ptr[2] = nop;
        inst_ptr[1] = nop;
        inst_ptr[0] = nop;
        inst_ptr[-1] = nop;
        inst_ptr[-2] = 0x50000000 | (0x7 << 10);
#endif
        latxs_ras_ibar();
    }

    latxs_ras_set_flag(tb, MF_RAS_CLEAR_B0);
    latxs_ras_clear_flag(tb, MF_RAS_LINKED);
}

#ifdef CONFIG_LATX_TU
/*
 * Actively detach a live source so that it can be linked to another target.
 * Unlike permanent unlink, an empty slot remains zero rather than becoming
 * the permanent tag value one.
 */
static bool latxs_ras_detach(TranslationBlock *tb)
{
    TranslationBlock *target;
    uintptr_t dest;
    uintptr_t tagged;

    for (;;) {
        dest = qatomic_read(&tb->ras_dest);
        if (!dest) {
            return true;
        }
        if (dest & 1) {
            return false;
        }

        tagged = dest | 1;
        if (qatomic_cmpxchg(&tb->ras_dest, dest, tagged) != dest) {
            continue;
        }

        target = (TranslationBlock *)dest;
        qemu_spin_lock(&target->jmp_lock);
        if (qatomic_read(&tb->ras_dest) != tagged) {
            qemu_spin_unlock(&target->jmp_lock);
            continue;
        }

        if (unlikely(!latxs_ras_remove_source_locked(target, tb))) {
            g_assert_not_reached();
        }
        if (tb->inst_ptr_offset && tb->tc.ptr) {
            latxs_ras_patch_detach(tb);
        } else {
            latxs_ras_clear_flag(tb, MF_RAS_LINKED);
        }
        qatomic_set(&tb->ras_dest,
                    ((tb_cflags(tb) & CF_INVALID) ||
                     latxs_ras_flag_is_set(tb, MF_RAS_CLEAR_B0)) ?
                    (uintptr_t)1 : (uintptr_t)NULL);
        qemu_spin_unlock(&target->jmp_lock);
        return !(tb_cflags(tb) & CF_INVALID) &&
               !latxs_ras_flag_is_set(tb, MF_RAS_CLEAR_B0);
    }
}
#endif

/*
 * Close a software-RAS source without permanently invalidating its patch
 * site.  This is used when generated code must be dormant while it is moved
 * or serialized, but may be linked to a new target afterwards.
 */
bool latxs_ras_close_relinkable(TranslationBlock *tb)
{
    if (!option_soft_ras || !tb || !tb->inst_ptr_offset || !tb->tc.ptr) {
        return false;
    }
    if ((tb_cflags(tb) & CF_INVALID) ||
        latxs_ras_flag_is_set(tb, MF_RAS_CLEAR_B0)) {
        return false;
    }

    qemu_thread_jit_write();
#ifdef CONFIG_LATX_TU
    if (!latxs_ras_detach(tb)) {
        return false;
    }
    g_assert(qatomic_read(&tb->ras_dest) == (uintptr_t)NULL);
#endif

    /* Idempotently close the gate while retaining its relink payload. */
    latxs_ras_patch_detach(tb);
    return !latxs_ras_flag_is_set(tb, MF_RAS_LINKED) &&
           !latxs_ras_flag_is_set(tb, MF_RAS_CLEAR_B0);
}

bool latxs_ras_tp_permanent_valid(TranslationBlock *tb)
{
    if (!tb || !tb->tc.ptr || !tb->inst_ptr_offset ||
        !latxs_ras_flag_is_set(tb, MF_RAS_CLEAR_B0) ||
        latxs_ras_flag_is_set(tb, MF_RAS_LINKED)) {
        return false;
    }
    int64_t gate = tb->inst_ptr_offset;
#ifdef CONFIG_LATX_TU
    if (tb_is_tu(tb)) gate -= tb->tc.offset_in_tu;
#endif
    if (gate < 12 || (gate & 3) ||
        (uint64_t)gate + 16 > (uint64_t)tb->ir2_num * 4) {
        return false;
    }
    uint32_t *inst = latxs_ras_inst_ptr(tb);
    return qatomic_read(&inst[-3]) == LATX_RAS_TP_CALL_SKIP &&
           qatomic_read(&inst[0]) == latxs_ras_tp_closed_gate(tb);
}

/* Zero-length TU members cannot own an independently invalidatable entry.
 * This is stable for these published TBs; transient lookup misses are not. */
static bool ras_tp_empty_target(TranslationBlock *source,
                                TranslationBlock *target)
{
    return source && target && !target->ir2_num &&
           source != target && source->flags == target->flags &&
           source->inst_ptr_offset && source->tc.ptr && target->tc.ptr &&
           !(tb_cflags(source) & CF_INVALID) &&
           !(tb_cflags(target) & CF_INVALID) &&
           !latxs_ras_flag_is_set(source, MF_RAS_CLEAR_B0) &&
           (!qemu_tcg_mtcc_enabled() ||
            get_mask_from_miscflags(source, MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT) ==
            get_mask_from_miscflags(target, MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT));
}

static bool ras_link_now(TranslationBlock *tb, TranslationBlock *tb_next)
{
    TranslationBlock *first;

    if (!option_soft_ras || !tb || !tb_next || tb == tb_next ||
        tb->flags != tb_next->flags ||
        !tb->inst_ptr_offset || !tb->tc.ptr || !tb_next->tc.ptr ||
        (tb_cflags(tb) & CF_INVALID) ||
        latxs_ras_flag_is_set(tb, MF_RAS_CLEAR_B0)) {
        return false;
    }
    /* An empty TU member shares the next member's host entry and cannot be
     * identified independently after that entry is poisoned. */
    if (option_rastoptp && !tb_next->ir2_num) {
        return false;
    }

    /*
     * Independently recycled MTCC regions cannot retain direct references
     * into one another; see the matching guard in tb_add_jump().
     */
    if (qemu_tcg_mtcc_enabled() &&
        get_mask_from_miscflags(tb, MF_MTCC_ID_MASK, MF_MTCC_ID_SHIFT) !=
        get_mask_from_miscflags(tb_next, MF_MTCC_ID_MASK,
                                MF_MTCC_ID_SHIFT)) {
        return false;
    }

    qemu_thread_jit_write();
#ifdef CONFIG_LATX_TU
    uintptr_t dest = qatomic_read(&tb->ras_dest);

    if (dest == (uintptr_t)tb_next &&
        latxs_ras_flag_is_set(tb, MF_RAS_LINKED)) {
        bool valid;

        qemu_spin_lock(&tb_next->jmp_lock);
        valid = qatomic_read(&tb->ras_dest) == (uintptr_t)tb_next &&
                latxs_ras_flag_is_set(tb, MF_RAS_LINKED) &&
                !latxs_ras_flag_is_set(tb, MF_RAS_CLEAR_B0) &&
                tb->flags == tb_next->flags &&
                !(tb_cflags(tb) & CF_INVALID) &&
                !(tb_cflags(tb_next) & CF_INVALID);
        qemu_spin_unlock(&tb_next->jmp_lock);
        return valid;
    }
    if (dest && !latxs_ras_detach(tb)) {
        return false;
    }
#else
    if (latxs_ras_flag_is_set(tb, MF_RAS_LINKED)) {
        return true;
    }
#endif

    qemu_spin_lock(&tb_next->jmp_lock);
    if (tb->flags != tb_next->flags ||
        (tb_cflags(tb) & CF_INVALID) ||
        (tb_cflags(tb_next) & CF_INVALID)) {
        qemu_spin_unlock(&tb_next->jmp_lock);
        return false;
    }

#ifdef CONFIG_LATX_TU
    if (latxs_ras_flag_is_set(tb, MF_RAS_CLEAR_B0) ||
        qatomic_cmpxchg(&tb->ras_dest, (uintptr_t)NULL,
                        (uintptr_t)tb_next) != (uintptr_t)NULL) {
        qemu_spin_unlock(&tb_next->jmp_lock);
        return false;
    }
    g_assert(tb->preorder_next == NULL);
#endif

    /*
     * Register the reverse edge before opening the generated-code gate.
     * Target invalidation therefore sees every source that can execute the
     * linked sequence.
     */
    first = latxs_ras_first_source(tb_next);
    tb->preorder_next = first;
    latxs_ras_set_first_source(tb_next, tb);
    latxs_ras_patch_link(tb, tb_next);
    latxs_ras_set_flag(tb, MF_RAS_LINKED);
    qemu_spin_unlock(&tb_next->jmp_lock);
    return true;
}

/* Code-cache counts must be checked before dereferencing either TB. */
typedef struct RasTPLinkRequest {
    TranslationBlock *source, *target;
    uint32_t full_count, mtcc_count, mtcc;
} RasTPLinkRequest;

static void ras_tp_finish_link(CPUState *cpu, run_on_cpu_data data)
{
    RasTPLinkRequest *request = data.host_ptr;
    CPUX86State *env = cpu->env_ptr;
    env->ras_tp.link_queued = false;
    if (qatomic_read(&tb_ctx.tb_flush_count) != request->full_count ||
        qatomic_read(&tb_ctx.tb_flush_count_mtcc[request->mtcc]) !=
            request->mtcc_count) {
        env->ras_tp.expired_links++;
    } else if (ras_tp_empty_target(request->source, request->target)) {
        latxs_ras_unlink(request->source);
    } else if (ras_link_now(request->source, request->target)) {
        env->ras_tp.repaired_links++;
    }
    g_free(request);
}

/* The source is not discoverable by another CPU yet. */
bool latxs_ras_tp_link_initial(CPUState *cpu, TranslationBlock *source,
                               TranslationBlock *target)
{
    if (ras_tp_empty_target(source, target)) {
        latxs_ras_unlink(source);
        return false;
    }
    bool linked = ras_link_now(source, target);
    if (linked) ((CPUX86State *)cpu->env_ptr)->ras_tp.initial_links++;
    return linked;
}

bool latxs_ras_link(TranslationBlock *tb, TranslationBlock *target)
{
    if (!option_rastoptp) return ras_link_now(tb, target);
    CPUState *cpu = env_cpu(lsenv->cpu_state);
    CPUX86State *env = cpu->env_ptr;
    if (!tb || !target || (tb_cflags(tb) & CF_INVALID) ||
        (tb_cflags(target) & CF_INVALID) ||
        latxs_ras_flag_is_set(tb, MF_RAS_CLEAR_B0)) return false;
    if (latxs_ras_flag_is_set(tb, MF_RAS_LINKED) ||
        env->ras_tp.link_queued) return true;
    RasTPLinkRequest *request = g_new(RasTPLinkRequest, 1);
    request->source = tb;
    request->target = target;
    request->mtcc = get_mask_from_miscflags(tb, MF_MTCC_ID_MASK,
                                          MF_MTCC_ID_SHIFT);
    request->full_count = qatomic_read(&tb_ctx.tb_flush_count);
    request->mtcc_count = qatomic_read(
                            &tb_ctx.tb_flush_count_mtcc[request->mtcc]);
    env->ras_tp.link_queued = true;
    env->ras_tp.queued_links++;
    async_safe_run_on_cpu(cpu, ras_tp_finish_link,
                          RUN_ON_CPU_HOST_PTR(request));
    return true;
}

#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
static inline bool hras_local_member(const TranslationBlock *source,
                                     const TranslationBlock *member,
                                     target_ulong pc)
{
    return member && member->pc == pc &&
           member->cs_base == source->cs_base &&
           member->flags == source->flags &&
           tb_cflags(member) == (tb_cflags(source) & ~CF_INVALID) &&
           TB_TU_PAGES(member) == TB_TU_PAGES(source) &&
           tb_tu_identity(member) == tb_tu_identity(source) &&
           get_mask_from_miscflags(member, MF_MTCC_ID_MASK,
                                   MF_MTCC_ID_SHIFT) ==
           get_mask_from_miscflags(source, MF_MTCC_ID_MASK,
                                   MF_MTCC_ID_SHIFT);
}

/* Recognize only a generated CALL whose exact link+4 is its owner-local
 * return. Old AOT keeps its canonical-return repair path unchanged. */
bool latxs_hras_paired_return(const TranslationBlock *source)
{
    const TranslationBlock *next = source->s_data->next_tb[1];
    if (!next || !next->icount || !next->ir2_num ||
        !hras_local_member(source, next, source->pc + source->size)) {
        return false;
    }
    if (source->bool_flags & IS_HRAS_CALL) {
        return source->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID &&
               (uintptr_t)source->tc.ptr + source->jmp_reset_offset[0] ==
                   (uintptr_t)next->tc.ptr;
    }
    if (source->s_data->last_ir1_type == IR1_TYPE_CALLIN &&
        source->ir2_num && (uintptr_t)source->tc.ptr + source->ir2_num * 4 ==
                          (uintptr_t)next->tc.ptr) {
        uint32_t last;
        memcpy(&last, (const uint8_t *)next->tc.ptr - 4, 4);
        return (last & ~0x3e1U) == 0x4c000000U && (last & 0x3e0U);
    }
    return false;
}

/* Only an edge owned by this TU may select its private members. Global
 * lookups and the indirect-jump caches continue to use canonical entries. */
TranslationBlock *latxs_hras_local_target(TranslationBlock *source,
                                         TranslationBlock *target)
{
    TranslationBlock *member;

    if (!option_tu_hras || !tb_is_tu(source) ||
        ((source->bool_flags & IS_AOT_TB) &&
         !latxs_hras_paired_return(source)) ||
        (tb_cflags(source) & CF_INVALID)) {
        return target;
    }
    /* Local successors and adjacent returns need no walk from the TU head. */
    if (hras_local_member(source, target, target->pc)) {
        return target;
    }
    if (hras_local_member(source, source, target->pc)) {
        return source;
    }
    member = source->s_data->next_tb[1];
    if (hras_local_member(source, member, target->pc)) {
        return member;
    }
    const TranslationUnitPages *pages = TB_TU_PAGES(source);
    if (pages && pages->hras_members) {
        uint32_t lo = 0, hi = pages->hras_member_count;

        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;

            member = pages->hras_members[mid];
            if (member->pc < target->pc) {
                lo = mid + 1;
            } else if (member->pc > target->pc) {
                hi = mid;
            } else {
                return hras_local_member(source, member, target->pc) ?
                       member : target;
            }
        }
        return target;
    }
    /* A private member cannot start outside its owner's guest pages.
     * Without an index, reject such targets before walking the TU. */
    if (pages) {
        uint64_t page = target->pc & TARGET_PAGE_MASK;
        bool contains = false;

        for (int i = 0; i < TRANSLATION_UNIT_MAX_PAGES; i++) {
            uint64_t packed = pages->pages[i].vp_phys_hi;

            if (packed == UINT64_MAX) {
                break;
            }
            if (tu_page_desc_vp(packed) == page) {
                contains = true;
                break;
            }
        }
        if (!contains) {
            return target;
        }
    }
    member = source;
    while (member->s_data->next_tb[0]) {
        member = member->s_data->next_tb[0];
    }
    for (; member; member = member->s_data->next_tb[1]) {
        if (hras_local_member(source, member, target->pc)) {
            return member;
        }
    }
    return target;
}

static uint32_t *hras_check_gate(TranslationBlock *tb)
{
    uint16_t cp_data;
    uintptr_t cp_label;

    if (!(tb->bool_flags & IS_HRAS_CALL) || !tb->tc.ptr ||
        !tb->s_data->has_hras_check_gate) {
        return NULL;
    }
    cp_data = tb->jmp_cp_data[0];
    if (!(cp_data & TB_JMP_CP_DATA_VALID) ||
        !(cp_data & TB_JMP_CP_DATA_OFF)) {
        return NULL;
    }
    cp_label = (uintptr_t)tb->tc.ptr + tb->jmp_insn_offset[0] -
               (cp_data & TB_JMP_CP_DATA_OFF) * 4;
    return (uint32_t *)cp_label;
}

bool latxs_hras_open_check_gate(TranslationBlock *tb, uintptr_t hot_slot)
{
    uint32_t *gate = hras_check_gate(tb);
    int64_t offset;
    uint32_t inst;

    if (!gate) {
        return true;
    }
    offset = ((int64_t)hot_slot - (int64_t)(uintptr_t)gate) >> 2;
    if (offset != sextract64(offset, 0, 26)) {
        return false;
    }
    inst = 0x50000000 | ((offset >> 16) & 0x3ff) |
           ((offset << 10) & 0x3fffc00);
    qatomic_set(gate, inst);
    flush_idcache_range((uintptr_t)gate, (uintptr_t)gate, 4);
    return true;
}

void latxs_hras_close_check_gate(TranslationBlock *tb)
{
    uint32_t *gate = hras_check_gate(tb);

    if (gate) {
        qatomic_set(gate, 0x03400000);
        flush_idcache_range((uintptr_t)gate, (uintptr_t)gate, 4);
    }
}

bool latxs_hras_prepare_link(CPUState *cpu, TranslationBlock *source,
                             int n, TranslationBlock *target)
{
    uintptr_t site;
    bool direct_call, checked;
    bool target_valid, unchecked_valid;
    TranslationBlock *next;

    (void)cpu;
    if ((!option_tu_hras &&
         (source->bool_flags & (IS_AOT_TB | IS_HRAS_CALL)) !=
             (IS_AOT_TB | IS_HRAS_CALL)) || !option_rastoptp || n != 0 ||
        !tb_is_tu(source) ||
        (tb_cflags(source) & CF_INVALID) ||
        source->s_data->last_ir1_type != IR1_TYPE_CALL ||
        !(source->flags & HF_CS64_MASK) || source->cs_base ||
        (source->flags & HF_CPL_MASK) != HF_CPL_MASK) {
        return true;
    }
    if ((source->bool_flags & IS_AOT_TB) &&
        !(source->bool_flags & IS_HRAS_CALL)) {
        return true;
    }
    /* Registered TUs keep their members in this doubly linked list. */
    next = source->s_data->next_tb[1];
    if (!next || !next->icount || next->pc != source->pc + source->size) {
        return true;
    }
    direct_call = source->bool_flags & IS_HRAS_CALL;
    if (!direct_call) {
        return true;
    }
    site = (uintptr_t)source->tc.ptr + source->jmp_insn_offset[n];
    checked =
        (((source->jmp_cp_data[n] & TB_JMP_CP_DATA_VALID) &&
          (source->jmp_cp_data[n] & TB_JMP_CP_DATA_OFF)) ||
         (tb_is_tu(target) && TB_TU_PAGES(source) &&
          TB_TU_PAGES(source) == TB_TU_PAGES(target) &&
          tb_tu_identity(source) == tb_tu_identity(target)));
    target_valid = latxs_hras_call_target_can_link(source, target, site,
                                                    checked);
    if (!target_valid) {
        return false;
    }
    if (source->bool_flags & IS_AOT_TB) {
        return source->jmp_reset_offset[n] - source->jmp_insn_offset[n] == 8;
    }
    if (hras_check_gate(source)) {
        unchecked_valid = latxs_hras_call_target_can_link(source, target,
                                                           site, false);
        if (unchecked_valid) {
            qemu_thread_jit_write();
            if (latxs_hras_open_check_gate(source, site)) {
                return true;
            }
            latxs_hras_close_check_gate(source);
            return false;
        }
        latxs_hras_close_check_gate(source);
    }
    return true;
}
#endif

/* Called only by an unlinked CALL's cold tail, after restoring native TP. */
void ras_tp_repair_call(TranslationBlock *source)
{
    CPUState *cpu = env_cpu(lsenv->cpu_state);
    if ((tb_cflags(source) & CF_INVALID) ||
        latxs_ras_flag_is_set(source, MF_RAS_CLEAR_B0)) {
        return;
    }
    TranslationBlock *target = NULL;
#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
    if (option_tu_hras && tb_is_tu(source) &&
        (!(source->bool_flags & IS_AOT_TB) ||
         latxs_hras_paired_return(source))) {
        TranslationBlock *next = source->s_data->next_tb[1];
        if (next && next->pc == source->pc + source->size &&
            !(tb_cflags(next) & CF_INVALID)) {
            target = next;
        }
    }
#endif
    if (!target) {
        target = tb_htable_lookup_noexit(cpu,
        source->pc + source->size, source->cs_base, source->flags,
        tb_cflags(source) & ~CF_INVALID);
    }
    if (target) {
        latxs_ras_link(source, target);
    }
}

void latxs_ras_unlink(TranslationBlock *tb)
{
    if (!option_soft_ras || !tb || !tb->inst_ptr_offset || !tb->tc.ptr) {
        return;
    }
#ifdef CONFIG_RAS_DEBUG
    fprintf(stderr, "[RAS-DBG] %s tb %lx unlink\n",
            __func__, tb->pc);
#endif

    qemu_thread_jit_write();
#ifdef CONFIG_LATX_TU
    /*
     * Publish the permanent state before claiming the outgoing edge.  A
     * concurrent target invalidation will consequently retain tag one.
     */
    latxs_ras_set_flag(tb, MF_RAS_CLEAR_B0);
    for (;;) {
        TranslationBlock *target;
        uintptr_t dest;

        /*
         * Permanent unlink follows the generic jmp_dest invalidation rule:
         * tag the outgoing pointer before taking the target lock and retain
         * the value one after detaching.
         */
        dest = qatomic_or_fetch(&tb->ras_dest, 1);
        target = (TranslationBlock *)(dest & ~(uintptr_t)1);
        if (!target) {
            latxs_ras_patch_permanent(tb);
            qatomic_set(&tb->ras_dest, (uintptr_t)1);
            return;
        }

        qemu_spin_lock(&target->jmp_lock);
        if (qatomic_read(&tb->ras_dest) != dest) {
            qemu_spin_unlock(&target->jmp_lock);
            continue;
        }

        if (unlikely(!latxs_ras_remove_source_locked(target, tb))) {
            g_assert_not_reached();
        }
        latxs_ras_patch_permanent(tb);
        qatomic_set(&tb->ras_dest, (uintptr_t)1);
        qemu_spin_unlock(&target->jmp_lock);
        return;
    }
#else
    latxs_ras_patch_permanent(tb);
#endif
}

/*
 * Detach all software-RAS sources that point at @target.  The caller holds
 * target->jmp_lock and has set CF_INVALID under the same lock.
 */
void latxs_ras_unlink_target_locked(TranslationBlock *target)
{
    TranslationBlock *source = latxs_ras_first_source(target);

    latxs_ras_set_first_source(target, NULL);
    qemu_thread_jit_write();
    while (source) {
        TranslationBlock *next = source->preorder_next;

#ifdef CONFIG_LATX_TU
        uintptr_t dest;
        uintptr_t final_dest;

        /*
         * Check ownership before tagging: a stale reverse-list entry must not
         * mark an edge owned by another target.  The target lock prevents a
         * correctly owned edge from moving while it is processed.
         */
        for (;;) {
            dest = qatomic_read(&source->ras_dest);
            if ((dest & ~(uintptr_t)1) != (uintptr_t)target) {
                break;
            }
            if (dest & 1) {
                break;
            }
            if (qatomic_cmpxchg(&source->ras_dest, dest, dest | 1) == dest) {
                dest |= 1;
                break;
            }
        }
        if (unlikely((dest & ~(uintptr_t)1) != (uintptr_t)target)) {
            g_assert_not_reached();
            return;
        }

        source->preorder_next = NULL;
        if (source->inst_ptr_offset && source->tc.ptr) {
            latxs_ras_patch_detach(source);
        } else {
            latxs_ras_clear_flag(source, MF_RAS_LINKED);
        }
        final_dest = ((tb_cflags(source) & CF_INVALID) ||
                      latxs_ras_flag_is_set(source, MF_RAS_CLEAR_B0)) ?
                     (uintptr_t)1 : (uintptr_t)NULL;
        if (unlikely(qatomic_cmpxchg(&source->ras_dest, dest,
                                     final_dest) != dest)) {
            g_assert_not_reached();
        }
#else
        source->preorder_next = NULL;
        latxs_ras_patch_permanent(source);
#endif
        source = next;
    }
}

static
void latxs_rr_sigsegv_handler(int host_signum, siginfo_t *info, void *puc)
{
    if (!option_soft_ras) {
        return;
    }

    sigset_t new_set, old_set;
    sigemptyset(&new_set);
    sigaddset(&new_set, host_signum);
    if (sigprocmask(SIG_BLOCK, &new_set, &old_set) != 0) {
        perror("sigprocmask_block");
        return;
    }

    ucontext_t *uc = puc;
    unsigned long address = (unsigned long)info->si_addr;
    CPUX86State *env = (CPUX86State *)lsenv->cpu_state;
    size_t page_size = getpagesize();
    Soft_RAS *ras = (Soft_RAS *)env->s_ras_ptr;
    unsigned long guard_page = (unsigned long)ras + page_size;

    if (info->si_code == SEGV_ACCERR && address == guard_page) {
        uint64_t top_addr __attribute__((unused));
        top_addr = (uint64_t)&ras->stack[0];

#ifdef CONFIG_RAS_SLOW
        memset(ras, 0, page_size);
        UC_PC(uc) -= 12;
#else
        uint32_t* inst = (uint32_t *)UC_PC(uc);
        /* scr2gr */
        if ((inst[-1] >> 7) == 0x18) {
            /* scr0 */
            memset(ras, 0, page_size);
            uint64_t top_addr = (uint64_t) &ras->stack[0];
#ifdef LOONGARCH_NEW_WORLD
            struct extctx_layout extctx;
            memset(&extctx, 0, sizeof(extctx));
            /* we need to parse the extcontext data */
            parse_extcontext(uc, &extctx);
            UC_SET_FPR(&extctx, 0, top_addr, int64_t)
#else
            UC_FREG(uc)[0].__val64[0] = top_addr;
#endif
        }
        UC_PC(uc) -= 4;
#endif
#ifdef CONFIG_RAS_DEBUG
        CPUState *cpu = env_cpu(env);
        fprintf(stderr,
                "[RAS-DBG] [cpu%d] %s ras overflow, clear it.\n",
                cpu->cpu_index, __func__);
#endif
    }

    if (sigprocmask(SIG_SETMASK, &old_set, NULL) != 0) {
        perror("sigprocmask_setmask");
        return;
    }

    return;
}

void latxs_init_rr_thread_sigsegv(CPUState *cpu)
{
    if (!option_soft_ras || option_rastoptp) {
        return;
    }

    /* 1. unblock the signal for vCPU thread */
    sigset_t set, oldset;
    sigemptyset(&set);
    sigaddset(&set, SIGSEGV);

    int ret = pthread_sigmask(SIG_UNBLOCK, &set, &oldset);
    if (ret < 0) {
        fprintf(stderr, "[SIGSEGV] unblock failed\n");
        exit(-1);
    }

    /* 2. set the handler for signal */

    struct sigaction act;
    struct sigaction old_act;

    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_SIGINFO;
    act.sa_sigaction = latxs_rr_sigsegv_handler;

    ret = sigaction(SIGSEGV, &act, &old_act);
    if (ret < 0) {
        fprintf(stderr, "[SIGSEGV] set handler failed\n");
        exit(-1);
    }
}

typedef struct {
    struct {
        uint64_t l;
        uint64_t h;
    } vec[32];
} vec_temp_data_t;

static __thread vec_temp_data_t vec_temp_data[4];

static void vec_temp_data_save(int cpuid)
{
    vec_temp_data_t *d = &(vec_temp_data[cpuid]);

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

static void vec_temp_data_restore(int cpuid)
{
    vec_temp_data_t *d = &(vec_temp_data[cpuid]);

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

uint64_t ras_target_check(CPUX86State *env, uint64_t gva, uint64_t hva)
{
    CPUState *cpu = env_cpu(env);
    TranslationBlock *tb;
    uint32_t flags, cflags;
    uint64_t cs_base;
    vaddr pc;
    int cpuid = cpu->cpu_index;

    vec_temp_data_save(cpuid);
    flags = env->hflags |
        (env->eflags & (IOPL_MASK | TF_MASK | RF_MASK | VM_MASK | AC_MASK));
    cflags = curr_cflags(cpu);
    if (env->hflags & HF_CS64_MASK) {
        cs_base = 0;
    } else {
        cs_base = env->segs[R_CS].base;
    }
    pc = gva - cs_base;

    tb = export_tb_lookup(cpu, pc, cs_base, flags, cflags);
    if (!tb) {
#ifdef CONFIG_LATX_TU
        tb = tu_tree_lookup(pc);
#endif
        if (!tb) {
            fprintf(stderr, "[RAS-DBG] [cpu%d] %s gva %lx, "
                 "tu_tree_lookup return NULL, ras return hva %lx\n",
                 cpu->cpu_index, __func__, gva, hva);
            vec_temp_data_restore(cpuid);
            return 0;
        }
        if (hva != (uint64_t)tb->tc.ptr) {
            fprintf(stderr, "[RAS-DBG] [cpu%d] %s gva %lx, "
                 "tu_tree_lookup return %p, ras return hva %lx\n",
                 cpu->cpu_index, __func__, gva, tb->tc.ptr, hva);
            vec_temp_data_restore(cpuid);
            return (uint64_t)tb->tc.ptr;
        }
    }
    if (hva != (uint64_t)tb->tc.ptr) {
        fprintf(stderr, "[RAS-DBG] [cpu%d] "
            "%s gva %lx, tb_lookup return %p, ras return hva %lx\n",
             cpu->cpu_index, __func__, gva, tb->tc.ptr, hva);
        vec_temp_data_restore(cpuid);
        return (uint64_t)tb->tc.ptr;
    }
    vec_temp_data_restore(cpuid);
    return hva;
}

void ras_debug_helper(CPUX86State *env, TranslationBlock *tb)
{
    CPUState *cpu = env_cpu(env);
    Soft_RAS *ras = cpu->s_ras;
    int cpuid = cpu->cpu_index;
    vec_temp_data_save(cpuid);
    fprintf(stderr, "[RAS-DBG] [cpu%d] %s tb %lx ras %p top %d\n",
            cpuid, __func__, tb->pc, ras, ras->top);
    vec_temp_data_restore(cpuid);
}

void latxs_flush_soft_ras(CPUState *cpu)
{
    Soft_RAS *ras = cpu->s_ras;

    /* During early initialization, the cache may not yet be allocated. */
    if (unlikely(ras == NULL)) {
        return;
    }
    if (option_rastoptp) {
        /* Remote CPUs only request reset; the owner consumes it atomically. */
        if (qemu_cpu_is_self(cpu)) ras->top = 0;
        qatomic_set(&((LatxRasTPState *)ras)->reset_pending, 1);
        return;
    }
#ifdef CONFIG_RAS_SLOW
    ras->top = 0;
#else
    if (qemu_cpu_is_self(cpu)) {
        uint64_t top_addr = (uint64_t)&ras->stack[0];
        __asm__ __volatile__ (
                MOVGR_2_SCR0(%0)
                ::"r"(top_addr)
                :);
#ifdef CONFIG_RAS_DEBUG
    } else {
        fprintf(stderr,
                "[RAS-DBG] [cpu%d] %s qemu_cpu_is_self return %d.\n",
                cpu->cpu_index, __func__, qemu_cpu_is_self(cpu));
#endif
    }
#endif
}

#define RAS_TP_OFF(member) \
    (offsetof(CPUX86State, ras_tp) + offsetof(LatxRasTPState, member))

/* Only called at generated-code/C boundaries. Preserve the live scratch. */
void latxs_ras_tp_enter_c(void)
{
    if (!option_rastoptp) return;
    IR2_OPND tp = latxs_ir2_opnd_new(IR2_OPND_GPR, 2);
    IR2_OPND scratch = latxs_ir2_opnd_new(IR2_OPND_GPR, 21);
    IR2_OPND pending = latxs_ir2_opnd_new(IR2_OPND_GPR, 20);
    IR2_OPND done = latxs_ir2_opnd_new_label();
    IR2_OPND restore = latxs_ir2_opnd_new_label();
    /* A native-stack spill remains private across nested exceptions. */
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&la_sp);
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, -16);
    la_out2i(LISA_STPTR_D, &scratch, &la_sp, 0);
    la_out2i(LISA_STPTR_D, &pending, &la_sp, 2);
    la_out2i(LISA_LDPTR_D, &scratch, &la_env, RAS_TP_OFF(native_tp) >> 2);
    la_out3(LISA_BEQ, &tp, &scratch, &done);
    la_out2i(LISA_STPTR_W, &tp, &la_env, RAS_TP_OFF(top) >> 2);
    latxs_load_imm64_to_ir2(&scratch, RAS_TP_OFF(reset_pending));
    la_out3(LISA_ADD_D, &scratch, &la_env, &scratch);
    la_out3(LISA_AMSWAP_W, &pending, &scratch, &la_zero);
    la_out2(LISA_BEQZ, &pending, &restore);
    /* An interrupt after reset was consumed must not republish the old top.
     * Clear live r2 before clearing its saved copy, then restore real TLS. */
    la_out2i(LISA_ADDI_D, &tp, &la_zero, 0);
    la_out2i(LISA_STPTR_W, &la_zero, &la_env, RAS_TP_OFF(top) >> 2);
    la_label(restore);
    la_out2i(LISA_LDPTR_D, &tp, &la_env, RAS_TP_OFF(native_tp) >> 2);
    la_label(done);
    la_out2i(LISA_LDPTR_D, &scratch, &la_sp, 0);
    la_out2i(LISA_LDPTR_D, &pending, &la_sp, 2);
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, 16);
    if (hamt_enabled()) latxs_tr_dmw_out_fix_mem(&la_sp);
}

void latxs_ras_tp_leave_c(void)
{
    if (!option_rastoptp) return;
    IR2_OPND tp = latxs_ir2_opnd_new(IR2_OPND_GPR, 2);
    IR2_OPND scratch = latxs_ir2_opnd_new(IR2_OPND_GPR, 21);
    IR2_OPND pending = latxs_ir2_opnd_new(IR2_OPND_GPR, 20);
    IR2_OPND load = latxs_ir2_opnd_new_label();
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&la_sp);
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, -16);
    la_out2i(LISA_STPTR_D, &scratch, &la_sp, 0);
    la_out2i(LISA_STPTR_D, &pending, &la_sp, 2);
    latxs_load_imm64_to_ir2(&scratch, RAS_TP_OFF(reset_pending));
    la_out3(LISA_ADD_D, &scratch, &la_env, &scratch);
    la_out3(LISA_AMSWAP_W, &pending, &scratch, &la_zero);
    la_out2(LISA_BEQZ, &pending, &load);
    la_out2i(LISA_STPTR_W, &la_zero, &la_env, RAS_TP_OFF(top) >> 2);
    la_label(load);
    la_out2i(LISA_LDPTR_W, &tp, &la_env, RAS_TP_OFF(top) >> 2);
    la_out2i(LISA_LDPTR_D, &scratch, &la_sp, 0);
    la_out2i(LISA_LDPTR_D, &pending, &la_sp, 2);
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, 16);
    if (hamt_enabled()) latxs_tr_dmw_out_fix_mem(&la_sp);
}

void latxs_ras_tp_save(CPUX86State *env, const uint64_t *gprs)
{
    if (!option_rastoptp) return;
    if (gprs[2] != env->ras_tp.native_tp) {
        if (gprs[2] <= LATX_RAS_TP_SIZE) {
            env->ras_tp.top = gprs[2];
        } else {
            /* Includes the empty-sentinel RET's transient -1. Preserve
             * the frame for exact-PC resume, but never publish it as top. */
            qatomic_set(&env->ras_tp.reset_pending, 1);
        }
    }
    if (qatomic_read(&env->ras_tp.reset_pending)) env->ras_tp.top = 0;
}

void latxs_ras_tp_redirect(CPUX86State *env, uint64_t *gprs)
{
    if (!option_rastoptp) return;
    if (qatomic_xchg(&env->ras_tp.reset_pending, 0)) env->ras_tp.top = 0;
    /* Only redirect at a recovered guest boundary. Exact-PC resumes keep
     * the original r2, including intermediate CALL/RET state. */
    gprs[2] = env->ras_tp.top;
}

void latxs_ras_tp_prepare(CPUState *cpu, unsigned mtcc_id)
{
#if defined(__loongarch__) && !defined(CONFIG_USER_ONLY)
    if (!option_rastoptp) return;
    CPUX86State *env = cpu->env_ptr;
    uint64_t native_tp;
    asm volatile("or %0, $tp, $zero" : "=r"(native_tp));
    g_assert(native_tp > LATX_RAS_TP_SIZE && native_tp != UINT64_MAX);
    env->ras_tp.native_tp = native_tp;
    int cpuid = qemu_tcg_mttcg_enabled() ? cpu->cpu_index : 0;
    hamt_ras_tp_mirror(cpuid, native_tp);
    if (qatomic_xchg(&env->ras_tp.reset_pending, 0)) env->ras_tp.top = 0;
    /* Kernel execution shares r2 but never owns the user's return stack. */
    if ((env->hflags & HF_CPL_MASK) != HF_CPL_MASK) return;
    if (!env->ras_tp.initialized || env->ras_tp.mtcc_id != mtcc_id) {
        uint64_t target = GET_SC_TABLE(mtcc_id, ras_tp_fallback);
        g_assert(target);
        if (hamt_mode_dmw()) target = DMW_IN(target);
        env->ras_tp.top = 0;
        env->ras_tp.sentinel.pc = 0;
        env->ras_tp.sentinel.ptr = (const void *)(uintptr_t)target;
        env->ras_tp.mtcc_id = mtcc_id;
        env->ras_tp.initialized = true;
    }
#endif
}

static bool ras_tp_eligible(IR1_INST *pir1)
{
    TranslationBlock *tb = lsenv->tr_data->curr_tb;
    return (tb->flags & HF_CS64_MASK) &&
           (tb->flags & HF_CPL_MASK) == HF_CPL_MASK && !tb->cs_base &&
           latxs_ir1_data_size(pir1) != 16;
}

static void ras_tp_clear(void)
{
    IR2_OPND tp = latxs_ir2_opnd_new(IR2_OPND_GPR, 2);
    la_out2i(LISA_ADDI_D, &tp, &la_zero, 0);
    la_out2i(LISA_STPTR_W, &la_zero, &la_env, RAS_TP_OFF(top) >> 2);
    la_out2i(LISA_STPTR_W, &la_zero, &la_env, RAS_TP_OFF(reset_pending) >> 2);
}

void latxs_ras_tp_emit_clear(void)
{
    if (option_rastoptp) {
        ras_tp_clear();
    }
}

static bool ras_tp_push(IR1_INST *pir1, IR2_OPND *return_pc)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    TranslationBlock *tb = td->curr_tb;
    if ((tb->flags & HF_CPL_MASK) != HF_CPL_MASK) return true;
    if (!ras_tp_eligible(pir1)) { ras_tp_clear(); return true; }
    IR2_OPND tp = latxs_ir2_opnd_new(IR2_OPND_GPR, 2);
    IR2_OPND index = latxs_ra_alloc_itemp();
    IR2_OPND entry = latxs_ra_alloc_itemp();
    IR2_OPND pc = return_pc ? *return_pc : latxs_ra_alloc_itemp();
    if (!return_pc) {
        if (option_aot_save) {
            aot_load_guest_addr_to_ir2(&pc, ir1_addr_next(pir1), LOAD_GUEST_ADDR);
        } else {
            latxs_load_imm64_to_ir2(&pc, ir1_addr_next(pir1));
        }
    }
    la_out2i(LISA_ANDI, &index, &tp, LATX_RAS_TP_SIZE - 1);
    la_out3i(LISA_ALSL_D, &entry, &index, &la_env, 3);
    la_out2i(LISA_STPTR_D, &pc, &entry, RAS_TP_OFF(stack) >> 2);
    td->ras_tp_has_call = true;
    td->ras_tp_call_gate = latxs_ir2_opnd_new_label();
    td->ras_tp_call_miss = latxs_ir2_opnd_new_label();
    td->ras_tp_call_done = latxs_ir2_opnd_new_label();
    tb->inst_ptr_offset = get_current_inst_addr(tb) - (uintptr_t)tb->tc.ptr;
    la_label(td->ras_tp_call_gate);
    la_out1(LISA_B, &td->ras_tp_call_miss); /* published as PCALAU12I */
    la_out_nop();                       /* ORI */
    la_out2i(LISA_STPTR_D, &pc, &entry, (RAS_TP_OFF(stack) + 8) >> 2);
    la_out2i(LISA_ADDI_D, &tp, &index, 0); /* linked patch changes 0 to 1 */
    la_label(td->ras_tp_call_done);
    if (!return_pc) latxs_ra_free_temp(&pc);
    latxs_ra_free_temp(&entry);
    latxs_ra_free_temp(&index);
    return true;
}

int gen_latxs_ras_tp_repair(void *code_ptr)
{
    latxs_tr_begin(code_ptr);
    /* CALL temps remain live on this cold edge. BL's return address, the
     * source TB and the indirect target must survive helper entry as well.
     * Use a private native-stack frame, never the shared env spill slots. */
    int frame_size = QEMU_ALIGN_UP(24 + latxs_itemp_status_num * 8, 16);
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&la_sp);
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, -frame_size);
    la_out2i(LISA_ST_D, &la_ra, &la_sp, 0);
    la_out2i(LISA_ST_D, &la_a0, &la_sp, 8);
    la_out2i(LISA_ST_D, &la_a1, &la_sp, 16);
    for (unsigned i = 0; i < latxs_itemp_status_num; ++i) {
        IR2_OPND reg = la_new_gpr(latxs_itemp_status_default[i].physical_id);
        la_out2i(LISA_ST_D, &reg, &la_sp, 24 + i * 8);
    }
    if (hamt_enabled()) latxs_tr_dmw_out_fix_mem(&la_sp);
    ras_tp_clear();
#ifdef CONFIG_LATX_OPT_NOFP
    helper_cfg_t cfg = nofp_helper_cfg;
#else
    helper_cfg_t cfg = default_helper_cfg;
#endif
    latxs_tr_gen_call_to_helper_prologue_inline(cfg);
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&la_sp);
    la_out2i(LISA_LD_D, &la_a0, &la_sp, 8);
    if (hamt_enabled()) latxs_tr_dmw_out_fix_mem(&la_sp);
    latxs_tr_gen_call_to_helper((ADDR)ras_tp_repair_call,
                               LATX_HELPER_RAS_TP_REPAIR_CALL);
    latxs_tr_gen_call_to_helper_epilogue_inline(cfg);
    /* A repair never restores the pre-repair top, even if C was interrupted. */
    ras_tp_clear();
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&la_sp);
    for (unsigned i = 0; i < latxs_itemp_status_num; ++i) {
        IR2_OPND reg = la_new_gpr(latxs_itemp_status_default[i].physical_id);
        la_out2i(LISA_LD_D, &reg, &la_sp, 24 + i * 8);
    }
    la_out2i(LISA_LD_D, &la_a1, &la_sp, 16);
    la_out2i(LISA_LD_D, &la_a0, &la_sp, 8);
    la_out2i(LISA_LD_D, &la_ra, &la_sp, 0);
    la_out2i(LISA_ADDI_D, &la_sp, &la_sp, frame_size);
    if (hamt_enabled()) latxs_tr_dmw_out_fix_mem(&la_sp);
    la_out2i(LISA_JIRL, &la_zero, &la_ra, 0);
    return latxs_tr_end(code_ptr);
}

void latxs_ras_tp_emit_call_cold(void)
{
    TRANSLATION_DATA *td = lsenv->tr_data;
    TranslationBlock *tb = td->curr_tb;
    if (!option_rastoptp || !td->ras_tp_has_call) return;
    unsigned start = td->ir2_asm_nr;
    la_label(td->ras_tp_call_miss);
    /* TU exit labels are resolved independently of the persistent RAS
     * repair position stored in the TB's private side data. */
#ifdef CONFIG_LATX_TU
    if (!tb_is_tu(tb))
#endif
    {
        LATXS_RAS_TP_COLD_OFFSET(tb) =
            get_current_inst_addr(tb) - (uintptr_t)tb->tc.ptr;
    }
    latxs_load_addr_to_ir2(&la_a0, (uintptr_t)tb, LOAD_TB_FULL_ADDR);
    if (option_aot_save) {
        tb_sc_table_offset_jmp(tb, td->mtcc_id, ras_tp_repair,
                td->tcg_region_offset, td->ir2_asm_nr << 2, 1, RAS_TP_REPAIR);
    } else {
        int offset = (GET_SC_TABLE_OFFSET(td->mtcc_id, ras_tp_repair,
                             td->tcg_region_offset) -
                      get_current_inst_addr(tb)) >> 2;
        if (option_large_code_cache && !int32_in_int26(offset)) {
            /* Unlike the general far-jump emitter, do not allocate a temp:
             * every CALL temp may still be live. BL already clobbers ra. */
            int64_t upper = (((int64_t)offset + (1 << 15)) >> 16) << 44 >> 44;
            int64_t lower = ((int64_t)offset) << 48 >> 48;
            latxs_append_ir2_opnd1i(LISA_PCADDU18I, &la_ra, upper);
            la_out2i(LISA_JIRL, &la_ra, &la_ra, lower);
        } else {
            la_outa(LISA_BL, offset);
        }
    }
    la_out1(LISA_B, &td->ras_tp_call_done);
    unsigned cold_insns = td->ir2_asm_nr - start;
    QEMU_BUILD_BUG_ON(LATX_RAS_TP_CALL_COLD_INSNS > 7);
    g_assert(cold_insns && cold_insns <= 7);
    LATXS_RAS_TP_COLD_INSNS(tb) = cold_insns;
    g_assert(!option_aot_save || cold_insns == LATX_RAS_TP_CALL_COLD_INSNS);
}

/* Save is enabled during ordinary JIT execution too. Only this explicit
 * translation context identifies the offline AOT builder. */
bool latxs_hras_jit_translate(const TranslationBlock *tb)
{
    return !in_aot_translate &&
        (!option_aot_save || (option_tu_hras == 2 && aot_tu_enabled() && tb &&
         (tb->flags & HF_CS64_MASK) && !tb->cs_base &&
         (tb->flags & HF_CPL_MASK) == HF_CPL_MASK));
}

bool latxs_hras_aot_translate(void)
{
    return in_aot_translate && option_aot_save && option_tu_hras == 2 &&
           option_rastoptp && option_soft_ras;
}

static bool ras_tp_pop(IR2_OPND *ret_pc)
{
    IR2_OPND tp = latxs_ir2_opnd_new(IR2_OPND_GPR, 2);
    IR2_OPND entry = latxs_ra_alloc_itemp();
    IR2_OPND saved_pc = latxs_ra_alloc_itemp();
    IR2_OPND target = latxs_ra_alloc_itemp();
    IR2_OPND miss = latxs_ir2_opnd_new_label();
    IR2_OPND *jump_target = &target;
#ifdef CONFIG_LATX_TU
    if (option_tu_hras == 2 &&
        (latxs_hras_jit_translate(lsenv->tr_data->curr_tb) ||
         (latxs_hras_aot_translate() && tb_is_tu(lsenv->tr_data->curr_tb)))) {
        jump_target = &la_ra;
    }
#endif
    la_out3i(LISA_ALSL_D, &entry, &tp, &la_env, 3);
    la_out2i(LISA_LDPTR_D, &saved_pc, &entry,
             (RAS_TP_OFF(stack) - sizeof(LatxRasTPEntry)) >> 2);
    la_out3(LISA_BNE, &saved_pc, ret_pc, &miss);
    la_out2i(LISA_LDPTR_D, jump_target, &entry,
             (RAS_TP_OFF(stack) - sizeof(LatxRasTPEntry) + 8) >> 2);
    la_out2i(LISA_ADDI_D, &tp, &tp, -1);
    la_out2i(LISA_JIRL, &la_zero, jump_target, 0);
    la_label(miss);
    ras_tp_clear();
    if (aot_tu_enabled() && hamt_enabled() && option_rastoptp) {
        /* The ordinary lookup below will not return to any suspended LLVM
         * caller. Drop all of its fixed host frames before that lookup can
         * enter another translated function. The saved SP uses the normal
         * userspace alias, just like the translator prologue. */
        size_t off = offsetof(CPUX86State, aot_native_outer_sp);
        g_assert(!(off & 3) && off <= 32764);
        la_out2i(LISA_LDPTR_D, &la_sp, &la_env, off >> 2);
    }
    latxs_ra_free_temp(&target);
    latxs_ra_free_temp(&saved_pc);
    latxs_ra_free_temp(&entry);
    return true;
}

static bool latxs_translate_ras_push_slow(IR1_INST *pir1)
{
    TranslationBlock *tb = lsenv->tr_data->curr_tb;
    uint32_t gva_off = offsetof(RAS_Entry, pc);
    uint32_t hva_off = offsetof(RAS_Entry, ptr);
    int top_off = (int)(offsetof(Soft_RAS, top) - offsetof(Soft_RAS, stack));

    IR2_OPND ptr = latxs_ra_alloc_itemp();
    IR2_OPND gva = latxs_ra_alloc_itemp();
    IR2_OPND top = latxs_ra_alloc_itemp();
    IR2_OPND entry = latxs_ra_alloc_itemp();

    //latxs_tr_save_temp_register_mask(-1);
    //latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
    //latxs_append_ir2_opnd2_(lisa_mov, &la_a0, &la_env);
    //latxs_load_imm64_to_ir2(&la_a1, tb);
    //latxs_tr_gen_call_to_helper(ras_debug_helper);
    //latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    //latxs_tr_restore_temp_register_mask(-1);

    uint64_t pc = ir1_addr_next(pir1) + tb->cs_base;
    if (option_aot_save) {
        aot_load_guest_addr_to_ir2(&gva, pc, LOAD_GUEST_ADDR);
    } else {
        latxs_load_imm64_to_ir2(&gva, pc);
    }
    /* get ras ptr */
    latxs_append_ir2_opnd2(LISA_SCR2GR, &ptr, &la_scr0);
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&ptr);
    latxs_append_ir2_opnd2i(LISA_LD_W, &top, &ptr, top_off);
    la_out2i(LISA_ANDI, &top, &top, top_mask);
    latxs_append_ir2_opnd3i(LISA_ALSL_D, &entry, &top, &ptr, 3);

    /* store gva */
    latxs_append_ir2_opnd2i(LISA_ST_D, &gva, &entry, gva_off);

    /* store hva */
    tb->inst_ptr_offset = get_current_inst_addr(tb) - (uintptr_t)tb->tc.ptr;
    latxs_append_ir2_opnda(LISA_B, 0); /* PCALAU12I */
    //latxs_append_ir2_opnd0_(lisa_nop); /* ORI       */
    //latxs_append_ir2_opnd0_(lisa_nop); /* ORI       */
    latxs_append_ir2_opnd0_(lisa_nop); /* ORI       */
    latxs_append_ir2_opnd2i(LISA_ST_D, &gva, &entry, hva_off);

    /* update top */
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &top, &top, 0);
    latxs_append_ir2_opnd2i(LISA_ST_W, &top, &ptr, top_off);

    latxs_ra_free_temp(&ptr);
    latxs_ra_free_temp(&gva);
    latxs_ra_free_temp(&top);
    latxs_ra_free_temp(&entry);
    return true;

}

bool latxs_translate_ras_push(IR1_INST *pir1, IR2_OPND *return_pc)
{
    TranslationBlock *tb = lsenv->tr_data->curr_tb;
    if (!tb) {
        return false;
    }

#ifdef CONFIG_RAS_DISABLE_CPL0
    if (!(tb->flags & HF_CPL_MASK)) {
        return true;
    }
#endif

#ifdef CONFIG_RAS_SLOW
    if (option_rastoptp) return ras_tp_push(pir1, return_pc);
    return latxs_translate_ras_push_slow(pir1);
#endif
    uint32_t gva_off = offsetof(RAS_Entry, pc);
    uint32_t hva_off = offsetof(RAS_Entry, ptr);
    int entry_size = sizeof(RAS_Entry);

    IR2_OPND ras_top = latxs_ra_alloc_itemp();
    IR2_OPND top_new = latxs_ra_alloc_itemp();
    IR2_OPND ras_gva = latxs_ra_alloc_itemp();

    /* store gva */
    uint64_t pc = ir1_addr_next(pir1) + tb->cs_base;
    if (option_aot_save) {
        aot_load_guest_addr_to_ir2(&ras_gva, pc, LOAD_GUEST_ADDR);
    } else {
        latxs_load_imm64_to_ir2(&ras_gva, pc);
    }
    latxs_append_ir2_opnd2(LISA_SCR2GR, &ras_top, &la_scr0);
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&ras_top);
    latxs_append_ir2_opnd2i(LISA_ST_D, &ras_gva, &ras_top, gva_off);

    /* get top addr */
    tb->inst_ptr_offset = get_current_inst_addr(tb) - (uintptr_t)tb->tc.ptr;
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &top_new, &ras_top, entry_size);

    /* store hva */
    latxs_append_ir2_opnd0_(lisa_nop); /* PCALAU12I */
    latxs_append_ir2_opnd0_(lisa_nop); /* ORI       */
    latxs_append_ir2_opnd2i(LISA_ST_D, &ras_gva, &ras_top, hva_off);

    /* update top */
    latxs_append_ir2_opnd2(LISA_GR2SCR, &la_scr0, &ras_top);
    latxs_ra_free_temp(&ras_top);
    latxs_ra_free_temp(&top_new);
    latxs_ra_free_temp(&ras_gva);

    return true;
}

static bool latxs_translate_ras_pop_slow(IR2_OPND  *ret_gva)
{
    IR2_OPND miss = latxs_ir2_opnd_new_label();
    uint32_t gva_off = offsetof(RAS_Entry, pc);
    uint32_t hva_off = offsetof(RAS_Entry, ptr);
    int top_off = (int)(offsetof(Soft_RAS, top) - offsetof(Soft_RAS, stack));

    IR2_OPND ptr = latxs_ra_alloc_itemp();
    IR2_OPND top = latxs_ra_alloc_itemp();
    IR2_OPND entry = latxs_ra_alloc_itemp();

#if defined(CONFIG_RAS_CHECK) || defined(CONFIG_RAS_PROFILER)
    latxs_tr_gen_store_to_curr_eip(ret_gva);
#endif
#ifdef CONFIG_RAS_PROFILER
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &la_a0, &la_env);
    latxs_tr_gen_call_to_helper(ras_total_count);
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
#endif
#ifdef CONFIG_RAS_PROFILER
    latxs_tr_gen_load_from_curr_eip(ret_gva);
#endif

    /* get ras ptr */
    latxs_append_ir2_opnd2(LISA_SCR2GR, &ptr, &la_scr0);
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&ptr);
    latxs_append_ir2_opnd2i(LISA_LD_W, &top, &ptr, top_off);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &top, &top, -1);
    latxs_append_ir2_opnd3i(LISA_ALSL_D, &entry, &top, &ptr, 3);

    /* compare gva */
    IR2_OPND gva = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_LD_D, &gva, &entry, gva_off);
    latxs_append_ir2_opnd3(LISA_BNE, &gva, ret_gva, &miss);
    latxs_ra_free_temp(&gva);

    /* hit, update top*/
    latxs_append_ir2_opnd2i(LISA_ST_W, &top, &ptr, top_off);
    latxs_ra_free_temp(&top);
    IR2_OPND hva = latxs_ra_alloc_itemp();
#ifndef CONFIG_RAS_DISABLE_CPL0
    TRANSLATION_DATA *td = lsenv->tr_data;
    TranslationBlock *tb = td->curr_tb;
    if (!(tb->flags & HF_CPL_MASK)) {
        latxs_load_intcheck_to_reg(&hva);
        latxs_append_ir2_opnd3(LISA_BLT, &hva, &la_zero,
                            latxs_ir2_interrupt_get_label(tb, 0));
    }
#endif
    latxs_append_ir2_opnd2i(LISA_LD_D, &hva, &entry, hva_off);
#ifdef CONFIG_RAS_CHECK
    latxs_tr_gen_store_to_curr_eip(ret_gva);
    latxs_tr_save_temp_register_mask(-1);
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &la_a0, &la_env);
    latxs_append_ir2_opnd2_(lisa_mov, &la_a1, ret_gva);
    latxs_append_ir2_opnd2_(lisa_mov, &la_a2, &hva);
    latxs_tr_gen_call_to_helper(ras_target_check);
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    latxs_tr_restore_temp_register_mask(-1);
    latxs_append_ir2_opnd2_(lisa_mov, &hva, &la_a0);
#endif
    latxs_append_ir2_opnd2(LISA_BEQZ, &hva, &miss);
#ifdef CONFIG_RAS_PROFILER
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &la_a0, &la_env);
    latxs_tr_gen_call_to_helper(ras_hit_count);
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
#endif
#if defined(CONFIG_RAS_CHECK) || defined(CONFIG_RAS_PROFILER)
    latxs_tr_gen_load_from_curr_eip(ret_gva);
#endif
    latxs_append_ir2_opnd2i(LISA_JIRL, &la_zero, &hva, 0);

    /* TODO miss */
    latxs_append_ir2_opnd1(LISA_LABEL, &miss);
#if defined(CONFIG_RAS_CHECK) || defined(CONFIG_RAS_PROFILER)
    latxs_tr_gen_load_from_curr_eip(ret_gva);
#endif

    latxs_ra_free_temp(&hva);
    latxs_ra_free_temp(&ptr);
    latxs_ra_free_temp(&entry);
    return true;
}

bool latxs_translate_ras_pop(IR2_OPND *ret_gva)
{
#ifdef CONFIG_RAS_DISABLE_CPL0
    TranslationBlock *tb = lsenv->tr_data->curr_tb;
    if (!(tb->flags & HF_CPL_MASK)) {
        return true;
    }
#endif
    if (option_rastoptp) {
        TranslationBlock *tb = lsenv->tr_data->curr_tb;
        if ((tb->flags & HF_CPL_MASK) != HF_CPL_MASK) {
            return true;
        }
        if (!ras_tp_eligible(lsenv->tr_data->curr_ir1_inst)) {
            ras_tp_clear();
            return true;
        }
        return ras_tp_pop(ret_gva);
    }
#ifdef CONFIG_RAS_SLOW
    return latxs_translate_ras_pop_slow(ret_gva);
#endif
    IR2_OPND miss = latxs_ir2_opnd_new_label();
    uint32_t gva_off = offsetof(RAS_Entry, pc);
    uint32_t hva_off = offsetof(RAS_Entry, ptr);
    int entry_size = sizeof(RAS_Entry);

    /* get top addr and update it */
    IR2_OPND ras_top = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2(LISA_SCR2GR, &ras_top, &la_scr0);
    if (hamt_enabled()) latxs_tr_dmw_in_fix_mem(&ras_top);
    latxs_append_ir2_opnd2i(LISA_ADDI_D, &ras_top, &ras_top, -entry_size);

    /* compare gva */
    IR2_OPND ras_gva = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_LD_D, &ras_gva, &ras_top, gva_off);
    latxs_append_ir2_opnd3(LISA_BNE, &ras_gva, ret_gva, &miss);
    latxs_ra_free_temp(&ras_gva);

    /* hit */
    IR2_OPND ras_hva = latxs_ra_alloc_itemp();
    latxs_append_ir2_opnd2i(LISA_LD_D, &ras_hva, &ras_top, hva_off);
#ifdef CONFIG_RAS_CHECK
    latxs_tr_gen_store_to_curr_eip(ret_gva);
    latxs_tr_save_temp_register_mask(-1);
    latxs_tr_gen_call_to_helper_prologue_cfg(default_helper_cfg);
    latxs_append_ir2_opnd2_(lisa_mov, &la_a0, &la_env);
    latxs_append_ir2_opnd2_(lisa_mov, &la_a1, ret_gva);
    latxs_append_ir2_opnd2_(lisa_mov, &la_a2, &ras_hva);
    latxs_tr_gen_call_to_helper(ras_target_check);
    latxs_tr_gen_call_to_helper_epilogue_cfg(default_helper_cfg);
    latxs_tr_restore_temp_register_mask(-1);
#endif
#ifdef CONFIG_RAS_CHECK
    latxs_tr_gen_load_from_curr_eip(ret_gva);
#endif
    latxs_append_ir2_opnd2(LISA_GR2SCR, &la_scr0, &ras_top);
    latxs_append_ir2_opnd2i(LISA_JIRL, &la_zero, &ras_hva, 0);
    latxs_ra_free_temp(&ras_hva);
    latxs_ra_free_temp(&ras_top);

    /* TODO miss */
    latxs_append_ir2_opnd1(LISA_LABEL, &miss);
    return true;
}

#ifdef CONFIG_RAS_PROFILER
void ras_hit_count(CPUX86State *env)
{
    struct RAS_Profile *rp = (struct RAS_Profile *)env->ras_profile_ptr;
    rp->period_hit++;
}

void ras_total_count(CPUX86State *env)
{
    struct RAS_Profile *rp = (struct RAS_Profile *)env->ras_profile_ptr;
    rp->period_total++;
}

void ras_profiler(CPUState *cpu)
{
    CPUX86State *env = cpu->env_ptr;
    struct RAS_Profile *rp = (struct RAS_Profile *)env->ras_profile_ptr;
    uint64_t cur_time = get_clock() / 1000000000;
    if (rp->cur_time != cur_time) {
        rp->cur_time = cur_time;
        rp->total += rp->period_total;
        rp->hit += rp->period_hit;

        if (rp->total < 0) {
            rp->total = 0;
            rp->hit = 0;
        }

        if (rp->period_total) {
            double rate = (double)rp->hit / (double)rp->total * 100;
            double period_rate =
                (double)rp->period_hit / (double)rp->period_total * 100;
            fprintf(stderr,
                    "[RAS-PROFILER] [cpu%d] total %d hit %d rate %.2f%% "
                    "period_total %d period_hit %d period_rate %.2f%%\n",
                    cpu->cpu_index, rp->total, rp->hit, rate,
                    rp->period_total, rp->period_hit, period_rate);
        }
        rp->period_total = 0;
        rp->period_hit = 0;
    }
}
#endif
