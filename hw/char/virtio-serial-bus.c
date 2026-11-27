/*
 * A bus for connecting virtio serial and console ports
 *
 * Copyright (C) 2009, 2010 Red Hat, Inc.
 *
 * Author(s):
 *  Amit Shah <amit.shah@redhat.com>
 *
 * Some earlier parts are:
 *  Copyright IBM, Corp. 2008
 * authored by
 *  Christian Ehrhardt <ehrhardt@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "qemu/rcu.h"
#include "chardev/char-socket.h"
#ifdef CONFIG_LINUX
#include <sys/ioctl.h>
#endif
#include "exec/address-spaces.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/replay.h"
#include "sysemu/tcg.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "migration/qemu-file-types.h"
#include "monitor/monitor.h"
#include "qemu/error-report.h"
#include "qemu/queue.h"
#include "hw/qdev-properties.h"
#include "trace.h"
#include "hw/virtio/virtio-serial.h"
#include "hw/virtio/virtio-access.h"
#include "hw/virtio/virtio-bus.h"

static struct VirtIOSerialDevices {
    QLIST_HEAD(, VirtIOSerial) devices;
} vserdevices;

static VirtIOSerialPort *find_port_by_id(VirtIOSerial *vser, uint32_t id)
{
    VirtIOSerialPort *port;

    if (id == VIRTIO_CONSOLE_BAD_ID) {
        return NULL;
    }

    QTAILQ_FOREACH(port, &vser->ports, next) {
        if (port->id == id)
            return port;
    }
    return NULL;
}

static VirtIOSerialPort *find_port_by_vq(VirtIOSerial *vser, VirtQueue *vq)
{
    VirtIOSerialPort *port;

    QTAILQ_FOREACH(port, &vser->ports, next) {
        if (port->ivq == vq || port->ovq == vq)
            return port;
    }
    return NULL;
}

static VirtIOSerialPort *find_port_by_name(char *name)
{
    VirtIOSerial *vser;

    QLIST_FOREACH(vser, &vserdevices.devices, next) {
        VirtIOSerialPort *port;

        QTAILQ_FOREACH(port, &vser->ports, next) {
            if (port->name && !strcmp(port->name, name)) {
                return port;
            }
        }
    }
    return NULL;
}

static VirtIOSerialPort *find_first_connected_console(VirtIOSerial *vser)
{
    VirtIOSerialPort *port;

    QTAILQ_FOREACH(port, &vser->ports, next) {
        VirtIOSerialPortClass const *vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
        if (vsc->is_console && port->host_connected) {
            return port;
        }
    }
    return NULL;
}

static bool use_multiport(VirtIOSerial *vser)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vser);
    return virtio_vdev_has_feature(vdev, VIRTIO_CONSOLE_F_MULTIPORT);
}

static void virtio_serial_notify_vq(VirtIOSerial *vser, VirtQueue *vq,
                                    VirtIOSerialPort *port)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vser);

    if (qemu_mutex_iothread_locked()) {
        virtio_notify(vdev, vq);
        return;
    }
    if (!virtio_queue_try_notify(vdev, vq)) {
        if (vq == vser->c_ivq) {
            vser->ctrl_irq_pending = true;
        } else if (port && vq == port->ivq) {
            port->rx_irq_pending = true;
        } else if (port && vq == port->ovq) {
            port->tx_irq_pending = true;
        }
        vdev->output_irqs_may_be_pending = true;
        qemu_bh_schedule(vser->tx_irq_bh);
    }
}

static size_t write_to_port(VirtIOSerialPort *port,
                            const uint8_t *buf, size_t size)
{
    VirtQueueElement *elem;
    VirtQueue *vq;
    size_t offset;

    vq = port->ivq;
    if (!virtio_queue_ready(vq)) {
        return 0;
    }

    offset = 0;
    WITH_RCU_READ_LOCK_GUARD() {
        while (offset < size) {
            size_t len;

            elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
            if (!elem) {
                break;
            }

            len = iov_from_buf(elem->in_sg, elem->in_num, 0,
                               buf + offset, size - offset);
            offset += len;

            virtqueue_push(vq, elem, len);
            g_free(elem);
        }
    }

    virtio_serial_notify_vq(port->vser, vq, port);
    return offset;
}

static void discard_vq_data(VirtQueue *vq, VirtIODevice *vdev)
{
    VirtIOSerial *vser = VIRTIO_SERIAL(vdev);
    VirtQueueElement *elem;

    if (!virtio_queue_ready(vq)) {
        return;
    }
    WITH_RCU_READ_LOCK_GUARD() {
        for (;;) {
            elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
            if (!elem) {
                break;
            }
            virtqueue_push(vq, elem, 0);
            g_free(elem);
        }
    }
    virtio_serial_notify_vq(vser, vq, find_port_by_vq(vser, vq));
}

static void discard_throttle_data(VirtIOSerialPort *port)
{
    port->direct_partial = false;
    if (port->elem) {
        virtqueue_detach_element(port->ovq, port->elem, 0);
        g_free(port->elem);
        port->elem = NULL;
    }
}

static void discard_port_output(VirtIOSerialPort *port)
{
    if (port->direct_partial) {
        /* The port closed before the ordinary output handler ran.
         * It would have returned this chain along with the other posted ones. */
        virtqueue_push(port->ovq, port->elem, 0);
        g_clear_pointer(&port->elem, g_free);
    }
    discard_throttle_data(port);
    discard_vq_data(port->ovq, VIRTIO_DEVICE(port->vser));
}

static void do_flush_queued_data(VirtIOSerialPort *port, VirtQueue *vq,
                                 VirtIODevice *vdev)
{
    VirtIOSerialPortClass *vsc;

    assert(port);
    assert(virtio_queue_ready(vq));

    vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
    port->direct_partial = false;

    while (!port->throttled) {
        unsigned int i;

        /* Pop an elem only if we haven't left off a previous one mid-way */
        if (!port->elem) {
            port->elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
            if (!port->elem) {
                break;
            }
            port->iov_idx = 0;
            port->iov_offset = 0;
        }

        for (i = port->iov_idx; i < port->elem->out_num; i++) {
            size_t buf_size;
            ssize_t ret;

            buf_size = port->elem->out_sg[i].iov_len - port->iov_offset;
            ret = vsc->have_data(port,
                                  port->elem->out_sg[i].iov_base
                                  + port->iov_offset,
                                  buf_size);
            if (!port->elem) { /* bail if we got disconnected */
                return;
            }
            if (port->throttled) {
                port->iov_idx = i;
                if (ret > 0) {
                    port->iov_offset += ret;
                }
                break;
            }
            port->iov_offset = 0;
        }
        if (port->throttled) {
            break;
        }
        virtqueue_push(vq, port->elem, 0);
        g_free(port->elem);
        port->elem = NULL;
    }
    virtio_notify(vdev, vq);
}

