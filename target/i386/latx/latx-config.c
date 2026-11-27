/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "common.h"
#include "ir1.h"
#include "ir2.h"
#include "lsenv.h"
#include "reg-alloc.h"
#include "latx-options.h"
#include "flag-reduction.h"
#include "translate.h"
#include "hamt-ptw.h"
#include "latx-config.h"
#include "latx-sys-flag-rdtn.h"
#include "latx-static-codes.h"
#include "latx-tb-trace-sys.h"
#include "latx-sys-inst-ptn.h"
#include "latx-native-asm.h"
#include "latx-cp-counter.h"
#include "latx-ras.h"
#ifdef CONFIG_LATX_TU
#include "./include/latx-tu.h"
#endif

#ifdef CONFIG_LATX_PRINT
uint64_t cp_num[2];
uint64_t cp_tb_hit[2];
uint64_t cp_itlb_hit[2];
uint64_t cp_ptw_hit[2];
uint64_t cp_check_hit[2];
#endif

#include "latx-hamt.h"
#include "latx-hamt-counter.h"
#include "hamt-cpu.h"
#include "hamt-dmw.h"

#include "sysemu/rr.h"

#include "latxs-imm-cache.h"

static void xtm_capstone_init(void)
{
    if (cs_open(CS_ARCH_X86, CS_MODE_32, &handle) != CS_ERR_OK) {
        fprintf(stderr, "%s %s %d error : cs_open \n", __FILE__, __func__,
                __LINE__);
        exit(-1);
    }
    cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);

#ifdef CONFIG_SOFTMMU
    if (cs_open(CS_ARCH_X86, CS_MODE_16, &handle16) != CS_ERR_OK) {
        fprintf(stderr, "%s %s %d error : cs_open\n",
                __FILE__, __func__, __LINE__);
        exit(-1);
    }
    cs_option(handle16, CS_OPT_DETAIL, CS_OPT_ON);
#endif

    if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle64) != CS_ERR_OK) {
        fprintf(stderr, "%s %s %d error : cs_open\n",
                __FILE__, __func__, __LINE__);
        exit(-1);
    }
    cs_option(handle64, CS_OPT_DETAIL, CS_OPT_ON);
}

static void __attribute__((__constructor__)) latx_init(void)
{
    latxs_options_init();
}

static __thread ENV lsenv_real;
__thread TRANSLATION_DATA tr_data_real;

/* global lsenv defined here */
__thread ENV *lsenv;

#include "accel/tcg/tb-jmp-cache.h"
#include "qemu/log-for-trace.h"

#ifdef CONFIG_SOFTMMU
extern void *hpet_global_ptr;
extern void *timers_state_ptr;

extern QemuMutex qemu_global_mutex;
#endif

void latx_lsenv_init(CPUArchState *env)
{
    lsenv = &lsenv_real;
    lsenv->cpu_state = env;
    lsenv->tr_data = &tr_data_real;
#ifdef CONFIG_LATX_TU
    tu_control_init();
#endif

    if (!lsenv->tr_data->slow_path_rcd) {
        lsenv->tr_data->slow_path_rcd_max = 4;
        lsenv->tr_data->slow_path_rcd_nr  = 0;
        lsenv->tr_data->slow_path_rcd =
            mm_calloc(4, sizeof(softmmu_sp_rcd_t));
    }

#ifdef LATXS_IMM_CACHE_ENABLE
    lsenv->tr_data->dynamic_option_imm = option_imm_cache;
    imm_trace("lsenv init dynamic option_imm_cache:%d\n", option_imm_cache);
#endif

#ifndef CONFIG_LATX_OPT_NOFP
    if (option_soft_fpu_fast) {
        env->mode_fpu = 1;
    }
#endif

    CPUState *cpu = env_cpu(env);
    X86CPU *xcpu = X86_CPU(cpu);
    env->tb_jmp_cache_ptr = (void *)&cpu->tb_jmp_cache->array[0];
    env->latx_njc_cache_ptr = cpu->latx_njc_cache ?
        (void *)&cpu->latx_njc_cache->array[0] : NULL;
    if (hamt_enabled() && hamt_mode_dmw()) {
        if (option_table_addr) {
            env->tb_jmp_cache_ptr =
                (void *)(uintptr_t)DMW_IN(env->tb_jmp_cache_ptr);
        }
        if (env->latx_njc_cache_ptr) {
            env->latx_njc_cache_ptr =
                (void *)(uintptr_t)DMW_IN(env->latx_njc_cache_ptr);
        }
    }
    if (tucp_generation_enabled() && !env->tu_generation_array) {
        void *array = g_malloc0(option_tucp_generation_array_size);

        env->tu_generation_array = hamt_enabled() && hamt_mode_dmw() ?
            (void *)(uintptr_t)DMW_IN(array) : array;
        env->tu_generation = tucp_generation_counter() ? 1 : 0;
    }
    env->in_hamt_exception = false;
    env->cflags = cpu->tcg_cflags;
    env->s_ras_ptr = cpu->s_ras;
#ifdef CONFIG_RAS_PROFILER
    env->ras_profile_ptr = malloc(sizeof(struct RAS_Profile));
    memset(env->ras_profile_ptr, 0, sizeof(struct RAS_Profile));
#endif

#if defined(CONFIG_SOFTMMU) && defined(CONFIG_LATX_PROFILER)
    env->hamt_data_ptr = (void *)hamt_cpu_base[cpu->cpu_index].data;
#endif

    if (qemu_loglevel_mask(CPU_LOG_TB_NOCHAIN)) {
        latx_disable_all_tblink();
    }

    env->rr_counter_ptr = rr_counter_get_ptr();
#ifdef CONFIG_SOFTMMU
    env->qemu_global_mutex_ptr = (void *)&(qemu_global_mutex.spin);
#endif
    env->syscall_tb = NULL;

#ifdef CONFIG_SOFTMMU
    if (hamt_use_ptw()) {
        env->ptwflags = hamt_get_ptwflags();
    }
#endif
#ifdef CONFIG_LATX_PRINT
    printf("%s:%d env %p maps of ptwflags %p\n", __func__, __LINE__,
            env, env->ptwflags);
#endif
    xtm_capstone_init();

#ifdef CONFIG_LATX_TB_EXEC_TRACE
    if (!env->tb_exec_trace) {
        env->tb_exec_trace = latx_new_tb_exec_trace();
        printf("%s:%d tb exec record %p\n", __func__, __LINE__,
                env->tb_exec_trace);
    }
    env->tb_exec_trace_nr = 0;
#endif

    env->apic_state = xcpu->apic_state;
    env->interrupt_request_ptr = &(cpu->interrupt_request);
#ifdef CONFIG_LATX_PRINT
    printf("%s:%d env %p apic_state %p\n", __func__, __LINE__,
            env, env->apic_state);
    printf("%s:%d env %p interrupt_request_ptr %p\n", __func__, __LINE__,
            env, env->interrupt_request_ptr);
#endif

#ifdef CONFIG_SOFTMMU
    env->hpet_state = hpet_global_ptr;
#ifdef CONFIG_LATX_PRINT
    printf("%s:%d env %p hpet_state %p\n", __func__, __LINE__,
            env, env->hpet_state);
#endif

    env->timers_state = timers_state_ptr;
#ifdef CONFIG_LATX_PRINT
    printf("%s:%d env %p timers_state %p\n", __func__, __LINE__,
            env, env->timers_state);
#endif
#endif
}

