/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdlib.h>
#include <string.h>

#include "hamt-smc-vcpu-bitmap.h"

bool hamt_smc_vcpu_bitmap_init(HamtSmcVcpuBitmap *bitmap, size_t pages)
{
    size_t count;
    uint64_t *words;

    memset(bitmap, 0, sizeof(*bitmap));
    if (!pages || pages > SIZE_MAX - 63) {
        return false;
    }
    count = (pages + 63) / 64;
    if (count > SIZE_MAX / sizeof(*words)) {
        return false;
    }
    words = calloc(count, sizeof(*words));
    if (!words) {
        return false;
    }
    bitmap->words = words;
    bitmap->pages = pages;
    bitmap->word_count = count;
    return true;
}

void hamt_smc_vcpu_bitmap_destroy(HamtSmcVcpuBitmap *bitmap)
{
    free(bitmap->words);
    memset(bitmap, 0, sizeof(*bitmap));
}