static void flush_queued_data(VirtIOSerialPort *port)
{
    assert(port);

    if (!virtio_queue_ready(port->ovq)) {
        return;
    }
    do_flush_queued_data(port, port->ovq, VIRTIO_DEVICE(port->vser));
}

static size_t send_control_msg(VirtIOSerial *vser, void *buf, size_t len)
{
    g_autoptr(VirtioNetworkLock) guard = virtio_lock_network(VIRTIO_DEVICE(vser));
    VirtQueueElement *elem;
    VirtQueue *vq;

    vq = vser->c_ivq;
    if (!virtio_queue_ready(vq)) {
        return 0;
    }

    WITH_RCU_READ_LOCK_GUARD() {
        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            return 0;
        }

        /* TODO: detect a buffer that's too short, set NEEDS_RESET */
        iov_from_buf(elem->in_sg, elem->in_num, 0, buf, len);

        virtqueue_push(vq, elem, len);
        g_free(elem);
    }
    virtio_serial_notify_vq(vser, vq, NULL);

    return len;
}

static size_t send_control_event(VirtIOSerial *vser, uint32_t port_id,
                                 uint16_t event, uint16_t value)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vser);
    struct virtio_console_control cpkt;

    virtio_stl_p(vdev, &cpkt.id, port_id);
    virtio_stw_p(vdev, &cpkt.event, event);
    virtio_stw_p(vdev, &cpkt.value, value);

    trace_virtio_serial_send_control_event(port_id, event, value);
    return send_control_msg(vser, &cpkt, sizeof(cpkt));
}

/* Functions for use inside qemu to open and read from/write to ports */
int virtio_serial_open(VirtIOSerialPort *port)
{
    g_autoptr(VirtioNetworkLock) guard =
        virtio_lock_network(VIRTIO_DEVICE(port->vser));

    /* Don't allow opening an already-open port */
    if (port->host_connected) {
        return 0;
    }
    /* Send port open notification to the guest */
    port->host_connected = true;
    send_control_event(port->vser, port->id, VIRTIO_CONSOLE_PORT_OPEN, 1);

    return 0;
}

int virtio_serial_close(VirtIOSerialPort *port)
{
    g_autoptr(VirtioNetworkLock) guard =
        virtio_lock_network(VIRTIO_DEVICE(port->vser));

    port->host_connected = false;
    /*
     * If there's any data the guest sent which the app didn't
     * consume, reset the throttling flag and discard the data.
     */
    port->throttled = false;
    discard_port_output(port);

    send_control_event(port->vser, port->id, VIRTIO_CONSOLE_PORT_OPEN, 0);

    return 0;
}

/* Individual ports/apps call this function to write to the guest. */
ssize_t virtio_serial_write(VirtIOSerialPort *port, const uint8_t *buf,
                            size_t size)
{
    if (!port || !port->host_connected || !port->guest_connected) {
        return 0;
    }
    return write_to_port(port, buf, size);
}

/*
 * Readiness of the guest to accept data on a port.
 * Returns max. data the guest can receive
 */
size_t virtio_serial_guest_ready(VirtIOSerialPort *port)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(port->vser);
    VirtQueue *vq = port->ivq;
    unsigned int bytes;

    if (!virtio_queue_ready(vq) ||
        !(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) ||
        virtio_queue_empty(vq)) {
        return 0;
    }
    if (use_multiport(port->vser) && !port->guest_connected) {
        return 0;
    }
    virtqueue_get_avail_bytes(vq, &bytes, NULL, 4096, 0);
    return bytes;
}

static void flush_queued_data_bh(void *opaque)
{
    VirtIOSerialPort *port = opaque;

    g_autoptr(VirtioNetworkLock) guard =
        virtio_lock_network(VIRTIO_DEVICE(port->vser));
    flush_queued_data(port);
}

void virtio_serial_throttle_port(VirtIOSerialPort *port, bool throttle)
{
    if (!port) {
        return;
    }

    g_autoptr(VirtioNetworkLock) guard =
        virtio_lock_network(VIRTIO_DEVICE(port->vser));
    trace_virtio_serial_throttle_port(port->id, throttle);
    port->throttled = throttle;
    if (throttle) {
        return;
    }
    qemu_bh_schedule(port->bh);
}

/* Guest wants to notify us of some event */
static void handle_control_message(VirtIOSerial *vser, void *buf, size_t len)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vser);
    struct VirtIOSerialPort *port;
    VirtIOSerialPortClass *vsc;
    struct virtio_console_control cpkt, *gcpkt;
    uint8_t *buffer;
    size_t buffer_len;

    gcpkt = buf;

    if (len < sizeof(cpkt)) {
        /* The guest sent an invalid control packet */
        return;
    }

    cpkt.event = virtio_lduw_p(vdev, &gcpkt->event);
    cpkt.value = virtio_lduw_p(vdev, &gcpkt->value);

    trace_virtio_serial_handle_control_message(cpkt.event, cpkt.value);

    if (cpkt.event == VIRTIO_CONSOLE_DEVICE_READY) {
        if (!cpkt.value) {
            error_report("virtio-serial-bus: Guest failure in adding device %s",
                         vser->bus.qbus.name);
            return;
        }
        /*
         * The device is up, we can now tell the device about all the
         * ports we have here.
         */
        QTAILQ_FOREACH(port, &vser->ports, next) {
            send_control_event(vser, port->id, VIRTIO_CONSOLE_PORT_ADD, 1);
        }
        return;
    }

    port = find_port_by_id(vser, virtio_ldl_p(vdev, &gcpkt->id));
    if (!port) {
        error_report("virtio-serial-bus: Unexpected port id %u for device %s",
                     virtio_ldl_p(vdev, &gcpkt->id), vser->bus.qbus.name);
        return;
    }

    trace_virtio_serial_handle_control_message_port(port->id);

    vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);

    switch(cpkt.event) {
    case VIRTIO_CONSOLE_PORT_READY:
        if (!cpkt.value) {
            error_report("virtio-serial-bus: Guest failure in adding port %u for device %s",
                         port->id, vser->bus.qbus.name);
            break;
        }
        /*
         * Now that we know the guest asked for the port name, we're
         * sure the guest has initialised whatever state is necessary
         * for this port. Now's a good time to let the guest know if
         * this port is a console port so that the guest can hook it
         * up to hvc.
         */
        if (vsc->is_console) {
            send_control_event(vser, port->id, VIRTIO_CONSOLE_CONSOLE_PORT, 1);
        }

        if (port->name) {
            virtio_stl_p(vdev, &cpkt.id, port->id);
            virtio_stw_p(vdev, &cpkt.event, VIRTIO_CONSOLE_PORT_NAME);
            virtio_stw_p(vdev, &cpkt.value, 1);

            buffer_len = sizeof(cpkt) + strlen(port->name) + 1;
            buffer = g_malloc(buffer_len);

            memcpy(buffer, &cpkt, sizeof(cpkt));
            memcpy(buffer + sizeof(cpkt), port->name, strlen(port->name));
            buffer[buffer_len - 1] = 0;

            send_control_msg(vser, buffer, buffer_len);
            g_free(buffer);
        }

        if (port->host_connected) {
            send_control_event(vser, port->id, VIRTIO_CONSOLE_PORT_OPEN, 1);
        }

        /*
         * When the guest has asked us for this information it means
         * the guest is all setup and has its virtqueues
         * initialised. If some app is interested in knowing about
         * this event, let it know.
         */
        if (vsc->guest_ready) {
            vsc->guest_ready(port);
        }
        break;

    case VIRTIO_CONSOLE_PORT_OPEN: {
        g_autoptr(VirtioNetworkLock) guard =
            virtio_lock_network(VIRTIO_DEVICE(vser));
        port->guest_connected = cpkt.value;
        if (vsc->set_guest_connected) {
            /* Send the guest opened notification if an app is interested */
            vsc->set_guest_connected(port, cpkt.value);
        }
        break;
    }
    }
}

