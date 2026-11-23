/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HATM_PTW_H_
#define _HATM_PTW_H_

void *hamt_get_ptwdata(void);
void *hamt_get_ptwdata1m(void);
size_t hamt_get_ptwdata_size(void);
struct CPUState;
void hamt_smc_resume_flush(struct CPUState *cpu);
void hamt_ptwdata_set_mmio(uint64_t base, uint64_t size);
void hamt_ptwdata_smc_protect(uint64_t base, uint64_t size);
void hamt_ptwdata_smc_unprotect(uint64_t base, uint64_t size);
void hamt_ptwdata_smc_protect_page(uint64_t page);
void hamt_ptwdata_smc_unprotect_page(uint64_t page);
void hamt_ptwdata_vga_protect(uint64_t base, uint64_t size);
void hamt_ptwdata_vga_unprotect(uint64_t base, uint64_t size);

bool hamt_ptwdata_smc_page_is_protected(uint64_t page);
bool hamt_ptwdata_smc_range_is_protected(uint64_t base, uint64_t size);
bool hamt_ptwdata_vga_range_is_protected(uint64_t base, uint64_t size);

void hamt_ptwdata_vga_snapshot(uint64_t base, uint64_t size, uint8_t *dest);
bool hamt_ptwdata_vga_snapshot_is_dirty(uint64_t base, uint64_t size, uint8_t *snap);

uint8_t hamt_ptwdata_get_page(uint64_t page);
void hamt_ptwdata_set_page(uint64_t page, uint8_t v);

void *hamt_get_ptwrcd(int cpuid);
void hamt_set_ptwrcd(int cpuid, uint64_t gpa);
void hamt_clr_ptwrcd(int cpuid);

enum {
    HAMT_PTW_ERROR_NONE,
    HAMT_PTW_ERROR_DISABLE,
    HAMT_PTW_ERROR_PF,
    HAMT_PTW_ERROR_NOTRAM,
    HAMT_PTW_ERROR_SMC,
    HAMT_PTW_ERROR_VGA,
    HAMT_PTW_ERROR_MMIO,
};

typedef struct {
    int error;
    int error_code;
    uint64_t paddr;
} hamt_ptw_error_t;

#endif
