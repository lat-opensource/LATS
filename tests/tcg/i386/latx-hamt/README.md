# HAMT MMIO ADE guest test

`mmio-ade.S` is an x86-64 Linux guestside trigger for the HAMT MMIO
injected-ADE retry path. It covers ordinary base plus displacement loads,
indexed loads, `rd == rj`, `rd == rk`, and `rj == rk`. The final case maps a
page at `0x200000000000` and uses `A + A` with `A = 0x100000000000`.

The translator can lower a single x86 memory operation in different ways, so
an x86 instruction cannot guarantee a particular generated LoongArch opcode.
Running this program still verifies that the translated DMW fault path
completes and preserves each guest-visible result.

Build a static x86-64 binary and copy it into the guest:

```sh
x86_64-linux-gnu-gcc -nostdlib -static -Wl,--build-id=none \
    tests/tcg/i386/latx-hamt/mmio-ade.S -o mmio-ade
```

Run `mmio-ade` inside the LATS guest. A successful run prints:

```text
PASS: HAMT MMIO ADE guest aliases
```

`mmio-ade-vector.S` covers SSE 128-bit loads/stores and, when CPUID reports
AVX support, AVX 256-bit loads/stores. It is intended to exercise translated
`VLD/VST`, `VLDX/VSTX`, `XVLD/XVST`, and `XVLDX/XVSTX` operands when the
active translator and KVM version can produce them.

```sh
x86_64-linux-gnu-gcc -nostdlib -static -Wl,--build-id=none \
    tests/tcg/i386/latx-hamt/mmio-ade-vector.S -o mmio-ade-vector
```

A successful run prints:

```text
PASS: HAMT vector MMIO ADE
```
