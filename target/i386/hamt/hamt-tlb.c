/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include <pthread.h>

#include "qemu/osdep.h"
#include "qemu/compiler.h"
#include "qemu/host-utils.h"
#include "qemu/typedefs.h"
#include "exec/cpu-defs.h"
#include "cpu.h"
#include "exec/cpu-all.h"
#include "exec/exec-all.h"
#include "qemu/xxhash.h"
#include "exec/ram_addr.h"

#include "lsenv.h"

#include "dune-info.h"
#include "dune-kvm-init-value.h"

#include "hamt.h"
#include "hamt-internal.h"
#include "hamt-data.h"
#include "hamt-tlb.h"
#include "hamt-ptw.h"
#include "hamt-smc-bitmap.h"
#include "hamt-ptw.h"
#include "hamt-interrupt.h"

#include "loongarch-csr.h"

#include "latx-hamt.h"
#include "latx-options.h"
#include "latx-hack.h"
#include "latx-sys-mmio.h"

static inline void tlb_read(void) { __asm__ __volatile__("tlbrd"); }
static inline void tlb_probe(void) { __asm__ __volatile__("tlbsrch"); }

static inline void tlb_write_indexed(void) { __asm__ __volatile__("tlbwr"); }
static inline void tlb_write_random(void) { __asm__ __volatile__("tlbfill"); }

static bool tlbindex_valid(int32_t index) { return index >= 0; }

static inline void enable_pg(void)
{
    __asm__ __volatile__(
            "ori      $t1, $zero, 0x18\n\t"
            "ori      $t0, $zero, %0\n\t"
            "csrxchg  $t0, $t1,   %1\n\t"
            :
            : "i"(CSR_CRMD_PG), "i"(LOONGARCH_CSR_CRMD)
            : "$t0", "$t1"
            );
}

static inline void disable_pg(void)
{
    __asm__ __volatile__(
            "ori      $t1, $zero, 0x18\n\t"
            "ori      $t0, $zero, %0\n\t"
            "csrxchg  $t0, $t1,   %1\n\t"
            :
            : "i"(CSR_CRMD_DA), "i"(LOONGARCH_CSR_CRMD)
            : "$t0", "$t1"
            );
}

static inline void set_attr(int r, int w, int x, uint64_t *addr)
{
    if (r) clear_bit(61, addr);
    else set_bit(61, addr);

    if (x) clear_bit(62, addr);
    else set_bit(62, addr);

    if (w) set_bit(1, addr);
    else clear_bit(1, addr);
}


/*
 * invtlb op info addr
 * (0x1 << 26) | (0x24 << 20) | (0x13 << 15) |
 * (addr << 10) | (info << 5) | op
 */
/* A TLB entry contains two 4K pages. An IPI between tlbrd and tlbwr
 * could otherwise resurrect the old sibling after acknowledging a flush. */
static inline uint32_t hamt_tlb_pair_irq_save(void)
{
    uint32_t old = 0;

    if (hamt_enable_ipi()) {
        __asm__ __volatile__("csrxchg %0, %1, %2"
                             : "+r"(old)
                             : "r"(CSR_CRMD_IE), "i"(LOONGARCH_CSR_CRMD)
                             : "memory");
    }
    return old;
}

static inline void hamt_tlb_pair_irq_restore(uint32_t old)
{
    if (hamt_enable_ipi()) {
        __asm__ __volatile__("csrxchg %0, %1, %2"
                             : "+r"(old)
                             : "r"(CSR_CRMD_IE), "i"(LOONGARCH_CSR_CRMD)
                             : "memory");
    }
}

static uint64_t hamt_local_full_invalidations;

static inline void hamt_smc_clear_local_bitmap(void)
{
    CPUState *cpu = current_cpu;

    if (option_smc_bitmap_flush && hamt_smc_bitmap_enabled() &&
        cpu && cpu->cpu_index >= 0 && cpu->cpu_index < HAMT_CPU_MAX) {
        HamtSmcBitmap *bitmap = hamt_smc_bitmap_get();

        if (bitmap->vcpu) {
            /* In-flight C fills republish and recheck after their TLB write. */
            smp_mb();
            hamt_smc_vcpu_bitmap_clear(&bitmap->vcpu[cpu->cpu_index]);
            smp_mb();
        }
    }
}

