/*
 * Virtio PCI Bindings
 *
 * Copyright IBM, Corp. 2007
 * Copyright (c) 2009 CodeSourcery
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Paul Brook        <paul@codesourcery.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "sysemu/runstate.h"
#include "sysemu/cpu-timers.h"
#include "exec/irq-commit.h"
#include "sysemu/tcg.h"
#ifdef CONFIG_LINUX
#include "qemu/futex.h"
#endif

#include "exec/memop.h"
#include "exec/address-spaces.h"
#include "standard-headers/linux/virtio_pci.h"
#include "standard-headers/linux/virtio_ids.h"
#include "hw/boards.h"
#include "hw/virtio/virtio.h"
#include "migration/qemu-file-types.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/loader.h"
#include "sysemu/cpus.h"
#include "sysemu/kvm.h"
#include "hw/virtio/virtio-pci.h"
#include "qemu/range.h"
#include "hw/virtio/virtio-bus.h"
#include "qapi/visitor.h"
#include "sysemu/replay.h"
#include "trace.h"
#include "trace/trace-softmmu.h"

#define VIRTIO_PCI_REGION_SIZE(dev)     VIRTIO_PCI_CONFIG_OFF(msix_present(dev))

#undef VIRTIO_PCI_CONFIG

/* The remaining space is defined by each driver as the per-driver
 * configuration space */
#define VIRTIO_PCI_CONFIG_SIZE(dev)     VIRTIO_PCI_CONFIG_OFF(msix_enabled(dev))

/* The low byte of the same atomic word holds the VirtIO ISR. */
#define VIRTIO_PCI_ISR_INTX_LOW 0x100UL
#define VIRTIO_PCI_ISR_ASYNC_INTX 0x200UL
#define VIRTIO_PCI_ISR_PRESENT 0x400UL

static void virtio_pci_bus_new(VirtioBusState *bus, size_t bus_size,
                               VirtIOPCIProxy *dev);
static void virtio_pci_reset(DeviceState *qdev);

/* BQL handlers can call other devices while the queue staging is paused. */
typedef VirtIOPCIProxy VirtIOPCIQueueConfigPause;

static void virtio_pci_update_queue_config_device(VirtIOPCIProxy *proxy,
                                                  VirtIODevice *vdev)
{
    /* Drain IOThread writes before saving the live and staged queue state. */
    QEMU_LOCK_GUARD(&proxy->queue_config_lock);
    qatomic_set(&proxy->queue_config_device,
                vdev && vdev->vm_running ? vdev : NULL);
}

static VirtIOPCIQueueConfigPause *
virtio_pci_pause_queue_config(VirtIOPCIProxy *p)
{
    if (!qatomic_read(&p->queue_config_device)) {
        return NULL;
    }

    QEMU_LOCK_GUARD(&p->queue_config_lock);
    ++p->queue_config_paused;
    return p;
}

static void virtio_pci_resume_queue_config(VirtIOPCIQueueConfigPause *p)
{
    QEMU_LOCK_GUARD(&p->queue_config_lock);
    --p->queue_config_paused;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VirtIOPCIQueueConfigPause,
                              virtio_pci_resume_queue_config)

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static void virtio_pci_queue_config_map_begin(MemoryListener *listener)
{
    VirtIOPCIProxy *proxy = container_of(listener, VirtIOPCIProxy,
                                         queue_config_listener);

    QEMU_LOCK_GUARD(&proxy->queue_config_lock);
    ++proxy->queue_config_paused;
}

static void virtio_pci_queue_config_map_end(MemoryListener *listener)
{
    VirtIOPCIProxy *proxy = container_of(listener, VirtIOPCIProxy,
                                         queue_config_listener);

    QEMU_LOCK_GUARD(&proxy->queue_config_lock);
    --proxy->queue_config_paused;
}

static void virtio_pci_queue_trace_enabled(Notifier *notifier, void *event)
{
    VirtIOPCIProxy *proxy = container_of(notifier, VirtIOPCIProxy,
                                         queue_trace_enabled);

    if (event == &_TRACE_MEMORY_REGION_OPS_WRITE_EVENT ||
        event == &_TRACE_MEMORY_REGION_OPS_READ_EVENT) {
        /* Complete accesses that already observed tracing disabled. */
        qemu_mutex_lock(&proxy->queue_config_lock);
        qemu_mutex_unlock(&proxy->queue_config_lock);
    }
}

static bool virtio_pci_pause_split_queue_config(void *opaque, hwaddr addr,
                                                unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;

    /*
     * The common and legacy BARs have no per-address access filter. Pieces
     * can dispatch under the BQL without a CPU transaction failure.
     */
    if (!qatomic_read(&proxy->queue_config_device) ||
        addr >= proxy->common.size || size > proxy->common.size - addr ||
        DEVICE(proxy)->mem_reentrancy_guard.engaged_in_io) {
        return false;
    }

    return virtio_pci_pause_queue_config(proxy) != NULL;
}

static void virtio_pci_resume_split_queue_config(void *opaque)
{
    virtio_pci_resume_queue_config(opaque);
}
#endif

/* virtio device */
/* DeviceState to VirtIOPCIProxy. For use off data-path. TODO: use QOM. */
static inline VirtIOPCIProxy *to_virtio_pci_proxy(DeviceState *d)
{
    return container_of(d, VirtIOPCIProxy, pci_dev.qdev);
}

/* DeviceState to VirtIOPCIProxy. Note: used on datapath,
 * be careful and test performance if you change this.
 */
static inline VirtIOPCIProxy *to_virtio_pci_proxy_fast(DeviceState *d)
{
    return container_of(d, VirtIOPCIProxy, pci_dev.qdev);
}

/*
 * Keep the ISR in the same word as the route version. A successful edge
 * commits the ISR and APIC together. Config writers revoke the old version
 * before touching PCI, VirtIO, or the address map.
 */
#define VIRTIO_PCI_DIRECT_IRQ UINT64_C(0x800)
#define VIRTIO_PCI_ROUTE_SHIFT 13
#define VIRTIO_PCI_ROUTE_MASK (IRQ_COMMIT_PAYLOAD & ~UINT64_C(0x1fff))

typedef struct VirtIOPCIDirectRoutes {
    struct rcu_head rcu;
    uint64_t version;
    unsigned queues;
    struct {
        MSIMessage msi;
        bool valid;
        bool no_vector;
    } entries[];
} VirtIOPCIDirectRoutes;

static void virtio_pci_irq_routes_begin(VirtIOPCIProxy *proxy)
{
    if (!proxy->direct_irq_enabled) {
        return;
    }
    if (proxy->direct_irq_depth++ == 0) {
        irq_commit_source_update(&proxy->bus.interrupt_state,
                                 VIRTIO_PCI_DIRECT_IRQ, 0);
    }
}

static void virtio_pci_irq_routes_end(VirtIOPCIProxy *proxy)
{
    VirtIOPCIDirectRoutes *old, *routes = NULL;
    PCIDevice *dev = &proxy->pci_dev;
    VirtIODevice *vdev = proxy->fast_notify_device;
    unsigned queues = proxy->fast_notify_queues;
    uint64_t enabled = 0;

    if (!proxy->direct_irq_enabled) {
        return;
    }
    g_assert(proxy->direct_irq_depth);
    if (--proxy->direct_irq_depth) {
        return;
    }

    if (proxy->direct_irq_device != VIRTIO_ID_SCSI) {
        /* Completion routing is independent of guest kick delivery. */
        vdev = virtio_bus_get_device(&proxy->bus);
        queues = vdev ? virtio_get_num_queues(vdev) : 0;
    }
    ++proxy->direct_irq_version;
    g_assert(proxy->direct_irq_version < (VIRTIO_PCI_ROUTE_MASK >>
                                          VIRTIO_PCI_ROUTE_SHIFT));
    if (vdev && queues && !proxy->guest_reset_pending && !vdev->disabled &&
        (vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) &&
        dev->has_power &&
        (pci_get_word(dev->config + PCI_COMMAND) & PCI_COMMAND_MASTER) &&
        msix_enabled(dev)) {
        routes = g_malloc0(sizeof(*routes) + queues * sizeof(routes->entries[0]));
        routes->version = proxy->direct_irq_version;
        routes->queues = queues;
        for (unsigned i = 0; i < queues; i++) {
            unsigned vector = virtio_queue_vector(vdev, i);

            routes->entries[i].no_vector = vector == VIRTIO_NO_VECTOR;
            if (vector < dev->msix_entries_nr && dev->msix_entry_used[vector] &&
                !msix_is_masked(dev, vector)) {
                routes->entries[i].msi = msix_get_message(dev, vector);
                routes->entries[i].valid = true;
            }
        }
        enabled = VIRTIO_PCI_DIRECT_IRQ;
    }

    old = proxy->direct_irq_routes;
    qatomic_rcu_set(&proxy->direct_irq_routes, routes);
    irq_commit_source_update(&proxy->bus.interrupt_state,
                             VIRTIO_PCI_ROUTE_MASK | VIRTIO_PCI_DIRECT_IRQ,
                             enabled | (proxy->direct_irq_version <<
                                        VIRTIO_PCI_ROUTE_SHIFT));
    if (old) {
        g_free_rcu(old, rcu);
    }
}

static void virtio_pci_irq_route_change(PCIDevice *dev, bool before)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(dev);

    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    if (before) {
        qemu_mutex_lock(&proxy->queue_config_lock);
        ++proxy->queue_config_paused;
        qemu_mutex_unlock(&proxy->queue_config_lock);
        if (vdev && vdev->flush_output_irqs) {
            qemu_rec_mutex_lock(vdev->network_mutex);
            vdev->flush_output_irqs(vdev);
        }
        virtio_pci_irq_routes_begin(proxy);
    } else {
        virtio_pci_irq_routes_end(proxy);
        if (vdev && vdev->flush_output_irqs) {
            qemu_rec_mutex_unlock(vdev->network_mutex);
        }
        qemu_mutex_lock(&proxy->queue_config_lock);
        --proxy->queue_config_paused;
        qemu_mutex_unlock(&proxy->queue_config_lock);
    }
}

typedef VirtIOPCIProxy VirtIOPCIIRQRouteGuard;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VirtIOPCIIRQRouteGuard, virtio_pci_irq_routes_end)

static VirtIOPCIIRQRouteGuard *virtio_pci_guard_irq_routes(VirtIOPCIProxy *proxy)
{
    if (!proxy->direct_irq_enabled) {
        return NULL;
    }
    virtio_pci_irq_routes_begin(proxy);
    return proxy;
}

static void virtio_pci_irq_map_begin(MemoryListener *listener)
{
    VirtIOPCIProxy *proxy = container_of(listener, VirtIOPCIProxy,
                                        direct_irq_listener);

    virtio_pci_irq_routes_begin(proxy);
}

static void virtio_pci_irq_map_end(MemoryListener *listener)
{
    VirtIOPCIProxy *proxy = container_of(listener, VirtIOPCIProxy,
                                        direct_irq_listener);

    virtio_pci_irq_routes_end(proxy);
}

static uint64_t trial_scsi_offers, trial_scsi_direct;
static bool trial_scsi_disable_direct;
static uint64_t trial_net_offers, trial_net_direct;

void trial_scsi_enable_notifier_counts(void);
uint64_t trial_scsi_notifier_count(void);
void trial_exclusive_work_counts(const char *phase) __attribute__((weak));
bool apic_msi_commit_configured(void) __attribute__((weak));

static void trial_scsi_counts(const char *phase)
{
    if (trial_exclusive_work_counts) {
        trial_exclusive_work_counts(phase);
    }
    fprintf(stderr, "SCSI_ATOMIC %s offered=%" PRIu64
                    " direct=%" PRIu64 " notifier=%" PRIu64 "\n",
            phase, qatomic_read(&trial_scsi_offers),
            qatomic_read(&trial_scsi_direct), trial_scsi_notifier_count());
}

static void trial_scsi_vmstate(void *opaque, bool running, RunState state)
{
    if (!running && state == RUN_STATE_PAUSED) {
        trial_scsi_counts("menu");
    }
}

static void trial_scsi_exit(void)
{
    trial_scsi_counts("exit");
}

static void trial_net_irq_counts(const char *phase)
{
    fprintf(stderr, "NET_ATOMIC %s offered=%" PRIu64 " direct=%" PRIu64 "\n",
            phase, qatomic_read(&trial_net_offers),
            qatomic_read(&trial_net_direct));
}

static void trial_net_irq_vmstate(void *opaque, bool running, RunState state)
{
    if (!running && state == RUN_STATE_PAUSED) {
        trial_net_irq_counts("pause");
    }
}

static void trial_net_irq_exit(void)
{
    trial_net_irq_counts("exit");
}

static VirtIOPCIDirectRoutes *virtio_pci_queue_routes(VirtIOPCIProxy *proxy,
                                                      unsigned queue, uint64_t state)
{
    if (!(state & VIRTIO_PCI_DIRECT_IRQ) || (state & VIRTIO_ISR_BROKEN)) {
        return NULL;
    }
    VirtIOPCIDirectRoutes *routes = qatomic_rcu_read(&proxy->direct_irq_routes);
    return routes && (state >> VIRTIO_PCI_ROUTE_SHIFT) == routes->version &&
           queue < routes->queues ? routes : NULL;
}

static bool virtio_pci_queue_irq_ready(DeviceState *dev, unsigned queue)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy_fast(dev);
    RCU_READ_LOCK_GUARD();
    uint64_t state = irq_commit_source_read(&proxy->bus.interrupt_state);
    VirtIOPCIDirectRoutes *routes = virtio_pci_queue_routes(proxy, queue, state);
    return routes && (routes->entries[queue].valid || routes->entries[queue].no_vector);
}

static bool virtio_pci_try_queue_irq(DeviceState *d, unsigned queue)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy_fast(d);
    uint64_t *source = &proxy->bus.interrupt_state;
    VirtIOPCIDirectRoutes *routes;
    uint64_t state;
    MSIMessage msi;

    if (!qatomic_load_acquire(&proxy->direct_irq_enabled)) {
        return false;
    }
    RCU_READ_LOCK_GUARD();
    if (proxy->direct_irq_device == VIRTIO_ID_NET) {
        qatomic_inc(&trial_net_offers);
    } else if (proxy->direct_irq_device == VIRTIO_ID_SCSI) {
        qatomic_inc(&trial_scsi_offers);
        if (trial_scsi_disable_direct) {
            return false;
        }
    }
    bool committed = false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        state = irq_commit_source_read(source);
        routes = virtio_pci_queue_routes(proxy, queue, state);
        if (!routes) {
            return false;
        }
        if (proxy->direct_irq_device == VIRTIO_ID_CONSOLE &&
            routes->entries[queue].no_vector) {
            /* MSI-X with no queue vector only publishes the ISR bit. */
            if (qatomic_cmpxchg(source, state, state | 1) == state) {
                committed = true;
                break;
            }
            continue;
        }
        if (!routes->entries[queue].valid) {
            return false;
        }
        msi = routes->entries[queue].msi;
        if (address_space_try_commit_msi(&proxy->pci_dev.bus_master_as,
                                         msi.address, msi.data, source,
                                         state, state | 1,
                                         proxy->direct_irq_device == VIRTIO_ID_SCSI)) {
            committed = true;
            break;
        }
        if (irq_commit_source_read(source) == state ||
            (proxy->direct_irq_device == VIRTIO_ID_SCSI &&
             g_getenv("LATS_SCSI_COMMIT_TEST_DIR"))) {
            return false;
        }
    }
    if (!committed) {
        return false;
    }
    if (proxy->direct_irq_device == VIRTIO_ID_NET) {
        qatomic_inc(&trial_net_direct);
    } else if (proxy->direct_irq_device == VIRTIO_ID_SCSI) {
        g_assert(!qemu_mutex_iothread_locked());
        qatomic_inc(&trial_scsi_direct);
    }
    return true;
}

static void virtio_pci_publish_intx_low(VirtIOPCIProxy *proxy)
{
    uint64_t *state = &proxy->bus.interrupt_state;
    unsigned long flags = irq_commit_source_read(state);

    if (!qatomic_read(&proxy->pci_dev.irq_state) &&
        (flags & (VIRTIO_PCI_ISR_PRESENT | VIRTIO_PCI_ISR_INTX_LOW)) ==
            VIRTIO_PCI_ISR_PRESENT) {
        irq_commit_source_update(state, 0, VIRTIO_PCI_ISR_INTX_LOW);
    }
}

static uint8_t virtio_pci_ack_isr(VirtIOPCIProxy *proxy, VirtIODevice *vdev)
{
    uint8_t ret = virtio_clear_isr(vdev, VIRTIO_ISR_MASK);

    pci_irq_deassert(&proxy->pci_dev);
    virtio_pci_publish_intx_low(proxy);
    return ret;
}

static void virtio_pci_notify(DeviceState *d, uint16_t vector)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy_fast(d);

    if (msix_enabled(&proxy->pci_dev)) {
        if (vector != VIRTIO_NO_VECTOR) {
            msix_notify(&proxy->pci_dev, vector);
        }
    } else {
        VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
        uint64_t *state = &proxy->bus.interrupt_state;
        bool tracked = irq_commit_source_read(state) & VIRTIO_PCI_ISR_PRESENT;
        bool locked = qemu_mutex_iothread_locked();

        /*
         * Withdraw the idle state before testing the ISR or changing INTx.
         * An async assertion cannot be serialized with a later deassert by
         * the BQL, so keep the idle shortcut disabled once one has occurred.
         */
        if (!locked) {
            irq_commit_source_update(state, 0, VIRTIO_PCI_ISR_ASYNC_INTX);
        } else if (tracked) {
            irq_commit_source_update(state, VIRTIO_PCI_ISR_INTX_LOW, 0);
        }
        pci_set_irq(&proxy->pci_dev, virtio_get_isr(vdev) & 1);
        if (tracked && locked) {
            virtio_pci_publish_intx_low(proxy);
        }
    }
}

