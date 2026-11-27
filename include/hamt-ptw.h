/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _HATM_PTW_H_
#define _HATM_PTW_H_

/* Final HAMT direct-access permissions for one 4K GPA. */
#define HAMT_PTWFLAGS_NOWRITE       (1U << 0)
#define HAMT_PTWFLAGS_NOACCESS      (1U << 1)
#define HAMT_PTWFLAGS_NOACCESS_MASK (0x7eU)
#define HAMT_PTWFLAGS_WRITE_MASK    (0x7fU)
/* Set on the first page of a 1M group when a 1M mapping is unsafe. */
#define HAMT_PTWFLAGS_NO2M          (1U << 7)

/* Reasons for NOWRITE, separate from the hardware permissions. */
#define PTW_REASON_SMC (1U << 0)
#define PTW_REASON_VGA (1U << 1)

/* Fail if the exclusive GPA end exceeds the fixed PTW table capacity. */
void hamt_ptwflags_reserve(uint64_t gpa_end);
void *hamt_get_ptwflags(void);
bool hamt_ptwflags_large_page_blocked(uint64_t base, uint64_t size);
void hamt_ptwflags_set_mmio(uint64_t base, uint64_t size);
void hamt_ptwflags_vga_protect(uint64_t base, uint64_t size);
void hamt_ptwflags_vga_snapshot(uint64_t base, uint64_t size, uint8_t *dest);
bool hamt_ptwflags_vga_snapshot_is_dirty(uint64_t base, uint64_t size, uint8_t *snap);
void hamt_ptwflags_vga_write_page(uint64_t page);
void hamt_ptwflags_smc_protect_page(uint64_t page);
void hamt_ptwflags_smc_unprotect_page(uint64_t page);
bool hamt_ptwflags_smc_page_is_protected(uint64_t page);
bool hamt_ptwflags_smc_range_is_protected(uint64_t base, uint64_t size);

uint8_t hamt_ptwflags_get_page(uint64_t page);
uint8_t hamt_ptwflags_get_page_state(uint64_t page, uint8_t *reasons);

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
    HAMT_PTW_ERROR_NOWRITE,
};

typedef struct {
    int error;
    int error_code;
    uint64_t paddr;
} hamt_ptw_error_t;

#endif
