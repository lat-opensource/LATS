#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Static regression checks for the SMP/Windows-XP fixes.

These assertions protect the non-obvious invariants that made SMP XP guests
livelock or crash:
  * one-shot translation and TU/JIT transitions must release page locks.
  * the dune_loop page-lock safety net must tolerate helper threads without
    a TCG context.

Usage: python3 tests/latx/test-latx-smp-defaults.py [source-root]
"""
from pathlib import Path
import sys


root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]


def read(path):
    return (root / path).read_text()


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


translate = read("accel/tcg/translate-all.c")
oneshot = function(translate, "    case GENCODE_ERR_TB_ONESHOT:")
assert oneshot.count("tb_unlock_ctx_pages(1);") >= 1, \
    "one-shot translations must release partial page locks"

maintenance = read("accel/tcg/tb-maint.c")
unlock_ctx = function(maintenance, "void tb_unlock_ctx_pages(int reset)")
tu_index = unlock_ctx.index("tb_unlock_tu_pages();")
pre_index = unlock_ctx.index("tcg_ctx->pre_locked_page1 != -1")
assert tu_index < pre_index, \
    "TB/TU and direct page locks must be released independently"
assert "return;" not in unlock_ctx[:pre_index], \
    "TU mode must not return before releasing pre_locked_pageX"

release = function(maintenance, "bool tb_release_ctx_page_locks(void)")
guard_index = release.index("if (tcg_ctx)")
deref_index = release.index("tcg_ctx->pre_locked_page0")
assert guard_index < deref_index, \
    "tb_release_ctx_page_locks must guard tcg_ctx before dereferencing it"
assert "lsenv && lsenv->tr_data" in release, \
    "tb_release_ctx_page_locks must guard the LATX environment"

dune = read("target/i386/hamt/dune-loop.c")
assert "tb_release_ctx_page_locks();" in dune, \
    "dune_loop must release leaked page locks before entering KVM"

latx_meson = read("target/i386/latx/meson.build")
assert "i386_ss.add(libm)" in latx_meson, \
    "LATX translator uses math functions and must link libm"

print("PASS: page-lock and libm regression invariants")
