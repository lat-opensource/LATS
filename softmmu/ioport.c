/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/*
 * splitted out ioport related stuffs from vl.c.
 */

#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "cpu.h"
#include "exec/ioport.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "trace.h"

struct MemoryRegionPortioList {
    Object obj;

    MemoryRegion mr;
    void *portio_opaque;
    MemoryRegionPortio *ports;
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    MemoryRegionOps *direct_ops;
#endif
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    QemuRecMutex *local_byte_lock;
    uint32_t local_byte_first;
    uint32_t local_byte_end;
#endif
};

#define TYPE_MEMORY_REGION_PORTIO_LIST "memory-region-portio-list"
OBJECT_DECLARE_SIMPLE_TYPE(MemoryRegionPortioList, MEMORY_REGION_PORTIO_LIST)

static uint64_t unassigned_io_read(void *opaque, hwaddr addr, unsigned size)
{
    return -1ULL;
}

static void unassigned_io_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
}

const MemoryRegionOps unassigned_io_ops = {
    .read = unassigned_io_read,
    .write = unassigned_io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void cpu_outb(uint32_t addr, uint8_t val)
{
    trace_cpu_out(addr, 'b', val);
    address_space_write(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED,
                        &val, 1);
}

void cpu_outw(uint32_t addr, uint16_t val)
{
    uint8_t buf[2];

    trace_cpu_out(addr, 'w', val);
    stw_p(buf, val);
    address_space_write(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED,
                        buf, 2);
}

void cpu_outl(uint32_t addr, uint32_t val)
{
    uint8_t buf[4];

    trace_cpu_out(addr, 'l', val);
    stl_p(buf, val);
    address_space_write(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED,
                        buf, 4);
}

uint8_t cpu_inb(uint32_t addr)
{
    uint8_t val;

    address_space_read(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED,
                       &val, 1);
    trace_cpu_in(addr, 'b', val);
    return val;
}

uint16_t cpu_inw(uint32_t addr)
{
    uint8_t buf[2];
    uint16_t val;

    address_space_read(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED, buf, 2);
    val = lduw_p(buf);
    trace_cpu_in(addr, 'w', val);
    return val;
}

uint32_t cpu_inl(uint32_t addr)
{
    uint8_t buf[4];
    uint32_t val;

    address_space_read(&address_space_io, addr, MEMTXATTRS_UNSPECIFIED, buf, 4);
    val = ldl_p(buf);
    trace_cpu_in(addr, 'l', val);
    return val;
}

void portio_list_init(PortioList *piolist,
                      Object *owner,
                      const MemoryRegionPortio *callbacks,
                      void *opaque, const char *name)
{
    unsigned n = 0;

    while (callbacks[n].size) {
        ++n;
    }

    piolist->ports = callbacks;
    piolist->nr = 0;
    piolist->regions = g_new0(MemoryRegion *, n);
    piolist->address_space = NULL;
    piolist->opaque = opaque;
    piolist->owner = owner;
    piolist->name = name;
    piolist->flush_coalesced_mmio = false;
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    piolist->local_registers = false;
    piolist->device_lock = NULL;
#endif
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    piolist->local_byte_lock = NULL;
#endif
}

void portio_list_set_flush_coalesced(PortioList *piolist)
{
    piolist->flush_coalesced_mmio = true;
}

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
void portio_list_set_device_lock(PortioList *piolist, QemuRecMutex *lock)
{
    piolist->device_lock = lock;
}

void portio_list_set_local_bytes(PortioList *piolist, QemuRecMutex *lock,
                                 uint32_t first, uint32_t end)
{
    piolist->local_byte_lock = lock;
    piolist->local_byte_first = first;
    piolist->local_byte_end = end;
}
#endif

void portio_list_destroy(PortioList *piolist)
{
    MemoryRegionPortioList *mrpio;
    unsigned i;

    for (i = 0; i < piolist->nr; ++i) {
        mrpio = container_of(piolist->regions[i], MemoryRegionPortioList, mr);
        object_unparent(OBJECT(&mrpio->mr));
        object_unref(mrpio);
    }
    g_free(piolist->regions);
}

static const MemoryRegionPortio *find_portio(MemoryRegionPortioList *mrpio,
                                             uint64_t offset, unsigned size,
                                             bool write)
{
    const MemoryRegionPortio *mrp;

    for (mrp = mrpio->ports; mrp->size; ++mrp) {
        if (offset >= mrp->offset && offset < mrp->offset + mrp->len &&
            size == mrp->size &&
            (write ? (bool)mrp->write : (bool)mrp->read)) {
            return mrp;
        }
    }
    return NULL;
}

static uint64_t portio_read(void *opaque, hwaddr addr, unsigned size)
{
    MemoryRegionPortioList *mrpio = opaque;
    const MemoryRegionPortio *mrp = find_portio(mrpio, addr, size, false);
    uint64_t data;

    data = ((uint64_t)1 << (size * 8)) - 1;
    if (mrp) {
        data = mrp->read(mrpio->portio_opaque, mrp->base + addr);
    } else if (size == 2) {
        mrp = find_portio(mrpio, addr, 1, false);
        if (mrp) {
            data = mrp->read(mrpio->portio_opaque, mrp->base + addr);
            if (addr + 1 < mrp->offset + mrp->len) {
                data |= mrp->read(mrpio->portio_opaque, mrp->base + addr + 1) << 8;
            } else {
                data |= 0xff00;
            }
        }
    }
    return data;
}

static void portio_write(void *opaque, hwaddr addr, uint64_t data,
                         unsigned size)
{
    MemoryRegionPortioList *mrpio = opaque;
    const MemoryRegionPortio *mrp = find_portio(mrpio, addr, size, true);

    if (mrp) {
        mrp->write(mrpio->portio_opaque, mrp->base + addr, data);
    } else if (size == 2) {
        mrp = find_portio(mrpio, addr, 1, true);
        if (mrp) {
            mrp->write(mrpio->portio_opaque, mrp->base + addr, data & 0xff);
            if (addr + 1 < mrp->offset + mrp->len) {
                mrp->write(mrpio->portio_opaque, mrp->base + addr + 1, data >> 8);
            }
        }
    }
}

static const MemoryRegionOps portio_ops = {
    .read = portio_read,
    .write = portio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.unaligned = true,
    .impl.unaligned = true,
};

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static bool portio_try_read(void *opaque, hwaddr addr, MemOp op,
                            MemTxAttrs attrs, uint64_t *value)
{
    MemoryRegionPortioList *mrpio = opaque;
    unsigned size = memop_size(op);
    const MemoryRegionPortio *port;

    if (attrs.memory ||
        trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_READ)) {
        return false;
    }
    port = find_portio(mrpio, addr, size, false);
    if (port && port->try_read) {
        return port->try_read(mrpio->portio_opaque, port->base + addr,
                              size, value);
    }
    if (size == 2) {
        port = find_portio(mrpio, addr, 1, false);
        if (port && port->try_read && addr + 1 < port->offset + port->len) {
            return port->try_read(mrpio->portio_opaque, port->base + addr,
                                  2, value);
        }
    }
    return false;
}

