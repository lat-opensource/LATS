/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HAMT_SMC_VCPU_BITMAP_H
#define HAMT_SMC_VCPU_BITMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One independently allocated bit array per vCPU; one bit per 4 KiB GPA. */
typedef struct HamtSmcVcpuBitmap {
    uint64_t *words;
    size_t pages;
    size_t word_count;
    /* Monotonic exclusive upper bound, shared with the generated refill. */
    uint64_t high_water;
} HamtSmcVcpuBitmap;

bool hamt_smc_vcpu_bitmap_init(HamtSmcVcpuBitmap *bitmap, size_t pages);
void hamt_smc_vcpu_bitmap_destroy(HamtSmcVcpuBitmap *bitmap);

static inline bool hamt_smc_vcpu_bitmap_mark(HamtSmcVcpuBitmap *bitmap,
                                              uint64_t gpa)
{
    uint64_t page = gpa >> 12;
    uint64_t end, old;

    if (!bitmap->words || page >= bitmap->pages) {
        return false;
    }
    end = page / 64 + 1;
    old = __atomic_load_n(&bitmap->high_water, __ATOMIC_RELAXED);
    while (old < end &&
           !__atomic_compare_exchange_n(&bitmap->high_water, &old, end, true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    __atomic_fetch_or(&bitmap->words[page / 64], UINT64_C(1) << (page % 64),
                      __ATOMIC_RELAXED);
    return true;
}

/* Unknown pages are conservatively treated as present. */
static inline bool hamt_smc_vcpu_bitmap_test(const HamtSmcVcpuBitmap *bitmap,
                                              uint64_t gpa)
{
    uint64_t page = gpa >> 12;

    if (!bitmap->words || page >= bitmap->pages) {
        return true;
    }
    return __atomic_load_n(&bitmap->words[page / 64], __ATOMIC_RELAXED) &
           (UINT64_C(1) << (page % 64));
}

/*
 * The caller must serialize clearing against this vCPU's fills, including
 * fills interrupted by an IPI. Clearing before the hardware invalidation or
 * without that protocol can lose the record of a live writable TLB entry.
 */
static inline void hamt_smc_vcpu_bitmap_clear(HamtSmcVcpuBitmap *bitmap)
{
    uint64_t count = __atomic_load_n(&bitmap->high_water, __ATOMIC_RELAXED);

    for (size_t i = 0; i < count; i++) {
        __atomic_store_n(&bitmap->words[i], 0, __ATOMIC_RELAXED);
    }
}

#endif