void latxs_lsenv_switch(CPUArchState *env)
{
    lsenv->cpu_state = env;
}

#include "sysemu/cpu-timers.h"

target_ulong get_eip_cpu_state(TranslationBlock *tb)
{
    if ((tb_cflags(tb) & CF_PCREL)) {
        CPUX86State *env = lsenv->cpu_state;
        /* if option ptb, may have to add an offset
           for TB in the sequence */
        int offset = 0;
        target_ulong eip = env->eip + env->segs[R_CS].base;
        // printf("get_eip: eip = %lx, offset = %x\n", eip, offset);
        return eip + offset;
    } else {
        return tb->pc;
    }
}
int target_latxs_host(CPUState *cpu, TranslationBlock *tb,
        int max_insns, void *code_highwater, int *search_size,
        void *host_pc)
{
#ifdef CONFIG_SOFTMMU
    if (icount_enabled() && qemu_rrtrace_enabled()) {
        /* rrtrace dump tb */
        if (rrtrace_mode == 3) {
            int do_dump = 0;
            /* dump via pc */
            if (rrtrace_func == 3 && qemu_rrtrace_dump_if_tb_pc(tb->pc)) {
                latx_enable_dump(); do_dump = 1;
            }
            if (!do_dump) latx_disable_dump();
        }
    }
#endif

    option_config_dump(tb->flags & 0x3);

    TRANSLATION_DATA *td = lsenv->tr_data;
    latxs_tr_sys_init(tb, max_insns, code_highwater, host_pc);
    td->is_tu_mode = 0;

    latxs_tr_disasm(tb);

    if (td->deadlock_avoidance) {
        return td->deadlock_avoidance;
    }

    /* perform instruction pattern */
    latxs_inst_pattern(tb);
    /* perform flag reduction OR just set flag */
    latxs_flag_reduction(tb);

    return latxs_tr_translate_tb(tb, search_size);
}

void latxs_fix_after_excp_or_int(CPUState *cpu)
{
    CPUX86State *env = cpu->env_ptr;

    latxs_tb_trace(env, cpu->curr_exec_tb, 1);

    lsassertm(env->is_fcsr_simd == 0, "fcsr simd %s\n", __func__);

#ifdef CONFIG_LATX_OPT_NOFP
    if (!env->native_recovered) {
        __nofp_sync_xmm_to_env(cpu);
        __nofp_sync_fpr_to_env(cpu, 0);
    }
#endif
    env->native_recovered = false;
}

/* This is used in target/i386/fpu_helper.c */
int latxs_get_top_bias_from_env(CPUX86State *env)
{
    return (int)env->vregs[3];
}

void *latx_dump_hamt_context(void)
{
    CPUX86State *env = lsenv->cpu_state;

    printf("-------- LATX-SYS ----------------------\n");
    printf("----------------------------------------\n");
    printf("         ENV %p\n", env);
    printf("         CPL %d\n", env->hflags & 0x3);
    printf("----------------------------------------\n");
    if (qemu_tcg_mtcc_enabled()) {
        for (int i = 0; i < MTCC_NR; ++i) {
            printf(" Code Cache %d Base %p\n",
                    i, tcg_ctx_mtcc[i]->code_gen_buffer);
        }
    } else {
        printf(" Code Cache Base %p\n",
                tcg_ctx->code_gen_buffer);
    }
    printf("----------------------------------------\n");

    return env;
}
