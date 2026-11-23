/*
 * SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sys-aot.h"
#include "sys-aot-tu-codec.h"
#include "latx-misc.h"
#include "latx-ras.h"
#include "latx-tu.h"
#include "accel/tcg/internal.h"

static bool aot_codec_write(FILE *file, const void *ptr, size_t size, size_t n)
{
    return !size || !n || fwrite(ptr, size, n, file) == n;
}

void aot_encode_block(aot_block *aot_tb, TranslationBlock *tb)
{
    /* memcpy(aot_tb, tb, sizeof(TranslationBlock)); */
    aot_tb->pc = tb->pc;
    aot_tb->jmp_reset_offset[0] = tb->jmp_reset_offset[0];
    aot_tb->jmp_reset_offset[1] = tb->jmp_reset_offset[1];
    aot_tb->jmp_insn_offset[0] = tb->jmp_insn_offset[0];
    aot_tb->jmp_insn_offset[1] = tb->jmp_insn_offset[1];
    aot_tb->jmp_target_addr[0] = tb->jmp_target_addr[0];
    aot_tb->jmp_target_addr[1] = tb->jmp_target_addr[1];
    aot_tb->inst_ptr_offset = tb->inst_ptr_offset;
    aot_tb->ras_tp_cold_offset = option_rastoptp && tb->inst_ptr_offset ?
        LATXS_RAS_TP_COLD_OFFSET(tb) : 0;
    aot_tb->label_pos_nocheck = tb->label_pos_nocheck;
    aot_tb->link_opt[0] = tb->link_opt[0];
    aot_tb->link_opt[1] = tb->link_opt[1];
    aot_tb->int_opt[0] = tb->int_opt[0];
    aot_tb->int_opt[1] = tb->int_opt[1];
    aot_tb->eflag_use = tb->s_data->eflag_use;
    aot_tb->eflag_use_real = tb->s_data->eflag_use_real;
    aot_tb->last_ir1_type = tb->s_data->last_ir1_type;
    aot_tb->tu_tb_mode = tb->s_data->tu_tb_mode;
    aot_tb->next_pc[0] = tb->next_pc[0];
    aot_tb->next_pc[1] = tb->next_pc[1];
    aot_tb->jmp_cp_data[0] = tb->jmp_cp_data[0];
    aot_tb->jmp_cp_data[1] = tb->jmp_cp_data[1];
    aot_tb->tu_generation_offset_patch[0] =
        tb->s_data->tu_generation_offset_patch[0];
    aot_tb->tu_generation_offset_patch[1] =
        tb->s_data->tu_generation_offset_patch[1];
    aot_tb->misc_flags = tb->misc_flags;
    aot_tb->flags = tb->flags;
    aot_tb->cflags = tb->cflags;
    aot_tb->size = tb->size;
    aot_tb->icount = tb->icount;
    aot_tb->ir2_num = tb->ir2_num;
    /* Do not serialize the search_ptr arm of the union as a byte count. */
    aot_tb->search_size = option_split_search ? tb->icount * sizeof(int) :
                                               tb->search_size;
    aot_tb->x86_next_pc = tb->s_data->x86_next_pc;
    aot_tb->x86_target_pc = tb->s_data->x86_target_pc;
    aot_tb->tu_jmp[0] = tb->s_data->tu_jmp[0];
    aot_tb->tu_jmp[1] = tb->s_data->tu_jmp[1];
    aot_tb->tc.offset_in_tu = tb->tc.offset_in_tu;
    aot_tb->rel_start = TB_REL_START(tb);
    aot_tb->rel_end = TB_REL_END(tb);
#ifdef CONFIG_LATX_TU
    aot_tb->tu_size = tb->s_data->tu_size;
#endif
    aot_tb->cs_base = tb->cs_base;
    aot_tb->page_addr[0] = tb->page_addr[0];
    aot_tb->page_addr[1] = tb->page_addr[1];
#ifdef CONFIG_LATX_TU
    aot_tb->ptn.eflag_type = tb->ptn.eflag_type;
    if (tb->ptn.eflag_type != PTN_EFLAG_TYPE_INVALID) {
        aot_tb->ptn.os = tb->ptn.os;
        aot_tb->ptn.opnd0 = tb->ptn.opnd0;
        aot_tb->ptn.opnd1 = tb->ptn.opnd1;
        aot_tb->ptn.pcoffset = tb->ptn.pcoffset;
    }
