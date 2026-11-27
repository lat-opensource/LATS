/*
 * Virtio Console and Generic Serial Port Devices
 *
 * Copyright Red Hat, Inc. 2009, 2010
 *
 * Authors:
 *  Amit Shah <amit.shah@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "chardev/char-socket.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#include "qemu/lockable.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/iothread.h"
#include "sysemu/replay.h"
#include "sysemu/runstate.h"
#include "sysemu/tcg.h"
#include "trace.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/virtio/virtio-serial.h"
#include "qapi/error.h"
#include "qapi/qapi-events-char.h"
#include "qom/object.h"

#define TYPE_VIRTIO_CONSOLE_SERIAL_PORT "virtserialport"
typedef struct VirtConsole VirtConsole;
DECLARE_INSTANCE_CHECKER(VirtConsole, VIRTIO_CONSOLE,
                         TYPE_VIRTIO_CONSOLE_SERIAL_PORT)

struct VirtConsole {
    VirtIOSerialPort parent_obj;

    CharBackend chr;
    guint watch;
    bool local_socket_wake;
    bool suppress_rx_kicks;
    bool rx_kicks_suppressed;
    bool qga_iothread_enabled;
    IOThread *qga_iothread;
    GMainContext *qga_gcontext;
    Notifier rx_suspend;
};

static bool virtconsole_can_use_qga_iothread(VirtConsole *vcon)
{
    Chardev *chr = vcon->chr.chr;
    SocketChardev *s;

    if (!vcon->qga_iothread_enabled || !chr || qemu_chr_replay(chr) ||
        !object_dynamic_cast(OBJECT(chr), TYPE_CHARDEV_SOCKET)) {
        return false;
    }
    s = SOCKET_CHARDEV(chr);
    return s->is_listen && s->addr &&
           s->addr->type == SOCKET_ADDRESS_TYPE_UNIX &&
           !s->tls_creds && !s->is_websock &&
           !s->is_telnet && !s->is_tn3270;
}

static GMainContext *virtconsole_get_chr_context(VirtConsole *vcon)
{
    if (!virtconsole_can_use_qga_iothread(vcon)) {
        return NULL;
    }
    if (!vcon->qga_iothread) {
        Error *local_err = NULL;
        g_autofree char *id = g_strdup_printf("qga-io-%u",
                                              VIRTIO_SERIAL_PORT(vcon)->id);

        vcon->qga_iothread = iothread_create(id, &local_err);
        if (!vcon->qga_iothread) {
            error_free(local_err);
            return NULL;
        }
        vcon->qga_gcontext = iothread_get_g_main_context(vcon->qga_iothread);
    }
    return vcon->qga_gcontext;
}

static void virtconsole_remove_watch(VirtConsole *vcon)
{
    Chardev *chr;
    GMainContext *ctx;

    if (!vcon->watch) {
        return;
    }
    chr = vcon->chr.chr;
    ctx = chr ? chr->gcontext : NULL;
    if (ctx) {
        GSource *src = g_main_context_find_source_by_id(ctx, vcon->watch);
        if (src) {
            g_source_destroy(src);
        }
    } else {
        g_source_remove(vcon->watch);
    }
    vcon->watch = 0;
}

typedef struct VirtConsoleSetHandlersReq {
    VirtConsole *vcon;
    IOCanReadHandler *fd_can_read;
    IOReadHandler *fd_read;
    IOEventHandler *fd_event;
    BackendChangeHandler *be_change;
    void *opaque;
    GMainContext *context;
    bool set_open;
    QemuEvent done;
} VirtConsoleSetHandlersReq;

static void virtconsole_set_handlers_bh(void *opaque)
{
    VirtConsoleSetHandlersReq *req = opaque;

    qemu_chr_fe_set_handlers(&req->vcon->chr, req->fd_can_read, req->fd_read,
                             req->fd_event, req->be_change, req->opaque,
                             req->context, req->set_open);
    qemu_event_set(&req->done);
}

static void virtconsole_set_handlers_sync(VirtConsole *vcon,
                                          IOCanReadHandler *fd_can_read,
                                          IOReadHandler *fd_read,
                                          IOEventHandler *fd_event,
                                          BackendChangeHandler *be_change,
                                          void *opaque,
                                          GMainContext *context,
                                          bool set_open)
{
    Chardev *chr = vcon->chr.chr;

    if (vcon->qga_iothread && chr &&
        (chr->gcontext == vcon->qga_gcontext ||
         context == vcon->qga_gcontext) &&
        qemu_get_current_aio_context() !=
        iothread_get_aio_context(vcon->qga_iothread)) {
        VirtConsoleSetHandlersReq req = {
            .vcon = vcon,
            .fd_can_read = fd_can_read,
            .fd_read = fd_read,
            .fd_event = fd_event,
            .be_change = be_change,
            .opaque = opaque,
            .context = context,
            .set_open = set_open,
        };
        qemu_event_init(&req.done, false);
        aio_bh_schedule_oneshot(iothread_get_aio_context(vcon->qga_iothread),
                                virtconsole_set_handlers_bh, &req);
        qemu_event_wait(&req.done);
        qemu_event_destroy(&req.done);
        return;
    }

    qemu_chr_fe_set_handlers(&vcon->chr, fd_can_read, fd_read, fd_event,
                             be_change, opaque, context, set_open);
}

static void enable_rx_kicks(VirtIOSerialPort *port)
{
    VirtConsole *vcon = VIRTIO_CONSOLE(port);
    VirtQueue *vq = port->ivq;

    if (!vcon->rx_kicks_suppressed) {
        return;
    }

    g_autoptr(VirtioNetworkLock) guard =
        virtio_lock_network(VIRTIO_DEVICE(port->vser));
    RCU_READ_LOCK_GUARD();
    if (!virtio_queue_ready(vq)) {
        vcon->rx_kicks_suppressed = false;
    } else if (virtio_queue_has_ram_rings(vq)) {
        virtio_queue_set_notification(vq, true);
        vcon->rx_kicks_suppressed = false;
    }
}

static void virtconsole_suspend(Notifier *notifier, void *unused)
{
    VirtConsole *vcon = container_of(notifier, VirtConsole, rx_suspend);

    enable_rx_kicks(VIRTIO_SERIAL_PORT(vcon));
}

/*
 * Callback function that's called from chardevs when backend becomes
 * writable.
 */