static void control_in(VirtIODevice *vdev, VirtQueue *vq)
{
}

static void virtio_serial_flush_tx_irqs(VirtIODevice *vdev)
{
    VirtIOSerial *vser = VIRTIO_SERIAL(vdev);
    VirtIOSerialPort *port;
    g_autoptr(VirtioNetworkLock) guard = virtio_lock_network(vdev);

    if (vser->ctrl_irq_pending) {
        vser->ctrl_irq_pending = false;
        virtio_queue_deliver_irq(vser->c_ivq);
    }
    QTAILQ_FOREACH(port, &vser->ports, next) {
        if (port->rx_irq_pending) {
            port->rx_irq_pending = false;
            virtio_queue_deliver_irq(port->ivq);
        }
        if (port->tx_irq_pending) {
            port->tx_irq_pending = false;
            virtio_queue_deliver_irq(port->ovq);
        }
    }
    vdev->output_irqs_may_be_pending = false;
}

#if defined(CONFIG_LINUX) && defined(CONFIG_LATX_OPT_MMIO_LOCKLESS)
typedef struct SerialPollRX {
    DeviceState *transport;
    VirtIOSerial *vser;
    VirtIOSerialPort *port;
    Chardev *chr;
    QIOChannelSocket *socket;
    int fd;
    size_t limit;
} SerialPollRX;

void *virtio_serial_poll_rx_begin(void);
bool virtio_serial_poll_rx_read(void *opaque, int fd);
void virtio_serial_poll_rx_end(void *opaque);

/* Snapshot eligible streams after GLib has prepared their ordinary watches. */
void *virtio_serial_poll_rx_begin(void)
{
    GArray *ports = NULL;
    VirtIOSerial *vser;

    QLIST_FOREACH(vser, &vserdevices.devices, next) {
        VirtIODevice *vdev = VIRTIO_DEVICE(vser);
        VirtIOSerialPort *port;
        if (!vdev->try_output) {
            continue;
        }
        g_autoptr(VirtioNetworkLock) guard = virtio_lock_network(vdev);
        QTAILQ_FOREACH(port, &vser->ports, next) {
            Chardev *chr = port->direct_write_backend;
            if (!chr || !port->host_connected || !port->guest_connected ||
                (chr->gcontext && chr->gcontext != g_main_context_default()) ||
                qemu_mutex_trylock(&chr->chr_write_lock)) {
                continue;
            }
            SocketChardev *s = SOCKET_CHARDEV(chr);
            if (s->state == TCP_CHARDEV_STATE_CONNECTED && s->sioc &&
                s->ioc == QIO_CHANNEL(s->sioc) && s->max_size > 0 &&
                chr->gsource && !g_source_is_destroyed(chr->gsource)) {
                SerialPollRX entry = {
                    .transport = qdev_get_parent_bus(DEVICE(vser))->parent,
                    .vser = vser,
                    .port = port,
                    .chr = chr,
                    .socket = s->sioc,
                    .fd = s->sioc->fd,
                    .limit = MIN(s->max_size, CHR_READ_BUF_LEN),
                };
                /* The PCI transport embeds its VirtIOSerial instance. */
                object_ref(entry.transport);
                object_ref(entry.vser);
                object_ref(entry.port);
                object_ref(entry.chr);
                object_ref(entry.socket);
                if (!ports) {
                    ports = g_array_new(false, false, sizeof(entry));
                }
                g_array_append_val(ports, entry);
            }
            qemu_mutex_unlock(&chr->chr_write_lock);
        }
    }
    return ports;
}

void virtio_serial_poll_rx_end(void *opaque)
{
    GArray *ports = opaque;
    for (unsigned i = 0; i < ports->len; i++) {
        SerialPollRX *p = &g_array_index(ports, SerialPollRX, i);
        object_unref(p->socket);
        object_unref(p->chr);
        object_unref(p->port);
        object_unref(p->vser);
        object_unref(p->transport);
    }
    g_array_free(ports, true);
}

static bool virtio_serial_read_ready(SerialPollRX *p)
{
    VirtIOSerialPort *port = p->port;
    VirtIODevice *vdev = VIRTIO_DEVICE(p->vser);
    SocketChardev *s = SOCKET_CHARDEV(p->chr);
    VirtQueue *vq;
    g_autofree VirtQueueElement *elem = NULL;
    uint8_t buf[CHR_READ_BUF_LEN];
    int available;
    ssize_t n;

    QEMU_LOCK_GUARD(&p->vser->socket_io_mutex);
    if (qemu_rec_mutex_trylock(&p->vser->tx_mutex)) {
        return false;
    }
    g_autoptr(VirtioNetworkLock) guard = &p->vser->tx_mutex;
    if (!vdev->try_output || port->direct_write_backend != p->chr ||
        !port->host_connected || !port->guest_connected ||
        vdev->queue_mapping_changing || !vdev->vm_running ||
        vdev->start_on_kick || !(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) ||
        virtio_device_disabled(vdev) || vdev->dma_as != &address_space_memory ||
        !virtio_vdev_has_feature(vdev, VIRTIO_F_VERSION_1) ||
        virtio_vdev_has_feature(vdev, VIRTIO_F_RING_PACKED)) {
        return false;
    }
    RCU_READ_LOCK_GUARD();
    vq = port->ivq;
    if (!virtio_queue_ready(vq) || !virtio_queue_has_ram_rings(vq) ||
        (!virtio_queue_irq_ready(vdev, vq) && !virtio_split_next_irq_suppressed(vdev, vq)) ||
        qemu_mutex_trylock(&p->chr->chr_write_lock)) {
        return false;
    }
    if (s->sioc != p->socket || s->ioc != QIO_CHANNEL(p->socket) ||
        s->state != TCP_CHARDEV_STATE_CONNECTED || s->sioc->fd != p->fd ||
        ioctl(p->fd, FIONREAD, &available) < 0 || available <= 0) {
        goto out;
    }
    size_t size = MIN((size_t)available, p->limit);
    elem = virtqueue_pop(vq, sizeof(*elem));
    if (!elem) {
        vdev->network_queue_retry = false;
        goto out;
    }
    if (elem->out_num || !elem->in_num || iov_size(elem->in_sg, elem->in_num) < size) {
        virtqueue_unpop(vq, elem, 0);
        goto out;
    }
    n = qemu_chr_socket_recv(p->chr, (char *)buf, size);
    if (n <= 0) {
        virtqueue_unpop(vq, elem, 0);
        goto out;
    }
    /* Recheck guest capacity if ordinary input and hangup dispatch together. */
    s->max_size = -1;
    qemu_mutex_unlock(&p->chr->chr_write_lock);
    iov_from_buf(elem->in_sg, elem->in_num, 0, buf, n);
    trace_virtio_console_chr_read(port->id, n);
    virtqueue_push(vq, elem, n);
    if (!virtio_queue_try_notify(vdev, vq)) {
        port->rx_irq_pending = true;
        vdev->output_irqs_may_be_pending = true;
        qemu_bh_schedule(p->vser->tx_irq_bh);
    }
    return true;
out:
    qemu_mutex_unlock(&p->chr->chr_write_lock);
    return false;
}

