#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise production AOT probe/lock helpers with a fault-injecting MMU.

The compiled harness uses the actual C function bodies, not a second model
of their control flow. The source guard also checks the allocation phase,
where taking a guest exception would bypass page-lock cleanup.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
source = (root / "target/i386/latx/sbt/sys-aot-load.c").read_text()
maintenance = (root / "accel/tcg/tb-maint.c").read_text()
cputlb = (root / "accel/tcg/cputlb.c").read_text()


def function(name, source=source):
    start = source.index(name + "(")
    start = source.rfind("\n", 0, start) + 1
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


load = function("TranslationBlock *load_tu")
assert "get_page_addr_code" not in load
assert "latxs_tr_cvt_cpu_addr_code(" not in load
assert "= tb_lookup(" not in load
assert "tb_htable_lookup_noexit(" in load
assert "env->error_code = saved_error_code" in load
assert load.index("aot_locked_tu_page(curr_pc, NULL)") < load.index(
    "tcg_tb_alloc_full(")
assert "aot_probe_code_page(" in function(
    "static uint64_t aot_find_tu_page_record")
assert "get_page_addr_code_hostp(" not in function(
    "static uint64_t aot_find_tu_page_record")
invalidation = maintenance[maintenance.index(
    "tb_invalidate_phys_page_range__locked("):]
