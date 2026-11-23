#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise production cold-length emission, reset and both AOT load paths."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]


def function(path, signature):
    text = (root / path).read_text()
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 2]


header = (root / "target/i386/latx/include/latx-ras.h").read_text()
start = header.index("#ifdef CONFIG_LATX_TU\n#define LATXS_RAS_TP_COLD_OFFSET")
macros = header[start:header.index("\n#endif", start) + len("\n#endif")]
macros += "\n" + re.search(r"^#define LATXS_RAS_TP_COLD_SIZE.*", header, re.M)[0]
load = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
loads = re.findall(r"curr_tb->inst_ptr_offset = aot_tb->inst_ptr_offset;\n"
                   r"\s*LATXS_RAS_TP_COLD_OFFSET\(curr_tb\) = 0;.*?"
                   r"(?=\s*curr_tb->label_pos_nocheck)", load, re.S)
assert len(loads) == 2

program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define g_assert assert
#define QEMU_BUILD_BUG_ON(cond) _Static_assert(!(cond), #cond)
#define LATX_RAS_TP_CALL_COLD_INSNS 5
#define LOAD_TB_FULL_ADDR 1
typedef int IR2_OPND;
enum { LISA_BL, LISA_PCADDU18I, LISA_JIRL, LISA_B };
typedef struct { uint32_t ras_tp_cold_offset, ras_tp_cold_insns; } Side;
typedef struct {
    int inst_ptr_offset;
    uint32_t ras_tp_cold_offset, ras_tp_cold_insns;
    Side *s_data;
    struct { void *ptr; } tc;
    bool tu;
} TranslationBlock;
typedef struct {
    TranslationBlock *curr_tb;
    void *curr_ir1_inst;
    unsigned ir2_asm_nr;
    bool ras_tp_has_call;
    IR2_OPND ras_tp_call_miss, ras_tp_call_done;
} TRANSLATION_DATA;
static TRANSLATION_DATA data;
static struct { TRANSLATION_DATA *tr_data; } state = { &data }, *lsenv = &state;
static const IR2_OPND la_a0 = 4, la_ra = 1;
static bool option_rastoptp = true, option_aot_save, option_large_code_cache = true;
static unsigned load_words;
static uintptr_t repair;
#define tb_is_tu(tb) ((tb)->tu)
#define GET_SC_TABLE_OFFSET(...) repair
#define get_current_inst_addr(tb) ((uintptr_t)(tb)->tc.ptr + data.ir2_asm_nr * 4)
''' + macros + r'''
static bool int32_in_int26(int32_t x) { return x >= -(1 << 25) && x < (1 << 25); }
static void la_label(IR2_OPND p) { (void)p; }
static void latxs_load_addr_to_ir2(const IR2_OPND *p, uintptr_t address, int kind)
{ (void)p; (void)address; (void)kind; data.ir2_asm_nr += load_words; }
static void latxs_append_ir2_opnd1i(int op, const IR2_OPND *rd, int64_t imm)
{ (void)op; (void)rd; (void)imm; data.ir2_asm_nr++; }
static void la_out2i(int op, const IR2_OPND *rd, const IR2_OPND *rj, int64_t imm)
{ (void)op; (void)rd; (void)rj; (void)imm; data.ir2_asm_nr++; }
static void la_outa(int op, int64_t imm)
{ (void)op; (void)imm; data.ir2_asm_nr++; }
static void la_out1(int op, const IR2_OPND *label)
{ (void)op; (void)label; data.ir2_asm_nr++; }
#define tb_sc_table_offset_jmp(...) (data.ir2_asm_nr++)
''' + function("target/i386/latx/translator/sys-translate.c",
               "void latxs_tr_tb_init(") + "\n" + function(
    "target/i386/latx/optimization/sys-ras.c",
    "void latxs_ras_tp_emit_call_cold(") + "\n"
for i, body in enumerate(loads):
    program += f"static void load_{i}(TranslationBlock *curr_tb, const TranslationBlock *aot_tb)\n{{\n{body}\n}}\n"
program += r'''
int main(void)
{
    Side side = {0};
    TranslationBlock tb = { .tc = { .ptr = (void *)(uintptr_t)0x1000000000ULL }, .s_data = &side };
    for (int tu = 0; tu < 2; tu++) {
        for (unsigned words = 1; words <= 4; words++) {
            for (int far = 0; far < 2; far++) {
                tb.tu = tu;
                LATXS_RAS_TP_COLD_OFFSET(&tb) = 99;
                LATXS_RAS_TP_COLD_INSNS(&tb) = 7;
                latxs_tr_tb_init(&data, &tb);
                assert(!LATXS_RAS_TP_COLD_OFFSET(&tb) && LATXS_RAS_TP_COLD_SIZE(&tb) == 0);
                data.ir2_asm_nr = 16;
                data.ras_tp_has_call = true;
                load_words = words;
                repair = (uintptr_t)tb.tc.ptr + (16 + words) * 4 +
                         (far ? (UINT64_C(1) << 28) : 64);
                latxs_ras_tp_emit_call_cold();
                assert(LATXS_RAS_TP_COLD_SIZE(&tb) == (words + (far ? 2 : 1) + 1) * 4);
#ifdef CONFIG_LATX_TU
                assert(LATXS_RAS_TP_COLD_OFFSET(&tb) == (tu ? 0 : 64));
#else
                assert(LATXS_RAS_TP_COLD_OFFSET(&tb) == 64);
#endif
            }
        }
    }
    latxs_tr_tb_init(&data, &tb);
    data.ir2_asm_nr = 16;
    data.ras_tp_has_call = false;
    latxs_ras_tp_emit_call_cold();
    assert(data.ir2_asm_nr == 16 && LATXS_RAS_TP_COLD_SIZE(&tb) == 0);
    data.ras_tp_has_call = true;
    option_rastoptp = false;
    latxs_ras_tp_emit_call_cold();
    assert(data.ir2_asm_nr == 16 && LATXS_RAS_TP_COLD_SIZE(&tb) == 0);
    option_rastoptp = true;
    option_aot_save = true;
    load_words = 3;
    latxs_ras_tp_emit_call_cold();
    assert(LATXS_RAS_TP_COLD_SIZE(&tb) == 20);

    TranslationBlock saved = { .inst_ptr_offset = 16, .ras_tp_cold_offset = 44 };
    void (*loaders[])(TranslationBlock *, const TranslationBlock *) = { load_0, load_1 };
    for (unsigned i = 0; i < 2; i++) {
        for (int enabled = 0; enabled < 2; enabled++) {
            for (int has_call = 0; has_call < 2; has_call++) {
                option_rastoptp = enabled;
                saved.inst_ptr_offset = has_call ? 16 : 0;
                LATXS_RAS_TP_COLD_OFFSET(&tb) = 99;
                LATXS_RAS_TP_COLD_INSNS(&tb) = 7;
                loaders[i](&tb, &saved);
                assert(LATXS_RAS_TP_COLD_OFFSET(&tb) == (enabled && has_call ? 44 : 0));
                assert(LATXS_RAS_TP_COLD_SIZE(&tb) == (enabled && has_call ? 20 : 0));
            }
        }
    }
    puts("PASS: JIT 3-7 words, TU/ordinary reset, AOT five words and both loaders");
}
'''

layout_header = (root / "include/exec/translation-block.h").read_text()
start = layout_header.index("struct separated_data{")
side = layout_header[start:layout_header.index("\n};", start) + 3]
layout_program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct TranslationBlock TranslationBlock;
typedef struct TranslationUnitPages TranslationUnitPages;
''' + side + r'''
int main(void)
{
#ifdef CONFIG_LATX_TU
    _Static_assert(sizeof(struct separated_data) == 128, "sidecar remains 128 bytes");
    _Static_assert(offsetof(struct separated_data, tu_pages) == 64, "page descriptor offset");
    _Static_assert(offsetof(struct separated_data, label_intexit) == 84, "IRQ label offset");
    _Static_assert(offsetof(struct separated_data, last_ir1_type) == 125, "tail offset");
    for (unsigned flags = 0; flags < 32; flags++) {
        for (unsigned words = 0; words < 8; words++) {
            struct separated_data sd = {0};
            sd.sys_eob_can_link = flags & 1;
            sd.sys_eob_is_indir = (flags >> 1) & 1;
            sd.sys_eob_use_fsht = (flags >> 2) & 1;
            sd.has_hras_check_gate = (flags >> 3) & 1;
            sd.ignore_eip_update = (flags >> 4) & 1;
            sd.ras_tp_cold_insns = words;
            unsigned char value = ((unsigned char *)&sd)[126];
            assert((value & 31) == flags && (value >> 5) == words);
            sd.is_invalidate = true;
            assert(((unsigned char *)&sd)[126] == value);
        }
    }
#else
    _Static_assert(sizeof(struct separated_data) == 72, "non-TU sidecar unchanged");
#endif
}
'''

with tempfile.TemporaryDirectory(prefix="ras-tp-cold-size-") as directory:
    binary = Path(directory) / "test"
    for tu in (False, True):
        flags = ["-DCONFIG_LATX_TU"] if tu else []
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", *flags,
                        "-x", "c", "-o", str(binary), "-"],
                       input=program, text=True, check=True)
        subprocess.run([str(binary)], check=True)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", *flags,
                        "-x", "c", "-o", str(binary), "-"],
                       input=layout_program, text=True, check=True)
        subprocess.run([str(binary)], check=True)
