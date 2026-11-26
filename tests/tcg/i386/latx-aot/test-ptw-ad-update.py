#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Schedule PTE replacement inside the production C walkers' A/D update gap.

Run on a little-endian host with a C compiler. --revision replays the same
cases against an earlier source tree without changing the checkout. The
permitted in-flight translations do not model a completed TLB shootdown.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--revision', help='test an earlier source tree without checking it out')
args = parser.parse_args()
root = Path(__file__).resolve().parents[4]


def source(path):
    if args.revision:
        return subprocess.check_output(['git', 'show', f'{args.revision}:{path}'],
                                       cwd=root, text=True)
    return (root / path).read_text()


def function(text, name):
    start = text.index(name)
    return text[start:text.index('\n}', start) + 2]


hamt = source('target/i386/hamt/hamt-ptw.c')
mmu = source('target/i386/tcg/sysemu/excp_helper.c')
walk = function(hamt, 'static bool hamt_do_ptw(')
fast = function(mmu, 'static bool mmu_translate_fast(')
# These are scheduling hooks, not replacement implementations of the walkers.
# The other vCPU changes the PTE after the local load or just before leaf A/D.
load = 'pte = *((uint64_t *)haddr);'
assert walk.count(load) == 4
walk = walk.replace(load, load + '\n    test_interleave(haddr, false);')
walk = walk.replace('    /* yes, it can! */',
                    '    test_interleave(haddr, true);\n    /* yes, it can! */')
load = 'pte = *((uint64_t *)pte_trans.haddr);'
assert fast.count(load) == 3
fast = fast.replace(load, load + '\n    test_interleave((uintptr_t)pte_trans.haddr, false);')
fast = fast.replace('    pte = ptw_ldq(&pte_trans);',
                    '    pte = ptw_ldq(&pte_trans);\n    test_interleave((uintptr_t)pte_trans.haddr, false);')
fast = fast.replace('    /* yes, it can! */',
                    '    test_interleave((uintptr_t)pte_trans.haddr, true);\n    /* yes, it can! */')

program = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_LATX_HACK_MEM 1
#define likely(x) (x)
#define cpu_to_le32(x) ((uint32_t)(x))
#define qatomic_cmpxchg(p,o,n) __extension__ ({ __typeof__(*(p)) expected=(o); \
    __atomic_compare_exchange_n((p), &expected, (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); expected; })
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "HAMT host must be little endian");
typedef uint64_t hwaddr;
typedef uint64_t target_ulong;
typedef enum { MMU_DATA_LOAD, MMU_DATA_STORE, MMU_INST_FETCH } MMUAccessType;
typedef struct { uint64_t cr[5]; uint8_t *ptw4k; uint32_t hamt_large_page_bits; } CPUX86State;
typedef struct { int error, error_code; uint64_t paddr; } hamt_ptw_error_t;
typedef struct { int exception_index, error_code; uint64_t cr2; } TranslateFault;
typedef struct { uint64_t addr, cr3; MMUAccessType access_type; int ptw_idx, mmu_idx; } TranslateParams;
typedef struct { uint64_t paddr; int prot, page_size; } TranslateResult;
typedef struct { CPUX86State *env; TranslateFault *err; int ptw_idx; void *haddr; } PTETranslate;
enum { HAMT_PTW_ERROR_NONE, HAMT_PTW_ERROR_PF, HAMT_PTW_ERROR_MMIO,
       HAMT_PTW_ERROR_SMC, HAMT_PTW_ERROR_VGA, HAMT_PTW_ERROR_NOTRAM };