static void virtio_pci_save_config(DeviceState *d, QEMUFile *f)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    pci_device_save(&proxy->pci_dev, f);
    msix_save(&proxy->pci_dev, f);
    if (msix_present(&proxy->pci_dev))
        qemu_put_be16(f, vdev->config_vector);
}

static const VMStateDescription vmstate_virtio_pci_modern_queue_state = {
    .name = "virtio_pci/modern_queue_state",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT16(num, VirtIOPCIQueue),
        VMSTATE_UNUSED(1), /* enabled was stored as be16 */
        VMSTATE_BOOL(enabled, VirtIOPCIQueue),
        VMSTATE_UINT32_ARRAY(desc, VirtIOPCIQueue, 2),
        VMSTATE_UINT32_ARRAY(avail, VirtIOPCIQueue, 2),
        VMSTATE_UINT32_ARRAY(used, VirtIOPCIQueue, 2),
        VMSTATE_END_OF_LIST()
    }
};

static bool virtio_pci_modern_state_needed(void *opaque)
{
    VirtIOPCIProxy *proxy = opaque;

    return virtio_pci_modern(proxy);
}

static const VMStateDescription vmstate_virtio_pci_modern_state_sub = {
    .name = "virtio_pci/modern_state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = &virtio_pci_modern_state_needed,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(dfselect, VirtIOPCIProxy),
        VMSTATE_UINT32(gfselect, VirtIOPCIProxy),
        VMSTATE_UINT32_ARRAY(guest_features, VirtIOPCIProxy, 2),
        VMSTATE_STRUCT_ARRAY(vqs, VirtIOPCIProxy, VIRTIO_QUEUE_MAX, 0,
                             vmstate_virtio_pci_modern_queue_state,
                             VirtIOPCIQueue),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_virtio_pci = {
    .name = "virtio_pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription*[]) {
        &vmstate_virtio_pci_modern_state_sub,
        NULL
    }
};

static bool virtio_pci_has_extra_state(DeviceState *d)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);

    return proxy->flags & VIRTIO_PCI_FLAG_MIGRATE_EXTRA;
}

static void virtio_pci_save_extra_state(DeviceState *d, QEMUFile *f)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);

    vmstate_save_state(f, &vmstate_virtio_pci, proxy, NULL);
}

static int virtio_pci_load_extra_state(DeviceState *d, QEMUFile *f)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);

    return vmstate_load_state(f, &vmstate_virtio_pci, proxy, 1);
}

static void virtio_pci_save_queue(DeviceState *d, int n, QEMUFile *f)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    if (msix_present(&proxy->pci_dev))
        qemu_put_be16(f, virtio_queue_vector(vdev, n));
}

static int virtio_pci_load_config(DeviceState *d, QEMUFile *f)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    uint16_t vector;
    g_autoptr(VirtIOPCIIRQRouteGuard) route_guard G_GNUC_UNUSED =
        virtio_pci_guard_irq_routes(proxy);

    int ret;
    irq_commit_source_update(&proxy->bus.interrupt_state, VIRTIO_PCI_ISR_INTX_LOW, 0);
    ret = pci_device_load(&proxy->pci_dev, f);
    if (ret) {
        return ret;
    }
    msix_unuse_all_vectors(&proxy->pci_dev);
    msix_load(&proxy->pci_dev, f);
    if (msix_present(&proxy->pci_dev)) {
        qemu_get_be16s(f, &vector);

        if (vector != VIRTIO_NO_VECTOR && vector >= proxy->nvectors) {
            return -EINVAL;
        }
    } else {
        vector = VIRTIO_NO_VECTOR;
    }
    vdev->config_vector = vector;
    if (vector != VIRTIO_NO_VECTOR) {
        msix_vector_use(&proxy->pci_dev, vector);
    }
    virtio_pci_publish_intx_low(proxy);
    return 0;
}

static int virtio_pci_load_queue(DeviceState *d, int n, QEMUFile *f)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    uint16_t vector;
    g_autoptr(VirtIOPCIIRQRouteGuard) route_guard G_GNUC_UNUSED =
        virtio_pci_guard_irq_routes(proxy);
    if (msix_present(&proxy->pci_dev)) {
        qemu_get_be16s(f, &vector);
        if (vector != VIRTIO_NO_VECTOR && vector >= proxy->nvectors) {
            return -EINVAL;
        }
    } else {
        vector = VIRTIO_NO_VECTOR;
    }
    virtio_queue_set_vector(vdev, n, vector);
    if (vector != VIRTIO_NO_VECTOR) {
        msix_vector_use(&proxy->pci_dev, vector);
    }

    return 0;
}

typedef struct VirtIOPCIIDInfo {
    /* virtio id */
    uint16_t vdev_id;
    /* pci device id for the transitional device */
    uint16_t trans_devid;
    uint16_t class_id;
} VirtIOPCIIDInfo;

static const VirtIOPCIIDInfo virtio_pci_id_info[] = {
    {
        .vdev_id = VIRTIO_ID_CRYPTO,
        .class_id = PCI_CLASS_OTHERS,
    }, {
        .vdev_id = VIRTIO_ID_FS,
        .class_id = PCI_CLASS_STORAGE_OTHER,
    }, {
        .vdev_id = VIRTIO_ID_NET,
        .trans_devid = PCI_DEVICE_ID_VIRTIO_NET,
        .class_id = PCI_CLASS_NETWORK_ETHERNET,
    }, {
        .vdev_id = VIRTIO_ID_BLOCK,
        .trans_devid = PCI_DEVICE_ID_VIRTIO_BLOCK,
        .class_id = PCI_CLASS_STORAGE_SCSI,
    }, {
        .vdev_id = VIRTIO_ID_CONSOLE,
        .trans_devid = PCI_DEVICE_ID_VIRTIO_CONSOLE,
        .class_id = PCI_CLASS_COMMUNICATION_OTHER,
    }, {
        .vdev_id = VIRTIO_ID_SCSI,
        .trans_devid = PCI_DEVICE_ID_VIRTIO_SCSI,
        .class_id = PCI_CLASS_STORAGE_SCSI
    }, {
        .vdev_id = VIRTIO_ID_9P,
        .trans_devid = PCI_DEVICE_ID_VIRTIO_9P,
        .class_id = PCI_BASE_CLASS_NETWORK,
    }, {
        .vdev_id = VIRTIO_ID_BALLOON,
        .trans_devid = PCI_DEVICE_ID_VIRTIO_BALLOON,
        .class_id = PCI_CLASS_OTHERS,
    }, {
        .vdev_id = VIRTIO_ID_RNG,
        .trans_devid = PCI_DEVICE_ID_VIRTIO_RNG,
        .class_id = PCI_CLASS_OTHERS,
    },
};

static const VirtIOPCIIDInfo *virtio_pci_get_id_info(uint16_t vdev_id)
{
    const VirtIOPCIIDInfo *info = NULL;
    int i;

    for (i = 0; i < ARRAY_SIZE(virtio_pci_id_info); i++) {
        if (virtio_pci_id_info[i].vdev_id == vdev_id) {
            info = &virtio_pci_id_info[i];
            break;
        }
    }

    if (!info) {
        /* The device id is invalid or not added to the id_info yet. */
        error_report("Invalid virtio device(id %u)", vdev_id);
        abort();
    }

    return info;
}

/*
 * Get the Transitional Device ID for the specific device, return
 * zero if the device is non-transitional.
 */
uint16_t virtio_pci_get_trans_devid(uint16_t device_id)
{
    return virtio_pci_get_id_info(device_id)->trans_devid;
}

/*
 * Get the Class ID for the specific device.
 */
uint16_t virtio_pci_get_class_id(uint16_t device_id)
{
    return virtio_pci_get_id_info(device_id)->class_id;
}

static bool virtio_pci_ioeventfd_enabled(DeviceState *d)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);

    return (proxy->flags & VIRTIO_PCI_FLAG_USE_IOEVENTFD) != 0;
}

#define QEMU_VIRTIO_PCI_QUEUE_MEM_MULT 0x1000

static inline int virtio_pci_queue_mem_mult(struct VirtIOPCIProxy *proxy)
{
    return (proxy->flags & VIRTIO_PCI_FLAG_PAGE_PER_VQ) ?
        QEMU_VIRTIO_PCI_QUEUE_MEM_MULT : 4;
}

/* Configuration is serialized by the BQL. A queue notification never waits
 * for configuration or for a notification on another vCPU. */
static void virtio_pci_set_fast_notifiers(DeviceState *d, VirtIODevice *vdev,
                                          unsigned queues)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    unsigned *state = &proxy->fast_notify_state;
    g_autoptr(VirtIOPCIIRQRouteGuard) route_guard G_GNUC_UNUSED =
        proxy->direct_irq_device == VIRTIO_ID_SCSI ?
        virtio_pci_guard_irq_routes(proxy) : NULL;
    unsigned old = qatomic_fetch_and(state, ~1u);

    if ((old & 1) && old > 1) {
        unsigned pending;
        while ((pending = qatomic_load_acquire(state)) != 0) {
#ifdef CONFIG_LINUX
            qemu_futex_wait(state, pending);
#else
            g_usleep(50);
#endif
        }
    }
    proxy->fast_notify_queues = queues;
    proxy->fast_notify_stride = virtio_pci_queue_mem_mult(proxy);
    proxy->fast_notify_device = vdev;
    if (queues) {
        bitmap_zero(proxy->fast_notify_present, VIRTIO_QUEUE_MAX);
        for (unsigned i = 0; i < queues; ++i) {
            if (virtio_queue_get_num(vdev, i)) {
                set_bit(i, proxy->fast_notify_present);
            }
        }
        qatomic_or(state, 1u);
    }
}

static bool virtio_pci_try_notify_eventfd(void *opaque, hwaddr addr,
                                         unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    unsigned *state = &proxy->fast_notify_state;
    bool handled = false;

    if (size != 2 || !(qatomic_read(state) & 1)) {
        return false;
    }
    /* Admission and pinning the notifier must be one indivisible operation. */
    if (qatomic_fetch_add(state, 2u) & 1) {
        hwaddr index = addr / proxy->fast_notify_stride;
        if (!(addr % proxy->fast_notify_stride) &&
            index < proxy->fast_notify_queues &&
            test_bit(index, proxy->fast_notify_present)) {
            VirtQueue *vq = virtio_get_queue(proxy->fast_notify_device, index);
            VirtIODevice *vdev = proxy->fast_notify_device;
            if (!vdev->try_output || !vdev->try_output(vdev, vq)) {
                event_notifier_set(virtio_queue_get_host_notifier(vq));
            }
            handled = true;
        }
    }
    if (qatomic_fetch_sub(state, 2u) == 2) {
#ifdef CONFIG_LINUX
        qemu_futex_wake(state, 1);
#endif
    }
    return handled;
}

static int virtio_pci_ioeventfd_assign(DeviceState *d, EventNotifier *notifier,
                                       int n, bool assign)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtQueue *vq = virtio_get_queue(vdev, n);
    bool legacy = virtio_pci_legacy(proxy);
    bool modern = virtio_pci_modern(proxy);
    bool fast_mmio = kvm_ioeventfd_any_length_enabled();
    bool modern_pio = proxy->flags & VIRTIO_PCI_FLAG_MODERN_PIO_NOTIFY;
    MemoryRegion *modern_mr = &proxy->notify.mr;
    MemoryRegion *modern_notify_mr = &proxy->notify_pio.mr;
    MemoryRegion *legacy_mr = &proxy->bar;
    hwaddr modern_addr = virtio_pci_queue_mem_mult(proxy) *
                         virtio_get_queue_index(vq);
    hwaddr legacy_addr = VIRTIO_PCI_QUEUE_NOTIFY;

    if (assign) {
        if (modern) {
            if (fast_mmio) {
                memory_region_add_eventfd(modern_mr, modern_addr, 0,
                                          false, n, notifier);
            } else {
                memory_region_add_eventfd(modern_mr, modern_addr, 2,
                                          false, n, notifier);
            }
            if (modern_pio) {
                memory_region_add_eventfd(modern_notify_mr, 0, 2,
                                              true, n, notifier);
            }
        }
        if (legacy) {
            memory_region_add_eventfd(legacy_mr, legacy_addr, 2,
                                      true, n, notifier);
        }
    } else {
        virtio_pci_set_fast_notifiers(d, NULL, 0);
        if (modern) {
            if (fast_mmio) {
                memory_region_del_eventfd(modern_mr, modern_addr, 0,
                                          false, n, notifier);
            } else {
                memory_region_del_eventfd(modern_mr, modern_addr, 2,
                                          false, n, notifier);
            }
            if (modern_pio) {
                memory_region_del_eventfd(modern_notify_mr, 0, 2,
                                          true, n, notifier);
            }
        }
        if (legacy) {
            memory_region_del_eventfd(legacy_mr, legacy_addr, 2,
                                      true, n, notifier);
        }
    }
    return 0;
}

static void virtio_pci_start_ioeventfd(VirtIOPCIProxy *proxy)
{
    virtio_bus_start_ioeventfd(&proxy->bus);
}

static void virtio_pci_stop_ioeventfd(VirtIOPCIProxy *proxy)
{
    virtio_pci_set_fast_notifiers(DEVICE(proxy), NULL, 0);
    virtio_bus_stop_ioeventfd(&proxy->bus);
}

static void virtio_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    VirtIOPCIProxy *proxy = opaque;
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    uint16_t vector;
    hwaddr pa;
    g_autoptr(VirtIOPCIIRQRouteGuard) route_guard G_GNUC_UNUSED =
        addr != VIRTIO_PCI_QUEUE_NOTIFY ?
        virtio_pci_guard_irq_routes(proxy) : NULL;

    if (proxy->guest_reset_pending) {
        return;
    }

    switch (addr) {
    case VIRTIO_PCI_GUEST_FEATURES:
        /* Guest does not negotiate properly?  We have to assume nothing. */
        if (val & (1 << VIRTIO_F_BAD_FEATURE)) {
            val = virtio_bus_get_vdev_bad_features(&proxy->bus);
        }
        virtio_set_features(vdev, val);
        break;
    case VIRTIO_PCI_QUEUE_PFN:
        pa = (hwaddr)val << VIRTIO_PCI_QUEUE_ADDR_SHIFT;
        if (pa == 0) {
            virtio_pci_reset(DEVICE(proxy));
        }
        else
            virtio_queue_set_addr(vdev, vdev->queue_sel, pa);
        break;
    case VIRTIO_PCI_QUEUE_SEL:
        if (val < VIRTIO_QUEUE_MAX)
            vdev->queue_sel = val;
        break;
    case VIRTIO_PCI_QUEUE_NOTIFY:
        if (val < VIRTIO_QUEUE_MAX) {
            virtio_queue_notify(vdev, val);
        }
        break;
    case VIRTIO_PCI_STATUS:
        if (!(val & VIRTIO_CONFIG_S_DRIVER_OK)) {
            virtio_pci_stop_ioeventfd(proxy);
        }

        virtio_set_status(vdev, val & 0xFF);

        if (val & VIRTIO_CONFIG_S_DRIVER_OK) {
            virtio_pci_start_ioeventfd(proxy);
        }

        if (vdev->status == 0) {
            virtio_pci_reset(DEVICE(proxy));
        }

        /* Linux before 2.6.34 drives the device without enabling
           the PCI device bus master bit. Enable it automatically
           for the guest. This is a PCI spec violation but so is
           initiating DMA with bus master bit clear. */
        if (val == (VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER)) {
            pci_default_write_config(&proxy->pci_dev, PCI_COMMAND,
                                     proxy->pci_dev.config[PCI_COMMAND] |
                                     PCI_COMMAND_MASTER, 1);
        }
        break;
    case VIRTIO_MSI_CONFIG_VECTOR:
        if (vdev->config_vector != VIRTIO_NO_VECTOR) {
            msix_vector_unuse(&proxy->pci_dev, vdev->config_vector);
        }
        /* Make it possible for guest to discover an error took place. */
        if (val < proxy->nvectors) {
            msix_vector_use(&proxy->pci_dev, val);
        } else {
            val = VIRTIO_NO_VECTOR;
        }
        vdev->config_vector = val;
        break;
    case VIRTIO_MSI_QUEUE_VECTOR:
        if (vdev->flush_output_irqs) {
            vdev->flush_output_irqs(vdev);
        }
        vector = virtio_queue_vector(vdev, vdev->queue_sel);
        if (vector != VIRTIO_NO_VECTOR) {
            msix_vector_unuse(&proxy->pci_dev, vector);
        }
        /* Make it possible for guest to discover an error took place. */
        if (val < proxy->nvectors) {
            msix_vector_use(&proxy->pci_dev, val);
        } else {
            val = VIRTIO_NO_VECTOR;
        }
        virtio_queue_set_vector(vdev, vdev->queue_sel, val);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: unexpected address 0x%x value 0x%x\n",
                      __func__, addr, val);
        break;
    }
}

