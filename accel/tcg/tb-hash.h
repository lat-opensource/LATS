/*
 * internal execution defines for qemu
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EXEC_TB_HASH_H
#define EXEC_TB_HASH_H

#include "exec/cpu-defs.h"
#include "exec/exec-all.h"
#include "qemu/xxhash.h"
#include "tb-jmp-cache.h"
#include "lats/aot-format.h"
#include "hamt-dmw.h"
#include "hamt-options.h"
#include "target/i386/latx/include/latx-options.h"

#ifdef CONFIG_SOFTMMU

/* Only the bottom TB_JMP_PAGE_BITS of the jump cache hash bits vary for
   addresses on the same page.  The top bits are the same.  This allows
   TLB invalidation to quickly clear a subset of the hash table.  */
#define TB_JMP_PAGE_BITS (TB_JMP_CACHE_BITS / 2)
#define TB_JMP_PAGE_SIZE (1 << TB_JMP_PAGE_BITS)
#define TB_JMP_ADDR_MASK (TB_JMP_PAGE_SIZE - 1)
#define TB_JMP_PAGE_MASK (TB_JMP_CACHE_SIZE - TB_JMP_PAGE_SIZE)

static inline unsigned int tb_jmp_cache_hash_page_legacy(vaddr pc)
{
    vaddr tmp;
    tmp = pc ^ (pc >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));
    return (tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) & TB_JMP_PAGE_MASK;
}

static inline unsigned int tb_jmp_cache_hash_legacy(vaddr pc)
{
    vaddr tmp;
    tmp = pc ^ (pc >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));
    return (((tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) & TB_JMP_PAGE_MASK)
           | (tmp & TB_JMP_ADDR_MASK));
}

#else

/* In user-mode we can get better hashing because we do not have a TLB */
static inline unsigned int tb_jmp_cache_hash_legacy(vaddr pc)
{
    return (pc ^ (pc >> TB_JMP_CACHE_BITS)) & (TB_JMP_CACHE_SIZE - 1);
}

#endif /* CONFIG_SOFTMMU */

static inline unsigned int tb_jmp_cache_hash_page(vaddr pc)
{
#ifdef CONFIG_SOFTMMU
    return tb_jmp_cache_hash_page_legacy(pc);
#else
    return tb_jmp_cache_hash_legacy(pc);
#endif
}

static inline unsigned int tb_jmp_cache_hash(vaddr pc)
{
    switch (latx_jmp_cache_hash_mode()) {
    case LATX_JMP_CACHE_HASH_LINE:
        return pc & (latx_jmp_cache_size() - 1);
    case LATX_JMP_CACHE_HASH_FULL:
        return (pc ^ (pc >> latx_jmp_cache_bits())) &
               (latx_jmp_cache_size() - 1);
    case LATX_JMP_CACHE_HASH_PAGE:
        break;
    default:
        g_assert_not_reached();
    }
    return tb_jmp_cache_hash_legacy(pc);
}

static inline CPUJumpCacheEntry *tb_jmp_cache_entry(CPUState *cpu, vaddr pc)
{
    return &cpu->tb_jmp_cache->array[tb_jmp_cache_hash(pc)];
}

static inline unsigned int latx_njc_hash_page(CPUState *cpu, vaddr pc)
{
    (void)cpu;
#ifdef CONFIG_SOFTMMU
    vaddr tmp = pc ^ (pc >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));

    return (tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) &
           (TB_JMP_CACHE_SIZE - TB_JMP_PAGE_SIZE);
#else
    return (pc ^ (pc >> TB_JMP_CACHE_BITS)) &
           (TB_JMP_CACHE_SIZE - 1);
#endif
}

static inline LatxNJCEntry *latx_njc_entry(CPUState *cpu,
                                           unsigned int hash)
{
    return &cpu->latx_njc_cache->array[hash];
}

static inline const void *latx_njc_tcptr(const TranslationBlock *tb)
{
    uintptr_t tcptr = (uintptr_t)tb->tc.ptr;

    if (hamt_enabled() && hamt_mode_dmw()) {
        tcptr = DMW_IN(tcptr);
    }
    return (const void *)tcptr;
}

