/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "sys-aot-internal.h"
#include "sys-aot-format.h"
#include "latx-options.h"
#include "latx-ras.h"
#include "latx-misc.h"
#include "latx-native-recover.h"
#include "lats/aot-atomic.h"
#include "lats/aot-cpu-query.h"
#include "lats/aot-x87.h"
#include "lats/aot-system.h"
#include "qemu/bitops.h"
#include "qemu/xxhash.h"

/* cross_page */
/* instptn */
/* aes_opt */
/* opt_mmio */
/* tb_max_insns */
#define PAGE_PCREL_BIT 0
#define TB_PCREL_BIT 1
#define STATICCS_BIT 2
#define TB_LINK_BIT 3
#define INTB_LINK_BIT 4
#define INTB_NJC_BIT 5
#define SOFT_RAS_BIT 8
#define PAUSE_BIT 9
#define FAST_HTB_LOOKUP_BIT 11
#define FLAGED_SHT_BIT 12
#define FAST_ITLB_BIT 13
#define LAZYPC_BIT 14
#define LINK_OPT_BIT 15
#define LSFPU_BIT 16
#define FAST_FPR_LDST_BIT 17
#define FLAG_REDUCTION_BIT 18
#define RISK_MEM_OFFSET_BIT 19
#define RISK_SIMD_IGNORE_UPPER64_BIT 20
#define RISK_SIMD_IGNORE_UPPER64_STAT_TRACK_BIT 21
#define LAZY_MXCSR_BIT 22
#define RISK_IN_379_BIT 23
#define FAST_ATOMIC_BIT 24
#define SIGFPE_BIT 25
#define FAST_MASKED_V_BIT 26
#define SOFT_FPU_BIT 27
#define SOFT_FPU_FAST_BIT 28
#define NO_RESERVE_256_BIT 29
#define HAMT_BIT 30
#define COMMON_SLOWPATH_BIT 31
#define LARGE_CODE_CACHE_BIT 32
#define INT_OPT_BIT 33
#define INHIBIT_IRQ_BIT 34
#define REGION_PROLOGUE_BIT 35
#define SPLIT_TB_BIT 36
#define SPLIT_SEARCH_BIT 37
#define OPT_SYSCALL_BIT 38
#define OPT_SYSRET_BIT 39
#define OPT_IRET_BIT 40
#define OPT_RDTSC_RDTSCP_BIT 41
#define SMMU_SLOW_BIT 42
#define HAMT_MODE_BIT 43
#define HAMT_SUBMODE_BIT 44
#define HAMT_CONFIG_BIT 46 /* 45~46 */
#define PUSHPOP 47
#define FAST_ITLB_PTW_BIT 48
#define TU_MAX_TB_SHIFT 49
#define TU_MAX_TB_MASK UINT64_C(0x1ff)
#define AOT_GPR_HIGHBITS_BIT 58
#define INTB_NJC_INLINE_BIT 57
#define INTB_NJC_COMPACT_BIT 59
#define INTB_NJC_FULL_BIT 60
#define INTB_NJC_LINE_BIT 62
#define AOT_TU_CROSS_PAGE_BIT 61
#define AOT_TU_CROSS_PAGE_POLICY_BIT 63

#define AOT_TU_CODEGEN_FAST_MASK 0x3f
#define AOT_TU_CODEGEN_GEN_SHIFT 6
#define AOT_TU_CODEGEN_GEN_MASK 0x3
#define AOT_TU_CODEGEN_CP_SHIFT 8
#define AOT_TU_CODEGEN_CP_MASK 0xfff
#define AOT_TB_CODEGEN_POLICY (UINT32_C(1) << 20)
#define AOT_GHBR_LEVEL_SHIFT 21
#define AOT_GHBR_LEVEL_MASK UINT32_C(0x3)
#define AOT_RAS_TP_CODEGEN (UINT32_C(1) << 23)
#define AOT_IRQ_BACKEDGE_CODEGEN (UINT32_C(1) << 24)
#define AOT_NJC_RETURN_CODEGEN (UINT32_C(1) << 25)
#define AOT_NJC_INLINE_LAYOUT_CODEGEN (UINT32_C(1) << 26)
#define AOT_PCID_ASID_CODEGEN (UINT32_C(1) << 27)
#define AOT_PCID_CACHE_CODEGEN (UINT32_C(1) << 28)

/* Bits 6 and 7 were unused counter options, never emitted in the flags. */
#define SHBR_LEVEL_SHIFT 6
#define SHBR_LEVEL_MASK UINT64_C(0x3)

static int pre_file_tb_count;

typedef struct AOTLayoutTURefKey {
    uint64_t tu_id;
    int64_t anchor_delta;
} AOTLayoutTURefKey;

static guint aot_layout_tu_ref_hash(gconstpointer opaque)
{
    const AOTLayoutTURefKey *key = opaque;
    uint64_t value = key->tu_id ^ (uint64_t)key->anchor_delta;

    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    return value ^ (value >> 32);
}

static gboolean aot_layout_tu_ref_equal(gconstpointer ap, gconstpointer bp)
{
    const AOTLayoutTURefKey *a = ap;
    const AOTLayoutTURefKey *b = bp;

    return a->tu_id == b->tu_id && a->anchor_delta == b->anchor_delta;
}

static bool aot_layout_add_tu_ref(GHashTable *refs, uint64_t tu_id,
                                  int64_t anchor_delta)
{
    AOTLayoutTURefKey lookup = {
        .tu_id = tu_id,
        .anchor_delta = anchor_delta,
    };
    gpointer old_value = g_hash_table_lookup(refs, &lookup);
    guint count = GPOINTER_TO_UINT(old_value);

    if (count == G_MAXUINT) {
        return false;
    }
    g_hash_table_replace(refs, g_memdup2(&lookup, sizeof(lookup)),
                         GUINT_TO_POINTER(count + 1));
    return true;
}

static bool aot_layout_same_tu_refs(GHashTable *expected,
                                    GHashTable *actual)
{
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    if (g_hash_table_size(expected) != g_hash_table_size(actual)) {
        return false;
    }
    g_hash_table_iter_init(&iter, expected);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (g_hash_table_lookup(actual, key) != value) {
            return false;
        }
    }
    return true;
}

/*
 * validate_aot_layout() has already established every record boundary before
 * this pass. Check the cross-record invariant separately: each non-anchor TU
 * dependency has exactly one matching member-page reference.  Single-page
 * payload IDs are content identities and may legitimately repeat when equal
 * guest pages exist at different physical addresses.  Referenced cross-page
 * payload IDs, however, must remain unique in the file.
 */
static bool validate_aot_tu_references(const uint8_t *file,
                                       const page_info_array_t *info_array)
{
    GHashTable *cross_page_payloads = g_hash_table_new_full(g_int64_hash,
            g_int64_equal, g_free, NULL);
    GHashTable *expected = g_hash_table_new_full(aot_layout_tu_ref_hash,
            aot_layout_tu_ref_equal, g_free, NULL);
    GHashTable *actual = g_hash_table_new_full(aot_layout_tu_ref_hash,
            aot_layout_tu_ref_equal, g_free, NULL);
    bool ok = false;

    for (uint32_t i = 0; i < info_array->page_count; i++) {
        const page_head *page = (const page_head *)(file +
                info_array->pages[i].file_offset);
        const uint8_t *record = (const uint8_t *)page + sizeof(*page) +
                TARGET_PAGE_SIZE;

        for (uint32_t j = 0; j < page->tb_count; j++) {
            uint32_t magic = *(const uint32_t *)record;

            if (magic == AOT_TB_MAGIC) {
                const aot_tu_head *tu = (const aot_tu_head *)record;
                const aot_tu_page *pages = (const aot_tu_page *)(tu + 1);
                uint64_t lookup = tu->tu_id;

                if (tu->page_count > 1) {
                    if (g_hash_table_contains(cross_page_payloads, &lookup)) {
                        fprintf(stderr, "[AOT] Layout: duplicate cross-page "
                                "TU payload id=%" PRIx64 "\n", tu->tu_id);
                        goto out;
                    }
                    g_hash_table_insert(cross_page_payloads,
                            g_memdup2(&lookup, sizeof(lookup)),
                            GUINT_TO_POINTER(1));
                }
                for (uint32_t k = 0; k < tu->page_count; k++) {
                    if (pages[k].pc_delta &&
                            !aot_layout_add_tu_ref(expected, tu->tu_id,
                                                  -pages[k].pc_delta)) {
                        goto out;
                    }
                }
                record += tu->tu_sum_size;
            } else {
                const aot_tu_ref *ref = (const aot_tu_ref *)record;

                if (!aot_layout_add_tu_ref(actual, ref->tu_id,
                                           ref->anchor_delta)) {
                    goto out;
                }
                record += sizeof(*ref);
            }
        }
    }

    if (!aot_layout_same_tu_refs(expected, actual)) {
        fprintf(stderr, "[AOT] Layout: TU member references do not match "
                "canonical dependencies\n");
        goto out;
    }
    ok = true;

out:
    g_hash_table_destroy(actual);
    g_hash_table_destroy(expected);
    g_hash_table_destroy(cross_page_payloads);
    return ok;
}

static uint32_t get_aot_tu_codegen_flags(uint32_t aot_mode)
{
    /* The r2 convention also changes plain AOT TBs and helper boundaries. */
    uint32_t flags = option_rastoptp ? AOT_RAS_TP_CODEGEN : 0;

    /* Inline DMW helper returns restore either a constant or a vCPU ASID. */
    if (option_pcid_asid) {
        flags |= AOT_PCID_ASID_CODEGEN;
    }
    if (option_pcid_cache) {
        flags |= AOT_PCID_CACHE_CODEGEN;
    }
#ifdef CONFIG_LATX_TU
    if (aot_mode & AOT_USER) {
        flags |= (option_tucp_fast & AOT_TU_CODEGEN_FAST_MASK) |
            ((option_tucp_generation & AOT_TU_CODEGEN_GEN_MASK) <<
             AOT_TU_CODEGEN_GEN_SHIFT) |
            ((option_ghbr & AOT_GHBR_LEVEL_MASK) << AOT_GHBR_LEVEL_SHIFT);

        if (aot_tu_enabled()) {
            if (option_irq_backedge) {
                flags |= AOT_IRQ_BACKEDGE_CODEGEN;
            }
            if (option_njc_source_jump) {
                flags |= AOT_NJC_RETURN_CODEGEN;
                if (option_table_addr) {
                    flags |= AOT_NJC_INLINE_LAYOUT_CODEGEN;
                }
            }
            flags |= (option_cross_page & AOT_TU_CODEGEN_CP_MASK) <<
                     AOT_TU_CODEGEN_CP_SHIFT;
        } else {
            /* Legacy TB files did not identify fast/gen-dependent exits. */
            flags |= AOT_TB_CODEGEN_POLICY;
        }
        return flags;
    }
#endif
    return flags;
}