static bool portio_try_write(void *opaque, hwaddr addr, unsigned size,
                             uint64_t value)
{
    MemoryRegionPortioList *mrpio = opaque;
    const MemoryRegionPortio *port;

    port = find_portio(mrpio, addr, size, true);
    if (port && port->try_write) {
        return port->try_write(mrpio->portio_opaque, port->base + addr,
                               size, value);
    }
    if (size == 2) {
        port = find_portio(mrpio, addr, 1, true);
        if (port && port->try_write && addr + 1 < port->offset + port->len) {
            return port->try_write(mrpio->portio_opaque, port->base + addr,
                                   2, value);
        }
    }
    return false;
}

#endif

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static bool portio_local_access_lockless(void *opaque, hwaddr addr,
                                        unsigned size, bool is_write,
                                        MemTxAttrs attrs)
{
    MemoryRegionPortioList *mrpio = opaque;

    return size == 1 && addr >= mrpio->local_byte_first &&
           addr < mrpio->local_byte_end;
}

typedef QemuRecMutex PortioByteLock;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(PortioByteLock, qemu_rec_mutex_unlock)

static QemuRecMutex *portio_local_split_lock(void *opaque, hwaddr addr,
                                             unsigned size)
{
    MemoryRegionPortioList *mrpio = opaque;

    if (addr >= mrpio->local_byte_end ||
        addr + size <= mrpio->local_byte_first) {
        return NULL;
    }
    return mrpio->local_byte_lock;
}

static PortioByteLock *portio_lock_bytes(void *opaque, hwaddr addr,
                                        unsigned size)
{
    PortioByteLock *lock = portio_local_split_lock(opaque, addr, size);

    if (lock) {
        qemu_rec_mutex_lock(lock);
    }
    return lock;
}

static uint64_t portio_read_local(void *opaque, hwaddr addr, unsigned size)
{
    g_autoptr(PortioByteLock) lock = portio_lock_bytes(opaque, addr, size);

    return portio_read(opaque, addr, size);
}

static void portio_write_local(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    g_autoptr(PortioByteLock) lock = portio_lock_bytes(opaque, addr, size);

    portio_write(opaque, addr, data, size);
}

static const MemoryRegionOps portio_local_ops = {
    .read = portio_read_local,
    .write = portio_write_local,
    .access_lockless = portio_local_access_lockless,
    .mmio_split_lock = portio_local_split_lock,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.unaligned = true,
    .impl.unaligned = true,
};
#endif

