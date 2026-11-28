#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run production HAMT PTR decoders over every si14/register encoding."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
header = (root / 'target/i386/hamt/hamt-internal.h').read_text()
source = (root / 'target/i386/hamt/hamt.c').read_text()
inject = (root / 'target/i386/hamt/dune-inject-ade.c').read_text()

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

# Both injected-ADE paths must use the same scaled displacement decoder.
assert 'simm = hamt_ptr_offset(inst);' in function(source, 'static uint64_t hamt_fix_injected_ade(')
assert 'access->simm = hamt_ptr_offset(inst);' in function(
    inject, 'static bool dune_decode_inject_ade_access(')
for name in ('hamt_get_st_val_softmmu', 'load_into_reg_softmmu'):
    start = source.index(name + '(')
    assert 'get_opc_from_epc(epc)' in source[start:source.index('\n}', start)]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
static inline int32_t sextract32(uint32_t value, int start, int length)
{
    return (int32_t)(value << (32 - start - length)) >> (32 - length);
}
typedef unsigned MemOpIdx;
enum {MO_SB=1,MO_UB,MO_LESW,MO_LEUW,MO_LESL,MO_LEUL,MO_LEUQ,MO_128};
static MemOpIdx make_memop_idx(unsigned op,unsigned idx) {return op|(idx<<8);}
#define dune_info(...) ((void)0)
#define dune_die(...) abort()
static uint64_t registers[32];
static uint64_t hamt_data_area[1];
'''
start = header.index('typedef enum {')
end = header.index('} ld_st_inst;', start) + len('} ld_st_inst;')
parts = [prefix, header[start:end]]
for name in ('hamt_is_ptr_mem', 'hamt_ptr_offset', 'hamt_indexed_mem_opcode', 'hamt_mem_opcode'):
    parts.append(function(header, 'static inline ' + ('bool' if name == 'hamt_is_ptr_mem' else 'int' if name == 'hamt_ptr_offset' else 'uint32_t') + ' ' + name + '('))
parts.append(function(header, 'static inline int hamt_simm12('))
for signature in ('static inline uint64_t hamt_get_mem_address(', 'static inline uint32_t get_opc_from_epc(', 'static inline int is_atomic_inst(', 'static inline int is_write_inst(', 'static MemOpIdx hamt_get_oi('):
    parts.append(function(source, signature))
parts.append(r'''
int main(void) {
    const uint32_t ptr[]={OPC_LDPTR_W,OPC_STPTR_W,OPC_LDPTR_D,OPC_STPTR_D};
    const uint32_t canonical[]={OPC_LD_W,OPC_ST_W,OPC_LD_D,OPC_ST_D};
    const unsigned ops[]={MO_LESL,MO_LEUL,MO_LEUQ,MO_LEUQ};
    hamt_data_area[0]=(uintptr_t)registers;
    for(unsigned kind=0;kind<4;++kind)
        for(unsigned imm=0;imm<16384;++imm)
            for(unsigned reg=0;reg<32;++reg) {
                uint32_t inst=ptr[kind]|(imm<<10)|(reg<<5)|21;
                int expected=(imm<8192?(int)imm:(int)imm-16384)*4;
                registers[reg]=UINT64_C(0x1234567800000000)+reg*1000;
                assert(hamt_is_ptr_mem(inst));
                assert(hamt_ptr_offset(inst)==expected);
                assert(get_opc_from_epc(&inst)==canonical[kind]);
                assert(!is_atomic_inst(&inst));
                assert(is_write_inst(&inst)==(int)(kind&1));
                assert(hamt_get_oi(&inst,3)==make_memop_idx(ops[kind],3));
                assert(hamt_get_mem_address(&inst,0)==registers[reg]+expected);
            }
    uint32_t inst=0x27000075; /* Startup failure: STPTR.D r21,r3,0. */
    assert(is_write_inst(&inst)==1);
    assert(hamt_ptr_offset(inst)==0);
    assert(!hamt_is_ptr_mem(OPC_ST_D));
    assert(hamt_mem_opcode(OPC_ST_D)==OPC_ST_D);
    puts("HAMT PTR: 2097152 encodings passed");
}
''')
with tempfile.TemporaryDirectory(prefix='hamt-ptr-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    c.write_text('\n'.join(parts))
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