bool virtio_serial_poll_rx_read(void *opaque, int fd)
{
    GArray *ports = opaque;
    for (unsigned i = 0; i < ports->len; i++) {
        SerialPollRX *p = &g_array_index(ports, SerialPollRX, i);
        if (fd == p->fd) {
            return virtio_serial_read_ready(p);
        }
    }
    return false;
}

static void virtio_serial_tx_irq_bh(void *opaque)
{
    virtio_serial_flush_tx_irqs(opaque);
}

static bool virtio_serial_try_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSerial *vser = VIRTIO_SERIAL(vdev);
    unsigned int index = virtio_get_queue_index(vq);
    g_autofree VirtQueueElement *elem = NULL;
    VirtIOSerialPort *port;
    VirtIOSerialPortClass *k;
    ssize_t sent;

    /*
     * Even queues are guest RX queues (control_in at index 2, or port->ivq).
     * control_in() is a no-op, and handle_input() is a no-op whenever the port
     * is absent, disconnected on either side, or has no guest_writable callback
     * (e.g. early-boot guest posting 128 RX buffers before QGA connects).
     * Consume those no-op kicks here without waking the main loop.
     */
    if (!(index & 1)) {
        if (vdev->start_on_kick ||
            !virtio_vdev_has_feature(vdev, VIRTIO_F_VERSION_1)) {
            return false;
        }
        if (index == 2) {
            return true;
        }
        if (qemu_rec_mutex_trylock(&vser->tx_mutex)) {
            return false;
        }
        g_autoptr(VirtioNetworkLock) guard = &vser->tx_mutex;
        if (vdev->queue_mapping_changing || !vdev->vm_running ||
            !(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) ||
            virtio_device_disabled(vdev)) {
            return false;
        }
        port = find_port_by_vq(vser, vq);
        if (!port || !port->guest_connected || !port->host_connected ||
            !VIRTIO_SERIAL_PORT_GET_CLASS(port)->guest_writable) {
            return true;
        }
        return false;
    }

    if (index < 5) {
        return false;
    }
    /* Direct readers and writers only wait for another socket operation.
     * Never wait for a config writer while holding its doorbell reference. */
    g_autoptr(QemuLockable) socket_guard = vser->direct_rx ?
        qemu_lockable_auto_lock(QEMU_MAKE_LOCKABLE(&vser->socket_io_mutex)) :
        NULL;
    if (qemu_rec_mutex_trylock(&vser->tx_mutex)) {
        return false;
    }
    g_autoptr(VirtioNetworkLock) guard = &vser->tx_mutex;
    RCU_READ_LOCK_GUARD();
    if (vdev->queue_mapping_changing || !vdev->vm_running || vdev->start_on_kick ||
        !(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) || virtio_device_disabled(vdev) ||
        !virtio_vdev_has_feature(vdev, VIRTIO_F_VERSION_1) ||
        virtio_vdev_has_feature(vdev, VIRTIO_F_RING_PACKED) ||
        vdev->dma_as != &address_space_memory || !virtio_queue_ready(vq) ||
        !virtio_queue_has_ram_rings(vq) ||
        (!virtio_queue_irq_ready(vdev, vq) && !virtio_split_next_irq_suppressed(vdev, vq))) {
        return false;
    }
    port = find_port_by_vq(vser, vq);
    if (!port || !port->direct_write_backend || !port->host_connected ||
        !port->guest_connected || port->elem || port->throttled) {
        return false;
    }
    k = VIRTIO_SERIAL_PORT_GET_CLASS(port);
    elem = virtqueue_pop(vq, sizeof(*elem));
    if (!elem) {
        vdev->network_queue_retry = false;
        return false;
    }
    if (!elem->out_num || elem->in_num) {
        virtqueue_unpop(vq, elem, 0);
        return false;
    }

    /* Limit the work performed by a guest MMIO write to one nonblocking
     * syscall. The ordinary handler resumes the same chain on a short write. */
    sent = k->try_write(port, elem->out_sg, elem->out_num);
    if (sent <= 0) {
        virtqueue_unpop(vq, elem, 0);
        return false;
    }
    for (unsigned int i = 0; i < elem->out_num; i++) {
        if (sent < elem->out_sg[i].iov_len) {
            port->iov_idx = i;
            port->iov_offset = sent;
            port->elem = g_steal_pointer(&elem);
            port->direct_partial = true;
            return false;
        }
        sent -= elem->out_sg[i].iov_len;
    }
    virtqueue_push(vq, elem, 0);
    if (!virtio_queue_try_notify(vdev, vq)) {
        port->tx_irq_pending = true;
        vdev->output_irqs_may_be_pending = true;
        qemu_bh_schedule(vser->tx_irq_bh);
    }
    return virtio_queue_empty(vq);
}
#endif

static int virtio_serial_start_ioeventfd(VirtIODevice *vdev)
{
    int ret = virtio_device_start_ioeventfd_impl(vdev);

    /*
     * Control messages consume guest buffers when the event occurs. A
     * buffer kick has no work of its own. Legacy devices can still use
     * their first kick to start the device.
     */
    if (ret == 0 && virtio_vdev_has_feature(vdev, VIRTIO_F_VERSION_1) &&
        !vdev->start_on_kick &&
        lats_nobql_enabled()) {
        virtio_queue_set_ioeventfd_fd_handler(VIRTIO_SERIAL(vdev)->c_ivq, false);
    }
    return ret;
}