#endif
}

#ifdef CONFIG_LATX_TU
static uint64_t aot_tu_id_mix(uint64_t hash, uint64_t value)
{
    hash ^= value;
    return hash * UINT64_C(1099511628211);
}

static uint64_t aot_tu_id_start(uint64_t entry, unsigned tb_count,
                                const aot_tu_page *pages, uint32_t page_count)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    hash = aot_tu_id_mix(hash, entry & ~TARGET_PAGE_MASK);
    hash = aot_tu_id_mix(hash, page_count);
    for (uint32_t i = 0; i < page_count; i++) {
        hash = aot_tu_id_mix(hash, pages[i].pc_delta);
        hash = aot_tu_id_mix(hash, pages[i].page_fingerprint);
    }
    hash = aot_tu_id_mix(hash, tb_count);
    return hash;
}

static uint64_t aot_tu_id_member(uint64_t hash, uint64_t offset,
                                 uint32_t cs_base, uint32_t flags,
                                 uint32_t cflags, unsigned size,
                                 unsigned icount)
{
    hash = aot_tu_id_mix(hash, offset);
    hash = aot_tu_id_mix(hash, cs_base);
    hash = aot_tu_id_mix(hash, flags);
    hash = aot_tu_id_mix(hash, cflags);
    hash = aot_tu_id_mix(hash, size);
    return aot_tu_id_mix(hash, icount);
}

static uint64_t aot_make_tu_id(TranslationBlock **tb_vector, int tb_count,
                               const aot_tu_page *pages, uint32_t page_count)
{
    target_ulong anchor_page = tb_vector[0]->pc & TARGET_PAGE_MASK;
    uint64_t hash =
        aot_tu_id_start(tb_vector[0]->pc, tb_count, pages, page_count);

    for (int i = 0; i < tb_count; i++) {
        TranslationBlock *tb = tb_vector[i];

        hash = aot_tu_id_member(hash, tb->pc - anchor_page, tb->cs_base,
                                tb->flags, tb->cflags, tb->size, tb->icount);
    }
    return hash ? hash : 1;
}

static gint aot_function_page_cmp(gconstpointer ap, gconstpointer bp)
{
    const aot_tu_page *a = ap, *b = bp;
    return a->pc_delta < b->pc_delta ? -1 : a->pc_delta > b->pc_delta;
}

/* Concatenate complete code blobs, so their internal branches do not move
 * relative to one another. Only TU-relative offsets and relocation indices
 * change. Search and push arrays must remain separate across fragments. */
