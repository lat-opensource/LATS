#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
# SPDX-License-Identifier: GPL-2.0-only

"""Check the JIT TU member admission and split-instruction fetch guards."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]) if len(sys.argv) == 2 else Path(__file__).resolve().parents[4]
latx = root / "target/i386/latx"
tu = (latx / "optimization/sys-tu.c").read_text()
disasm = (latx / "translator/sys-disasm.c").read_text()


def prefix(source, first, last):
    start = source.index(first)
    return source[start:source.index(last, start)]


fixture = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CONFIG_SOFTMMU 1
#define unlikely(x) (x)
#define likely(x) (x)
#define TARGET_PAGE_MASK (~UINT64_C(4095))
typedef uint64_t target_ulong, ADDRX, tb_page_addr_t;
typedef int TU_TB_START_TYPE;
typedef struct { int error_code; } CPUArchState, CPUX86State;
typedef struct { CPUArchState *env_ptr; } CPUState;
typedef struct { ADDRX pc; tb_page_addr_t page; } TranslationBlock;
typedef struct { tb_page_addr_t pre_locked_page0; } Context;
typedef struct {
    bool is_tu_mode, disable_cross_page_tb;
    int deadlock_avoidance;
} TRANSLATION_DATA;

enum { TU_EXPLORE_OK, TU_EXPLORE_FALLBACK, TU_LOCK_FALLBACK };
enum { ENTRY = 0x1000, SUCCESSOR = 0x2000, ENTRY_FRAME = 0x4000 };
static TranslationBlock entry = {.pc = ENTRY, .page = ENTRY_FRAME};
static struct {
    TranslationBlock *tb_list[1];
    bool disable_cross_page_for_run;
    unsigned pages_num;
} tu_state = {{&entry}, false, 0}, *tu_data = &tu_state;
static TRANSLATION_DATA decoder;
static struct { TRANSLATION_DATA *tr_data; } env = {&decoder}, *lsenv = &env;
static Context ctx = {ENTRY_FRAME}, *tcg_ctx, *tcg_ctx_mtcc[] = {&ctx};
static bool track_pages, missing, remapped;
static unsigned faulting, speculative, delivered;

static ADDRX get_page(ADDRX pc) { return pc & TARGET_PAGE_MASK; }
static unsigned curr_mtcc(CPUArchState *cpu) { (void)cpu; return 0; }
static bool cp_tu_pages(void) { return track_pages; }
static bool cp_tu_cptb(void) { return true; }
static unsigned cp_tu_page_num(void) { return 8; }
static tb_page_addr_t tb_page_addr0(TranslationBlock *tb) { return tb->page; }

static tb_page_addr_t get_page_addr_code_hostp(CPUArchState *cpu,
                                              ADDRX pc, void **host)
{
    (void)host;
    faulting++;
    if (missing) {
        delivered++;
        cpu->error_code = 99;
        return -1;
    }
    return get_page(pc) == ENTRY && !remapped ? ENTRY_FRAME : 0x5000;
}

static __attribute__((unused)) tb_page_addr_t get_page_addr_code_hostp_noexit(
        CPUArchState *cpu, ADDRX pc, void **host)
{
    (void)cpu;
    (void)host;
    speculative++;
    if (missing) {
        return -1;
    }
    return get_page(pc) == ENTRY && !remapped ? ENTRY_FRAME : 0x5000;
}
"""
# The first lookup belongs to tb_explore, before its page-lock block.
explore = tu[tu.index("static TranslationBlock *tb_explore("):]
fixture += "static tb_page_addr_t demand_entry(CPUArchState *env, ADDRX pc)\n{\n"
fixture += prefix(explore, "    void *host_pc;", "    if (phys_pc != -1)")
fixture += "    return phys_pc;\n}\n"
fixture += prefix(tu, "TranslationBlock* tb_create(", "#ifdef CONFIG_USER_ONLY")
fixture += r"""
    (void)tb; (void)lock_result; (void)phys_pc;
    (void)cs_base; (void)flags; (void)cflags; (void)max_insns;
    (void)is_first_tb; (void)mode; (void)hras_caller;
    return &entry;
}
"""
fixture += "#define in_aot_translate false\n"
fetch = disasm.split('#include "accel/tcg/internal.h"', 1)[1]
fixture += fetch[:fetch.index("    void *host_pc2")]
fixture += r"""
    (void)env; (void)phys_page2_old; (void)phys_page2_new;
}

static bool admit(ADDRX pc, bool first, int *result)
{
    CPUArchState env = {.error_code = 17};
    CPUState cpu = {.env_ptr = &env};

    delivered = 0;
    return tb_create(&cpu, pc, 0, 0, 0, 1, first, 0, NULL, result) == &entry;
}

static int failures;
static void check(const char *name, bool pass)
{
    puts(name);
    puts(pass ? "PASS" : "FAIL");
    failures += !pass;
}

int main(void)
{
    int result;
    CPUArchState env = {.error_code = 17};
    CPUState cpu = {.env_ptr = &env};

    track_pages = true;
    missing = true;
    bool declined = !tb_create(&cpu, SUCCESSOR, 0, 0, 0, 1, false, 0, NULL,
                              &result);
    check("missing speculative successor has no guest fault",
          declined && result == TU_EXPLORE_OK && speculative == 1 &&
          faulting == 0 && delivered == 0 && env.error_code == 17);

    check("first TU entry fetch still delivers the guest fault",
          demand_entry(&env, ENTRY) == (tb_page_addr_t)-1 && delivered == 1 &&
          env.error_code == 99);
    for (unsigned tracked = 0; tracked <= 1; tracked++) {
        track_pages = tracked;
        missing = true;
        unsigned before = speculative;
        declined = !admit(ENTRY, true, &result);
        check(tracked ? "tracked entry disappeared after lock" :
                        "untracked entry disappeared after lock",
              declined && speculative == before + 1 && !delivered &&
              result == TU_EXPLORE_OK);
        missing = false;
        remapped = true;
        declined = !admit(ENTRY, true, &result);
        check(tracked ? "tracked entry moved off locked frame" :
                        "untracked entry moved off locked frame",
              declined && result == TU_EXPLORE_OK);
        remapped = false;
        check("entry still on the locked frame is admitted",
              admit(ENTRY, true, &result));
    }
    bool admitted = admit(SUCCESSOR, false, &result);
    check("tracked mapped successor is admitted", admitted);

    track_pages = false;
    declined = !admit(SUCCESSOR, false, &result);
    check("untracked successor on another virtual page is left out",
          declined && result == TU_EXPLORE_OK);
    remapped = true;
    declined = !admit(ENTRY + 1, false, &result);
    check("successor cannot use a different physical page from the lock",
          declined && result == TU_EXPLORE_OK);
    remapped = false;
    admitted = admit(ENTRY + 1, false, &result);
    check("same-page successor is admitted", admitted);

    decoder.is_tu_mode = true;
    latxs_before_inst_fetch(&env, &entry, SUCCESSOR);
    check("split instruction requires TU page tracking",
          decoder.deadlock_avoidance == -4);
    decoder.is_tu_mode = false;
    decoder.deadlock_avoidance = 0;
    latxs_before_inst_fetch(&env, &entry, SUCCESSOR);
    check("ordinary TB can fetch across the boundary",
          decoder.deadlock_avoidance == 0);

    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
"""

with tempfile.TemporaryDirectory(prefix="lats-jit-tu-pages-") as tmp:
    test = Path(tmp) / "check"
    subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=gnu11",
                    "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                    "-x", "c", "-o", str(test), "-"],
                   input=fixture, text=True, check=True)
    sys.exit(subprocess.run([str(test)], check=False, timeout=10).returncode)
