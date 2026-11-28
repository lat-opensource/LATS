#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise HAMT MMIO instruction classification and ADE retry repair."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
header = (root / 'target/i386/hamt/hamt-internal.h').read_text()
source = (root / 'target/i386/hamt/hamt.c').read_text()
dune = (root / 'target/i386/hamt/dune-loop.c').read_text()
inject = (root / 'target/i386/hamt/dune-inject-ade.c').read_text()
guest = (root / 'tests/tcg/i386/latx-hamt/mmio-ade.S').read_text()
vector_guest = (
    root / 'tests/tcg/i386/latx-hamt/mmio-ade-vector.S').read_text()


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    end, depth = opening + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


softmmu = function(source, 'static void hamt_tlb_handler_softmmu(')
handler = function(source, 'void hamt_tlb_handler(')
decode = function(inject, 'static bool dune_decode_inject_ade_access(')
rewrite = function(inject, 'static bool dune_rewrite_injected_ade(')
internal = function(inject, 'static bool dune_prepare_vector_internal_ade(')
page_fault = function(source, 'static void hamt_tlb_handler_page_fault(')
recover = function(source, 'static uint64_t hamt_recover_injected_ade(')

# Injected registers and the original address must be repaired before either
# recovery engine can consume the temporary 0xab... address from dune-loop.c.
assert 'hamt_fix_injected_ade' not in softmmu
assert 'hamt_injected_ade' not in page_fault
assert 'hamt_recover_injected_ade(env, epc, cpuid)' in handler
assert 'CONFIG_LATX_DEBUG' in recover
assert 'HAMT injected ade at' in recover
recover_at = handler.index('hamt_recover_injected_ade(env, epc, cpuid)')
assert recover_at < handler.index('hamt_try_do_ptw(')
assert recover_at < handler.index('hamt_native_recover(')

# dune-loop.c owns the exit policy; dune-inject-ade.c owns injection details.
assert 'DUNE_DMW_TOP8' not in dune
assert 'hamt_mark_injected_ade' not in dune
assert 'hamt_mmio_inst_type' not in dune
assert '#define DUNE_DMW_TOP8  0x90ULL' in inject
assert 'hamt_mark_injected_ade(epc, original_rj, original_rk)' in inject
assert inject.index('KVM_SET_REGS') < inject.index(
    'hamt_mark_injected_ade(epc, original_rj, original_rk)')

# Only scalar indexed loads have a GPR index that can alias load completion.
assert ('access.type < 4 &&\n'
        '            (access.rd == access.rj ||\n'
        '             (access.type == 2 && access.rd == access.rk))') in inject
assert 'regs->gpr[access.rd]' in inject
assert 'KVM_EXIT_INTERNAL_ERROR' in dune
assert 'dune_inject_ade_from_mmio(vcpu, &regs)' in dune
assert 'dune_inject_ade_from_internal_error(vcpu, &regs)' in dune

mmio_start = dune.index('exit_reason == KVM_EXIT_MMIO')
mmio_end = dune.index('exit_reason == KVM_EXIT_INTERNAL_ERROR', mmio_start)
mmio_branch = dune[mmio_start:mmio_end]
assert 'KVM_GET_REGS' in mmio_branch
assert 'KVM_SET_REGS' not in mmio_branch

internal_start = dune.index(
    'exit_reason == KVM_EXIT_INTERNAL_ERROR')
internal_end = dune.index('dune_handle_ade_in_dmw_fail:', internal_start)
internal_branch = dune[internal_start:internal_end]
assert 'KVM_GET_REGS' in internal_branch
assert 'KVM_SET_REGS' not in internal_branch
assert 'continue;' in internal_branch
assert 'regs.pc -= 4' not in internal_branch
assert guest.count('(%r12,%r13,1)') >= 2
assert '(%r14,%r13,1)' in guest
assert '(%r12,%r12,1)' in guest
assert 'movdqu' in vector_guest
assert 'vmovdqu' in vector_guest

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline int32_t sextract32(uint32_t value, int start, int length)
{
    return (int32_t)(value << (32 - start - length)) >> (32 - length);
}

