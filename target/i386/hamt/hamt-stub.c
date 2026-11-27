/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include "hamt-va.h"
#include "hamt-tlb.h"
#include "hamt-ptw.h"
#include "hamt-init.h"
#include "target/i386/latx/tb-record.h"
#include "latx-options.h"
#include "target/i386/latx/include/latx-hamt.h"
#include "include/hamt-fake-fork.h"

/* Stub implementations for non-LoongArch hosts */

/* Forward declarations for functions without header prototypes */
void hamt_init(void);
int dune_enter(void *cpu, int mode, int cpuid);
void latxs_init_rr_thread_sigfpe(void);
void hamt_ras_tp_mirror(int cpuid, uint64_t native_tp);

int hamt_cpus_num = 0;
int hamt_va = 0;

void hamt_init_options_configuration(void)
{
}

void hamt_init(void)
{
}

bool hamt_is_running(void)
{
    return false;
}

pid_t hamt_fake_fork(void *dunekvmcpu, void *fn, void *data)
{
    (void)dunekvmcpu;
    (void)fn;
    (void)data;
    errno = ENOSYS;
    return -1;
}

bool hamt_switch_pcid(struct CPUArchState *env, uint64_t cr3, bool noflush)
{
    return false;
}

void hamt_flush_tlb_all(void)
{
}

void hamt_flush_tlb_page(uint64_t vaddr)
{
}

void hamt_protect_code(uint64_t guest_phys_page, int full)
{
}

bool hamt_ptwflags_smc_page_is_protected(uint64_t page)
{
    return false;
}

void hamt_ptwflags_smc_protect_page(uint64_t page)
{
}

void hamt_ptwflags_smc_unprotect_page(uint64_t page)
{
}

uint8_t hamt_ptwflags_get_page(uint64_t page)
{
    return 0;
}

void hamt_ptwflags_set_mmio(uint64_t start, uint64_t size)
{
}

void hamt_ptwflags_vga_protect(uint64_t start, uint64_t size)
{
}

void hamt_ptwflags_vga_snapshot(uint64_t start, uint64_t size, uint8_t *dest)
{
}

bool hamt_ptwflags_vga_snapshot_is_dirty(uint64_t start, uint64_t size, uint8_t *snap)
{
    return false;
}

void hamt_ptwflags_vga_write_page(uint64_t page)
{
}

bool hamt_ptwflags_smc_range_is_protected(uint64_t start, uint64_t size)
{
    return false;
}

void *hamt_get_ptwflags(void)
{
    return NULL;
}

void hamt_ptwflags_reserve(uint64_t gpa_end)
{
}

/* dune_enter stub */
int dune_enter(void *cpu, int mode, int cpuid)
{
    return 0;
}

/* latxs_init_rr_thread_sigfpe stub */
void latxs_init_rr_thread_sigfpe(void)
{
}

void hamt_poison_tb_entry(TranslationBlock *tb)
{
}

void hamt_ras_tp_mirror(int cpuid, uint64_t native_tp)
{
}