static uint64_t get_option_flags(uint32_t aot_mode)
{
    uint64_t flags = 0;
    if (option_page_pcrel) {
        flags |= 1 << PAGE_PCREL_BIT;
    }
    if (option_tb_pcrel) {
        flags |= 1 << TB_PCREL_BIT;
    }
    if (option_staticcs) {
        flags |= 1 << STATICCS_BIT;
    }
    if (option_tb_link) {
        flags |= 1 << TB_LINK_BIT;
    }
    if (option_intb_link) {
        flags |= 1 << INTB_LINK_BIT;
    }
    if (option_intb_njc) {
        flags |= 1 << INTB_NJC_BIT;
    }
    if (intb_njc_inline_enabled()) {
        flags |= UINT64_C(1) << INTB_NJC_INLINE_BIT;
        if (latx_jmp_cache_compact_enabled()) {
            flags |= UINT64_C(1) << INTB_NJC_COMPACT_BIT;
        }
    }
    /* Saved inline queries must use the same hash as runtime invalidation. */
    if (intb_njc_inline_enabled() ||
        (get_aot_tu_codegen_flags(aot_mode) & AOT_NJC_INLINE_LAYOUT_CODEGEN)) {
        if (option_jmp_cache_hash != LATX_JMP_CACHE_HASH_PAGE) {
            if (option_jmp_cache_hash == LATX_JMP_CACHE_HASH_FULL) {
                flags |= UINT64_C(1) << INTB_NJC_FULL_BIT;
            } else {
                flags |= UINT64_C(1) << INTB_NJC_LINE_BIT;
            }
        }
    }
    if (option_soft_ras) {
        flags |= 1 << SOFT_RAS_BIT;
    }
    if (option_pause) {
        flags |= 1 << PAUSE_BIT;
    }
#ifdef CONFIG_LATX_TU
    if (aot_tu_enabled() && (aot_mode & AOT_USER)) {
        flags |= UINT64_C(1) << AOT_TU_FLAG_BIT;
        /*
         * Legacy TU files did not record aottucp: neither on nor off may
         * accept them as having a known cross-page policy.
         */
        flags |= UINT64_C(1) << AOT_TU_CROSS_PAGE_POLICY_BIT;
        if (option_aot_cross_page_tu) {
            flags |= UINT64_C(1) << AOT_TU_CROSS_PAGE_BIT;
        }
        flags |= ((uint64_t)option_tu_max_tb & TU_MAX_TB_MASK)
                 << TU_MAX_TB_SHIFT;
    }
#endif
    flags |= ((uint64_t)option_shbr & SHBR_LEVEL_MASK) << SHBR_LEVEL_SHIFT;
    if (option_flaged_sht) {
        flags |= 1 << FLAGED_SHT_BIT;
    }
    if (option_fast_itlb) {
        flags |= UINT64_C(1) << FAST_ITLB_BIT;
    }
    if (option_fast_itlb_ptw) {
        flags |= UINT64_C(1) << FAST_ITLB_PTW_BIT;
    }
    if (option_lazypc) {
        flags |= 1 << LAZYPC_BIT;
    }
    if (option_link_opt) {
        flags |= 1 << LINK_OPT_BIT;
    }
    if (option_lsfpu) {
        flags |= 1 << LSFPU_BIT;
    }
    if (option_fast_fpr_ldst) {
        flags |= 1 << FAST_FPR_LDST_BIT;
    }
    if (option_sys_flag_reduction) {
        flags |= 1 << FLAG_REDUCTION_BIT;
    }
    if (option_risk_mem_offset) {
        flags |= 1 << RISK_MEM_OFFSET_BIT;
    }
    if (option_risk_simd_ignore_upper64) {
        flags |= 1 << RISK_SIMD_IGNORE_UPPER64_BIT;
    }
    if (option_risk_simd_ignore_upper64_stat_track) {
        flags |= 1 << RISK_SIMD_IGNORE_UPPER64_STAT_TRACK_BIT;
    }
    if (option_lazy_mxcsr) {
        flags |= 1 << LAZY_MXCSR_BIT;
    }
    if (option_risk_in_379) {
        flags |= 1 << RISK_IN_379_BIT;
    }
    if (option_fast_atomic) {
        flags |= 1 << FAST_ATOMIC_BIT;
    }
    if (option_sigfpe) {
        flags |= 1 << SIGFPE_BIT;
    }
    if (option_fast_masked_v) {
        flags |= 1 << FAST_MASKED_V_BIT;
    }
    if (option_soft_fpu) {
        flags |= 1 << SOFT_FPU_BIT;
    }
    if (option_soft_fpu_fast) {
        flags |= 1 << SOFT_FPU_FAST_BIT;
    }
    if (option_no_reserve_256) {
        flags |= 1 << NO_RESERVE_256_BIT;
    }
    if (option_common_slowpath) {
        flags |= (uint64_t)1 << COMMON_SLOWPATH_BIT;
    }
    if (option_large_code_cache) {
        flags |= (uint64)1 << LARGE_CODE_CACHE_BIT;
    }
    if (option_int_opt) {
        flags |= (uint64)1 << INT_OPT_BIT;
    }
    if (option_inhibit_irq) {
        flags |= (uint64)1 << INHIBIT_IRQ_BIT;
    }
    if (option_region_prologue) {
        flags |= (uint64)1 << REGION_PROLOGUE_BIT;
    }
    if (option_split_tb) {
        flags |= (uint64)1 << SPLIT_TB_BIT;
    }
    if (option_split_search) {
        flags |= (uint64)1 << SPLIT_SEARCH_BIT;
    }
    if (option_opt_syscall) {
        flags |= (uint64)1 << OPT_SYSCALL_BIT;
    }
    if (option_opt_sysret) {
        flags |= (uint64)1 << OPT_SYSRET_BIT;
    }
    if (option_opt_iret) {
        flags |= (uint64)1 << OPT_IRET_BIT;
    }
    if (option_opt_rdtsc_rdtscp) {
        flags |= (uint64)1 << OPT_RDTSC_RDTSCP_BIT;
    }
    if (option_smmu_slow) {
        flags |= (uint64)1 << SMMU_SLOW_BIT;
    }
    if (option_hamt) {
        flags |= (uint64)1 << HAMT_BIT;
        flags |= (uint64)(option_hamt_config & 0x3) << HAMT_CONFIG_BIT;
    }
    if (option_hamt_mode) {
        flags |= (uint64)1 << HAMT_MODE_BIT;
    }
    if (option_hamt_submode) {
        flags |= (uint64)1 << HAMT_SUBMODE_BIT;
    }
    if (option_pushpop) {
        flags |= (uint64)1 << PUSHPOP;
    }
    if (option_aot_gpr_highbits && (aot_mode & AOT_USER)) {
        flags |= UINT64_C(1) << AOT_GPR_HIGHBITS_BIT;
    }
    return flags;
}

aot_tail *get_aot_tail(int fd)
{
    aot_tail *tail = (aot_tail *)malloc(sizeof(aot_tail));
    if (!tail) {
        return NULL;
    }
    if (lseek(fd, -sizeof(aot_tail), SEEK_END) == (off_t)-1
            || read(fd, tail, sizeof(aot_tail)) != sizeof(aot_tail)) {
        fprintf(stderr, "get_aot_tail fd %d fail\n", fd);
        free(tail);
        return NULL;
    }
    return tail;
}

bool save_aot_tail(int page_num, int tb_num, uint32_t aot_mode)
{
    struct aot_tail tail = { 0 };
    tail.flags = get_option_flags(aot_mode);
    tail.sys_flag_reduction_tu = option_sys_flag_reduction_tu;
    tail.njc_bits = option_intb_njc &&
        option_jmp_cache_hash != LATX_JMP_CACHE_HASH_PAGE ?
        latx_jmp_cache_bits() : 0;
    tail.aot_tu_codegen_flags = get_aot_tu_codegen_flags(aot_mode);
    tail.interrupt_mode = option_int_opt;
    tail.page_num = page_num;
    tail.tb_num = tb_num;
    for (int i = LOAD_HELPER_BEGIN + 1; i < LOAD_HELPER_END; i++) {
        tail.helper_addr[i - LOAD_HELPER_BEGIN - 1] = relkind_to_fixup_addr[i];
    }
    snprintf(tail.aot_version, sizeof(tail.aot_version), "%s", AOT_VERSION);
    if (fwrite(&tail, sizeof(aot_tail), 1, out_tb_file) != 1) {
        fprintf(stderr, "[AOT] Failed to write file tail\n");
        return false;
    }
    if (option_verbose) {
        fprintf(stderr, "[AOT][File] Written: %d pages, %d TBs, "
                "flags=0x%lx, ver=%s\n",
                tail.page_num, tail.tb_num, tail.flags, tail.aot_version);
    }
    return true;
}

bool test_integrity(int fd, uint32_t aot_mode)
{
    aot_tail *tail = get_aot_tail(fd);
    if (!tail) {
        return false;
    }
    if (!memchr(tail->aot_version, '\0', sizeof(tail->aot_version)) ||
            strcmp(tail->aot_version, AOT_VERSION) != 0) {
        fprintf(stderr, "version mismatch in AOT file\n");
        free(tail);
        return false;
    }
    if (tail->interrupt_mode != option_int_opt) {
        fprintf(stderr, "interrupt mode differs in AOT file\n");
        free(tail);
        return false;
    }
    if (tail->flags != get_option_flags(aot_mode)) {
        fprintf(stderr, "flags not equal aot flags %lx curr flags %lx\n",
                tail->flags, get_option_flags(aot_mode));
        free(tail);
        return false;
    }
    uint32_t njc_bits = option_intb_njc &&
        option_jmp_cache_hash != LATX_JMP_CACHE_HASH_PAGE ?
        latx_jmp_cache_bits() : 0;
    if (tail->njc_bits != njc_bits) {
        fprintf(stderr, "njcbits not equal aot value %u curr value %u\n",
                tail->njc_bits, njc_bits);
        free(tail);
        return false;
    }
    if (tail->sys_flag_reduction_tu != option_sys_flag_reduction_tu) {
        fprintf(stderr, "sysflagtu not equal aot value %x curr value %x\n",
                tail->sys_flag_reduction_tu,
                option_sys_flag_reduction_tu);
        free(tail);
        return false;
    }
    if (tail->aot_tu_codegen_flags != get_aot_tu_codegen_flags(aot_mode)) {
        fprintf(stderr, "AOT TU codegen flags not equal aot value %x "
                "curr value %x\n", tail->aot_tu_codegen_flags,
                get_aot_tu_codegen_flags(aot_mode));
        free(tail);
        return false;
    }
    for (int i = LOAD_HELPER_BEGIN + 1; i < LOAD_HELPER_END; i++) {
        if (tail->helper_addr[i - LOAD_HELPER_BEGIN - 1] !=
                relkind_to_fixup_addr[i]) {
            fprintf(stderr, "helper adrr not equal %lx %lx\n",
                    tail->helper_addr[i - LOAD_HELPER_BEGIN - 1],
                    relkind_to_fixup_addr[i]);
            free(tail);
            return false;
        }
    }
    pre_file_tb_count = tail->tb_num;
    if (option_verbose) {
        fprintf(stderr, "[AOT][File] Integrity OK: %d pages, %d TBs, ver=%s\n",
            tail->page_num, tail->tb_num, tail->aot_version);
    }
    free(tail);
    return true;
}

static bool aot_ras_tp_offsets_valid(const aot_block *block,
                                    uint64_t code_size, uint64_t member_start,
                                    uint64_t member_size, uint32_t codegen)
{
    if (!(codegen & AOT_RAS_TP_CODEGEN) || !block->inst_ptr_offset) {
        return true;
    }
    int64_t gate = block->inst_ptr_offset;
    uint64_t cold = block->ras_tp_cold_offset;

    return gate >= 0 && !(gate & 3) && !(cold & 3) &&
           (uint64_t)gate >= member_start + 12 &&
           (uint64_t)gate + 16 <= member_start + member_size &&
           cold > (uint64_t)gate &&
           cold + LATX_RAS_TP_CALL_COLD_INSNS * 4 <= code_size &&
           (cold - gate) / 4 < (UINT64_C(1) << 25);
}

static bool aot_ras_tp_code_valid(const aot_block *block,
                                const uint8_t *code, uint32_t codegen)
{
    if (!(codegen & AOT_RAS_TP_CODEGEN) || !block->inst_ptr_offset) {
        return true;
    }
    /* Offset validation has already proved the first word is in its member. */
    uint32_t first, gate;
    memcpy(&first, code + block->inst_ptr_offset - 12, sizeof(first));
    memcpy(&gate, code + block->inst_ptr_offset, sizeof(gate));
    uint32_t delta = (block->ras_tp_cold_offset - block->inst_ptr_offset) / 4;
    uint32_t closed = 0x50000000U | ((delta & 0xffffU) << 10) | (delta >> 16);
    if ((block->misc_flags & MF_RAS_LINKED) || gate != closed) return false;
    if (block->misc_flags & MF_RAS_CLEAR_B0) {
        return first == LATX_RAS_TP_CALL_SKIP;
    }
    /* ANDI index, tp, 511; only the allocated index register varies. */
    return (first & ~31U) == (0x03400000U | (0x1ffU << 10) | (2U << 5));
}

