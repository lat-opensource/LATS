/*
 * Memory region management for Tiny Code Generator for QEMU
 *
 * Copyright (c) 2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/madvise.h"
#include "qemu/mprotect.h"
#include "qemu/memalign.h"
#include "qemu/cacheinfo.h"
#include "qemu/qtree.h"
#include "qapi/error.h"
#include "tcg/tcg.h"
#include "exec/translation-block.h"
#include "tcg-internal.h"
#include "tcg/tcg-split-tb.h"
#include "target/i386/latx/include/latx-perfmap.h"
#include "target/i386/latx/include/latx-options.h"
#include "qemu/log.h"

#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
extern void latxs_external_tu_stub_reset(unsigned mtcc);
#endif
#define LATX_SLEB_BUFF_COEF 10
#define LATX_TB_BUFF_COEF   1
/*
 * We divide code_gen_buffer into equally-sized "regions" that TCG threads
 * dynamically allocate from as demand dictates. Given appropriate region
 * sizing, this minimizes flushes even when some TCG threads generate a lot
 * more code than others.
 */
struct tcg_region_state {
    QemuMutex lock;

    /* fields set at init time */
    void *start_aligned;
    uint64_t after_prologue[MTCC_NR];
    size_t n;
    size_t size; /* size of one region */
    size_t stride; /* .size + guard size */
    size_t total_size; /* size of entire buffer, >= n * stride */

    size_t n_mtcc;
    /*
     * eg. if   n = 8, MTCC_NR = 2,
     *     then n_mtcc = 2, mtcc_n = 8/2 = 4
     *
     * +-+-+-+-+-+-+-+-+
     * |0|1|2|3|4|5|6|7|
     * +-+-+-+-+-+-+-+-+
     * mtcc_0  | mtcc_1
     *
     * mtcc_0 : use 0,1,2,3 and current_mtcc[0]
     * mtcc_1 : use 4,5,6,7 and current_mtcc[1]
     * 
     * mtcc_i  is full: when current_mtcc[i] == mtcc_n
     */
    size_t mtcc_n;
    size_t current_mtcc[MTCC_NR];

    /* fields protected by the lock */
    size_t current; /* current region index */
    size_t agg_size_full; /* aggregate size of full regions */
    void *tb_gen_buffer[MTCC_NR];
    size_t tb_gen_offset[MTCC_NR];
    size_t tb_gen_size[MTCC_NR];
    void *search_gen_buffer[MTCC_NR];
    void *search_gen_ptr[MTCC_NR];
    void *search_gen_end[MTCC_NR];
};

static struct tcg_region_state region;

void latxs_tr_tcg_region_offset(uint64_t offset, int mtcc_id);

extern void tcg_region_get_info(size_t *size, int *n_mtcc, int *mtcc_n);
void tcg_region_get_info(size_t *size, int *n_mtcc, int *mtcc_n)
{
    *size = region.size / 1024 / 1024;
    *n_mtcc = region.n_mtcc;
    *mtcc_n = region.mtcc_n;
}

uint64_t tcg_region_get_offset(const TCGContext *s)
{
    uintptr_t buffer = (uintptr_t)s->code_gen_buffer;
    uintptr_t start = (uintptr_t)region.start_aligned;
    size_t curr_region = (buffer - start) / region.stride;
    size_t base_region = s->mtcc_id * region.mtcc_n;

    g_assert(curr_region >= base_region);
    g_assert(curr_region < base_region + region.mtcc_n);
    return (curr_region - base_region) * region.stride;
}

static void tcg_region_get_range(int mtcc_id, uint64_t *base, uint64_t *size,
        int is_after_prologue)
{
    uint64_t mtcc_base = (uint64_t)region.start_aligned
                         + mtcc_id * region.mtcc_n * region.stride;;
    if (is_after_prologue) {
        if (option_region_prologue) {
            *base = mtcc_base + (uint64_t)region.after_prologue[mtcc_id];
        } else {
            *base = (uint64_t)region.after_prologue[mtcc_id];
        }
    } else {
        *base = mtcc_base;
    }
    *size = region.mtcc_n * region.stride
            - (*base - mtcc_base);
}

bool in_code_gen_buffer(const void *p)
{
    /*
     * Much like it is valid to have a pointer to the byte past the
     * end of an array (so long as you don't dereference it), allow
     * a pointer to the byte past the end of the code gen buffer.
     */
    return (size_t)(p - region.start_aligned) <= region.total_size;
}

#ifdef CONFIG_DEBUG_TCG
const void *tcg_splitwx_to_rx(void *rw)
{
    /* Pass NULL pointers unchanged. */
    if (rw) {
        g_assert(in_code_gen_buffer(rw));
        rw += tcg_splitwx_diff;
    }
    return rw;
}

void *tcg_splitwx_to_rw(const void *rx)
{
    /* Pass NULL pointers unchanged. */
    if (rx) {
        rx -= tcg_splitwx_diff;
        /* Assert that we end with a pointer in the rw region. */
        g_assert(in_code_gen_buffer(rx));
    }
    return (void *)rx;
}
#endif /* CONFIG_DEBUG_TCG */

#ifdef CONFIG_TCG_REGION_TREE_ENABLE

static void tb_destroy(gpointer value)
{
    TranslationBlock *tb = value;
    qemu_spin_destroy(&tb->jmp_lock);
}

#include "region-tree.inc"

#else

#ifdef CONFIG_LATX_TU
#ifdef QEMU_OSDEP_H
static TBMini *lats_tu_chunk_hint[65536];

void lats_register_tu_chunk_hint(uintptr_t tbm_addr, uintptr_t start,
                                 uintptr_t end);
void lats_register_tu_chunk_hint(uintptr_t tbm_addr, uintptr_t start,
                                 uintptr_t end)
{
    if (start >= end) {
        return;
    }
    TBMini *tbm = (TBMini *)tbm_addr;
    uintptr_t first_chunk = start >> 10;
    uintptr_t last_chunk = (end - 1) >> 10;
    for (uintptr_t c = first_chunk; c <= last_chunk; c++) {
        qatomic_set(&lats_tu_chunk_hint[c & 0xffff], tbm);
    }
}
#endif