static inline void latx_njc_insert(CPUState *cpu, unsigned int hash,
                                   vaddr pc, TranslationBlock *tb)
{
    LatxNJCEntry *entry;
    const void *tcptr;

    if (likely(cpu->latx_njc_cache == NULL) ||
        !(tb->flags & HF_CS64_MASK) ||
        (tb_cflags(tb) & CF_INVALID)) {
        return;
    }
    entry = latx_njc_entry(cpu, hash);
    qatomic_set(&entry->tcptr, NULL);
    qatomic_set(&entry->call_tcptr, NULL);
    entry->pc = pc;
    entry->flags = tb->flags;
    entry->observed_call_entry =
        (uint16_t)(qatomic_read(&tb->misc_flags) & AOT_BLOCK_OBSERVED_CALL_ENTRY);
    entry->direct_entry_off = 0;
    if (unlikely(tb_cflags(tb) & CF_INVALID)) {
        return;
    }
    tcptr = latx_njc_tcptr(tb);
    const void *call_tcptr = tcptr;
#ifdef CONFIG_LATX_TU
    if ((tb->bool_flags & (IS_AOT_TB | IS_TU_TB)) == (IS_AOT_TB | IS_TU_TB)) {
        const void *native_entry = qatomic_load_acquire(&tb->s_data->aot_native_call_entry);
        const void *direct_entry = qatomic_load_acquire(&tb->s_data->aot_direct_native_entry);
        if (native_entry) {
            uintptr_t call = (uintptr_t)native_entry;
            if (hamt_enabled() && hamt_mode_dmw()) call = DMW_IN(call);
            call_tcptr = (const void *)call;
            if (direct_entry) {
                entry->direct_entry_off = 32;
            }
        } else if (direct_entry) {
            return;
        }
    }
#endif
    qatomic_set(&entry->call_tcptr, call_tcptr);
    qatomic_set(&entry->tcptr, tcptr);
    if (unlikely(tb_cflags(tb) & CF_INVALID)) {
        qatomic_set(&entry->tcptr, NULL);
        qatomic_set(&entry->call_tcptr, NULL);
        entry->direct_entry_off = 0;
        return;
    }
}

static inline void latx_njc_invalidate(CPUState *cpu, unsigned int hash,
                                       vaddr pc, TranslationBlock *tb)
{
    LatxNJCEntry *entry;

    if (likely(cpu->latx_njc_cache == NULL) ||
        !(tb->flags & HF_CS64_MASK)) {
        return;
    }
    entry = latx_njc_entry(cpu, hash);
    if (entry->pc == pc) {
        if (qatomic_cmpxchg(&entry->tcptr, latx_njc_tcptr(tb), NULL) ==
            latx_njc_tcptr(tb)) {
            qatomic_set(&entry->call_tcptr, NULL);
            entry->direct_entry_off = 0;
        }
    }
}

static inline TranslationBlock *
tb_jmp_cache_load(CPUJumpCacheEntry *entry, bool pcrel)
{
#ifdef LATX_PCREL
    if (pcrel) {
        return qatomic_load_acquire(&entry->tb);
    }
#else
    (void)pcrel;
#endif
    return qatomic_rcu_read(&entry->tb);
}

static inline void tb_jmp_cache_insert(CPUState *cpu, vaddr pc,
                                       bool pcrel, TranslationBlock *tb)
{
    /* Generated lookups do not match these temporary execution flags. */
    if (tb_cflags(tb) & (CF_NO_GOTO_TB | CF_NO_GOTO_PTR)) {
        return;
    }
#ifdef CONFIG_LATX_TU
    if (tb_is_lookup_private(tb)) {
        return;
    }
#endif
    unsigned int hash = tb_jmp_cache_hash(pc);
    CPUJumpCacheEntry *entry = &cpu->tb_jmp_cache->array[hash];

    latx_njc_insert(cpu, hash, pc, tb);

#ifdef LATX_PCREL
    if (pcrel) {
        entry->pc = pc;
        qatomic_store_release(&entry->tb, tb);
    } else
#else
    (void)pcrel;
#endif
    {
        qatomic_set(&entry->tb, tb);
    }
#ifdef CONFIG_LATX_TU
    /* A publisher can retire this key after our first check. Pair with its
     * barrier before cache invalidation so neither side misses the store. */
    smp_mb();
    if (tb_is_lookup_private(tb)) {
        qatomic_cmpxchg(&entry->tb, tb, NULL);
        latx_njc_invalidate(cpu, hash, pc, tb);
    }
#endif
}

