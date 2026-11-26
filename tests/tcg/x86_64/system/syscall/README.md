# System-call transition firmware

This standalone 64 KiB ROM executes actual privileged `SYSCALL`, `SYSRETQ`,
`SYSRETL`, `SYSENTER`, `SYSEXITL`, and `SYSEXITQ` instructions. It needs no
OS, disk, bootloader, runtime library, or execution scheduler. Select the
emulator, CPU model, and optimization options externally.

Build using an x86-targeting Clang, GNU ld, objcopy, and nm:

    python3 build.py /tmp/syscall.bin
    python3 build.py --iterations 2 /tmp/syscall-trace.bin
    python3 build.py --bench-only --iterations 1000000 /tmp/syscall-bench.bin

The output is a 64 KiB ROM plus `.o`, `.elf`, and `.symbols` files. The ELF
text address is 0x60000, matching execution from RAM. Local symbols identify
the tested instructions and their return labels.

Example reference execution:

    qemu-system-x86_64 -machine pc -accel tcg -cpu max,vendor=GenuineIntel \
        -nodefaults -display none -smp 1 -m 32 -bios /tmp/syscall.bin \
        -debugcon stdio -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot

Success prints `TRANSITION PASS` and writes 0x10 to port 0xf4, producing
reference QEMU exit status 33. Failure prints `TRANSITION FAIL` and four
hexadecimal lines: case number, last exception vector, last exception error
code, and last fault RIP. It writes 0x11 to port 0xf4 (exit status 35).
The last exception fields can describe an earlier expected exception when
an ordinary assertion fails. A timeout or reset is a failure. An emulator
without the exit device can be stopped externally only after its fresh
log contains the complete success line. The final STI/PAUSE loop permits
external stop polling after all checks and measurements have finished.

The ROM boots into long mode with CR0.WP, CR4.PAE/PSE/OSFXSR, and EFER.NXE.
Kernel stack and GDT have high canonical aliases at ffff800000028000 and
ffff800000008000. User RSP is 0x38000. LSTAR, CSTAR, and SYSENTER_EIP use high
canonical code aliases. STAR contains deliberately unaligned selector values
(kernel 0x0b, user 0x1b); SYSENTER_CS is also tested with low RPL bits set.
The resulting selectors are:

| Instruction | CS | SS |
| --- | --- | --- |
| SYSCALL or SYSENTER entry | 0x08 | 0x10 |
| SYSRETQ | 0x2b | 0x23 |
| SYSRETL or SYSEXITL | 0x1b | 0x23 |
| SYSEXITQ | 0x2b | 0x33 |

## Correctness checks

| Case | Check |
| --- | --- |
| 1 | CPL3 SYSCALL to LSTAR and SYSRETQ return; exact next-instruction RCX, original R11 flags, unchanged entry RSP, resulting RIP/CS/SS/RFLAGS/RSP, 13 other GPRs, all 16 XMM registers, FS/GS bases |
| 2 | SYSRETL selects compatibility code and truncates return RIP to ECX while RCX has nonzero upper bits; flags, six other accessible GPRs, first eight XMM registers, FS/GS bases, stack, CS/SS |
| 3–4 | EFER.SCE=0 causes #UD for kernel SYSCALL and SYSRETQ |
| 5 | CPL3 SYSRETQ causes #GP(0) |
| 6–7 | Zero SYSENTER_CS causes #GP(0) on Intel; AMD long mode rejects SYSENTER/SYSEXIT with #UD first |
| 8 | CPL3 SYSEXITQ causes #GP(0) on Intel and #UD on AMD long mode |
| 9 | Compatibility user SYSENTER and SYSEXITL return; entry/return flags, CS/SS/RIP/RSP, RCX/RDX targets, GPRs, XMM state, FS/GS bases |
| 10 | 64-bit user SYSENTER and REX.W SYSEXITQ return; full entry/return state including GPRs and all 16 XMM registers |
| 11 | AMD compatibility SYSCALL selects CSTAR and SYSRETL returns to the exact next instruction with saved RCX/R11; Intel compatibility SYSCALL raises #UD |
| 12–13 | SYSRETQ with R11.TF=1 delivers #DB at the first user instruction before it executes; TF=0 executes that instruction without a false trap |
| 14–15 | CPL0 and CPL3 POPFQ(TF=1) immediately followed by SYSCALL; FMASK clears TF, the kernel entry executes without #DB, and R11 retains the old TF |
| 16–17 | LSTAR changes A/B/A, reusing the same source SYSCALL in CPL0 and separately CPL3; each transition selects the new target and preserves the exact return RCX/R11, entry CS/SS, and source stack |
| 18–19 | SYSRETL with R11.TF=1 delivers #DB before the first compatibility instruction; TF=0 executes normally |

Expected exceptions check vector, zero error code, original RIP/CS/SS/RSP,
original flags excluding the RF bit added by fault delivery, and preserved
RBX/R12. The #DB checks also validate the first target RIP, new CS/SS, new
RSP and new flags; a target-side marker proves that the first instruction has
not executed. They resume at a separate kernel continuation only after
validation. Full runs additionally print `TRANSITION TF/LSTAR PASS` after
these checks.
The ordinary SYSCALL tests include MOV/LEA before SYSCALL after POPF, so the
return-address check also covers a nonfirst instruction in a translation block.

The SYSCALL flag expectation follows the existing QEMU helper behavior:
R11 receives the complete original flags (0x647), while with FMASK=0x600 the
entry flags are 0x2, including cleared arithmetic flags. SYSRETQ restores the
R11 arithmetic flags. SYSENTER preserves arithmetic flags/DF while clearing
IF; SYSEXIT preserves the flags supplied by the kernel fixture. The test does
not require changing those existing helper semantics.

Reference QEMU 10.2.2 passes with `-cpu max`, `-cpu qemu64`, and each model
with `,vendor=GenuineIntel`. AMD vendor models print an explicit skip for the
normal long-mode SYSENTER/SYSEXIT cases; their #UD behavior remains tested.
Intel vendor models actually execute both 32-bit and 64-bit SEP returns.

## Measurements and scope

The default is 100000 iterations per timed loop. Each iteration completes
both entry and return. A final INT80 returns to the kernel to read the clock;
startup, that final interrupt, and fixture work add fixed overhead. Output
reports guest TSC ticks for each complete loop, not isolated instruction
latency or host CPU cycles. Repeat measurements externally with identical
ROM, CPU model, and emulator settings.

The named loops are `syscall_sysretq_ticks`, `cstar_sysretl_ticks` (AMD),
`int80_sysretl_ticks` (Intel), and Intel-only `sysenter_sysexitl_ticks` and
`sysenter_sysexitq_ticks`. Intel does not support compatibility SYSCALL, so
its SYSRETL loop uses INT80 entry; do not compare that loop directly against
the AMD CSTAR loop. `--bench-only` prints an explicit matrix-skipped marker
and cannot replace a complete correctness run.

This fixture exercises long mode and its compatibility submode. It does not
cover legacy protected mode with LMA=0, 16-bit operand forms, SVM interception,
hardware breakpoints or other debug traps, noncanonical instruction/stack targets, MPX, or every reserved
flag combination. General system tests and real OS regressions remain needed.