static void *query_tu_tbm_table(TBMini *tbm, uintptr_t tc_ptr)
{
    uint64_t tb_num =
        tbm->mtbp_uint64 & MAKE_64BIT_MASK(0, HOST_VIRT_ADDR_SPACE_BITS);
    uint64_t table_bytes;
    uintptr_t table_end;
    if (!tb_num || tb_num > INT_MAX ||
        __builtin_mul_overflow(tb_num + 1, sizeof(struct TBMini),
                               &table_bytes) ||
        __builtin_add_overflow((uintptr_t)tbm, table_bytes, &table_end) ||
        !in_code_gen_buffer(tbm) || !in_code_gen_buffer((void *)table_end)) {
        return NULL;
    }

    TBMini *first = tbm + 1;
    TranslationBlock *owner = (void *)(first->mtbp_uint64 &
            MAKE_64BIT_MASK(0, HOST_VIRT_ADDR_SPACE_BITS));
    uintptr_t base = tb_tu_identity(owner);
    uint64_t bytes = owner->s_data->tu_size;
    TranslationBlock *normal = NULL;
    if (!bytes || bytes > UINTPTR_MAX - base ||
        !in_code_gen_buffer((void *)base) ||
        !in_code_gen_buffer((void *)(base + bytes))) {
        return NULL;
    }

    /* Appended LLVM code has explicit records in this same live TU. */
    if (owner && TB_TU_PAGES(owner) && TB_TU_PAGES(owner)->native) {
        AOTNativeTUInfo *info = TB_TU_PAGES(owner)->native;
        for (unsigned i = 0; i < info->count; ++i) {
            AOTNativeRecord *record = &info->records[i];
            uintptr_t start = (uintptr_t)record->entry;
            if (record->recover && tc_ptr >= start &&
                tc_ptr - start < record->entry_bytes) {
                return owner;
            }
        }
    }

    for (uint64_t i = 0; i < tb_num; i++) {
        tbm += 1;
        if (!in_code_gen_buffer((void *)tbm)) {
            return NULL;
        }
        TranslationBlock *member = (void *)(tbm->mtbp_uint64 &
                MAKE_64BIT_MASK(0, HOST_VIRT_ADDR_SPACE_BITS));
        uintptr_t start = (uintptr_t)member->tc.ptr;
        uint32_t size = tbm->mtbp_struct.magic;
        if (tb_tu_identity(member) != base || start < base ||
            start - base > bytes || size > bytes - (start - base) ||
            (uint64_t)member->ir2_num * 4 != size) {
            return NULL;
        }

        /* Saved CALL members may follow out-of-line repair code. Existing
         * final members can also contain other members' repair stubs. */
        uint32_t cold = member->s_data->ras_tp_cold_offset;
        uint32_t cold_size = member->s_data->ras_tp_cold_insns * 4U;
        if (cold_size && cold <= bytes && cold_size <= bytes - cold &&
            tc_ptr >= base + cold && tc_ptr - (base + cold) < cold_size) {
            return member;
        }
        if (tc_ptr >= start && tc_ptr - start < size) {
            if (normal) {
                return NULL;
            }
            normal = member;
        }
    }
    return normal;
}
#endif

static void *tcg_tb_lookup_fast(uintptr_t tc_ptr)
{
    if (!in_code_gen_buffer((void *)tc_ptr)) {
        return NULL;
    }
#ifdef QEMU_OSDEP_H
    uintptr_t min_tbm = (uintptr_t)region.start_aligned;
    if (region.stride) {
        size_t curr_region =
            (tc_ptr - (uintptr_t)region.start_aligned) / region.stride;
        if (curr_region < region.n) {
            uintptr_t r_start = (uintptr_t)region.start_aligned +
                                curr_region * region.stride;
            uint32_t mtcc_id =
                region.mtcc_n ? (curr_region / region.mtcc_n) : 0;
            if (option_region_prologue) {
                r_start += (uintptr_t)region.after_prologue[mtcc_id];
            } else if ((uintptr_t)region.after_prologue[mtcc_id] > r_start) {
                r_start = (uintptr_t)region.after_prologue[mtcc_id];
            }
            if (tc_ptr < r_start) {
                return NULL;
            }
            min_tbm = ROUND_DOWN(r_start, (uintptr_t)CODE_GEN_ALIGN);
        }
    }
#endif
    TBMini *tbm = (TBMini *)(ROUND_DOWN((uintptr_t)tc_ptr,
                    CODE_GEN_ALIGN));
#if defined(CONFIG_LATX_TU) && defined(QEMU_OSDEP_H)
    TBMini *cand = qatomic_read(&lats_tu_chunk_hint[(tc_ptr >> 10) & 0xffff]);
    if (cand && in_code_gen_buffer(cand) &&
        ((uintptr_t)cand & (CODE_GEN_ALIGN - 1)) == 0 &&
        (uintptr_t)cand >= min_tbm &&
        (uintptr_t)cand <= (uintptr_t)tbm &&
        cand->mtbp_struct.magic == TU_TBM_MAGIC) {
        uint64_t tb_num = cand->mtbp_uint64 &
            MAKE_64BIT_MASK(0, HOST_VIRT_ADDR_SPACE_BITS);
        uint64_t table_bytes;
        uintptr_t table_end;
        if (tb_num && tb_num <= INT_MAX &&
            !__builtin_mul_overflow(tb_num + 1, sizeof(struct TBMini),
                                    &table_bytes) &&
            !__builtin_add_overflow((uintptr_t)cand, table_bytes,
                                    &table_end) &&
            in_code_gen_buffer((void *)table_end)) {
            TBMini *first = cand + 1;
            TranslationBlock *owner = (void *)(first->mtbp_uint64 &
                MAKE_64BIT_MASK(0, HOST_VIRT_ADDR_SPACE_BITS));
            uintptr_t base = tb_tu_identity(owner);
            uint64_t bytes = owner->s_data->tu_size;
            if (base >= table_end &&
                base <= ROUND_UP(table_end, (uintptr_t)CODE_GEN_ALIGN) &&
                bytes && bytes <= UINTPTR_MAX - base &&
                tc_ptr >= base && tc_ptr < base + bytes) {
                tbm = cand;
            }
        }
    }
#endif
    while (tbm->mtbp_struct.magic != TB_MAGIC
#ifdef CONFIG_LATX_TU
           && tbm->mtbp_struct.magic != TU_TBM_MAGIC
#endif
    ) {
        tbm = (TBMini *)((uint64_t)tbm - CODE_GEN_ALIGN);
        if (!in_code_gen_buffer((void *)tbm)
#ifdef QEMU_OSDEP_H
            || (uintptr_t)tbm < min_tbm
#endif
        ) {
            return NULL;
        }
    }
    void *tb = (void *)(tbm->mtbp_uint64 &
                MAKE_64BIT_MASK(0, HOST_VIRT_ADDR_SPACE_BITS));
#ifdef CONFIG_LATX_TU
    if (tbm->mtbp_struct.magic == TU_TBM_MAGIC) {
        tb = query_tu_tbm_table(tbm, tc_ptr);
#ifdef QEMU_OSDEP_H
        if (tb) {
            qatomic_set(&lats_tu_chunk_hint[(tc_ptr >> 10) & 0xffff], tbm);
        }
#endif
    }
#endif
    return tb;
}

TranslationBlock *tcg_tb_lookup(uintptr_t tc_ptr)
{
    return tcg_tb_lookup_fast(tc_ptr);
}

void tcg_tb_insert(TranslationBlock *tb) {}
void tcg_tb_remove(TranslationBlock *tb) {}

/* TODO for statistics */
void tcg_tb_foreach(GTraverseFunc func, gpointer user_data) {}
size_t tcg_nb_tbs(void) { return 0; }

#endif