static uint32_t virtio_ioport_read(VirtIOPCIProxy *proxy, uint32_t addr)
{
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    uint32_t ret = 0xFFFFFFFF;

    switch (addr) {
    case VIRTIO_PCI_HOST_FEATURES:
        ret = vdev->host_features;
        break;
    case VIRTIO_PCI_GUEST_FEATURES:
        ret = vdev->guest_features;
        break;
    case VIRTIO_PCI_QUEUE_PFN:
        ret = virtio_queue_get_addr(vdev, vdev->queue_sel)
              >> VIRTIO_PCI_QUEUE_ADDR_SHIFT;
        break;
    case VIRTIO_PCI_QUEUE_NUM:
        ret = virtio_queue_get_num(vdev, vdev->queue_sel);
        break;
    case VIRTIO_PCI_QUEUE_SEL:
        ret = vdev->queue_sel;
        break;
    case VIRTIO_PCI_STATUS:
        ret = vdev->status;
        break;
    case VIRTIO_PCI_ISR:
        /* reading from the ISR also clears it. */
        ret = virtio_pci_ack_isr(proxy, vdev);
        break;
    case VIRTIO_MSI_CONFIG_VECTOR:
        ret = vdev->config_vector;
        break;
    case VIRTIO_MSI_QUEUE_VECTOR:
        ret = virtio_queue_vector(vdev, vdev->queue_sel);
        break;
    default:
        break;
    }

    return ret;
}

static uint64_t virtio_pci_config_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    g_autoptr(VirtioNetworkLock) network = virtio_lock_network(vdev);
    uint32_t config = VIRTIO_PCI_CONFIG_SIZE(&proxy->pci_dev);
    uint64_t val = 0;

    if (vdev == NULL) {
        return UINT64_MAX;
    }

    if (addr < config) {
        g_autoptr(VirtIOPCIQueueConfigPause) paused =
            virtio_pci_pause_queue_config(proxy);

        return virtio_ioport_read(proxy, addr);
    }
    addr -= config;

    switch (size) {
    case 1:
        val = virtio_config_readb(vdev, addr);
        break;
    case 2:
        val = virtio_config_readw(vdev, addr);
        if (virtio_is_big_endian(vdev)) {
            val = bswap16(val);
        }
        break;
    case 4:
        val = virtio_config_readl(vdev, addr);
        if (virtio_is_big_endian(vdev)) {
            val = bswap32(val);
        }
        break;
    }
    return val;
}

static void virtio_pci_config_write(void *opaque, hwaddr addr,
                                    uint64_t val, unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    uint32_t config = VIRTIO_PCI_CONFIG_SIZE(&proxy->pci_dev);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    g_autoptr(VirtioNetworkLock) network = virtio_lock_network(vdev);

    if (vdev == NULL) {
        return;
    }

    if (addr < config) {
        g_autoptr(VirtIOPCIQueueConfigPause) paused =
            virtio_pci_pause_queue_config(proxy);

        virtio_ioport_write(proxy, addr, val);
        return;
    }
    addr -= config;
    /*
     * Virtio-PCI is odd. Ioports are LE but config space is target native
     * endian.
     */
    switch (size) {
    case 1:
        virtio_config_writeb(vdev, addr, val);
        break;
    case 2:
        if (virtio_is_big_endian(vdev)) {
            val = bswap16(val);
        }
        virtio_config_writew(vdev, addr, val);
        break;
    case 4:
        if (virtio_is_big_endian(vdev)) {
            val = bswap32(val);
        }
        virtio_config_writel(vdev, addr, val);
        break;
    }
}

static const MemoryRegionOps virtio_pci_config_ops = {
    .read = virtio_pci_config_read,
    .write = virtio_pci_config_write,
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    .mmio_split_pause = virtio_pci_pause_split_queue_config,
    .mmio_split_resume = virtio_pci_resume_split_queue_config,
#endif
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static MemoryRegion *virtio_address_space_lookup(VirtIOPCIProxy *proxy,
                                                 hwaddr *off, int len)
{
    int i;
    VirtIOPCIRegion *reg;

    for (i = 0; i < ARRAY_SIZE(proxy->regs); ++i) {
        reg = &proxy->regs[i];
        if (*off >= reg->offset &&
            *off + len <= reg->offset + reg->size) {
            *off -= reg->offset;
            return &reg->mr;
        }
    }

    return NULL;
}

/* Below are generic functions to do memcpy from/to an address space,
 * without byteswaps, with input validation.
 *
 * As regular address_space_* APIs all do some kind of byteswap at least for
 * some host/target combinations, we are forced to explicitly convert to a
 * known-endianness integer value.
 * It doesn't really matter which endian format to go through, so the code
 * below selects the endian that causes the least amount of work on the given
 * host.
 *
 * Note: host pointer must be aligned.
 */
static
void virtio_address_space_write(VirtIOPCIProxy *proxy, hwaddr addr,
                                const uint8_t *buf, int len)
{
    uint64_t val;
    MemoryRegion *mr;

    /* address_space_* APIs assume an aligned address.
     * As address is under guest control, handle illegal values.
     */
    addr &= ~(len - 1);

    mr = virtio_address_space_lookup(proxy, &addr, len);
    if (!mr) {
        return;
    }

    /* Make sure caller aligned buf properly */
    assert(!(((uintptr_t)buf) & (len - 1)));

    switch (len) {
    case 1:
        val = pci_get_byte(buf);
        break;
    case 2:
        val = pci_get_word(buf);
        break;
    case 4:
        val = pci_get_long(buf);
        break;
    default:
        /* As length is under guest control, handle illegal values. */
        return;
    }
    memory_region_dispatch_write(mr, addr, val, size_memop(len) | MO_LE,
                                 MEMTXATTRS_UNSPECIFIED);
}

static void
virtio_address_space_read(VirtIOPCIProxy *proxy, hwaddr addr,
                          uint8_t *buf, int len)
{
    uint64_t val;
    MemoryRegion *mr;

    /* address_space_* APIs assume an aligned address.
     * As address is under guest control, handle illegal values.
     */
    addr &= ~(len - 1);

    mr = virtio_address_space_lookup(proxy, &addr, len);
    if (!mr) {
        return;
    }

    /* Make sure caller aligned buf properly */
    assert(!(((uintptr_t)buf) & (len - 1)));

    memory_region_dispatch_read(mr, addr, &val, size_memop(len) | MO_LE,
                                MEMTXATTRS_UNSPECIFIED);
    switch (len) {
    case 1:
        pci_set_byte(buf, val);
        break;
    case 2:
        pci_set_word(buf, val);
        break;
    case 4:
        pci_set_long(buf, val);
        break;
    default:
        /* As length is under guest control, handle illegal values. */
        break;
    }
}

static void virtio_pci_ats_ctrl_trigger(PCIDevice *pci_dev, bool enable)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(pci_dev);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);

    vdev->device_iotlb_enabled = enable;

    if (k->toggle_device_iotlb) {
        k->toggle_device_iotlb(vdev);
    }
}

static void pcie_ats_config_write(PCIDevice *dev, uint32_t address,
                                  uint32_t val, int len)
{
    uint32_t off;
    uint16_t ats_cap = dev->exp.ats_cap;

    if (!ats_cap || address < ats_cap) {
        return;
    }
    off = address - ats_cap;
    if (off >= PCI_EXT_CAP_ATS_SIZEOF) {
        return;
    }

    if (range_covers_byte(off, len, PCI_ATS_CTRL + 1)) {
        virtio_pci_ats_ctrl_trigger(dev, !!(val & PCI_ATS_CTRL_ENABLE));
    }
}

static void virtio_write_config(PCIDevice *pci_dev, uint32_t address,
                                uint32_t val, int len)
{
    QEMU_MEMORY_TOPOLOGY_LOCK_GUARD();
    VirtIOPCIProxy *proxy = VIRTIO_PCI(pci_dev);
    g_autoptr(VirtIOPCIQueueConfigPause) paused =
        virtio_pci_pause_queue_config(proxy);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    g_autoptr(VirtioNetworkLock) network = virtio_lock_network(vdev);
    struct virtio_pci_cfg_cap *cfg;
    g_autoptr(VirtIOPCIIRQRouteGuard) route_guard G_GNUC_UNUSED =
        virtio_pci_guard_irq_routes(proxy);

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    pci_config_read_slot_begin(pci_dev);
#endif
    pci_default_write_config(pci_dev, address, val, len);

    if (proxy->flags & VIRTIO_PCI_FLAG_INIT_FLR) {
        uint8_t *devctl = pci_dev->config + pci_dev->exp.exp_cap +
                          PCI_EXP_DEVCTL;

        if (proxy->guest_reset_pending &&
            (pci_get_word(devctl) & PCI_EXP_DEVCTL_BCR_FLR)) {
            proxy->guest_flr_pending = true;
            pci_word_test_and_clear_mask(devctl, PCI_EXP_DEVCTL_BCR_FLR);
        } else {
            pcie_cap_flr_write_config(pci_dev, address, val, len);
        }
    }

    if (proxy->flags & VIRTIO_PCI_FLAG_ATS) {
        pcie_ats_config_write(pci_dev, address, val, len);
    }

    if (range_covers_byte(address, len, PCI_COMMAND)) {
        if (!(pci_dev->config[PCI_COMMAND] & PCI_COMMAND_MASTER)) {
            virtio_set_disabled(vdev, true);
            virtio_pci_stop_ioeventfd(proxy);
            virtio_set_status(vdev, vdev->status & ~VIRTIO_CONFIG_S_DRIVER_OK);
        } else {
            virtio_set_disabled(vdev, false);
        }
    }

    if (proxy->config_cap &&
        ranges_overlap(address, len, proxy->config_cap + offsetof(struct virtio_pci_cfg_cap,
                                                                  pci_cfg_data),
                       sizeof cfg->pci_cfg_data)) {
        uint32_t off;
        uint32_t len;

        cfg = (void *)(proxy->pci_dev.config + proxy->config_cap);
        off = le32_to_cpu(cfg->cap.offset);
        len = le32_to_cpu(cfg->cap.length);

        if (len == 1 || len == 2 || len == 4) {
            assert(len <= sizeof cfg->pci_cfg_data);
            virtio_address_space_write(proxy, off, cfg->pci_cfg_data, len);
        }
    }
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    pci_config_read_slot_end(pci_dev);
#endif
}

static uint32_t virtio_read_config(PCIDevice *pci_dev,
                                   uint32_t address, int len)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(pci_dev);
    struct virtio_pci_cfg_cap *cfg;

    if (proxy->config_cap &&
        ranges_overlap(address, len, proxy->config_cap + offsetof(struct virtio_pci_cfg_cap,
                                                                  pci_cfg_data),
                       sizeof cfg->pci_cfg_data)) {
        uint32_t off;
        uint32_t len;

        cfg = (void *)(proxy->pci_dev.config + proxy->config_cap);
        off = le32_to_cpu(cfg->cap.offset);
        len = le32_to_cpu(cfg->cap.length);

        if (len == 1 || len == 2 || len == 4) {
            assert(len <= sizeof cfg->pci_cfg_data);
            virtio_address_space_read(proxy, off, cfg->pci_cfg_data, len);
        }
    }

    return pci_default_read_config(pci_dev, address, len);
}

static int kvm_virtio_pci_vq_vector_use(VirtIOPCIProxy *proxy,
                                        unsigned int vector)
{
    VirtIOIRQFD *irqfd = &proxy->vector_irqfd[vector];
    int ret;

    if (irqfd->users == 0) {
        KVMRouteChange c = kvm_irqchip_begin_route_changes(kvm_state);
        ret = kvm_irqchip_add_msi_route(&c, vector, &proxy->pci_dev);
        if (ret < 0) {
            return ret;
        }
        kvm_irqchip_commit_route_changes(&c);
        irqfd->virq = ret;
    }
    irqfd->users++;
    return 0;
}

static void kvm_virtio_pci_vq_vector_release(VirtIOPCIProxy *proxy,
                                             unsigned int vector)
{
    VirtIOIRQFD *irqfd = &proxy->vector_irqfd[vector];
    if (--irqfd->users == 0) {
        kvm_irqchip_release_virq(kvm_state, irqfd->virq);
    }
}

static int kvm_virtio_pci_irqfd_use(VirtIOPCIProxy *proxy,
                                 EventNotifier *n,
                                 unsigned int vector)
{
    VirtIOIRQFD *irqfd = &proxy->vector_irqfd[vector];
    return kvm_irqchip_add_irqfd_notifier_gsi(kvm_state, n, NULL, irqfd->virq);
}

static void kvm_virtio_pci_irqfd_release(VirtIOPCIProxy *proxy,
                                      EventNotifier *n ,
                                      unsigned int vector)
{
    VirtIOIRQFD *irqfd = &proxy->vector_irqfd[vector];
    int ret;

    ret = kvm_irqchip_remove_irqfd_notifier_gsi(kvm_state, n, irqfd->virq);
    assert(ret == 0);
}
static int virtio_pci_get_notifier(VirtIOPCIProxy *proxy, int queue_no,
                                      EventNotifier **n, unsigned int *vector)
{
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtQueue *vq;

    if (queue_no == VIRTIO_CONFIG_IRQ_IDX) {
        *n = virtio_config_get_guest_notifier(vdev);
        *vector = vdev->config_vector;
    } else {
        if (!virtio_queue_get_num(vdev, queue_no)) {
            return -1;
        }
        *vector = virtio_queue_vector(vdev, queue_no);
        vq = virtio_get_queue(vdev, queue_no);
        *n = virtio_queue_get_guest_notifier(vq);
    }
    return 0;
}

static int kvm_virtio_pci_vector_use_one(VirtIOPCIProxy *proxy, int queue_no)
{
    unsigned int vector;
    int ret;
    EventNotifier *n;
    PCIDevice *dev = &proxy->pci_dev;
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);

    ret = virtio_pci_get_notifier(proxy, queue_no, &n, &vector);
    if (ret < 0) {
        return ret;
    }
    if (vector >= msix_nr_vectors_allocated(dev)) {
        return 0;
    }
    ret = kvm_virtio_pci_vq_vector_use(proxy, vector);
    if (ret < 0) {
        goto undo;
    }
    /*
     * If guest supports masking, set up irqfd now.
     * Otherwise, delay until unmasked in the frontend.
     */
    if (vdev->use_guest_notifier_mask && k->guest_notifier_mask) {
        ret = kvm_virtio_pci_irqfd_use(proxy, n, vector);
        if (ret < 0) {
            kvm_virtio_pci_vq_vector_release(proxy, vector);
            goto undo;
        }
    }

    return 0;
undo:

    vector = virtio_queue_vector(vdev, queue_no);
    if (vector >= msix_nr_vectors_allocated(dev)) {
        return ret;
    }
    if (vdev->use_guest_notifier_mask && k->guest_notifier_mask) {
        ret = virtio_pci_get_notifier(proxy, queue_no, &n, &vector);
        if (ret < 0) {
            return ret;
        }
        kvm_virtio_pci_irqfd_release(proxy, n, vector);
    }
    return ret;
}
static int kvm_virtio_pci_vector_vq_use(VirtIOPCIProxy *proxy, int nvqs)
{
    int queue_no;
    int ret = 0;
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    for (queue_no = 0; queue_no < nvqs; queue_no++) {
        if (!virtio_queue_get_num(vdev, queue_no)) {
            return -1;
        }
        ret = kvm_virtio_pci_vector_use_one(proxy, queue_no);
    }
    return ret;
}

static int kvm_virtio_pci_vector_config_use(VirtIOPCIProxy *proxy)
{
    return kvm_virtio_pci_vector_use_one(proxy, VIRTIO_CONFIG_IRQ_IDX);
}

static void kvm_virtio_pci_vector_release_one(VirtIOPCIProxy *proxy,
                                              int queue_no)
{
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    unsigned int vector;
    EventNotifier *n;
    int ret;
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    PCIDevice *dev = &proxy->pci_dev;

    ret = virtio_pci_get_notifier(proxy, queue_no, &n, &vector);
    if (ret < 0) {
        return;
    }
    if (vector >= msix_nr_vectors_allocated(dev)) {
        return;
    }
    if (vdev->use_guest_notifier_mask && k->guest_notifier_mask) {
        kvm_virtio_pci_irqfd_release(proxy, n, vector);
    }
    kvm_virtio_pci_vq_vector_release(proxy, vector);
}

static void kvm_virtio_pci_vector_vq_release(VirtIOPCIProxy *proxy, int nvqs)
{
    int queue_no;
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    for (queue_no = 0; queue_no < nvqs; queue_no++) {
        if (!virtio_queue_get_num(vdev, queue_no)) {
            break;
        }
        kvm_virtio_pci_vector_release_one(proxy, queue_no);
    }
}

static void kvm_virtio_pci_vector_config_release(VirtIOPCIProxy *proxy)
{
    kvm_virtio_pci_vector_release_one(proxy, VIRTIO_CONFIG_IRQ_IDX);
}

static int virtio_pci_one_vector_unmask(VirtIOPCIProxy *proxy,
                                       unsigned int queue_no,
                                       unsigned int vector,
                                       MSIMessage msg,
                                       EventNotifier *n)
{
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    VirtIOIRQFD *irqfd;
    int ret = 0;

    if (proxy->vector_irqfd) {
        irqfd = &proxy->vector_irqfd[vector];
        if (irqfd->msg.data != msg.data || irqfd->msg.address != msg.address) {
            ret = kvm_irqchip_update_msi_route(kvm_state, irqfd->virq, msg,
                                               &proxy->pci_dev);
            if (ret < 0) {
                return ret;
            }
            kvm_irqchip_commit_routes(kvm_state);
        }
    }

    /* If guest supports masking, irqfd is already setup, unmask it.
     * Otherwise, set it up now.
     */
    if (vdev->use_guest_notifier_mask && k->guest_notifier_mask) {
        k->guest_notifier_mask(vdev, queue_no, false);
        /* Test after unmasking to avoid losing events. */
        if (k->guest_notifier_pending &&
            k->guest_notifier_pending(vdev, queue_no)) {
            event_notifier_set(n);
        }
    } else {
        ret = kvm_virtio_pci_irqfd_use(proxy, n, vector);
    }
    return ret;
}