#define STLBSETS 256
static inline void __local_flush_tlb_all(void)
{
    /* invtlb 0x3 : flush guest all TLB that G=0 */
    __asm__ __volatile__(
        ".word ((0x6498000) | (0 << 10) | (0 << 5) | 0x3)\n\t"
        );
    qatomic_inc(&hamt_local_full_invalidations);
    hamt_smc_clear_local_bitmap();
}
static inline void local_flush_tlb_all(void)
{
    disable_pg();
    /* invtlb 0x3 : flush guest all TLB that G=0 */
    __asm__ __volatile__(
        ".word ((0x6498000) | (0 << 10) | (0 << 5) | 0x3)\n\t"
        );
    enable_pg();
    qatomic_inc(&hamt_local_full_invalidations);
    hamt_smc_clear_local_bitmap();
    latx_mem_clear_hpet_vpages_non_global();
    latx_mem_clear_apic_vpages_non_global();
}

static inline uint32_t guest_asid(void)
{
    CPUX86State *env = current_cpu->env_ptr;

    return option_pcid_asid ? env->hamt_guest_asid : 0xf;
}

bool hamt_switch_pcid(CPUX86State *env, uint64_t cr3, bool noflush)
{
    unsigned slot = cr3 & (X86_PCID_SLOTS - 1);
    unsigned asid = X86_HAMT_ASID_BASE + slot;
    unsigned bits = extract32(read_csr_asid(), 16, 8);

    /* ASID 0 belongs to the translator and 0xf to the untagged guest. */
    if (bits < 5) {
        return false;
    }
    bool same_context = noflush && env->pcid_context[slot] == cr3;
    bool retain = same_context && (env->pcid_valid & (1U << slot));

    /* Select the software bank even when SMC protection flushed its ASID. */
    if (option_pcid_cache) {
        retain &= latx_pcid_cache_select(env_cpu(env), slot, same_context);
    }
    if (retain) {
        if (option_pcid_stats) {
            qatomic_inc(&env->pcid_stats.retained);
        }
    } else {
        /* INVTLB op 4: local non-global entries for this ASID only. */
        __asm__ __volatile__("invtlb 0x4, %0, $zero" :: "r"((uint64_t)asid)
                             : "memory");
        env->pcid_context[slot] = cr3;
        env->pcid_valid |= 1U << slot;
        if (option_pcid_stats) {
            qatomic_inc(&env->pcid_stats.bindings);
        }
    }
    env->hamt_guest_asid = asid;
    latx_mem_clear_hpet_vpages_non_global();
    latx_mem_clear_apic_vpages_non_global();
    return true;
}

void hamt_set_tlb_simple(uint64_t ehi,
        uint64_t elo0, uint64_t elo1,
        int is_tlbr)
{
    if (is_tlbr) {
        ehi &= ~0x1f;
        ehi |= PS_4K ;
        if (hamt_mode_dmw()) {
            write_csr_asid(guest_asid());
        }
        write_csr_tlbr_ehi(ehi);
        write_csr_tlbr_elo0(elo0);
        write_csr_tlbr_elo1(elo1);
        tlb_write_random();
        if (hamt_mode_dmw()) write_csr_asid(0);
        return;
    }

    disable_pg();

    write_csr_tlbehi(ehi);
    if (hamt_mode_dmw()) {
        write_csr_asid(guest_asid());
    }
    tlb_probe();
    int32_t tlbidx = read_csr_tlbidx();
    if (tlbidx == 0xc00083f) {
        __local_flush_tlb_all();
        tlbidx |= 0x80000000;
    }

    tlbidx &= 0xc0ffffff;
    tlbidx |= PS_4K << PS_SHIFT;
    if (hamt_mode_dmw()) {
        write_csr_asid(guest_asid());
    }
    write_csr_tlbehi(ehi);
    write_csr_tlbelo0(elo0);
    write_csr_tlbelo1(elo1);
    write_csr_tlbidx(tlbidx);

    if (tlbindex_valid(tlbidx)) {
        tlb_write_indexed();
    } else {
        tlb_write_random();
    }

    if (hamt_mode_dmw()) write_csr_asid(0);

    enable_pg();
}

void hamt_flush_tlb_all(void)
{
    CPUX86State *env = current_cpu->env_ptr;

    if (option_pcid_asid) {
        env->pcid_valid = 0;
    } else {
        env->hamt_large_page_bits = 0;
    }
    local_flush_tlb_all();
}

void hamt_flush_tlb_page(uint64_t vaddr)
{
    CPUX86State *env = current_cpu->env_ptr;

    latx_mem_clear_hpet_vpage(vaddr);
    latx_mem_clear_apic_vpage(vaddr);

    /* Tagged or shared hardware contexts may contain other copies. */
    if (option_pcid_asid || !qemu_tcg_mttcg_enabled() ||
        (env->hamt_large_page_bits & hamt_large_page_bit(vaddr))) {
        hamt_flush_tlb_all();
        return;
    }
    hamt_set_tlb((current_cpu->env_ptr),
            vaddr, 0x0, 0x0, false);
}

