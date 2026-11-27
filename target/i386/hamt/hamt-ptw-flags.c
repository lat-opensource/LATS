/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/processor.h"
#include "cpu.h"
#include "hamt-options.h"
#include "hamt-ptw.h"
#include "latx/include/error.h"

#define HAMT_PTW_GPA_END (UINT64_C(128) << 30)

static uint16_t *ptwdata1m;
static uint8_t *ptwflags;
static const size_t ptwflags_size = HAMT_PTW_GPA_END >> TARGET_PAGE_BITS;
#define PTW_STAT_LOCK (1U << 7)
#define PTW_GROUP_LOCK (1U << 15)
static uint8_t *ptwflagstat;

static uint8_t hamt_ptwstat_lock(uint64_t index)
{
    uint8_t reasons;

    lsassert(index < ptwflags_size);
    do {
        reasons = qatomic_read(&ptwflagstat[index]);
        if (!(reasons & PTW_STAT_LOCK) &&
            qatomic_cmpxchg(&ptwflagstat[index], reasons,
                            reasons | PTW_STAT_LOCK) == reasons) {
            return reasons;
        }
        cpu_relax();
    } while (true);
}

static uint16_t hamt_ptwgroup_lock(uint64_t group)
{
    uint16_t count;

    lsassert(group < (ptwflags_size >> 8));
    do {
        count = qatomic_read(&ptwdata1m[group]);
        if (!(count & PTW_GROUP_LOCK) &&
            qatomic_cmpxchg(&ptwdata1m[group], count,
                            count | PTW_GROUP_LOCK) == count) {
            return count;
        }
        cpu_relax();
    } while (true);
}

/* Called with the group locked; count includes pending SMC restrictions. */
static void hamt_ptwflags_refresh_group(uint64_t group, uint16_t count)
{
    uint64_t first = group << 8;
    bool no_2m = count != 0;

    lsassertm(first + 256 <= ptwflags_size,
              "ptwflags group overflow: %" PRIu64, first);
    for (unsigned i = 0; !no_2m && i < 256; ++i) {
        if (qatomic_read(&ptwflags[first + i]) & HAMT_PTWFLAGS_WRITE_MASK) {
            no_2m = true;
        }
    }

    if (no_2m) {
        qatomic_or(&ptwflags[first], HAMT_PTWFLAGS_NO2M);
    } else {
        qatomic_and(&ptwflags[first], ~HAMT_PTWFLAGS_NO2M);
    }
}

/* Both 4K tables and the 1M counters cover the same GPA range. */
static size_t hamt_ptwflags_mapping_size(size_t pages)
{
    return 2 * pages + (pages >> 8) * sizeof(*ptwdata1m);
}

void hamt_ptwflags_reserve(uint64_t gpa_end)
{
    if (gpa_end <= HAMT_PTW_GPA_END) {
        return;
    }

    fprintf(stderr, "HAMT PTW tables cannot cover GPA end 0x%" PRIx64
            " (maximum 0x%" PRIx64 ")\n", gpa_end, HAMT_PTW_GPA_END);
    exit(EXIT_FAILURE);
}