static void control_out(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtQueueElement *elem;
    VirtIOSerial *vser;
    uint8_t *buf;
    size_t len;

    vser = VIRTIO_SERIAL(vdev);

    len = 0;
    buf = NULL;
    for (;;) {
        size_t cur_len;

        elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }

        cur_len = iov_size(elem->out_sg, elem->out_num);
        /*
         * Allocate a new buf only if we didn't have one previously or
         * if the size of the buf differs
         */
        if (cur_len > len) {
            g_free(buf);

            buf = g_malloc(cur_len);
            len = cur_len;
        }
        iov_to_buf(elem->out_sg, elem->out_num, 0, buf, cur_len);

        handle_control_message(vser, buf, cur_len);
        virtqueue_push(vq, elem, 0);
        g_free(elem);
    }
    g_free(buf);
    virtio_notify(vdev, vq);
}

/* Guest wrote something to some port. */
static void handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSerial *vser;
    VirtIOSerialPort *port;

    vser = VIRTIO_SERIAL(vdev);
    port = find_port_by_vq(vser, vq);

    if (!port || !port->host_connected) {
        discard_vq_data(vq, vdev);
        return;
    }

    if (!port->throttled) {
        do_flush_queued_data(port, vq, vdev);
        return;
    }
}

static void handle_input(VirtIODevice *vdev, VirtQueue *vq)
{
    /*
     * Users of virtio-serial would like to know when guest becomes
     * writable again -- i.e. if a vq had stuff queued up and the
     * guest wasn't reading at all, the host would not be able to
     * write to the vq anymore.  Once the guest reads off something,
     * we can start queueing things up again.  However, this call is
     * made for each buffer addition by the guest -- even though free
     * buffers existed prior to the current buffer addition.  This is
     * done so as not to maintain previous state, which will need
     * additional live-migration-related changes.
     */
    VirtIOSerial *vser;
    VirtIOSerialPort *port;
    VirtIOSerialPortClass *vsc;

    vser = VIRTIO_SERIAL(vdev);
    port = find_port_by_vq(vser, vq);

    if (!port) {
        return;
    }
    vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);

    /*
     * If guest_connected is false, this call is being made by the
     * early-boot queueing up of descriptors, which is just noise for
     * the host apps -- don't disturb them in that case.
     */
    if (port->guest_connected && port->host_connected && vsc->guest_writable) {
        vsc->guest_writable(port);
    }
}

static uint64_t get_features(VirtIODevice *vdev, uint64_t features,
                             Error **errp)
{
    VirtIOSerial *vser;

    vser = VIRTIO_SERIAL(vdev);

    features |= vser->host_features;
    if (vser->bus.max_nr_ports > 1) {
        virtio_add_feature(&features, VIRTIO_CONSOLE_F_MULTIPORT);
    }
    return features;
}

/* Guest requested config info */
static void get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOSerial *vser = VIRTIO_SERIAL(vdev);
    struct virtio_console_config *config =
        (struct virtio_console_config *)config_data;

    config->cols = 0;
    config->rows = 0;
    config->max_nr_ports = virtio_tswap32(vdev,
                                          vser->serial.max_virtserial_ports);
}

/* Guest sent new config info */
static void set_config(VirtIODevice *vdev, const uint8_t *config_data)
{
    VirtIOSerial *vser = VIRTIO_SERIAL(vdev);
    struct virtio_console_config *config =
        (struct virtio_console_config *)config_data;
    VirtIOSerialPort *port = find_first_connected_console(vser);
    VirtIOSerialPortClass *vsc;
    uint8_t emerg_wr_lo;

    if (!virtio_has_feature(vser->host_features,
        VIRTIO_CONSOLE_F_EMERG_WRITE) || !config->emerg_wr) {
        return;
    }

    emerg_wr_lo = le32_to_cpu(config->emerg_wr);
    /* Make sure we don't misdetect an emergency write when the guest
     * does a short config write after an emergency write. */
    config->emerg_wr = 0;
    if (!port) {
        return;
    }
    vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
    (void)vsc->have_data(port, &emerg_wr_lo, 1);
}

static void guest_reset(VirtIOSerial *vser)
{
    VirtIOSerialPort *port;
    VirtIOSerialPortClass *vsc;

    g_autoptr(VirtioNetworkLock) guard = virtio_lock_network(VIRTIO_DEVICE(vser));
    vser->ctrl_irq_pending = false;
    QTAILQ_FOREACH(port, &vser->ports, next) {
        vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);

        port->rx_irq_pending = false;
        port->tx_irq_pending = false;
        discard_throttle_data(port);

        if (port->guest_connected) {
            port->guest_connected = false;
            if (vsc->set_guest_connected) {
                vsc->set_guest_connected(port, false);
            }
        }
    }
}

static void set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOSerial *vser;
    VirtIOSerialPort *port;

    vser = VIRTIO_SERIAL(vdev);
    if (!vdev->vm_running && vdev->flush_output_irqs) {
        vdev->flush_output_irqs(vdev);
    }
    port = find_port_by_id(vser, 0);

    if (port && !use_multiport(port->vser)
        && (status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        /*
         * Non-multiport guests won't be able to tell us guest
         * open/close status.  Such guests can only have a port at id
         * 0, so set guest_connected for such ports as soon as guest
         * is up.
         */
        port->guest_connected = true;
    }
    if (!(status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        guest_reset(vser);
    }

    QTAILQ_FOREACH(port, &vser->ports, next) {
        VirtIOSerialPortClass *vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
        if (vsc->enable_backend) {
            vsc->enable_backend(port, vdev->vm_running);
        }
    }
}

static void vser_restore_rx_notifications(VirtIOSerialPort *port)
{
    VirtIOSerialPortClass *vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);

    if (vsc->restore_rx_notifications) {
        vsc->restore_rx_notifications(port);
    }
}

static void vser_queue_reset(VirtIODevice *vdev, uint32_t index)
{
    VirtQueue *vq = virtio_get_queue(vdev, index);
    VirtIOSerialPort *port = find_port_by_vq(VIRTIO_SERIAL(vdev), vq);

    if (port && port->ovq == vq) {
        port->tx_irq_pending = false;
        /* Detach a partially written buffer while its old ring is valid. */
        discard_throttle_data(port);
    } else if (port && port->ivq == vq) {
        port->rx_irq_pending = false;
        vser_restore_rx_notifications(port);
    }
}