void hamt_flush_tlb_global_mmio(void)
{
    latx_mem_clear_hpet_vpages();
    latx_mem_clear_apic_vpages();
}

void hamt_flush_tlb_page_mmio(uint64_t vaddr)
{
    latx_mem_clear_hpet_vpage(vaddr);
    latx_mem_clear_apic_vpage(vaddr);
}

static void hamt_set_tlbr(uint64_t vaddr, uint64_t paddr,
        int prot, bool mode)
{
    uint64_t csr_tlbr_ehi = 0;
    uint64_t csr_tlbr_elo0 = 0;
    uint64_t csr_tlbr_elo1 = 0;
    uint32_t csr_asid = guest_asid();

    int32_t csr_tlbidx = 0;
    uint32_t irq_state = hamt_tlb_pair_irq_save();

    int w = prot & PAGE_WRITE ? 1 : 0;
    int r = prot & PAGE_READ  ? 1 : 0;
    int x = prot & PAGE_EXEC  ? 1 : 0;

    csr_tlbr_ehi = vaddr & ~0x1fffULL;
    write_csr_tlbr_ehi(csr_tlbr_ehi);
    if (hamt_mode_dmw()) write_csr_asid(csr_asid);
    tlb_probe(); /* tlbsrch */
    csr_tlbidx = read_csr_tlbidx();
    if (tlbindex_valid(csr_tlbidx)) {
        tlb_read(); /* tlbrd: always update TLBELO0/1 */
        csr_tlbr_elo0 = read_csr_tlbelo0();
        csr_tlbr_elo1 = read_csr_tlbelo1();
    }

    //FIX
    int is_2111 = 0;
    if (csr_tlbidx == 0xc00083f) {
        local_flush_tlb_all();
        csr_tlbidx |= 0x80000000;
        is_2111 = 1;
    }

    if (vaddr & 0x1000) {
        if (mode) {
            csr_tlbr_elo1 = ((paddr >> 12 << 12) | TLBRELO0_STANDARD_BITS) & (~((uint64_t)0xe000 << 48));
            set_attr(r, w, x, &csr_tlbr_elo1);
        }
        else csr_tlbr_elo1 = 0; 
        if (is_2111) csr_tlbr_elo0 = 0;
    } else {
        if (mode) {
            csr_tlbr_elo0 = ((paddr >> 12 << 12) | TLBRELO0_STANDARD_BITS) & (~((uint64_t)0xe000 << 48));
            set_attr(r, w, x, &csr_tlbr_elo0);
        }
        else csr_tlbr_elo0 = 0; 
        if (is_2111) csr_tlbr_elo1 = 0;
    }

    csr_tlbidx &= 0xc0ffffff;
    csr_tlbidx |= PS_4K << PS_SHIFT;

    csr_tlbr_ehi &= ~0x1f;
    csr_tlbr_ehi |= PS_4K ;

    if (hamt_mode_dmw()) write_csr_asid(csr_asid);
    write_csr_tlbr_ehi(csr_tlbr_ehi);
    write_csr_tlbr_elo0(csr_tlbr_elo0);
    write_csr_tlbr_elo1(csr_tlbr_elo1);
    write_csr_tlbidx(csr_tlbidx);

    /* tlbwr : tlbfill */
    tlbindex_valid(csr_tlbidx) ? tlb_write_indexed() : tlb_write_random();

    if (hamt_mode_dmw()) write_csr_asid(0);
    hamt_tlb_pair_irq_restore(irq_state);
}

/*
 * mode:
 *     true: hamt_set_tlb fills in valid pte entry
 *     false: hamt_set_tlb fills in invalid pte entry
 * trap_code:
 *     0: tlbsearch
 *     1: tlbwr
 *     2: tlbfill
 * TODO:
 *     tlbelo0 / tlbelo1 may not be right
 */