GBytes *aot_pack_function_tus(const GPtrArray *fragments, uint64_t entry)
{
    g_autoptr(GArray) members = g_array_new(false, false, sizeof(aot_block));
    g_autoptr(GArray) pages = g_array_new(false, false, sizeof(aot_tu_page));
    g_autoptr(GArray) relocations = g_array_new(false, false, sizeof(aot_rel));
    g_autoptr(GByteArray) code = g_byte_array_new();
    g_autoptr(GByteArray) search = g_byte_array_new();
    g_autoptr(GByteArray) push = g_byte_array_new();
    uint64_t anchor = entry & TARGET_PAGE_MASK;
    guint root = fragments->len;

    for (guint i = 0; i < fragments->len; i++) {
        gsize size;
        const aot_tu_head *h = g_bytes_get_data(fragments->pdata[i], &size);
        if (size < sizeof(*h) || h->tb_magic != AOT_TB_MAGIC ||
            h->tu_sum_size != size) {
            return NULL;
        }
        if (h->first_pc == entry) {
            if (root != fragments->len) {
                return NULL;
            }
            root = i;
        }
    }
    if (root == fragments->len) {
        return NULL;
    }
    for (guint order = 0; order < fragments->len; order++) {
        guint index = !order ? root : order <= root ? order - 1 : order;
        gsize size;
        const aot_tu_head *h = g_bytes_get_data(fragments->pdata[index], &size);
        uint64_t prefix =
            sizeof(*h) + (uint64_t)h->page_count * sizeof(aot_tu_page);
        uint64_t blocks_end =
            prefix + (uint64_t)h->tb_count * sizeof(aot_block);
        if (!h->tb_count || h->tb_count > INT_MAX - members->len ||
            !h->page_count || h->page_count > AOT_TU_MAX_PAGES ||
            h->rel_count > INT_MAX - relocations->len || blocks_end > size) {
            return NULL;
        }
        const aot_tu_page *deps = (const void *)(h + 1);
        const aot_block *blocks = (const void *)((const uint8_t *)h + prefix);
        uint64_t code_size = blocks[0].tu_size;
        uint64_t rel_end =
            blocks_end + code_size + (uint64_t)h->rel_count * sizeof(aot_rel);
        if (blocks[0].pc != h->first_pc || !code_size || (code_size & 3) ||
            code_size > INT_MAX - code->len || rel_end > size) {
            return NULL;
        }
        for (uint32_t i = 0; i < h->page_count; i++) {
            uint64_t pc = (h->first_pc & TARGET_PAGE_MASK) + deps[i].pc_delta;
            aot_tu_page dep = {(int64_t)(pc - anchor),
                               deps[i].page_fingerprint};
            guint j;
            for (j = 0; j < pages->len; j++) {
                const aot_tu_page *old = &g_array_index(pages, aot_tu_page, j);
                if (old->pc_delta == dep.pc_delta) {
                    if (old->page_fingerprint != dep.page_fingerprint) {
                        return NULL;
                    }
                    break;
                }
            }
            if (j == pages->len) {
                if (pages->len == AOT_TU_MAX_PAGES ||
                    (dep.pc_delta & ~TARGET_PAGE_MASK)) {
                    return NULL;
                }
                g_array_append_val(pages, dep);
            }
        }
        uint64_t search_size = 0, push_size = 0;
        for (uint32_t i = 0; i < h->tb_count; i++) {
            aot_block block = blocks[i];
            if (block.tc.offset_in_tu > code_size ||
                (uint64_t)block.ir2_num * 4 >
                    code_size - block.tc.offset_in_tu ||
                block.inst_ptr_offset < 0 ||
                (uint64_t)block.inst_ptr_offset > code_size ||
                (block.inst_ptr_offset && option_rastoptp &&
                 (block.ras_tp_cold_offset > code_size ||
                  LATX_RAS_TP_CALL_COLD_INSNS * 4 >
                      code_size - block.ras_tp_cold_offset))) {
                return NULL;
            }
            if (members->len) {
                const aot_block *first = &g_array_index(members, aot_block, 0);
                if (block.cs_base != first->cs_base ||
                    block.flags != first->flags ||
                    block.cflags != first->cflags) {
                    return NULL;
                }
            }
            block.tc.offset_in_tu += code->len;
            if (block.inst_ptr_offset) {
                block.inst_ptr_offset += code->len;
                if (option_rastoptp) {
                    block.ras_tp_cold_offset += code->len;
                }
            }
            for (unsigned j = 0; j < 2; j++) {
                if (block.tu_jmp[j] != TB_JMP_OFFSET_INVALID) {
                    if (block.tu_jmp[j] < 0 ||
                        (uint64_t)block.tu_jmp[j] > code_size) {
                        return NULL;
                    }
                    block.tu_jmp[j] += code->len;
                }
            }
            if (block.rel_start >= 0) {
                if (block.rel_end < block.rel_start ||
                    (uint32_t)block.rel_end >= h->rel_count) {
                    return NULL;
                }
                block.rel_start += relocations->len;
                block.rel_end += relocations->len;
            } else if (block.rel_start != -1 || block.rel_end != -2) {
                return NULL;
            }
            block.tu_size = 0;
            g_array_append_val(members, block);
            search_size += option_split_search ? (uint64_t)block.icount * 4
                                               : block.search_size;
            push_size += option_pushpop ? (uint64_t)block.icount * 4 : 0;
        }
        if (search_size > INT_MAX - search->len ||
            push_size > INT_MAX - push->len ||
            rel_end + search_size + push_size != size) {
            return NULL;
        }
        const uint8_t *bytes = (const void *)h;
        const aot_rel *rels = (const void *)(bytes + blocks_end + code_size);
        for (uint32_t i = 0; i < h->rel_count; i++) {
            switch (rels[i].kind) {
            case AOT_NATIVE_ENTRY:
            case AOT_NATIVE_CALL:
            case AOT_NATIVE_CORE:
            case AOT_NATIVE_CORE_CALL:
            case AOT_NATIVE_DIRECT_CALL:
            case AOT_NATIVE_LINKED_CALL:
            case AOT_NATIVE_INDIRECT_CALL:
            case AOT_NATIVE_RET_MISS:
            case AOT_NATIVE_SYSCALL:
            case AOT_NATIVE_ATOMIC:
            case AOT_NATIVE_CPU_QUERY:
            case AOT_NATIVE_X87:
            case AOT_NATIVE_SYSTEM:
                return NULL;
            default:
                break;
            }
        }
        g_array_append_vals(relocations, rels, h->rel_count);
        g_byte_array_append(code, bytes + blocks_end, code_size);
        g_byte_array_append(search, bytes + rel_end, search_size);
        g_byte_array_append(push, bytes + rel_end + search_size, push_size);
    }
    g_array_sort(pages, aot_function_page_cmp);
    uint64_t total =
        sizeof(aot_tu_head) + (uint64_t)pages->len * sizeof(aot_tu_page) +
        (uint64_t)members->len * sizeof(aot_block) + code->len +
        (uint64_t)relocations->len * sizeof(aot_rel) + search->len + push->len;
    if (total > INT_MAX) {
        return NULL;
    }
    uint64_t id = aot_tu_id_start(entry, members->len,
                                  (const void *)pages->data, pages->len);
    for (guint i = 0; i < members->len; i++) {
        const aot_block *b = &g_array_index(members, aot_block, i);
        id = aot_tu_id_member(id, b->pc - anchor, b->cs_base, b->flags,
                              b->cflags, b->size, b->icount);
    }
    g_array_index(members, aot_block, 0).tu_size = code->len;
    aot_tu_head head = {
        .tb_magic = AOT_TB_MAGIC,
        .tu_sum_size = total,
        .tb_count = members->len,
        .rel_count = relocations->len,
        .first_pc = entry,
        .tu_id = id ? id : 1,
        .page_count = pages->len,
    };
    GByteArray *result = g_byte_array_sized_new(total);
    g_byte_array_append(result, (const void *)&head, sizeof(head));
    g_byte_array_append(result, (const void *)pages->data,
                        pages->len * sizeof(aot_tu_page));
    g_byte_array_append(result, (const void *)members->data,
                        members->len * sizeof(aot_block));
    g_byte_array_append(result, code->data, code->len);
    g_byte_array_append(result, (const void *)relocations->data,
                        relocations->len * sizeof(aot_rel));
    g_byte_array_append(result, search->data, search->len);
    g_byte_array_append(result, push->data, push->len);
    return g_byte_array_free_to_bytes(result);
}

