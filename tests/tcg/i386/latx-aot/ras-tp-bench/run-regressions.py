#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2024-2026 LATS Project Authors
#
# SPDX-License-Identifier: GPL-2.0-only

"""Run the RAS implementation and benchmark protocol regressions."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[5]
TESTS = [
    "latx-aot/test-hamt-ptr-memory.py",
    "latx-aot/test-hamt-poison-entry.py",
    "latx-aot/test-mtcc-region-reset.py",
    "latx-aot/test-ras-tp-boundary.py",
    "latx-aot/test-ras-tp-preserve.py",
    "latx-aot/test-ras-tp-invalidate-scope.py",
    "latx-aot/test-ras-tp-far-call.py",
    "latx-aot/test-ras-tp-layout.py",
    "latx-aot/test-ras-tp-lifetime.py",
    "latx-aot/test-ras-tp-reloc.py",
    "latx-aot/test-tu-ras-state.py",
    "latx-aot/test-aot-ras-save.py",
    "latx-aot/test-aottucp-cache-policy.py",
    "latx-aot/test-cross-page-tu-corruption.py",
    "latx-aot/test-jit-tu-page-boundaries.py",
    "latx-aot/test-jit-tu-capacity.py",
    "latx-ras/check-aot-ras-link-policy.py",
    "latx-aot/test-host-load-guard.py",
    "latx-aot/ras-tp-bench/spec2000/test-output.py",
    "latx-aot/ras-tp-bench/test-qmp-deadline.py",
]

def main():
    for test in TESTS:
        subprocess.run([sys.executable, str(ROOT / "tests/tcg/i386" / test)],
                       cwd=ROOT, check=True)
    print(f"PASS: {len(TESTS)} RAS and benchmark regression scripts")

if __name__ == "__main__":
    main()