static void tcg_region_bounds(size_t curr_region, void **pstart, void **pend,
        int is_after_prologue)
{
    void *start, *end;

    start = region.start_aligned + curr_region * region.stride;
    end = start + region.size;

    uint32_t mtcc_id = curr_region / region.mtcc_n;
    if (option_region_prologue) {
        if (is_after_prologue || region.after_prologue[mtcc_id]) {
            start += (uint64_t)region.after_prologue[mtcc_id];
        }
    } else if (is_after_prologue) {
        start = (void *) (uint64_t)region.after_prologue[mtcc_id];
    }
    /* The final region may have a few extra pages due to earlier rounding. */
    if (curr_region == region.n - 1) {
        end = region.start_aligned + region.total_size;
    }

    *pstart = start;
    *pend = end;
}

static void tcg_region_copy_prologue(size_t curr_region)
{
    uint32_t mtcc_id = curr_region / region.mtcc_n;
    uint32_t mtcc_base_region = mtcc_id * region.mtcc_n;
    void * src = region.start_aligned + mtcc_base_region * region.stride;
    void * dst = region.start_aligned + curr_region * region.stride;
    size_t len = (size_t)region.after_prologue[mtcc_id];
    memcpy(dst, src, len);
    __builtin___clear_cache((char *)dst, (char *)dst + len);
#ifdef CONFIG_LATX_PRINT
    printf(">tcg_region_copy from %d to %ld\n", mtcc_base_region, curr_region);
#endif
}

static void tcg_region_assign(TCGContext *s, size_t curr_region,
        int is_after_prologue)
{
    void *start, *end;

    tcg_region_bounds(curr_region, &start, &end, is_after_prologue);

    if (option_region_prologue) {
        uint32_t mtcc_id = curr_region / region.mtcc_n;
        if (!is_after_prologue && region.after_prologue[mtcc_id]) {
            tcg_region_copy_prologue(curr_region);
        }
    
        uint32_t curr_region_in_mtcc = curr_region % region.mtcc_n;
        latxs_tr_tcg_region_offset(curr_region_in_mtcc * region.stride, mtcc_id);
    }
    s->code_gen_buffer = start;
    s->code_gen_ptr = start;
    s->code_gen_buffer_size = end - start;
    s->code_gen_highwater = end - TCG_HIGHWATER;
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    /* leave space for TBMini for the 1st TB */
    if (option_split_tb) {
        s->code_gen_ptr += qemu_icache_linesize;
    }
#endif
}

void tcg_region_rewind_current(TCGContext *s)
{
    uintptr_t buffer = (uintptr_t)s->code_gen_buffer;
    uintptr_t start = (uintptr_t)region.start_aligned;
    size_t curr_region;
    size_t base_region = s->mtcc_id * region.mtcc_n;
    bool skip_prologue;

    g_assert(buffer >= start);
    curr_region = (buffer - start) / region.stride;
    g_assert(curr_region >= base_region);
    g_assert(curr_region < base_region + region.mtcc_n);

    /* Only the base region has a prologue when copies are disabled. */
    skip_prologue = option_region_prologue || curr_region == base_region;
    tcg_region_assign(s, curr_region, skip_prologue);
}

extern uint64_t get_region_offset(int mtcc_id);
#define int32_in_intn(x, n) (!(((x) + (1 << ((n) - 1))) & (~((1 << n) - 1))))

int reset_region_offset(int offset, uint32_t mtcc_id)
{
    assert (!int32_in_intn(offset, 26));
    offset <<= 2;
    offset -= get_region_offset(mtcc_id);
    for (int i = 0; i < region.mtcc_n; i++) {
        int new_offset = (offset + i * region.stride) >> 2;
        if (int32_in_intn(new_offset, 26)) {
            latxs_tr_tcg_region_offset(i * region.stride, mtcc_id);
            return new_offset;
        }
    }
    fprintf(stderr, "reset_region_offset fail\n");
    return offset;
}

static bool tcg_region_alloc__locked(TCGContext *s)
{
    if (qemu_tcg_mtcc_enabled()) {
#ifdef CONFIG_LATX_PRINT
        printf("%s tcgcontext %p mtcc %d\n",
                __func__, s, s->mtcc_id);
#endif

        uint32_t i = s->mtcc_id;
        assert(i < region.n_mtcc);
        if (region.current_mtcc[i] == region.mtcc_n) {
            return true;
        }

        int r = region.mtcc_n * i + region.current_mtcc[i];
        assert(r < region.n);
        tcg_region_assign(s, r, 0);
        region.current_mtcc[i]++;

#ifdef CONFIG_LATX_PRINT
        printf("%s tcgcontext %p code %p n %d\n",
                __func__, s, s->code_gen_buffer,
                (int)region.current_mtcc[i]);
#endif

        return false;
    }

    if (region.current == region.n) {
        return true;
    }
    tcg_region_assign(s, region.current, 0);
    region.current++;
    return false;
}

/*
 * Request a new region once the one in use has filled up.
 * Returns true on error.
 */
bool tcg_region_alloc(TCGContext *s)
{
    bool err;
    /* read the region size now; alloc__locked will overwrite it on success */
    size_t size_full = s->code_gen_buffer_size;

    qemu_mutex_lock(&region.lock);
    err = tcg_region_alloc__locked(s);
    if (!err) {
        region.agg_size_full += size_full - TCG_HIGHWATER;
    }
    qemu_mutex_unlock(&region.lock);
    return err;
}

/* Reserve a code/data block without flushing; the caller owns the stream. */
void *tcg_region_alloc_data(TCGContext *s, size_t size)
{
    uintptr_t start, end;

    if (!size || size > SIZE_MAX - (CODE_GEN_ALIGN - 1)) {
        return NULL;
    }
    size = QEMU_ALIGN_UP(size, CODE_GEN_ALIGN);
    for (;;) {
        start = QEMU_ALIGN_UP((uintptr_t)s->code_gen_ptr, CODE_GEN_ALIGN);
        end = (uintptr_t)s->code_gen_highwater;
        if (start <= end && size <= end - start) {
            qatomic_set(&s->code_gen_ptr, (void *)(start + size));
            return (void *)start;
        }
        if (size > s->code_gen_buffer_size - TCG_HIGHWATER ||
            tcg_region_alloc(s)) {
            return NULL;
        }
    }
}

/*
 * Perform a context's first region allocation.
 * This function does _not_ increment region.agg_size_full.
 */
static void tcg_region_initial_alloc__locked(TCGContext *s)
{
    bool err = tcg_region_alloc__locked(s);
    g_assert(!err);
}

void tcg_region_initial_alloc(TCGContext *s)
{
    qemu_mutex_lock(&region.lock);
    tcg_region_initial_alloc__locked(s);
    qemu_mutex_unlock(&region.lock);
}

/* Call from a safe-work context */
void tcg_region_reset_all(void)
{
    unsigned int n_ctxs = qatomic_read(&tcg_cur_ctxs);
    unsigned int i;

    qemu_mutex_lock(&region.lock);
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE) && defined(CONFIG_LATX_TU) && \
    defined(QEMU_OSDEP_H)
    memset(lats_tu_chunk_hint, 0, sizeof(lats_tu_chunk_hint));
