#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Resolve ordinary, cold and LLVM host PCs in TU tables larger than 1024."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]


def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


header = (root / "include/exec/translation-block.h").read_text()
sys_tu_src = (root / "target/i386/latx/optimization/sys-tu.c").read_text()
aot_load_src = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
jit_hdr_call = re.search(
    r"tu_tbmini_set_pointer\(\(uint64_t \*\)tbmini_ptr,\s*tb_num_in_tu,\s*\w+\);",
    sys_tu_src,
)[0]
aot_hdr_call = re.search(
    r"tu_tbmini_set_pointer\(\(uint64_t \*\)tbmini_ptr,\s*tb_num_in_tu,\s*\w+\);",
    aot_load_src,
)[0]
definitions = "\n".join(re.search(r"^#define " + name + r" .*", header, re.M)[0]
                          for name in ("TB_MAGIC", "TU_TBM_MAGIC", "HOST_VIRT_ADDR_SPACE_BITS",
                                       "CODE_GEN_ALIGN"))
tbmini = header[header.index("typedef struct MagicTBP {"):
                header.index("} TBMini;") + len("} TBMini;")]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#define CONFIG_LATX_TU 1
#define MAKE_64BIT_MASK(lo, width) (((UINT64_C(1) << (width)) - 1) << (lo))
#define ROUND_DOWN(value, align) ((value) & ~((uintptr_t)(align) - 1))
''' + definitions + "\n" + tbmini + r'''
#endif
typedef struct {
    void *entry, *recover;
    uint64_t entry_bytes;
} AOTNativeRecord;
typedef struct { unsigned count; AOTNativeRecord records[1]; } AOTNativeTUInfo;
typedef struct { AOTNativeTUInfo *native; } TranslationUnitPages;
typedef struct { uint64_t tu_size; uint32_t ras_tp_cold_offset, ras_tp_cold_insns; } Data;
typedef struct {
    struct { void *ptr; size_t offset_in_tu; } tc;
    unsigned ir2_num;
    Data *s_data;
    TranslationUnitPages *pages;
} TranslationBlock;
#define TB_TU_PAGES(tb) ((tb)->pages)
static uintptr_t tb_tu_identity(TranslationBlock *tb)
{ return (uintptr_t)tb->tc.ptr - tb->tc.offset_in_tu; }
static uint8_t *buffer;
static const size_t buffer_size = 4 * 1024 * 1024;
static bool in_code_gen_buffer(const void *p)
{ return (uintptr_t)p >= (uintptr_t)buffer && (uintptr_t)p - (uintptr_t)buffer < buffer_size; }
''' + function("tcg/region.c", "static void *query_tu_tbm_table(") + "\n" + function(
    "tcg/region.c", "static void *tcg_tb_lookup_fast(") + "\n" + function(
    "accel/tcg/translate-all.c", "inline void tbmini_set_pointer(").replace("inline", "static", 1) + "\n" + function(
    "target/i386/latx/optimization/sys-tu.c", "inline void\ntu_tbmini_set_pointer(").replace("inline", "static", 1) + r'''
static void write_jit_tu_header(uintptr_t tbmini_ptr, int tb_num_in_tu)
{
    ''' + jit_hdr_call + r'''
}
static void write_aot_tu_header(uintptr_t tbmini_ptr, int tb_num_in_tu)
{
    ''' + aot_hdr_call + r'''
}
static void check_table(unsigned count, bool from_aot_loader)
{
    memset(buffer, 0, buffer_size);
    TranslationBlock *members = calloc(count, sizeof(*members));
    Data *data = calloc(count, sizeof(*data));
    assert(members && data);
    TBMini *header = (void *)buffer;
    uint8_t *code = buffer + (count + 1) * sizeof(*header);
    unsigned native_offset = count * 8;
    AOTNativeTUInfo native = { .count = 1, .records = {
        { .entry = code + native_offset, .entry_bytes = 64, .recover = (void *)1 } } };
    TranslationUnitPages pages = { .native = &native };
    if (from_aot_loader) {
        write_aot_tu_header((uintptr_t)header, (int)count);
    } else {
        write_jit_tu_header((uintptr_t)header, (int)count);
    }
    for (unsigned i = 0; i < count; i++) {
        members[i].tc.ptr = code + i * 8;
        members[i].tc.offset_in_tu = i * 8;
        members[i].ir2_num = 2;
        members[i].s_data = &data[i];
        members[i].pages = &pages;
        data[i].tu_size = native_offset + 128;
        tu_tbmini_set_pointer(&(header + i + 1)->mtbp_uint64, (uintptr_t)&members[i], 8);
    }
    data[count - 1].ras_tp_cold_offset = native_offset + 64;
    data[count - 1].ras_tp_cold_insns = 5;
    assert(tcg_tb_lookup_fast((uintptr_t)code + 4) == &members[0]);
    assert(tcg_tb_lookup_fast((uintptr_t)code + (count - 1) * 8 + 4) == &members[count - 1]);
    assert(tcg_tb_lookup_fast((uintptr_t)code + native_offset + 4) == &members[0]);
    assert(tcg_tb_lookup_fast((uintptr_t)code + native_offset + 64 + 4) == &members[count - 1]);
    tu_tbmini_set_pointer(&header->mtbp_uint64, 0, TU_TBM_MAGIC);
    assert(!tcg_tb_lookup_fast((uintptr_t)code + 4));
    tu_tbmini_set_pointer(&header->mtbp_uint64, (uint64_t)INT_MAX + 1, TU_TBM_MAGIC);
    assert(!tcg_tb_lookup_fast((uintptr_t)code + 4));
    tu_tbmini_set_pointer(&header->mtbp_uint64, buffer_size / sizeof(*header), TU_TBM_MAGIC);
    assert(!tcg_tb_lookup_fast((uintptr_t)code + 4));
    free(members); free(data);
}