static gboolean chr_write_unblocked(void *do_not_use, GIOCondition cond,
                                    void *opaque)
{
    VirtConsole *vcon = opaque;

    vcon->watch = 0;
    virtio_serial_throttle_port(VIRTIO_SERIAL_PORT(vcon), false);
    return FALSE;
}

/* Callback function that's called when the guest sends us data */
static ssize_t flush_buf(VirtIOSerialPort *port,
                         const uint8_t *buf, ssize_t len)
{
    VirtConsole *vcon = VIRTIO_CONSOLE(port);
    ssize_t ret;

    if (!qemu_chr_fe_backend_connected(&vcon->chr)) {
        /* If there's no backend, we can just say we consumed all data. */
        return len;
    }

    ret = qemu_chr_fe_write(&vcon->chr, buf, len);
    trace_virtio_console_flush_buf(port->id, len, ret);

    if (ret < len) {
        VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(port);

        /*
         * Ideally we'd get a better error code than just -1, but
         * that's what the chardev interface gives us right now.  If
         * we had a finer-grained message, like -EPIPE, we could close
         * this connection.
         */
        if (ret < 0)
            ret = 0;

        /* XXX we should be queuing data to send later for the
         * console devices too rather than silently dropping
         * console data on EAGAIN. The Linux virtio-console
         * hvc driver though does sends with spinlocks held,
         * so if we enable throttling that'll stall the entire
         * guest kernel, not merely the process writing to the
         * console.
         *
         * While we could queue data for later write without
         * enabling throttling, this would result in the guest
         * being able to trigger arbitrary memory usage in QEMU
         * buffering data for later writes.
         *
         * So fixing this problem likely requires fixing the
         * Linux virtio-console hvc driver to not hold spinlocks
         * while writing, and instead merely block the process
         * that's writing. QEMU would then need some way to detect
         * if the guest had the fixed driver too, before we can
         * use throttling on host side.
         */
        if (!k->is_console) {
            virtio_serial_throttle_port(port, true);
            if (!vcon->watch) {
                vcon->watch = qemu_chr_fe_add_watch(&vcon->chr,
                                                    G_IO_OUT|G_IO_HUP,
                                                    chr_write_unblocked, vcon);
            }
        }
    }
    return ret;
}