#endif
    region.current = 0;
    region.agg_size_full = 0;
    for (i = 0; i < MTCC_NR; i++) {
        region.current_mtcc[i] = 0;
#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
        latxs_external_tu_stub_reset(i);
#endif
        if (option_split_tb) region_reset_tb(i);
        if (option_split_search) region_reset_search(i);
    }

    if (qemu_tcg_mtcc_enabled()) {
        for (i = 0; i < n_ctxs; i++) {
            TCGContext *s = qatomic_read(&tcg_ctxs[i]);
            uint32_t id = s->mtcc_id;
            g_assert(id < region.n_mtcc);
            if (region.current_mtcc[id] == 0) {
                /* Preserve this MTCC's static code and reserve its region.
                 * Another vCPU must not allocate the same stream again. */
                tcg_region_assign(s, id * region.mtcc_n, 1);
                region.current_mtcc[id] = 1;
            } else {
                tcg_region_initial_alloc__locked(s);
            }
        }
    } else {
        TCGContext *s = qatomic_read(&tcg_ctxs[0]);
        tcg_region_assign(s, 0, 1);
        region.current = 1;
        for (i = 1; i < n_ctxs; i++) {
            s = qatomic_read(&tcg_ctxs[i]);
            tcg_region_initial_alloc__locked(s);
        }
    }
    qemu_mutex_unlock(&region.lock);

#ifdef CONFIG_TCG_REGION_TREE_ENABLE
    tcg_region_tree_reset_all();
#endif
}

void tcg_region_reset_mtcc(uint32_t mtcc_id)
{
    assert(qemu_tcg_mtcc_enabled());

    unsigned int n_ctxs = qatomic_read(&tcg_cur_ctxs);
    unsigned int i;

    qemu_mutex_lock(&region.lock);
#if !defined(CONFIG_TCG_REGION_TREE_ENABLE) && defined(CONFIG_LATX_TU) && \
    defined(QEMU_OSDEP_H)
    memset(lats_tu_chunk_hint, 0, sizeof(lats_tu_chunk_hint));
#endif
    region.agg_size_full = 0;
    region.current_mtcc[mtcc_id] = 0;

#if defined(CONFIG_LATX_TU) && defined(CONFIG_SOFTMMU)
    latxs_external_tu_stub_reset(mtcc_id);
#endif

    bool is_first = true;
    for (i = 0; i < n_ctxs; i++) {
        TCGContext *s = qatomic_read(&tcg_ctxs[i]);
        if (s->mtcc_id == mtcc_id) {
            if (is_first) {
                is_first = false;
                int r = region.mtcc_n * mtcc_id + 0;
                tcg_region_assign(s, r, 1);
                region.current_mtcc[mtcc_id] = 1;
            } else {
                tcg_region_initial_alloc__locked(s);
            }
        }
    }
    qemu_mutex_unlock(&region.lock);

#ifdef CONFIG_TCG_REGION_TREE_ENABLE
    tcg_region_tree_reset_mtcc(mtcc_id);
#endif
    /* print_buffer_size(mtcc_id); */
    if (option_split_tb) {
        region_reset_tb(mtcc_id);
    }
    if (option_split_search) {
        region_reset_search(mtcc_id);
    }
}

static size_t tcg_n_regions(size_t tb_size, unsigned max_cpus)
{
#ifdef CONFIG_USER_ONLY
    return 1;
#else
    return 32;
    size_t n_regions;

    if (tcg_cc_n_regions) {
        return tcg_cc_n_regions;
    }

    /*
     * It is likely that some vCPUs will translate more code than others,
     * so we first try to set more regions than max_cpus, with those regions
     * being of reasonable size. If that's not possible we make do by evenly
     * dividing the code_gen_buffer among the vCPUs.
     */
    /* Use a single region if all we have is one vCPU thread */
    if (max_cpus == 1 || !qemu_tcg_mttcg_enabled()) {
        return qemu_tcg_mtcc_enabled() ? MTCC_NR : 1;
    }

    /*
     * Try to have more regions than max_cpus, with each region being >= 2 MB.
     * If we can't, then just allocate one region per vCPU thread.
     */
    n_regions = tb_size / (2 * MiB);
    if (n_regions <= max_cpus) {
        return max_cpus * (qemu_tcg_mtcc_enabled()?MTCC_NR:1);
    }
    return MIN(n_regions, max_cpus * 8 * (qemu_tcg_mtcc_enabled()?MTCC_NR:1));
#endif
}

/*
 * Minimum size of the code gen buffer.  This number is randomly chosen,
 * but not so small that we can't have a fair number of TB's live.
 *
 * Maximum size, MAX_CODE_GEN_BUFFER_SIZE, is defined in tcg-target.h.
 * Unless otherwise indicated, this is constrained by the range of
 * direct branches on the host cpu, as used by the TCG implementation
 * of goto_tb.
 */
#define MIN_CODE_GEN_BUFFER_SIZE     (1 * MiB)

#if TCG_TARGET_REG_BITS == 32
#define DEFAULT_CODE_GEN_BUFFER_SIZE_1 (32 * MiB)
#ifdef CONFIG_USER_ONLY
/*
 * For user mode on smaller 32 bit systems we may run into trouble
 * allocating big chunks of data in the right place. On these systems
 * we utilise a static code generation buffer directly in the binary.
 */
#define USE_STATIC_CODE_GEN_BUFFER
#endif
#else /* TCG_TARGET_REG_BITS == 64 */
#ifdef CONFIG_USER_ONLY
/*
 * As user-mode emulation typically means running multiple instances
 * of the translator don't go too nuts with our default code gen
 * buffer lest we make things too hard for the OS.
 */
#define DEFAULT_CODE_GEN_BUFFER_SIZE_1 (1 * GiB)
#else
/*
 * We expect most system emulation to run one or two guests per host.
 * Users running large scale system emulation may want to tweak their
 * runtime setup via the tb-size control on the command line.
 */
#define DEFAULT_CODE_GEN_BUFFER_SIZE_1 (2 * GiB)
#endif
#endif

#define DEFAULT_CODE_GEN_BUFFER_SIZE \
  (DEFAULT_CODE_GEN_BUFFER_SIZE_1 < MAX_CODE_GEN_BUFFER_SIZE \
   ? DEFAULT_CODE_GEN_BUFFER_SIZE_1 : MAX_CODE_GEN_BUFFER_SIZE)

#define ALIGNMENT_4GB           (4UL * 1024 * 1024 * 1024)
#define ALIGNMENT_1MB           (1UL * 1024 * 1024)
#define ALL_64BIT_ADDR_SPACE    0xffffffffffffffff

#ifdef USE_STATIC_CODE_GEN_BUFFER
static uint8_t static_code_gen_buffer[DEFAULT_CODE_GEN_BUFFER_SIZE]
    __attribute__((aligned(CODE_GEN_ALIGN)));

static int alloc_code_gen_buffer(size_t tb_size, int splitwx, Error **errp)
{
    void *buf, *end;
    size_t size;

    if (splitwx > 0) {
        error_setg(errp, "jit split-wx not supported");
        return -1;
    }

    /* page-align the beginning and end of the buffer */
    buf = static_code_gen_buffer;
    end = static_code_gen_buffer + sizeof(static_code_gen_buffer);
    buf = QEMU_ALIGN_PTR_UP(buf, qemu_real_host_page_size());
    end = QEMU_ALIGN_PTR_DOWN(end, qemu_real_host_page_size());

    size = end - buf;

    /* Honor a command-line option limiting the size of the buffer.  */
    if (size > tb_size) {
        size = QEMU_ALIGN_DOWN(tb_size, qemu_real_host_page_size());
    }

    region.start_aligned = buf;
    region.total_size = size;

    return PROT_READ | PROT_WRITE;
}
#elif defined(_WIN32)
/*
 * Local source-level compatibility with Unix.
 * Used by tcg_region_init below.
 */
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