/* Match the write widths in aot_do_tb_reloc(), before trusting file ranges. */
static bool aot_reloc_slots_valid(const aot_rel *rel)
{
    switch (rel->kind) {
    case AOT_NATIVE_ENTRY:
    case AOT_NATIVE_CORE:
        return rel->rel_slots_num > 0;
    case AOT_NATIVE_CORE_CALL:
        return rel->rel_slots_num == 4;
    case AOT_NATIVE_RET_MISS:
        return rel->rel_slots_num == 2;
    case AOT_NATIVE_CALL:
    case AOT_NATIVE_DIRECT_CALL:
    case AOT_NATIVE_LINKED_CALL:
    case AOT_NATIVE_INDIRECT_CALL:
        return rel->rel_slots_num == 1;
    case LOAD_TB_ADDR:
    case LOAD_TB_FULL_ADDR:
    case LOAD_HELPER_BEGIN ... LOAD_HELPER_END:
    case LOAD_HRAS_CALL_HELPER:
    case LOAD_HRAS_CALL_RET0_ADDR:
    case LOAD_SOFTWARE_INTERRUPT_HELPER:
        return rel->rel_slots_num == 3;
    case LOAD_HRAS_FSHT_BASE:
        return rel->rel_slots_num == 2;
    case LOAD_GUEST_ADDR:
    case LOAD_GUEST_ADDR32:
        return rel->rel_slots_num == 2 || rel->rel_slots_num == 3;
    case SC_TABLE_BEGIN ... SC_TABLE_END:
        return rel->rel_slots_num == 2;
    case LOAD_BY_GUEST_ADDR_CPU_NEG_ITLB:
    case INTB_FSHT_LOOKUP:
    case INTB_FSHT_LOOKUP_TUCPFAST:
    case INTB_FSHT_AFTER_NJC:
    case INTB_FSHT_CALL_LOOKUP_TUCPFAST:
    case INTB_LOOKUP_COMPACT:
    case INTB_NJC_RETURN:
    case INTB_RESUME_LOOKUP:
    case AOT_NATIVE_SYSCALL:
    case AOT_NATIVE_ATOMIC:
    case AOT_NATIVE_CPU_QUERY:
    case AOT_NATIVE_X87:
    case AOT_NATIVE_SYSTEM:
    case HRAS_TU_CP_CHECK_PTR:
    case SC_TABLE_OFFSET_BEGIN ... SC_TABLE_OFFSET_END:
        return rel->rel_slots_num == 1;
    default:
        /* Other kinds do not write code in aot_do_tb_reloc(). */
        return true;
    }
}

#ifdef CONFIG_LATX_TU
static bool aot_ranges_overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return a < b + bn && b < a + an;
}

/* A cold exception helper cannot return. Accept only the translator's
 * complete #DE stub, with a loader-patched helper address and one owner. */
static bool aot_exception_helper_valid(const aot_tu_head *tu,
        const aot_block *blocks, const aot_rel *rels, const uint8_t *code,
        uint64_t begin, uint64_t end, uint64_t call)
{
    if (call < begin + 40 || call + 12 > end) {
        return false;
    }
    uint64_t stub = call - 40;
    uint32_t w[13];
    memcpy(w, code + stub, sizeof(w));
    if ((w[0] & 0xfe00001fU) != 0x14000008U ||
        (w[1] & 0xffc003ffU) != 0x03800108U ||
        w[2] != (0x29c002e8U | (offsetof(CPUX86State, eip) << 10)) ||
        (w[3] & 0xfc000000U) != 0x54000000U ||
        w[4] != 0x02c00005U || w[5] != 0x001502e4U ||
        w[6] != 0x03000084U ||
        (w[7] & 0xfe00001fU) != 0x14000001U ||
        (w[8] & 0xffc003ffU) != 0x03800021U ||
        (w[9] & 0xfe00001fU) != 0x16000001U ||
        w[10] != 0x4c000021U || (w[11] & 0xfc000000U) != 0x54000000U ||
        w[12] != 0x50000000U) {
        return false;
    }
    unsigned gpc = 0, prologue = 0, helper = 0, epilogue = 0;
    int owner = -1;
    for (uint32_t m = 0; m < tu->tb_count; m++) {
        if (blocks[m].rel_start < 0) continue;
        for (int i = blocks[m].rel_start; i <= blocks[m].rel_end; i++) {
            const aot_rel *rel = &rels[i];
            if (rel->kind == AOT_NATIVE_CORE) continue;
            uint64_t at = blocks[m].tc.offset_in_tu + rel->tc_offset;
            uint64_t size = rel->rel_slots_num * UINT64_C(4);
            if (!aot_ranges_overlap(stub, sizeof(w), at, size)) continue;
            bool valid = false;
            if (at == stub && rel->kind == LOAD_GUEST_ADDR && size == 8 &&
                rel->extra_addend < blocks[m].size) {
                gpc++; valid = true;
            } else if (at == stub + 12 && rel->kind == SCS_PROLOGUE &&
                       size == 4 && rel->extra_addend == 1) {
                prologue++; valid = true;
            } else if (at == stub + 28 && rel->kind == HELPER_RAISE_EXCEPTION &&
                       size == 12 && rel->extra_addend == 0) {
                helper++; valid = true;
            } else if (at == stub + 44 && rel->kind == SCS_EPILOGUE &&
                       size == 4 && rel->extra_addend == 1) {
                epilogue++; valid = true;
            }
            if (!valid || (owner != -1 && owner != (int)m)) return false;
            owner = m;
        }
    }
    if (gpc != 1 || prologue != 1 || helper != 1 || epilogue != 1) {
        return false;
    }
    /* No direct branch may bypass the address load and enter the stub body.
     * The two SCS branches are relocated to runtime static code. */
    for (uint64_t at = begin; at < end; at += 4) {
        if (at == stub + 12 || at == stub + 44) continue;
        uint32_t word;
        memcpy(&word, code + at, 4);
        uint32_t op = word & 0xfc000000U;
        int64_t delta;
        if (op == 0x50000000U || op == 0x54000000U) {
            delta = sextract64(((word & 1023U) << 16) | ((word >> 10) & 65535U), 0, 26);
        } else if (op == 0x40000000U || op == 0x44000000U || op == 0x48000000U) {
            delta = sextract64(((word & 31U) << 16) | ((word >> 10) & 65535U), 0, 21);
        } else if (op >= 0x58000000U && op <= 0x6c000000U) {
            delta = sextract64((word >> 10) & 65535U, 0, 16);
        } else {
            continue;
        }
        int64_t target = (int64_t)at + delta * 4;
        if (target > (int64_t)stub && target <= (int64_t)call) return false;
    }
    return true;
}

/* The invalidation branch can replace a continuation's first word only when
 * every other machine-code entry is excluded. Only the local invalidation
 * stub and ordinary software-RAS RETs may transfer indirectly. */
static bool aot_direct_continuation_valid(const aot_tu_head *tu,
        const aot_block *blocks, const aot_rel *rels, const uint8_t *code,
        uint64_t begin, uint64_t end, uint64_t guard, uint64_t continuation)
{
    for (uint64_t at = begin; at < end; at += 4) {
        uint32_t word, prev = 0;
        memcpy(&word, code + at, 4);
        if (at >= begin + 4) memcpy(&prev, code + at - 4, 4);
        uint32_t op = word & 0xfc000000U;
        int64_t offset = 0;
        bool branch = true;
        if (op == 0x50000000U || op == 0x54000000U) {
            offset = sextract64(((word & 1023U) << 16) |
                                ((word >> 10) & 65535U), 0, 26);
        } else if (op == 0x40000000U || op == 0x44000000U ||
                   op == 0x48000000U) {
            offset = sextract64(((word & 31U) << 16) |
                                ((word >> 10) & 65535U), 0, 21);
        } else if (op >= 0x58000000U && op <= 0x6c000000U) {
            offset = sextract64((word >> 10) & 65535U, 0, 16);
        } else {
            branch = false;
        }
        if (branch) {
            int64_t target = (int64_t)at + offset * 4;
            if (target > (int64_t)guard && target <= (int64_t)continuation) {
                /* The SCS calls in a recognized exception stub are
                 * relocated to runtime static code. Their saved BL
                 * displacement does not name the loaded destination. */
                bool relocated_scs = op == 0x54000000U &&
                    (aot_exception_helper_valid(tu, blocks, rels, code,
                                                begin, end, at + 28) ||
                     (at >= begin + 4 &&
                      aot_exception_helper_valid(tu, blocks, rels, code,
                                                 begin, end, at - 4)));
                if (!relocated_scs) {
                    return false;
                }
            }
        }
        if (op != 0x4c000000U) continue;
        bool cold = false;
        for (uint32_t i = 0; i < tu->rel_count; i++) {
            if (rels[i].kind == AOT_NATIVE_RET_MISS) {
                for (uint32_t m = 0; m < tu->tb_count; m++) {
                    if (blocks[m].rel_start >= 0 && i >= blocks[m].rel_start &&
                        i <= blocks[m].rel_end) {
                        uint64_t slot = blocks[m].tc.offset_in_tu +
                                        rels[i].tc_offset;
                        if (slot >= begin && slot < end) {
                            cold |= at == slot + 4;
                        }
                    }
                }
                continue;
            }
            if (rels[i].kind != AOT_NATIVE_DIRECT_CALL &&
                rels[i].kind != AOT_NATIVE_LINKED_CALL &&
                rels[i].kind != AOT_NATIVE_INDIRECT_CALL) continue;
            uint64_t poison = (uint32_t)rels[i].next_entry_id;
            cold |= at == poison + 4;
            if (rels[i].kind == AOT_NATIVE_INDIRECT_CALL) {
                for (uint32_t m = 0; m < tu->tb_count; m++) {
                    /* The relation is owned by exactly one source block. */
                    if (blocks[m].rel_start >= 0 && i >= blocks[m].rel_start &&
                        i <= blocks[m].rel_end) {
                        cold |= at == blocks[m].tc.offset_in_tu + rels[i].tc_offset;
                    }
                }
            }
        }
        bool direct_ret = word == 0x4c000020U && prev == 0x5c000885U;
        if (!cold && !direct_ret &&
            ((word & 31U) != 0 || prev != 0x02fffc42U) &&
            !aot_exception_helper_valid(tu, blocks, rels, code, begin, end, at)) {
            return false;
        }
    }
    return true;
}

static bool aot_native_source_page_present(const aot_tu_head *tu, uint64_t pc)
{
    const aot_tu_page *pages = (const void *)(tu + 1);
    int64_t delta = (int64_t)((pc & TARGET_PAGE_MASK) -
                            (tu->first_pc & TARGET_PAGE_MASK));
    for (uint32_t i = 0; i < tu->page_count; i++) {
        if (pages[i].pc_delta == delta) {
            return true;
        }
    }
    return false;
}

/* Code pages may have gaps or precede the entry. Every recovery PC must
 * still refer to a page covered by the owner's invalidation dependencies.
 * The caller has already validated the complete packed recovery table. */
static bool aot_native_recovery_pages_valid(const aot_tu_head *tu,
                                          uint64_t entry, uint64_t extent,
                                          const NativeRecoverHeader *h)
{
    uint64_t last;
    if (!extent || __builtin_add_overflow(entry, extent - 1, &last) ||
        !aot_native_source_page_present(tu, entry) ||
        !aot_native_source_page_present(tu, last)) {
        return false;
    }
    const uint8_t *cursor = (const void *)(h + 1);
    for (uint16_t i = 0; i < h->points; i++) {
        const NativeRecoverPoint *point = (const void *)cursor;
        uint64_t pc;
        int64_t offset = point->guest_offset;
        if (offset < 0) {
            if (__builtin_sub_overflow(entry, (uint64_t)-offset, &pc)) {
                return false;
            }
        } else if (__builtin_add_overflow(entry, (uint64_t)offset, &pc)) {
            return false;
        }
        if (!aot_native_source_page_present(tu, pc)) {
            return false;
        }
        cursor += sizeof(*point) +
                  point->locations * sizeof(NativeRecoverLocation);
    }
    if (!h->state_bytes) {
        for (uint32_t k = 0; k < h->osr_count; k++) {
            int64_t offset = h->osr_guest_offsets[k];
            uint64_t pc;
            if (!offset) {
                continue;
            }
            if (offset < 0) {
                if (__builtin_sub_overflow(entry, (uint64_t)-offset, &pc)) {
                    return false;
                }
            } else if (__builtin_add_overflow(entry, (uint64_t)offset, &pc)) {
                return false;
            }
            if (!aot_native_source_page_present(tu, pc)) {
                return false;
            }
        }
    }
    return true;
}