static void virtio_pci_one_vector_mask(VirtIOPCIProxy *proxy,
                                             unsigned int queue_no,
                                             unsigned int vector,
                                             EventNotifier *n)
{
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);

    /* If guest supports masking, keep irqfd but mask it.
     * Otherwise, clean it up now.
     */ 
    if (vdev->use_guest_notifier_mask && k->guest_notifier_mask) {
        k->guest_notifier_mask(vdev, queue_no, true);
    } else {
        kvm_virtio_pci_irqfd_release(proxy, n, vector);
    }
}

static int virtio_pci_vector_unmask(PCIDevice *dev, unsigned vector,
                                    MSIMessage msg)
{
    VirtIOPCIProxy *proxy = container_of(dev, VirtIOPCIProxy, pci_dev);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtQueue *vq = virtio_vector_first_queue(vdev, vector);
    EventNotifier *n;
    int ret, index, unmasked = 0;

    while (vq) {
        index = virtio_get_queue_index(vq);
        if (!virtio_queue_get_num(vdev, index)) {
            break;
        }
        if (index < proxy->nvqs_with_notifiers) {
            n = virtio_queue_get_guest_notifier(vq);
            ret = virtio_pci_one_vector_unmask(proxy, index, vector, msg, n);
            if (ret < 0) {
                goto undo;
            }
            ++unmasked;
        }
        vq = virtio_vector_next_queue(vq);
    }
    /* unmask config intr */
    if (vector == vdev->config_vector) {
        n = virtio_config_get_guest_notifier(vdev);
        ret = virtio_pci_one_vector_unmask(proxy, VIRTIO_CONFIG_IRQ_IDX, vector,
                                           msg, n);
        if (ret < 0) {
            goto undo_config;
        }
    }
    return 0;
undo_config:
    n = virtio_config_get_guest_notifier(vdev);
    virtio_pci_one_vector_mask(proxy, VIRTIO_CONFIG_IRQ_IDX, vector, n);
undo:
    vq = virtio_vector_first_queue(vdev, vector);
    while (vq && unmasked >= 0) {
        index = virtio_get_queue_index(vq);
        if (index < proxy->nvqs_with_notifiers) {
            n = virtio_queue_get_guest_notifier(vq);
            virtio_pci_one_vector_mask(proxy, index, vector, n);
            --unmasked;
        }
        vq = virtio_vector_next_queue(vq);
    }
    return ret;
}

static void virtio_pci_vector_mask(PCIDevice *dev, unsigned vector)
{
    VirtIOPCIProxy *proxy = container_of(dev, VirtIOPCIProxy, pci_dev);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtQueue *vq = virtio_vector_first_queue(vdev, vector);
    EventNotifier *n;
    int index;

    while (vq) {
        index = virtio_get_queue_index(vq);
        n = virtio_queue_get_guest_notifier(vq);
        if (!virtio_queue_get_num(vdev, index)) {
            break;
        }
        if (index < proxy->nvqs_with_notifiers) {
            virtio_pci_one_vector_mask(proxy, index, vector, n);
        }
        vq = virtio_vector_next_queue(vq);
    }

    if (vector == vdev->config_vector) {
        n = virtio_config_get_guest_notifier(vdev);
        virtio_pci_one_vector_mask(proxy, VIRTIO_CONFIG_IRQ_IDX, vector, n);
    }
}

static void virtio_pci_vector_poll(PCIDevice *dev,
                                   unsigned int vector_start,
                                   unsigned int vector_end)
{
    VirtIOPCIProxy *proxy = container_of(dev, VirtIOPCIProxy, pci_dev);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    int queue_no;
    unsigned int vector;
    EventNotifier *notifier;
    int ret;

    for (queue_no = 0; queue_no < proxy->nvqs_with_notifiers; queue_no++) {
        ret = virtio_pci_get_notifier(proxy, queue_no, &notifier, &vector);
        if (ret < 0) {
            break;
        }
        if (vector < vector_start || vector >= vector_end ||
            !msix_is_masked(dev, vector)) {
            continue;
        }
        if (k->guest_notifier_pending) {
            if (k->guest_notifier_pending(vdev, queue_no)) {
                msix_set_pending(dev, vector);
            }
        } else if (event_notifier_test_and_clear(notifier)) {
            msix_set_pending(dev, vector);
        }
    }
    /* poll the config intr */
    ret = virtio_pci_get_notifier(proxy, VIRTIO_CONFIG_IRQ_IDX, &notifier,
                                  &vector);
    if (ret < 0) {
        return;
    }
    if (vector < vector_start || vector >= vector_end ||
        !msix_is_masked(dev, vector)) {
        return;
    }
    if (k->guest_notifier_pending) {
        if (k->guest_notifier_pending(vdev, VIRTIO_CONFIG_IRQ_IDX)) {
            msix_set_pending(dev, vector);
        }
    } else if (event_notifier_test_and_clear(notifier)) {
        msix_set_pending(dev, vector);
    }
}

void virtio_pci_set_guest_notifier_fd_handler(VirtIODevice *vdev, VirtQueue *vq,
                                              int n, bool assign,
                                              bool with_irqfd)
{
    if (n == VIRTIO_CONFIG_IRQ_IDX) {
        virtio_config_set_guest_notifier_fd_handler(vdev, assign, with_irqfd);
    } else {
        virtio_queue_set_guest_notifier_fd_handler(vq, assign, with_irqfd);
    }
}

static int virtio_pci_set_guest_notifier(DeviceState *d, int n, bool assign,
                                         bool with_irqfd)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_GET_CLASS(vdev);
    VirtQueue *vq = NULL;
    EventNotifier *notifier = NULL;

    if (n == VIRTIO_CONFIG_IRQ_IDX) {
        notifier = virtio_config_get_guest_notifier(vdev);
    } else {
        vq = virtio_get_queue(vdev, n);
        notifier = virtio_queue_get_guest_notifier(vq);
    }

    if (assign) {
        int r = event_notifier_init(notifier, 0);
        if (r < 0) {
            return r;
        }
        virtio_pci_set_guest_notifier_fd_handler(vdev, vq, n, true, with_irqfd);
    } else {
        virtio_pci_set_guest_notifier_fd_handler(vdev, vq, n, false,
                                                 with_irqfd);
        event_notifier_cleanup(notifier);
    }

    if (!msix_enabled(&proxy->pci_dev) &&
        vdev->use_guest_notifier_mask &&
        vdc->guest_notifier_mask) {
        vdc->guest_notifier_mask(vdev, n, !assign);
    }

    return 0;
}

static bool virtio_pci_query_guest_notifiers(DeviceState *d)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    return msix_enabled(&proxy->pci_dev);
}

static int virtio_pci_set_guest_notifiers(DeviceState *d, int nvqs, bool assign)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    VirtioDeviceClass *k = VIRTIO_DEVICE_GET_CLASS(vdev);
    int r, n;
    bool with_irqfd = msix_enabled(&proxy->pci_dev) &&
        kvm_msi_via_irqfd_enabled();

    nvqs = MIN(nvqs, VIRTIO_QUEUE_MAX);

    /*
     * When deassigning, pass a consistent nvqs value to avoid leaking
     * notifiers. But first check we've actually been configured, exit
     * early if we haven't.
     */
    if (!assign && !proxy->nvqs_with_notifiers) {
        return 0;
    }
    assert(assign || nvqs == proxy->nvqs_with_notifiers);

    proxy->nvqs_with_notifiers = nvqs;

    /* Must unset vector notifier while guest notifier is still assigned */
    if ((proxy->vector_irqfd ||
         (vdev->use_guest_notifier_mask && k->guest_notifier_mask)) &&
        !assign) {
        msix_unset_vector_notifiers(&proxy->pci_dev);
        if (proxy->vector_irqfd) {
            kvm_virtio_pci_vector_vq_release(proxy, nvqs);
            kvm_virtio_pci_vector_config_release(proxy);
            g_free(proxy->vector_irqfd);
            proxy->vector_irqfd = NULL;
        }
    }

    for (n = 0; n < nvqs; n++) {
        if (!virtio_queue_get_num(vdev, n)) {
            break;
        }

        r = virtio_pci_set_guest_notifier(d, n, assign, with_irqfd);
        if (r < 0) {
            goto assign_error;
        }
    }
    r = virtio_pci_set_guest_notifier(d, VIRTIO_CONFIG_IRQ_IDX, assign,
                                      with_irqfd);
    if (r < 0) {
        goto config_assign_error;
    }
    /* Must set vector notifier after guest notifier has been assigned */
    if ((with_irqfd ||
         (vdev->use_guest_notifier_mask && k->guest_notifier_mask)) &&
        assign) {
        if (with_irqfd) {
            proxy->vector_irqfd =
                g_malloc0(sizeof(*proxy->vector_irqfd) *
                          msix_nr_vectors_allocated(&proxy->pci_dev));
            r = kvm_virtio_pci_vector_vq_use(proxy, nvqs);
            if (r < 0) {
                goto config_assign_error;
            }
            r = kvm_virtio_pci_vector_config_use(proxy);
            if (r < 0) {
                goto config_error;
            }
        }

        r = msix_set_vector_notifiers(&proxy->pci_dev, virtio_pci_vector_unmask,
                                      virtio_pci_vector_mask,
                                      virtio_pci_vector_poll);
        if (r < 0) {
            goto notifiers_error;
        }
    }

    return 0;

notifiers_error:
    if (with_irqfd) {
        assert(assign);
        kvm_virtio_pci_vector_vq_release(proxy, nvqs);
    }
config_error:
    if (with_irqfd) {
        kvm_virtio_pci_vector_config_release(proxy);
    }
config_assign_error:
    virtio_pci_set_guest_notifier(d, VIRTIO_CONFIG_IRQ_IDX, !assign,
                                  with_irqfd);
assign_error:
    /* We get here on assignment failure. Recover by undoing for VQs 0 .. n. */
    assert(assign);
    while (--n >= 0) {
        virtio_pci_set_guest_notifier(d, n, !assign, with_irqfd);
    }
    g_free(proxy->vector_irqfd);
    proxy->vector_irqfd = NULL;
    return r;
}

static int virtio_pci_set_host_notifier_mr(DeviceState *d, int n,
                                           MemoryRegion *mr, bool assign)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    int offset;

    if (n >= VIRTIO_QUEUE_MAX || !virtio_pci_modern(proxy) ||
        virtio_pci_queue_mem_mult(proxy) != memory_region_size(mr)) {
        return -1;
    }

    if (assign) {
        offset = virtio_pci_queue_mem_mult(proxy) * n;
        memory_region_add_subregion_overlap(&proxy->notify.mr, offset, mr, 1);
    } else {
        memory_region_del_subregion(&proxy->notify.mr, mr);
    }

    return 0;
}

static void virtio_pci_vmstate_change(DeviceState *d, bool running)
{
    VirtIOPCIProxy *proxy = to_virtio_pci_proxy(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    if (proxy->queue_trace_enabled.notify) {
        virtio_pci_update_queue_config_device(proxy, vdev);
    }
    if (running) {
        /* Old QEMU versions did not set bus master enable on status write.
         * Detect DRIVER set and enable it.
         */
        if ((proxy->flags & VIRTIO_PCI_FLAG_BUS_MASTER_BUG_MIGRATION) &&
            (vdev->status & VIRTIO_CONFIG_S_DRIVER) &&
            !(proxy->pci_dev.config[PCI_COMMAND] & PCI_COMMAND_MASTER)) {
            pci_default_write_config(&proxy->pci_dev, PCI_COMMAND,
                                     proxy->pci_dev.config[PCI_COMMAND] |
                                     PCI_COMMAND_MASTER, 1);
        }
        if (!proxy->guest_reset_pending) {
            virtio_pci_start_ioeventfd(proxy);
        }
    } else {
        virtio_pci_stop_ioeventfd(proxy);
    }
}

/*
 * virtio-pci: This is the PCIDevice which has a virtio-pci-bus.
 */

static int virtio_pci_query_nvectors(DeviceState *d)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(d);

    return proxy->nvectors;
}

static AddressSpace *virtio_pci_get_dma_as(DeviceState *d)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(d);
    PCIDevice *dev = &proxy->pci_dev;

    return pci_get_address_space(dev);
}

static bool virtio_pci_iommu_enabled(DeviceState *d)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(d);
    PCIDevice *dev = &proxy->pci_dev;
    AddressSpace *dma_as = pci_device_iommu_address_space(dev);

    if (dma_as == &address_space_memory) {
        return false;
    }

    return true;
}

static bool virtio_pci_queue_enabled(DeviceState *d, int n)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    if (virtio_vdev_has_feature(vdev, VIRTIO_F_VERSION_1)) {
        return proxy->vqs[n].enabled;
    }

    return virtio_queue_enabled_legacy(vdev, n);
}

static int virtio_pci_add_mem_cap(VirtIOPCIProxy *proxy,
                                   struct virtio_pci_cap *cap)
{
    PCIDevice *dev = &proxy->pci_dev;
    int offset;

    offset = pci_add_capability(dev, PCI_CAP_ID_VNDR, 0,
                                cap->cap_len, &error_abort);

    assert(cap->cap_len >= sizeof *cap);
    memcpy(dev->config + offset + PCI_CAP_FLAGS, &cap->cap_len,
           cap->cap_len - PCI_CAP_FLAGS);

    return offset;
}

int virtio_pci_add_shm_cap(VirtIOPCIProxy *proxy,
                           uint8_t bar, uint64_t offset, uint64_t length,
                           uint8_t id)
{
    struct virtio_pci_cap64 cap = {
        .cap.cap_len = sizeof cap,
        .cap.cfg_type = VIRTIO_PCI_CAP_SHARED_MEMORY_CFG,
    };

    cap.cap.bar = bar;
    cap.cap.length = cpu_to_le32(length);
    cap.length_hi = cpu_to_le32(length >> 32);
    cap.cap.offset = cpu_to_le32(offset);
    cap.offset_hi = cpu_to_le32(offset >> 32);
    cap.cap.id = id;
    return virtio_pci_add_mem_cap(proxy, &cap.cap);
}

static uint32_t virtio_pci_common_read_value(VirtIOPCIProxy *proxy,
                                             VirtIODevice *vdev, hwaddr addr)
{
    uint32_t val = 0;
    int i;

    switch (addr) {
    case VIRTIO_PCI_COMMON_DFSELECT:
        val = proxy->dfselect;
        break;
    case VIRTIO_PCI_COMMON_DF:
        if (proxy->dfselect <= 1) {
            VirtioDeviceClass *vdc = VIRTIO_DEVICE_GET_CLASS(vdev);

            val = (vdev->host_features & ~vdc->legacy_features) >>
                (32 * proxy->dfselect);
        }
        break;
    case VIRTIO_PCI_COMMON_GFSELECT:
        val = proxy->gfselect;
        break;
    case VIRTIO_PCI_COMMON_GF:
        if (proxy->gfselect < ARRAY_SIZE(proxy->guest_features)) {
            val = proxy->guest_features[proxy->gfselect];
        }
        break;
    case VIRTIO_PCI_COMMON_MSIX:
        val = vdev->config_vector;
        break;
    case VIRTIO_PCI_COMMON_NUMQ:
        for (i = 0; i < VIRTIO_QUEUE_MAX; ++i) {
            if (virtio_queue_get_num(vdev, i)) {
                val = i + 1;
            }
        }
        break;
    case VIRTIO_PCI_COMMON_STATUS:
        val = vdev->status;
        break;
    case VIRTIO_PCI_COMMON_CFGGENERATION:
        val = vdev->generation;
        break;
    case VIRTIO_PCI_COMMON_Q_SELECT:
        val = vdev->queue_sel;
        break;
    case VIRTIO_PCI_COMMON_Q_SIZE:
        val = virtio_queue_get_num(vdev, vdev->queue_sel);
        break;
    case VIRTIO_PCI_COMMON_Q_MSIX:
        val = virtio_queue_vector(vdev, vdev->queue_sel);
        break;
    case VIRTIO_PCI_COMMON_Q_ENABLE:
        val = proxy->vqs[vdev->queue_sel].enabled;
        break;
    case VIRTIO_PCI_COMMON_Q_NOFF:
        /* Simply map queues in order */
        val = vdev->queue_sel;
        break;
    case VIRTIO_PCI_COMMON_Q_DESCLO:
        val = proxy->vqs[vdev->queue_sel].desc[0];
        break;
    case VIRTIO_PCI_COMMON_Q_DESCHI:
        val = proxy->vqs[vdev->queue_sel].desc[1];
        break;
    case VIRTIO_PCI_COMMON_Q_AVAILLO:
        val = proxy->vqs[vdev->queue_sel].avail[0];
        break;
    case VIRTIO_PCI_COMMON_Q_AVAILHI:
        val = proxy->vqs[vdev->queue_sel].avail[1];
        break;
    case VIRTIO_PCI_COMMON_Q_USEDLO:
        val = proxy->vqs[vdev->queue_sel].used[0];
        break;
    case VIRTIO_PCI_COMMON_Q_USEDHI:
        val = proxy->vqs[vdev->queue_sel].used[1];
        break;
    case VIRTIO_PCI_COMMON_Q_RESET:
        val = proxy->vqs[vdev->queue_sel].reset;
        break;
    default:
        val = 0;
    }

    return val;
}