static int alloc_code_gen_buffer(size_t size, int splitwx, Error **errp)
{
    void *buf;

    if (splitwx > 0) {
        error_setg(errp, "jit split-wx not supported");
        return -1;
    }

    buf = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT,
                             PAGE_EXECUTE_READWRITE);
    if (buf == NULL) {
        error_setg_win32(errp, GetLastError(),
                         "allocate %zu bytes for jit buffer", size);
        return false;
    }

    region.start_aligned = buf;
    region.total_size = size;

    return PROT_READ | PROT_WRITE | PROT_EXEC;
}
#else
static int alloc_code_gen_buffer_anon(size_t size, int prot,
                                      int flags, Error **errp)
{
    void *buf, *buf_ptr, *ptr;

    ptr = mmap(NULL, ALIGNMENT_1MB, prot, flags, -1, 0);
    buf_ptr = (void*) ((uint64_t)ptr & (ALL_64BIT_ADDR_SPACE - ALIGNMENT_4GB + 1));
    buf_ptr = (void*) ((uint64_t)buf_ptr - ALIGNMENT_4GB);

    buf = mmap(buf_ptr, size, prot, flags, -1, 0);
    if (buf == MAP_FAILED) {
        error_setg_errno(errp, errno,
                         "allocate %zu bytes for jit buffer", size);
        return -1;
    }

    munmap(ptr, ALIGNMENT_1MB);

    region.start_aligned = buf;
    region.total_size = size;
    return prot;
}

struct __attribute__((aligned(64))) TBAlign {
    struct TranslationBlock tb;
};

#ifdef CONFIG_LATX_TU
/*
 * Ordinary TBs use the shared separated_data in tcg.c.  Full TU TBs and
 * opt-in modes with persistent cold state select this combined slot, so do
 * not make the default split-TB slot pay for it.
 */
struct __attribute__((aligned(64))) TBFullAlign {
    struct TranslationBlock tb;
    struct separated_data sd;
};

struct __attribute__((aligned(64))) TBTULinkAlign {
    struct TranslationBlock tb;
    struct separated_data sd;
    struct tb_tu_link_data link;
};

struct __attribute__((aligned(64))) TUPagesAlign {
    TranslationUnitPages pages;
};
#endif

int split_tb_base_stat;

static bool alloc_tb_gen_buffer(size_t size)
{
    void *buf;
    int i;
    size_t buf_size;

    buf = mmap((void*)SPLIT_TB_BASE, size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((uint64_t)buf != SPLIT_TB_BASE) {
        split_tb_base_stat = 0;
        buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    } else {
        split_tb_base_stat = 1;
    }
    if (buf == MAP_FAILED) {
        return false;
    }
#ifdef CONFIG_LATX_PRINT
    printf("%s:%d split tb %p stat %x\n", __func__, __LINE__,
            buf, split_tb_base_stat);
#endif

    /* Request large pages for the buffer.  */
    qemu_madvise(buf, size, QEMU_MADV_HUGEPAGE);

    buf_size = size / MTCC_NR;
    buf_size = QEMU_ALIGN_DOWN(buf_size, qemu_real_host_page_size());
    g_assert(qemu_icache_linesize > 0);
    g_assert(buf_size >= qemu_icache_linesize);
    g_assert(QEMU_IS_ALIGNED(buf_size, qemu_icache_linesize));

    for (i = 0; i < MTCC_NR; i++) {
        region.tb_gen_buffer[i] = buf + i * buf_size;
        /* Keep offset zero available as the no-last-TB return sentinel. */
        region.tb_gen_offset[i] = qemu_icache_linesize;
        region.tb_gen_size[i] = buf_size;
    }

    return true;
}

static void *region_alloc_tb_slot(TCGContext *s, size_t slot_size)
{
    uint32_t mtcc;
    size_t offset;

    mtcc = s->mtcc_id;
    /* All slot sizes retain cache-line alignment, so a byte bump pointer can
     * safely mix regular TBs, full TU TBs and TU page descriptors. */
    offset = qatomic_read(&region.tb_gen_offset[mtcc]);
    for (;;) {
        size_t expected = offset;

        if (slot_size > region.tb_gen_size[mtcc] ||
            offset > region.tb_gen_size[mtcc] - slot_size) {
            return NULL;
        }
        offset = qatomic_cmpxchg(&region.tb_gen_offset[mtcc],
                                 offset, offset + slot_size);
        if (offset == expected) {
            g_assert(offset != 0);
            break;
        }
    }

    return (uint8_t *)region.tb_gen_buffer[mtcc] + offset;
}

TranslationBlock *region_alloc_tb(TCGContext *s, bool full, bool tu_link)
{
    /* Pin the compact system LAT layout.  Instrumented, user and QEMU TCG
     * builds have additional TB state and use their actual aligned sizes. */
#if defined(CONFIG_LATX_TU) && !defined(CONFIG_LATX_PROFILER) && \
    !defined(CONFIG_LATX_DEBUG) && !defined(CONFIG_USER_ONLY) && \
    !defined(CONFIG_LATX_TCG_MODE) && !defined(CONFIG_TCG_REGION_TREE_ENABLE)
    QEMU_BUILD_BUG_ON(sizeof(TranslationBlock) > 384);
    QEMU_BUILD_BUG_ON(sizeof(struct TBFullAlign) > 512);
    QEMU_BUILD_BUG_ON(sizeof(struct TBTULinkAlign) > 512);
#endif
#ifdef CONFIG_LATX_TU
    QEMU_BUILD_BUG_ON(sizeof(struct separated_data) > 128);
    QEMU_BUILD_BUG_ON(sizeof(struct tb_tu_link_data) > 32);
    QEMU_BUILD_BUG_ON(sizeof(struct TUPagesAlign) > 320);
    QEMU_BUILD_BUG_ON(offsetof(struct TBFullAlign, sd) !=
                      sizeof(TranslationBlock));
    QEMU_BUILD_BUG_ON(offsetof(struct TBTULinkAlign, sd) !=
                      sizeof(TranslationBlock));
    QEMU_BUILD_BUG_ON(offsetof(struct TBTULinkAlign, link) !=
                      sizeof(TranslationBlock) + sizeof(struct separated_data));
    g_assert(!tu_link || full);
    size_t slot_size = tu_link ? sizeof(struct TBTULinkAlign) :
                       full ? sizeof(struct TBFullAlign) :
                              sizeof(struct TBAlign);
#else
    g_assert(!full);
    g_assert(!tu_link);
    size_t slot_size = sizeof(struct TBAlign);
#endif

    return region_alloc_tb_slot(s, slot_size);
}

#ifdef CONFIG_LATX_TU
TranslationUnitPages *region_alloc_tu_pages(TCGContext *s)
{
    return region_alloc_tb_slot(s, sizeof(struct TUPagesAlign));
}

TranslationBlock **region_alloc_tu_members(TCGContext *s, size_t count)
{
    size_t bytes = QEMU_ALIGN_UP(count * sizeof(TranslationBlock *),
                                 qemu_icache_linesize);

    return region_alloc_tb_slot(s, bytes);
}
#endif

void region_reset_tb(uint32_t mtcc)
{
    g_assert(qemu_icache_linesize > 0);
    qatomic_set(&region.tb_gen_offset[mtcc], qemu_icache_linesize);
}

static bool alloc_search_buffer(size_t size)
{
    void *buf;
    int i;
    size_t buf_size;

    buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        return false;
    }

    /* Request large pages for the buffer.  */
    qemu_madvise(buf, size, QEMU_MADV_HUGEPAGE);

    buf_size = size / MTCC_NR;
    buf_size = QEMU_ALIGN_DOWN(buf_size, qemu_real_host_page_size());

    for (i = 0; i < MTCC_NR; i++) {
        region.search_gen_buffer[i] = buf + i * buf_size;
        region.search_gen_ptr[i] = region.search_gen_buffer[i];
        region.search_gen_end[i] = region.search_gen_buffer[i] + buf_size;
    }

    return true;
}

