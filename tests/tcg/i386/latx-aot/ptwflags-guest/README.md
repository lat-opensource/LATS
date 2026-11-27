# HAMT ptwflags guest behavior suite

This suite runs real privileged and unprivileged guest accesses through LATS.
It complements the host state-machine test and the refill-emitter inspection.

| Case | ptwflags path exercised |
|---|---|
| `baseline` | flags clear: normal readable and writable data |
| `smc` | code writes set bit 0 and group NO2M, then execution must use 4K fallback |
| `vga` | VGA memory must retain MMIO/VGA fallback semantics |
| `mmio` | the PCI hole must reject direct HAMT access and use the device path |
| `super_read` | a CPL0 TLB entry must not satisfy a CPL3 supervisor-page read |
| `super_write` | the same page must report a user write fault, not a HAMT bypass |
| `super_write_cold` | a cold CPL3 write must report the same fault without preload |
| `super_exec` | instruction fetch must report a user instruction-fetch fault |
| `readonly_write` | a not-present, read-only user page must report fault error 6 |
| `user_present_readonly_write` | a present read-only user page must report error 7 |

The kernel module exposes an allocated page, one VGA page, and one unassigned
PCI-hole page through `/dev/ptwflags_tlb`. The supervisor mapping clears the
PTE user bit. An ioctl first reads that exact user GVA at CPL0 so the following
CPL3 access specifically tests PLV tagging rather than only a cold refill. The
write cases map the guest VMA read-only; mapping it writable would make the
guest kernel repeatedly try to repair a supervisor-only PTE instead of
delivering the user fault.

Place this directory at `/home/ng/ptwflags-guest` in the test QCOW2 image. The
runner first builds the module and freestanding user modes with `hamt=0`, so
compilation cannot depend on the path under test. It then boots each case with
the ptwflags configuration and a temporary QEMU snapshot. Running the access
case as PID 1 avoids depending on guest fork/wait behavior in the measurement.

From the repository root:

```sh
python3 tests/tcg/i386/latx-aot/ptwflags-guest/run-linux.py \
    --binary build/lats-x86_64 \
    --disk run/ubuntu16s.x64.m2048.spec2k.32.64.ok.img \
    --kernel run/vmlinuz-4.4.0-186-generic-x64 \
    --output run/ptwflags-guest.$(date +%Y%m%d-%H%M%S)
```

The default LATS configuration is `hamtconfig=0x3769` with AOT, TU (`tumode`)
and `tucpopt` disabled. This selects the unified ptwflags 4K/2M refill path and
enables PLV tagging.
The guest suite defaults to one vCPU. Booting this unified ptwflags
configuration with more than one vCPU currently stops after all vCPU threads
enter HAMT, before guest serial output. The host-side safety test separately
stresses concurrent protect/unprotect and MMIO invalidation; resolving the SMP
boot path remains a separate architectural issue.
The runner requires each case marker and the expected x86 page-fault error codes
`5` (user read), `6` (not-present user write), `7` (present user write), and
`15` (user instruction fetch).

The reserved permission bits 2 through 6 do not have a natural guest producer;
they are covered by the production-state harness in
`test-hamt-ptwflags.py` and by the emitted refill checks in
`test-hamt-ptwdata1m.py`.