/* Lookup uses recorded cold ranges independently of instruction encoding,
 * skips out-of-TU metadata without rejecting normal lookup, and preserves
 * source ownership across adjacent cold fragments in the final member. */
static void check_variable_cold(unsigned words)
{
    memset(buffer, 0, buffer_size);
    TBMini *header = (void *)buffer;
    uint8_t *code = buffer + 3 * sizeof(*header);
    Data data[2] = {{0}, {0}};
    TranslationBlock members[2] = {
        { .tc = {code, 0}, .ir2_num = 4, .s_data = &data[0] },
        { .tc = {code + 16, 16}, .ir2_num = 4 + words,
          .s_data = &data[1] },
    };
    uint32_t *cold = (void *)(code + 32);
    unsigned bytes = 32 + words * 4;

    write_jit_tu_header((uintptr_t)header, 2);
    data[0].tu_size = bytes;
    data[0].ras_tp_cold_offset = 32;
    data[0].ras_tp_cold_insns = words;
    data[1].ras_tp_cold_offset = UINT32_MAX; /* no CALL */
    tu_tbmini_set_pointer(&(header + 1)->mtbp_uint64,
                         (uintptr_t)&members[0], 16);
    tu_tbmini_set_pointer(&(header + 2)->mtbp_uint64,
                         (uintptr_t)&members[1], 16 + words * 4);
    memset(cold, 0xff, words * 4);

    assert(tcg_tb_lookup_fast((uintptr_t)code + 4) == &members[0]);
    assert(tcg_tb_lookup_fast((uintptr_t)code + 20) == &members[1]);
    for (unsigned i = 0; i < words; i++) {
        assert(tcg_tb_lookup_fast((uintptr_t)&cold[i]) == &members[0]);
    }
    assert(!tcg_tb_lookup_fast((uintptr_t)code + bytes));

    data[0].ras_tp_cold_insns = 0;
    data[0].ras_tp_cold_offset = UINT32_MAX;
    assert(tcg_tb_lookup_fast((uintptr_t)code + 4) == &members[0]);
    assert(tcg_tb_lookup_fast((uintptr_t)cold) == &members[1]);

    data[0].ras_tp_cold_offset = bytes - 4;
    data[0].ras_tp_cold_insns = 3;
    assert(tcg_tb_lookup_fast((uintptr_t)code + 4) == &members[0]);
    assert(tcg_tb_lookup_fast((uintptr_t)code + 20) == &members[1]);
    assert(tcg_tb_lookup_fast((uintptr_t)code + bytes - 4) == &members[1]);
    assert(!tcg_tb_lookup_fast((uintptr_t)code + bytes));

    data[0].ras_tp_cold_offset = bytes + 16;
    data[0].ras_tp_cold_insns = 3;
    assert(tcg_tb_lookup_fast((uintptr_t)code + 4) == &members[0]);
    assert(tcg_tb_lookup_fast((uintptr_t)code + 20) == &members[1]);
    assert(!tcg_tb_lookup_fast((uintptr_t)code + bytes + 16));

    /* Two adjacent, source-owned fragments can live in the final member. */
    data[0].ras_tp_cold_offset = 32;
    data[0].ras_tp_cold_insns = words;
    data[1].ras_tp_cold_offset = bytes;
    data[1].ras_tp_cold_insns = 3;
    data[0].tu_size += 12;
    members[1].ir2_num += 3;
    tu_tbmini_set_pointer(&(header + 2)->mtbp_uint64,
                         (uintptr_t)&members[1], 16 + words * 4 + 12);
    assert(tcg_tb_lookup_fast((uintptr_t)code + bytes - 4) == &members[0]);
    assert(tcg_tb_lookup_fast((uintptr_t)code + bytes) == &members[1]);
    assert(tcg_tb_lookup_fast((uintptr_t)code + bytes + 8) == &members[1]);
    assert(!tcg_tb_lookup_fast((uintptr_t)code + bytes + 12));
}
int main(void)
{
    assert(!posix_memalign((void **)&buffer, CODE_GEN_ALIGN, buffer_size));
    unsigned counts[] = {1, 2, 1024, 1025, 1535, 2048};
    for (unsigned loader = 0; loader < 2; loader++) {
        for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
            check_table(counts[i], loader != 0);
        }
    }
    for (unsigned words = 3; words <= 7; words++) {
        check_variable_cold(words);
    }
    memset(buffer, 0, buffer_size);
    TranslationBlock ordinary = {0};
    tbmini_set_pointer((void *)buffer, (uintptr_t)&ordinary);
    assert(tcg_tb_lookup_fast((uintptr_t)buffer + CODE_GEN_ALIGN + 4) == &ordinary);
    assert(!tcg_tb_lookup_fast((uintptr_t)buffer - 1));
    free(buffer);
    puts("PASS large TU lookup: ordinary TB, final member, cold paths and LLVM recovery owner");
}
'''
with tempfile.TemporaryDirectory(prefix="lats-large-tu-lookup-") as temp:
    path = Path(temp)
    (path / "check.c").write_text(program)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    str(path / "check.c"), "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)
