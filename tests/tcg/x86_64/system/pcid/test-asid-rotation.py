#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Exercise HAMT CR3 ASID rotation with mocked host operations."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[5] / 'target/i386'
header = (root / 'cpu.h').read_text()
definitions = '\n'.join(re.findall(
    r'^#define X86_(?:HAMT_ASID_BASE|PCID_SLOTS)\s+[^\n]+', header, re.M
))
assert definitions.count('#define') == 2

implementation = (root / 'hamt/hamt-tlb.c').read_text()
start = implementation.index('bool hamt_rotate_cr3_asid(')
end = implementation.index('\n}', start) + 2

source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define qatomic_inc(p) (++*(p))
#define env_cpu(env) (&(env)->cpu)
#define env_archcpu(env) (env)

typedef struct {
    struct { int cpu_index; } cpu;
    uint64_t cr3;
    unsigned hamt_guest_asid;
    struct { unsigned asid_rotations, asid_batches; } pcid_profile;
} CPUX86State;

static bool option_pcid_stats = true;
static unsigned width, csr_reads, full_flushes, hpet_clears;
static CPUX86State *active_cpu;
static unsigned asid_at_flush;

static uint32_t read_csr_asid(void)
{
    ++csr_reads;
    return width << 16 | active_cpu->hamt_guest_asid;
}

static unsigned extract32(uint32_t value, unsigned start, unsigned length)
{
    return (value >> start) & ((1U << length) - 1);
}

static void latx_mem_clear_hpet_vpages(void)
{
    ++hpet_clears;
}

static void hamt_flush_tlb_all(void)
{
    ++full_flushes;
    asid_at_flush = active_cpu->hamt_guest_asid;
    latx_mem_clear_hpet_vpages();
}
'''
source += definitions + '\n' + implementation[start:end] + '\n'
source += r'''
static void check_rotation(CPUX86State *cpu, unsigned expected_tag,
                           bool rotate, bool flush, bool check_width)
{
    unsigned old_tag = cpu->hamt_guest_asid;
    unsigned rotations = cpu->pcid_profile.asid_rotations;
    unsigned batches = cpu->pcid_profile.asid_batches;
    uint64_t root = cpu->cr3;

    active_cpu = cpu;
    csr_reads = full_flushes = hpet_clears = 0;
    assert(hamt_rotate_cr3_asid(cpu) == rotate);
    assert(cpu->hamt_guest_asid == expected_tag);
    assert(cpu->cr3 == root);
    assert(csr_reads == check_width);
    assert(full_flushes == flush);
    assert(hpet_clears == rotate);
    assert(cpu->pcid_profile.asid_rotations ==
           rotations + (rotate && option_pcid_stats));
    assert(cpu->pcid_profile.asid_batches ==
           batches + (flush && option_pcid_stats));
    if (flush) {
        assert(asid_at_flush == old_tag);
    }
}

static void check_widths(void)
{
    static const unsigned widths[] = {5, 6, 10};
    static const int indices[] = {0, 1, 30, 31};
    static const int max_cpu[] = {-1, 0, 30};

    for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        width = widths[w];
        for (unsigned i = 0; i < sizeof(indices) / sizeof(indices[0]); ++i) {
            CPUX86State cpu = {.cpu.cpu_index = indices[i],
                               .hamt_guest_asid = 0xf};
            bool rotate = indices[i] <= max_cpu[w];
            unsigned tag = rotate ? 32 + indices[i] * 32 : 0xf;
            check_rotation(&cpu, tag, rotate, rotate, indices[i] < 31);
        }
    }
}

static void check_disjoint_ranges(void)
{
    bool used[1024] = {0};
    width = 10;
    for (int index = 0; index <= 30; ++index) {
        CPUX86State cpu = {.cpu.cpu_index = index, .cr3 = 0x1000,
                           .hamt_guest_asid = 0xf};
        unsigned first = 32 + index * 32;

        /* Keep the same guest CR3 for consecutive writes and two rollovers. */
        for (unsigned offset = 0; offset < 32; ++offset) {
            unsigned tag = first + offset;
            check_rotation(&cpu, tag, true, offset == 0, offset == 0);
            assert(!used[tag]);
            used[tag] = true;
        }
        check_rotation(&cpu, first, true, true, false);
        for (unsigned offset = 1; offset < 32; ++offset) {
            check_rotation(&cpu, first + offset, true, false, false);
        }
        check_rotation(&cpu, first, true, true, false);
    }
}

static void check_tag_transitions(void)
{
    CPUX86State cpu = {.cpu.cpu_index = 1, .hamt_guest_asid = 16};
    width = 10;

    check_rotation(&cpu, 64, true, true, true);
    check_rotation(&cpu, 65, true, false, false);
    /* Mode changes made by the caller leave the rotation range. */
    cpu.hamt_guest_asid = 31;
    check_rotation(&cpu, 64, true, true, true);
    cpu.hamt_guest_asid = 0xf;
    check_rotation(&cpu, 64, true, true, true);

    width = 6;
    cpu.hamt_guest_asid = 31;
    check_rotation(&cpu, 31, false, false, true);
    cpu.hamt_guest_asid = 0xf;
    check_rotation(&cpu, 0xf, false, false, true);

    option_pcid_stats = false;
    cpu.cpu.cpu_index = 0;
    check_rotation(&cpu, 32, true, true, true);
    check_rotation(&cpu, 33, true, false, false);
}

int main(void)
{
    check_widths();
    check_disjoint_ranges();
    check_tag_transitions();
    puts("PASS: HAMT ASID width, ranges, rollover and transitions");
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    program = path / 'asid-rotation.c'
    executable = path / 'asid-rotation'
    program.write_text(source)
    subprocess.run([
        *shlex.split(os.environ.get('CC', 'cc')), '-std=c11',
        '-Wall', '-Wextra', '-Werror', str(program), '-o', str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