static bool aot_native_complete_layout_valid_one(const aot_tu_head *tu,
                                                 const aot_block *blocks,
                                                 uint64_t original_end,
                                                 uint32_t selected_core,
                                                 uint32_t core_member,
                                                 const uint32_t *first_owners)
{
    const uint8_t *code = (const void *)(blocks + tu->tb_count);
    uint64_t bytes = blocks[0].tu_size;
    const aot_rel *rels = (const void *)(code + bytes);
    const aot_rel *core = NULL;
    bool direct_core = false;
    uint32_t frame_bytes = 0;
    uint64_t begin = 0, end = 0, data_begin = 0, data_end = 0;
    for (uint32_t i = 0; i < tu->rel_count; i++) {
        const aot_rel *rel = &rels[i];
        if (rel->kind != AOT_NATIVE_CORE || i != selected_core) {
            continue;
        }
        if (core || blocks[core_member].rel_start < 0 ||
            i < blocks[core_member].rel_start || i > blocks[core_member].rel_end ||
            !rel->rel_slots_num || rel->next_entry_id < 0 ||
            (rel->next_entry_id & 3)) {
            return false;
        }
        begin = blocks[core_member].tc.offset_in_tu + rel->tc_offset;
        end = begin + rel->rel_slots_num * UINT64_C(4);
        if ((begin & 7) || begin < original_end || end > bytes ||
            end - begin > UINT32_MAX) {
            return false;
        }
        bool recognized;
        const NativeRecoverHeader *h = native_recover_parse(code + begin,
                                                end - begin, &recognized);
        if (!recognized || !h || h->code_bytes < 4 ||
            (uint32_t)rel->next_entry_id > h->code_bytes - 4) {
            return false;
        }
        unsigned outer_calls = 0;
        for (uint32_t off = 0; off < rel->next_entry_id; off += 4) {
            uint32_t word;
            memcpy(&word, code + begin + off, sizeof(word));
            if ((word & 0xfc000000U) == 0x54000000U) {
                int64_t delta = ((word & 1023) << 16) |
                                ((word >> 10) & 65535);
                if (delta & (INT64_C(1) << 25)) {
                    delta -= INT64_C(1) << 26;
                }
                if ((int64_t)off + (int64_t)delta * 4 !=
                    rel->next_entry_id || ++outer_calls != 1) {
                    return false;
                }
            }
        }
        direct_core = h->state_bytes == 0;
        frame_bytes = h->frame_bytes;
        if (direct_core && rel->next_entry_id == 32) {
            uint32_t compat[8];
            size_t ras_off = offsetof(CPUX86State, ras_tp.stack);
            if (outer_calls != 0 || h->code_bytes < 36 || (ras_off & 3) ||
                ras_off < 16 || ras_off > 32764) {
                return false;
            }
            memcpy(compat, code + begin, sizeof(compat));
            if (compat[0] != 0x002ddc4bU ||
                compat[1] != (0x26000164U | (((ras_off - 16) >> 2) << 10)) ||
                compat[2] != (0x26000161U | (((ras_off - 8) >> 2) << 10)) ||
                compat[3] != 0x00128814U ||
                compat[4] != 0x0011d042U ||
                compat[5] != 0x03400000U ||
                compat[6] != 0x03400000U ||
                compat[7] != 0x03400000U) {
                return false;
            }
        } else if ((direct_core ? rel->next_entry_id != 0 : outer_calls != 1)) {
            return false;
        }
        if (!rel->extra_addend) {
            return false;
        }
        if (!aot_native_recovery_pages_valid(tu, blocks[core_member].pc,
                                             rel->extra_addend, h)) {
            return false;
        }
        if (!direct_core && h->osr_count) {
            return false;
        }
        if (direct_core && h->osr_count) {
            if (rel->next_entry_id != 32 || h->code_bytes < 36 || h->points) {
                return false;
            }
            for (uint32_t k = 0; k < h->osr_count; k++) {
                int32_t off = h->osr_guest_offsets[k];
                if (!off) {
                    continue;
                }
                uint64_t target_pc = blocks[core_member].pc + (int64_t)off;
                bool matched = false;
                for (uint32_t m = 0; m < tu->tb_count; m++) {
                    if (m == core_member || blocks[m].pc != target_pc ||
                        blocks[m].cs_base != blocks[core_member].cs_base ||
                        blocks[m].flags != blocks[core_member].flags ||
                        blocks[m].cflags != blocks[core_member].cflags ||
                        (blocks[m].misc_flags & AOT_BLOCK_OBSERVED_CALL_ENTRY)) {
                        continue;
                    }
                    matched = true;
                    break;
                }
                if (!matched) {
                    return false;
                }
            }
        }
        NativeRecoverFooter footer;
        memcpy(&footer, code + end - sizeof(footer), sizeof(footer));
        data_begin = begin + h->code_bytes;
        data_end = begin + footer.offset;
        core = rel;
    }
    for (uint32_t member = 0; member < tu->tb_count; member++) {
        const aot_block *block = &blocks[member];
        if (block->rel_start < 0) {
            continue;
        }
        unsigned calls = 0;
        for (int i = block->rel_start; i <= block->rel_end; i++) {
            const aot_rel *rel = &rels[i];
            if (rel->kind == AOT_NATIVE_SYSCALL ||
                rel->kind == AOT_NATIVE_ATOMIC ||
                rel->kind == AOT_NATIVE_CPU_QUERY ||
                rel->kind == AOT_NATIVE_X87 || rel->kind == AOT_NATIVE_SYSTEM) {
                uint64_t slot = block->tc.offset_in_tu + rel->tc_offset;
                bool atomic = rel->kind == AOT_NATIVE_ATOMIC;
                bool query = rel->kind == AOT_NATIVE_CPU_QUERY;
                bool x87 = rel->kind == AOT_NATIVE_X87;
                bool system = rel->kind == AOT_NATIVE_SYSTEM;
                uint64_t descriptor = rel->extra_addend;
                uint64_t length = system   ? LATS_SYSTEM_LENGTH(descriptor)
                                  : atomic ? LATS_ATOMIC_LENGTH(descriptor)
                                  : query  ? LATS_CPU_QUERY_LENGTH(descriptor)
                                  : x87    ? LATS_X87_LENGTH(descriptor)
                                           : descriptor;
                unsigned prefix_bytes = atomic || system ? 24 : 20;
                uint32_t words[7];
                if (core && (slot < begin || slot >= end)) continue;
                if (!core || !direct_core || rel->rel_slots_num != 1 ||
                    rel->next_entry_id != -1 || length < (x87 ? 1 : 2) ||
                    length > 15 || length > block->size || (slot & 3) ||
                    slot < begin + prefix_bytes || slot + 4 > data_begin ||
                    offsetof(CPUX86State, eip) > 2047)
                    return false;
                memcpy(words, code + slot - prefix_bytes, prefix_bytes + 4);
                if (words[0] != 0x14000005U || words[1] != 0x038000a5U ||
                    words[2] != 0x16000005U ||
                    words[3] != (0x29c002e5U | (offsetof(CPUX86State, eip) << 10))) return false;
                if (system) {
                    if (!lats_aot_system_valid(descriptor) ||
                        words[4] != (0x14000005U | ((descriptor >> 12) << 5)) ||
                        words[5] !=
                            (0x038000a5U | ((descriptor & 4095) << 10)) ||
                        words[6] != 0x50000000U)
                        return false;
                } else if (atomic) {
                    if (descriptor >> 16 ||
                        !LATS_ATOMIC_OPERATION(descriptor) ||
                        LATS_ATOMIC_OPERATION(descriptor) > LATS_ATOMIC_BIT ||
                        (LATS_ATOMIC_OPERATION(descriptor) == LATS_ATOMIC_BIT &&
                         (LATS_ATOMIC_BIT_KIND(descriptor) > 2 ||
                          LATS_ATOMIC_BYTES(descriptor) == 1)) ||
                        words[4] != (0x14000004U | ((descriptor >> 12) << 5)) ||
                        words[5] !=
                            (0x03800084U | ((descriptor & 4095) << 10)) ||
                        words[6] != 0x50000000U)
                        return false;
                } else {
                    unsigned kind = LATS_CPU_QUERY_KIND(descriptor);
                    if ((query && (descriptor >> 12 || kind < LATS_CPU_QUERY_CPUID ||
                                   kind > LATS_CPU_QUERY_PAUSE)) ||
                        (x87 && !lats_aot_x87_valid(descriptor)) ||
                        words[4] != ((x87 ? 0x03800005U : 0x03800004U) |
                                     ((query || x87 ? descriptor : length) << 10)) ||
                        words[5] != 0x50000000U) return false;
                }
                unsigned guest_pc = 0;
                for (uint32_t m = 0; m < tu->tb_count; m++) {
                    if (blocks[m].rel_start < 0) continue;
                    for (int j = blocks[m].rel_start; j <= blocks[m].rel_end; j++) {
                        if (j == i || rels[j].kind == AOT_NATIVE_CORE) continue;
                        uint64_t at = blocks[m].tc.offset_in_tu + rels[j].tc_offset;
                        if (!aot_ranges_overlap(slot - prefix_bytes, prefix_bytes + 4, at,
                                                rels[j].rel_slots_num * UINT64_C(4))) continue;
                        if (m != member || rels[j].kind != LOAD_GUEST_ADDR ||
                            at != slot - prefix_bytes || rels[j].rel_slots_num != 3 ||
                            rels[j].next_entry_id != -1 ||
                            rels[j].extra_addend != block->size - length) return false;
                        guest_pc++;
                    }
                }
                if (guest_pc != 1) return false;
                continue;
            }
            if (core && (rel->kind == AOT_NATIVE_RET_MISS ||
                         rel->kind == AOT_NATIVE_DIRECT_CALL ||
                         rel->kind == AOT_NATIVE_LINKED_CALL ||
                         rel->kind == AOT_NATIVE_INDIRECT_CALL)) {
                uint64_t slot = block->tc.offset_in_tu + rel->tc_offset;
                if (slot < begin || slot >= end) continue;
                /* Fresh continuation selection compares the host SP with
                 * the translator's outer SP. A suspended CALL must have a
                 * nonempty frame; leaves can retain zero-byte frames. */
                if (direct_core && !frame_bytes &&
                    (rel->kind == AOT_NATIVE_DIRECT_CALL ||
                     rel->kind == AOT_NATIVE_LINKED_CALL ||
                     rel->kind == AOT_NATIVE_INDIRECT_CALL)) {
                    return false;
                }
            }
            if (rel->kind == AOT_NATIVE_RET_MISS) {
                uint64_t slot = block->tc.offset_in_tu + rel->tc_offset;
                uint32_t words[2];
                if (!core || !direct_core || member != core_member ||
                    rel->rel_slots_num != 2 || rel->extra_addend ||
                    rel->next_entry_id != -1 || (slot & 3) ||
                    slot < begin || slot + sizeof(words) > data_begin) {
                    return false;
                }
                memcpy(words, code + slot, sizeof(words));
                if (words[0] != 0x1e000004U || words[1] != 0x4c000080U) {
                    return false;
                }
                for (uint32_t j = 0; j < tu->rel_count; j++) {
                    if (j == i || (rels[j].kind != AOT_NATIVE_RET_MISS &&
                                   rels[j].kind != AOT_NATIVE_DIRECT_CALL &&
                                   rels[j].kind != AOT_NATIVE_LINKED_CALL &&
                                   rels[j].kind != AOT_NATIVE_INDIRECT_CALL)) continue;
                    for (uint32_t m = 0; m < tu->tb_count; m++) {
                        if (blocks[m].rel_start >= 0 && j >= blocks[m].rel_start &&
                            j <= blocks[m].rel_end) {
                            uint64_t other = blocks[m].tc.offset_in_tu + rels[j].tc_offset;
                            if (other >= begin && other < end) return false;
                        }
                    }
                }
                for (uint32_t m = 0; m < tu->tb_count; m++) {
                    if (blocks[m].rel_start < 0) continue;
                    for (int j = blocks[m].rel_start; j <= blocks[m].rel_end; j++) {
                        if (j == i || rels[j].kind == AOT_NATIVE_CORE) continue;
                        uint64_t at = blocks[m].tc.offset_in_tu + rels[j].tc_offset;
                        if (aot_ranges_overlap(slot, sizeof(words), at,
                                               rels[j].rel_slots_num * UINT64_C(4))) {
                            return false;
                        }
                    }
                }
                continue;
            }
            if (rel->kind == AOT_NATIVE_ENTRY && core) {
                return false;
            }
            if (rel->kind == AOT_NATIVE_INDIRECT_CALL) {
                uint64_t slot = block->tc.offset_in_tu + rel->tc_offset;
                uint64_t poison = (uint32_t)rel->next_entry_id;
                uint64_t length = rel->extra_addend;
                uint32_t w[20], stub[2];
                size_t ras = offsetof(CPUX86State, ras_tp.stack);
                size_t cache = offsetof(CPUX86State, latx_njc_cache_ptr);
                size_t hflags = offsetof(CPUX86State, hflags);
                if (!core || !direct_core || rel->rel_slots_num != 1 ||
                    rel->next_entry_id < 0 || !length || length > 15 ||
                    length > block->size || (slot & 3) || (poison & 3) ||
                    slot < begin + core->next_entry_id + 76 || slot + 8 > data_begin ||
                    poison < begin || poison + 8 > data_begin ||
                    cache > 2047 || hflags > 2047 || (ras & 3) || ras + 8 > 32764) {
                    return false;
                }
                memcpy(w, code + slot - 76, sizeof(w));
                memcpy(stub, code + poison, sizeof(stub));
                unsigned target = (w[0] >> 5) & 31;
                if ((w[0] & ~(31U << 5)) != 0x00150005U ||
                    target == 0 || (target >= 2 && target <= 4) ||
                    (core->next_entry_id != 0 && target == 1) || target == 23 ||
                    w[1] != 0x004518a9U ||
                    w[2] != (0x28c002e7U | (cache << 10)) ||
                    w[3] != 0x0015a4a9U ||
                    w[4] != (0x2a8002eaU | (hflags << 10)) ||
                    w[5] != 0x008b1929U || w[6] != 0x00d11928U ||
                    w[7] != 0x0300094aU || w[8] != 0x00411508U ||
                    w[9] != 0x00109d07U || w[10] != 0x28c060e6U ||
                    w[11] != 0x28c020e9U || w[12] != 0x28c040e8U ||
                    w[13] != 0x00af8008U || w[14] != 0x00159529U ||
                    w[15] != 0x0015a908U || w[16] != 0x00152129U ||
                    w[17] != 0x0013a4c9U ||
                    (w[18] & 0xfc0003e0U) != 0x40000120U ||
                    w[19] != 0x4c0020c1U ||
                    stub[0] != 0x1e000004U || stub[1] != 0x4c000080U) {
                    return false;
                }
                {
                    int64_t delta = sextract64(
                        ((w[18] & 31U) << 16) | ((w[18] >> 10) & 65535U), 0, 21);
                    if ((int64_t)(slot - 4) + delta * 4 != poison) {
                        return false;
                    }
                }
                for (uint32_t m = 0; m < tu->tb_count; m++) {
                    if (blocks[m].rel_start < 0) continue;
                    for (int j = blocks[m].rel_start; j <= blocks[m].rel_end; j++) {
                        const aot_rel *other = &rels[j];
                        if (j == i || other->kind == AOT_NATIVE_CORE) continue;
                        uint64_t at = blocks[m].tc.offset_in_tu + other->tc_offset;
                        uint64_t bytes = other->rel_slots_num * UINT64_C(4);
                        if (other->kind == AOT_NATIVE_INDIRECT_CALL ||
                            other->kind == AOT_NATIVE_DIRECT_CALL ||
                            other->kind == AOT_NATIVE_LINKED_CALL) {
                            uint64_t other_poison = (uint32_t)other->next_entry_id;
                            if (aot_ranges_overlap(slot - 76, 84, other_poison, 8) ||
                                (other_poison != poison &&
                                 aot_ranges_overlap(poison, 8, other_poison, 8))) return false;
                        }
                        if (other->kind == AOT_NATIVE_INDIRECT_CALL) {
                            if (at < 76) return false;
                            at -= 76;
                            bytes = 84;
                        } else if (other->kind == AOT_NATIVE_DIRECT_CALL ||
                                   other->kind == AOT_NATIVE_LINKED_CALL) {
                            uint64_t prefix =
                                other->kind == AOT_NATIVE_LINKED_CALL ? 0 : 44;
                            if (at < prefix) return false;
                            at -= prefix;
                            bytes = prefix + 8;
                            uint64_t cold = (uint32_t)other->extra_addend;
                            if (aot_ranges_overlap(slot - 76, 84, cold, 32) ||
                                aot_ranges_overlap(poison, 8, cold, 32)) return false;
                        }
                        if (aot_ranges_overlap(slot - 76, 84, at, bytes) ||
                            aot_ranges_overlap(poison, 8, at, bytes)) return false;
                    }
                }
                if (aot_ranges_overlap(slot - 76, 84, poison, 8) ||
                    !aot_direct_continuation_valid(tu, blocks, rels, code,
                                                   begin, data_begin, slot - 76, slot + 4)) {
                    return false;
                }
                continue;
            }
            if (rel->kind == AOT_NATIVE_DIRECT_CALL ||
                rel->kind == AOT_NATIVE_LINKED_CALL) {
                bool linked = rel->kind == AOT_NATIVE_LINKED_CALL;
                uint64_t slot = block->tc.offset_in_tu + rel->tc_offset;
                uint64_t cold = (uint32_t)rel->extra_addend;
                uint64_t poison = (uint32_t)rel->next_entry_id;
                uint32_t insn, cont_word, templates[2], cold_words[7], guard[11];
                uint64_t my_prefix = linked ? 0 : sizeof(guard);
                uint64_t my_span = my_prefix + 8;
                uint64_t min_slot = begin +
                    (linked ? (uint32_t)(core ? core->next_entry_id : 0) :
                              sizeof(guard));
                if (!core || !direct_core || rel->rel_slots_num != 1 ||
                    rel->next_entry_id < 0 || block->size < 5 ||
                    block->jmp_insn_offset[0] == TB_JMP_OFFSET_INVALID ||
                    (slot & 3) || (cold & 3) || (poison & 3) ||
                    slot < min_slot || slot + 8 > data_begin ||
                    cold < begin || cold + 32 > data_begin ||
                    poison < begin || poison + 8 > data_begin) {
                    return false;
                }
                memcpy(&insn, code + slot, 4);
                memcpy(&cont_word, code + slot + 4, 4);
                int64_t displacement = ((insn & 1023) << 16) |
                                       ((insn >> 10) & 65535);
                if (displacement & (INT64_C(1) << 25)) {
                    displacement -= INT64_C(1) << 26;
                }
                memcpy(templates, code + poison, sizeof(templates));
                memcpy(cold_words, code + cold, sizeof(cold_words));
                if ((insn & 0xfc000000U) != 0x54000000U ||
                    (int64_t)slot + displacement * 4 != cold ||
                    (linked && cont_word != 0x03240063U) ||
                    templates[0] != 0x1e000004U ||
                    templates[1] != 0x4c000080U ||
                    cold_words[0] != 0x14000005U ||
                    cold_words[1] != 0x038000a5U ||
                    cold_words[2] != 0x16000005U ||
                    offsetof(CPUX86State, eip) > 2047 ||
                    cold_words[3] != (0x29c002e5U |
                                      (offsetof(CPUX86State, eip) << 10)) ||
                    cold_words[4] != 0x1400000aU ||
                    cold_words[5] != 0x0380014aU ||
                    cold_words[6] != 0x1600000aU) {
                    return false;
                }
                if (!linked) {
                    memcpy(guard, code + slot - sizeof(guard), sizeof(guard));
                    int64_t guard_delta = sextract64(((guard[4] & 31U) << 16) |
                                                     ((guard[4] >> 10) & 65535U),
                                                     0, 21);
                    size_t array_off = offsetof(CPUX86State, tu_generation_array);
                    size_t ras_off = offsetof(CPUX86State, ras_tp.stack);
                    if (array_off > 2047 || (ras_off & 3) ||
                        ras_off + 8 > 32764 ||
                        guard[0] != 0x14000005U || guard[1] != 0x038000a5U ||
                        guard[2] != (0x28c002e6U | (array_off << 10)) ||
                        guard[3] != 0x382014c7U ||
                        (guard[4] & 0xfc0003e0U) != 0x400000e0U ||
                        (int64_t)(slot - 28) + guard_delta * 4 != cold ||
                        guard[5] != (0x03400045U | ((LATX_RAS_TP_SIZE - 1) << 10)) ||
                        guard[6] != 0x002ddca6U ||
                        guard[7] != (0x270000c4U | ((ras_off >> 2) << 10)) ||
                        guard[8] != 0x18000087U ||
                        guard[9] != (0x270000c7U | (((ras_off + 8) >> 2) << 10)) ||
                        guard[10] != 0x02c004a2U) {
                        return false;
                    }
                }
                unsigned gpc = 0, tb_addr = 0, exit = 0;
                int32_t guest_disp = rel->extra_addend >> 32;
                for (int j = block->rel_start; j <= block->rel_end; j++) {
                    const aot_rel *other = &rels[j];
                    uint64_t at = block->tc.offset_in_tu + other->tc_offset;
                    gpc += other->kind == LOAD_GUEST_ADDR && at == cold &&
                           other->rel_slots_num == 3 &&
                           other->extra_addend == (uint64_t)((int64_t)block->size + guest_disp);
                    tb_addr += other->kind == LOAD_TB_FULL_ADDR && at == cold + 16 &&
                               other->rel_slots_num == 3;
                    exit += other->kind == CS_NATIVE_TO_BT_RET_N0 && at == cold + 28 &&
                            other->rel_slots_num == 1 && other->extra_addend == 0;
                }
                for (uint32_t j = 0; j < tu->rel_count; j++) {
                    const aot_rel *other = &rels[j];
                    if (other->kind != AOT_NATIVE_DIRECT_CALL &&
                        other->kind != AOT_NATIVE_LINKED_CALL) continue;
                    uint64_t other_prefix =
                        other->kind == AOT_NATIVE_LINKED_CALL ? 0 : 44;
                    uint64_t other_span = other_prefix + 8;
                    uint64_t other_slot = 0;
                    uint32_t owner = first_owners ? first_owners[j] : UINT32_MAX;
                    if (!first_owners) {
                        for (uint32_t m = 0; m < tu->tb_count; m++) {
                            if (blocks[m].rel_start >= 0 &&
                                j >= blocks[m].rel_start && j <= blocks[m].rel_end) {
                                owner = m;
                                break;
                            }
                        }
                    }
                    if (owner != UINT32_MAX) {
                        other_slot = blocks[owner].tc.offset_in_tu + other->tc_offset;
                    }
                    uint64_t other_cold = (uint32_t)other->extra_addend;
                    uint64_t other_poison = (uint32_t)other->next_entry_id;
                    if (!other_slot || other_slot < other_prefix ||
                        aot_ranges_overlap(cold, 32, other_slot - other_prefix, other_span) ||
                        aot_ranges_overlap(poison, 8, other_slot - other_prefix, other_span) ||
                        aot_ranges_overlap(cold, 32, other_poison, 8) ||
                        aot_ranges_overlap(poison, 8, other_cold, 32) ||
                        (j != i && aot_ranges_overlap(cold, 32, other_cold, 32)) ||
                        (j != i && aot_ranges_overlap(slot - my_prefix, my_span,
                                                      other_slot - other_prefix, other_span)) ||
                        (poison != other_poison &&
                         aot_ranges_overlap(poison, 8, other_poison, 8))) {
                        return false;
                    }
                }
                for (uint32_t m = 0; m < tu->tb_count; m++) {
                    if (blocks[m].rel_start < 0) continue;
                    for (int j = blocks[m].rel_start; j <= blocks[m].rel_end; j++) {
                        const aot_rel *other = &rels[j];
                        if (other->kind == AOT_NATIVE_CORE ||
                            other->kind == AOT_NATIVE_DIRECT_CALL ||
                            other->kind == AOT_NATIVE_LINKED_CALL) continue;
                        uint64_t at = blocks[m].tc.offset_in_tu + other->tc_offset;
                        uint64_t length = other->rel_slots_num * UINT64_C(4);
                        bool expected_cold = m == member &&
                            ((other->kind == LOAD_GUEST_ADDR && at == cold && length == 12) ||
                             (other->kind == LOAD_TB_FULL_ADDR && at == cold + 16 && length == 12) ||
                             (other->kind == CS_NATIVE_TO_BT_RET_N0 && at == cold + 28 && length == 4));
                        if ((aot_ranges_overlap(cold, 32, at, length) && !expected_cold) ||
                            aot_ranges_overlap(poison, 8, at, length) ||
                            aot_ranges_overlap(slot - my_prefix, my_span, at, length)) {
                            return false;
                        }
                    }
                }
                if (gpc != 1 || tb_addr != 1 || exit != 1 ||
                    !aot_direct_continuation_valid(tu, blocks, rels, code,
                                                   begin, data_begin,
                                                   slot - my_prefix, slot + 4)) {
                    return false;
                }
                continue;
            }
            if (rel->kind != AOT_NATIVE_CORE_CALL) {
                continue;
            }
            uint64_t slot = block->tc.offset_in_tu + rel->tc_offset;
            uint64_t original = (uint32_t)rel->extra_addend;
            uint64_t member_end = block->tc.offset_in_tu +
                                  block->ir2_num * UINT64_C(4);
            uint64_t initial[2];
            if (!core || direct_core || ++calls != 1 || rel->rel_slots_num != 4 ||
                rel->next_entry_id != -1 || (slot & 7) ||
                slot < data_begin || slot > data_end ||
                data_end - slot < sizeof(initial) ||
                block->size < 5 ||
                original < block->tc.offset_in_tu ||
                original + 32 > member_end ||
                original + 28 != block->inst_ptr_offset) {
                return false;
            }
            memcpy(initial, code + slot, sizeof(initial));
            if (initial[0] || initial[1]) {
                return false;
            }
            uint32_t prefix[4];
            memcpy(prefix, code + original, sizeof(prefix));
            if ((prefix[0] & 0xfe00001fU) != 0x14000008U ||
                (prefix[1] & 0xffc003ffU) != 0x03800108U ||
                prefix[2] != 0x29ffe388U || prefix[3] != 0x02ffe39cU) {
                return false;
            }
            for (uint32_t j = 0; j < tu->rel_count; j++) {
                if (j == i) {
                    continue;
                }
                const aot_rel *other = &rels[j];
                if (other->kind == AOT_NATIVE_CALL &&
                    j >= block->rel_start && j <= block->rel_end) {
                    return false;
                }
                if (other->kind != AOT_NATIVE_CORE_CALL) {
                    continue;
                }
                uint64_t other_slot = 0;
                bool found = false;
                for (uint32_t k = 0; k < tu->tb_count; k++) {
                    if (blocks[k].rel_start >= 0 &&
                        j >= blocks[k].rel_start && j <= blocks[k].rel_end) {
                        other_slot = blocks[k].tc.offset_in_tu +
                                     other->tc_offset;
                        found = true;
                        break;
                    }
                }
                if (!found || (slot < other_slot + 16 &&
                               other_slot < slot + 16)) {
                    return false;
                }
            }
        }
    }
    if (core) {
        for (uint32_t i = 0; i < tu->rel_count; i++) {
            const aot_rel *rel = &rels[i];
            uint64_t other_begin, other_end;
            if (rel->kind == AOT_NATIVE_ENTRY) {
                other_begin = blocks[0].tc.offset_in_tu + rel->tc_offset;
                other_end = other_begin + rel->rel_slots_num * UINT64_C(4);
            } else if (rel->kind == AOT_NATIVE_CALL) {
                other_begin = (uint32_t)rel->extra_addend;
                other_end = other_begin + AOT_NATIVE_CALL_STUB_SIZE;
            } else {
                continue;
            }
            if (begin < other_end && other_begin < end) {
                return false;
            }
        }
    }
    return true;
}