static inline void tb_jmp_cache_invalidate_current(CPUState *cpu, vaddr pc,
                                                    TranslationBlock *tb)
{
    unsigned int hash = tb_jmp_cache_hash(pc);
    CPUJumpCacheEntry *entry = &cpu->tb_jmp_cache->array[hash];

    qatomic_cmpxchg(&entry->tb, tb, NULL);
    latx_njc_invalidate(cpu, hash, pc, tb);
}

static inline void tb_jmp_cache_flush_bank(CPUJumpCache *jc, LatxNJC *njc)
{
    memset(jc->array, 0, latx_jmp_cache_size() * sizeof(jc->array[0]));
    if (njc) {
        memset(njc->array, 0, latx_jmp_cache_size() * sizeof(njc->array[0]));
    }
}

static inline void tb_jmp_cache_flush_current(CPUState *cpu)
{
    tb_jmp_cache_flush_bank(cpu->tb_jmp_cache, cpu->latx_njc_cache);
}

#ifdef CONFIG_SOFTMMU
static inline void tb_jmp_cache_flush_page_bank(CPUState *cpu, CPUJumpCache *jc,
                                                LatxNJC *njc, vaddr page_addr)
{
    unsigned int i0;

    assert(latx_jmp_cache_hash_mode() == LATX_JMP_CACHE_HASH_PAGE);
    i0 = tb_jmp_cache_hash_page(page_addr);
    memset(&jc->array[i0], 0,
           TB_JMP_PAGE_SIZE * sizeof(jc->array[0]));
    if (njc) {
        i0 = latx_njc_hash_page(cpu, page_addr);
        memset(&njc->array[i0], 0, TB_JMP_PAGE_SIZE * sizeof(njc->array[0]));
    }
}

static inline void tb_jmp_cache_flush_page_current(CPUState *cpu, vaddr page)
{
    tb_jmp_cache_flush_page_bank(cpu, cpu->tb_jmp_cache,
                                 cpu->latx_njc_cache, page);
}
#endif

static inline void tb_jmp_cache_invalidate(CPUState *cpu, vaddr pc,
                                           TranslationBlock *tb)
{
#ifdef CONFIG_SOFTMMU
    if (option_pcid_cache) {
        latx_pcid_jump_invalidate(cpu, pc, tb);
        return;
    }
#endif
    tb_jmp_cache_invalidate_current(cpu, pc, tb);
}

static inline void tb_jmp_cache_flush_all(CPUState *cpu)
{
#ifdef CONFIG_SOFTMMU
    if (option_pcid_cache) {
        latx_pcid_jump_flush(cpu);
        return;
    }
#endif
    tb_jmp_cache_flush_current(cpu);
}

#ifdef CONFIG_SOFTMMU
static inline void tb_jmp_cache_flush_page(CPUState *cpu, vaddr page)
{
    if (option_pcid_cache) {
        latx_pcid_jump_flush_page(cpu, page);
        return;
    }
    tb_jmp_cache_flush_page_current(cpu, page);
}
#endif

static inline
uint32_t tb_hash_func(tb_page_addr_t phys_pc, vaddr pc,
                      uint32_t flags, uint64_t flags2, uint32_t cf_mask)
{
    return phys_pc ^ (pc >> 32) ^ flags2 ^ flags ^ cf_mask;
}
TranslationBlock *tb_lookup(CPUState *cpu, vaddr pc,
                                          uint64_t cs_base, uint32_t flags,
                                          uint32_t cflags);
TranslationBlock *aot_tb_lookup(CPUState *cpu, vaddr pc,
                                          uint64_t cs_base, uint32_t flags,
                                          uint32_t cflags);


#endif