bool aot_encode_tu(FILE *file, TranslationBlock **tb_vector, int tb_count,
                   const aot_tu_page *tu_pages, uint32_t mapping_count,
                   int *record_size, uint64_t *tu_id)
{
    aot_tu_head tu_head = { 0 };
    aot_rel *saved_rels = NULL;
    int curr_ir2_size = 0;
    int saved_rel_count = 0;
    bool ok = false;

    if (!file || !mapping_count || mapping_count > AOT_TU_MAX_PAGES ||
            tb_count <= 0 || !tb_vector[0]->s_data->is_first_tb) {
        return false;
    }

    tu_head.tb_magic = AOT_TB_MAGIC;
    tu_head.tb_count = tb_count;
    tu_head.first_pc = tb_vector[0]->pc;
    tu_head.tu_sum_size = sizeof(tu_head) + tb_vector[0]->s_data->tu_size;

    for (int i = 0; i < tb_count; i++) {
        TranslationBlock *tb = tb_vector[i];

        if (i && tb->s_data->is_first_tb) {
            fprintf(stderr, "[AOT] Save: nested TU entry while serializing\n");
            return false;
        }
        remove_misc_flags(tb, MF_LINK0_MASK | MF_LINK1_MASK | MF_LINK_STATUS);
        /* A permanently disabled TP site stays disabled across cache loads. */
        if (option_soft_ras && tb->inst_ptr_offset) {
            bool permanent = option_rastoptp &&
                    (qatomic_read(&tb->misc_flags) & MF_RAS_CLEAR_B0);
            if (permanent ? !latxs_ras_tp_permanent_valid(tb) :
                            !latxs_ras_close_relinkable(tb)) {
                goto out;
            }
        }
#if DEBUG_AOT
        ++out_tb_file_tb_count;
#endif
        if (tb->jmp_reset_offset[0] != TB_JMP_OFFSET_INVALID) {
            latx_tb_reset_jmp_target(tb, 0);
        }
        if (tb->jmp_reset_offset[1] != TB_JMP_OFFSET_INVALID) {
            latx_tb_reset_jmp_target(tb, 1);
        }
        if (TB_REL_START(tb) != -1) {
            tu_head.rel_count += TB_REL_END(tb) - TB_REL_START(tb) + 1;
        }
        tu_head.tu_sum_size += option_split_search ?
            tb->icount << 2 : tb->search_size;
        if (option_pushpop) {
            tu_head.tu_sum_size += tb->icount << 2;
        }
    }

    tu_head.page_count = mapping_count;
    tu_head.tu_id = aot_make_tu_id(tb_vector, tb_count, tu_pages,
            tu_head.page_count);

    tu_head.tu_sum_size += sizeof(aot_block) * tu_head.tb_count
        + sizeof(aot_rel) * tu_head.rel_count
        + sizeof(aot_tu_page) * tu_head.page_count;
    if (tu_head.rel_count) {
        saved_rels = g_try_new(aot_rel, tu_head.rel_count);
        if (!saved_rels) {
            return false;
        }
    }

    if (!aot_codec_write(file, &tu_head, sizeof(tu_head), 1) ||
            !aot_codec_write(file, tu_pages, sizeof(*tu_pages),
                            tu_head.page_count)) {
        goto out;
    }

    for (int i = 0; i < tb_count; i++) {
        TranslationBlock *tb = tb_vector[i];
        aot_block aot_tb = { 0 };

        aot_encode_block(&aot_tb, tb);
        if (TB_REL_START(tb) != -1) {
            int rel_count = TB_REL_END(tb) - TB_REL_START(tb) + 1;

            aot_tb.rel_start = saved_rel_count;
            aot_tb.rel_end = saved_rel_count + rel_count - 1;
            for (int j = 0; j < rel_count; j++) {
                saved_rels[saved_rel_count] =
                    tu_rel_table[TB_REL_START(tb) + j];
                saved_rels[saved_rel_count].tc_offset -= curr_ir2_size;
                saved_rel_count++;
            }
        } else {
            aot_tb.rel_start = -1;
            aot_tb.rel_end = -2;
        }
        if (!aot_codec_write(file, &aot_tb, sizeof(aot_tb), 1)) {
            goto out;
        }
        curr_ir2_size += tb->ir2_num * 4;
    }
    g_assert(saved_rel_count == tu_head.rel_count);

    if (!aot_codec_write(file, tb_vector[0]->tc.ptr,
                         tb_vector[0]->s_data->tu_size, 1) ||
            (tu_head.rel_count &&
             !aot_codec_write(file, saved_rels, sizeof(*saved_rels),
                             tu_head.rel_count))) {
        goto out;
    }

    for (int i = 0; i < tb_count; i++) {
        TranslationBlock *tb = tb_vector[i];

        if (!option_split_search) {
            if (!aot_codec_write(file, (void *)(ADDR)tb->tc.ptr +
                                (tb->ir2_num << 2), tb->search_size, 1)) {
                goto out;
            }
        } else if (!aot_codec_write(file, (void *)(ADDR)tb->search_ptr,
                                   tb->icount * 4, 1)) {
            goto out;
        }
    }
    if (option_pushpop) {
        for (int i = 0; i < tb_count; i++) {
            TranslationBlock *tb = tb_vector[i];

            if (!aot_codec_write(file, (void *)(ADDR)tb_opt_ptr(tb),
                                tb->icount * 4, 1)) {
                goto out;
            }
        }
    }

    *record_size += tu_head.tu_sum_size;
    if (tu_id) {
        *tu_id = tu_head.tu_id;
    }
    ok = true;

out:
    g_free(saved_rels);
    return ok;
}

#endif