assert "p->aot_tu_cache_generation = 0;" in invalidation

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_LATX_TU 1
#define CONFIG_SOFTMMU 1
#define TARGET_PAGE_SIZE 4096ULL
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_MASK (~(TARGET_PAGE_SIZE - 1))
#define AOT_TU_MAX_PAGES 8
#define g_assert assert
#define g_try_new(type, n) ((type *)calloc((n), sizeof(type)))
#define g_free free
typedef uint64_t vaddr;
typedef uint64_t tb_page_addr_t;
typedef uint64_t ram_addr_t;
typedef uintptr_t ADDR;
typedef struct {
    int error_code;
    uint64_t cr2;
    int exception_index;
} CPUArchState;
typedef struct { CPUArchState *env_ptr; } CPUState;
typedef struct { int64_t pc_delta; uint64_t page_fingerprint; } aot_tu_page;
typedef struct { int page_count; aot_tu_page pages[8]; } aot_tu_head;
static aot_tu_page *aot_tu_pages(aot_tu_head *h) { return h->pages; }
static unsigned char ram[8][4096] __attribute__((aligned(4096)));
static tb_page_addr_t mapping[8];
static int missing = -1, fail_locked = -1;
static int probes, locks, unlocks, held, comparisons;
static unsigned char saved[8][4096];
static bool protected_page[8];
static uint64_t locked[8];
static tb_page_addr_t *aot_tu_locked_pages;
static int aot_tu_locked_pages_num;
typedef struct AOTTUPageDependency {
    vaddr vp; tb_page_addr_t pp; int64_t pc_delta; void *host_page;
} AOTTUPageDependency;
static AOTTUPageDependency aot_tu_dependencies[8];
static int aot_tu_dependencies_num;
static tb_page_addr_t get_page_addr_code_hostp_noexit(CPUArchState *env,
        vaddr vp, void **hostp)
{
    unsigned n = vp / 4096 - 1;
    probes++;
    assert(n < 8);
    if ((int)n == missing || (held && (int)n == fail_locked)) {
        env->error_code = 99;
        return -1;
    }
    if (hostp) { *hostp = ram[mapping[n] / 4096]; }
    return mapping[n];
}
static bool aot_host_pc_in_main_ram(void *p) { return p != NULL; }
#ifdef CONFIG_LATX_HACK_MEM
static bool latx_hack_gpa_in_ram(uint64_t pp) { return pp < sizeof(ram); }
static bool hamt_ptwflags_smc_page_is_protected(uint64_t pp)
{
    return protected_page[pp / 4096];
}
#endif
static void tlb_protect_code(uint64_t pp) { protected_page[pp / 4096] = true; }
static void tb_lock_page0(uint64_t pp)
{
    assert(!held || locked[held - 1] < pp);
    locked[held++] = pp;
    locks++;
}
static void tb_unlock_page0(uint64_t pp)
{
    assert(held && locked[held - 1] == pp);
    held--;
    unlocks++;
}
/* The production byte/ownership lookup is covered by test-tu-page-bytes.py. */
static bool aot_tu_dependency_matches(const void *p,
        const aot_tu_head *tu, int64_t delta)
{
    int n = ((const unsigned char *)p - ram[0]) / 4096;
    assert(held > 0 && protected_page[n]);
    assert(delta / 4096 < tu->page_count);
    comparisons++;
    return !memcmp(p, saved[delta / 4096], 4096);
}
'''

bodies = function("bool tlb_code_page_is_protected", cputlb) + "\n\n"
bodies += "\n\n".join(function(name) for name in (
    "static void aot_unlock_load_pages",
    "static tb_page_addr_t aot_probe_code_page",
    "static int aot_phys_page_cmp",
    "static bool aot_lock_tu_dependencies",
    "static tb_page_addr_t aot_locked_tu_page",
))

main = r'''
static void reset(void)
{
    assert(held == 0 && aot_tu_dependencies_num == 0);
    missing = fail_locked = -1;
    probes = locks = unlocks = comparisons = 0;
    memset(protected_page, 0, sizeof(protected_page));
    memset(ram, 0, sizeof(ram));
    memset(saved, 0, sizeof(saved));
    for (int i = 0; i < 8; i++) { mapping[i] = (7 - i) * 4096; }
}
static aot_tu_head header(int count)
{
    aot_tu_head h = { .page_count = count };
    for (int i = 0; i < count; i++) {
        h.pages[i] = (aot_tu_page) { i * 4096, mapping[i] / 4096 + 100 };
    }
    return h;
}
int main(void)
{
    CPUArchState env = {
        .error_code = 17, .cr2 = 0x1234, .exception_index = 23,
    };
    CPUState cpu = { &env };
    reset();
    missing = 0;
    assert(aot_probe_code_page(&env, 4096, NULL) == (uint64_t)-1);
    assert(env.error_code == 17 && env.cr2 == 0x1234 &&
           env.exception_index == 23);
    for (int bad = 0; bad < 8; bad++) {
        reset();
        aot_tu_head h = header(8);
        missing = bad;
        assert(!aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
        assert(held == 0 && locks == 0 && aot_tu_dependencies_num == 0);
        assert(env.error_code == 17);
    }
    reset();
    aot_tu_head h = header(8);
    fail_locked = 3;
    assert(!aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    assert(locks == 64 && unlocks == 64 && held == 0 && env.error_code == 17);
    reset();
    h = header(8);
    ram[4][17] = 1;
    assert(!aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    assert(locks == unlocks && held == 0);
    assert(locks == 8 && comparisons == 4);
    reset();
    mapping[1] = mapping[0]; /* aliases acquire only one physical lock */
    h = header(8);
    assert(aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    assert(held == 7 && comparisons == 8 && aot_tu_dependencies_num == 8);
    int before = probes;
    for (int i = 0; i < 8; i++) {
        void *hostp = NULL;
        assert(aot_locked_tu_page((i + 1) * 4096 + 11, &hostp) ==
               mapping[i] + 11);
        assert(hostp == ram[mapping[i] / 4096] + 11);
    }
    assert(aot_locked_tu_page(9 * 4096, NULL) == (uint64_t)-1);
    assert(probes == before); /* no page walk after the validated snapshot */
    aot_unlock_load_pages(-1);
    assert(held == 0 && locks == unlocks && aot_tu_dependencies_num == 0);
    assert(env.error_code == 17 && env.cr2 == 0x1234 &&
           env.exception_index == 23);
    before = probes;
    assert(aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    assert(probes > before && comparisons == 16);
    aot_unlock_load_pages(-1);
    /* Different saved bytes must not reuse a previous successful match. */
    saved[0][17] = 1;
    before = locks;
    assert(!aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    assert(locks == before + 7 && comparisons == 17 && held == 0);
    saved[0][17] = 0;
    /* A physical page write is detected even with virtual aliases. */
    ram[mapping[0] / 4096][17] = 1;
    assert(!aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    assert(comparisons == 18 && held == 0);
    /* Reject, then modify a page before trying another TU. */
    reset();
    h = header(2);
    saved[1][17] = 1;
    assert(!aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    assert(comparisons == 2 && held == 0);
    int changed_page = mapping[1] / 4096;
    assert(protected_page[changed_page]);
    ram[changed_page][17] = 1;
    protected_page[changed_page] = false;
    saved[1][17] = 0;
    assert(!aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    assert(comparisons == 4 && held == 0);
    assert(protected_page[changed_page]);
    /* Accept only after the saved and current bytes agree again. */
    saved[1][17] = 1;
    assert(aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    assert(comparisons == 6);
    aot_unlock_load_pages(-1);
    reset();
    h = header(0);
    assert(!aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    h.page_count = 9;
    assert(!aot_lock_tu_dependencies(&cpu, 4096, &h, NULL, 0));
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="aot-nonfault-") as directory:
    cfile = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    cfile.write_text(prefix + bodies + main)
    for hack_mem in (False, True):
        flags = ["-DCONFIG_LATX_HACK_MEM=1"] if hack_mem else []
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11",
                        "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-parameter", *flags,
                        *shlex.split(os.environ.get("CFLAGS", "")),
                        str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print("PASS: dependency protection and locked byte checks, "
              f"HACK_MEM={hack_mem}")
print("PASS: speculative AOT probes preserve exception state and release locks")