#define DUNE_DMW_TOP8  0x90ULL
#define DUNE_ADE_TOP8  0xabULL
#define DUNE_TOP8(v)   (((v) >> 56) & 0xffULL)
'''

start = header.index('typedef enum {')
end = header.index('} ld_st_inst;', start) + len('} ld_st_inst;')
parts = [prefix, header[start:end]]
access_start = inject.index('typedef struct DuneInjectAdeAccess {')
access_end = inject.index('} DuneInjectAdeAccess;', access_start)
parts.append(inject[access_start:access_end + len('} DuneInjectAdeAccess;')])
for signature in (
    'static inline int hamt_simm12(',
    'static inline bool hamt_is_ptr_mem(',
    'static inline bool hamt_is_vector_mem(',
    'static inline bool hamt_is_vector_indexed_mem(',
    'static inline int hamt_ptr_offset(',
    'static inline uint32_t hamt_indexed_mem_opcode(',
    'static inline uint32_t hamt_mem_opcode(',
):
    parts.append(function(header, signature))
parts.append(function(source, 'int hamt_mmio_inst_type('))
parts.append(decode)
parts.append(rewrite)
parts.append(internal)
parts.append(r'''
static uint32_t indexed(uint32_t opc, unsigned rd, unsigned rj, unsigned rk)
{
    return opc | rd | (rj << 5) | (rk << 10);
}