static uint64_t virtio_pci_common_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    g_autoptr(VirtIOPCIQueueConfigPause) paused =
        virtio_pci_pause_queue_config(proxy);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    g_autoptr(VirtioNetworkLock) network = virtio_lock_network(vdev);

    return vdev ? virtio_pci_common_read_value(proxy, vdev, addr) : UINT64_MAX;
}

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static bool virtio_pci_common_read_size_ok(hwaddr addr, unsigned size)
{
    switch (size) {
    case 1:
        return addr == VIRTIO_PCI_COMMON_STATUS ||
               addr == VIRTIO_PCI_COMMON_CFGGENERATION;
    case 2:
        return addr == VIRTIO_PCI_COMMON_MSIX ||
               addr == VIRTIO_PCI_COMMON_NUMQ ||
               addr == VIRTIO_PCI_COMMON_Q_SELECT ||
               addr == VIRTIO_PCI_COMMON_Q_SIZE ||
               addr == VIRTIO_PCI_COMMON_Q_MSIX ||
               addr == VIRTIO_PCI_COMMON_Q_ENABLE ||
               addr == VIRTIO_PCI_COMMON_Q_NOFF ||
               addr == VIRTIO_PCI_COMMON_Q_RESET;
    case 4:
        return addr == VIRTIO_PCI_COMMON_DFSELECT ||
               addr == VIRTIO_PCI_COMMON_DF ||
               addr == VIRTIO_PCI_COMMON_GFSELECT ||
               addr == VIRTIO_PCI_COMMON_GF ||
               (addr >= VIRTIO_PCI_COMMON_Q_DESCLO &&
                addr <= VIRTIO_PCI_COMMON_Q_USEDHI && !(addr & 3));
    default:
        return false;
    }
}

static bool virtio_pci_try_read_queue_config(void *opaque, hwaddr addr,
                                             MemOp op, MemTxAttrs attrs,
                                             uint64_t *value)
{
    VirtIOPCIProxy *proxy = opaque;
    VirtIODevice *vdev;
    g_autoptr(VirtioNetworkLock) guard = NULL;
    bool handled;

    if (!virtio_pci_common_read_size_ok(addr, memop_size(op)) ||
        !qatomic_read(&proxy->queue_config_device) ||
        qemu_mutex_trylock(&proxy->queue_config_lock)) {
        return false;
    }

    vdev = qatomic_read(&proxy->queue_config_device);
    handled = vdev && !proxy->queue_config_paused &&
              !trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_READ);
    if (handled && vdev->network_mutex) {
        if (qemu_rec_mutex_trylock(vdev->network_mutex)) {
            handled = false;
        } else {
            guard = vdev->network_mutex;
        }
    }
    if (handled) {
        *value = virtio_pci_common_read_value(proxy, vdev, addr);
    }
    qemu_mutex_unlock(&proxy->queue_config_lock);
    return handled;
}
#endif

static void virtio_pci_cancel_deferred_guest_reset(VirtIOPCIProxy *proxy)
{
    VirtIODevice *vdev;

    if (!proxy->guest_reset_pending) {
        return;
    }

    vdev = virtio_bus_get_device(&proxy->bus);
    if (vdev) {
        vdev->reset_pending = false;
    }
    qemu_bh_cancel(proxy->guest_reset_bh);
    proxy->guest_reset_pending = false;
    proxy->guest_flr_pending = false;
}

static void virtio_pci_deferred_guest_reset(void *opaque)
{
    VirtIOPCIProxy *proxy = opaque;
    g_autoptr(VirtIOPCIQueueConfigPause) paused =
        virtio_pci_pause_queue_config(proxy);
    g_autoptr(VirtIOPCIIRQRouteGuard) route_guard G_GNUC_UNUSED =
        virtio_pci_guard_irq_routes(proxy);
    VirtIODevice *vdev;

    if (!proxy->guest_reset_pending) {
        return;
    }

    proxy->guest_reset_pending = false;
    vdev = virtio_bus_get_device(&proxy->bus);
    g_autoptr(VirtioNetworkLock) network = virtio_lock_network(vdev);
    if (vdev) {
        if (proxy->guest_flr_pending) {
            uint8_t *devctl = proxy->pci_dev.config +
                              proxy->pci_dev.exp.exp_cap + PCI_EXP_DEVCTL;

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
            pci_config_read_slot_begin(&proxy->pci_dev);
#endif
            pci_word_test_and_set_mask(devctl, PCI_EXP_DEVCTL_BCR_FLR);
            pci_device_reset(&proxy->pci_dev);
            pci_word_test_and_clear_mask(devctl, PCI_EXP_DEVCTL_BCR_FLR);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
            pci_config_read_slot_end(&proxy->pci_dev);
#endif
        }
        if (vdev->reset_pending) {
            virtio_pci_reset(DEVICE(proxy));
        }
        virtio_set_disabled(vdev,
            !(proxy->pci_dev.config[PCI_COMMAND] & PCI_COMMAND_MASTER));
    }
    proxy->guest_flr_pending = false;
}

static void virtio_pci_stage_queue_config(VirtIOPCIProxy *proxy,
                                           VirtIODevice *vdev,
                                           hwaddr addr, uint32_t value)
{
    VirtIOPCIQueue *queue;
    unsigned word = (addr >> 2) & 1;

    if (addr == VIRTIO_PCI_COMMON_Q_SELECT) {
        if (value < VIRTIO_QUEUE_MAX) {
            qatomic_set(&vdev->queue_sel, value);
        }
        return;
    }

    queue = &proxy->vqs[qatomic_read(&vdev->queue_sel)];
    switch (addr) {
    case VIRTIO_PCI_COMMON_Q_DESCLO:
    case VIRTIO_PCI_COMMON_Q_DESCHI:
        queue->desc[word] = value;
        break;
    case VIRTIO_PCI_COMMON_Q_AVAILLO:
    case VIRTIO_PCI_COMMON_Q_AVAILHI:
        queue->avail[word] = value;
        break;
    case VIRTIO_PCI_COMMON_Q_USEDLO:
    case VIRTIO_PCI_COMMON_Q_USEDHI:
        queue->used[word] = value;
        break;
    default:
        g_assert_not_reached();
    }
}

static void virtio_pci_set_queue_size(VirtIOPCIProxy *proxy,
                                      VirtIODevice *vdev, uint16_t size)
{
    unsigned n = vdev->queue_sel;

    proxy->vqs[n].num = size;
    virtio_queue_set_num(vdev, n, size);
    virtio_init_region_cache(vdev, n);
}

static uint64_t virtio_pci_ring_address(const uint32_t words[2])
{
    return ((uint64_t)words[1] << 32) | words[0];
}

static bool virtio_pci_try_configure_queue(VirtIOPCIProxy *proxy,
                                           VirtIODevice *vdev, hwaddr addr,
                                           uint16_t value)
{
    unsigned n = vdev->queue_sel;
    VirtIOPCIQueue *queue = &proxy->vqs[n];
    g_autoptr(VirtioNetworkLock) guard = NULL;

    if (vdev->network_mutex) {
        /* The legacy port acquires the device lock before pausing configuration. */
        if (qemu_rec_mutex_trylock(vdev->network_mutex)) {
            return false;
        }
        guard = vdev->network_mutex;
    } else if (vdev->device_id == VIRTIO_ID_CONSOLE ||
               (vdev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        return false;
    }

    /* Live rings can remap guest memory and report mapping errors. */
    if (proxy->vqs[n].enabled || virtio_queue_get_desc_addr(vdev, n) ||
        virtio_queue_get_avail_addr(vdev, n)) {
        return false;
    }
    if (addr == VIRTIO_PCI_COMMON_Q_SIZE) {
        virtio_pci_set_queue_size(proxy, vdev, value);
        return true;
    }

    /* A driver may leave the default queue size untouched. */
    if (value != 1 ||
        (queue->num && queue->num != virtio_queue_get_num(vdev, n)) ||
        (VIRTIO_DEVICE_GET_CLASS(vdev)->queue_enable &&
         !(vdev->device_id == VIRTIO_ID_NET && !vdev->vhost_started)) ||
        !virtio_queue_try_set_rings(vdev, n,
                                    virtio_pci_ring_address(queue->desc),
                                    virtio_pci_ring_address(queue->avail),
                                    virtio_pci_ring_address(queue->used))) {
        return false;
    }
    queue->enabled = true;
    queue->reset = false;
    return true;
}

static bool virtio_pci_try_set_queue_vector(VirtIOPCIProxy *proxy,
                                            VirtIODevice *vdev,
                                            uint16_t value)
{
    PCIDevice *dev = &proxy->pci_dev;
    unsigned n = vdev->queue_sel;
    uint16_t old_vector;
    g_autoptr(VirtioNetworkLock) guard = NULL;

    if (vdev->network_mutex) {
        /* The legacy port acquires the device lock before pausing configuration. */
        if (qemu_rec_mutex_trylock(vdev->network_mutex)) {
            return false;
        }
        guard = vdev->network_mutex;
    } else if (vdev->device_id == VIRTIO_ID_CONSOLE) {
        return false;
    }

    /* Live drivers or IRQFD notifiers need BQL. */
    if (!msix_present(dev) ||
        (vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) ||
        vdev->output_irqs_may_be_pending ||
        proxy->nvqs_with_notifiers ||
        dev->msix_vector_use_notifier ||
        dev->msix_vector_poll_notifier) {
        return false;
    }
    if (value < proxy->nvectors && value >= dev->msix_entries_nr) {
        return false;
    }
    old_vector = virtio_queue_vector(vdev, n);
    if (old_vector != VIRTIO_NO_VECTOR) {
        msix_vector_unuse(dev, old_vector);
    }
    if (value < proxy->nvectors) {
        msix_vector_use(dev, value);
    } else {
        value = VIRTIO_NO_VECTOR;
    }
    virtio_queue_set_vector(vdev, n, value);
    return true;
}

static bool virtio_pci_try_set_config_vector(VirtIOPCIProxy *proxy,
                                             VirtIODevice *vdev,
                                             uint16_t value)
{
    PCIDevice *dev = &proxy->pci_dev;
    g_autoptr(VirtioNetworkLock) guard = NULL;

    if (vdev->network_mutex) {
        if (qemu_rec_mutex_trylock(vdev->network_mutex)) {
            return false;
        }
        guard = vdev->network_mutex;
    } else if (vdev->device_id == VIRTIO_ID_CONSOLE) {
        return false;
    }

    if (!msix_present(dev) ||
        (vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) ||
        vdev->output_irqs_may_be_pending ||
        proxy->nvqs_with_notifiers ||
        dev->msix_vector_use_notifier ||
        dev->msix_vector_poll_notifier) {
        return false;
    }
    if (value < proxy->nvectors && value >= dev->msix_entries_nr) {
        return false;
    }
    if (vdev->config_vector != VIRTIO_NO_VECTOR) {
        msix_vector_unuse(dev, vdev->config_vector);
    }
    if (value < proxy->nvectors) {
        msix_vector_use(dev, value);
    } else {
        value = VIRTIO_NO_VECTOR;
    }
    vdev->config_vector = value;
    return true;
}

static bool virtio_pci_try_set_guest_features(VirtIOPCIProxy *proxy,
                                              VirtIODevice *vdev,
                                              uint32_t value)
{
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_GET_CLASS(vdev);
    uint32_t words[2];
    uint64_t features;
    g_autoptr(VirtioNetworkLock) guard = NULL;

    if (proxy->gfselect >= ARRAY_SIZE(proxy->guest_features)) {
        return true;
    }
    if ((vdev->status & (VIRTIO_CONFIG_S_FEATURES_OK |
                         VIRTIO_CONFIG_S_DRIVER_OK)) ||
        (vdc->set_features &&
         !(vdev->device_id == VIRTIO_ID_NET && !vdev->vhost_started &&
           vdev->network_mutex))) {
        return false;
    }
    if (vdev->network_mutex) {
        if (qemu_rec_mutex_trylock(vdev->network_mutex)) {
            return false;
        }
        guard = vdev->network_mutex;
    }

    words[0] = proxy->guest_features[0];
    words[1] = proxy->guest_features[1];
    words[proxy->gfselect] = value;
    features = (((uint64_t)words[1]) << 32) | words[0];
    if (features & (1ULL << VIRTIO_F_BAD_FEATURE)) {
        return false;
    }
    if (vdev->device_id == VIRTIO_ID_NET &&
        (features & ((1ULL << 22 /* VIRTIO_NET_F_MQ */) |
                     (1ULL << 60 /* VIRTIO_NET_F_RSS */)))) {
        return false;
    }

    proxy->guest_features[proxy->gfselect] = value;
    {
        RCU_READ_LOCK_GUARD();
        virtio_set_features(vdev, features);
    }
    return true;
}

static bool virtio_pci_try_set_status(VirtIOPCIProxy *proxy,
                                      VirtIODevice *vdev,
                                      uint8_t value)
{
    g_autoptr(VirtioNetworkLock) guard = NULL;

    /*
     * Only handle non-reset, pre-DRIVER_OK staging writes
     * (ACKNOWLEDGE / DRIVER / FEATURES_OK) without BQL.
     */
    if (value == 0 ||
        (value & VIRTIO_CONFIG_S_DRIVER_OK) ||
        (vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) ||
        proxy->bus.ioeventfd_started ||
        !vdev->vm_running ||
        trace_event_get_state_backends(TRACE_VIRTIO_SET_STATUS)) {
        return false;
    }
    if (vdev->device_id == VIRTIO_ID_NET &&
        (vdev->vhost_started || !vdev->network_mutex)) {
        return false;
    }
    if (vdev->network_mutex) {
        if (qemu_rec_mutex_trylock(vdev->network_mutex)) {
            return false;
        }
        guard = vdev->network_mutex;
    }

    virtio_set_status(vdev, value);
    return true;
}

static void virtio_pci_common_write(void *opaque, hwaddr addr,
                                    uint64_t val, unsigned size);

static bool virtio_pci_try_stage_queue_config(void *opaque, hwaddr addr,
                                              unsigned size, uint64_t value)
{
    VirtIOPCIProxy *proxy = opaque;
    VirtIODevice *vdev;
    bool staged;

    if (!((size == 1 && addr == VIRTIO_PCI_COMMON_STATUS) ||
          (size == 2 && (addr == VIRTIO_PCI_COMMON_MSIX ||
                         addr == VIRTIO_PCI_COMMON_Q_SELECT ||
                         addr == VIRTIO_PCI_COMMON_Q_SIZE ||
                         addr == VIRTIO_PCI_COMMON_Q_MSIX ||
                         addr == VIRTIO_PCI_COMMON_Q_ENABLE)) ||
          (size == 4 && (addr == VIRTIO_PCI_COMMON_DFSELECT ||
                         addr == VIRTIO_PCI_COMMON_GFSELECT ||
                         addr == VIRTIO_PCI_COMMON_GF ||
                         (addr >= VIRTIO_PCI_COMMON_Q_DESCLO &&
                          addr <= VIRTIO_PCI_COMMON_Q_USEDHI &&
                          !(addr & 3))))) ||
        !qatomic_read(&proxy->queue_config_device) ||
        qemu_mutex_trylock(&proxy->queue_config_lock)) {
        if (lats_nobql_enabled() &&
            !trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_WRITE)) {
            qemu_memory_topology_lock();
            virtio_pci_common_write(opaque, addr, value, size);
            qemu_memory_topology_unlock();
            return true;
        }
        return false;
    }

    vdev = qatomic_read(&proxy->queue_config_device);
    staged = vdev && !proxy->queue_config_paused &&
             !proxy->guest_reset_pending &&
             !trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_WRITE);
    if (staged && addr == VIRTIO_PCI_COMMON_DFSELECT) {
        proxy->dfselect = value;
    } else if (staged && addr == VIRTIO_PCI_COMMON_GFSELECT) {
        proxy->gfselect = value;
    } else if (staged && addr == VIRTIO_PCI_COMMON_GF) {
        staged = virtio_pci_try_set_guest_features(proxy, vdev, value);
    } else if (staged && addr == VIRTIO_PCI_COMMON_MSIX) {
        staged = virtio_pci_try_set_config_vector(proxy, vdev, value);
    } else if (staged && addr == VIRTIO_PCI_COMMON_STATUS) {
        staged = virtio_pci_try_set_status(proxy, vdev, value & 0xff);
    } else if (staged && addr == VIRTIO_PCI_COMMON_Q_MSIX) {
        staged = virtio_pci_try_set_queue_vector(proxy, vdev, value);
    } else if (staged && (addr == VIRTIO_PCI_COMMON_Q_SIZE ||
                          addr == VIRTIO_PCI_COMMON_Q_ENABLE)) {
        staged = virtio_pci_try_configure_queue(proxy, vdev, addr, value);
    } else if (staged) {
        virtio_pci_stage_queue_config(proxy, vdev, addr, value);
    }
    qemu_mutex_unlock(&proxy->queue_config_lock);
    if (!staged && lats_nobql_enabled() &&
        !trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_WRITE)) {
        qemu_memory_topology_lock();
        virtio_pci_common_write(opaque, addr, value, size);
        qemu_memory_topology_unlock();
        return true;
    }
    return staged;
}