void *region_alloc_push_pop_data(TranslationBlock *tb, uint32_t mtcc, int ir1_nr)
{
    if (!option_pushpop) {
        return NULL;
    }

    if (option_split_search && tb->search_ptr) {
        void *opt = (uint8_t *)tb->search_ptr + ir1_nr * sizeof(int);

#ifndef CONFIG_LATX_TU
        tb_set_opt_ptr(tb, opt);
#endif
        memset(opt, 0, ir1_nr * sizeof(int));
        return opt;
    }

    uint64_t start = (uint64_t)qatomic_read(&region.search_gen_ptr[mtcc]);
    uint64_t end;
    size_t bytes = ir1_nr * sizeof(int);

    /*
     * With split search, search and push/pop recovery data have the same
     * per-instruction size.  Reserve them together and keep search_ptr at
     * the first half, so the opt pointer is derivable without a per-TB field.
     */
    if (option_split_search) {
        bytes *= 2;
    }
    for (;;) {
        uint64_t expected = start;
        end = start + bytes;
        if (end >= (uint64_t)region.search_gen_end[mtcc]) {
            return NULL;
        }
        start = (uint64_t)qatomic_cmpxchg(
                &region.search_gen_ptr[mtcc], (void *)start, (void *)end);
        if (start == expected) {
            break;
        }
    }

    void *opt;
    if (option_split_search) {
        tb->search_ptr = (void *)start;
        opt = (uint8_t *)tb->search_ptr + ir1_nr * sizeof(int);
#ifndef CONFIG_LATX_TU
        tb_set_opt_ptr(tb, opt);
#endif
    } else {
        opt = (void *)start;
        tb_set_opt_ptr(tb, opt);
    }
    memset(opt, 0, ir1_nr * sizeof(int));
    qemu_log_mask(LAT_LOG_PUSHPOP,
                "[LAT-PUSH] %s opt_ptr %p\n", __func__, opt);
    return opt;
}

void *region_alloc_search(TranslationBlock *tb, uint32_t mtcc)
{
    if (option_split_search && tb->search_ptr) {
        return tb->search_ptr;
    }

    uint64_t start = (uint64_t) qatomic_read(&region.search_gen_ptr[mtcc]);
    for (;;) {
        uint64_t expected = start;
        size_t bytes = tb->icount * sizeof(int);
        if (option_split_search && option_pushpop) {
            bytes *= 2;
        }
        uint64_t end = start + bytes;
        if (end >= (uint64_t) region.search_gen_end[mtcc]) {
            return NULL;
        }
        start = (uint64_t) qatomic_cmpxchg(&region.search_gen_ptr[mtcc], (void*) start, (void*)end);
        if (start == expected) {
            break;
        }
    }
    tb->search_ptr = (void *)start;
    return (void *)start;
}

#ifdef CONFIG_LATX_TU
void *tb_opt_ptr(const TranslationBlock *tb)
{
    if (option_split_search) {
        if (!tb->search_ptr) {
            return NULL;
        }
        return (uint8_t *)tb->search_ptr + tb->icount * sizeof(int);
    }
    return tb->s_data->opt_ptr;
}

void tb_set_opt_ptr(TranslationBlock *tb, void *ptr)
{
    if (option_split_search) {
        /* search_ptr owns the combined search/opt allocation in this mode. */
        if (!ptr) {
            tb->search_ptr = NULL;
        } else {
            g_assert(ptr == tb_opt_ptr(tb));
        }
        return;
    }
    tb->s_data->opt_ptr = ptr;
}
#endif

void region_reset_search(uint32_t mtcc)
{
    uint64_t start = (uint64_t) region.search_gen_buffer[mtcc];
    qatomic_set(&region.search_gen_ptr[mtcc], start);
}

void print_buffer_size(uint32_t mtcc)
{
    size_t tb_offset = region.tb_gen_offset[mtcc];
    size_t tb_gen_size = region.tb_gen_size[mtcc];
    uint64_t sleb_start = (uint64_t) region.search_gen_buffer[mtcc];
    uint64_t sleb_ptr = (uint64_t) region.search_gen_ptr[mtcc];
    uint64_t sleb_end = (uint64_t) region.search_gen_end[mtcc];

    double tb_used = ((double)tb_offset / tb_gen_size) * 100;
    double sleb_used = ((double) (sleb_ptr - sleb_start)/ (sleb_end - sleb_start)) * 100;

    fprintf(stderr, "\n\n tb bytes %zu max %zu used %.2f%% ",
            tb_offset, tb_gen_size, tb_used);
    fprintf(stderr, "sleb start 0x%lx ptr 0x%lx end 0x%lx used %.2f%%\n",
            sleb_start, sleb_ptr, sleb_end, sleb_used);
}

#ifndef CONFIG_TCG_INTERPRETER
#ifdef CONFIG_POSIX
#include "qemu/memfd.h"

static int alloc_code_gen_buffer_splitwx_memfd(size_t size, Error **errp)
{
    void *buf_rw = NULL, *buf_rx = MAP_FAILED;
    int fd = -1;

    buf_rw = qemu_memfd_alloc("tcg-jit", size, 0, &fd, errp);
    if (buf_rw == NULL) {
        goto fail;
    }

    buf_rx = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    if (buf_rx == MAP_FAILED) {
        goto fail_rx;
    }

    close(fd);
    region.start_aligned = buf_rw;
    region.total_size = size;
    tcg_splitwx_diff = buf_rx - buf_rw;

    return PROT_READ | PROT_WRITE;

 fail_rx:
    error_setg_errno(errp, errno, "failed to map shared memory for execute");
 fail:
    if (buf_rx != MAP_FAILED) {
        munmap(buf_rx, size);
    }
    if (buf_rw) {
        munmap(buf_rw, size);
    }
    if (fd >= 0) {
        close(fd);
    }
    return -1;
}
#endif /* CONFIG_POSIX */

