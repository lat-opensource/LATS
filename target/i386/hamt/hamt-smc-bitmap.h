/*
 * SPDX-FileCopyrightText: 2026 LATS Project Authors
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef HAMT_SMC_BITMAP_H
#define HAMT_SMC_BITMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hamt-smc-vcpu-bitmap.h"

/*
 * Each vCPU has a bitmap recording the GPA pages it may have mapped.
 * Without smcbitmapflush, entries are monotonic. With it, the owning vCPU
 * clears its bitmap after full invalidation; interrupted C fills republish
 * and recheck ptwflags before returning to translated code. The page4k mask
 * is used only by legacy standalone tests. The second array records 1 MiB
 * TLB subpages and is consulted alongside the 4 KiB bitmap. The wildcard
 * records vCPUs whose fill could not be attributed to a tracked page.
 *
 * A filler publishes all covered pages, executes the full barrier below,
 * then reads ptwflags before creating a writable entry. A protector first
 * publishes the restrictive ptwflags, executes the same full barrier, and
 * then reads the bitmap. These barriers prohibit both sides from missing
 * each other. This alone does not serialize the IPI with the TLB write: if
 * an IPI may be acknowledged before a fill finishes, that path must recheck
 * the permission after filling and invalidate before returning to the guest.
 */
typedef struct HamtSmcBitmap {
    HamtSmcVcpuBitmap *vcpu;
    uint32_t *page4k;
    size_t page4k_count;
    uint32_t *page1m;
    size_t page1m_count;
    uint32_t wildcard;
    uint32_t *protect_pending;
    uint32_t unknown_pending;
} HamtSmcBitmap;

#define HAMT_SMC_BITMAP_ALL UINT32_MAX

/* Initialise once before publishing the structure to any vCPU. */
bool hamt_smc_bitmap_init(HamtSmcBitmap *bitmap, size_t page4k_count);

/* Only valid after all vCPUs and refill handlers have stopped using it. */
void hamt_smc_bitmap_destroy(HamtSmcBitmap *bitmap);

/* Stable shared mapping, initialized before use by a fill or protector. */
HamtSmcBitmap *hamt_smc_bitmap_get(void);

/* Begin before publishing NOWRITE; end only after synchronous TLB flush. */
static inline bool hamt_smc_protect_begin(HamtSmcBitmap *bitmap, uint64_t page)
{
    uint64_t index = page >> 12;

    if (!bitmap->protect_pending || index >= bitmap->page4k_count) {
        __atomic_fetch_add(&bitmap->unknown_pending, 1, __ATOMIC_SEQ_CST);
    } else {
        __atomic_fetch_add(&bitmap->protect_pending[index], 1, __ATOMIC_SEQ_CST);
    }
    return true;
}

static inline void hamt_smc_protect_end(HamtSmcBitmap *bitmap, uint64_t page)
{
    uint64_t index = page >> 12;

    if (!bitmap->protect_pending || index >= bitmap->page4k_count) {
        __atomic_fetch_sub(&bitmap->unknown_pending, 1, __ATOMIC_SEQ_CST);
    } else {
        __atomic_fetch_sub(&bitmap->protect_pending[index], 1, __ATOMIC_SEQ_CST);
    }
}

/* Called after observing NOWRITE. */
static inline bool hamt_smc_protect_pending(HamtSmcBitmap *bitmap, uint64_t page)
{
    uint64_t index = page >> 12;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!bitmap->protect_pending || index >= bitmap->page4k_count) {
        return __atomic_load_n(&bitmap->unknown_pending, __ATOMIC_SEQ_CST);
    }
    return __atomic_load_n(&bitmap->protect_pending[index], __ATOMIC_SEQ_CST);
}

static inline bool hamt_smc_bitmap_mark(HamtSmcBitmap *bitmap, uint64_t gpa,
                                        unsigned shift, unsigned cpu)
{
    uint32_t *array;
    uint32_t bit = cpu < 32 ? UINT32_C(1) << cpu : HAMT_SMC_BITMAP_ALL;
    size_t count;
    uint64_t index;

    if (shift == 12) {
        array = bitmap->page4k;
        count = bitmap->page4k_count;
    } else if (shift == 20) {
        array = bitmap->page1m;
        count = bitmap->page1m_count;
    } else {
        __atomic_fetch_or(&bitmap->wildcard, bit, __ATOMIC_RELAXED);
        return false;
    }
    index = gpa >> shift;
    if ((!array && !(shift == 12 && bitmap->vcpu)) ||
        index >= count || cpu >= 32) {
        __atomic_fetch_or(&bitmap->wildcard, bit, __ATOMIC_RELAXED);
        return false;
    }
    if (shift == 12 && bitmap->vcpu) {
        return hamt_smc_vcpu_bitmap_mark(&bitmap->vcpu[cpu], gpa);
    }
    __atomic_fetch_or(&array[index], bit, __ATOMIC_RELAXED);
    return true;
}

static inline void hamt_smc_bitmap_publish(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* Call only after the restrictive ptwflags store and a full barrier. */
static inline uint32_t hamt_smc_bitmap_mask(const HamtSmcBitmap *bitmap,
                                            uint64_t gpa)
{
    uint64_t page = gpa >> 12;
    uint64_t group = gpa >> 20;

    if ((!bitmap->vcpu && !bitmap->page4k) || !bitmap->page1m ||
        page >= bitmap->page4k_count || group >= bitmap->page1m_count) {
        return HAMT_SMC_BITMAP_ALL;
    }
    if (bitmap->vcpu) {
        uint32_t mask = __atomic_load_n(&bitmap->page1m[group], __ATOMIC_RELAXED) |
                        __atomic_load_n(&bitmap->wildcard, __ATOMIC_RELAXED);
        for (unsigned cpu = 0; cpu < 32; cpu++) {
            if (hamt_smc_vcpu_bitmap_test(&bitmap->vcpu[cpu], gpa)) {
                mask |= UINT32_C(1) << cpu;
            }
        }
        return mask;
    }
    return __atomic_load_n(&bitmap->page4k[page], __ATOMIC_RELAXED) |
           __atomic_load_n(&bitmap->page1m[group], __ATOMIC_RELAXED) |
           __atomic_load_n(&bitmap->wildcard, __ATOMIC_RELAXED);
}

#endif