/* Assign each direct-register record to exactly one non-overlapping CORE.
 * The single-CORE and legacy State-wrapper checks remain in the original
 * validation path below. */
static bool aot_native_complete_layout_valid(const aot_tu_head *tu,
                                             const aot_block *blocks,
                                             uint64_t original_end)
{
    const uint8_t *code = (const void *)(blocks + tu->tb_count);
    uint64_t bytes = blocks[0].tu_size;
    const aot_rel *rels = (const void *)(code + bytes);
    g_autofree uint32_t *first_owners = NULL;
    bool direct_calls = false;
    uint32_t cores = 0;
    for (uint32_t i = 0; i < tu->rel_count; i++) {
        if (rels[i].kind == AOT_NATIVE_CORE) cores++;
        direct_calls |= rels[i].kind == AOT_NATIVE_DIRECT_CALL ||
                        rels[i].kind == AOT_NATIVE_LINKED_CALL;
    }
    if (!cores) {
        for (uint32_t i = 0; i < tu->rel_count; i++) {
            if (rels[i].kind == AOT_NATIVE_SYSCALL ||
                rels[i].kind == AOT_NATIVE_ATOMIC ||
                rels[i].kind == AOT_NATIVE_CPU_QUERY ||
                rels[i].kind == AOT_NATIVE_X87 ||
                rels[i].kind == AOT_NATIVE_SYSTEM)
                return false;
        }
        return aot_native_complete_layout_valid_one(tu, blocks, original_end,
                                                    UINT32_MAX, 0, NULL);
    }
    /*
     * Calls repeatedly query the first containing member. Keep the original
     * scan order without replacing the ownership and overlap checks.
     * Allocation failure retains the original scan.
     */
    if (direct_calls) {
        first_owners = g_try_new(uint32_t, tu->rel_count);
    }
    if (first_owners) {
        for (uint32_t i = 0; i < tu->rel_count; i++) {
            first_owners[i] = UINT32_MAX;
        }
        for (uint32_t m = 0; m < tu->tb_count; m++) {
            if (blocks[m].rel_start < 0) {
                continue;
            }
            for (uint32_t i = blocks[m].rel_start;
                 i <= (uint32_t)blocks[m].rel_end; i++) {
                if (first_owners[i] == UINT32_MAX) {
                    first_owners[i] = m;
                }
            }
        }
    }
    for (uint32_t i = 0; i < tu->rel_count; i++) {
        if (rels[i].kind != AOT_NATIVE_CORE) continue;
        uint32_t owner = UINT32_MAX, owners = 0;
        for (uint32_t m = 0; m < tu->tb_count; m++) {
            if (blocks[m].rel_start >= 0 && i >= blocks[m].rel_start &&
                i <= blocks[m].rel_end) { owner = m; owners++; }
        }
        if (owners != 1 || (cores == 1 && owner != 0)) return false;
        uint64_t begin, end;
        if (__builtin_add_overflow((uint64_t)blocks[owner].tc.offset_in_tu,
                                   (uint64_t)rels[i].tc_offset, &begin) ||
            __builtin_add_overflow(begin, rels[i].rel_slots_num * UINT64_C(4), &end) ||
            (begin & 7) || !rels[i].rel_slots_num || begin < original_end ||
            end > bytes || end - begin > UINT32_MAX) {
            return false;
        }
        if (cores > 1) {
            bool recognized;
            const NativeRecoverHeader *h = native_recover_parse(code + begin,
                                                        end - begin, &recognized);
            if (!recognized || !h || h->state_bytes ||
                (rels[i].next_entry_id != 0 && rels[i].next_entry_id != 32)) {
                return false;
            }
        }
        for (uint32_t j = 0; j < i; j++) {
            if (rels[j].kind != AOT_NATIVE_CORE) continue;
            if (blocks[owner].rel_start >= 0 &&
                j >= blocks[owner].rel_start && j <= blocks[owner].rel_end) {
                return false;
            }
            for (uint32_t m = 0; m < tu->tb_count; m++) {
                if (blocks[m].rel_start < 0 || j < blocks[m].rel_start ||
                    j > blocks[m].rel_end) continue;
                uint64_t other = blocks[m].tc.offset_in_tu + rels[j].tc_offset;
                if (aot_ranges_overlap(begin, end - begin, other,
                                       rels[j].rel_slots_num * UINT64_C(4))) return false;
            }
        }
        if (!aot_native_complete_layout_valid_one(tu, blocks, original_end, i, owner,
                                                  first_owners)) return false;
    }
    {
        for (uint32_t i = 0; i < tu->rel_count; i++) {
            const aot_rel *rel = &rels[i];
            if (cores > 1 && (rel->kind == AOT_NATIVE_ENTRY ||
                rel->kind == AOT_NATIVE_CALL ||
                rel->kind == AOT_NATIVE_CORE_CALL)) return false;
            if (rel->kind != AOT_NATIVE_DIRECT_CALL &&
                rel->kind != AOT_NATIVE_LINKED_CALL &&
                rel->kind != AOT_NATIVE_INDIRECT_CALL &&
                rel->kind != AOT_NATIVE_SYSCALL &&
                rel->kind != AOT_NATIVE_ATOMIC &&
                rel->kind != AOT_NATIVE_CPU_QUERY &&
                rel->kind != AOT_NATIVE_X87 && rel->kind != AOT_NATIVE_SYSTEM &&
                rel->kind != AOT_NATIVE_RET_MISS)
                continue;
            uint32_t owner = UINT32_MAX, owners = 0, matches = 0;
            for (uint32_t m = 0; m < tu->tb_count; m++) {
                if (blocks[m].rel_start >= 0 && i >= blocks[m].rel_start &&
                    i <= blocks[m].rel_end) { owner = m; owners++; }
            }
            if (owners != 1) return false;
            uint64_t slot = blocks[owner].tc.offset_in_tu + rel->tc_offset;
            for (uint32_t j = 0; j < tu->rel_count; j++) {
                if (rels[j].kind != AOT_NATIVE_CORE) continue;
                for (uint32_t m = 0; m < tu->tb_count; m++) {
                    if (blocks[m].rel_start < 0 || j < blocks[m].rel_start ||
                        j > blocks[m].rel_end) continue;
                    uint64_t begin = blocks[m].tc.offset_in_tu + rels[j].tc_offset;
                    uint64_t end = begin + rels[j].rel_slots_num * UINT64_C(4);
                    matches += slot >= begin && slot < end;
                }
            }
            if (matches != 1) return false;
        }
    }
    return true;
}