/* Callback function that's called when the guest opens/closes the port */
static void set_guest_connected(VirtIOSerialPort *port, int guest_connected)
{
    VirtConsole *vcon = VIRTIO_CONSOLE(port);
    DeviceState *dev = DEVICE(port);
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(port);

    if (!guest_connected) {
        enable_rx_kicks(port);
    }

    if (!k->is_console) {
        qemu_chr_fe_set_open(&vcon->chr, guest_connected);
    }

    if (dev->id) {
        qapi_event_send_vserport_change(dev->id, guest_connected);
    }
}

static bool has_local_read_watch(CharBackend *backend)
{
    Chardev *chr;
    SocketChardev *socket;

    if (!qemu_mutex_iothread_locked()) {
        return false;
    }

    /* The frontend supports chardev replacement under the BQL. */
    chr = backend->chr;
    if (!chr || qemu_chr_replay(chr) ||
        !object_dynamic_cast(OBJECT(chr), TYPE_CHARDEV_SOCKET) ||
        CHARDEV_GET_CLASS(chr)->chr_accept_input) {
        return false;
    }

    socket = SOCKET_CHARDEV(chr);
    QEMU_LOCK_GUARD(&chr->chr_write_lock);
    return socket->state == TCP_CHARDEV_STATE_CONNECTED &&
           socket->is_listen && socket->addr &&
           socket->addr->type == SOCKET_ADDRESS_TYPE_UNIX &&
           !socket->is_telnet && !socket->is_tn3270 &&
           socket->ioc == QIO_CHANNEL(socket->sioc) &&
           chr->gsource &&
           g_source_get_context(chr->gsource) == g_main_context_default();
}

static void guest_writable(VirtIOSerialPort *port)
{
    VirtConsole *vcon = VIRTIO_CONSOLE(port);

    if (vcon->qga_gcontext && vcon->chr.chr &&
        vcon->chr.chr->gcontext == vcon->qga_gcontext) {
        g_main_context_wakeup(vcon->qga_gcontext);
    } else if (vcon->local_socket_wake && has_local_read_watch(&vcon->chr)) {
        qemu_notify_event_for_main_context();
    } else {
        qemu_chr_fe_accept_input(&vcon->chr);
    }
}

