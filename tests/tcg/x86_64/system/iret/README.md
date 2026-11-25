# IRETQ system test

This 64 KiB x86-64 firmware tests actual privileged `IRETQ` instructions.
It needs no guest disk, guest OS, bootloader, or runtime libraries. Select the
emulator and its IRET optimization options outside the test.

Build with an x86-targeting Clang, GNU ld, objcopy, and nm:

    python3 build.py /tmp/iret.bin

For example, a reference QEMU TCG run is:

    qemu-system-x86_64 -machine pc -accel tcg -nodefaults -display none \
        -smp 1 -m 32 -bios /tmp/iret.bin -debugcon stdio \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 -no-reboot

A successful guest prints `IRET PASS` and writes value 0x10 to port 0xf4.
With the reference QEMU `isa-debug-exit` device this exits with process status
33. A failed assertion prints `IRET FAIL` followed by four hexadecimal lines:
case, exception vector, exception error code, and fault RIP. These describe
the last observed exception and remain zero before the first exception.
The guest then writes 0x11 to port 0xf4 (reference process status 35).

LATS builds that do not exit through that device may be controlled externally:
wait for the complete `IRET PASS` line in a fresh debugcon file, then stop and
quit through QMP. Record that completion protocol separately from a natural
device exit. After the port write the firmware enables interrupts and waits
with PAUSE so LATS can poll and acknowledge external stop requests. All tests
and timing finish before this wait. A reset, failed marker, or timeout without a complete success
marker is a failure. Do not treat a timed-out process alone as a pass.

The successful output also reports `iterations`, `kernel_ticks`, and
`user_roundtrip_ticks` as hexadecimal values. Each timed loop has 100000
iterations by default. The first loop executes a real CPL0-to-CPL0 IRET per
iteration; the second includes a user software interrupt and kernel-to-user
IRET per iteration. These are guest TSC ticks for each complete loop, not
host CPU cycles or isolated IRET instruction costs. Compare the same firmware,
CPU model, and emulator settings, repeating measurements externally.

`build.py --iterations 2 /tmp/iret-trace.bin` builds a short diagnostic run.
`--bench-only` skips the correctness matrix and runs only the two timed loops;
it prints `IRET BENCH ONLY (matrix skipped)`. This can measure an existing
implementation that fails a later matrix case, and cannot replace the full
correctness run. The assembly also accepts `-DIRET_BENCH_ITERATIONS=N` and
`-DIRET_BENCH_ONLY`. The build
retains `.o`, `.elf`, and `.symbols` files beside the ROM. The ELF text VMA is
0x60000, matching execution from RAM. Local labels are retained so that
`objdump -d /tmp/iret-trace.elf` identifies each IRET and its return labels.

The kernel stack is at `ffff800000028000`; the GDT is normally at
`ffff800000008000`. Both use a high canonical alias of low RAM. Ordinary
frames occupy one 4 KiB page, while dedicated cases cross a page. These
conditions let translators with a high-half kernel IRET fast path exercise
that path. Boot also enables CR0.WP, CR4.PAE/PSE, and EFER.LME/NXE; LATS
requires NXE among its HAMT page-mode conditions. An otherwise identical ROM
without NXE can pass through the ordinary IRET helper and does not establish
fast-path coverage. Instrument the emulator externally to confirm which cases
actually use its fast path; the firmware does not infer that from timing.

The cases are:

| Case | Check |
| --- | --- |
| 1 | CPL0 return, non-null SS; RIP/RSP/SS/RFLAGS, 15 GPRs, all 16 XMM registers |
| 2 | CPL0 return with a null SS |
| 3 | CPL0-to-CPL3 return; RIP/RSP/CS/SS/RFLAGS, 15 GPRs, all XMM registers; subsequent CPL3 IRET and software interrupt back to the kernel |
| 4–5 | Null CS and data selector used as CS: #GP with the correct selector code |
| 6–8 | CPL3 null SS, wrong SS RPL, and code selector used as SS |
| 9–10 | Nonpresent CS and SS: #NP with the correct selector code |
| 11 | CS beyond the GDT limit |
| 12–13 | Page-split frame success and missing SS-tail page |
| 14 | Read-only frame page remains readable to supervisor IRET |
| 15 | Current NT flag: #GP before state is committed |
| 16 | Returned AC/ID/IOPL flags and ignored VM flag in long mode |
| 17–19 | Page-split GDT success, missing CS page, missing SS page |
| 20 | Entire frame page missing |
| 21 | Invalid CS takes precedence over an inaccessible RSP/SS tail |
| 22–23 | Nonwritable SS and mismatched CS descriptor privilege |
| 24 | Missing RFLAGS takes precedence over invalid CS |
| 25 | Two self-directed local-APIC NMIs: the second remains blocked inside the first handler and arrives after IRET |
| 26 | A pending self-directed APIC IRQ stays blocked with IF=0 and arrives after IRET restores IF=1 |
| 27 | CPL3 invalidates supervisor DS/ES, while null FS/GS preserve their distinct 64-bit bases |
| 28–29 | With XSAVE+MPX, opposite BNDCFGS/BNDCFGU preserve settings switch correctly at CPL3; XSAVE checks the bounds after a conditional branch |
| 30 | DPL0 conforming readable code loaded into DS/ES/FS/GS retains all selectors at CPL3; non-null FS/GS also retain their distinct bases (runs before optional MPX cases) |

Expected exceptions check vector, error code, faulting IRET RIP, original
RSP/CS/SS/RFLAGS, and preserved RBX/R12; page faults also check CR2. Separate
interrupt-stack-table stacks permit checking an inaccessible IRET stack.
Exception recovery restores the fixture page mappings and GDT before resuming.
The test checks the existing QEMU helper's #NP behavior for a nonpresent SS.

The APIC and SIMD checks require an APIC/SSE2-capable CPU model, such as
`qemu64`. The MPX cases additionally require CPUID XSAVE and MPX; the firmware
prints an explicit skip otherwise. Reference QEMU's `-cpu max` runs them;
`-cpu qemu64` skips them. They use XSETBV, XRSTOR, and XSAVE in the guest,
without external emulator-state changes.

This firmware does not cover 16/32-bit IRET, VM86/task returns, SVM interception,
or debugging traps. Existing general x86 system tests and OS boots remain necessary.