static bool aot_native_layout_valid(const aot_tu_head *tu,
                                    const aot_block *blocks)
{
    const uint8_t *code = (const uint8_t *)(blocks + tu->tb_count);
    uint64_t code_size = blocks[0].tu_size;
    const aot_rel *rels = (const void *)(code + code_size);
    uint64_t original_end = 0;
    for (uint32_t j = 0; j < tu->rel_count; j++) {
        if (rels[j].kind != AOT_NATIVE_ENTRY &&
            rels[j].kind != AOT_NATIVE_CALL &&
            rels[j].kind != AOT_NATIVE_CORE &&
            rels[j].kind != AOT_NATIVE_CORE_CALL &&
            rels[j].kind != AOT_NATIVE_DIRECT_CALL &&
            rels[j].kind != AOT_NATIVE_LINKED_CALL &&
            rels[j].kind != AOT_NATIVE_INDIRECT_CALL) {
            continue;
        }
        unsigned owners = 0;
        for (uint32_t i = 0; i < tu->tb_count; i++) {
            owners += blocks[i].rel_start >= 0 &&
                      j >= blocks[i].rel_start && j <= blocks[i].rel_end;
        }
        if (owners != 1) {
            return false;
        }
    }
    for (uint32_t i = 0; i < tu->tb_count; i++) {
        original_end = MAX(original_end, blocks[i].tc.offset_in_tu +
                           blocks[i].ir2_num * UINT64_C(4));
        if (blocks[i].inst_ptr_offset) {
            original_end = MAX(original_end,
                (uint64_t)blocks[i].ras_tp_cold_offset +
                LATX_RAS_TP_CALL_COLD_INSNS * 4);
        }
    }
    for (uint32_t i = 0; i < tu->tb_count; i++) {
        const aot_block *block = &blocks[i];
        unsigned entries = 0, calls = 0;
        if (block->rel_start < 0) {
            continue;
        }
        for (int j = block->rel_start; j <= block->rel_end; j++) {
            const aot_rel *rel = &rels[j];
            uint64_t start = block->tc.offset_in_tu + rel->tc_offset;
            if (rel->kind == AOT_NATIVE_ENTRY) {
                /* Initial native page lifetime is the canonical entry page.
                 * A larger function needs all-page invalidation first. */
                if (++entries != 1 || i != 0 || (start & 3) ||
                    start < original_end || !rel->rel_slots_num ||
                    start + rel->rel_slots_num * UINT64_C(4) > code_size ||
                    !rel->extra_addend ||
                    rel->extra_addend > TARGET_PAGE_SIZE -
                        (block->pc & ~TARGET_PAGE_MASK)) {
                    return false;
                }
            } else if (rel->kind == AOT_NATIVE_CALL) {
                uint32_t stub = rel->extra_addend;
                uint32_t continuation = rel->extra_addend >> 32;
                uint32_t original;
                bool has_return = false;
                if (++calls != 1 || rel->rel_slots_num != 1 ||
                    !block->inst_ptr_offset ||
                    start + 16 != block->inst_ptr_offset ||
                    (start & 3) || start + 4 > original_end ||
                    (stub & 3) || stub < original_end ||
                    (uint64_t)stub + AOT_NATIVE_CALL_STUB_SIZE > code_size) {
                    return false;
                }
                memcpy(&original, code + start, 4);
                if (original != 0x02ffe39c) { /* addi.d guest RSP, RSP, -8 */
                    return false;
                }
                for (uint32_t k = 0; k < tu->tb_count; k++) {
                    has_return |= blocks[k].tc.offset_in_tu == continuation &&
                                  blocks[k].pc == block->pc + block->size;
                }
                if (!has_return) {
                    return false;
                }
            }
        }
    }
    /* Linked stubs are rewritten in place; none may alias another stub or
     * the shared native body. Entry ownership was checked above. */
    for (uint32_t i = 0; i < tu->rel_count; i++) {
        const aot_rel *rel = &rels[i];
        if (rel->kind != AOT_NATIVE_ENTRY && rel->kind != AOT_NATIVE_CALL) {
            continue;
        }
        uint64_t start = rel->kind == AOT_NATIVE_ENTRY ?
            blocks[0].tc.offset_in_tu + rel->tc_offset :
            (uint32_t)rel->extra_addend;
        uint64_t end = start + (rel->kind == AOT_NATIVE_ENTRY ?
            rel->rel_slots_num * UINT64_C(4) : AOT_NATIVE_CALL_STUB_SIZE);
        for (uint32_t j = 0; j < i; j++) {
            const aot_rel *other = &rels[j];
            if (other->kind != AOT_NATIVE_ENTRY &&
                other->kind != AOT_NATIVE_CALL) {
                continue;
            }
            uint64_t other_start = other->kind == AOT_NATIVE_ENTRY ?
                blocks[0].tc.offset_in_tu + other->tc_offset :
                (uint32_t)other->extra_addend;
            uint64_t other_end = other_start +
                (other->kind == AOT_NATIVE_ENTRY ?
                 other->rel_slots_num * UINT64_C(4) : AOT_NATIVE_CALL_STUB_SIZE);
            if (start < other_end && other_start < end) {
                return false;
            }
        }
    }
    return aot_native_complete_layout_valid(tu, blocks, original_end);
}
#endif