/* Readiness of the guest to accept data on a port */
static int chr_can_read(void *opaque)
{
    VirtConsole *vcon = opaque;
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(vcon);
    g_autoptr(VirtioNetworkLock) guard =
        virtio_lock_network(VIRTIO_DEVICE(port->vser));
    size_t bytes;
    VirtIODevice *vdev;
    Chardev *chr;
    SocketChardev *s;

    WITH_RCU_READ_LOCK_GUARD() {
        bytes = virtio_serial_guest_ready(port);
    }

    bool on_qga_iothread = vcon->qga_gcontext &&
                           g_main_context_is_owner(vcon->qga_gcontext);

    if (!vcon->suppress_rx_kicks ||
        (!on_qga_iothread &&
         (!qemu_mutex_iothread_locked() ||
          !g_main_context_is_owner(g_main_context_default()))) ||
        icount_enabled() || replay_mode != REPLAY_MODE_NONE) {
        return bytes;
    }

    vdev = VIRTIO_DEVICE(port->vser);
    /* A platform DMA mapping can disappear before a stopped queue is rearmed. */
    if (vdev->dma_as != &address_space_memory) {
        return bytes;
    }
    chr = vcon->chr.chr;          /* Backend changes run under the BQL. */
    if (!chr || !object_dynamic_cast(OBJECT(chr), TYPE_CHARDEV_SOCKET) ||
        qemu_chr_replay(chr) || !port->host_connected || !port->guest_connected ||
        !vdev->vm_running || !runstate_is_running() ||
        !(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) ||
        virtio_vdev_has_feature(vdev, VIRTIO_F_RING_PACKED) ||
        virtio_vdev_has_feature(vdev, VIRTIO_RING_F_EVENT_IDX) ||
        (chr->gcontext && chr->gcontext != g_main_context_default() &&
         chr->gcontext != vcon->qga_gcontext)) {
        enable_rx_kicks(port);
        return bytes;
    }

    /* Socket configuration is immutable until the backend is replaced. */
    s = SOCKET_CHARDEV(chr);
    if (!s->is_listen || !s->addr || s->addr->type != SOCKET_ADDRESS_TYPE_UNIX ||
        s->tls_creds || s->is_telnet || s->is_tn3270 || s->is_websock) {
        enable_rx_kicks(port);
        return bytes;
    }

    RCU_READ_LOCK_GUARD();
    if (!virtio_queue_has_ram_rings(port->ivq)) {
        return bytes;
    }

    if (!bytes && vcon->rx_kicks_suppressed) {
        enable_rx_kicks(port);
        /* The guest may post a buffer before it sees the enabled flag. */
        bytes = virtio_serial_guest_ready(port);
    }
    /* Resetting an individual queue restores its notification state. */
    if (bytes && virtio_queue_get_notification(port->ivq)) {
        virtio_queue_set_notification(port->ivq, false);
        vcon->rx_kicks_suppressed = true;
    }
    return bytes;
}

/* Send data from a char device over to the guest */
static void chr_read(void *opaque, const uint8_t *buf, int size)
{
    VirtConsole *vcon = opaque;
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(vcon);

    g_autoptr(VirtioNetworkLock) guard =
        virtio_lock_network(VIRTIO_DEVICE(port->vser));

    trace_virtio_console_chr_read(port->id, size);
    virtio_serial_write(port, buf, size);
}

static void virtconsole_update_tx_backend(VirtConsole *vcon, bool enable)
{
#ifdef CONFIG_LINUX
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(vcon);
    Chardev *chr = vcon->chr.chr;
    SocketChardev *s;
    int type;
    socklen_t len = sizeof(type);

    if (!VIRTIO_DEVICE(port->vser)->try_output ||
        VIRTIO_SERIAL_PORT_GET_CLASS(port)->is_console) {
        return;
    }
    g_autoptr(VirtioNetworkLock) guard =
        virtio_lock_network(VIRTIO_DEVICE(port->vser));

    port->direct_write_backend = NULL;
    if (!enable || !chr || qemu_chr_replay(chr) || chr->logfd >= 0 ||
        !object_dynamic_cast(OBJECT(chr), TYPE_CHARDEV_SOCKET)) {
        return;
    }

    /* Publish after connect initialized the channels. Revoke on close and
     * before pause removes the frontend's connection callbacks. */
    s = SOCKET_CHARDEV(chr);
    if (s->state == TCP_CHARDEV_STATE_CONNECTED && s->is_listen &&
        s->addr && s->addr->type == SOCKET_ADDRESS_TYPE_UNIX &&
        !s->tls_creds && !s->is_websock && !s->is_telnet && !s->is_tn3270 &&
        s->sioc && s->ioc == QIO_CHANNEL(s->sioc) &&
        s->sioc->localAddr.ss_family == AF_UNIX &&
        !getsockopt(s->sioc->fd, SOL_SOCKET, SO_TYPE, &type, &len) &&
        type == SOCK_STREAM &&
        (!chr->gcontext || chr->gcontext == g_main_context_default() ||
         (vcon->qga_gcontext && chr->gcontext == vcon->qga_gcontext))) {
        port->direct_write_backend = chr;
    }
#endif
}

