# PCID cache-bank design

Hardware ASID retention alone still loses the software lookup state on a
CR3 reload. Cache banks preserve that state without adding a PCID or version
comparison to each lookup. Valid no-flush transitions change a per-vCPU root
rather than clearing or copying entry arrays. Each of the 16 hardware ASID
slots owns its software TLB descriptors/tables, fast instruction caches,
ordinary jump cache and compact NJC. Bank 0 is an additional untagged bank.
Lookups retain their existing entry formats and comparisons. The generic
TLB's inline descriptors are saved/restored; their backing tables stay in
the bank. Fast instruction and jump lookups load the active bank root.

A retained bank requires both an architectural no-flush request with an
exact CR3 identity and a current software invalidation generation. A missing,
recycled or expired bank is cleared before reuse, and its hardware tag is
invalidated. With a tagged context active, INVPCID type 0 invalidates the
address in the target ASID and software bank, including an inactive bank.
The page walkers record large-leaf regions at fill time. A bitmap collision
or a possible large mapping widens the invalidation to the target context,
even if the guest has since split the mapping. Software banks save the
bitmap with their descriptors; ASID-only mode shares a conservative bitmap.
Jump-cache invalidation includes the preceding page for crossing TBs;
non-page hashes and multi-page TU dependencies require a full jump-cache
clear. An inactive bank defers that clear until selection, coalescing
requests while its cached code cannot run.
Type 1 always invalidates the target context. Whole inactive contexts are
marked stale before reuse. A different PCID in the same slot remains valid.
Generic mapping invalidations expire inactive banks lazily. With tagged
HAMT enabled, a guest INVLPG invalidates the address under the active
ASID and each reusable ASID: guest global pages can be cached under
several tags. It clears that page in each live software bank. A possible
large mapping in an inactive bank expires that context before reuse. A
hit in the active bank clears its ASID and software tables. In ASID-only
mode the large-page bitmap is shared, so a hit requires a full local flush. Without tagged HAMT, the existing large-page bitmap
also allows unrelated hardware translations to survive an ordinary INVLPG.
Physical dirty-tracking notifications update matching writable TLB entries in every
live bank under the TLB lock, preserving unrelated mappings and jump caches.
The dirty scan holds the CPU-list lock across traversal so unplug waits for
readers before freeing the active and saved tables.
A hardware-only flush (such as new SMC page protection) does not discard a
software bank whose identity and mapping generation are still valid.
Physical TB invalidation scans all allocated jump-cache banks,
including inactive banks. Jump-cache roots are published after their entries
have been initialized.
All software TLB bank transitions and generation changes use the existing
per-CPU TLB lock. The vCPU owns selection and writes. Remote TB invalidators
read the published bank root once under RCU. Teardown can detach that root
while a reader is using it. The CPU object, bank metadata and jump caches
stay alive through the RCU grace period.

Reset and VM-state load expire the banks. CPU unrealize frees every backing
table exactly once. Software cache banks are not migrated. The switch still
resets the RAS and TU mapping-check generation. The first supported profile
is x86-64 HAMT DMW with pinned vCPUs and compact NJC. Saved AOT code records
the bank-layout option and relocates fast iTLB indices. Disable the banks with `pcidcache=off` to exercise the
conservative software-cache path.

The ROM tests and runner in `tests/tcg/x86_64/system/pcid/` check architectural
faults, slot reuse, permissions, global invalidation, cross-vCPU SMC and reset.
They check reuse counters as well as results: an implementation which always
clears its banks can produce correct output without testing retained entries.