static void virtio_pci_common_write(void *opaque, hwaddr addr,
                                    uint64_t val, unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    g_autoptr(VirtIOPCIQueueConfigPause) paused =
        virtio_pci_pause_queue_config(proxy);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    g_autoptr(VirtioNetworkLock) network = virtio_lock_network(vdev);
    uint16_t vector;
    g_autoptr(VirtIOPCIIRQRouteGuard) route_guard G_GNUC_UNUSED =
        virtio_pci_guard_irq_routes(proxy);

    if (vdev == NULL) {
        return;
    }

    if (proxy->guest_reset_pending &&
        addr != VIRTIO_PCI_COMMON_STATUS) {
        return;
    }

    switch (addr) {
    case VIRTIO_PCI_COMMON_DFSELECT:
        proxy->dfselect = val;
        break;
    case VIRTIO_PCI_COMMON_GFSELECT:
        proxy->gfselect = val;
        break;
    case VIRTIO_PCI_COMMON_GF:
        if (proxy->gfselect < ARRAY_SIZE(proxy->guest_features)) {
            proxy->guest_features[proxy->gfselect] = val;
            virtio_set_features(vdev,
                                (((uint64_t)proxy->guest_features[1]) << 32) |
                                proxy->guest_features[0]);
        }
        break;
    case VIRTIO_PCI_COMMON_MSIX:
        if (vdev->config_vector != VIRTIO_NO_VECTOR) {
            msix_vector_unuse(&proxy->pci_dev, vdev->config_vector);
        }
        /* Make it possible for guest to discover an error took place. */
        if (val < proxy->nvectors) {
            msix_vector_use(&proxy->pci_dev, val);
        } else {
            val = VIRTIO_NO_VECTOR;
        }
        vdev->config_vector = val;
        break;
    case VIRTIO_PCI_COMMON_STATUS:
        if (proxy->guest_reset_pending) {
            break;
        }
        if (!(val & VIRTIO_CONFIG_S_DRIVER_OK)) {
            virtio_pci_stop_ioeventfd(proxy);
        }

        if ((val & 0xff) == 0 &&
            VIRTIO_DEVICE_GET_CLASS(vdev)->defer_modern_guest_reset &&
            qemu_in_vcpu_thread()) {
            if (vdev->status == 0) {
                break;
            }
            /* Keep the old status visible until the reset BH completes. */
            vdev->reset_pending = true;
            proxy->guest_reset_pending = true;
            if (!proxy->guest_reset_bh) {
                proxy->guest_reset_bh = qemu_bh_new_guarded(
                    virtio_pci_deferred_guest_reset, proxy,
                    &DEVICE(proxy)->mem_reentrancy_guard);
            }
            qemu_bh_schedule(proxy->guest_reset_bh);
            break;
        }

        virtio_set_status(vdev, val & 0xFF);

        if (val & VIRTIO_CONFIG_S_DRIVER_OK) {
            virtio_pci_start_ioeventfd(proxy);
        }

        if (vdev->status == 0) {
            virtio_pci_reset(DEVICE(proxy));
        }

        break;
    case VIRTIO_PCI_COMMON_Q_SIZE:
        virtio_pci_set_queue_size(proxy, vdev, val);
        break;
    case VIRTIO_PCI_COMMON_Q_MSIX:
        if (vdev->flush_output_irqs) {
            vdev->flush_output_irqs(vdev);
        }
        vector = virtio_queue_vector(vdev, vdev->queue_sel);
        if (vector != VIRTIO_NO_VECTOR) {
            msix_vector_unuse(&proxy->pci_dev, vector);
        }
        /* Make it possible for guest to discover an error took place. */
        if (val < proxy->nvectors) {
            msix_vector_use(&proxy->pci_dev, val);
        } else {
            val = VIRTIO_NO_VECTOR;
        }
        virtio_queue_set_vector(vdev, vdev->queue_sel, val);
        break;
    case VIRTIO_PCI_COMMON_Q_ENABLE:
        if (val == 1) {
            VirtIOPCIQueue *queue = &proxy->vqs[vdev->queue_sel];

            virtio_queue_set_num(vdev, vdev->queue_sel, queue->num);
            virtio_queue_set_rings(vdev, vdev->queue_sel,
                       virtio_pci_ring_address(queue->desc),
                       virtio_pci_ring_address(queue->avail),
                       virtio_pci_ring_address(queue->used));
            proxy->vqs[vdev->queue_sel].enabled = 1;
            proxy->vqs[vdev->queue_sel].reset = 0;
            virtio_queue_enable(vdev, vdev->queue_sel);
        } else {
            virtio_error(vdev, "wrong value for queue_enable %"PRIx64, val);
        }
        break;
    case VIRTIO_PCI_COMMON_Q_SELECT:
    case VIRTIO_PCI_COMMON_Q_DESCLO:
    case VIRTIO_PCI_COMMON_Q_DESCHI:
    case VIRTIO_PCI_COMMON_Q_AVAILLO:
    case VIRTIO_PCI_COMMON_Q_AVAILHI:
    case VIRTIO_PCI_COMMON_Q_USEDLO:
    case VIRTIO_PCI_COMMON_Q_USEDHI:
        virtio_pci_stage_queue_config(proxy, vdev, addr, val);
        break;
    case VIRTIO_PCI_COMMON_Q_RESET:
        if (val == 1) {
            proxy->vqs[vdev->queue_sel].reset = 1;

            virtio_queue_reset(vdev, vdev->queue_sel);

            proxy->vqs[vdev->queue_sel].reset = 0;
            proxy->vqs[vdev->queue_sel].enabled = 0;
        }
        break;
    default:
        break;
    }
}


static uint64_t virtio_pci_notify_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    if (virtio_bus_get_device(&proxy->bus) == NULL) {
        return UINT64_MAX;
    }

    return 0;
}

static void virtio_pci_notify_write(void *opaque, hwaddr addr,
                                    uint64_t val, unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    unsigned queue = addr / virtio_pci_queue_mem_mult(proxy);

    if (!proxy->guest_reset_pending && vdev != NULL &&
        queue < VIRTIO_QUEUE_MAX) {
        trace_virtio_pci_notify_write(addr, val, size);
        virtio_queue_notify(vdev, queue);
    }
}

static void virtio_pci_notify_write_pio(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    unsigned queue = val;

    if (!proxy->guest_reset_pending && vdev != NULL &&
        queue < VIRTIO_QUEUE_MAX) {
        trace_virtio_pci_notify_write_pio(addr, val, size);
        virtio_queue_notify(vdev, queue);
    }
}

static uint64_t virtio_pci_isr_read(void *opaque, hwaddr addr,
                                    unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    if (vdev == NULL) {
        return UINT64_MAX;
    }

    return virtio_pci_ack_isr(proxy, vdev);
}

static bool virtio_pci_try_read_isr(void *opaque, hwaddr addr, MemOp op,
                                    MemTxAttrs attrs, uint64_t *value)
{
    VirtIOPCIProxy *proxy = opaque;
    unsigned long required = VIRTIO_PCI_ISR_PRESENT | VIRTIO_PCI_ISR_INTX_LOW;
    unsigned long mask = required | VIRTIO_PCI_ISR_ASYNC_INTX | VIRTIO_ISR_MASK;

    if (addr || memop_size(op) > 4 ||
        (irq_commit_source_read(&proxy->bus.interrupt_state) & mask) != required) {
        if (lats_nobql_enabled() &&
            !trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_READ)) {
            qemu_memory_topology_lock();
            *value = virtio_pci_isr_read(opaque, addr, memop_size(op));
            qemu_memory_topology_unlock();
            return true;
        }
        return false;
    }

    *value = 0;
    return true;
}

static void virtio_pci_isr_write(void *opaque, hwaddr addr,
                                 uint64_t val, unsigned size)
{
}

static uint64_t virtio_pci_device_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    g_autoptr(VirtIOPCIQueueConfigPause) paused =
        virtio_pci_pause_queue_config(proxy);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    g_autoptr(VirtioNetworkLock) network = virtio_lock_network(vdev);
    uint64_t val;

    if (vdev == NULL) {
        return UINT64_MAX;
    }

    switch (size) {
    case 1:
        val = virtio_config_modern_readb(vdev, addr);
        break;
    case 2:
        val = virtio_config_modern_readw(vdev, addr);
        break;
    case 4:
        val = virtio_config_modern_readl(vdev, addr);
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static bool virtio_pci_try_read_device_config(void *opaque, hwaddr addr,
                                              MemOp op, MemTxAttrs attrs,
                                              uint64_t *value)
{
    VirtIOPCIProxy *proxy = opaque;
    VirtIODevice *vdev;
    unsigned size = memop_size(op);
    g_autoptr(VirtioNetworkLock) guard = NULL;
    bool handled;

    if ((size != 1 && size != 2 && size != 4) ||
        !qatomic_read(&proxy->queue_config_device) ||
        qemu_mutex_trylock(&proxy->queue_config_lock)) {
        return false;
    }

    vdev = qatomic_read(&proxy->queue_config_device);
    handled = vdev && !proxy->queue_config_paused &&
              (vdev->device_id == VIRTIO_ID_SCSI ||
               vdev->device_id == VIRTIO_ID_NET ||
               vdev->device_id == VIRTIO_ID_CONSOLE ||
               vdev->device_id == VIRTIO_ID_BALLOON) &&
              !vdev->vhost_started &&
              !trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_READ);
    if (handled && vdev->network_mutex) {
        if (qemu_rec_mutex_trylock(vdev->network_mutex)) {
            handled = false;
        } else {
            guard = vdev->network_mutex;
        }
    }
    if (handled) {
        switch (size) {
        case 1:
            *value = virtio_config_modern_readb(vdev, addr);
            break;
        case 2:
            *value = virtio_config_modern_readw(vdev, addr);
            break;
        case 4:
            *value = virtio_config_modern_readl(vdev, addr);
            break;
        default:
            handled = false;
            break;
        }
    }
    qemu_mutex_unlock(&proxy->queue_config_lock);
    return handled;
}

static bool virtio_pci_try_notify_lockless(void *opaque, hwaddr addr,
                                           unsigned size, uint64_t value)
{
    VirtIOPCIProxy *proxy = opaque;
    VirtIODevice *vdev;
    unsigned stride;
    unsigned queue;
    bool handled = false;

    if (!qatomic_read(&proxy->queue_config_device) ||
        qemu_mutex_trylock(&proxy->queue_config_lock)) {
        if (lats_nobql_enabled() &&
            !trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_WRITE)) {
            qemu_memory_topology_lock();
            virtio_pci_notify_write(opaque, addr, value, size);
            qemu_memory_topology_unlock();
            return true;
        }
        return false;
    }

    vdev = qatomic_read(&proxy->queue_config_device);
    if (vdev && !proxy->queue_config_paused &&
        !proxy->guest_reset_pending && !vdev->broken &&
        !vdev->start_on_kick &&
        virtio_vdev_has_feature(vdev, VIRTIO_F_VERSION_1) &&
        !trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_WRITE) &&
        !trace_event_get_state_backends(TRACE_VIRTIO_PCI_NOTIFY_WRITE) &&
        !trace_event_get_state_backends(TRACE_VIRTIO_QUEUE_NOTIFY)) {
        stride = virtio_pci_queue_mem_mult(proxy);
        queue = stride ? addr / stride : VIRTIO_QUEUE_MAX;
        if (queue < VIRTIO_QUEUE_MAX) {
            /*
             * virtio-serial c_ivq (queue 2) handler control_in() is a no-op;
             * virtio-scsi event queue (queue 1) pre-DRIVER_OK kick is a no-op.
             */
            if ((vdev->device_id == VIRTIO_ID_CONSOLE && queue == 2) ||
                (vdev->device_id == VIRTIO_ID_SCSI && queue == 1 &&
                 !(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK))) {
                handled = true;
            }
        }
    }
    qemu_mutex_unlock(&proxy->queue_config_lock);
    if (!handled && lats_nobql_enabled() &&
        !trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_WRITE)) {
        qemu_memory_topology_lock();
        virtio_pci_notify_write(opaque, addr, value, size);
        qemu_memory_topology_unlock();
        return true;
    }
    return handled;
}
#endif

static void virtio_pci_device_write(void *opaque, hwaddr addr,
                                    uint64_t val, unsigned size)
{
    VirtIOPCIProxy *proxy = opaque;
    g_autoptr(VirtIOPCIQueueConfigPause) paused =
        virtio_pci_pause_queue_config(proxy);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);
    g_autoptr(VirtioNetworkLock) network = virtio_lock_network(vdev);

    if (vdev == NULL || proxy->guest_reset_pending) {
        return;
    }

    switch (size) {
    case 1:
        virtio_config_modern_writeb(vdev, addr, val);
        break;
    case 2:
        virtio_config_modern_writew(vdev, addr, val);
        break;
    case 4:
        virtio_config_modern_writel(vdev, addr, val);
        break;
    }
}

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static bool virtio_pci_try_write_device_config(void *opaque, hwaddr addr,
                                               unsigned size, uint64_t val)
{
    if (!lats_nobql_enabled() ||
        trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_WRITE)) {
        return false;
    }
    qemu_memory_topology_lock();
    virtio_pci_device_write(opaque, addr, val, size);
    qemu_memory_topology_unlock();
    return true;
}
#endif

static void virtio_pci_modern_regions_init(VirtIOPCIProxy *proxy,
                                           const char *vdev_name)
{
    static const MemoryRegionOps common_ops = {
        .read = virtio_pci_common_read,
        .write = virtio_pci_common_write,
        .try_write_lockless = virtio_pci_try_stage_queue_config,
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        .try_read_lockless = virtio_pci_try_read_queue_config,
        .mmio_split_pause = virtio_pci_pause_split_queue_config,
        .mmio_split_resume = virtio_pci_resume_split_queue_config,
#endif
        .impl = {
            .min_access_size = 1,
            .max_access_size = 4,
        },
        .endianness = DEVICE_LITTLE_ENDIAN,
    };
    static const MemoryRegionOps isr_ops = {
        .read = virtio_pci_isr_read,
        .write = virtio_pci_isr_write,
        .try_read_lockless = virtio_pci_try_read_isr,
        .impl = {
            .min_access_size = 1,
            .max_access_size = 4,
        },
        .endianness = DEVICE_LITTLE_ENDIAN,
    };
    static const MemoryRegionOps device_ops = {
        .read = virtio_pci_device_read,
        .write = virtio_pci_device_write,
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        .try_read_lockless = virtio_pci_try_read_device_config,
        .try_write_lockless = virtio_pci_try_write_device_config,
#endif
        .impl = {
            .min_access_size = 1,
            .max_access_size = 4,
        },
        .endianness = DEVICE_LITTLE_ENDIAN,
    };
    static const MemoryRegionOps notify_ops = {
        .read = virtio_pci_notify_read,
        .write = virtio_pci_notify_write,
        .try_write_eventfd = virtio_pci_try_notify_eventfd,
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        .try_write_lockless = virtio_pci_try_notify_lockless,
#endif
        .impl = {
            .min_access_size = 1,
            .max_access_size = 4,
        },
        .endianness = DEVICE_LITTLE_ENDIAN,
    };
    static const MemoryRegionOps notify_pio_ops = {
        .read = virtio_pci_notify_read,
        .write = virtio_pci_notify_write_pio,
        .impl = {
            .min_access_size = 1,
            .max_access_size = 4,
        },
        .endianness = DEVICE_LITTLE_ENDIAN,
    };
    g_autoptr(GString) name = g_string_new(NULL);

    g_string_printf(name, "virtio-pci-common-%s", vdev_name);
    memory_region_init_io(&proxy->common.mr, OBJECT(proxy),
                          &common_ops,
                          proxy,
                          name->str,
                          proxy->common.size);

    g_string_printf(name, "virtio-pci-isr-%s", vdev_name);
    memory_region_init_io(&proxy->isr.mr, OBJECT(proxy),
                          &isr_ops,
                          proxy,
                          name->str,
                          proxy->isr.size);

    g_string_printf(name, "virtio-pci-device-%s", vdev_name);
    memory_region_init_io(&proxy->device.mr, OBJECT(proxy),
                          &device_ops,
                          proxy,
                          name->str,
                          proxy->device.size);

    g_string_printf(name, "virtio-pci-notify-%s", vdev_name);
    memory_region_init_io(&proxy->notify.mr, OBJECT(proxy),
                          &notify_ops,
                          proxy,
                          name->str,
                          proxy->notify.size);

    g_string_printf(name, "virtio-pci-notify-pio-%s", vdev_name);
    memory_region_init_io(&proxy->notify_pio.mr, OBJECT(proxy),
                          &notify_pio_ops,
                          proxy,
                          name->str,
                          proxy->notify_pio.size);
}

static void virtio_pci_modern_region_map(VirtIOPCIProxy *proxy,
                                         VirtIOPCIRegion *region,
                                         struct virtio_pci_cap *cap,
                                         MemoryRegion *mr,
                                         uint8_t bar)
{
    memory_region_add_subregion(mr, region->offset, &region->mr);

    cap->cfg_type = region->type;
    cap->bar = bar;
    cap->offset = cpu_to_le32(region->offset);
    cap->length = cpu_to_le32(region->size);
    virtio_pci_add_mem_cap(proxy, cap);

}

static void virtio_pci_modern_mem_region_map(VirtIOPCIProxy *proxy,
                                             VirtIOPCIRegion *region,
                                             struct virtio_pci_cap *cap)
{
    virtio_pci_modern_region_map(proxy, region, cap,
                                 &proxy->modern_bar, proxy->modern_mem_bar_idx);
}