static void vser_memory_begin(MemoryListener *listener)
{
    VirtIODevice *vdev = container_of(listener, VirtIODevice, listener);
    VirtIOSerialPort *port;

    g_autoptr(VirtioNetworkLock) guard = virtio_lock_network(vdev);
    if (vdev->try_output) {
        vdev->queue_mapping_changing = true;
        vdev->flush_output_irqs(vdev);
    }
    /* The old ring caches are still valid at the start of a memory update. */
    QTAILQ_FOREACH(port, &VIRTIO_SERIAL(vdev)->ports, next) {
        vser_restore_rx_notifications(port);
    }
}

static void vser_reset(VirtIODevice *vdev)
{
    VirtIOSerial *vser;

    vser = VIRTIO_SERIAL(vdev);
    guest_reset(vser);
}

static void virtio_serial_save_device(VirtIODevice *vdev, QEMUFile *f)
{
    VirtIOSerial *s = VIRTIO_SERIAL(vdev);
    VirtIOSerialPort *port;
    uint32_t nr_active_ports;
    unsigned int i, max_nr_ports;
    struct virtio_console_config config;

    /* The config space (ignored on the far end in current versions) */
    get_config(vdev, (uint8_t *)&config);
    qemu_put_be16(f, config.cols);
    qemu_put_be16(f, config.rows);
    qemu_put_be32(f, config.max_nr_ports);

    /* The ports map */
    max_nr_ports = s->serial.max_virtserial_ports;
    for (i = 0; i < DIV_ROUND_UP(max_nr_ports, 32); i++) {
        qemu_put_be32s(f, &s->ports_map[i]);
    }

    /* Ports */

    nr_active_ports = 0;
    QTAILQ_FOREACH(port, &s->ports, next) {
        nr_active_ports++;
    }

    qemu_put_be32s(f, &nr_active_ports);

    /*
     * Items in struct VirtIOSerialPort.
     */
    QTAILQ_FOREACH(port, &s->ports, next) {
        uint32_t elem_popped;

        qemu_put_be32s(f, &port->id);
        qemu_put_byte(f, port->guest_connected);
        qemu_put_byte(f, port->host_connected);

        elem_popped = 0;
        if (port->elem) {
            elem_popped = 1;
        }
        qemu_put_be32s(f, &elem_popped);
        if (elem_popped) {
            qemu_put_be32s(f, &port->iov_idx);
            qemu_put_be64s(f, &port->iov_offset);
            qemu_put_virtqueue_element(vdev, f, port->elem);
        }
    }
}

static void virtio_serial_post_load_timer_cb(void *opaque)
{
    uint32_t i;
    VirtIOSerial *s = VIRTIO_SERIAL(opaque);
    VirtIOSerialPort *port;
    uint8_t host_connected;
    VirtIOSerialPortClass *vsc;

    if (!s->post_load) {
        return;
    }
    for (i = 0 ; i < s->post_load->nr_active_ports; ++i) {
        port = s->post_load->connected[i].port;
        host_connected = s->post_load->connected[i].host_connected;
        if (host_connected != port->host_connected) {
            /*
             * We have to let the guest know of the host connection
             * status change
             */
            send_control_event(s, port->id, VIRTIO_CONSOLE_PORT_OPEN,
                               port->host_connected);
        }
        vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
        if (vsc->set_guest_connected) {
            vsc->set_guest_connected(port, port->guest_connected);
        }
    }
    g_free(s->post_load->connected);
    timer_free(s->post_load->timer);
    g_free(s->post_load);
    s->post_load = NULL;
}

static int fetch_active_ports_list(QEMUFile *f,
                                   VirtIOSerial *s, uint32_t nr_active_ports)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    uint32_t i;

    s->post_load = g_malloc0(sizeof(*s->post_load));
    s->post_load->nr_active_ports = nr_active_ports;
    s->post_load->connected =
        g_malloc0(sizeof(*s->post_load->connected) * nr_active_ports);

    s->post_load->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                            virtio_serial_post_load_timer_cb,
                                            s);

    /* Items in struct VirtIOSerialPort */
    for (i = 0; i < nr_active_ports; i++) {
        VirtIOSerialPort *port;
        uint32_t elem_popped;
        uint32_t id;

        id = qemu_get_be32(f);
        port = find_port_by_id(s, id);
        if (!port) {
            return -EINVAL;
        }

        port->guest_connected = qemu_get_byte(f);
        s->post_load->connected[i].port = port;
        s->post_load->connected[i].host_connected = qemu_get_byte(f);

        qemu_get_be32s(f, &elem_popped);
        if (elem_popped) {
            qemu_get_be32s(f, &port->iov_idx);
            qemu_get_be64s(f, &port->iov_offset);

            port->elem =
                qemu_get_virtqueue_element(vdev, f, sizeof(VirtQueueElement));

            /*
             *  Port was throttled on source machine.  Let's
             *  unthrottle it here so data starts flowing again.
             */
            virtio_serial_throttle_port(port, false);
        }
    }
    timer_mod(s->post_load->timer, 1);
    return 0;
}