void hamt_set_tlb(void *env,
        uint64_t vaddr, uint64_t paddr, int prot, bool mode)
{
    uint64_t csr_tlbehi = 0;
    uint64_t csr_tlbelo0 = 0;
    uint64_t csr_tlbelo1 = 0;
    uint32_t csr_asid = guest_asid();
    int32_t csr_tlbidx;
    uint32_t irq_state = hamt_tlb_pair_irq_save();

    int w = prot & PAGE_WRITE ? 1 : 0;

    disable_pg();

    csr_tlbehi = vaddr & ~0x1fffULL;
    write_csr_tlbehi(csr_tlbehi);
    if (hamt_mode_dmw()) write_csr_asid(csr_asid);
    tlb_probe();
    csr_tlbidx = read_csr_tlbidx();
    if (csr_tlbidx >= 0) {
        tlb_read();
        csr_tlbelo0 = read_csr_tlbelo0();
        csr_tlbelo1 = read_csr_tlbelo1();
    }

    if (vaddr & 0x1000) {
        if (mode) {
            csr_tlbelo1 = ((paddr >> 12 << 12) | 0x11) & (~((uint64_t)0xe000 << 48));
            if (w) csr_tlbelo1 |= 0x2;
        }
        else csr_tlbelo1 = 0; 
    } else {
        if (mode) {
            csr_tlbelo0 = ((paddr >> 12 << 12) | 0x11) & (~((uint64_t)0xe000 << 48));
            if (w) csr_tlbelo0 |= 0x2;
        }
        else csr_tlbelo0 = 0;
    }

    /* set page size 4K */
    csr_tlbidx &= 0xc0ffffff;
    csr_tlbidx |= PS_4K << PS_SHIFT;
    if (hamt_mode_dmw()) write_csr_asid(csr_asid);
    write_csr_tlbehi(csr_tlbehi);
    write_csr_tlbelo0(csr_tlbelo0);
    write_csr_tlbelo1(csr_tlbelo1);
    write_csr_tlbidx(csr_tlbidx);

    tlbindex_valid(csr_tlbidx) ? tlb_write_indexed() : tlb_write_random();

    if (hamt_mode_dmw()) write_csr_asid(0);

    enable_pg();
    hamt_tlb_pair_irq_restore(irq_state);
}

void hamt_set_hardware_tlb_nocheck(void *env,
        uint64_t vaddr, uint64_t paddr,
        int prot, int is_tlbr)
{
    bool mode = prot ? true : false;
    if (is_tlbr) {
        hamt_set_tlbr(vaddr, paddr, prot, mode);
    } else {
        hamt_set_tlb(env, vaddr, paddr, prot, mode);
    }
}

void hamt_set_hardware_tlb(void *env,
        uint64_t vaddr, uint64_t paddr,
        int prot, int is_tlbr)
{
    if (hamt_enabled() && hamt_is_running()) {
        hamt_set_hardware_tlb_nocheck(env, vaddr, paddr, prot, is_tlbr);
    }
}

static uint64_t smc_protect_calls, smc_flush_candidates, smc_flush_selected;
static uint64_t smc_full_fallbacks;
static uint64_t smc_deferred_published, smc_deferred_skipped;
static uint64_t smc_resume_invalidations;
static pthread_once_t smc_stats_once = PTHREAD_ONCE_INIT;
extern uint64_t hamt_data_area[];
static unsigned smc_deferred_flush[HAMT_CPU_MAX];

void hamt_smc_resume_flush(CPUState *cpu)
{
    int cpuid = cpu->cpu_index;

    if (!hamt_enabled() || !hamt_is_running() ||
        cpuid < 0 || cpuid >= HAMT_CPU_MAX) {
        return;
    }
    if (qatomic_xchg(&smc_deferred_flush[cpuid], 0)) {
        hamt_flush_tlb_all();
        qatomic_inc(&smc_resume_invalidations);
    }
}

static void hamt_smc_defer_flush(CPUState *cpu)
{
    g_assert(cpu->cpu_index >= 0 && cpu->cpu_index < HAMT_CPU_MAX);
    qatomic_inc(&smc_deferred_published);
    qatomic_set(&smc_deferred_flush[cpu->cpu_index], 1);
    smp_mb();
}

static void hamt_smc_stats_print(void)
{
    uint64_t candidates = qatomic_read(&smc_flush_candidates);
    uint64_t selected = qatomic_read(&smc_flush_selected);
    uint64_t ipi_full = 0;

    for (int i = 0; i < hamt_cpus_num && i < HAMT_CPU_MAX; i++) {
        if (hamt_data_area[i]) {
            uint64_t *area = (uint64_t *)hamt_data_area[i];
            ipi_full += qatomic_read(&area[HD_CNT_IPI_INDEX]);
        }
    }

    fprintf(stderr, "HAMT SMC bitmap: protect=%" PRIu64
            " candidates=%" PRIu64 " selected=%" PRIu64
            " skipped=%" PRIu64 " full=%" PRIu64 "\n",
            qatomic_read(&smc_protect_calls), candidates, selected,
            candidates - selected, qatomic_read(&smc_full_fallbacks));
    fprintf(stderr, "HAMT TLB full invalidations: local=%" PRIu64
            " ipi=%" PRIu64 "\n",
            qatomic_read(&hamt_local_full_invalidations), ipi_full);
    fprintf(stderr, "HAMT SMC deferred: published=%" PRIu64
            " skipped=%" PRIu64 " resume=%" PRIu64 "\n",
            qatomic_read(&smc_deferred_published),
            qatomic_read(&smc_deferred_skipped), qatomic_read(&smc_resume_invalidations));
}