static void virtio_pci_modern_io_region_map(VirtIOPCIProxy *proxy,
                                            VirtIOPCIRegion *region,
                                            struct virtio_pci_cap *cap)
{
    virtio_pci_modern_region_map(proxy, region, cap,
                                 &proxy->io_bar, proxy->modern_io_bar_idx);
}

static void virtio_pci_modern_mem_region_unmap(VirtIOPCIProxy *proxy,
                                               VirtIOPCIRegion *region)
{
    memory_region_del_subregion(&proxy->modern_bar,
                                &region->mr);
}

static void virtio_pci_modern_io_region_unmap(VirtIOPCIProxy *proxy,
                                              VirtIOPCIRegion *region)
{
    memory_region_del_subregion(&proxy->io_bar,
                                &region->mr);
}

static void virtio_pci_pre_plugged(DeviceState *d, Error **errp)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(d);
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    if (virtio_pci_modern(proxy)) {
        virtio_add_feature(&vdev->host_features, VIRTIO_F_VERSION_1);
    }

    virtio_add_feature(&vdev->host_features, VIRTIO_F_BAD_FEATURE);
}

/* This is called by virtio-bus just after the device is plugged. */
static void virtio_pci_device_plugged(DeviceState *d, Error **errp)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(d);
    VirtioBusState *bus = &proxy->bus;
    bool legacy = virtio_pci_legacy(proxy);
    bool modern;
    bool modern_pio = proxy->flags & VIRTIO_PCI_FLAG_MODERN_PIO_NOTIFY;
    uint8_t *config;
    uint32_t size;
    VirtIODevice *vdev = virtio_bus_get_device(&proxy->bus);

    /*
     * Virtio capabilities present without
     * VIRTIO_F_VERSION_1 confuses guests
     */
    if (!proxy->ignore_backend_features &&
            !virtio_has_feature(vdev->host_features, VIRTIO_F_VERSION_1)) {
        virtio_pci_disable_modern(proxy);

        if (!legacy) {
            error_setg(errp, "Device doesn't support modern mode, and legacy"
                             " mode is disabled");
            error_append_hint(errp, "Set disable-legacy to off\n");

            return;
        }
    }

    modern = virtio_pci_modern(proxy);

    config = proxy->pci_dev.config;
    if (proxy->class_code) {
        pci_config_set_class(config, proxy->class_code);
    }

    if (legacy) {
        if (!virtio_legacy_allowed(vdev)) {
            /*
             * To avoid migration issues, we allow legacy mode when legacy
             * check is disabled in the old machine types (< 5.1).
             */
            if (virtio_legacy_check_disabled(vdev)) {
                warn_report("device is modern-only, but for backward "
                            "compatibility legacy is allowed");
            } else {
                error_setg(errp,
                           "device is modern-only, use disable-legacy=on");
                return;
            }
        }
        if (virtio_host_has_feature(vdev, VIRTIO_F_IOMMU_PLATFORM)) {
            error_setg(errp, "VIRTIO_F_IOMMU_PLATFORM was supported by"
                       " neither legacy nor transitional device");
            return;
        }
        /*
         * Legacy and transitional devices use specific subsystem IDs.
         * Note that the subsystem vendor ID (config + PCI_SUBSYSTEM_VENDOR_ID)
         * is set to PCI_SUBVENDOR_ID_REDHAT_QUMRANET by default.
         */
        pci_set_word(config + PCI_SUBSYSTEM_ID, virtio_bus_get_vdev_id(bus));
        if (proxy->trans_devid) {
            pci_config_set_device_id(config, proxy->trans_devid);
        }
    } else {
        /* pure virtio-1.0 */
        pci_set_word(config + PCI_VENDOR_ID,
                     PCI_VENDOR_ID_REDHAT_QUMRANET);
        pci_set_word(config + PCI_DEVICE_ID,
                     PCI_DEVICE_ID_VIRTIO_10_BASE + virtio_bus_get_vdev_id(bus));
        pci_config_set_revision(config, 1);
    }
    config[PCI_INTERRUPT_PIN] = 1;


    if (modern) {
        struct virtio_pci_cap cap = {
            .cap_len = sizeof cap,
        };
        struct virtio_pci_notify_cap notify = {
            .cap.cap_len = sizeof notify,
            .notify_off_multiplier =
                cpu_to_le32(virtio_pci_queue_mem_mult(proxy)),
        };
        struct virtio_pci_cfg_cap cfg = {
            .cap.cap_len = sizeof cfg,
            .cap.cfg_type = VIRTIO_PCI_CAP_PCI_CFG,
        };
        struct virtio_pci_notify_cap notify_pio = {
            .cap.cap_len = sizeof notify,
            .notify_off_multiplier = cpu_to_le32(0x0),
        };

        struct virtio_pci_cfg_cap *cfg_mask;

        virtio_pci_modern_regions_init(proxy, vdev->name);

        virtio_pci_modern_mem_region_map(proxy, &proxy->common, &cap);
        virtio_pci_modern_mem_region_map(proxy, &proxy->isr, &cap);
        virtio_pci_modern_mem_region_map(proxy, &proxy->device, &cap);
        virtio_pci_modern_mem_region_map(proxy, &proxy->notify, &notify.cap);

        if (modern_pio) {
            memory_region_init(&proxy->io_bar, OBJECT(proxy),
                               "virtio-pci-io", 0x4);

            pci_register_bar(&proxy->pci_dev, proxy->modern_io_bar_idx,
                             PCI_BASE_ADDRESS_SPACE_IO, &proxy->io_bar);

            virtio_pci_modern_io_region_map(proxy, &proxy->notify_pio,
                                            &notify_pio.cap);
        }

        pci_register_bar(&proxy->pci_dev, proxy->modern_mem_bar_idx,
                         PCI_BASE_ADDRESS_SPACE_MEMORY |
                         PCI_BASE_ADDRESS_MEM_PREFETCH |
                         PCI_BASE_ADDRESS_MEM_TYPE_64,
                         &proxy->modern_bar);

        proxy->config_cap = virtio_pci_add_mem_cap(proxy, &cfg.cap);
        cfg_mask = (void *)(proxy->pci_dev.wmask + proxy->config_cap);
        pci_set_byte(&cfg_mask->cap.bar, ~0x0);
        pci_set_long((uint8_t *)&cfg_mask->cap.offset, ~0x0);
        pci_set_long((uint8_t *)&cfg_mask->cap.length, ~0x0);
        pci_set_long(cfg_mask->pci_cfg_data, ~0x0);
    }

    if (proxy->nvectors) {
        int err = msix_init_exclusive_bar(&proxy->pci_dev, proxy->nvectors,
                                          proxy->msix_bar_idx, NULL);
        if (err) {
            /* Notice when a system that supports MSIx can't initialize it */
            if (err != -ENOTSUP) {
                warn_report("unable to init msix vectors to %" PRIu32,
                            proxy->nvectors);
            }
            proxy->nvectors = 0;
        }
    }

    proxy->pci_dev.config_write = virtio_write_config;
    proxy->pci_dev.config_read = virtio_read_config;

    if (legacy) {
        size = VIRTIO_PCI_REGION_SIZE(&proxy->pci_dev)
            + virtio_bus_get_vdev_config_len(bus);
        size = pow2ceil(size);

        memory_region_init_io(&proxy->bar, OBJECT(proxy),
                              &virtio_pci_config_ops,
                              proxy, "virtio-pci", size);

        pci_register_bar(&proxy->pci_dev, proxy->legacy_io_bar_idx,
                         PCI_BASE_ADDRESS_SPACE_IO, &proxy->bar);
    }
    if (modern) {
        irq_commit_source_update(&proxy->bus.interrupt_state, 0, VIRTIO_PCI_ISR_PRESENT);
        virtio_pci_publish_intx_low(proxy);
    }

    proxy->direct_irq_device = 0;
    if (modern && tcg_enabled() &&
        ((vdev->device_id == VIRTIO_ID_NET &&
          lats_nobql_enabled()) ||
         (vdev->device_id == VIRTIO_ID_CONSOLE &&
          vdev->try_output)) &&
        apic_msi_commit_configured && apic_msi_commit_configured()) {
        proxy->direct_irq_device = vdev->device_id;
    }
    if (proxy->direct_irq_device == VIRTIO_ID_NET) {
        static bool report_installed;

        if (!report_installed) {
            report_installed = true;
            qemu_add_vm_change_state_handler(trial_net_irq_vmstate, NULL);
            atexit(trial_net_irq_exit);
        }
    }

    if (vdev->device_id == VIRTIO_ID_SCSI && tcg_enabled() &&
        lats_nobql_enabled()) {
        static bool report_installed;

        proxy->direct_irq_device = VIRTIO_ID_SCSI;

        if (!report_installed) {
            report_installed = true;
            trial_scsi_disable_direct = false;
            qemu_add_vm_change_state_handler(trial_scsi_vmstate, NULL);
            atexit(trial_scsi_exit);
            trial_scsi_enable_notifier_counts();
        }
    }
    if (proxy->direct_irq_device) {
        qatomic_store_release(&proxy->direct_irq_enabled, true);
        proxy->direct_irq_listener.begin = virtio_pci_irq_map_begin;
        proxy->direct_irq_listener.commit = virtio_pci_irq_map_end;
        proxy->direct_irq_listener.nobql_safe = true;
        memory_listener_register(&proxy->direct_irq_listener,
                                 &proxy->pci_dev.bus_master_as);
    }
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    if (modern && pci_config_size(&proxy->pci_dev) == PCI_CONFIG_SPACE_SIZE &&
        (vdev->device_id == VIRTIO_ID_NET || vdev->device_id == VIRTIO_ID_SCSI ||
         vdev->device_id == VIRTIO_ID_BALLOON ||
         vdev->device_id == VIRTIO_ID_CONSOLE)) {
        unsigned data = proxy->config_cap +
                        offsetof(struct virtio_pci_cfg_cap, pci_cfg_data);
        pci_config_read_slot_register(&proxy->pci_dev, data,
            vdev->flush_output_irqs ? vdev->network_mutex : NULL,
            vdev->flush_output_irqs ? &vdev->output_irqs_may_be_pending : NULL);
    }
    if (modern && tcg_enabled() && !icount_enabled() &&
        replay_mode == REPLAY_MODE_NONE &&
        ((vdev->device_id == VIRTIO_ID_CONSOLE ||
          vdev->device_id == VIRTIO_ID_SCSI ||
          vdev->device_id == VIRTIO_ID_NET ||
          vdev->device_id == VIRTIO_ID_BALLOON) &&
         lats_nobql_enabled())) {
        proxy->queue_config_listener.begin = virtio_pci_queue_config_map_begin;
        proxy->queue_config_listener.commit = virtio_pci_queue_config_map_end;
        proxy->queue_config_listener.nobql_safe = true;
        memory_listener_register(&proxy->queue_config_listener,
                                 &address_space_memory);
        proxy->queue_config_listener_registered = true;
        proxy->queue_trace_enabled.notify = virtio_pci_queue_trace_enabled;
        trace_add_enable_notifier(&proxy->queue_trace_enabled);
        virtio_pci_update_queue_config_device(proxy, vdev);
    }
#endif
}

static void virtio_pci_device_unplugged(DeviceState *d)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(d);
    bool modern = virtio_pci_modern(proxy);
    bool modern_pio = proxy->flags & VIRTIO_PCI_FLAG_MODERN_PIO_NOTIFY;
    g_autoptr(VirtIOPCIIRQRouteGuard) route_guard G_GNUC_UNUSED =
        virtio_pci_guard_irq_routes(proxy);

    if (proxy->queue_trace_enabled.notify) {
        virtio_pci_update_queue_config_device(proxy, NULL);
        trace_remove_enable_notifier(&proxy->queue_trace_enabled);
        proxy->queue_trace_enabled.notify = NULL;
    }
    if (proxy->queue_config_listener_registered) {
        memory_listener_unregister(&proxy->queue_config_listener);
        proxy->queue_config_listener_registered = false;
    }

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    pci_config_read_slot_remove(&proxy->pci_dev);
#endif
    irq_commit_source_update(&proxy->bus.interrupt_state, VIRTIO_PCI_ISR_PRESENT, 0);
    virtio_pci_cancel_deferred_guest_reset(proxy);
    virtio_pci_stop_ioeventfd(proxy);

    if (modern) {
        virtio_pci_modern_mem_region_unmap(proxy, &proxy->common);
        virtio_pci_modern_mem_region_unmap(proxy, &proxy->isr);
        virtio_pci_modern_mem_region_unmap(proxy, &proxy->device);
        virtio_pci_modern_mem_region_unmap(proxy, &proxy->notify);
        if (modern_pio) {
            virtio_pci_modern_io_region_unmap(proxy, &proxy->notify_pio);
        }
    }
    if (proxy->direct_irq_enabled) {
        memory_listener_unregister(&proxy->direct_irq_listener);
        /* Wait-free for the completion path: revoke the source first. */
        qatomic_store_release(&proxy->direct_irq_enabled, false);
        proxy->direct_irq_depth = 0;
        VirtIOPCIDirectRoutes *old = proxy->direct_irq_routes;
        qatomic_rcu_set(&proxy->direct_irq_routes, NULL);
        if (old) {
            g_free_rcu(old, rcu);
        }
    }
}

static void virtio_pci_realize(PCIDevice *pci_dev, Error **errp)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(pci_dev);
    VirtioPCIClass *k = VIRTIO_PCI_GET_CLASS(pci_dev);
    bool pcie_port = pci_bus_is_express(pci_get_bus(pci_dev)) &&
                     !pci_bus_is_root(pci_get_bus(pci_dev));

    if (kvm_enabled() && !kvm_has_many_ioeventfds()) {
        proxy->flags &= ~VIRTIO_PCI_FLAG_USE_IOEVENTFD;
    }

    /* fd-based ioevents can't be synchronized in record/replay */
    if (replay_mode != REPLAY_MODE_NONE) {
        proxy->flags &= ~VIRTIO_PCI_FLAG_USE_IOEVENTFD;
    }

    /*
     * virtio pci bar layout used by default.
     * subclasses can re-arrange things if needed.
     *
     *   region 0   --  virtio legacy io bar
     *   region 1   --  msi-x bar
     *   region 2   --  virtio modern io bar (off by default)
     *   region 4+5 --  virtio modern memory (64bit) bar
     *
     */
    proxy->legacy_io_bar_idx  = 0;
    proxy->msix_bar_idx       = 1;
    proxy->modern_io_bar_idx  = 2;
    proxy->modern_mem_bar_idx = 4;

    proxy->common.offset = 0x0;
    proxy->common.size = 0x1000;
    proxy->common.type = VIRTIO_PCI_CAP_COMMON_CFG;

    proxy->isr.offset = 0x1000;
    proxy->isr.size = 0x1000;
    proxy->isr.type = VIRTIO_PCI_CAP_ISR_CFG;

    proxy->device.offset = 0x2000;
    proxy->device.size = 0x1000;
    proxy->device.type = VIRTIO_PCI_CAP_DEVICE_CFG;

    proxy->notify.offset = 0x3000;
    proxy->notify.size = virtio_pci_queue_mem_mult(proxy) * VIRTIO_QUEUE_MAX;
    proxy->notify.type = VIRTIO_PCI_CAP_NOTIFY_CFG;

    proxy->notify_pio.offset = 0x0;
    proxy->notify_pio.size = 0x4;
    proxy->notify_pio.type = VIRTIO_PCI_CAP_NOTIFY_CFG;

    /* subclasses can enforce modern, so do this unconditionally */
    memory_region_init(&proxy->modern_bar, OBJECT(proxy), "virtio-pci",
                       /* PCI BAR regions must be powers of 2 */
                       pow2ceil(proxy->notify.offset + proxy->notify.size));

    if (proxy->disable_legacy == ON_OFF_AUTO_AUTO) {
        proxy->disable_legacy = pcie_port ? ON_OFF_AUTO_ON : ON_OFF_AUTO_OFF;
    }

    if (!virtio_pci_modern(proxy) && !virtio_pci_legacy(proxy)) {
        error_setg(errp, "device cannot work as neither modern nor legacy mode"
                   " is enabled");
        error_append_hint(errp, "Set either disable-modern or disable-legacy"
                          " to off\n");
        return;
    }

    if (pcie_port && pci_is_express(pci_dev)) {
        int pos;
        uint16_t last_pcie_cap_offset = PCI_CONFIG_SPACE_SIZE;

        pos = pcie_endpoint_cap_init(pci_dev, 0);
        assert(pos > 0);

        pos = pci_add_capability(pci_dev, PCI_CAP_ID_PM, 0,
                                 PCI_PM_SIZEOF, errp);
        if (pos < 0) {
            return;
        }

        pci_dev->exp.pm_cap = pos;

        /*
         * Indicates that this function complies with revision 1.2 of the
         * PCI Power Management Interface Specification.
         */
        pci_set_word(pci_dev->config + pos + PCI_PM_PMC, 0x3);

        if (proxy->flags & VIRTIO_PCI_FLAG_AER) {
            pcie_aer_init(pci_dev, PCI_ERR_VER, last_pcie_cap_offset,
                          PCI_ERR_SIZEOF, NULL);
            last_pcie_cap_offset += PCI_ERR_SIZEOF;
        }

        if (proxy->flags & VIRTIO_PCI_FLAG_INIT_DEVERR) {
            /* Init error enabling flags */
            pcie_cap_deverr_init(pci_dev);
        }

        if (proxy->flags & VIRTIO_PCI_FLAG_INIT_LNKCTL) {
            /* Init Link Control Register */
            pcie_cap_lnkctl_init(pci_dev);
        }

        if (proxy->flags & VIRTIO_PCI_FLAG_PM_NO_SOFT_RESET) {
            pci_set_word(pci_dev->config + pos + PCI_PM_CTRL,
                         PCI_PM_CTRL_NO_SOFT_RESET);
        }

        if (proxy->flags & VIRTIO_PCI_FLAG_INIT_PM) {
            /* Init Power Management Control Register */
            pci_set_word(pci_dev->wmask + pos + PCI_PM_CTRL,
                         PCI_PM_CTRL_STATE_MASK);
        }

        if (proxy->flags & VIRTIO_PCI_FLAG_ATS) {
            pcie_ats_init(pci_dev, last_pcie_cap_offset,
                          proxy->flags & VIRTIO_PCI_FLAG_ATS_PAGE_ALIGNED);
            last_pcie_cap_offset += PCI_EXT_CAP_ATS_SIZEOF;
        }

        if (proxy->flags & VIRTIO_PCI_FLAG_INIT_FLR) {
            /* Set Function Level Reset capability bit */
            pcie_cap_flr_init(pci_dev);
        }
    } else {
        /*
         * make future invocations of pci_is_express() return false
         * and pci_config_size() return PCI_CONFIG_SPACE_SIZE.
         */
        pci_dev->cap_present &= ~QEMU_PCI_CAP_EXPRESS;

        if (proxy->flags & VIRTIO_PCI_FLAG_PM_NO_SOFT_RESET) {
            int pos;

            pos = pci_add_capability(pci_dev, PCI_CAP_ID_PM, 0,
                                     PCI_PM_SIZEOF, errp);
            if (pos < 0) {
                return;
            }
            pci_dev->exp.pm_cap = pos;
            pci_set_word(pci_dev->config + pos + PCI_PM_PMC, 0x3);
            pci_set_word(pci_dev->config + pos + PCI_PM_CTRL,
                         PCI_PM_CTRL_NO_SOFT_RESET);
            pci_set_word(pci_dev->wmask + pos + PCI_PM_CTRL,
                         PCI_PM_CTRL_STATE_MASK);
        }
    }

    virtio_pci_bus_new(&proxy->bus, sizeof(proxy->bus), proxy);
    if (k->realize) {
        k->realize(proxy, errp);
    }
}

