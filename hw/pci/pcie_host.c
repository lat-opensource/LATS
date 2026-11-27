/*
 * pcie_host.c
 * utility functions for pci express host bridge.
 *
 * Copyright (c) 2009 Isaku Yamahata <yamahata at valinux co jp>
 *                    VA Linux Systems Japan K.K.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "exec/memory.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pcie_host.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "sysemu/tcg.h"

/* a helper function to get a PCIDevice for a given mmconfig address */
static inline PCIDevice *pcie_dev_find_by_mmcfg_addr(PCIBus *s,
                                                     uint32_t mmcfg_addr)
{
    return pci_find_device(s, PCIE_MMCFG_BUS(mmcfg_addr),
                           PCIE_MMCFG_DEVFN(mmcfg_addr));
}

static void pcie_mmcfg_data_write(void *opaque, hwaddr mmcfg_addr,
                                  uint64_t val, unsigned len)
{
    PCIExpressHost *e = opaque;
    PCIBus *s = e->pci.bus;
    PCIDevice *pci_dev = pcie_dev_find_by_mmcfg_addr(s, mmcfg_addr);
    uint32_t addr;
    uint32_t limit;

    if (!pci_dev) {
        return;
    }
    addr = PCIE_MMCFG_CONFOFFSET(mmcfg_addr);
    limit = pci_config_size(pci_dev);
    pci_host_config_write_common(pci_dev, addr, limit, val, len);
}

static uint64_t pcie_mmcfg_data_read(void *opaque,
                                     hwaddr mmcfg_addr,
                                     unsigned len)
{
    PCIExpressHost *e = opaque;
    PCIBus *s = e->pci.bus;
    PCIDevice *pci_dev = pcie_dev_find_by_mmcfg_addr(s, mmcfg_addr);
    uint32_t addr;
    uint32_t limit;

    if (!pci_dev) {
        return ~0x0;
    }
    addr = PCIE_MMCFG_CONFOFFSET(mmcfg_addr);
    limit = pci_config_size(pci_dev);
    return pci_host_config_read_common(pci_dev, addr, limit, len);
}

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static bool pcie_mmcfg_try_write(void *opaque, hwaddr addr, unsigned size,
                                 uint64_t value)
{
    PCIExpressHost *e = opaque;
    PCIBus *bus = e->pci.bus;

    if (!bus || addr >= PCIE_MMCFG_SIZE_MAX || PCIE_MMCFG_BUS(addr)) {
        return false;
    }
    if (size == 1 && PCIE_MMCFG_CONFOFFSET(addr) == PCI_INTERRUPT_LINE) {
        return pci_config_try_interrupt_line_write(bus, PCIE_MMCFG_DEVFN(addr),
                                                   value);
    }
    if (size == 4 &&
        pci_config_try_unused_bar_write(bus, PCIE_MMCFG_DEVFN(addr),
                                        PCIE_MMCFG_CONFOFFSET(addr), NULL, 0)) {
        return true;
    }
    if (pci_config_try_local_write(bus, PCIE_MMCFG_DEVFN(addr),
                                   PCIE_MMCFG_CONFOFFSET(addr), size, value)) {
        return true;
    }
    if (lats_nobql_enabled()) {
        qemu_memory_topology_lock();
        pcie_mmcfg_data_write(opaque, addr, value, size);
        qemu_memory_topology_unlock();
        return true;
    }
    return false;
}

static bool pcie_mmcfg_try_read(void *opaque, hwaddr addr, MemOp op,
                                      MemTxAttrs attrs, uint64_t *value)
{
    PCIExpressHost *e = opaque;
    PCIBus *bus = e->pci.bus;
    unsigned size = memop_size(op);
    unsigned seq;
    bool absent;

    if (!bus || (!e->mmcfg_empty_read && !bus->config_read_slots) ||
        !tcg_enabled() || size > 4 ||
        addr >= PCIE_MMCFG_SIZE_MAX || (addr & (size - 1))) {
        return false;
    }

    if (PCIE_MMCFG_BUS(addr) == 0 &&
        pci_config_read_slot_try(bus, PCIE_MMCFG_DEVFN(addr),
                                 PCIE_MMCFG_CONFOFFSET(addr), size, value,
                                 NULL, 0)) {
        return true;
    }
    if (!e->mmcfg_empty_read) {
        return false;
    }

    /* q35's root is bus 0. Never dereference an unlocked device or bridge. */
    seq = seqlock_read_begin(&bus->config_topology);
    if (PCIE_MMCFG_BUS(addr) == 0) {
        absent = !qatomic_read(&bus->devices[PCIE_MMCFG_DEVFN(addr)]);
    } else {
        absent = !qatomic_read(&bus->child.lh_first);
    }
    if (!absent || seqlock_read_retry(&bus->config_topology, seq)) {
        if (lats_nobql_enabled()) {
            qemu_memory_topology_lock();
            *value = pcie_mmcfg_data_read(opaque, addr, size);
            qemu_memory_topology_unlock();
            return true;
        }
        return false;
    }

    *value = UINT64_MAX;
    return true;
}
#endif

static const MemoryRegionOps pcie_mmcfg_ops = {
    .read = pcie_mmcfg_data_read,
    .write = pcie_mmcfg_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    .try_read_lockless = pcie_mmcfg_try_read,
    .try_write_lockless = pcie_mmcfg_try_write,
#endif
};

static void pcie_host_init(Object *obj)
{
    PCIExpressHost *e = PCIE_HOST_BRIDGE(obj);

    e->base_addr = PCIE_BASE_ADDR_UNMAPPED;
    memory_region_init_io(&e->mmio, OBJECT(e), &pcie_mmcfg_ops, e, "pcie-mmcfg-mmio",
                          PCIE_MMCFG_SIZE_MAX);
}

void pcie_host_mmcfg_unmap(PCIExpressHost *e)
{
    if (e->base_addr != PCIE_BASE_ADDR_UNMAPPED) {
        memory_region_del_subregion(get_system_memory(), &e->mmio);
        e->base_addr = PCIE_BASE_ADDR_UNMAPPED;
    }
}

void pcie_host_mmcfg_init(PCIExpressHost *e, uint32_t size)
{
    assert(!(size & (size - 1)));       /* power of 2 */
    assert(size >= PCIE_MMCFG_SIZE_MIN);
    assert(size <= PCIE_MMCFG_SIZE_MAX);
    e->size = size;
    memory_region_set_size(&e->mmio, e->size);
}

void pcie_host_mmcfg_map(PCIExpressHost *e, hwaddr addr,
                         uint32_t size)
{
    pcie_host_mmcfg_init(e, size);
    e->base_addr = addr;
    memory_region_add_subregion(get_system_memory(), e->base_addr, &e->mmio);
}

void pcie_host_mmcfg_update(PCIExpressHost *e,
                            int enable,
                            hwaddr addr,
                            uint32_t size)
{
    memory_region_transaction_begin();
    pcie_host_mmcfg_unmap(e);
    if (enable) {
        pcie_host_mmcfg_map(e, addr, size);
    }
    memory_region_transaction_commit();
}

static const TypeInfo pcie_host_type_info = {
    .name = TYPE_PCIE_HOST_BRIDGE,
    .parent = TYPE_PCI_HOST_BRIDGE,
    .abstract = true,
    .instance_size = sizeof(PCIExpressHost),
    .instance_init = pcie_host_init,
};

static void pcie_host_register_types(void)
{
    type_register_static(&pcie_host_type_info);
}

type_init(pcie_host_register_types)
