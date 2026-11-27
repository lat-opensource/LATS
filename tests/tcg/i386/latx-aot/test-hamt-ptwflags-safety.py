#!/usr/bin/env python3
"""Stress ptwflags transitions and validate SMC protection ordering."""

import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]


def function(source, signature):
    start = source.index(signature)
    pos = source.index("{", start)
    depth = 0
    state = "code"
    while pos < len(source):
        char = source[pos]
        next_char = source[pos + 1] if pos + 1 < len(source) else ""
        if state == "code":
            if char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "/" and next_char == "/":
                state = "line-comment"
                pos += 1
            elif char == "/" and next_char == "*":
                state = "block-comment"
                pos += 1
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[start:pos + 1]
        elif state in ("string", "char"):
            if char == "\\":
                pos += 1
            elif (state == "string" and char == '"') or \
                    (state == "char" and char == "'"):
                state = "code"
        elif state == "line-comment" and char == "\n":
            state = "code"
        elif state == "block-comment" and char == "*" and next_char == "/":
            state = "code"
            pos += 1
        pos += 1
    raise ValueError("unterminated function: " + signature)


def build_harness(source, physmem):
    declarations = source[source.index("#define HAMT_PTW_GPA_END"):
                          source.index("static uint8_t hamt_ptwstat_lock(")]
    harness = r"""
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define qemu_build_assert(x) assert(x)
#include "qemu/atomic.h"
#include "qemu/processor.h"
#include "hamt-ptw.h"
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1U << TARGET_PAGE_BITS)
#define PAGE TARGET_PAGE_SIZE
#define TARGET_PAGE_ALIGN(x) (((x) + TARGET_PAGE_SIZE - 1) & \
                              ~(TARGET_PAGE_SIZE - 1))
#define HAMT_HOLE_ASSERT(p) ((p) != MAP_FAILED)
#define lsassert(p) assert(p)
#define lsassertm(p, ...) assert(p)
#define g_assert(p) assert(p)
static bool hamt_no_vga_dirty(void) { return false; }
""" + declarations
    for signature in (
        "static size_t hamt_ptwflags_mapping_size(",
        "static void hamt_ptw_init(",
        "static uint8_t hamt_ptwstat_lock(",
        "static uint16_t hamt_ptwgroup_lock(",
        "static void hamt_ptwflags_refresh_group(",
        "static void hamt_ptwflags_reserve_smc(",
        "void hamt_ptwflags_set_mmio(",
        "void hamt_ptwflags_smc_protect_page(",
        "void hamt_ptwflags_smc_unprotect_page(",
        "void hamt_ptwflags_vga_protect(",
        "void hamt_ptwflags_vga_snapshot(",
        "bool hamt_ptwflags_vga_snapshot_is_dirty(",
        "void hamt_ptwflags_vga_write_page(",
        "uint8_t hamt_ptwflags_get_page(",
        "uint8_t hamt_ptwflags_get_page_state(",
    ):
        harness += function(source, signature) + "\n"
    harness += "typedef uint64_t ram_addr_t;\n#define CONFIG_LATX_HACK_MEM 1\n"
    harness += "typedef " + function(physmem, "struct DirtyBitmapSnapshot {")
    harness += " DirtyBitmapSnapshot;\n"
    for signature in (
        "static uint8_t *dirty_bitmap_snapshot_get_hamt(",
        "bool dirty_bitmap_snapshot_hamt_vga_is_dirty(",
    ):
        harness += function(physmem, signature) + "\n"
    return harness


