#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the actual ordinary-fragment packer and its on-disk offsets."""
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-tu-codec.c").read_text()
body = source[source.index("static uint64_t aot_tu_id_mix("):
              source.index("bool aot_encode_tu(")]
program = r'''
#include <glib.h>
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lats/aot-format.h"
#include "latx-ras-config.h"
#define TARGET_PAGE_MASK (~UINT64_C(4095))
#define TB_JMP_OFFSET_INVALID 0xffff
#define MAX_TB_IN_TS 1024
typedef uint64_t target_ulong;
typedef struct TranslationBlock {
    uint64_t pc;
    uint32_t cs_base, flags, cflags;
    unsigned size, icount;
} TranslationBlock;
static bool option_split_search, option_pushpop, option_rastoptp;
''' + body + r'''
static GBytes *fragment(bool root)
{
    unsigned np = root ? 1 : 2, ns = option_split_search ? 16 : 10;
    size_t size = sizeof(aot_tu_head) + np * sizeof(aot_tu_page) +
                  2 * sizeof(aot_block) + 64 + 2 * sizeof(aot_rel) + ns +
                  (option_pushpop ? 16 : 0);
    uint8_t *data = g_malloc0(size);
    aot_tu_head *h = (void *)data;
    *h = (aot_tu_head) { .tb_magic = AOT_TB_MAGIC, .tu_sum_size = size,
        .tb_count = 2, .rel_count = 2, .first_pc = root ? 0x420100 : 0x400100,
        .tu_id = root ? 11 : 22, .page_count = np };
    aot_tu_page *pages = (void *)(h + 1);
    pages[0] = (aot_tu_page) {0, root ? 111 : 222};
    if (!root) pages[1] = (aot_tu_page) {0x20000, 111};
    aot_block *b = (void *)(pages + np);
    for (unsigned i = 0; i < 2; i++) {
        b[i].pc = i ? 0x420105 : h->first_pc;
        b[i].page_addr[0] = b[i].pc & TARGET_PAGE_MASK;
        b[i].page_addr[1] = UINT64_MAX;
        b[i].tc.offset_in_tu = i * 32;
        b[i].flags = 0x123; b[i].cflags = 0x456;
        b[i].size = i ? 1 : 5; b[i].icount = 2;
        b[i].ir2_num = 8; b[i].search_size = option_split_search ? 8 : 5;
        b[i].rel_start = b[i].rel_end = i;
        b[i].tu_jmp[0] = i ? 32 : 0;
        b[i].tu_jmp[1] = TB_JMP_OFFSET_INVALID;
        b[i].label_pos_nocheck = 3;
        b[i].jmp_insn_offset[0] = 8; b[i].jmp_reset_offset[0] = 12;
        b[i].tu_generation_offset_patch[0] = 24;
        b[i].tu_generation_offset_patch[1] = TB_JMP_OFFSET_INVALID;
        b[i].jmp_cp_data[0] = 0xa17;
    }
    b[0].tu_size = 64; b[0].inst_ptr_offset = 16;
    b[0].ras_tp_cold_offset = option_rastoptp ? 40 : 0;
    uint8_t *code = (void *)(b + 2);
    memset(code, root ? 0x51 : 0xa7, 64);
    aot_rel *rels = (void *)(code + 64);
    rels[0] = (aot_rel) { .kind = LOAD_GUEST_ADDR, .tc_offset = 4,
        .rel_slots_num = 3, .extra_addend = 17, .next_entry_id = -1 };
    rels[1] = (aot_rel) { .kind = HRAS_TU_CP_CHECK_PTR, .tc_offset = 24,
        .rel_slots_num = 1, .extra_addend = 29, .next_entry_id = 0 };
    memset(rels + 2, root ? 0x11 : 0x22, ns);
    if (option_pushpop) memset((uint8_t *)(rels + 2) + ns, root ? 0x33 : 0x44, 16);
    return g_bytes_new_take(data, size);
}

static GBytes *replace(GPtrArray *fragments, unsigned index)
{
    GBytes *old = g_ptr_array_index(fragments, index);
    gsize size;
    const void *data = g_bytes_get_data(old, &size);
    GBytes *copy = g_bytes_new_take(g_memdup2(data, size), size);
    fragments->pdata[index] = copy;
    return old;
}

static uint64_t independent_id(const aot_tu_head *h, const aot_tu_page *p,
                               const aot_block *b)
{
    /* Same published format, calculated independently of the codec helpers. */
    uint64_t hash = UINT64_C(1469598103934665603);
    uint64_t values[] = {h->first_pc & 4095, h->page_count,
        (uint64_t)p[0].pc_delta, p[0].page_fingerprint,
        (uint64_t)p[1].pc_delta, p[1].page_fingerprint, h->tb_count};
    for (unsigned i = 0; i < G_N_ELEMENTS(values); i++)
        hash = (hash ^ values[i]) * UINT64_C(1099511628211);
    for (unsigned i = 0; i < h->tb_count; i++) {
        uint64_t row[] = {b[i].pc - (h->first_pc & TARGET_PAGE_MASK), b[i].cs_base,
            b[i].flags, b[i].cflags, b[i].size, b[i].icount};
        for (unsigned j = 0; j < G_N_ELEMENTS(row); j++)
            hash = (hash ^ row[j]) * UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}

static void check_pack(void)
{
    GPtrArray *fragments = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
    /* The fragment before the entry appears first; both copies of RET survive. */
    g_ptr_array_add(fragments, fragment(false));
    g_ptr_array_add(fragments, fragment(true));
    GBytes *packed = aot_pack_function_tus(fragments, 0x420100);
    assert(packed);
    gsize size;
    const aot_tu_head *h = g_bytes_get_data(packed, &size);
    const aot_tu_page *pages = (const void *)(h + 1);
    const aot_block *b = (const void *)(pages + h->page_count);
    assert(h->tu_sum_size == size && h->tb_count == 4 && h->rel_count == 4);
    assert(h->page_count == 2 && pages[0].pc_delta == -0x20000 &&
           pages[0].page_fingerprint == 222 && pages[1].pc_delta == 0);
    assert(b[0].pc == 0x420100 && b[1].pc == 0x420105 &&
           b[2].pc == 0x400100 && b[3].pc == 0x420105);
    assert(b[0].tu_size == 128 && !b[1].tu_size && !b[2].tu_size && !b[3].tu_size);
    assert(b[2].tc.offset_in_tu == 64 && b[3].tc.offset_in_tu == 96);
    assert(b[2].inst_ptr_offset == 80 && b[3].inst_ptr_offset == 0);
    assert(b[2].ras_tp_cold_offset == (option_rastoptp ? 104 : 0));
    assert(b[2].tu_jmp[0] == 64 && b[3].tu_jmp[0] == 96 && b[2].tu_jmp[1] == 0xffff);
    assert(b[2].rel_start == 2 && b[3].rel_end == 3);
    assert(b[2].label_pos_nocheck == 3 && b[2].jmp_insn_offset[0] == 8 &&
           b[2].jmp_reset_offset[0] == 12 && b[2].jmp_cp_data[0] == 0xa17 &&
           b[2].tu_generation_offset_patch[0] == 24 &&
           b[2].tu_generation_offset_patch[1] == 0xffff);
    const uint8_t *code = (const void *)(b + 4);
    for (unsigned i = 0; i < 128; i++) assert(code[i] == (i < 64 ? 0x51 : 0xa7));
    const aot_rel *rels = (const void *)(code + 128);
    assert(rels[2].tc_offset == 4 && rels[2].extra_addend == 17 && rels[2].next_entry_id == -1);
    assert(rels[3].tc_offset == 24 && rels[3].extra_addend == 29 && rels[3].next_entry_id == 0);
    unsigned ns = option_split_search ? 16 : 10;
    const uint8_t *search = (const void *)(rels + 4);
    for (unsigned i = 0; i < 2 * ns; i++) assert(search[i] == (i < ns ? 0x11 : 0x22));
    if (option_pushpop)
        for (unsigned i = 0; i < 32; i++) assert(search[2 * ns + i] == (i < 16 ? 0x33 : 0x44));
    assert(h->tu_id == independent_id(h, pages, b));
    TranslationBlock tb[4], *vector[4];
    for (unsigned i = 0; i < 4; i++) {
        tb[i] = (TranslationBlock) {b[i].pc, b[i].cs_base, b[i].flags,
                                   b[i].cflags, b[i].size, b[i].icount};
        vector[i] = &tb[i];
    }
    assert(aot_make_tu_id(vector, 4, pages, 2) == h->tu_id);
    g_bytes_unref(packed);
    assert(!aot_pack_function_tus(fragments, 0x800100));
    if (option_rastoptp) {
        const unsigned cold_offsets[] = {40, 44, 48, 64, 68};
        for (unsigned i = 0; i < G_N_ELEMENTS(cold_offsets); i++) {
            GBytes *old = replace(fragments, 0);
            aot_tu_head *head = (void *)g_bytes_get_data(fragments->pdata[0], NULL);
            aot_block *blocks = (void *)((aot_tu_page *)(head + 1) + head->page_count);
            blocks[0].ras_tp_cold_offset = cold_offsets[i];
            GBytes *candidate = aot_pack_function_tus(fragments, 0x420100);
            /* Appending the second fragment must not conceal truncation. */
            assert((candidate != NULL) == (cold_offsets[i] <= 44));
            if (candidate) g_bytes_unref(candidate);
            g_bytes_unref(fragments->pdata[0]); fragments->pdata[0] = old;
        }
    }
    for (unsigned test = 0; test < 7; test++) {
        GBytes *old = replace(fragments, 0);
        gsize bytes;
        aot_tu_head *bad = (void *)g_bytes_get_data(fragments->pdata[0], &bytes);
        aot_tu_page *deps = (void *)(bad + 1);
        aot_block *blocks = (void *)(deps + bad->page_count);
        if (test == 0) deps[1].page_fingerprint++;
        if (test == 1) blocks[0].flags++;
        if (test == 2) blocks[0].inst_ptr_offset = INT_MAX;
        if (test == 3) blocks[0].tu_jmp[0] = -4;
        if (test == 4) blocks[0].rel_end = 2;
        if (test == 5) ((aot_rel *)((uint8_t *)(blocks + 2) + 64))[0].kind = AOT_NATIVE_CORE;
        if (test == 6) bad->tu_sum_size--;
        assert(!aot_pack_function_tus(fragments, 0x420100));
        g_bytes_unref(fragments->pdata[0]); fragments->pdata[0] = old;
    }
    g_ptr_array_unref(fragments);
}
static void check_large_member_count(void)
{
    GPtrArray *fragments = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
    g_ptr_array_add(fragments, fragment(true));
    for (unsigned i = 0; i < MAX_TB_IN_TS / 2 - 1; i++)
        g_ptr_array_add(fragments, fragment(false));
    GBytes *packed = aot_pack_function_tus(fragments, 0x420100);
    assert(packed);
    const aot_tu_head *head = g_bytes_get_data(packed, NULL);
    assert(head->tb_count == MAX_TB_IN_TS);
    g_bytes_unref(packed);
    while (fragments->len < 768)
        g_ptr_array_add(fragments, fragment(false));
    packed = aot_pack_function_tus(fragments, 0x420100);
    assert(packed);
    head = g_bytes_get_data(packed, NULL);
    assert(head->tb_count == 1536);
    g_bytes_unref(packed);
    g_ptr_array_unref(fragments);
}
int main(void)
{
    for (unsigned mode = 0; mode < 8; mode++) {
        option_split_search = mode & 1;
        option_pushpop = mode & 2;
        option_rastoptp = mode & 4;
        check_pack();
    }
    check_large_member_count();
    puts("PASS function source packing: code, offsets, duplicate RETs, arrays and identities");
}
'''
with tempfile.TemporaryDirectory(prefix="lats-function-source-") as temp:
    path = Path(temp)
    (path / "check.c").write_text(program)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-DLATS_AOT_DISK_TU", "-I", str(root / "include"),
                    "-I", str(root / "target/i386/latx/include"),
                    str(path / "check.c"), "-o", str(path / "check"), *flags], check=True)
    subprocess.run([str(path / "check")], check=True)