static void portio_list_add_1(PortioList *piolist,
                              const MemoryRegionPortio *pio_init,
                              unsigned count, unsigned start,
                              unsigned off_low, unsigned off_high)
{
    const MemoryRegionOps *ops = &portio_ops;
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    bool direct_read = false;
    bool direct_write = false;
#endif
    MemoryRegionPortioList *mrpio;
    Object *owner;
    char *name;
    unsigned i;

    /* Copy the sub-list and null-terminate it.  */
    mrpio = MEMORY_REGION_PORTIO_LIST(
                object_new(TYPE_MEMORY_REGION_PORTIO_LIST));
    mrpio->portio_opaque = piolist->opaque;
    mrpio->ports = g_malloc0(sizeof(MemoryRegionPortio) * (count + 1));
    memcpy(mrpio->ports, pio_init, sizeof(MemoryRegionPortio) * count);
    memset(mrpio->ports + count, 0, sizeof(MemoryRegionPortio));

    /* Adjust the offsets to all be zero-based for the region.  */
    for (i = 0; i < count; ++i) {
        mrpio->ports[i].offset -= off_low;
        mrpio->ports[i].base = start + off_low;
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        direct_read |= mrpio->ports[i].try_read != NULL;
        direct_write |= mrpio->ports[i].try_write != NULL;
#endif
    }

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    if (piolist->local_byte_lock &&
        piolist->local_byte_first < start + off_high &&
        piolist->local_byte_end > start + off_low) {
        unsigned base = start + off_low;

        mrpio->local_byte_lock = piolist->local_byte_lock;
        mrpio->local_byte_first = MAX(piolist->local_byte_first, base) - base;
        mrpio->local_byte_end =
            MIN(piolist->local_byte_end, start + off_high) - base;
        ops = &portio_local_ops;
    }
#endif

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    if (piolist->local_registers && (direct_read || direct_write)) {
        mrpio->direct_ops = g_new(MemoryRegionOps, 1);
        *mrpio->direct_ops = *ops;
        mrpio->direct_ops->try_read_lockless = direct_read ? portio_try_read : NULL;
        mrpio->direct_ops->try_write_lockless = direct_write ? portio_try_write : NULL;
        ops = mrpio->direct_ops;
    }
#endif

    /*
     * The MemoryRegion owner is the MemoryRegionPortioList since that manages
     * the lifecycle via the refcount
     */
    memory_region_init_io(&mrpio->mr, OBJECT(mrpio), ops, mrpio,
                          piolist->name, off_high - off_low);

    /* Reparent the MemoryRegion to the piolist owner */
    object_ref(&mrpio->mr);
    object_unparent(OBJECT(&mrpio->mr));
    if (!piolist->owner) {
        owner = container_get(qdev_get_machine(), "/unattached");
    } else {
        owner = piolist->owner;
    }
    name = g_strdup_printf("%s[*]", piolist->name);
    object_property_add_child(owner, name, OBJECT(&mrpio->mr));
    g_free(name);

    if (piolist->flush_coalesced_mmio) {
        memory_region_set_flush_coalesced(&mrpio->mr);
    }
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    if (piolist->device_lock) {
        memory_region_set_device_lock(&mrpio->mr, piolist->device_lock);
    }
#endif
    memory_region_add_subregion(piolist->address_space,
                                start + off_low, &mrpio->mr);
    piolist->regions[piolist->nr] = &mrpio->mr;
    ++piolist->nr;
}

void portio_list_add(PortioList *piolist,
                     MemoryRegion *address_space,
                     uint32_t start)
{
    const MemoryRegionPortio *pio, *pio_start = piolist->ports;
    unsigned int off_low, off_high, off_last, count;

    piolist->address_space = address_space;

    /* Handle the first entry specially.  */
    off_last = off_low = pio_start->offset;
    off_high = off_low + pio_start->len + pio_start->size - 1;
    count = 1;

    for (pio = pio_start + 1; pio->size != 0; pio++, count++) {
        /* All entries must be sorted by offset.  */
        assert(pio->offset >= off_last);
        off_last = pio->offset;

        /* If we see a hole, break the region.  */
        if (off_last > off_high) {
            portio_list_add_1(piolist, pio_start, count, start, off_low,
                              off_high);
            /* ... and start collecting anew.  */
            pio_start = pio;
            off_low = off_last;
            off_high = off_low + pio->len + pio_start->size - 1;
            count = 0;
        } else if (off_last + pio->len > off_high) {
            off_high = off_last + pio->len + pio_start->size - 1;
        }
    }

    /* There will always be an open sub-list.  */
    portio_list_add_1(piolist, pio_start, count, start, off_low, off_high);
}

void portio_list_del(PortioList *piolist)
{
    MemoryRegionPortioList *mrpio;
    unsigned i;

    for (i = 0; i < piolist->nr; ++i) {
        mrpio = container_of(piolist->regions[i], MemoryRegionPortioList, mr);
        memory_region_del_subregion(piolist->address_space, &mrpio->mr);
    }
}

static void memory_region_portio_list_finalize(Object *obj)
{
    MemoryRegionPortioList *mrpio = MEMORY_REGION_PORTIO_LIST(obj);

    object_unref(&mrpio->mr);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    g_free(mrpio->direct_ops);
#endif
    g_free(mrpio->ports);
}

static const TypeInfo memory_region_portio_list_info = {
    .parent             = TYPE_OBJECT,
    .name               = TYPE_MEMORY_REGION_PORTIO_LIST,
    .instance_size      = sizeof(MemoryRegionPortioList),
    .instance_finalize  = memory_region_portio_list_finalize,
};

static void ioport_register_types(void)
{
    type_register_static(&memory_region_portio_list_info);
}

type_init(ioport_register_types)