def run_host_tests(source, cputlb, physmem, compiler):
    harness = build_harness(source, physmem)
    harness += r"""
#define GROUPS 6
#define THREADS 12
#define ITERATIONS 20000
#define MMIO_ITERATIONS 2000

typedef struct {
    uint64_t group;
    unsigned index;
} worker_arg_t;

static volatile int race_error;

static void *toggle_worker(void *opaque)
{
    const worker_arg_t *arg = opaque;

    for (unsigned i = 0; i < ITERATIONS; i++) {
        uint64_t page = arg->group + ((i + arg->index) & 7) * PAGE;

        if ((i + arg->index) & 1) {
            hamt_ptwflags_smc_protect_page(page);
        } else {
            hamt_ptwflags_smc_unprotect_page(page);
        }
        if (!((i + arg->index) & 3)) {
            hamt_ptwflags_smc_protect_page(page);
            hamt_ptwflags_smc_unprotect_page(page);
        }
        if ((qatomic_read(&ptwdata1m[arg->group >> 20]) & ~PTW_GROUP_LOCK) > 8) {
            qatomic_set(&race_error, 1);
        }
    }
    return NULL;
}

static void *mmio_worker(void *opaque)
{
    const worker_arg_t *arg = opaque;

    for (unsigned i = 0; i < MMIO_ITERATIONS; i++) {
        hamt_ptwflags_set_mmio(arg->group + PAGE, PAGE);
    }
    return NULL;
}

static void *vga_smc_worker(void *opaque)
{
    const worker_arg_t *arg = opaque;
    uint64_t page = arg->group + PAGE;

    for (unsigned i = 0; i < ITERATIONS; i++) {
        if (arg->index & 1) {
            hamt_ptwflags_smc_protect_page(page);
            hamt_ptwflags_smc_unprotect_page(page);
        } else {
            hamt_ptwflags_vga_protect(page, PAGE);
            hamt_ptwflags_vga_write_page(page);
        }

        uint8_t reasons;
        uint8_t flags = hamt_ptwflags_get_page_state(page, &reasons);

        assert(!(reasons & ~(PTW_REASON_SMC | PTW_REASON_VGA)));
        assert(!!(flags & HAMT_PTWFLAGS_NOWRITE) == !!reasons);
    }
    return NULL;
}

static void assert_page_state(uint64_t page, uint8_t expected)
{
    uint8_t reasons;
    uint8_t flags = hamt_ptwflags_get_page_state(page, &reasons);

    assert(reasons == expected);
    assert(!!(flags & HAMT_PTWFLAGS_NOWRITE) == !!expected);
}

static void test_pending_smc(void)
{
    const uint64_t group = 5;
    const uint64_t base = group << 20;
    const uint64_t index = (base + PAGE) >> TARGET_PAGE_BITS;
    uint8_t reasons;

    hamt_ptwflags_smc_protect_page(base);
    reasons = hamt_ptwstat_lock(index);
    assert(reasons == 0);
    hamt_ptwflags_reserve_smc(group);
    assert(hamt_ptwflags_get_page(base + PAGE) == 0);
    hamt_ptwflags_smc_unprotect_page(base);
    assert(ptwdata1m[group] == 1);
    assert(hamt_ptwflags_get_page(base) == HAMT_PTWFLAGS_NO2M);

    qatomic_or(&ptwflags[index], HAMT_PTWFLAGS_NOWRITE);
    qatomic_store_release(&ptwflagstat[index], PTW_REASON_SMC);
    hamt_ptwflags_smc_unprotect_page(base + PAGE);
    assert(ptwdata1m[group] == 0);
    assert(hamt_ptwflags_get_page(base) == 0);
    puts("PASS: pending SMC restriction keeps NO2M set");
}

static void test_vga_snapshot(void)
{
    const uint64_t gpa = (5ULL << 20) + PAGE;
    const unsigned long ram_dirty = 0x5a;
    DirtyBitmapSnapshot *snap = calloc(1, sizeof(*snap) + sizeof(ram_dirty) + 3);

    assert(snap);
    snap->start = 2 * PAGE;
    snap->end = snap->start + sizeof(ram_dirty) * 8 * PAGE;
    snap->dirty[0] = ram_dirty;
    snap->hamt_vga_start = gpa >> TARGET_PAGE_BITS;
    snap->hamt_vga_pages = 3;
    hamt_ptwflags_vga_protect(gpa, 3 * PAGE);
    hamt_ptwflags_vga_write_page(gpa + PAGE);
    hamt_ptwflags_vga_snapshot(gpa, 3 * PAGE, dirty_bitmap_snapshot_get_hamt(snap));

    assert(snap->dirty[0] == ram_dirty);
    assert(!dirty_bitmap_snapshot_hamt_vga_is_dirty(snap, gpa + 31, 1));
    assert(dirty_bitmap_snapshot_hamt_vga_is_dirty(snap, gpa + PAGE - 1, 2));
    assert(!dirty_bitmap_snapshot_hamt_vga_is_dirty(snap, gpa + 2 * PAGE, PAGE));
    assert(!dirty_bitmap_snapshot_hamt_vga_is_dirty(snap, gpa + PAGE, 0));
    assert(dirty_bitmap_snapshot_hamt_vga_is_dirty(snap, gpa - 1, 1));
    assert(dirty_bitmap_snapshot_hamt_vga_is_dirty(snap, gpa + 3 * PAGE, 1));
    free(snap);
    puts("PASS: VGA snapshot uses GPA-relative pages and a separate buffer");
}

int main(void)
{
    const uint64_t group2 = 2ULL << 20;
    const uint64_t group3 = 3ULL << 20;
    pthread_t threads[THREADS];
    worker_arg_t args[THREADS];

    hamt_ptw_init();

    test_pending_smc();
    test_vga_snapshot();

    hamt_ptwflags_set_mmio(1ULL << 20, PAGE);
    assert(hamt_ptwflags_get_page(1ULL << 20) &
           (HAMT_PTWFLAGS_NOACCESS | HAMT_PTWFLAGS_NO2M));
    hamt_ptwflags_set_mmio(1ULL << 20, PAGE);
    for (unsigned i = 0; i < 8; i++) {
        hamt_ptwflags_smc_protect_page(group2 + i * PAGE);
    }
    for (unsigned i = 0; i < THREADS; i++) {
        args[i].group = group2;
        args[i].index = i;
        assert(!pthread_create(&threads[i], NULL, toggle_worker, &args[i]));
    }
    for (unsigned i = 0; i < THREADS; i++) {
        assert(!pthread_join(threads[i], NULL));
    }
    for (unsigned i = 0; i < 8; i++) {
        hamt_ptwflags_smc_unprotect_page(group2 + i * PAGE);
    }
    assert(!race_error);
    assert(ptwdata1m[2] == 0);
    assert(hamt_ptwflags_get_page(group2) == 0);

    for (unsigned i = 0; i < 8; i++) {
        hamt_ptwflags_smc_protect_page(group3 + i * PAGE);
    }
    for (unsigned i = 0; i < THREADS - 1; i++) {
        args[i].group = group3;
        args[i].index = i;
        assert(!pthread_create(&threads[i], NULL, toggle_worker, &args[i]));
    }
    args[THREADS - 1].group = group3;
    args[THREADS - 1].index = THREADS - 1;
    assert(!pthread_create(&threads[THREADS - 1], NULL, mmio_worker,
                           &args[THREADS - 1]));
    for (unsigned i = 0; i < THREADS; i++) {
        assert(!pthread_join(threads[i], NULL));
    }
    for (unsigned i = 0; i < 8; i++) {
        hamt_ptwflags_smc_unprotect_page(group3 + i * PAGE);
    }
    assert(ptwdata1m[3] == 0);
    assert(hamt_ptwflags_get_page(group3 + PAGE) ==
           HAMT_PTWFLAGS_NOACCESS);
    assert(hamt_ptwflags_get_page(group3) &
           (HAMT_PTWFLAGS_NOACCESS | HAMT_PTWFLAGS_NO2M));

    for (unsigned i = 0; i < THREADS; i++) {
        args[i].group = 4ULL << 20;
        args[i].index = i;
        assert(!pthread_create(&threads[i], NULL, vga_smc_worker, &args[i]));
    }
    for (unsigned i = 0; i < THREADS; i++) {
        assert(!pthread_join(threads[i], NULL));
    }
    uint64_t shared = (4ULL << 20) + PAGE;
    assert_page_state(shared, 0);
    hamt_ptwflags_vga_protect(shared, PAGE);
    assert_page_state(shared, PTW_REASON_VGA);
    hamt_ptwflags_smc_protect_page(shared);
    assert_page_state(shared, PTW_REASON_SMC | PTW_REASON_VGA);
    assert(ptwdata1m[4] == 1);
    hamt_ptwflags_vga_write_page(shared);
    assert_page_state(shared, PTW_REASON_SMC);
    hamt_ptwflags_smc_unprotect_page(shared);
    assert(ptwdata1m[4] == 0);
    assert_page_state(shared, 0);
    puts("PASS: concurrent VGA and SMC protection snapshots");

    assert(!munmap(ptwflags, hamt_ptwflags_mapping_size(ptwflags_size)));
    puts("PASS: concurrent ptwflags transitions");
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="hamt-ptwflags-safety-") as directory:
        binary = Path(directory) / "test"
        subprocess.run(
            [*shlex.split(compiler), "-std=gnu11", "-O2", "-Wall", "-Wextra",
             "-Werror", "-pthread", "-I", str(ROOT / "include"), "-x", "c",
             "-o", str(binary), "-"],
            input=harness, text=True, check=True,
        )
        subprocess.run([str(binary)], check=True)

    protect_body = function(source, "void hamt_ptwflags_smc_protect_page(")
    unprotect_body = function(source, "void hamt_ptwflags_smc_unprotect_page(")
    reserve = protect_body.index("hamt_ptwflags_reserve_smc")
    restrict_page = protect_body.index("HAMT_PTWFLAGS_NOWRITE")
    relax_page = unprotect_body.index("HAMT_PTWFLAGS_NOWRITE")
    refresh = unprotect_body.index("hamt_ptwflags_refresh_group")
    if not reserve < restrict_page or not relax_page < refresh:
        raise AssertionError("SMC group and page publication order is wrong")
    if "ptwflags_lock" in source:
        raise AssertionError("ptwflags still uses a global lock")

    tlb_body = function(cputlb, "void tlb_protect_code(")
    tag = tlb_body.index("hamt_ptwflags_smc_protect_page")
    flush = tlb_body.index("hamt_protect_code", tag)
    if not tag < flush:
        raise AssertionError("SMC permission published after code TLB invalidation")
    print("PASS: SMC state publication precedes TLB invalidation")

    snapshot = function(physmem, "DirtyBitmapSnapshot *cpu_physical_memory_snapshot_and_clear_dirty")
    software_flush = snapshot.index("tlb_reset_dirty_range_all")
    vga_snapshot = snapshot.index("hamt_ptwflags_vga_snapshot(")
    vga_protect = snapshot.index("hamt_ptwflags_vga_protect(")
    if not software_flush < vga_snapshot < vga_protect:
        raise AssertionError("VGA snapshot ordering differs from original path")
    write = function(cputlb, "static void notdirty_write(")
    if "hamt_ptwflags_vga_write_page(ram_addr)" not in write:
        raise AssertionError("VGA first-write handling is missing")
    print("PASS: original VGA snapshot and first-write paths")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ptw-source", type=Path,
                        default=ROOT / "target/i386/hamt/hamt-ptw-flags.c")
    parser.add_argument("--cputlb-source", type=Path,
                        default=ROOT / "accel/tcg/cputlb.c")
    parser.add_argument("--physmem-source", type=Path,
                        default=ROOT / "softmmu/physmem.c")
    args = parser.parse_args()
    run_host_tests(args.ptw_source.read_text(), args.cputlb_source.read_text(),
                   args.physmem_source.read_text(), "cc")


if __name__ == "__main__":
    main()