int main(void)
{
    const uint32_t scalar[] = {
        OPC_LD_B, OPC_LD_H, OPC_LD_W, OPC_LD_D,
        OPC_ST_B, OPC_ST_H, OPC_ST_W, OPC_ST_D,
        OPC_LD_BU, OPC_LD_HU, OPC_LD_WU,
    };
    const uint32_t scalar_indexed[] = {
        OPC_LDX_B, OPC_LDX_H, OPC_LDX_W, OPC_LDX_D,
        OPC_STX_B, OPC_STX_H, OPC_STX_W, OPC_STX_D,
        OPC_LDX_BU, OPC_LDX_HU, OPC_LDX_WU,
    };
    const uint32_t vector_base[] = {
        OPC_VLD, OPC_VST, OPC_XVLD, OPC_XVST,
    };
    const uint32_t vector_indexed[] = {
        OPC_VLDX, OPC_VSTX, OPC_XVLDX, OPC_XVSTX,
    };
    const uint32_t ptr[] = {
        OPC_LDPTR_W, OPC_STPTR_W, OPC_LDPTR_D, OPC_STPTR_D,
    };

    for (unsigned i = 0; i < sizeof(scalar) / sizeof(scalar[0]); ++i) {
        for (unsigned low = 0; low < 1024; ++low) {
            assert(hamt_mmio_inst_type(scalar[i] | low) == 1);
        }
    }

    for (unsigned i = 0; i < sizeof(scalar_indexed) / sizeof(scalar_indexed[0]); ++i) {
        for (unsigned rd = 0; rd < 32; ++rd) {
            for (unsigned rj = 0; rj < 32; ++rj) {
                for (unsigned rk = 0; rk < 32; ++rk) {
                    assert(hamt_mmio_inst_type(
                        indexed(scalar_indexed[i], rd, rj, rk)) == 2);
                }
            }
        }
        assert(hamt_mmio_inst_type(scalar_indexed[i] | (1u << 15)) == 0);
    }

    for (unsigned i = 0; i < sizeof(vector_base) / sizeof(vector_base[0]); ++i) {
        for (unsigned low = 0; low < 1024; ++low) {
            assert(hamt_mmio_inst_type(vector_base[i] | low) == 4);
        }
    }

    for (unsigned i = 0; i < sizeof(vector_indexed) /
                            sizeof(vector_indexed[0]); ++i) {
        for (unsigned rd = 0; rd < 32; ++rd) {
            for (unsigned rj = 0; rj < 32; ++rj) {
                for (unsigned rk = 0; rk < 32; ++rk) {
                    assert(hamt_mmio_inst_type(indexed(vector_indexed[i], rd,
                                                       rj, rk)) == 5);
                }
            }
        }
    }

    for (unsigned i = 0; i < sizeof(ptr) / sizeof(ptr[0]); ++i) {
        for (unsigned low = 0; low < 4096; ++low) {
            assert(hamt_mmio_inst_type(ptr[i] | low) == 3);
        }
    }

    for (unsigned imm = 0; imm < 4096; ++imm) {
        uint32_t inst = OPC_LD_D | (imm << 10);
        int expected = imm < 0x800 ? (int)imm : (int)imm - 0x1000;
        assert(hamt_simm12(inst) == expected);
    }

    uint64_t gpr[32] = { 0 };
    uint64_t retry;
    DuneInjectAdeAccess access;

    gpr[3] = UINT64_C(0x9000000012345000);
    assert(dune_decode_inject_ade_access(
        OPC_LD_D | (3 << 5) | (0x10 << 10), gpr, &access));
    assert(access.type == 1);
    assert(access.rd == 0);
    assert(access.rj == 3);
    assert(access.simm == 0x10);
    assert(access.addr == gpr[3] + 0x10);

    gpr[4] = UINT64_C(0x0000000000000100);
    assert(dune_decode_inject_ade_access(
        indexed(OPC_LDX_D, 7, 3, 4), gpr, &access));
    assert(access.type == 2);
    assert(access.addr == gpr[3] + gpr[4]);

    assert(dune_decode_inject_ade_access(
        indexed(OPC_XVLDX, 2, 3, 4), gpr, &access));
    assert(access.type == 5);
    assert(access.addr == gpr[3] + gpr[4]);
    assert(!dune_decode_inject_ade_access(UINT32_MAX, gpr, &access));

    uint32_t base_inst = OPC_LD_D | (5 << 5) | (0xfff << 10);
    gpr[5] = UINT64_C(0x9000000012345000);
    assert(dune_rewrite_injected_ade(base_inst, gpr, &retry));
    assert(DUNE_TOP8(gpr[5]) == DUNE_ADE_TOP8);
    assert(retry == gpr[5] - 1);
    assert(DUNE_TOP8(retry) != DUNE_DMW_TOP8);

    uint64_t original_rj = 0;
    uint64_t original_rk = 0;
    gpr[10] = UINT64_C(0x9000000000004000);
    assert(dune_prepare_vector_internal_ade(OPC_VLD | (10 << 5),
                                             gpr, &original_rj, &original_rk));
    assert(DUNE_TOP8(gpr[10]) == DUNE_ADE_TOP8);
    assert(original_rj == UINT64_C(0x9000000000004000));

    gpr[11] = UINT64_C(0x9000000000005000);
    gpr[12] = UINT64_C(0x0000000000000100);
    assert(dune_prepare_vector_internal_ade(indexed(OPC_XVLDX, 3, 11, 12),
                                             gpr, &original_rj, &original_rk));
    assert(DUNE_TOP8(gpr[11]) == DUNE_ADE_TOP8);
    assert(DUNE_TOP8(gpr[12]) == 0);
    assert(original_rj == UINT64_C(0x9000000000005000));
    assert(original_rk == UINT64_C(0x0000000000000100));

    gpr[13] = UINT64_C(0x1234000000006000);
    assert(!dune_prepare_vector_internal_ade(OPC_XVST | (13 << 5),
                                              gpr, &original_rj, &original_rk));
    assert(!dune_prepare_vector_internal_ade(OPC_LD_D | (13 << 5),
                                              gpr, &original_rj, &original_rk));

    gpr[5] = UINT64_C(0x9000000012345000);
    gpr[8] = UINT64_C(0x9000000000000123);
    assert(dune_rewrite_injected_ade(indexed(OPC_LDX_D, 8, 5, 8),
                                     gpr, &retry));
    assert(DUNE_TOP8(gpr[5]) == DUNE_ADE_TOP8);
    assert(DUNE_TOP8(gpr[8]) == 0);
    assert(retry == gpr[5] + gpr[8]);
    assert(DUNE_TOP8(retry) != DUNE_DMW_TOP8);

    gpr[6] = UINT64_C(0x9000000000001000);
    assert(dune_rewrite_injected_ade(indexed(OPC_LDX_D, 7, 6, 6),
                                     gpr, &retry));
    assert(DUNE_TOP8(gpr[6]) == DUNE_ADE_TOP8);
    assert(retry == gpr[6] + gpr[6]);
    assert(DUNE_TOP8(retry) != DUNE_DMW_TOP8);

    gpr[9] = UINT64_C(0x9000000000002000);
    assert(dune_rewrite_injected_ade(indexed(OPC_LDPTR_D, 7, 9, 0) | (7 << 10),
                                     gpr, &retry));
    assert(DUNE_TOP8(retry) != DUNE_DMW_TOP8);

    assert(!dune_rewrite_injected_ade(UINT32_MAX, gpr, &retry));
    puts("HAMT MMIO ADE: scalar/vector classification and retry passed");
}
''')

with tempfile.TemporaryDirectory(prefix='hamt-mmio-ade-') as temp:
    c = Path(temp) / 'test.c'
    binary = Path(temp) / 'test'
    c.write_text('\n'.join(parts))
    cc = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run([*cc, '-std=gnu11', '-O2', '-Wall', '-Wextra',
                    '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
