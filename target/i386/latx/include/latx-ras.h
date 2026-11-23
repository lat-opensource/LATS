/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef LATX_RAS_H
#define LATX_RAS_H
#include "loongarch-extcontext.h"

#include "latx/include/ir1.h"
#include "latx/include/ir2.h"
#include "latx/include/latx-ras-config.h"

struct RAS_Entry {
    uint64_t pc;
    const void *ptr;    /* pointer to the translated code */
};

struct __attribute ((packed)) Soft_RAS {
    int reserved;
    int top;
    TranslationBlock* patching_tb;
    RAS_Entry stack[RAS_SIZE];
};

#ifdef CONFIG_RAS_PROFILER
struct RAS_Profile {
    long long total;
    long long hit;
    long long period_total;
    long long period_hit;
    uint64_t cur_time;
};
#endif

#define UC_PC(uc) ((uc)->uc_mcontext.__pc)
#define UC_GR(uc) ((uc)->uc_mcontext.__gregs)
#ifndef LOONGARCH_NEW_WORLD
#define UC_FREG(uc) ((uc)->uc_mcontext.__fpregs)
#endif

bool latxs_ras_link(TranslationBlock *tb, TranslationBlock *tb_next);
bool latxs_ras_tp_link_initial(CPUState *cpu, TranslationBlock *source,
                               TranslationBlock *target);
bool latxs_ras_close_relinkable(TranslationBlock *tb);
bool latxs_ras_tp_permanent_valid(TranslationBlock *tb);
void latxs_ras_unlink(TranslationBlock *tb);
void latxs_ras_unlink_target_locked(TranslationBlock *tb);
void latxs_init_rr_thread_sigsegv(CPUState *cpu);
uint64_t ras_target_check(CPUX86State *env, uint64_t gva, uint64_t hva);
TranslationBlock *export_tb_lookup(CPUState *cpu, vaddr pc,
                uint64_t cs_base, uint32_t flags, uint32_t cflags);
void latxs_flush_soft_ras(CPUState *cpu);
/* r2 is top in generated code, the native TLS pointer in C. */
void latxs_ras_tp_enter_c(void);
void latxs_ras_tp_leave_c(void);
void latxs_ras_tp_save(CPUX86State *env, const uint64_t *gprs);
void latxs_ras_tp_redirect(CPUX86State *env, uint64_t *gprs);
void latxs_ras_tp_prepare(CPUState *cpu, unsigned mtcc_id);
/* Skip all seven RAS instructions, retaining the original gate and payload. */
#define LATX_RAS_TP_CALL_SKIP (0x50000000U | (7U << 10))
int gen_latxs_ras_tp_repair(void *code_ptr);
uint32_t latxs_ras_tp_closed_gate(TranslationBlock *tb);
#ifdef CONFIG_LATX_TU
#define LATXS_RAS_TP_COLD_OFFSET(tb) ((tb)->s_data->ras_tp_cold_offset)
#define LATXS_RAS_TP_COLD_INSNS(tb) ((tb)->s_data->ras_tp_cold_insns)
#else
#define LATXS_RAS_TP_COLD_OFFSET(tb) ((tb)->ras_tp_cold_offset)
#define LATXS_RAS_TP_COLD_INSNS(tb) ((tb)->ras_tp_cold_insns)
#endif
#define LATXS_RAS_TP_COLD_SIZE(tb) (LATXS_RAS_TP_COLD_INSNS(tb) * 4U)
void latxs_ras_tp_emit_call_cold(void);
/* Invalidate generated return predictions before a TB lookup mode change. */
void latxs_ras_tp_emit_clear(void);
void ras_tp_repair_call(TranslationBlock *source);
#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
TranslationBlock *latxs_hras_local_target(TranslationBlock *source,
                                         TranslationBlock *target);
/* False permits a checked fallback; signed 36-bit overflow is fatal. */
bool latxs_hras_call_target_can_link(TranslationBlock *source,
        TranslationBlock *target, uintptr_t call_site, bool checked);
bool latxs_hras_call_target_needs_long(TranslationBlock *source,
        TranslationBlock *target, uintptr_t call_site, bool checked);
bool latxs_hras_open_check_gate(TranslationBlock *tb, uintptr_t hot_slot);
void latxs_hras_close_check_gate(TranslationBlock *tb);
bool latxs_hras_paired_return(const TranslationBlock *source);
bool latxs_hras_prepare_link(CPUState *cpu, TranslationBlock *source,
                              int n, TranslationBlock *target);
#endif
/* Only newly translated mode-2 ordinary AOT uses the paired layout. */
bool latxs_hras_jit_translate(const TranslationBlock *tb);
bool latxs_hras_aot_translate(void);
bool latxs_translate_ras_push(IR1_INST *pir1, IR2_OPND *return_pc);
bool latxs_translate_ras_pop(IR2_OPND  *ret_gva);
void ras_debug_helper(CPUX86State *env, TranslationBlock *tb);
uint32_t get_ras_cpuindex(TranslationBlock *tb);
void set_ras_cpuindex(TranslationBlock *tb, uint32_t index);
void latxs_ras_init(CPUState *cpu);
void ras_total_count(CPUX86State *env);
void ras_hit_count(CPUX86State *env);
void ras_profiler(CPUState *cpu);

#endif /* LATX_RAS_H */