static void virtio_pci_exit(PCIDevice *pci_dev)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(pci_dev);
    bool pcie_port = pci_bus_is_express(pci_get_bus(pci_dev)) &&
                     !pci_bus_is_root(pci_get_bus(pci_dev));

    g_clear_pointer(&proxy->guest_reset_bh, qemu_bh_delete);
    msix_uninit_exclusive_bar(pci_dev);
    if (proxy->flags & VIRTIO_PCI_FLAG_AER && pcie_port &&
        pci_is_express(pci_dev)) {
        pcie_aer_exit(pci_dev);
    }
}

static void virtio_pci_reset(DeviceState *qdev)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(qdev);
    g_autoptr(VirtIOPCIQueueConfigPause) paused =
        virtio_pci_pause_queue_config(proxy);
    VirtioBusState *bus = VIRTIO_BUS(&proxy->bus);
    g_autoptr(VirtioNetworkLock) lock =
        virtio_lock_network(virtio_bus_get_device(bus));
    int i;
    g_autoptr(VirtIOPCIIRQRouteGuard) route_guard G_GNUC_UNUSED =
        virtio_pci_guard_irq_routes(proxy);

    virtio_pci_set_fast_notifiers(qdev, NULL, 0);
    virtio_pci_cancel_deferred_guest_reset(proxy);
    virtio_bus_reset(bus);
    msix_unuse_all_vectors(&proxy->pci_dev);

    for (i = 0; i < VIRTIO_QUEUE_MAX; i++) {
        proxy->vqs[i].enabled = 0;
        proxy->vqs[i].reset = 0;
        proxy->vqs[i].num = 0;
        proxy->vqs[i].desc[0] = proxy->vqs[i].desc[1] = 0;
        proxy->vqs[i].avail[0] = proxy->vqs[i].avail[1] = 0;
        proxy->vqs[i].used[0] = proxy->vqs[i].used[1] = 0;
    }
}

static bool virtio_pci_no_soft_reset(PCIDevice *dev)
{
    uint16_t pmcsr;

    if (!dev->exp.pm_cap) {
        return false;
    }

    pmcsr = pci_get_word(dev->config + dev->exp.pm_cap + PCI_PM_CTRL);
    return (pmcsr & PCI_PM_CTRL_NO_SOFT_RESET) &&
           (pmcsr & PCI_PM_CTRL_STATE_MASK) == 3;
}

static void virtio_pci_bus_reset_hold(Object *obj)
{
    PCIDevice *dev = PCI_DEVICE(obj);
    DeviceState *qdev = DEVICE(obj);
    VirtIOPCIProxy *proxy = VIRTIO_PCI(dev);

    if (virtio_pci_no_soft_reset(dev)) {
        return;
    }

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    pci_config_read_slot_begin(dev);
#endif
    virtio_pci_reset(qdev);

    if (pci_is_express(dev)) {
        pcie_cap_deverr_reset(dev);
        pcie_cap_lnkctl_reset(dev);
    }

    if (dev->exp.pm_cap &&
        (proxy->flags & (VIRTIO_PCI_FLAG_INIT_PM |
                         VIRTIO_PCI_FLAG_PM_NO_SOFT_RESET))) {
        pci_word_test_and_clear_mask(
            dev->config + dev->exp.pm_cap + PCI_PM_CTRL,
            PCI_PM_CTRL_STATE_MASK);
    }
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    pci_config_read_slot_end(dev);
#endif
}

static Property virtio_pci_properties[] = {
    DEFINE_PROP_BIT("virtio-pci-bus-master-bug-migration", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_BUS_MASTER_BUG_MIGRATION_BIT, false),
    DEFINE_PROP_BIT("migrate-extra", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_MIGRATE_EXTRA_BIT, true),
    DEFINE_PROP_BIT("modern-pio-notify", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_MODERN_PIO_NOTIFY_BIT, false),
    DEFINE_PROP_BIT("x-disable-pcie", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_DISABLE_PCIE_BIT, false),
    DEFINE_PROP_BIT("page-per-vq", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_PAGE_PER_VQ_BIT, false),
    DEFINE_PROP_BOOL("x-ignore-backend-features", VirtIOPCIProxy,
                     ignore_backend_features, false),
    DEFINE_PROP_BIT("ats", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_ATS_BIT, false),
    DEFINE_PROP_BIT("x-ats-page-aligned", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_ATS_PAGE_ALIGNED_BIT, true),
    DEFINE_PROP_BIT("x-pcie-deverr-init", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_INIT_DEVERR_BIT, true),
    DEFINE_PROP_BIT("x-pcie-lnkctl-init", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_INIT_LNKCTL_BIT, true),
    DEFINE_PROP_BIT("x-pcie-pm-init", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_INIT_PM_BIT, true),
    DEFINE_PROP_BIT("x-pcie-pm-no-soft-reset", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_PM_NO_SOFT_RESET_BIT, true),
    DEFINE_PROP_BIT("x-pcie-flr-init", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_INIT_FLR_BIT, true),
    DEFINE_PROP_BIT("aer", VirtIOPCIProxy, flags,
                    VIRTIO_PCI_FLAG_AER_BIT, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_pci_dc_realize(DeviceState *qdev, Error **errp)
{
    VirtioPCIClass *vpciklass = VIRTIO_PCI_GET_CLASS(qdev);
    VirtIOPCIProxy *proxy = VIRTIO_PCI(qdev);
    PCIDevice *pci_dev = &proxy->pci_dev;

    if (!(proxy->flags & VIRTIO_PCI_FLAG_DISABLE_PCIE) &&
        virtio_pci_modern(proxy)) {
        pci_dev->cap_present |= QEMU_PCI_CAP_EXPRESS;
    }

    vpciklass->parent_dc_realize(qdev, errp);
}

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static bool virtio_pci_try_topo_command_write(PCIDevice *pci_dev,
                                              uint16_t old_cmd,
                                              uint16_t new_cmd)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(pci_dev);
    VirtIODevice *vdev;

    QEMU_LOCK_GUARD(&proxy->queue_config_lock);
    vdev = virtio_bus_get_device(&proxy->bus);
    if (!vdev) {
        return true;
    }
    if (!(new_cmd & PCI_COMMAND_MASTER)) {
        if ((vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) ||
            proxy->bus.ioeventfd_started) {
            return false;
        }
        virtio_set_disabled(vdev, true);
    } else {
        virtio_set_disabled(vdev, false);
    }
    return true;
}
#endif

static void virtio_pci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    VirtioPCIClass *vpciklass = VIRTIO_PCI_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    device_class_set_props(dc, virtio_pci_properties);
    k->msi_route_change = virtio_pci_irq_route_change;
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    k->try_topo_command_write = virtio_pci_try_topo_command_write;
#endif
    k->realize = virtio_pci_realize;
    k->exit = virtio_pci_exit;
    k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    k->revision = VIRTIO_PCI_ABI_VERSION;
    k->class_id = PCI_CLASS_OTHERS;
    device_class_set_parent_realize(dc, virtio_pci_dc_realize,
                                    &vpciklass->parent_dc_realize);
    rc->phases.hold = virtio_pci_bus_reset_hold;
}

static void virtio_pci_instance_init(Object *obj)
{
    qemu_mutex_init(&VIRTIO_PCI(obj)->queue_config_lock);
}

static void virtio_pci_instance_finalize(Object *obj)
{
    qemu_mutex_destroy(&VIRTIO_PCI(obj)->queue_config_lock);
}

static const TypeInfo virtio_pci_info = {
    .name          = TYPE_VIRTIO_PCI,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VirtIOPCIProxy),
    .instance_init = virtio_pci_instance_init,
    .instance_finalize = virtio_pci_instance_finalize,
    .class_init    = virtio_pci_class_init,
    .class_size    = sizeof(VirtioPCIClass),
    .abstract      = true,
};

static Property virtio_pci_generic_properties[] = {
    DEFINE_PROP_ON_OFF_AUTO("disable-legacy", VirtIOPCIProxy, disable_legacy,
                            ON_OFF_AUTO_AUTO),
    DEFINE_PROP_BOOL("disable-modern", VirtIOPCIProxy, disable_modern, false),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_pci_base_class_init(ObjectClass *klass, void *data)
{
    const VirtioPCIDeviceTypeInfo *t = data;
    if (t->class_init) {
        t->class_init(klass, NULL);
    }
}

static void virtio_pci_generic_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, virtio_pci_generic_properties);
}

static void virtio_pci_transitional_instance_init(Object *obj)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(obj);

    proxy->disable_legacy = ON_OFF_AUTO_OFF;
    proxy->disable_modern = false;
}

static void virtio_pci_non_transitional_instance_init(Object *obj)
{
    VirtIOPCIProxy *proxy = VIRTIO_PCI(obj);

    proxy->disable_legacy = ON_OFF_AUTO_ON;
    proxy->disable_modern = false;
}

void virtio_pci_types_register(const VirtioPCIDeviceTypeInfo *t)
{
    char *base_name = NULL;
    TypeInfo base_type_info = {
        .name          = t->base_name,
        .parent        = t->parent ? t->parent : TYPE_VIRTIO_PCI,
        .instance_size = t->instance_size,
        .instance_init = t->instance_init,
        .instance_finalize = t->instance_finalize,
        .class_size    = t->class_size,
        .abstract      = true,
        .interfaces    = t->interfaces,
    };
    TypeInfo generic_type_info = {
        .name = t->generic_name,
        .parent = base_type_info.name,
        .class_init = virtio_pci_generic_class_init,
        .interfaces = (InterfaceInfo[]) {
            { INTERFACE_PCIE_DEVICE },
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { }
        },
    };

    if (!base_type_info.name) {
        /* No base type -> register a single generic device type */
        /* use intermediate %s-base-type to add generic device props */
        base_name = g_strdup_printf("%s-base-type", t->generic_name);
        base_type_info.name = base_name;
        base_type_info.class_init = virtio_pci_generic_class_init;

        generic_type_info.parent = base_name;
        generic_type_info.class_init = virtio_pci_base_class_init;
        generic_type_info.class_data = (void *)t;

        assert(!t->non_transitional_name);
        assert(!t->transitional_name);
    } else {
        base_type_info.class_init = virtio_pci_base_class_init;
        base_type_info.class_data = (void *)t;
    }

    type_register(&base_type_info);
    if (generic_type_info.name) {
        type_register(&generic_type_info);
    }

    if (t->non_transitional_name) {
        const TypeInfo non_transitional_type_info = {
            .name          = t->non_transitional_name,
            .parent        = base_type_info.name,
            .instance_init = virtio_pci_non_transitional_instance_init,
            .interfaces = (InterfaceInfo[]) {
                { INTERFACE_PCIE_DEVICE },
                { INTERFACE_CONVENTIONAL_PCI_DEVICE },
                { }
            },
        };
        type_register(&non_transitional_type_info);
    }

    if (t->transitional_name) {
        const TypeInfo transitional_type_info = {
            .name          = t->transitional_name,
            .parent        = base_type_info.name,
            .instance_init = virtio_pci_transitional_instance_init,
            .interfaces = (InterfaceInfo[]) {
                /*
                 * Transitional virtio devices work only as Conventional PCI
                 * devices because they require PIO ports.
                 */
                { INTERFACE_CONVENTIONAL_PCI_DEVICE },
                { }
            },
        };
        type_register(&transitional_type_info);
    }
    g_free(base_name);
}

unsigned virtio_pci_optimal_num_queues(unsigned fixed_queues)
{
    /*
     * 1:1 vq to vCPU mapping is ideal because the same vCPU that submitted
     * virtqueue buffers can handle their completion. When a different vCPU
     * handles completion it may need to IPI the vCPU that submitted the
     * request and this adds overhead.
     *
     * Virtqueues consume guest RAM and MSI-X vectors. This is wasteful in
     * guests with very many vCPUs and a device that is only used by a few
     * vCPUs. Unfortunately optimizing that case requires manual pinning inside
     * the guest, so those users might as well manually set the number of
     * queues. There is no upper limit that can be applied automatically and
     * doing so arbitrarily would result in a sudden performance drop once the
     * threshold number of vCPUs is exceeded.
     */
    unsigned num_queues = current_machine->smp.cpus;

    /*
     * The maximum number of MSI-X vectors is PCI_MSIX_FLAGS_QSIZE + 1, but the
     * config change interrupt and the fixed virtqueues must be taken into
     * account too.
     */
    num_queues = MIN(num_queues, PCI_MSIX_FLAGS_QSIZE - fixed_queues);

    /*
     * There is a limit to how many virtqueues a device can have.
     */
    return MIN(num_queues, VIRTIO_QUEUE_MAX - fixed_queues);
}

/* virtio-pci-bus */

static void virtio_pci_bus_new(VirtioBusState *bus, size_t bus_size,
                               VirtIOPCIProxy *dev)
{
    DeviceState *qdev = DEVICE(dev);
    char virtio_bus_name[] = "virtio-bus";

    qbus_init(bus, bus_size, TYPE_VIRTIO_PCI_BUS, qdev, virtio_bus_name);
}

static void virtio_pci_bus_class_init(ObjectClass *klass, void *data)
{
    BusClass *bus_class = BUS_CLASS(klass);
    VirtioBusClass *k = VIRTIO_BUS_CLASS(klass);
    bus_class->max_dev = 1;
    k->notify = virtio_pci_notify;
    k->save_config = virtio_pci_save_config;
    k->load_config = virtio_pci_load_config;
    k->save_queue = virtio_pci_save_queue;
    k->load_queue = virtio_pci_load_queue;
    k->save_extra_state = virtio_pci_save_extra_state;
    k->load_extra_state = virtio_pci_load_extra_state;
    k->has_extra_state = virtio_pci_has_extra_state;
    k->query_guest_notifiers = virtio_pci_query_guest_notifiers;
    k->set_guest_notifiers = virtio_pci_set_guest_notifiers;
    k->set_host_notifier_mr = virtio_pci_set_host_notifier_mr;
    k->vmstate_change = virtio_pci_vmstate_change;
    k->pre_plugged = virtio_pci_pre_plugged;
    k->device_plugged = virtio_pci_device_plugged;
    k->device_unplugged = virtio_pci_device_unplugged;
    k->query_nvectors = virtio_pci_query_nvectors;
    k->ioeventfd_enabled = virtio_pci_ioeventfd_enabled;
    k->ioeventfd_assign = virtio_pci_ioeventfd_assign;
    k->try_queue_irq = virtio_pci_try_queue_irq;
    k->queue_irq_ready = virtio_pci_queue_irq_ready;
    k->set_fast_notifiers = virtio_pci_set_fast_notifiers;
    k->get_dma_as = virtio_pci_get_dma_as;
    k->iommu_enabled = virtio_pci_iommu_enabled;
    k->queue_enabled = virtio_pci_queue_enabled;
}

static const TypeInfo virtio_pci_bus_info = {
    .name          = TYPE_VIRTIO_PCI_BUS,
    .parent        = TYPE_VIRTIO_BUS,
    .instance_size = sizeof(VirtioPCIBusState),
    .class_size    = sizeof(VirtioPCIBusClass),
    .class_init    = virtio_pci_bus_class_init,
};

static void virtio_pci_register_types(void)
{
    /* Base types: */
    type_register_static(&virtio_pci_bus_info);
    type_register_static(&virtio_pci_info);
}

type_init(virtio_pci_register_types)