__attribute__((__constructor__))
static void hamt_ptw_init(void)
{
    uint8_t *data = mmap(NULL, hamt_ptwflags_mapping_size(ptwflags_size),
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                         -1, 0);

    if (data == MAP_FAILED) {
        fprintf(stderr, "Cannot allocate HAMT PTW tables: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    lsassert(HAMT_HOLE_ASSERT(data));
    ptwflags = data;
    ptwflagstat = data + ptwflags_size;
    ptwdata1m = (uint16_t *)(ptwflagstat + ptwflags_size);
}

void *hamt_get_ptwflags(void)
{
    return ptwflags;
}

void hamt_ptwflags_set_mmio(uint64_t base, uint64_t size)
{
    uint64_t index = base >> TARGET_PAGE_BITS;
    uint64_t end = (base + size + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_BITS;

    lsassert(end <= ptwflags_size);
    while (index < end) {
        uint64_t group = index >> 8;
        uint64_t next = (group + 1) << 8;
        uint16_t count = hamt_ptwgroup_lock(group);

        qatomic_or(&ptwflags[group << 8], HAMT_PTWFLAGS_NO2M);
        smp_wmb();
        while (index < next && index < end) {
            qatomic_or(&ptwflags[index], HAMT_PTWFLAGS_NOACCESS);
            index++;
        }
        qatomic_store_release(&ptwdata1m[group], count);
    }
}

void hamt_ptwflags_vga_protect(uint64_t base, uint64_t size)
{
    uint64_t start = base >> TARGET_PAGE_BITS;
    uint64_t end = start + ((size + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_BITS);

    g_assert(!hamt_no_vga_dirty());
    for (uint64_t index = start; index < end; index++) {
        uint64_t group = index >> 8;
        uint8_t reasons = hamt_ptwstat_lock(index);
        uint16_t count = hamt_ptwgroup_lock(group);

        qatomic_or(&ptwflags[group << 8], HAMT_PTWFLAGS_NO2M);
        smp_wmb();
        qatomic_or(&ptwflags[index], HAMT_PTWFLAGS_NOWRITE);
        qatomic_and(&ptwflags[index], ~HAMT_PTWFLAGS_NOACCESS_MASK);
        qatomic_store_release(&ptwdata1m[group], count);
        qatomic_store_release(&ptwflagstat[index], reasons | PTW_REASON_VGA);
    }
}

static void hamt_ptwflags_reserve_smc(uint64_t group)
{
    uint16_t count = hamt_ptwgroup_lock(group);

    lsassert(count < 256);
    qatomic_or(&ptwflags[group << 8], HAMT_PTWFLAGS_NO2M);
    qatomic_store_release(&ptwdata1m[group], count + 1);
    smp_wmb();
}

void hamt_ptwflags_smc_protect_page(uint64_t page)
{
    uint64_t index = page >> TARGET_PAGE_BITS;
    uint64_t group = index >> 8;
    uint8_t reasons = hamt_ptwstat_lock(index);

    if (!(reasons & PTW_REASON_SMC)) {
        /* Reserve the count before publishing the page restriction. */
        hamt_ptwflags_reserve_smc(group);
        qatomic_or(&ptwflags[index], HAMT_PTWFLAGS_NOWRITE);
    }
    qatomic_store_release(&ptwflagstat[index], reasons | PTW_REASON_SMC);
}

void hamt_ptwflags_smc_unprotect_page(uint64_t page)
{
    uint64_t index = page >> TARGET_PAGE_BITS;
    uint64_t group = index >> 8;
    uint8_t reasons = hamt_ptwstat_lock(index);

    if (reasons & PTW_REASON_SMC) {
        uint16_t count;

        if (!(reasons & PTW_REASON_VGA)) {
            qatomic_and(&ptwflags[index], ~HAMT_PTWFLAGS_NOWRITE);
        }
        /* Do not withdraw the reservation until the page is updated. */
        smp_wmb();
        count = hamt_ptwgroup_lock(group);
        lsassert(count > 0);
        hamt_ptwflags_refresh_group(group, count - 1);
        qatomic_store_release(&ptwdata1m[group], count - 1);
    }
    qatomic_store_release(&ptwflagstat[index], reasons & ~PTW_REASON_SMC);
}

void hamt_ptwflags_vga_snapshot(uint64_t base, uint64_t size, uint8_t *dest)
{
    uint64_t start = base >> TARGET_PAGE_BITS;
    uint64_t count = (size + TARGET_PAGE_SIZE - 1) >> TARGET_PAGE_BITS;

    g_assert(!hamt_no_vga_dirty());
    for (uint64_t i = 0; i < count; i++) {
        uint64_t index = start + i;
        uint8_t reasons = hamt_ptwstat_lock(index);

        dest[i] = reasons | (qatomic_read(&ptwflags[index]) &
                             HAMT_PTWFLAGS_NOACCESS_MASK);
        qatomic_store_release(&ptwflagstat[index], reasons);
    }
}

bool hamt_ptwflags_vga_snapshot_is_dirty(uint64_t base, uint64_t size,
                                         uint8_t *snap)
{
    uint64_t start = base >> TARGET_PAGE_BITS;
    uint64_t end = TARGET_PAGE_ALIGN(base + size) >> TARGET_PAGE_BITS;

    g_assert(!hamt_no_vga_dirty());
    while (start < end) {
        if (!snap[start++]) {
            return true;
        }
    }
    return false;
}

void hamt_ptwflags_vga_write_page(uint64_t page)
{
    uint64_t index = page >> TARGET_PAGE_BITS;
    uint64_t group = index >> 8;
    uint8_t reasons = hamt_ptwstat_lock(index);

    if ((reasons & PTW_REASON_VGA) && !(reasons & PTW_REASON_SMC)) {
        uint16_t count = hamt_ptwgroup_lock(group);

        qatomic_and(&ptwflags[index], ~HAMT_PTWFLAGS_NOWRITE);
        smp_wmb();
        hamt_ptwflags_refresh_group(group, count);
        qatomic_store_release(&ptwdata1m[group], count);
    }
    qatomic_store_release(&ptwflagstat[index], reasons & ~PTW_REASON_VGA);
}

uint8_t hamt_ptwflags_get_page(uint64_t page)
{
    return qatomic_read(&ptwflags[page >> TARGET_PAGE_BITS]);
}

/* Sample NOWRITE and its reasons together; MMIO and NO2M are independent. */
uint8_t hamt_ptwflags_get_page_state(uint64_t page, uint8_t *reasons)
{
    uint64_t index = page >> TARGET_PAGE_BITS;
    uint8_t state = hamt_ptwstat_lock(index);
    uint8_t flags = qatomic_read(&ptwflags[index]);

    qatomic_store_release(&ptwflagstat[index], state);
    *reasons = state;
    return flags;
}

bool hamt_ptwflags_smc_page_is_protected(uint64_t page)
{
    uint64_t index = page >> TARGET_PAGE_BITS;
    uint8_t reasons;

    do {
        reasons = qatomic_load_acquire(&ptwflagstat[index]);
        if (!(reasons & PTW_STAT_LOCK)) {
            return reasons & PTW_REASON_SMC;
        }
        cpu_relax();
    } while (true);
}

bool hamt_ptwflags_smc_range_is_protected(uint64_t base, uint64_t size)
{
    uint64_t start = base >> TARGET_PAGE_BITS;
    uint64_t end = TARGET_PAGE_ALIGN(base + size) >> TARGET_PAGE_BITS;

    while (start < end) {
        if (hamt_ptwflags_smc_page_is_protected(start++ << TARGET_PAGE_BITS)) {
            return true;
        }
    }
    return false;
}

bool hamt_ptwflags_large_page_blocked(uint64_t base, uint64_t size)
{
    uint64_t first = base >> 20;
    uint64_t last = (base + size - 1) >> 20;

    for (uint64_t group = first; group <= last; ++group) {
        uint8_t flags = hamt_ptwflags_get_page(group << 20);
        if (flags & HAMT_PTWFLAGS_NO2M) {
            return true;
        }
    }
    return false;
}
