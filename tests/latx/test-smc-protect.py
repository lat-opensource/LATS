#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise production protection and TLB constructors with an MMU fixture.

The fixture replaces memory-region lookup, locking and address comparison.
The production functions choose when to scan and which write flags to publish.
Usage: python3 tests/latx/test-smc-protect.py [source-root]
"""
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
source = (root / 'accel/tcg/cputlb.c').read_text()

def function(name):
    match = re.search(r'(?:static )?void ' + name + r'\([^;]*?\)\s*\{', source)
    if not match:
        raise RuntimeError('missing production function: ' + name)
    start, end, depth = match.start(), match.end(), 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

support = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_LATX_HACK_MEM 1
#define CONFIG_VICTIM_TLB 1
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE 4096
#define TARGET_PAGE_MASK (~(uint64_t)4095)
#define HACK_RAMBASE 0x100000000ULL
#define PAGE_READ 1
#define PAGE_WRITE 2
#define PAGE_EXEC 4
#define PAGE_WRITE_INV 8
#define TLB_NOTDIRTY 16
#define TLB_DISCARD_WRITE 32
#define TLB_MMIO 64
#define TLB_INVALID_MASK 128
#define TLB_BSWAP 256
#define TLB_WATCHPOINT 512
#define BP_MEM_READ 1
#define BP_MEM_WRITE 2
#define MMU_INST_FETCH 0
#define MMU_DATA_LOAD 1
#define MMU_DATA_STORE 2
#define CPU_VTLB_SIZE 4
#define DIRTY_MEMORY_CODE 1
#define HAMT_PTWFLAGS_WRITE_MASK 0x7f
#define HAMT_PROT_DEL_PLV(prot) ((void)(prot))
#define tlb_debug(...) ((void)0)
typedef uint64_t ram_addr_t, vaddr, hwaddr;
typedef struct { bool byte_swap; } MemTxAttrs;
typedef struct {
    hwaddr phys_addr, xlat_section;
    MemTxAttrs attrs;
    int prot, prot32, lg_page_size;
} CPUTLBEntryFull;
typedef struct { uintptr_t addend; uint64_t addr[3]; } CPUTLBEntry;
typedef struct {
    CPUTLBEntryFull fulltlb[4], vfulltlb[4];
    CPUTLBEntry vtable[4];
    unsigned vindex;
} CPUTLBDesc;
typedef struct {
    CPUTLBDesc d[2];
    struct { unsigned dirty; int lock; } c;
    CPUTLBEntry table[2][4];
} CPUTLB;
typedef struct { CPUTLB tlb; } CPUArchState;
typedef struct { CPUArchState *env_ptr; } CPUState;
static CPUState first;
static bool multiple_cpus;
#define first_cpu (&first)
#define CPU_NEXT(cpu) ((void)(cpu), multiple_cpus ? &first : NULL)
typedef struct { bool ram, romd; } MemoryRegion;
typedef struct { MemoryRegion *mr; bool readonly; } MemoryRegionSection;
static MemoryRegion mr = { .ram = true };
static MemoryRegionSection section = { .mr = &mr };
static uint8_t page_state, vga_state;
static bool enable_vga;
static bool parallel, clean, inject_protection, stub;
static unsigned scans, flushes, clears, large_pages;
static uint64_t scan_addr, flush_addr;
static int locked;
static bool latx_hack_gpa_in_ram(uint64_t p) { return p >= 0x100000 && p < 0x80000000; }
static bool paddr_in_vram(uint64_t p) { return enable_vga && p >= 0x7000000 && p < 0x7001000; }
static bool hamt_ptwflags_smc_page_is_protected(uint64_t p) { (void)p; return page_state==1; }
static uint8_t hamt_ptwflags_get_page(uint64_t p) { return paddr_in_vram(p) ? vga_state : page_state; }
static bool qemu_tcg_mttcg_enabled(void) { return parallel; }
static uint64_t qemu_ram_addr_to_gpa(uint64_t p) { return p; }
static void hamt_protect_code(uint64_t p,int full) { flush_addr=p;(void)full; flushes++; }
static void hamt_ptwflags_smc_protect_page(uint64_t p) { (void)p; if (!stub) page_state=1; }
static void tlb_reset_dirty_range_all(uint64_t p,uint64_t n) { assert(!(p&4095)&&n==4096);scan_addr=p;scans++; }
static bool cpu_physical_memory_test_and_clear_dirty(uint64_t p,uint64_t n,int b) { (void)p;(void)n;(void)b;clears++;return true; }
static bool cpu_physical_memory_is_clean(uint64_t p) { (void)p;return clean; }
static CPUTLB *env_tlb(CPUArchState *e) { return &e->tlb; }
static void assert_cpu_is_self(CPUState *c) { assert(c); }
static void tlb_add_large_page(CPUArchState *e,int i,uint64_t a,uint64_t s) { (void)e;(void)i;(void)a;(void)s;large_pages++; }
static int cpu_asidx_from_attrs(CPUState *c,MemTxAttrs a) { (void)c;(void)a;return 0; }
static MemoryRegionSection *address_space_translate_for_iotlb(CPUState *c,int i,hwaddr p,hwaddr *x,hwaddr *s,MemTxAttrs a,int *prot) { (void)c;(void)i;(void)s;(void)a;(void)prot;*x=p;return &section; }
static bool memory_region_is_ram(MemoryRegion *r) { return r->ram; }
static bool memory_region_is_romd(MemoryRegion *r) { return r->romd; }
static void *memory_region_get_ram_ptr(MemoryRegion *r) { (void)r; return (void *)(uintptr_t)HACK_RAMBASE; }
static hwaddr memory_region_get_ram_addr(MemoryRegion *r) { (void)r;return 0; }
static hwaddr memory_region_section_get_iotlb(CPUState *c,MemoryRegionSection *s) { (void)c;(void)s;return 0; }
static int cpu_watchpoint_address_matches(CPUState *c,vaddr a,uint64_t s) { (void)c;(void)a;(void)s;return 0; }
static unsigned tlb_index(CPUArchState *e,int i,vaddr a) { (void)e;(void)i;return (a>>12)&3; }
static CPUTLBEntry *tlb_entry(CPUArchState *e,int i,vaddr a) { return &e->tlb.table[i][tlb_index(e,i,a)]; }
static void qemu_spin_lock(int *p) {
    (void)p; assert(!locked); locked=1;
    /* Model protection completing after a constructor starts, before lock. */
    if (inject_protection) page_state=1;
}
static void qemu_spin_unlock(int *p) { (void)p;assert(locked);locked=0; }
static void tlb_flush_vtlb_page_locked(CPUArchState *e,int i,vaddr a) { (void)e;(void)i;(void)a;assert(locked); }
static bool tlb_hit_page_anyprot(CPUTLBEntry *e,vaddr a) { return e->addr[0]==a; }
static bool tlb_entry_is_empty(CPUTLBEntry *e) { return !e->addend; }
static void copy_tlb_helper_locked(CPUTLBEntry *d,const CPUTLBEntry *s) { assert(locked);*d=*s; }
static void tlb_n_used_entries_dec(CPUArchState *e,int i) { (void)e;(void)i; }
static void tlb_n_used_entries_inc(CPUArchState *e,int i) { (void)e;(void)i; }
static void tlb_set_compare(CPUTLBEntryFull *f,CPUTLBEntry *e,vaddr a,unsigned flags,int type,bool present) { (void)f;assert(locked);e->addr[type]=present ? a|flags : UINT64_MAX; }
static unsigned ctz64(uint64_t n) { return __builtin_ctzll(n); }
'''

checks = r'''
int main(void) {
    for (unsigned round=0;round<100;round++) {
        tlb_protect_code(0x200001);
    }
    assert(scans==1 && flushes==1);
    page_state=0;
    tlb_protect_code(0x200001);
    assert(scans==2 && flushes==2);
    parallel=true;
    tlb_protect_code(0x200001);
    assert(scans==2 && flushes==2); /* MTTCG with only one vCPU */
    multiple_cpus=true;
    for (unsigned round=0;round<100;round++) tlb_protect_code(0x200001);
    assert(scans==102 && flushes==2);
    parallel=false;
    tlb_protect_code(0x200001);
    assert(scans==102); /* serialized multi-vCPU */
    multiple_cpus=false; page_state=0; stub=true;
    for (unsigned round=0;round<3;round++) tlb_protect_code(0x200001);
    assert(scans==105 && flushes==5);
    stub=false;
    tlb_protect_code(0x1000);
    assert(clears==1 && flushes==6 && scans==105);
    CPUArchState env={0}; CPUState cpu={.env_ptr=&env};
    unsigned cases=0;
    for (int fast=0;fast<2;fast++)
    for (int protect=0;protect<2;protect++)
    for (int inject=0;inject<2;inject++)
    for (int writable=0;writable<2;writable++)
    for (int big=0;big<2;big++) {
        page_state=protect ? 1 : 0;
        inject_protection=inject;
        int prot=PAGE_READ|PAGE_EXEC|(writable ? PAGE_WRITE : 0);
        CPUTLBEntryFull full={.phys_addr=0x200000,.prot=prot,.lg_page_size=big ? 21 : 12};
        if (fast) tlb_set_page_full_fast(&cpu,0,0x400000,full.phys_addr,full.attrs,prot,1ULL<<full.lg_page_size);
        else tlb_set_page_full(&cpu,0,0x400000,&full);
        uint64_t write=env.tlb.table[0][0].addr[MMU_DATA_STORE];
        if (!writable) assert(write==UINT64_MAX);
        else assert(!!(write&TLB_NOTDIRTY)==(protect||inject));
        cases++;
    }
    inject_protection=false; page_state=1;
    section.readonly=true;
    CPUTLBEntryFull full={.phys_addr=0x200000,.prot=PAGE_READ|PAGE_WRITE,.lg_page_size=12};
    tlb_set_page_full(&cpu,0,0x400000,&full);
    assert(env.tlb.table[0][0].addr[MMU_DATA_STORE]&TLB_DISCARD_WRITE);
    assert(!(env.tlb.table[0][0].addr[MMU_DATA_STORE]&TLB_NOTDIRTY));
    section.readonly=false; page_state=0; clean=true;
    tlb_set_page_full(&cpu,0,0x400000,&full);
    assert(env.tlb.table[0][0].addr[MMU_DATA_STORE]&TLB_NOTDIRTY);
    clean=false; page_state=0;
    tlb_protect_code(0x7000000);
    assert(flush_addr==0x7000000 && scan_addr==0x7000000);
    assert(large_pages);
    printf("PASS: protection cycles, parallel/stub fallback, %u TLB construction cases and readonly/dirty bitmap\n",cases);
}
'''

with tempfile.TemporaryDirectory(prefix='smc-protect-test-') as temp:
    path = pathlib.Path(temp)
    c = path / 'test.c'
    c.write_text(support + '\n'.join(function(n) for n in ['tlb_protect_code','tlb_set_page_full','tlb_set_page_full_fast']) + checks)
    command = shlex.split(os.environ.get('CC', 'cc'))
    subprocess.run(command + ['-std=gnu11','-O2','-Wall','-Wextra','-Werror',str(c),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