static bool aot_ras_tp_control_overlap(const aot_block *block,
                                     uint64_t start, uint64_t end,
                                     uint64_t member_start)
{
    uint64_t gate = block->inst_ptr_offset - member_start;
    uint64_t first = gate - 12;
    return (start < first + 4 && end > first) ||
           (start < gate + 4 && end > gate);
}

/* These appended kinds deliberately do not extend the fixed helper footer.
 * Their addresses are rebuilt from this runtime, after validating the load. */
static bool aot_hras_call_loads_valid(const aot_block *block,
        const aot_rel *rels, uint32_t count, const uint8_t *code,
        uint64_t code_size, uint64_t member_start)
{
    for (uint32_t i = 0; i < count; i++) {
        const aot_rel *rel = &rels[i];
        uint32_t words[3];
        uint64_t start = member_start + (uint64_t)rel->tc_offset;
        if (rel->kind != LOAD_HRAS_CALL_HELPER &&
            rel->kind != LOAD_HRAS_CALL_RET0_ADDR &&
            rel->kind != LOAD_HRAS_FSHT_BASE &&
            rel->kind != HRAS_TU_CP_CHECK_PTR) continue;
        if (rel->kind == LOAD_HRAS_FSHT_BASE) {
            if (rel->rel_slots_num != 2 || (start & 3) || start > code_size ||
                code_size - start < 8 ||
                block->last_ir1_type != IR1_TYPE_CALLIN) return false;
            memcpy(words, code + start, 8);
            unsigned rd = words[0] & 31;
            if (!rd || ((words[0] & ~31U) != 0x00150000U &&
                        (words[0] & 0xfe000000U) != 0x14000000U) ||
                (words[1] & 0xfe00001fU) != (0x16000000U | rd)) return false;
            continue;
        }
        if (rel->kind == HRAS_TU_CP_CHECK_PTR) {
            uint32_t branch;
            if (rel->rel_slots_num != 1 || rel->extra_addend != 1 ||
                (start & 3) || start > code_size || code_size - start < 4 ||
                block->last_ir1_type != IR1_TYPE_CALLIN) return false;
            memcpy(&branch, code + start, 4);
            if ((branch & 0xfc000000U) != 0x54000000U) return false;
            continue;
        }
        if (rel->rel_slots_num != 3 || (start & 3) || start > code_size ||
            code_size - start < sizeof(words) ||
            block->last_ir1_type != IR1_TYPE_CALLIN) return false;
        memcpy(words, code + start, sizeof(words));
        unsigned rd = words[0] & 31;
        if (!rd || (words[0] & 0xfe000000U) != 0x14000000U ||
            (words[1] & 0xffc00000U) != 0x03800000U ||
            (words[1] & 0x3ff) != (rd | (rd << 5)) ||
            (words[2] & 0xfe000000U) != 0x16000000U ||
            (words[2] & 31) != rd) return false;
    }
    return true;
}

static bool aot_ras_tp_generation_patch_valid(const aot_block *block,
                                            uint16_t patch,
                                            uint64_t member_start,
                                            uint32_t codegen)
{
    if (!(codegen & AOT_RAS_TP_CODEGEN) || !block->inst_ptr_offset) return true;
    uint64_t end = (uint64_t)patch + 8;
    uint64_t cold = block->ras_tp_cold_offset - member_start;
    return !aot_ras_tp_control_overlap(block, patch, end, member_start) &&
           !(patch < cold + LATX_RAS_TP_CALL_COLD_INSNS * 4 && end > cold);
}

static bool aot_ras_tp_relocs_valid(const aot_block *block,
                                  const aot_rel *rels, uint32_t rel_count,
                                  uint64_t member_start, uint32_t codegen)
{
    if (!(codegen & AOT_RAS_TP_CODEGEN)) return true;
    if (block->ras_tp_cold_offset < member_start && block->inst_ptr_offset) {
        return false;
    }
    /* The codec makes each member's relocation offsets relative to its TB,
     * whereas the persisted cold position remains relative to the TU. */
    uint64_t cold = block->ras_tp_cold_offset - member_start;
    unsigned source_count = 0, repair_count = 0;
    for (uint32_t i = 0; i < rel_count; ++i) {
        const aot_rel *rel = &rels[i];
        if (!aot_reloc_slots_valid(rel)) return false;
        if (rel->kind == RAS_TP_REPAIR &&
            (!block->inst_ptr_offset || rel->tc_offset != cold + 12 ||
             rel->rel_slots_num != 1 || rel->extra_addend != 1)) {
            return false;
        }
        if (!block->inst_ptr_offset) continue;
        uint64_t rel_end = (uint64_t)rel->tc_offset + rel->rel_slots_num * 4ULL;
        if (aot_ras_tp_control_overlap(block, rel->tc_offset, rel_end,
                                      member_start)) return false;
        if (rel_end <= cold || rel->tc_offset >= cold + 20) continue;
        if (rel->kind == LOAD_TB_FULL_ADDR && rel->tc_offset == cold &&
            rel->rel_slots_num == 3) {
            source_count++;
        } else if (rel->kind == RAS_TP_REPAIR && rel->tc_offset == cold + 12 &&
                   rel->rel_slots_num == 1 && rel->extra_addend == 1) {
            repair_count++;
        } else {
            return false;
        }
    }
    return !block->inst_ptr_offset || (source_count == 1 && repair_count == 1);
}

