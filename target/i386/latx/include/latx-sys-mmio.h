/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _LATX_SYS_MMIO_H_
#define _LATX_SYS_MMIO_H_

//#define LATX_SYS_DEBUG_OPT_MMIO

extern uint64_t guest_hpet_base_ppage;
extern uint64_t guest_apic_base_ppage;

static inline
bool latx_mem_is_hpet(uint64_t paddr)
{
    return (paddr & ~0xfff) == guest_hpet_base_ppage;
}
static inline
bool latx_mem_is_apic(uint64_t paddr)
{
    return (paddr & ~0xfff) == guest_apic_base_ppage;
}

void latx_mem_set_hpet_vpage(uint64_t addr);
void latx_mem_clear_hpet_vpage(uint64_t addr);
void latx_mem_clear_hpet_vpages(void);
void latx_mem_clear_hpet_vpages_non_global(void);
void latx_mem_set_apic_vpage(uint64_t addr);
void latx_mem_clear_apic_vpage(uint64_t addr);
void latx_mem_clear_apic_vpages(void);
void latx_mem_clear_apic_vpages_non_global(void);
uint64_t *latx_mem_get_hpet_vpages(void);
uint64_t *latx_mem_get_apic_vpages(void);
void latx_apic_mem_write(uint64_t addr, uint64_t val, unsigned size,
                         void *tcptr);

void latx_mmio_tb_check(TranslationBlock *tb);
void latx_do_mmio_tb_retranslate(void *ptr, int opt);

#endif