static void chr_event(void *opaque, QEMUChrEvent event)
{
    VirtConsole *vcon = opaque;
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(vcon);

    trace_virtio_console_chr_event(port->id, event);
    switch (event) {
    case CHR_EVENT_OPENED:
        virtio_serial_open(port);
        virtconsole_update_tx_backend(vcon, true);
        break;
    case CHR_EVENT_CLOSED:
        virtconsole_update_tx_backend(vcon, false);
        enable_rx_kicks(port);
        virtconsole_remove_watch(vcon);
        virtio_serial_close(port);
        break;
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        /* Ignore */
        break;
    }
}

static void virtconsole_configure_rx_watch(VirtConsole *vcon)
{
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(vcon);

    vcon->chr.guard_socket_read = !k->is_console &&
        lats_nobql_enabled();
}

#ifdef CONFIG_LINUX
static ssize_t virtconsole_try_write(VirtIOSerialPort *port,
                                     const struct iovec *iov, unsigned int niov)
{
    SocketChardev *s = SOCKET_CHARDEV(port->direct_write_backend);
    struct iovec batch[16];
    struct msghdr msg = { .msg_iov = batch };
    ssize_t ret = -1;

    if (qemu_mutex_trylock(&CHARDEV(s)->chr_write_lock)) {
        return -1;
    }
    if (s->state == TCP_CHARDEV_STATE_CONNECTED && s->sioc &&
        s->ioc == QIO_CHANNEL(s->sioc) && !s->write_msgfds_num) {
        msg.msg_iovlen = iov_copy(batch, ARRAY_SIZE(batch), iov, niov, 0, 65536);
        ret = sendmsg(s->sioc->fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (ret > 0) {
            trace_virtio_console_flush_buf(port->id, iov_size(batch, msg.msg_iovlen), ret);
        }
    }
    qemu_mutex_unlock(&CHARDEV(s)->chr_write_lock);
    return ret;
}
#endif

static int chr_be_change(void *opaque)
{
    VirtConsole *vcon = opaque;
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(vcon);
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(port);
    GMainContext *ctx = k->is_console ? NULL : virtconsole_get_chr_context(vcon);

    virtconsole_configure_rx_watch(vcon);
    enable_rx_kicks(port);
    if (k->is_console) {
        qemu_chr_fe_set_handlers(&vcon->chr, chr_can_read, chr_read,
                                 NULL, chr_be_change, vcon, NULL, true);
    } else {
        virtconsole_set_handlers_sync(vcon, chr_can_read, chr_read,
                                      chr_event, chr_be_change, vcon,
                                      ctx, false);
    }

    if (vcon->watch) {
        virtconsole_remove_watch(vcon);
        vcon->watch = qemu_chr_fe_add_watch(&vcon->chr,
                                            G_IO_OUT | G_IO_HUP,
                                            chr_write_unblocked, vcon);
    }

    virtconsole_update_tx_backend(vcon, true);
    return 0;
}

static void virtconsole_enable_backend(VirtIOSerialPort *port, bool enable)
{
    VirtConsole *vcon = VIRTIO_CONSOLE(port);

    virtconsole_update_tx_backend(vcon, enable);
    if (!enable) {
        /* Clear the ring hint before a paused VM's RAM is saved. */
        enable_rx_kicks(port);
    }
    if (!qemu_chr_fe_backend_connected(&vcon->chr)) {
        return;
    }

    if (enable) {
        VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(port);
        GMainContext *ctx = k->is_console ? NULL :
                            virtconsole_get_chr_context(vcon);

        virtconsole_set_handlers_sync(vcon, chr_can_read, chr_read,
                                      k->is_console ? NULL : chr_event,
                                      chr_be_change, vcon, ctx, false);
    } else {
        virtconsole_set_handlers_sync(vcon, NULL, NULL, NULL,
                                      NULL, NULL, vcon->qga_gcontext, false);
    }
}

static void virtconsole_realize(DeviceState *dev, Error **errp)
{
    VirtIOSerialPort *port = VIRTIO_SERIAL_PORT(dev);
    VirtConsole *vcon = VIRTIO_CONSOLE(dev);
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_GET_CLASS(dev);

    virtconsole_configure_rx_watch(vcon);
    vcon->local_socket_wake = !k->is_console &&
        lats_nobql_enabled();
    vcon->suppress_rx_kicks = !k->is_console &&
        lats_nobql_enabled();
    vcon->qga_iothread_enabled = !k->is_console &&
        tcg_enabled() && !icount_enabled() &&
        replay_mode == REPLAY_MODE_NONE &&
        lats_nobql_enabled();

    if (port->id == 0 && !k->is_console) {
        error_setg(errp, "Port number 0 on virtio-serial devices reserved "
                   "for virtconsole devices for backward compatibility.");
        return;
    }

    if (qemu_chr_fe_backend_connected(&vcon->chr)) {
        /*
         * For consoles we don't block guest data transfer just
         * because nothing is connected - we'll just let it go
         * whetherever the chardev wants - /dev/null probably.
         *
         * For serial ports we need 100% reliable data transfer
         * so we use the opened/closed signals from chardev to
         * trigger open/close of the device
         */
        if (k->is_console) {
            qemu_chr_fe_set_handlers(&vcon->chr, chr_can_read, chr_read,
                                     NULL, chr_be_change,
                                     vcon, NULL, true);
            virtio_serial_open(port);
        } else {
            GMainContext *ctx = virtconsole_get_chr_context(vcon);

            virtconsole_set_handlers_sync(vcon, chr_can_read, chr_read,
                                          chr_event, chr_be_change,
                                          vcon, ctx, false);
        }
    }
    virtconsole_update_tx_backend(vcon, true);
    if (vcon->suppress_rx_kicks) {
        vcon->rx_suspend.notify = virtconsole_suspend;
        qemu_register_suspend_notifier(&vcon->rx_suspend);
    }
}

static void virtconsole_unrealize(DeviceState *dev)
{
    VirtConsole *vcon = VIRTIO_CONSOLE(dev);

    if (vcon->suppress_rx_kicks) {
        notifier_remove(&vcon->rx_suspend);
    }
    virtconsole_update_tx_backend(vcon, false);
    enable_rx_kicks(VIRTIO_SERIAL_PORT(vcon));
    if (qemu_chr_fe_backend_connected(&vcon->chr)) {
        virtconsole_set_handlers_sync(vcon, NULL, NULL, NULL,
                                      NULL, NULL, NULL, false);
    }
    virtconsole_remove_watch(vcon);
    if (vcon->qga_iothread) {
        iothread_destroy(vcon->qga_iothread);
        vcon->qga_iothread = NULL;
        vcon->qga_gcontext = NULL;
    }
}

static void virtconsole_class_init(ObjectClass *klass, void *data)
{
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_CLASS(klass);

    k->is_console = true;
}

static const TypeInfo virtconsole_info = {
    .name          = "virtconsole",
    .parent        = TYPE_VIRTIO_CONSOLE_SERIAL_PORT,
    .class_init    = virtconsole_class_init,
};

static Property virtserialport_properties[] = {
    DEFINE_PROP_CHR("chardev", VirtConsole, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtserialport_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtIOSerialPortClass *k = VIRTIO_SERIAL_PORT_CLASS(klass);

    k->realize = virtconsole_realize;
    k->unrealize = virtconsole_unrealize;
    k->have_data = flush_buf;
#ifdef CONFIG_LINUX
    k->try_write = virtconsole_try_write;
#endif
    k->set_guest_connected = set_guest_connected;
    k->enable_backend = virtconsole_enable_backend;
    k->guest_writable = guest_writable;
    k->restore_rx_notifications = enable_rx_kicks;
    device_class_set_props(dc, virtserialport_properties);
}

static const TypeInfo virtserialport_info = {
    .name          = TYPE_VIRTIO_CONSOLE_SERIAL_PORT,
    .parent        = TYPE_VIRTIO_SERIAL_PORT,
    .instance_size = sizeof(VirtConsole),
    .class_init    = virtserialport_class_init,
};

static void virtconsole_register_types(void)
{
    type_register_static(&virtserialport_info);
    type_register_static(&virtconsole_info);
}

type_init(virtconsole_register_types)
