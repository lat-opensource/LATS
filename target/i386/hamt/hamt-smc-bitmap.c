/*
 * SPDX-FileCopyrightText: 2026 LATS Project Authors
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "hamt-smc-bitmap.h"

bool hamt_smc_bitmap_init(HamtSmcBitmap *bitmap, size_t page4k_count)
{
    size_t page1m_count;
    uint32_t *page1m, *pending;

    memset(bitmap, 0, sizeof(*bitmap));
    if (!page4k_count || page4k_count > SIZE_MAX / sizeof(*pending)) {
        return false;
    }
    page1m_count = page4k_count / 256 + !!(page4k_count % 256);
    page1m = mmap(NULL, page1m_count * sizeof(*page1m),
                  PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page1m == MAP_FAILED) {
        return false;
    }
    pending = mmap(NULL, page4k_count * sizeof(*pending),
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pending == MAP_FAILED) {
        munmap(page1m, page1m_count * sizeof(*page1m));
        return false;
    }
    bitmap->page4k_count = page4k_count;
    bitmap->page1m = page1m;
    bitmap->page1m_count = page1m_count;
    bitmap->protect_pending = pending;
    bitmap->vcpu = calloc(32, sizeof(*bitmap->vcpu));
    if (!bitmap->vcpu) {
        hamt_smc_bitmap_destroy(bitmap);
        return false;
    }
    for (unsigned cpu = 0; cpu < 32; cpu++) {
        if (!hamt_smc_vcpu_bitmap_init(&bitmap->vcpu[cpu], page4k_count)) {
            hamt_smc_bitmap_destroy(bitmap);
            return false;
        }
    }
    return true;
}

void hamt_smc_bitmap_destroy(HamtSmcBitmap *bitmap)
{
    if (bitmap->vcpu) {
        for (unsigned cpu = 0; cpu < 32; cpu++) {
            hamt_smc_vcpu_bitmap_destroy(&bitmap->vcpu[cpu]);
        }
        free(bitmap->vcpu);
    }
    if (bitmap->page4k) {
        munmap(bitmap->page4k, bitmap->page4k_count * sizeof(*bitmap->page4k));
    }
    if (bitmap->page1m) {
        munmap(bitmap->page1m, bitmap->page1m_count * sizeof(*bitmap->page1m));
    }
    if (bitmap->protect_pending) {
        munmap(bitmap->protect_pending, bitmap->page4k_count * sizeof(*bitmap->protect_pending));
    }
    memset(bitmap, 0, sizeof(*bitmap));
}