static int virtio_serial_load_device(VirtIODevice *vdev, QEMUFile *f,
                                     int version_id)
{
    VirtIOSerial *s = VIRTIO_SERIAL(vdev);
    uint32_t max_nr_ports, nr_active_ports, ports_map;
    unsigned int i;
    int ret;
    uint32_t tmp;

    /* Unused */
    qemu_get_be16s(f, (uint16_t *) &tmp);
    qemu_get_be16s(f, (uint16_t *) &tmp);
    qemu_get_be32s(f, &tmp);

    max_nr_ports = s->serial.max_virtserial_ports;
    for (i = 0; i < DIV_ROUND_UP(max_nr_ports, 32); i++) {
        qemu_get_be32s(f, &ports_map);

        if (ports_map != s->ports_map[i]) {
            /*
             * Ports active on source and destination don't
             * match. Fail migration.
             */
            return -EINVAL;
        }
    }

    qemu_get_be32s(f, &nr_active_ports);

    if (nr_active_ports) {
        ret = fetch_active_ports_list(f, s, nr_active_ports);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static void virtser_bus_dev_print(Monitor *mon, DeviceState *qdev, int indent);

static Property virtser_props[] = {
    DEFINE_PROP_UINT32("nr", VirtIOSerialPort, id, VIRTIO_CONSOLE_BAD_ID),
    DEFINE_PROP_STRING("name", VirtIOSerialPort, name),
    DEFINE_PROP_END_OF_LIST()
};

static void virtser_bus_class_init(ObjectClass *klass, void *data)
{
    BusClass *k = BUS_CLASS(klass);
    k->print_dev = virtser_bus_dev_print;
}

static const TypeInfo virtser_bus_info = {
    .name = TYPE_VIRTIO_SERIAL_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(VirtIOSerialBus),
    .class_init = virtser_bus_class_init,
};

static void virtser_bus_dev_print(Monitor *mon, DeviceState *qdev, int indent)
{
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(qdev);

    monitor_printf(mon, "%*sport %d, guest %s, host %s, throttle %s\n",
                   indent, "", port->id,
                   port->guest_connected ? "on" : "off",
                   port->host_connected ? "on" : "off",
                   port->throttled ? "on" : "off");
}

/* This function is only used if a port id is not provided by the user */
static uint32_t find_free_port_id(VirtIOSerial *vser)
{
    unsigned int i, max_nr_ports;

    max_nr_ports = vser->serial.max_virtserial_ports;
    for (i = 0; i < DIV_ROUND_UP(max_nr_ports, 32); i++) {
        uint32_t map, zeroes;

        map = vser->ports_map[i];
        zeroes = ctz32(~map);
        if (zeroes != 32) {
            return zeroes + i * 32;
        }
    }
    return VIRTIO_CONSOLE_BAD_ID;
}

static void mark_port_added(VirtIOSerial *vser, uint32_t port_id)
{
    unsigned int i;

    i = port_id / 32;
    vser->ports_map[i] |= 1U << (port_id % 32);
}

static void add_port(VirtIOSerial *vser, uint32_t port_id)
{
    mark_port_added(vser, port_id);
    send_control_event(vser, port_id, VIRTIO_CONSOLE_PORT_ADD, 1);
}

static void remove_port(VirtIOSerial *vser, uint32_t port_id)
{
    VirtIOSerialPort *port;

    /*
     * Don't mark port 0 removed -- we explicitly reserve it for
     * backward compat with older guests, ensure a virtconsole device
     * unplug retains the reservation.
     */
    if (port_id) {
        unsigned int i;

        i = port_id / 32;
        vser->ports_map[i] &= ~(1U << (port_id % 32));
    }

    port = find_port_by_id(vser, port_id);
    /*
     * This function is only called from qdev's unplug callback; if we
     * get a NULL port here, we're in trouble.
     */
    assert(port);

    /* Flush out any unconsumed buffers first */
    discard_port_output(port);

    send_control_event(vser, port->id, VIRTIO_CONSOLE_PORT_REMOVE, 1);
}

static void virtser_port_device_realize(DeviceState *dev, Error **errp)
{
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(dev);
    VirtIOSerialPortClass *vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
    VirtIOSerialBus *bus = VIRTIO_SERIAL_BUS(qdev_get_parent_bus(dev));
    int max_nr_ports;
    bool plugging_port0;
    Error *err = NULL;

    port->vser = bus->vser;

    assert(vsc->have_data);

    /*
     * Is the first console port we're seeing? If so, put it up at
     * location 0. This is done for backward compatibility (old
     * kernel, new qemu).
     */
    plugging_port0 = vsc->is_console && !find_port_by_id(port->vser, 0);

    if (find_port_by_id(port->vser, port->id)) {
        error_setg(errp, "virtio-serial-bus: A port already exists at id %u",
                   port->id);
        return;
    }

    if (port->name != NULL && find_port_by_name(port->name)) {
        error_setg(errp, "virtio-serial-bus: A port already exists by name %s",
                   port->name);
        return;
    }

    if (port->id == VIRTIO_CONSOLE_BAD_ID) {
        if (plugging_port0) {
            port->id = 0;
        } else {
            port->id = find_free_port_id(port->vser);
            if (port->id == VIRTIO_CONSOLE_BAD_ID) {
                error_setg(errp, "virtio-serial-bus: Maximum port limit for "
                                 "this device reached");
                return;
            }
        }
    }

    max_nr_ports = port->vser->serial.max_virtserial_ports;
    if (port->id >= max_nr_ports) {
        error_setg(errp, "virtio-serial-bus: Out-of-range port id specified, "
                         "max. allowed: %u", max_nr_ports - 1);
        return;
    }

    vsc->realize(dev, &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }

    port->bh = qemu_bh_new_guarded(flush_queued_data_bh, port,
                                   &dev->mem_reentrancy_guard);
    port->elem = NULL;
}

static void virtser_port_device_plug(HotplugHandler *hotplug_dev,
                                     DeviceState *dev, Error **errp)
{
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(dev);

    g_autoptr(VirtioNetworkLock) guard =
        virtio_lock_network(VIRTIO_DEVICE(port->vser));
    QTAILQ_INSERT_TAIL(&port->vser->ports, port, next);
    port->ivq = port->vser->ivqs[port->id];
    port->ovq = port->vser->ovqs[port->id];

    add_port(port->vser, port->id);

    /* Send an update to the guest about this new port added */
    virtio_notify_config(VIRTIO_DEVICE(hotplug_dev));
}

static void virtser_port_device_unrealize(DeviceState *dev)
{
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(dev);
    VirtIOSerialPortClass *vsc = VIRTIO_SERIAL_PORT_GET_CLASS(dev);
    VirtIOSerial *vser = port->vser;

    g_autoptr(VirtioNetworkLock) guard = virtio_lock_network(VIRTIO_DEVICE(vser));
    if (port->tx_irq_pending || port->rx_irq_pending) {
        virtio_serial_flush_tx_irqs(VIRTIO_DEVICE(vser));
    }
    qemu_bh_delete(port->bh);
    remove_port(port->vser, port->id);

    QTAILQ_REMOVE(&vser->ports, port, next);

    if (vsc->unrealize) {
        vsc->unrealize(dev);
    }
}

static void virtio_serial_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOSerial *vser = VIRTIO_SERIAL(dev);
    uint32_t i, max_supported_ports;
    size_t config_size = sizeof(struct virtio_console_config);

    if (!vser->serial.max_virtserial_ports) {
        error_setg(errp, "Maximum number of serial ports not specified");
        return;
    }

    /* Each port takes 2 queues, and one pair is for the control queue */
    max_supported_ports = VIRTIO_QUEUE_MAX / 2 - 1;

    if (vser->serial.max_virtserial_ports > max_supported_ports) {
        error_setg(errp, "maximum ports supported: %u", max_supported_ports);
        return;
    }

    if (!virtio_has_feature(vser->host_features,
                            VIRTIO_CONSOLE_F_EMERG_WRITE)) {
        config_size = offsetof(struct virtio_console_config, emerg_wr);
    }
    virtio_init(vdev, VIRTIO_ID_CONSOLE, config_size);

    /* Spawn a new virtio-serial bus on which the ports will ride as devices */
    qbus_init(&vser->bus, sizeof(vser->bus), TYPE_VIRTIO_SERIAL_BUS,
              dev, vdev->bus_name);
    qbus_set_hotplug_handler(BUS(&vser->bus), OBJECT(vser));
    vser->bus.vser = vser;
    QTAILQ_INIT(&vser->ports);
#if defined(CONFIG_LINUX) && defined(CONFIG_LATX_OPT_MMIO_LOCKLESS)
    if (lats_nobql_enabled() &&
        VIRTIO_BUS_GET_CLASS(qdev_get_parent_bus(dev))->set_fast_notifiers &&
        tcg_enabled() && !icount_enabled() && replay_mode == REPLAY_MODE_NONE) {
        vser->direct_rx =
            lats_nobql_enabled();
        vdev->network_mutex = &vser->tx_mutex;
        vdev->try_output = virtio_serial_try_output;
        vdev->flush_output_irqs = virtio_serial_flush_tx_irqs;
        vser->tx_irq_bh = qemu_bh_new_guarded(virtio_serial_tx_irq_bh, vdev,
                                              &dev->mem_reentrancy_guard);
    }
#endif
    if (vdev->try_output ||
        lats_nobql_enabled()) {
        vdev->listener.begin = vser_memory_begin;
    }

    vser->bus.max_nr_ports = vser->serial.max_virtserial_ports;
    vser->ivqs = g_new(VirtQueue *, vser->serial.max_virtserial_ports);
    vser->ovqs = g_new(VirtQueue *, vser->serial.max_virtserial_ports);

    /* Add a queue for host to guest transfers for port 0 (backward compat) */
    vser->ivqs[0] = virtio_add_queue(vdev, 128, handle_input);
    /* Add a queue for guest to host transfers for port 0 (backward compat) */
    vser->ovqs[0] = virtio_add_queue(vdev, 128, handle_output);

    /* TODO: host to guest notifications can get dropped
     * if the queue fills up. Implement queueing in host,
     * this might also make it possible to reduce the control
     * queue size: as guest preposts buffers there,
     * this will save 4Kbyte of guest memory per entry. */

    /* control queue: host to guest */
    vser->c_ivq = virtio_add_queue(vdev, 32, control_in);
    /* control queue: guest to host */
    vser->c_ovq = virtio_add_queue(vdev, 32, control_out);

    for (i = 1; i < vser->bus.max_nr_ports; i++) {
        /* Add a per-port queue for host to guest transfers */
        vser->ivqs[i] = virtio_add_queue(vdev, 128, handle_input);
        /* Add a per-per queue for guest to host transfers */
        vser->ovqs[i] = virtio_add_queue(vdev, 128, handle_output);
    }

    vser->ports_map = g_malloc0((DIV_ROUND_UP(vser->serial.max_virtserial_ports, 32))
        * sizeof(vser->ports_map[0]));
    /*
     * Reserve location 0 for a console port for backward compat
     * (old kernel, new qemu)
     */
    mark_port_added(vser, 0);

    vser->post_load = NULL;

    QLIST_INSERT_HEAD(&vserdevices.devices, vser, next);
}

static void virtio_serial_port_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *k = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_INPUT, k->categories);
    k->bus_type = TYPE_VIRTIO_SERIAL_BUS;
    k->realize = virtser_port_device_realize;
    k->unrealize = virtser_port_device_unrealize;
    device_class_set_props(k, virtser_props);
}