static void hamt_smc_stats_init(void)
{
    atexit(hamt_smc_stats_print);
}

void hamt_protect_code(uint64_t guest_phys_page, int full)
{
    if (!(hamt_enabled() && hamt_is_running())) {
        return;
    }

    CPUState *cpu;
    int ncpus = hamt_cpus_num;
    int cpuid = current_cpu->cpu_index;
    uint64_t sequence = 0;
    uint32_t targets = ncpus >= 32 ? UINT32_MAX : (1U << ncpus) - 1;
    bool selective = hamt_smc_bitmap_enabled() && ncpus > 0 &&
                     ncpus <= 32 && cpuid >= 0 && cpuid < ncpus;

    if (selective) {
        CPU_FOREACH(cpu) {
            if (cpu->cpu_index < 0 || cpu->cpu_index >= ncpus) {
                selective = false;
            }
        }
    }

    pthread_once(&smc_stats_once, hamt_smc_stats_init);
    qatomic_inc(&smc_protect_calls);
    qatomic_add(&smc_flush_candidates, ncpus);
    if (selective) {
        if (!full) {
            hamt_smc_bitmap_publish();
            targets &= hamt_smc_bitmap_mask(hamt_smc_bitmap_get(),
                                            guest_phys_page);
        } else {
            qatomic_inc(&smc_full_fallbacks);
        }
        qatomic_add(&smc_flush_selected, __builtin_popcount(targets));
    } else {
        qatomic_inc(&smc_full_fallbacks);
        qatomic_add(&smc_flush_selected, ncpus);
    }

    if (!selective || (targets & (1U << cpuid))) {
        hamt_flush_tlb_all();
    }

    if (hamt_enable_ipi_sync()) {
        sequence = hamt_prepare_ipi(cpuid, ncpus);
    }

    CPU_FOREACH(cpu) {
        if (!qemu_cpu_is_self(cpu) &&
            (!selective || (targets & (1U << cpu->cpu_index)))) {
            hamt_send_ipi(cpuid, cpu);
        }
    }

    uint64_t crmd = read_csr_crmd(); (void)crmd;
    uint64_t ecfg = read_csr_ecfg(); (void)ecfg;
    lsassertm((crmd & CSR_CRMD_IE) && (ecfg & ECFGF_IPI),
            "WARNING: ipi wait but ipi disabled. CRMD %lx ECFG %lx",
            crmd, ecfg);

    if (hamt_enable_ipi_sync()) {
        uint64_t r = 0;
        unsigned retries = 0;
        int mask = selective ? targets & ~(1U << cpuid) : (-1) & ~(1 << cpuid);
        int waitcpus = selective ? __builtin_popcount((unsigned)mask) : ncpus - 1;
        while (!hamt_finish_ipi(cpuid, &r, ncpus, mask, waitcpus, sequence)) {
            smp_mb();
            CPU_FOREACH(cpu) {
                if (qemu_cpu_is_self(cpu)) continue;
                int c = cpu->cpu_index;
                if (!(mask & (1<<c))) continue;
                if (!qatomic_read(&cpu->running)) {
                    hamt_smc_defer_flush(cpu);
                    if (!qatomic_read(&cpu->running)) {
                        qatomic_inc(&smc_deferred_skipped);
                        waitcpus -= 1;
                        mask = mask & ~(1<<c);
                        continue;
                    }
                }
                /* A same-source request can merge with an older pending IPI.
                 * Retry until the matching sequence has been acknowledged. */
                if (!(retries & 0xffff)) {
                    hamt_send_ipi(cpuid, cpu);
                }
            }
            retries++;
        }
        /* Do not publish completed protection before observing all acks. */
        smp_mb();
    }
}

void hamt_unprotect_code(uint64_t guest_phys_page)
{
    assert(0); /* TODO */
    if (!(hamt_enabled() && hamt_is_running())) {
        return;
    }
}
