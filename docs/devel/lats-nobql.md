# LATS NoBQL Architecture

## Overview

LATS NoBQL eliminates the global Big QEMU Lock (BQL / `qemu_global_mutex`) from
hot execution, device emulation, interrupt delivery, timer dispatch, memory
topology updates, vCPU lifecycle, main-loop polling, and display refresh paths
in `x86_64-softmmu`.

By replacing coarse-grained BQL serialization with fine-grained subsystem locks
ordered under a strict 6-rank lock hierarchy, multi-vCPU guests (including
Windows 10 desktop workloads over GTK and VNC) run concurrently across host
cores without global lock contention.

## Runtime Controls

| Environment Variable | Default | Description |
|----------------------|---------|-------------|
| `LATS_NOBQL` | `1` (enabled) | Master switch for the BQL-free architecture (`lats_nobql_enabled()`). Set `LATS_NOBQL=0` to restore legacy BQL mode for emergency fallback or A/B comparison. |
| `LATS_LOCKDEP` | `0` (disabled) | Enables the lightweight runtime 6-rank lock-order checker (`util/lockdep-lite.c`). Zero hot-path overhead when disabled (`unlikely(lats_lockdep_active)`). |

## The 5 Architectural Pillars

1. **Per-Device Locking & BQL-Free Interrupt DAG**
   - `MemoryRegion` and `PortioList` support per-device `device_lock` dispatch
     instead of acquiring BQL around MMIO and port I/O handlers.
   - Interrupt delivery follows a BQL-free DAG (`include/exec/irq-commit.h`,
     `include/hw/i386/hpet-irq0.h`) across PCI/MSI-X, IOAPIC, PIC (i8259), and
     Local APIC.
   - Core platform devices (ACPI, PCI/PCIe host bridges, VirtIO-PCI, USB
     UHCI/xHCI, PS/2/HID, RTC/DMA/fw_cfg) use local device locks.

2. **BQL-Free Timers & IOThread Offloading**
   - `QEMU_TIMER_ATTR_NO_BQL` (`include/qemu/timer.h`, `util/qemu-timer.c`)
     allows timer callbacks (HPET, PIT i8254, RTC, xHCI) to execute without
     acquiring BQL.
   - VirtIO-Net, Slirp networking, VirtIO-Serial, and QGA chardev I/O offload
     packet/stream processing to dedicated IOThreads and lockless wake paths.

3. **Memory Topology, RCU & Per-vCPU Concurrency**
   - `memory_topology_mutex` (`softmmu/memory.c`) serializes FlatView and
     MemoryRegion topology updates independently of BQL.
   - `RCU_HEAD_NO_BQL` / `call_rcu_nobql()` (`include/qemu/rcu.h`, `util/rcu.c`)
     reclaims RCU-protected structures without bouncing through BQL.
   - Per-vCPU `state_lock` (`softmmu/cpus.c`, `cpus-common.c`) and
     `option_irq_kick_fast` enable lockless vCPU interrupt delivery and atomic
     `halt_cond` wakeups.

4. **BQL-Free Main Loop & VM Lifecycle**
   - `vm_lifecycle_lock` (`include/qemu/lockdep-lite.h`) serializes VM
     start/stop/reset/shutdown transitions independently of BQL.
   - `util/main-loop.c` performs deadline-driven timer/HPET repolling and
     event-loop dispatch without holding BQL across idle or poll intervals.
   - QMP and runstate transitions coordinate through `vm_lifecycle_lock`.

5. **BQL-Free Display Paths (GTK / VGA / VNC)**
   - VGA MMIO/port/DAC registers and EDID regions use local device locking
     (`hw/display/vga.c`, `hw/display/vga-pci.c`, `hw/display/edid-region.c`).
   - GTK lifecycle and display refresh (`ui/gtk.c`, `ui/console.c`) run cleanly
     without BQL stalls.
   - VNC framebuffer comparison, scanout publishing, and client I/O offload to
     worker threads (`ui/vnc.c`, `ui/vnc-jobs.c`).

## 6-Rank Lock Hierarchy

Defined in `include/qemu/lockdep-lite.h`:

| Rank | Enum Constant | Scope / Examples |
|------|---------------|------------------|
| 1 | `LOCK_RANK_BQL` | Legacy BQL (`qemu_global_mutex`) for unmigrated or fallback paths |
| 2 | `LOCK_RANK_VM_LIFECYCLE` | `vm_lifecycle_lock` (VM start/stop/reset/shutdown) |
| 3 | `LOCK_RANK_MEM_TOPO` | `memory_topology_mutex` (FlatView / MemoryRegion transactions) |
| 4 | `LOCK_RANK_DEVICE` | Per-device `device_lock` (PCI, VirtIO, VGA, ACPI, USB, PS/2) |
| 5 | `LOCK_RANK_INTC` | Interrupt controllers (IOAPIC, i8259 PIC, Local APIC, IRQ commit) |
| 6 | `LOCK_RANK_LEAF` | Leaf locks (per-vCPU `state_lock`, condvars, fast queues) |

**Invariant:** A thread holding a lock of rank `R` may only acquire locks of
rank `> R` (or recursively re-acquire the same recursive mutex). Any lock
acquisition with rank `< current_max_rank` is flagged immediately when running
with `LATS_LOCKDEP=1`.

## Verification & Debugging

### Build (with GTK support)

```bash
../configure --target-list=x86_64-softmmu --enable-gtk --enable-spice \
    --enable-sdl --enable-slirp --enable-libusb --enable-latx-ndebug \
    --disable-werror --disable-docs --disable-plugins --enable-tools \
    --enable-latx-opt --enable-latx-opt-nofp --enable-latx-opt-eflags \
    --enable-latx-opt-tu --enable-latx-opt-bql --enable-latx-opt-mmio-lockless \
    --enable-latx-hack-mem --enable-atomic
ninja -j8 lats-x86_64
```

### Runtime Validation

- **Default NoBQL mode:** Run `lats-x86_64` normally (`LATS_NOBQL` defaults to `1`).
- **Lock-order audit:** Run with `LATS_LOCKDEP=1` to verify zero rank inversions
  under guest boot, GTK display refresh, and I/O load.
- **Legacy BQL fallback / A/B test:** Run with `LATS_NOBQL=0` to compare against
  the legacy BQL execution path without rebuilding.
