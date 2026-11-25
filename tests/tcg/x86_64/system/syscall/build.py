#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the standalone system-transition ROM; select the execution engine externally."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--iterations', type=int, default=100000,
                    help='iterations per timed loop; use 2 for helper tracing')
parser.add_argument('--bench-only', action='store_true',
                    help='skip the correctness matrix; run the timed loops')
args = parser.parse_args()
if not 1 <= args.iterations <= 1000000000:
    parser.error('--iterations must be in 1..1000000000')
defines = ['-DTRANSITION_BENCH_ONLY'] if args.bench_only else []
source = Path(__file__).with_name('syscall.S')
args.output.parent.mkdir(parents=True, exist_ok=True)
obj = args.output.with_suffix('.o')
elf = args.output.with_suffix('.elf')
subprocess.run(['clang', '-target', 'x86_64-none-elf', '-Wa,-L', *defines,
                f'-DTRANSITION_ITERATIONS={args.iterations}', '-c', str(source),
                '-o', str(obj)], check=True)
subprocess.run(['ld', '-m', 'elf_x86_64', '-Ttext', '0x60000', '-e', '0x60000',
                '-o', str(elf), str(obj)], check=True)
subprocess.run(['objcopy', '-I', 'elf64-little', '-O', 'binary', '-j', '.text',
                str(elf), str(args.output)], check=True)
with args.output.with_suffix('.symbols').open('w') as stream:
    subprocess.run(['nm', '-an', str(elf)], stdout=stream, check=True)
