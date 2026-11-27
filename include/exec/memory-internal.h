/*
 * Declarations for functions which are internal to the memory subsystem.
 *
 * Copyright 2011 Red Hat, Inc. and/or its affiliates
 *
 * Authors:
 *  Avi Kivity <avi@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 */

/*
 * This header is for use by exec.c, memory.c and accel/tcg/cputlb.c ONLY,
 * for declarations which are shared between the memory subsystem's
 * internals and the TCG TLB code. Do not include it from elsewhere.
 */

#ifndef MEMORY_INTERNAL_H
#define MEMORY_INTERNAL_H

#include "cpu.h"

#ifndef CONFIG_USER_ONLY
static inline AddressSpaceDispatch *flatview_to_dispatch(FlatView *fv)
{
    return fv->dispatch;
}

static inline AddressSpaceDispatch *address_space_to_dispatch(AddressSpace *as)
{
    return flatview_to_dispatch(address_space_to_flatview(as));
}

FlatView *address_space_get_flatview(AddressSpace *as);
void flatview_unref(FlatView *view);

extern const MemoryRegionOps unassigned_mem_ops;

void flatview_add_to_dispatch(FlatView *fv, MemoryRegionSection *section);
AddressSpaceDispatch *address_space_dispatch_new(FlatView *fv);
void address_space_dispatch_compact(AddressSpaceDispatch *d);
void address_space_dispatch_free(AddressSpaceDispatch *d);

void mtree_print_dispatch(struct AddressSpaceDispatch *d,
                          MemoryRegion *root);

bool memory_region_try_read_lockless(MemoryRegion *mr, hwaddr addr,
                                     uint64_t *value, MemOp op,
                                     MemTxAttrs attrs);
bool memory_region_try_write_lockless(MemoryRegion *mr, hwaddr addr,
                                      uint64_t value, MemOp op,
                                      MemTxAttrs attrs);

typedef MemoryRegion MemoryRegionSplitPause;

MemoryRegionSplitPause *subpage_pause_split(MemoryRegion *mr, hwaddr addr,
                                            unsigned size);

static inline MemoryRegionSplitPause *memory_region_pause_split(
    MemoryRegion *mr, hwaddr addr, unsigned size)
{
    return mr->ops->mmio_split_pause &&
           mr->ops->mmio_split_pause(mr->opaque, addr, size) ? mr : NULL;
}

static inline void memory_region_resume_split(MemoryRegionSplitPause *mr)
{
    mr->ops->mmio_split_resume(mr->opaque);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(MemoryRegionSplitPause, memory_region_resume_split)
#endif
#endif