static const TypeInfo virtio_serial_port_type_info = {
    .name = TYPE_VIRTIO_SERIAL_PORT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(VirtIOSerialPort),
    .abstract = true,
    .class_size = sizeof(VirtIOSerialPortClass),
    .class_init = virtio_serial_port_class_init,
};

static void virtio_serial_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOSerial *vser = VIRTIO_SERIAL(dev);
    int i;

    g_autoptr(VirtioNetworkLock) guard = virtio_lock_network(vdev);
    QLIST_REMOVE(vser, next);

    virtio_delete_queue(vser->c_ivq);
    virtio_delete_queue(vser->c_ovq);
    for (i = 0; i < vser->bus.max_nr_ports; i++) {
        virtio_delete_queue(vser->ivqs[i]);
        virtio_delete_queue(vser->ovqs[i]);
    }

    g_free(vser->ivqs);
    g_free(vser->ovqs);
    g_free(vser->ports_map);
    if (vser->post_load) {
        g_free(vser->post_load->connected);
        timer_free(vser->post_load->timer);
        g_free(vser->post_load);
    }

    qbus_set_hotplug_handler(BUS(&vser->bus), NULL);

    virtio_cleanup(vdev);
    if (vdev->try_output) {
        qemu_bh_delete(vser->tx_irq_bh);
        vdev->flush_output_irqs = NULL;
        vdev->try_output = NULL;
        vdev->network_mutex = NULL;
    }
}

/* Note: 'console' is used for backwards compatibility */
static const VMStateDescription vmstate_virtio_console = {
    .name = "virtio-console",
    .minimum_version_id = 3,
    .version_id = 3,
    .fields = (VMStateField[]) {
        VMSTATE_VIRTIO_DEVICE,
        VMSTATE_END_OF_LIST()
    },
};

static Property virtio_serial_properties[] = {
    DEFINE_PROP_UINT32("max_ports", VirtIOSerial, serial.max_virtserial_ports,
                                                  31),
    DEFINE_PROP_BIT64("emergency-write", VirtIOSerial, host_features,
                      VIRTIO_CONSOLE_F_EMERG_WRITE, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_serial_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(klass);

    QLIST_INIT(&vserdevices.devices);

    device_class_set_props(dc, virtio_serial_properties);
    dc->vmsd = &vmstate_virtio_console;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    vdc->realize = virtio_serial_device_realize;
    vdc->unrealize = virtio_serial_device_unrealize;
    vdc->get_features = get_features;
    vdc->get_config = get_config;
    vdc->start_ioeventfd = virtio_serial_start_ioeventfd;
    vdc->set_config = set_config;
    vdc->set_status = set_status;
    vdc->reset = vser_reset;
    vdc->queue_reset = vser_queue_reset;
    vdc->save = virtio_serial_save_device;
    vdc->load = virtio_serial_load_device;
    hc->plug = virtser_port_device_plug;
    hc->unplug = qdev_simple_device_unplug_cb;
}

static void virtio_serial_instance_init(Object *obj)
{
    qemu_rec_mutex_init(&VIRTIO_SERIAL(obj)->tx_mutex);
    qemu_mutex_init(&VIRTIO_SERIAL(obj)->socket_io_mutex);
}

static void virtio_serial_instance_finalize(Object *obj)
{
    qemu_mutex_destroy(&VIRTIO_SERIAL(obj)->socket_io_mutex);
    qemu_rec_mutex_destroy(&VIRTIO_SERIAL(obj)->tx_mutex);
}

static const TypeInfo virtio_device_info = {
    .name = TYPE_VIRTIO_SERIAL,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOSerial),
    .instance_init = virtio_serial_instance_init,
    .instance_finalize = virtio_serial_instance_finalize,
    .class_init = virtio_serial_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_HOTPLUG_HANDLER },
        { }
    }
};

static void virtio_serial_register_types(void)
{
    type_register_static(&virtser_bus_info);
    type_register_static(&virtio_serial_port_type_info);
    type_register_static(&virtio_device_info);
}

type_init(virtio_serial_register_types)