#ifdef CONFIG_DARWIN
#include <mach/mach.h>

extern kern_return_t mach_vm_remap(vm_map_t target_task,
                                   mach_vm_address_t *target_address,
                                   mach_vm_size_t size,
                                   mach_vm_offset_t mask,
                                   int flags,
                                   vm_map_t src_task,
                                   mach_vm_address_t src_address,
                                   boolean_t copy,
                                   vm_prot_t *cur_protection,
                                   vm_prot_t *max_protection,
                                   vm_inherit_t inheritance);

static int alloc_code_gen_buffer_splitwx_vmremap(size_t size, Error **errp)
{
    kern_return_t ret;
    mach_vm_address_t buf_rw, buf_rx;
    vm_prot_t cur_prot, max_prot;

    /* Map the read-write portion via normal anon memory. */
    if (!alloc_code_gen_buffer_anon(size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, errp)) {
        return -1;
    }

    buf_rw = (mach_vm_address_t)region.start_aligned;
    buf_rx = 0;
    ret = mach_vm_remap(mach_task_self(),
                        &buf_rx,
                        size,
                        0,
                        VM_FLAGS_ANYWHERE,
                        mach_task_self(),
                        buf_rw,
                        false,
                        &cur_prot,
                        &max_prot,
                        VM_INHERIT_NONE);
    if (ret != KERN_SUCCESS) {
        /* TODO: Convert "ret" to a human readable error message. */
        error_setg(errp, "vm_remap for jit splitwx failed");
        munmap((void *)buf_rw, size);
        return -1;
    }

    if (mprotect((void *)buf_rx, size, PROT_READ | PROT_EXEC) != 0) {
        error_setg_errno(errp, errno, "mprotect for jit splitwx");
        munmap((void *)buf_rx, size);
        munmap((void *)buf_rw, size);
        return -1;
    }

    tcg_splitwx_diff = buf_rx - buf_rw;
    return PROT_READ | PROT_WRITE;
}
#endif /* CONFIG_DARWIN */
#endif /* CONFIG_TCG_INTERPRETER */

static int alloc_code_gen_buffer_splitwx(size_t size, Error **errp)
{
#ifndef CONFIG_TCG_INTERPRETER
# ifdef CONFIG_DARWIN
    return alloc_code_gen_buffer_splitwx_vmremap(size, errp);
# endif
# ifdef CONFIG_POSIX
    return alloc_code_gen_buffer_splitwx_memfd(size, errp);
# endif
#endif
    error_setg(errp, "jit split-wx not supported");
    return -1;
}

