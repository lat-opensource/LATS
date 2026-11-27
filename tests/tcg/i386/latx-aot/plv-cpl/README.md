# HAMT CPL0/CPL3 PLV regression

The kernel module exposes `/dev/plv_tlb`. Its `mmap` callback installs a page
in the calling process as supervisor-only. `PLV_TEST_TOUCH` then uses
`get_user()` in the same process at CPL0, which primes the hardware TLB for the
exact user GVA without another TLB flush.

The user program pins itself to CPU 0, verifies a normal user page at CPL3,
then repeatedly preloads the supervisor page at CPL0 and reads it at CPL3. A
pass requires SIGSEGV with `SEGV_ACCERR`, the expected `si_addr`, and x86 page
fault error bits `P=1, U/S=1, W/R=0` on every iteration.

Build the two artifacts in a matching Ubuntu guest. It is simplest to do this
in a non-HAMT boot, because compiling under PLV is unrelated to the test and is
much slower:

```sh
init=/home/ng/plv-cpl/build-in-guest.sh
```

Then boot LATS with PLV enabled and HAMT enabled for both CPL0 and CPL3, using
the prepared module and program:

```text
init=/home/ng/plv-cpl/run-minimal.sh
hamtconfig=0x37e9,pcidasid=off,pcidcache=off
```

The program loads the module, mmaps the supervisor-only page, preloads it with
`get_user()` at CPL0, then execs into the freestanding CPL3 program. The expected
terminal output ends with:

```text
USER_MIN=preload
USER_MIN=read-expect-fault
plv_user[1]: segfault at ... error 5
```

Error code 5 means x86 page-fault bits `P=1`, `U/S=1`, `W/R=0`. The guest kernel
then panics because the freestanding program is PID 1; that panic is expected.

Use two distinct negative controls:

- HAMT PLV-off control: `hamt=1,hamtconfig=0x33e9`. Verify that the log says
  `hamt enabled = 0x1` and `PLV support = 0`. If the old PLV0-only HAMT path
  retains the preloaded entry across the CPL transition, the CPL3 read should
  incorrectly succeed.
- True HAMT-off control: `hamt=0`. Verify that the log says
  `hamt enabled = 0x0`. The normal software MMU is expected to produce the same
  `segfault ... error 5`; a successful CPL3 read here would indicate a separate
  non-HAMT MMU problem.