#define MMU_USER_IDX 1
#define MMU_KSMAP_IDX 0
#define EXCP0E_PAGE 14
#define PG_PRESENT_MASK 1
#define PG_RW_MASK 2
#define PG_USER_MASK 4
#define PG_ACCESSED_MASK 0x20
#define PG_DIRTY_MASK 0x40
#define PG_PSE_MASK 0x80
#define PG_PSE_PAT_MASK 0x1000
#define PG_NX_MASK (UINT64_C(1) << 63)
#define PG_ADDRESS_MASK UINT64_C(0x000ffffffffff000)
#define PG_ERROR_P_MASK 1
#define PG_ERROR_W_MASK 2
#define PG_ERROR_U_MASK 4
#define PG_ERROR_RSVD_MASK 8
#define TARGET_PAGE_SIZE 4096
#define HAMT_LARGE_PAGE_SHIFT 21
#define PAGE_READ 1
#define PAGE_WRITE 2
#define PAGE_EXEC 4
static _Alignas(4096) unsigned char ram[6 * 4096];
static uint8_t page_flags[0x240000];
#define HACK_RAMBASE ((uintptr_t)ram)
static uint32_t hamt_large_page_bit(uint64_t a) { return 1u << ((a >> 21) & 31); }
static bool latx_hack_gpa_in_ram(uint64_t a) { return a < UINT64_C(0x240000000); }
static uint8_t *hamt_get_ptwdata(void) { return page_flags; }
static bool hamt_no_vga_dirty(void) { return true; }
static uint64_t installed_gpa;
static void hamt_set_hardware_tlb_nocheck(CPUX86State *e, uint64_t va,
                                         uint64_t pa, int prot, int mode) {
    installed_gpa = pa - HACK_RAMBASE;
}
static uint64_t ptw_ldq(const PTETranslate *p) { return *(uint64_t *)p->haddr; }
static bool ptw_setl_slow(const PTETranslate *p, uint32_t o, uint32_t n) { abort(); }
static uintptr_t hook_address;
static uint64_t replacement;
static bool hook_at_leaf, inject;
static unsigned hook_calls;
static void test_interleave(uint64_t address, bool final) {
    if (inject && address == hook_address && final == hook_at_leaf && !hook_calls++) {
        __atomic_store_n((uint64_t *)(uintptr_t)address, replacement, __ATOMIC_SEQ_CST);
    }
}
'''
if 'static bool hamt_ptw_set_ad(' in hamt:
    program += function(hamt, 'static bool hamt_ptw_set_ad(') + '\n'
program += function(mmu, 'static inline bool ptw_setl(') + '\n' + walk + '\n' + fast + '\n'

program += r'''
static unsigned check(int engine, const char *name, unsigned level, bool late,
                      uint64_t initial, bool race, uint64_t next, int access,
                      uint64_t expected, int expected_ok, uint64_t expected_gpa)
{
    memset(ram, 0, sizeof(ram));
    *(uint64_t *)(ram + 0x0000) = 0x1007;
    *(uint64_t *)(ram + 0x1000) = 0x2007;
    *(uint64_t *)(ram + 0x2000) = 0x3007;
    *(uint64_t *)(ram + 0x3020) = 0x4007;
    unsigned offset = level == 1 ? 0x3020 : (4 - level) * 4096;
    uint64_t *pte = (uint64_t *)(ram + offset);
    *pte = initial;
    hook_address = (uintptr_t)pte;
    hook_at_leaf = late;
    inject = race;
    replacement = next;
    hook_calls = 0;
    installed_gpa = UINT64_MAX;
    CPUX86State env = { .ptw4k = page_flags };
    bool ok;
    if (engine == 0) {
        hamt_ptw_error_t error = {0};
        ok = hamt_do_ptw(&env, 0x4000, NULL, MMU_USER_IDX, access, &error);
    } else {
        TranslateParams in = { .addr = 0x4000, .access_type = access, .mmu_idx = MMU_USER_IDX };
        TranslateResult out = {0};
        TranslateFault error = {0};
        ok = mmu_translate_fast(&env, &in, &out, &error);
        if (ok) installed_gpa = out.paddr;
    }
    bool pass = *pte == expected && (!race || hook_calls != 0);
    if (expected_ok >= 0) pass &= ok == expected_ok;
    if (expected_ok == 1) pass &= installed_gpa == expected_gpa;
    printf("%s %s %s pte=%#" PRIx64 " expected=%#" PRIx64 " ok=%d gpa=%#" PRIx64 "\n",
           engine ? "softmmu" : "hamt", name, pass ? "PASS" : "FAIL",
           *pte, expected, ok, installed_gpa);
    return !pass;
}
int main(void)
{
    unsigned failures = 0;
    for (int e = 0; e < 2; e++) {
        failures += check(e,"read",1,false,0x4007,false,0,MMU_DATA_LOAD,0x4027,1,0x4000);
        failures += check(e,"write",1,false,0x4007,false,0,MMU_DATA_STORE,0x4067,1,0x4000);
        failures += check(e,"unmap-leaf",1,false,0x4007,true,0,MMU_DATA_LOAD,0,0,0);
        failures += check(e,"replace-leaf",1,false,0x4007,true,0x5007,MMU_DATA_LOAD,0x5027,1,0x5000);
        failures += check(e,"revoke-write",1,false,0x4007,true,0x4005,MMU_DATA_STORE,0x4005,0,0);
        failures += check(e,"keep-other-dirty",1,false,0x4007,true,0x4047,MMU_DATA_LOAD,0x4067,1,0x4000);
        for (unsigned level = 2; level <= 4; level++) {
            char name[32];
            snprintf(name, sizeof(name), "unmap-parent-L%u", level);
            failures += check(e, name, level, false, (5-level)*4096+7,
                              true, 0, MMU_DATA_LOAD, 0, 0, 0);
        }
        /* Present is software-owned. A/D already being set does not permit
         * a stale write after the other CPU installs a transition entry.
         * An in-flight translation before the subsequent shootdown is allowed;
         * resurrecting the in-memory PTE is not. Keep the PFN above 4 GiB. */
        failures += check(e,"transition-high-pfn",1,false,UINT64_C(0x0a00000221596867),
                          true,UINT64_C(0x0a00000221596882),MMU_DATA_LOAD,
                          UINT64_C(0x0a00000221596882),-1,0);
        failures += check(e,"accessed-unmap",1,false,0x4067,true,0,MMU_DATA_LOAD,0,-1,0);
        /* Retry must discard reserved-bit constraints from the old large page. */
        failures += check(e,"large-to-small",2,true,0x200087,true,0x3007,
                          MMU_DATA_STORE,0x3027,1,0x4000);
    }

    return failures ? 1 : 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(program)
    subprocess.run([*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-O2',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