static int alloc_code_gen_buffer(size_t size, int splitwx, Error **errp)
{
    ERRP_GUARD();
    int prot, flags;

    if (splitwx) {
        prot = alloc_code_gen_buffer_splitwx(size, errp);
        if (prot >= 0) {
            return prot;
        }
        /*
         * If splitwx force-on (1), fail;
         * if splitwx default-on (-1), fall through to splitwx off.
         */
        if (splitwx > 0) {
            return -1;
        }
        error_free_or_abort(errp);
    }

    /*
     * macOS 11.2 has a bug (Apple Feedback FB8994773) in which mprotect
     * rejects a permission change from RWX -> NONE when reserving the
     * guard pages later.  We can go the other way with the same number
     * of syscalls, so always begin with PROT_NONE.
     */
#ifdef CONFIG_TCG_REGION_GUARDPAGE
    prot = PROT_NONE;
#else
    prot = (PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
    flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef CONFIG_DARWIN
    /* Applicable to both iOS and macOS (Apple Silicon). */
    if (!splitwx) {
        flags |= MAP_JIT;
    }
#endif

    return alloc_code_gen_buffer_anon(size, prot, flags, errp);
}
#endif /* USE_STATIC_CODE_GEN_BUFFER, WIN32, POSIX */

/*
 * Initializes region partitioning.
 *
 * Called at init time from the parent thread (i.e. the one calling
 * tcg_context_init), after the target's TCG globals have been set.
 *
 * Region partitioning works by splitting code_gen_buffer into separate regions,
 * and then assigning regions to TCG threads so that the threads can translate
 * code in parallel without synchronization.
 *
 * In softmmu the number of TCG threads is bounded by max_cpus, so we use at
 * least max_cpus regions in MTTCG. In !MTTCG we use a single region.
 * Note that the TCG options from the command-line (i.e. -accel accel=tcg,[...])
 * must have been parsed before calling this function, since it calls
 * qemu_tcg_mttcg_enabled().
 *
 * In user-mode we use a single region.  Having multiple regions in user-mode
 * is not supported, because the number of vCPU threads (recall that each thread
 * spawned by the guest corresponds to a vCPU thread) is only bounded by the
 * OS, and usually this number is huge (tens of thousands is not uncommon).
 * Thus, given this large bound on the number of vCPU threads and the fact
 * that code_gen_buffer is allocated at compile-time, we cannot guarantee
 * that the availability of at least one region per vCPU thread.
 *
 * However, this user-mode limitation is unlikely to be a significant problem
 * in practice. Multi-threaded guests share most if not all of their translated
 * code, which makes parallel code generation less appealing than in softmmu.
 */
#include "latx-hack.h"
void tcg_region_init(size_t tb_size, int splitwx, unsigned max_cpus)
{
    const size_t page_size = qemu_real_host_page_size();
    size_t region_size;
    int have_prot __attribute__((unused));
    int need_prot;

    /* Size the buffer.  */
    if (tb_size == 0) {
        size_t phys_mem = qemu_get_host_physmem();
        if (phys_mem == 0) {
            tb_size = DEFAULT_CODE_GEN_BUFFER_SIZE;
        } else {
            tb_size = QEMU_ALIGN_DOWN(phys_mem / 8, page_size);
            tb_size = MIN(DEFAULT_CODE_GEN_BUFFER_SIZE, tb_size);
        }
    }
    if (qemu_tcg_mtcc_enabled()) {
        tb_size = tb_size * MTCC_NR;
    }
    if (tb_size < MIN_CODE_GEN_BUFFER_SIZE) {
        tb_size = MIN_CODE_GEN_BUFFER_SIZE;
    }
    if (tb_size > MAX_CODE_GEN_BUFFER_SIZE) {
        tb_size = MAX_CODE_GEN_BUFFER_SIZE;
    }

    have_prot = alloc_code_gen_buffer(tb_size, splitwx, &error_fatal);
    assert(have_prot >= 0);

    if (option_split_tb) {
        bool ok = alloc_tb_gen_buffer(tb_size / LATX_TB_BUFF_COEF);
        if (!ok) {
            option_split_tb = 0;
        }
    }

    if (option_split_search) {
        bool ok = alloc_search_buffer(tb_size / LATX_SLEB_BUFF_COEF);
        if (!ok) {
            option_split_search = 0;
        }
    }

    region.n_mtcc = MTCC_NR;

    /* Request large pages for the buffer and the splitwx.  */
    qemu_madvise(region.start_aligned, region.total_size, QEMU_MADV_HUGEPAGE);
    if (tcg_splitwx_diff) {
        qemu_madvise(region.start_aligned + tcg_splitwx_diff,
                     region.total_size, QEMU_MADV_HUGEPAGE);
    }

    /*
     * Make region_size a multiple of page_size, using aligned as the start.
     * As a result of this we might end up with a few extra pages at the end of
     * the buffer; we will assign those to the last region.
     */
    region.n = tcg_n_regions(tb_size, max_cpus);
    if (qemu_tcg_mtcc_enabled()) {
        region.mtcc_n = region.n / MTCC_NR;
        assert(region.n == region.mtcc_n * MTCC_NR);
    } else {
        region.mtcc_n = region.n;
    }
    region_size = tb_size / region.n;
    region_size = QEMU_ALIGN_DOWN(region_size, page_size);

    /* A region must have at least 2 pages; one code, one guard */
    g_assert(region_size >= 2 * page_size);
    region.stride = region_size;

#ifdef CONFIG_TCG_REGION_GUARDPAGE
    /* Reserve space for guard pages. */
    region.size = region_size - page_size;
    region.total_size -= page_size;
#else
    region.size = region_size;
#endif

    /* init the region struct */
    qemu_mutex_init(&region.lock);

#ifdef CONFIG_LATX_PRINT
    printf("%s region.n      = %ld\n", __func__, region.n);
    printf("%s region.n_mtcc = %ld\n", __func__, region.n_mtcc);
    printf("%s region.mtcc_n = %ld\n", __func__, region.mtcc_n);
    printf("%s region.start  = %p\n", __func__, region.start_aligned);
    printf("%s region.size   = %#lx B = %#lx KB = %ld MB\n", __func__,
            region.size,
            region.size >> 10,
            region.size >> 20);
    printf("%s region.stride = %#lx B = %#lx KB = %ld MB\n", __func__,
            region.stride,
            region.stride >> 10,
            region.stride >> 20);
    printf("%s region.total  = %#lx B = %#lx KB = %ld MB\n", __func__,
            region.total_size,
            region.total_size >> 10,
            region.total_size >> 20);
#endif

    if (qemu_tcg_mtcc_enabled()) {
        void *start, *end;
        tcg_region_bounds(0, &start, &end, 0);
        tcg_region_bounds(region.mtcc_n, &start, &end, 0);
    } else {
        void *start, *end;
        tcg_region_bounds(0, &start, &end, 0);
    }

    /*
     * Set guard pages in the rw buffer, as that's the one into which
     * buffer overruns could occur.  Do not set guard pages in the rx
     * buffer -- let that one use hugepages throughout.
     * Work with the page protections set up with the initial mapping.
     */
    need_prot = PROT_READ | PROT_WRITE;
#ifndef CONFIG_TCG_INTERPRETER
    if (tcg_splitwx_diff == 0) {
        need_prot |= PROT_EXEC;
    }
#endif
#ifdef CONFIG_TCG_REGION_GUARDPAGE
    for (size_t i = 0, n = region.n; i < n; i++) {
        void *start, *end;

        tcg_region_bounds(i, &start, &end, 0);
        if (have_prot != need_prot) {
            int rc;

            if (need_prot == (PROT_READ | PROT_WRITE | PROT_EXEC)) {
                rc = qemu_mprotect_rwx(start, end - start);
            } else if (need_prot == (PROT_READ | PROT_WRITE)) {
                rc = qemu_mprotect_rw(start, end - start);
            } else {
                g_assert_not_reached();
            }
            if (rc) {
                error_setg_errno(&error_fatal, errno,
                                 "mprotect of jit buffer");
            }
        }
        if (have_prot != 0) {
            /* Guard pages are nice for bug detection but are not essential. */
            (void)qemu_mprotect_none(end, page_size);
        }
    }
#endif

#ifdef CONFIG_TCG_REGION_TREE_ENABLE
    tcg_region_trees_init();
#endif

    /*
     * Leave the initial context initialized to the first region.
     * This will be the context into which we generate the prologue.
     * It is also the only context for CONFIG_USER_ONLY.
     */
    int n = qemu_tcg_mtcc_enabled() ? MTCC_NR : 1;
    for (int i = 0; i < n; ++i) {
        tcg_region_initial_alloc__locked(&tcg_init_ctx[i]);
    }
}

void tcg_region_prologue_set(TCGContext *s)
{
    uint32_t mtcc_id = s->mtcc_id;
    /* Deduct the prologue from the first region.  */
    g_assert((region.start_aligned + mtcc_id * region.mtcc_n * region.stride) == s->code_gen_buffer);
    if (option_region_prologue) {
        region.after_prologue[mtcc_id] = (uint64_t)s->code_ptr -
                        (uint64_t)s->code_gen_buffer;
    } else {
        region.after_prologue[mtcc_id] = (uint64_t) s->code_ptr;
    }
#ifdef CONFIG_LATX_PRINT
    printf("%s after prologue %lx mtcc %d\n",
            __func__, region.after_prologue[mtcc_id],
            mtcc_id);
#endif

    /* Recompute boundaries of the first region. */
    int r = region.mtcc_n * s->mtcc_id + 0;
    tcg_region_assign(s, r, 1);

#ifdef CONFIG_LATX_PRINT
    printf("%s after prologue code %p mtcc %d\n",
            __func__, s->code_gen_buffer,
            mtcc_id);
#endif

    {
        uint64_t base, size;
        tcg_region_get_range(mtcc_id, &base, &size, 1);
        latx_perfmap_insert((void*)base, size,
                mtcc_id ? "latxs_code_cache_R1" : "latxs_code_cache_R0");
        latx_perfmap_flush();
    }

    /* Register the balance of the buffer with gdb. */
    /*tcg_register_jit(tcg_splitwx_to_rx(region.after_prologue),*/
                     /*region.start_aligned + region.total_size -*/
                     /*region.after_prologue);*/
}

/*
 * Returns the size (in bytes) of all translated code (i.e. from all regions)
 * currently in the cache.
 * See also: tcg_code_capacity()
 * Do not confuse with tcg_current_code_size(); that one applies to a single
 * TCG context.
 */
size_t tcg_code_size(void)
{
    unsigned int n_ctxs = qatomic_read(&tcg_cur_ctxs);
    unsigned int i;
    size_t total;

    qemu_mutex_lock(&region.lock);
    total = region.agg_size_full;
    for (i = 0; i < n_ctxs; i++) {
        const TCGContext *s = qatomic_read(&tcg_ctxs[i]);
        size_t size;

        size = qatomic_read(&s->code_gen_ptr) - s->code_gen_buffer;
        g_assert(size <= s->code_gen_buffer_size);
        total += size;
    }
    qemu_mutex_unlock(&region.lock);
    return total;
}

/*
 * Returns the code capacity (in bytes) of the entire cache, i.e. including all
 * regions.
 * See also: tcg_code_size()
 */
size_t tcg_code_capacity(void)
{
    size_t guard_size, capacity;

    /* no need for synchronization; these variables are set at init time */
    guard_size = region.stride - region.size;
    capacity = region.total_size;
    capacity -= (region.n - 1) * guard_size;
    capacity -= region.n * TCG_HIGHWATER;

    return capacity;
}