bool validate_aot_layout(const void *mem_ptr, uint64_t file_size)
{
    size_t page_info_size;
    const uint8_t *file = mem_ptr;
    const aot_tail *tail;
    const page_info_array_t *info_array;
    uint64_t page_data_size;
    uint64_t expected_offset = 0;
    const uint32_t min_page_size = sizeof(page_head) + TARGET_PAGE_SIZE;
    bool tu_format;

    if (file_size < sizeof(aot_tail)) {
        return false;
    }

    tail = (const aot_tail *)(file + file_size - sizeof(aot_tail));
    if (tail->page_num < 0 || tail->tb_num < 0 ||
            !aot_page_info_size(tail->page_num, &page_info_size) ||
            page_info_size > file_size - sizeof(aot_tail)) {
        return false;
    }

    info_array = (const page_info_array_t *)((const uint8_t *)tail -
            page_info_size);
    if (info_array->page_count != (uint32_t)tail->page_num) {
        return false;
    }

    tu_format = (tail->flags &
            (UINT64_C(1) << AOT_TU_FLAG_BIT)) != 0;
    page_data_size = (const uint8_t *)info_array - file;
    for (uint32_t i = 0; i < info_array->page_count; i++) {
        uint64_t offset = info_array->pages[i].file_offset;
        uint64_t record_offset;
        uint64_t page_end;
        uint32_t page_record_count = 0;
        const page_head *page;

        if (offset != expected_offset || offset > page_data_size ||
                page_data_size - offset < min_page_size) {
            return false;
        }
        page = (const page_head *)(file + offset);
        if ((page->page_magic != AOT_PAGE_MAGIC &&
                page->page_magic != AOT_MERGED_MAGIC) ||
                page->page_hash != info_array->pages[i].page_hash ||
                page->curr_page_sum_size < min_page_size ||
                (uint64_t)page->curr_page_sum_size >
                    page_data_size - offset) {
            return false;
        }

        page_end = offset + page->curr_page_sum_size;
        record_offset = offset + min_page_size;
        while (page_record_count < page->tb_count) {
            if (record_offset > page_end) {
                return false;
            }

            if (tu_format) {
                const aot_tu_head *tu;
                const aot_tu_page *tu_pages;
                const aot_block *blocks;
                size_t page_bytes;
                size_t block_bytes;
                size_t rel_bytes;
                uint64_t minimum_size;
                uint64_t expected_size;
                bool has_anchor_page = false;

                if (page_end - record_offset >= sizeof(aot_tu_ref)) {
                    const aot_tu_ref *ref =
                        (const aot_tu_ref *)(file + record_offset);

                    if (ref->ref_magic == AOT_TU_REF_MAGIC) {
                        if (ref->ref_size != sizeof(*ref) || !ref->tu_id ||
                                ref->anchor_delta == INT64_MIN ||
                                (ref->anchor_delta &
                                 (TARGET_PAGE_SIZE - 1)) != 0 ||
                                ref->reserved[0] || ref->reserved[1]) {
                            fprintf(stderr, "[AOT] Layout: invalid TU ref "
                                    "page=%u record=%u\n", i,
                                    page_record_count);
                            return false;
                        }
                        record_offset += sizeof(*ref);
                        page_record_count++;
                        continue;
                    }
                }

                if (page_end - record_offset < sizeof(*tu)) {
                    fprintf(stderr, "[AOT] Layout: truncated TU header page=%u\n", i);
                    return false;
                }
                tu = (const aot_tu_head *)(file + record_offset);
                if (tu->tb_magic != AOT_TB_MAGIC || tu->tb_count == 0 ||
                        tu->page_count == 0 ||
                        tu->page_count > AOT_TU_MAX_PAGES ||
                        !tu->tu_id ||
                        tu->reserved != 0 ||
                        __builtin_mul_overflow((size_t)tu->page_count,
                            sizeof(*tu_pages), &page_bytes) ||
                        __builtin_mul_overflow((size_t)tu->tb_count,
                            sizeof(*blocks), &block_bytes) ||
                        __builtin_mul_overflow((size_t)tu->rel_count,
                            sizeof(aot_rel), &rel_bytes) ||
                        __builtin_add_overflow(sizeof(*tu), page_bytes,
                            &minimum_size) ||
                        __builtin_add_overflow(minimum_size, block_bytes,
                            &minimum_size) ||
                        minimum_size > tu->tu_sum_size ||
                        tu->tu_sum_size > page_end - record_offset) {
                    fprintf(stderr, "[AOT] Layout: invalid TU header page=%u "
                            "seen=%u page_tbs=%u tu_tbs=%u sum=%u remain=%llu\n",
                            i, page_record_count, page->tb_count, tu->tb_count,
                            tu->tu_sum_size,
                            (unsigned long long)(page_end - record_offset));
                    return false;
                }

                tu_pages = (const aot_tu_page *)(tu + 1);
                blocks = (const aot_block *)((const uint8_t *)tu_pages +
                        page_bytes);
                for (uint32_t j = 0; j < tu->page_count; j++) {
                    if (tu_pages[j].pc_delta == INT64_MIN ||
                            (tu_pages[j].pc_delta & ~TARGET_PAGE_MASK) != 0 ||
                            (j && tu_pages[j - 1].pc_delta >=
                                  tu_pages[j].pc_delta)) {
                        fprintf(stderr, "[AOT] Layout: invalid TU pages "
                                "page=%u ref=%u delta=%lld\n", i, j,
                                (long long)tu_pages[j].pc_delta);
                        return false;
                    }
                    has_anchor_page |= tu_pages[j].pc_delta == 0;
                }
                if (!has_anchor_page) {
                    fprintf(stderr, "[AOT] Layout: TU page index missing "
                            "page=%u anchor=%d\n", i, has_anchor_page);
                    return false;
                }
                if (blocks[0].pc != tu->first_pc) {
                    fprintf(stderr, "[AOT] Layout: TU entry mismatch page=%u "
                            "first=%" PRIx64 " block=%" PRIx64 "\n", i,
                            (uint64_t)tu->first_pc, (uint64_t)blocks[0].pc);
                    return false;
                }
                if (blocks[0].tu_size < 0 ||
                        __builtin_add_overflow(minimum_size,
                            (uint64_t)blocks[0].tu_size, &minimum_size) ||
                        __builtin_add_overflow(minimum_size,
                            (uint64_t)rel_bytes, &minimum_size) ||
                        minimum_size > tu->tu_sum_size) {
                    fprintf(stderr, "[AOT] Layout: invalid TU payload page=%u "
                            "code=%d rels=%u minimum=%llu sum=%u\n",
                            i, blocks[0].tu_size, tu->rel_count,
                            (unsigned long long)minimum_size, tu->tu_sum_size);
                    return false;
                }
                expected_size = minimum_size;
                for (uint32_t j = 0; j < tu->tb_count; j++) {
                    uint64_t ir2_end;
                    uint64_t block_ir2_size;
                    uint64_t search_size = tail->flags &
                            (UINT64_C(1) << SPLIT_SEARCH_BIT) ?
                        (uint64_t)blocks[j].icount * 4 :
                        blocks[j].search_size;
                    bool invalid_generation_patch = false;
                    bool has_block_page = false;
                    bool has_block_page1 = blocks[j].page_addr[1] ==
                        (tb_page_addr_t)-1;
                    int64_t block_page_delta = (int64_t)(
                        (blocks[j].pc & TARGET_PAGE_MASK) -
                        (tu->first_pc & TARGET_PAGE_MASK));

                    for (uint32_t k = 0; k < tu->page_count; k++) {
                        has_block_page |=
                            tu_pages[k].pc_delta == block_page_delta;
                        if (!has_block_page1) {
                            has_block_page1 = tu_pages[k].pc_delta ==
                                block_page_delta + TARGET_PAGE_SIZE;
                        }
                    }
                    if (!has_block_page || !has_block_page1) {
                        fprintf(stderr, "[AOT] Layout: TU block dependency "
                                "missing page=%u block=%u delta=%lld "
                                "page1=%d\n", i, j,
                                (long long)block_page_delta,
                                has_block_page1);
                        return false;
                    }

                    if (__builtin_mul_overflow(
                            (uint64_t)blocks[j].ir2_num, UINT64_C(4),
                            &block_ir2_size)) {
                        return false;
                    }
                    for (int k = 0; k < 2; k++) {
                        uint16_t patch =
                            blocks[j].tu_generation_offset_patch[k];
                        uint64_t patch_end;

                        if (patch != TB_JMP_OFFSET_INVALID &&
                                ((patch & 3) ||
                                 __builtin_add_overflow((uint64_t)patch,
                                     UINT64_C(8), &patch_end) ||
                                 patch_end > block_ir2_size ||
                                 !aot_ras_tp_generation_patch_valid(&blocks[j],
                                     patch, blocks[j].tc.offset_in_tu,
                                     tail->aot_tu_codegen_flags))) {
                            invalid_generation_patch = true;
                        }
                    }

                    if (invalid_generation_patch ||
                            !aot_ras_tp_offsets_valid(&blocks[j],
                                blocks[0].tu_size, blocks[j].tc.offset_in_tu,
                                block_ir2_size, tail->aot_tu_codegen_flags) ||
                            __builtin_add_overflow(expected_size, search_size,
                                &expected_size) ||
                            ((tail->flags & (UINT64_C(1) << PUSHPOP)) &&
                             __builtin_add_overflow(expected_size,
                                 (uint64_t)blocks[j].icount * 4,
                                 &expected_size)) ||
                            __builtin_add_overflow(
                                (uint64_t)blocks[j].tc.offset_in_tu,
                                block_ir2_size, &ir2_end) ||
                            ir2_end > (uint64_t)blocks[0].tu_size ||
                            (blocks[j].rel_start < 0 ?
                                (blocks[j].rel_start != -1 ||
                                 blocks[j].rel_end != -2) :
                                (blocks[j].rel_start < 0 ||
                                 blocks[j].rel_end < blocks[j].rel_start ||
                                 (uint32_t)blocks[j].rel_end >= tu->rel_count))) {
                        fprintf(stderr, "[AOT] Layout: invalid TU block page=%u "
                                "block=%u offset=%zu ir2=%u code=%d rel=%d..%d/%u\n",
                                i, j, blocks[j].tc.offset_in_tu,
                                blocks[j].ir2_num, blocks[0].tu_size,
                                blocks[j].rel_start, blocks[j].rel_end,
                                tu->rel_count);
                        return false;
                    }
                    if (!aot_hras_call_loads_valid(&blocks[j],
                            (const aot_rel *)((const uint8_t *)blocks +
                                block_bytes + blocks[0].tu_size) +
                                (blocks[j].rel_start >= 0 ? blocks[j].rel_start : 0),
                            blocks[j].rel_start >= 0 ?
                                blocks[j].rel_end - blocks[j].rel_start + 1 : 0,
                            (const uint8_t *)blocks + block_bytes,
                            blocks[0].tu_size, blocks[j].tc.offset_in_tu)) {
                        return false;
                    }
                    if (!aot_ras_tp_code_valid(&blocks[j],
                            (const uint8_t *)blocks + block_bytes,
                            tail->aot_tu_codegen_flags) ||
                        !aot_ras_tp_relocs_valid(&blocks[j],
                            (const aot_rel *)((const uint8_t *)blocks +
                                block_bytes + blocks[0].tu_size) +
                                (blocks[j].rel_start >= 0 ? blocks[j].rel_start : 0),
                            blocks[j].rel_start >= 0 ?
                                blocks[j].rel_end - blocks[j].rel_start + 1 : 0,
                            blocks[j].tc.offset_in_tu,
                            tail->aot_tu_codegen_flags)) {
                        fprintf(stderr, "[AOT] Layout: invalid RAS code or relocations "
                                "page=%u block=%u\n", i, j);
                        return false;
                    }
                }
                if (!aot_native_layout_valid(tu, blocks)) {
                    fprintf(stderr, "[AOT] Layout: invalid native leaf metadata\n");
                    return false;
                }
                if (expected_size != tu->tu_sum_size) {
                    fprintf(stderr, "[AOT] Layout: TU payload size mismatch "
                            "page=%u expected=%llu sum=%u\n", i,
                            (unsigned long long)expected_size,
                            tu->tu_sum_size);
                    return false;
                }

                record_offset += tu->tu_sum_size;
                page_record_count++;
            } else {
                const aot_block *block;
                uint32_t magic, sum_size, all_tb_size, rel_count;
                size_t rel_bytes;
                uint64_t expected_size;
                uint64_t ir2_size;

                if (page_end - record_offset < 4 * sizeof(uint32_t)) {
                    return false;
                }
                memcpy(&magic, file + record_offset, sizeof(magic));
                memcpy(&sum_size, file + record_offset + 4,
                        sizeof(sum_size));
                memcpy(&all_tb_size, file + record_offset + 8,
                        sizeof(all_tb_size));
                memcpy(&rel_count, file + record_offset + 12,
                        sizeof(rel_count));
                if (magic != AOT_TB_MAGIC ||
                        all_tb_size < sizeof(*block) ||
                        __builtin_mul_overflow((size_t)rel_count,
                            sizeof(aot_rel), &rel_bytes) ||
                        __builtin_add_overflow(
                            (uint64_t)(4 * sizeof(uint32_t) + all_tb_size),
                            (uint64_t)rel_bytes, &expected_size) ||
                        expected_size != sum_size ||
                        sum_size > page_end - record_offset) {
                    return false;
                }
                block = (const aot_block *)(file + record_offset + 16);
                if (__builtin_mul_overflow((uint64_t)block->ir2_num,
                        UINT64_C(4), &ir2_size) ||
                        ir2_size > all_tb_size - sizeof(*block) ||
                        !aot_ras_tp_offsets_valid(block, ir2_size, 0,
                            ir2_size, tail->aot_tu_codegen_flags)) {
                    return false;
                }
                if (!aot_hras_call_loads_valid(block,
                        (const aot_rel *)((const uint8_t *)block + all_tb_size),
                        rel_count, (const uint8_t *)block + sizeof(*block),
                        ir2_size, 0)) {
                    return false;
                }
                if (!aot_ras_tp_code_valid(block,
                        (const uint8_t *)block + sizeof(*block),
                        tail->aot_tu_codegen_flags) ||
                    !aot_ras_tp_relocs_valid(block,
                        (const aot_rel *)((const uint8_t *)block + all_tb_size),
                        rel_count, 0, tail->aot_tu_codegen_flags)) {
                    return false;
                }

                record_offset += sum_size;
                page_record_count++;
            }
        }
        if (record_offset != page_end) {
            fprintf(stderr, "[AOT] Layout: page records do not fill page=%u "
                    "offset=%llu end=%llu\n", i,
                    (unsigned long long)record_offset,
                    (unsigned long long)page_end);
            return false;
        }
        expected_offset = page_end;
    }

    return expected_offset == page_data_size &&
        (!tu_format || validate_aot_tu_references(file, info_array));
}

static uint32_t hash_5_to_32(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
        uint32_t e)
{
    uint32_t seed = 0x9E3779B9;
    uint32_t h = seed;
    h = (h << 5) | (h >> 27);
    h ^= a + 0x5bd1e995;
    h *= 0x5bd1e995;

    h = (h << 7) | (h >> 25);
    h ^= b;
    h *= 0xcc9e2d51;

    h = (h << 11) | (h >> 21);
    h ^= c;
    h += d;

    h ^= e;

    return h;
}

static uint32_t hash_func(uint32_t pc, uint32_t code1, uint32_t code2,
        uint32_t code3, uint32_t code4)
{
    uint32_t prime1 = 2654435761U;
    uint32_t prime2 = 2246822519U;
    uint32_t prime3 = 3266489917U;
    uint32_t prime4 =  668265263U;
    uint32_t prime5 =  374761393U;
    uint32_t hash;
    hash = (rol32(pc * prime1, 1) + rol32(code1 * prime2, 12) +
            rol32(code2 * prime3, 16));
    hash += code3 * prime4;
    hash = rol32(hash, 17) * prime5;
    hash += code4 * prime4;
    hash = rol32(hash, 17) * prime5;

    hash ^= hash >> 16;
    hash *= prime2;
    hash ^= hash >> 10;
    return hash;
}

uint32_t get_page_hash(uint32_t *guest_code)
{
    uint32_t hash = hash_5_to_32(guest_code[2], guest_code[307],
            guest_code[1009], guest_code[731], guest_code[881]);
    return hash_func(hash, guest_code[683], guest_code[543],
            guest_code[1023], guest_code[0]);
}

uint64_t get_page_fingerprint(const void *guest_code)
{
    const uint64_t *words = guest_code;
    uint64_t v1 = QEMU_XXHASH_SEED + XXH_PRIME64_1 + XXH_PRIME64_2;
    uint64_t v2 = QEMU_XXHASH_SEED + XXH_PRIME64_2;
    uint64_t v3 = QEMU_XXHASH_SEED;
    uint64_t v4 = QEMU_XXHASH_SEED - XXH_PRIME64_1;

    for (size_t i = 0; i < TARGET_PAGE_SIZE / sizeof(*words); i += 4) {
        v1 = XXH64_round(v1, words[i]);
        v2 = XXH64_round(v2, words[i + 1]);
        v3 = XXH64_round(v3, words[i + 2]);
        v4 = XXH64_round(v4, words[i + 3]);
    }
    return XXH64_avalanche(XXH64_mergerounds(v1, v2, v3, v4) +
                           TARGET_PAGE_SIZE);
}
