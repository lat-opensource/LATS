/*
 * USB UHCI controller emulation
 *
 * Copyright (c) 2005 Fabrice Bellard
 *
 * Copyright (c) 2008 Max Krasnyansky
 *     Magor rewrite of the UHCI data structures parser and frame processor
 *     Support for fully async operation and multiple outstanding transactions
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

#include "qemu/osdep.h"
#include "hw/usb.h"
#include "hw/usb/hid.h"
#include "hw/usb/uhci-regs.h"
#include "migration/vmstate.h"
#include "hw/pci/pci.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "qemu/iov.h"
#include "sysemu/dma.h"
#include "trace.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/rcu.h"
#include "migration/misc.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/iothread.h"
#include "sysemu/qtest.h"
#include "sysemu/replay.h"
#include "qom/object.h"
#include "hcd-uhci.h"


enum {
    TRIAL_QH = 1 << 0,
    TRIAL_TD = 1 << 1,
    TRIAL_ACTIVE = 1 << 2,
    TRIAL_IOC = 1 << 3,
    TRIAL_NAK = 1 << 4,
    TRIAL_OTHER_PACKET = 1 << 5,
    TRIAL_QUEUES = 1 << 6,
    TRIAL_DMA_UPDATE = 1 << 7,
    TRIAL_IRQ = 1 << 8,
    TRIAL_STOP = 1 << 9,
    TRIAL_NO_FRAME = 1 << 10,
};

static bool trial_active;
static unsigned trial_work;
static uint64_t trial_counts[1 << 11];
static uint64_t trial_frames;
static uint64_t trial_locks;
static uint64_t trial_worker_ticks;
static uint64_t trial_worker_frames;
static unsigned trial_gate;
static uint64_t trial_nak_ticks;
static uint64_t trial_nak_frames;
static bool trial_enable_nak;


uint64_t uhci_trial_worker_ticks(void);
uint64_t uhci_trial_worker_ticks(void)
{
    return qatomic_read(&trial_worker_ticks);
}

unsigned uhci_trial_worker_gate(unsigned value);
unsigned uhci_trial_worker_gate(unsigned value)
{
    if (value == 1 || value == 3) {
        qatomic_set(&trial_gate, value);
    }
    return qatomic_read(&trial_gate);
}

static void trial_uhci_report(const char *phase)
{
    fprintf(stderr, "UHCI_PHASE %s locked_callbacks=%" PRIu64
                    " frames=%" PRIu64 "\n", phase, trial_locks, trial_frames);
    fprintf(stderr, "UHCI_IDLE %s callbacks=%" PRIu64 " frames=%" PRIu64 "\n",
            phase, qatomic_read(&trial_worker_ticks), qatomic_read(&trial_worker_frames));
    fprintf(stderr, "UHCI_NAK %s callbacks=%" PRIu64 " frames=%" PRIu64 "\n",
            phase, qatomic_read(&trial_nak_ticks), qatomic_read(&trial_nak_frames));
    for (unsigned i = 0; i < ARRAY_SIZE(trial_counts); i++) {
        if (trial_counts[i]) {
            fprintf(stderr, "UHCI_WORK %s %#x %" PRIu64 "\n", phase, i,
                    trial_counts[i]);
        }
    }
}

static void trial_uhci_exit(void)
{
    trial_uhci_report("exit");
}

static void trial_uhci_vmstate(void *opaque, bool running, RunState state)
{
    if (!running && state == RUN_STATE_PAUSED) {
        trial_uhci_report("menu");
    }
}

static void trial_uhci_end(void)
{
    trial_counts[trial_work]++;
    trial_active = false;
}

#define FRAME_TIMER_FREQ 1000

#define FRAME_MAX_LOOPS  256

/* Must be large enough to handle 10 frame delay for initial isoc requests */
#define QH_VALID         32

#define MAX_FRAMES_PER_TICK    (QH_VALID / 2)

enum {
    TD_RESULT_STOP_FRAME = 10,
    TD_RESULT_COMPLETE,
    TD_RESULT_NEXT_QH,
    TD_RESULT_ASYNC_START,
    TD_RESULT_ASYNC_CONT,
};

typedef struct UHCIAsync UHCIAsync;

struct UHCIPCIDeviceClass {
    PCIDeviceClass parent_class;
    UHCIInfo       info;
};

/* 
 * Pending async transaction.
 * 'packet' must be the first field because completion
 * handler does "(UHCIAsync *) pkt" cast.
 */

struct UHCIAsync {
    USBPacket packet;
    uint8_t   static_buf[64]; /* 64 bytes is enough, except for isoc packets */
    uint8_t   *buf;
    UHCIQueue *queue;
    QTAILQ_ENTRY(UHCIAsync) next;
    uint32_t  td_addr;
    uint8_t   done;
};

struct UHCIQueue {
    uint32_t  qh_addr;
    uint32_t  token;
    UHCIState *uhci;
    USBEndpoint *ep;
    QTAILQ_ENTRY(UHCIQueue) next;
    QTAILQ_HEAD(, UHCIAsync) asyncs;
    int8_t    valid;
};

typedef struct UHCI_TD {
    uint32_t link;
    uint32_t ctrl; /* see TD_CTRL_xxx */
    uint32_t token;
    uint32_t buffer;
} UHCI_TD;

typedef struct UHCI_QH {
    uint32_t link;
    uint32_t el_link;
} UHCI_QH;

static void uhci_async_cancel(UHCIAsync *async);
static void uhci_queue_fill(UHCIQueue *q, UHCI_TD *td);
static void uhci_resume(void *opaque);
static void uhci_idle_revoke(UHCIState *s);
static uint16_t uhci_idle_frnum(UHCIState *s);

static inline int32_t uhci_queue_token(UHCI_TD *td)
{
    if ((td->token & (0xf << 15)) == 0) {
        /* ctrl ep, cover ep and dev, not pid! */
        return td->token & 0x7ff00;
    } else {
        /* covers ep, dev, pid -> identifies the endpoint */
        return td->token & 0x7ffff;
    }
}

static UHCIQueue *uhci_queue_new(UHCIState *s, uint32_t qh_addr, UHCI_TD *td,
                                 USBEndpoint *ep)
{
    UHCIQueue *queue;

    queue = g_new0(UHCIQueue, 1);
    queue->uhci = s;
    queue->qh_addr = qh_addr;
    queue->token = uhci_queue_token(td);
    queue->ep = ep;
    QTAILQ_INIT(&queue->asyncs);
    QTAILQ_INSERT_HEAD(&s->queues, queue, next);
    queue->valid = QH_VALID;
    trace_usb_uhci_queue_add(queue->token);
    return queue;
}

static void uhci_queue_free(UHCIQueue *queue, const char *reason)
{
    UHCIState *s = queue->uhci;
    UHCIAsync *async;

    while (!QTAILQ_EMPTY(&queue->asyncs)) {
        async = QTAILQ_FIRST(&queue->asyncs);
        uhci_async_cancel(async);
    }
    usb_device_ep_stopped(queue->ep->dev, queue->ep);

    trace_usb_uhci_queue_del(queue->token, reason);
    QTAILQ_REMOVE(&s->queues, queue, next);
    g_free(queue);
}

static UHCIQueue *uhci_queue_find(UHCIState *s, UHCI_TD *td)
{
    uint32_t token = uhci_queue_token(td);
    UHCIQueue *queue;

    QTAILQ_FOREACH(queue, &s->queues, next) {
        if (queue->token == token) {
            return queue;
        }
    }
    return NULL;
}

static bool uhci_queue_verify(UHCIQueue *queue, uint32_t qh_addr, UHCI_TD *td,
                              uint32_t td_addr, bool queuing)
{
    UHCIAsync *first = QTAILQ_FIRST(&queue->asyncs);
    uint32_t queue_token_addr = (queue->token >> 8) & 0x7f;

    return queue->qh_addr == qh_addr &&
           queue->token == uhci_queue_token(td) &&
           queue_token_addr == queue->ep->dev->addr &&
           (queuing || !(td->ctrl & TD_CTRL_ACTIVE) || first == NULL ||
            first->td_addr == td_addr);
}

static UHCIAsync *uhci_async_alloc(UHCIQueue *queue, uint32_t td_addr)
{
    UHCIAsync *async = g_new0(UHCIAsync, 1);

    async->queue = queue;
    async->td_addr = td_addr;
    usb_packet_init(&async->packet);
    trace_usb_uhci_packet_add(async->queue->token, async->td_addr);

    return async;
}

static void uhci_async_free(UHCIAsync *async)
{
    trace_usb_uhci_packet_del(async->queue->token, async->td_addr);
    usb_packet_cleanup(&async->packet);
    if (async->buf != async->static_buf) {
        g_free(async->buf);
    }
    g_free(async);
}

static void uhci_async_link(UHCIAsync *async)
{
    UHCIQueue *queue = async->queue;
    QTAILQ_INSERT_TAIL(&queue->asyncs, async, next);
    trace_usb_uhci_packet_link_async(async->queue->token, async->td_addr);
}

static void uhci_async_unlink(UHCIAsync *async)
{
    UHCIQueue *queue = async->queue;
    QTAILQ_REMOVE(&queue->asyncs, async, next);
    trace_usb_uhci_packet_unlink_async(async->queue->token, async->td_addr);
}

static void uhci_async_cancel(UHCIAsync *async)
{
    uhci_async_unlink(async);
    trace_usb_uhci_packet_cancel(async->queue->token, async->td_addr,
                                 async->done);
    if (!async->done)
        usb_cancel_packet(&async->packet);
    uhci_async_free(async);
}

/*
 * Mark all outstanding async packets as invalid.
 * This is used for canceling them when TDs are removed by the HCD.
 */
static void uhci_async_validate_begin(UHCIState *s)
{
    UHCIQueue *queue;

    QTAILQ_FOREACH(queue, &s->queues, next) {
        queue->valid--;
    }
}

/*
 * Cancel async packets that are no longer valid
 */
static void uhci_async_validate_end(UHCIState *s)
{
    UHCIQueue *queue, *n;

    QTAILQ_FOREACH_SAFE(queue, &s->queues, next, n) {
        if (!queue->valid) {
            uhci_queue_free(queue, "validate-end");
        }
    }
}

static void uhci_async_cancel_device(UHCIState *s, USBDevice *dev)
{
    UHCIQueue *queue, *n;

    QTAILQ_FOREACH_SAFE(queue, &s->queues, next, n) {
        if (queue->ep->dev == dev) {
            uhci_queue_free(queue, "cancel-device");
        }
    }
}

static void uhci_async_cancel_all(UHCIState *s)
{
    UHCIQueue *queue, *nq;

    QTAILQ_FOREACH_SAFE(queue, &s->queues, next, nq) {
        uhci_queue_free(queue, "cancel-all");
    }
}

static UHCIAsync *uhci_async_find_td(UHCIState *s, uint32_t td_addr)
{
    UHCIQueue *queue;
    UHCIAsync *async;

    QTAILQ_FOREACH(queue, &s->queues, next) {
        QTAILQ_FOREACH(async, &queue->asyncs, next) {
            if (async->td_addr == td_addr) {
                return async;
            }
        }
    }
    return NULL;
}

static void uhci_update_irq(UHCIState *s)
{
    int level = 0;
    if (((s->status2 & 1) && (s->intr & (1 << 2))) ||
        ((s->status2 & 2) && (s->intr & (1 << 3))) ||
        ((s->status & UHCI_STS_USBERR) && (s->intr & (1 << 0))) ||
        ((s->status & UHCI_STS_RD) && (s->intr & (1 << 1))) ||
        (s->status & UHCI_STS_HSERR) ||
        (s->status & UHCI_STS_HCPERR)) {
        level = 1;
    }
    qemu_set_irq(s->irq, level);
}

static void uhci_reset(DeviceState *dev)
{
    PCIDevice *d = PCI_DEVICE(dev);
    UHCIState *s = UHCI(d);
    uint8_t *pci_conf;
    int i;
    UHCIPort *port;

    uhci_idle_revoke(s);
    trace_usb_uhci_reset();

    pci_conf = s->dev.config;

    pci_conf[0x6a] = 0x01; /* usb clock */
    pci_conf[0x6b] = 0x00;
    s->cmd = 0;
    s->status = UHCI_STS_HCHALTED;
    s->status2 = 0;
    s->intr = 0;
    s->fl_base_addr = 0;
    s->sof_timing = 64;

    for(i = 0; i < NB_PORTS; i++) {
        port = &s->ports[i];
        port->ctrl = 0x0080;
        if (port->port.dev && port->port.dev->attached) {
            usb_port_reset(&port->port);
        }
    }

    uhci_async_cancel_all(s);
    qemu_bh_cancel(s->bh);
    uhci_update_irq(s);
}

static const VMStateDescription vmstate_uhci_port = {
    .name = "uhci port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT16(ctrl, UHCIPort),
        VMSTATE_END_OF_LIST()
    }
};

static int uhci_pre_save(void *opaque)
{
    uhci_idle_revoke(opaque);
    return 0;
}

static int uhci_pre_load(void *opaque)
{
    UHCIState *s = opaque;

    uhci_idle_revoke(s);
    timer_del(s->frame_timer);
    return 0;
}

static int uhci_post_load(void *opaque, int version_id)
{
    UHCIState *s = opaque;

    if (version_id < 2) {
        s->expire_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
            (NANOSECONDS_PER_SECOND / FRAME_TIMER_FREQ);
    }
    return 0;
}

static const VMStateDescription vmstate_uhci = {
    .name = "uhci",
    .version_id = 3,
    .minimum_version_id = 1,
    .pre_save = uhci_pre_save,
    .pre_load = uhci_pre_load,
    .post_load = uhci_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, UHCIState),
        VMSTATE_UINT8_EQUAL(num_ports_vmstate, UHCIState, NULL),
        VMSTATE_STRUCT_ARRAY(ports, UHCIState, NB_PORTS, 1,
                             vmstate_uhci_port, UHCIPort),
        VMSTATE_UINT16(cmd, UHCIState),
        VMSTATE_UINT16(status, UHCIState),
        VMSTATE_UINT16(intr, UHCIState),
        VMSTATE_UINT16(frnum, UHCIState),
        VMSTATE_UINT32(fl_base_addr, UHCIState),
        VMSTATE_UINT8(sof_timing, UHCIState),
        VMSTATE_UINT8(status2, UHCIState),
        VMSTATE_TIMER_PTR(frame_timer, UHCIState),
        VMSTATE_INT64_V(expire_time, UHCIState, 2),
        VMSTATE_UINT32_V(pending_int_mask, UHCIState, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void uhci_port_write(void *opaque, hwaddr addr,
                            uint64_t val, unsigned size)
{
    UHCIState *s = opaque;

    uhci_idle_revoke(s);
    trace_usb_uhci_mmio_writew(addr, val);

    switch(addr) {
    case 0x00:
        if ((val & UHCI_CMD_RS) && !(s->cmd & UHCI_CMD_RS)) {
            /* start frame processing */
            trace_usb_uhci_schedule_start();
            s->expire_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                (NANOSECONDS_PER_SECOND / FRAME_TIMER_FREQ);
            timer_mod(s->frame_timer, s->expire_time);
            s->status &= ~UHCI_STS_HCHALTED;
        } else if (!(val & UHCI_CMD_RS)) {
            s->status |= UHCI_STS_HCHALTED;
        }
        if (val & UHCI_CMD_GRESET) {
            UHCIPort *port;
            int i;

            /* send reset on the USB bus */
            for(i = 0; i < NB_PORTS; i++) {
                port = &s->ports[i];
                usb_device_reset(port->port.dev);
            }
            uhci_reset(DEVICE(s));
            return;
        }
        if (val & UHCI_CMD_HCRESET) {
            uhci_reset(DEVICE(s));
            return;
        }
        s->cmd = val;
        if (val & UHCI_CMD_EGSM) {
            if ((s->ports[0].ctrl & UHCI_PORT_RD) ||
                (s->ports[1].ctrl & UHCI_PORT_RD)) {
                uhci_resume(s);
            }
        }
        break;
    case 0x02:
        s->status &= ~val;
        /* XXX: the chip spec is not coherent, so we add a hidden
           register to distinguish between IOC and SPD */
        if (val & UHCI_STS_USBINT)
            s->status2 = 0;
        uhci_update_irq(s);
        break;
    case 0x04:
        s->intr = val;
        uhci_update_irq(s);
        break;
    case 0x06:
        if (s->status & UHCI_STS_HCHALTED)
            s->frnum = val & 0x7ff;
        break;
    case 0x08:
        s->fl_base_addr &= 0xffff0000;
        s->fl_base_addr |= val & ~0xfff;
        break;
    case 0x0a:
        s->fl_base_addr &= 0x0000ffff;
        s->fl_base_addr |= (val << 16);
        break;
    case 0x0c:
        s->sof_timing = val & 0xff;
        break;
    case 0x10 ... 0x1f:
        {
            UHCIPort *port;
            USBDevice *dev;
            int n;

            n = (addr >> 1) & 7;
            if (n >= NB_PORTS)
                return;
            port = &s->ports[n];
            dev = port->port.dev;
            if (dev && dev->attached) {
                /* port reset */
                if ( (val & UHCI_PORT_RESET) &&
                     !(port->ctrl & UHCI_PORT_RESET) ) {
                    usb_device_reset(dev);
                }
            }
            port->ctrl &= UHCI_PORT_READ_ONLY;
            /* enabled may only be set if a device is connected */
            if (!(port->ctrl & UHCI_PORT_CCS)) {
                val &= ~UHCI_PORT_EN;
            }
            port->ctrl |= (val & ~UHCI_PORT_READ_ONLY);
            /* some bits are reset when a '1' is written to them */
            port->ctrl &= ~(val & UHCI_PORT_WRITE_CLEAR);
        }
        break;
    }
}

static uint64_t uhci_port_read(void *opaque, hwaddr addr, unsigned size)
{
    UHCIState *s = opaque;
    uint32_t val;

    switch(addr) {
    case 0x00:
        val = s->cmd;
        break;
    case 0x02:
        val = s->status;
        break;
    case 0x04:
        val = s->intr;
        break;
    case 0x06:
        val = uhci_idle_frnum(s);
        break;
    case 0x08:
        val = s->fl_base_addr & 0xffff;
        break;
    case 0x0a:
        val = (s->fl_base_addr >> 16) & 0xffff;
        break;
    case 0x0c:
        val = s->sof_timing;
        break;
    case 0x10 ... 0x1f:
        {
            UHCIPort *port;
            int n;
            n = (addr >> 1) & 7;
            if (n >= NB_PORTS)
                goto read_default;
            port = &s->ports[n];
            val = port->ctrl;
        }
        break;
    default:
    read_default:
        val = 0xff7f; /* disabled port */
        break;
    }

    trace_usb_uhci_mmio_readw(addr, val);

    return val;
}

/* signal resume if controller suspended */
static void uhci_resume (void *opaque)
{
    UHCIState *s = (UHCIState *)opaque;

    if (!s)
        return;

    if (s->cmd & UHCI_CMD_EGSM) {
        s->cmd |= UHCI_CMD_FGR;
        s->status |= UHCI_STS_RD;
        uhci_update_irq(s);
    }
}

static void uhci_attach(USBPort *port1)
{
    UHCIState *s = port1->opaque;
    UHCIPort *port = &s->ports[port1->index];

    /* set connect status */
    port->ctrl |= UHCI_PORT_CCS | UHCI_PORT_CSC;

    /* update speed */
    if (port->port.dev->speed == USB_SPEED_LOW) {
        port->ctrl |= UHCI_PORT_LSDA;
    } else {
        port->ctrl &= ~UHCI_PORT_LSDA;
    }

    uhci_resume(s);
}

static void uhci_detach(USBPort *port1)
{
    UHCIState *s = port1->opaque;
    UHCIPort *port = &s->ports[port1->index];

    uhci_idle_revoke(s);
    uhci_async_cancel_device(s, port1->dev);

    /* set connect status */
    if (port->ctrl & UHCI_PORT_CCS) {
        port->ctrl &= ~UHCI_PORT_CCS;
        port->ctrl |= UHCI_PORT_CSC;
    }
    /* disable port */
    if (port->ctrl & UHCI_PORT_EN) {
        port->ctrl &= ~UHCI_PORT_EN;
        port->ctrl |= UHCI_PORT_ENC;
    }

    uhci_resume(s);
}

static void uhci_child_detach(USBPort *port1, USBDevice *child)
{
    UHCIState *s = port1->opaque;

    uhci_idle_revoke(s);
    uhci_async_cancel_device(s, child);
}

static void uhci_wakeup(USBPort *port1)
{
    UHCIState *s = port1->opaque;
    UHCIPort *port = &s->ports[port1->index];

    if (port->ctrl & UHCI_PORT_SUSPEND && !(port->ctrl & UHCI_PORT_RD)) {
        uhci_idle_revoke(s);
        port->ctrl |= UHCI_PORT_RD;
        uhci_resume(s);
    }
}

static USBDevice *uhci_find_device(UHCIState *s, uint8_t addr)
{
    USBDevice *dev;
    int i;

    for (i = 0; i < NB_PORTS; i++) {
        UHCIPort *port = &s->ports[i];
        if (!(port->ctrl & UHCI_PORT_EN)) {
            continue;
        }
        dev = usb_find_device(&port->port, addr);
        if (dev != NULL) {
            return dev;
        }
    }
    return NULL;
}

static void uhci_read_td(UHCIState *s, UHCI_TD *td, uint32_t link)
{
    pci_dma_read(&s->dev, link & ~0xf, td, sizeof(*td));
    le32_to_cpus(&td->link);
    le32_to_cpus(&td->ctrl);
    le32_to_cpus(&td->token);
    le32_to_cpus(&td->buffer);
}

static int uhci_handle_td_error(UHCIState *s, UHCI_TD *td, uint32_t td_addr,
                                int status, uint32_t *int_mask)
{
    uint32_t queue_token = uhci_queue_token(td);
    int ret;

    switch (status) {
    case USB_RET_NAK:
        td->ctrl |= TD_CTRL_NAK;
        return TD_RESULT_NEXT_QH;

    case USB_RET_STALL:
        td->ctrl |= TD_CTRL_STALL;
        trace_usb_uhci_packet_complete_stall(queue_token, td_addr);
        ret = TD_RESULT_NEXT_QH;
        break;

    case USB_RET_BABBLE:
        td->ctrl |= TD_CTRL_BABBLE | TD_CTRL_STALL;
        /* frame interrupted */
        trace_usb_uhci_packet_complete_babble(queue_token, td_addr);
        ret = TD_RESULT_STOP_FRAME;
        break;

    case USB_RET_IOERROR:
    case USB_RET_NODEV:
    default:
        td->ctrl |= TD_CTRL_TIMEOUT;
        td->ctrl &= ~(3 << TD_CTRL_ERROR_SHIFT);
        trace_usb_uhci_packet_complete_error(queue_token, td_addr);
        ret = TD_RESULT_NEXT_QH;
        break;
    }

    td->ctrl &= ~TD_CTRL_ACTIVE;
    s->status |= UHCI_STS_USBERR;
    if (td->ctrl & TD_CTRL_IOC) {
        *int_mask |= 0x01;
    }
    uhci_update_irq(s);
    return ret;
}

static int uhci_complete_td(UHCIState *s, UHCI_TD *td, UHCIAsync *async, uint32_t *int_mask)
{
    int len = 0, max_len;
    uint8_t pid;

    max_len = ((td->token >> 21) + 1) & 0x7ff;
    pid = td->token & 0xff;

    if (td->ctrl & TD_CTRL_IOS)
        td->ctrl &= ~TD_CTRL_ACTIVE;

    if (async->packet.status != USB_RET_SUCCESS) {
        return uhci_handle_td_error(s, td, async->td_addr,
                                    async->packet.status, int_mask);
    }

    len = async->packet.actual_length;
    td->ctrl = (td->ctrl & ~0x7ff) | ((len - 1) & 0x7ff);

    /* The NAK bit may have been set by a previous frame, so clear it
       here.  The docs are somewhat unclear, but win2k relies on this
       behavior.  */
    td->ctrl &= ~(TD_CTRL_ACTIVE | TD_CTRL_NAK);
    if (td->ctrl & TD_CTRL_IOC)
        *int_mask |= 0x01;

    if (pid == USB_TOKEN_IN) {
        pci_dma_write(&s->dev, td->buffer, async->buf, len);
        if ((td->ctrl & TD_CTRL_SPD) && len < max_len) {
            *int_mask |= 0x02;
            /* short packet: do not update QH */
            trace_usb_uhci_packet_complete_shortxfer(async->queue->token,
                                                     async->td_addr);
            return TD_RESULT_NEXT_QH;
        }
    }

    /* success */
    trace_usb_uhci_packet_complete_success(async->queue->token,
                                           async->td_addr);
    return TD_RESULT_COMPLETE;
}

static int uhci_handle_td(UHCIState *s, UHCIQueue *q, uint32_t qh_addr,
                          UHCI_TD *td, uint32_t td_addr, uint32_t *int_mask)
{
    int ret, max_len;
    bool spd;
    bool queuing = (q != NULL);
    uint8_t pid = td->token & 0xff;
    UHCIAsync *async;

    async = uhci_async_find_td(s, td_addr);
    if (async) {
        if (uhci_queue_verify(async->queue, qh_addr, td, td_addr, queuing)) {
            assert(q == NULL || q == async->queue);
            q = async->queue;
        } else {
            uhci_queue_free(async->queue, "guest re-used pending td");
            async = NULL;
        }
    }

    if (q == NULL) {
        q = uhci_queue_find(s, td);
        if (q && !uhci_queue_verify(q, qh_addr, td, td_addr, queuing)) {
            uhci_queue_free(q, "guest re-used qh");
            q = NULL;
        }
    }

    if (q) {
        q->valid = QH_VALID;
    }

    if (trial_active) {
        trial_work |= TRIAL_TD;
        if (td->ctrl & TD_CTRL_ACTIVE) {
            trial_work |= TRIAL_ACTIVE;
        } else if (td->ctrl & TD_CTRL_IOC) {
            trial_work |= TRIAL_IOC;
        }
    }

    /* Is active ? */
    if (!(td->ctrl & TD_CTRL_ACTIVE)) {
        if (async) {
            /* Guest marked a pending td non-active, cancel the queue */
            uhci_queue_free(async->queue, "pending td non-active");
        }
        /*
         * ehci11d spec page 22: "Even if the Active bit in the TD is already
         * cleared when the TD is fetched ... an IOC interrupt is generated"
         */
        if (td->ctrl & TD_CTRL_IOC) {
                *int_mask |= 0x01;
        }
        return TD_RESULT_NEXT_QH;
    }

    switch (pid) {
    case USB_TOKEN_OUT:
    case USB_TOKEN_SETUP:
    case USB_TOKEN_IN:
        break;
    default:
        /* invalid pid : frame interrupted */
        s->status |= UHCI_STS_HCPERR;
        s->cmd &= ~UHCI_CMD_RS;
        uhci_update_irq(s);
        return TD_RESULT_STOP_FRAME;
    }

    if (async) {
        if (queuing) {
            /* we are busy filling the queue, we are not prepared
               to consume completed packages then, just leave them
               in async state */
            return TD_RESULT_ASYNC_CONT;
        }
        if (!async->done) {
            UHCI_TD last_td;
            UHCIAsync *last = QTAILQ_LAST(&async->queue->asyncs);
            /*
             * While we are waiting for the current td to complete, the guest
             * may have added more tds to the queue. Note we re-read the td
             * rather then caching it, as we want to see guest made changes!
             */
            uhci_read_td(s, &last_td, last->td_addr);
            uhci_queue_fill(async->queue, &last_td);

            return TD_RESULT_ASYNC_CONT;
        }
        uhci_async_unlink(async);
        goto done;
    }

    if (s->completions_only) {
        return TD_RESULT_ASYNC_CONT;
    }

    /* Allocate new packet */
    if (q == NULL) {
        USBDevice *dev;
        USBEndpoint *ep;

        dev = uhci_find_device(s, (td->token >> 8) & 0x7f);
        if (dev == NULL) {
            return uhci_handle_td_error(s, td, td_addr, USB_RET_NODEV,
                                        int_mask);
        }
        ep = usb_ep_get(dev, pid, (td->token >> 15) & 0xf);
        q = uhci_queue_new(s, qh_addr, td, ep);
    }
    async = uhci_async_alloc(q, td_addr);

    max_len = ((td->token >> 21) + 1) & 0x7ff;
    spd = (pid == USB_TOKEN_IN && (td->ctrl & TD_CTRL_SPD) != 0);
    usb_packet_setup(&async->packet, pid, q->ep, 0, td_addr, spd,
                     (td->ctrl & TD_CTRL_IOC) != 0);
    if (max_len <= sizeof(async->static_buf)) {
        async->buf = async->static_buf;
    } else {
        async->buf = g_malloc(max_len);
    }
    usb_packet_addbuf(&async->packet, async->buf, max_len);

    switch(pid) {
    case USB_TOKEN_OUT:
    case USB_TOKEN_SETUP:
        pci_dma_read(&s->dev, td->buffer, async->buf, max_len);
        usb_handle_packet(q->ep->dev, &async->packet);
        if (async->packet.status == USB_RET_SUCCESS) {
            async->packet.actual_length = max_len;
        }
        break;

    case USB_TOKEN_IN:
        usb_handle_packet(q->ep->dev, &async->packet);
        break;

    default:
        abort(); /* Never to execute */
    }

    if (async->packet.status == USB_RET_ASYNC) {
        uhci_async_link(async);
        if (!queuing) {
            uhci_queue_fill(q, td);
        }
        return TD_RESULT_ASYNC_START;
    }

done:
    if (trial_active) {
        trial_work |= async->packet.status == USB_RET_NAK ? TRIAL_NAK :
                                                            TRIAL_OTHER_PACKET;
    }
    ret = uhci_complete_td(s, td, async, int_mask);
    uhci_async_free(async);
    return ret;
}

static void uhci_async_complete(USBPort *port, USBPacket *packet)
{
    UHCIAsync *async = container_of(packet, UHCIAsync, packet);
    UHCIState *s = async->queue->uhci;

    if (packet->status == USB_RET_REMOVE_FROM_QUEUE) {
        uhci_async_cancel(async);
        return;
    }

    async->done = 1;
    /* Force processing of this packet *now*, needed for migration */
    s->completions_only = true;
    qemu_bh_schedule(s->bh);
}

static int is_valid(uint32_t link)
{
    return (link & 1) == 0;
}

static int is_qh(uint32_t link)
{
    return (link & 2) != 0;
}

static int depth_first(uint32_t link)
{
    return (link & 4) != 0;
}

/* QH DB used for detecting QH loops */
#define UHCI_MAX_QUEUES 128
typedef struct {
    uint32_t addr[UHCI_MAX_QUEUES];
    int      count;
} QhDb;

static void qhdb_reset(QhDb *db)
{
    db->count = 0;
}

/* Add QH to DB. Returns 1 if already present or DB is full. */
static int qhdb_insert(QhDb *db, uint32_t addr)
{
    int i;
    for (i = 0; i < db->count; i++)
        if (db->addr[i] == addr)
            return 1;

    if (db->count >= UHCI_MAX_QUEUES)
        return 1;

    db->addr[db->count++] = addr;
    return 0;
}

static void uhci_queue_fill(UHCIQueue *q, UHCI_TD *td)
{
    uint32_t int_mask = 0;
    uint32_t plink = td->link;
    UHCI_TD ptd;
    int ret;

    while (is_valid(plink)) {
        uhci_read_td(q->uhci, &ptd, plink);
        if (!(ptd.ctrl & TD_CTRL_ACTIVE)) {
            break;
        }
        if (uhci_queue_token(&ptd) != q->token) {
            break;
        }
        trace_usb_uhci_td_queue(plink & ~0xf, ptd.ctrl, ptd.token);
        ret = uhci_handle_td(q->uhci, q, q->qh_addr, &ptd, plink, &int_mask);
        if (ret == TD_RESULT_ASYNC_CONT) {
            break;
        }
        assert(ret == TD_RESULT_ASYNC_START);
        assert(int_mask == 0);
        plink = ptd.link;
    }
    usb_device_flush_ep_queue(q->ep->dev, q->ep);
}

static void uhci_process_frame(UHCIState *s)
{
    uint32_t frame_addr, link, old_td_ctrl, val, int_mask;
    uint32_t curr_qh, td_count = 0;
    int cnt, ret;
    UHCI_TD td;
    UHCI_QH qh;
    QhDb qhdb;

    frame_addr = s->fl_base_addr + ((s->frnum & 0x3ff) << 2);

    pci_dma_read(&s->dev, frame_addr, &link, 4);
    le32_to_cpus(&link);

    int_mask = 0;
    curr_qh  = 0;

    qhdb_reset(&qhdb);

    for (cnt = FRAME_MAX_LOOPS; is_valid(link) && cnt; cnt--) {
        if (!s->completions_only && s->frame_bytes >= s->frame_bandwidth) {
            /* We've reached the usb 1.1 bandwidth, which is
               1280 bytes/frame, stop processing */
            trace_usb_uhci_frame_stop_bandwidth();
            break;
        }
        if (is_qh(link)) {
            if (trial_active) {
                trial_work |= TRIAL_QH;
            }
            /* QH */
            trace_usb_uhci_qh_load(link & ~0xf);

            if (qhdb_insert(&qhdb, link)) {
                /*
                 * We're going in circles. Which is not a bug because
                 * HCD is allowed to do that as part of the BW management.
                 *
                 * Stop processing here if no transaction has been done
                 * since we've been here last time.
                 */
                if (td_count == 0) {
                    trace_usb_uhci_frame_loop_stop_idle();
                    break;
                } else {
                    trace_usb_uhci_frame_loop_continue();
                    td_count = 0;
                    qhdb_reset(&qhdb);
                    qhdb_insert(&qhdb, link);
                }
            }

            pci_dma_read(&s->dev, link & ~0xf, &qh, sizeof(qh));
            le32_to_cpus(&qh.link);
            le32_to_cpus(&qh.el_link);

            if (!is_valid(qh.el_link)) {
                /* QH w/o elements */
                curr_qh = 0;
                link = qh.link;
            } else {
                /* QH with elements */
                curr_qh = link;
                link = qh.el_link;
            }
            continue;
        }

        /* TD */
        uhci_read_td(s, &td, link);
        trace_usb_uhci_td_load(curr_qh & ~0xf, link & ~0xf, td.ctrl, td.token);

        old_td_ctrl = td.ctrl;
        ret = uhci_handle_td(s, NULL, curr_qh, &td, link, &int_mask);
        if (old_td_ctrl != td.ctrl) {
            if (trial_active) {
                trial_work |= TRIAL_DMA_UPDATE;
            }
            /* update the status bits of the TD */
            val = cpu_to_le32(td.ctrl);
            pci_dma_write(&s->dev, (link & ~0xf) + 4, &val, sizeof(val));
        }

        switch (ret) {
        case TD_RESULT_STOP_FRAME: /* interrupted frame */
            goto out;

        case TD_RESULT_NEXT_QH:
        case TD_RESULT_ASYNC_CONT:
            trace_usb_uhci_td_nextqh(curr_qh & ~0xf, link & ~0xf);
            link = curr_qh ? qh.link : td.link;
            continue;

        case TD_RESULT_ASYNC_START:
            trace_usb_uhci_td_async(curr_qh & ~0xf, link & ~0xf);
            link = curr_qh ? qh.link : td.link;
            continue;

        case TD_RESULT_COMPLETE:
            trace_usb_uhci_td_complete(curr_qh & ~0xf, link & ~0xf);
            link = td.link;
            td_count++;
            s->frame_bytes += (td.ctrl & 0x7ff) + 1;

            if (curr_qh) {
                if (trial_active) {
                    trial_work |= TRIAL_DMA_UPDATE;
                }
                /* update QH element link */
                qh.el_link = link;
                val = cpu_to_le32(qh.el_link);
                pci_dma_write(&s->dev, (curr_qh & ~0xf) + 4, &val, sizeof(val));

                if (!depth_first(link)) {
                    /* done with this QH */
                    curr_qh = 0;
                    link    = qh.link;
                }
            }
            break;

        default:
            assert(!"unknown return code");
        }

        /* go to the next entry */
    }

out:
    s->pending_int_mask |= int_mask;
}

static void uhci_bh(void *opaque)
{
    UHCIState *s = opaque;
    uhci_idle_revoke(s);
    uhci_process_frame(s);
}

#if HOST_LONG_BITS == 64

/*
 * An armed interval begins under BQL. The worker may advance frames through
 * ordinary RAM until it encounters work requiring the device model. The
 * epoch invalidates a scan when guest register writes, reset or unplug
 * change the input. BQL owners revoke the interval with one exchange and
 * consume only results that were published before that exchange.
 *
 * The lower word contains age (6 bits), slow and deadline slot flags, then
 * the frame advance (24 bits). The worker writes only the unpublished
 * deadline slot. FRNUM is derived from the same word as the cache age.
 */
#define IDLE_FRAME_NS (NANOSECONDS_PER_SECOND / FRAME_TIMER_FREQ)
#define IDLE_AGE_MASK UINT64_C(63)
#define IDLE_SLOW UINT64_C(64)
#define IDLE_SLOT UINT64_C(128)
#define IDLE_COUNT_MASK UINT32_C(0xffffff)
#define IDLE_EPOCH_MASK UINT64_C(0xffffffff00000000)

typedef struct UHCIIdle {
    MemoryListener dma_listener;
    UHCIState *owner;
    IOThread *thread;
    QEMUTimer *timer;
    QEMUBH *fallback;

    uint64_t phase;
    uint32_t epoch;               /* BQL only */
    USBEndpoint *nak_ep;          /* BQL only, valid for the armed interval */
    bool tablet_nak;              /* immutable while phase matches */
    int64_t first_alarm;
    int64_t alarms[2];            /* worker writes, published by phase */
    int64_t first_expire;
    uint32_t base;
    uint16_t first_frnum;
    uint32_t queue_addr;
    uint32_t queue_token;
    uint8_t device_addr;
} UHCIIdle;

static void uhci_frame_timer(void *opaque);

static uint32_t uhci_idle_count(uint64_t phase)
{
    return (phase >> 8) & IDLE_COUNT_MASK;
}

static int64_t uhci_idle_alarm(UHCIIdle *idle, uint64_t phase)
{
    return uhci_idle_count(phase)
        ? qatomic_read(&idle->alarms[!!(phase & IDLE_SLOT)])
        : qatomic_read(&idle->first_alarm);
}

static uint16_t uhci_idle_frnum(UHCIState *s)
{
    uint64_t phase = s->idle ? qatomic_read(&s->idle->phase) : 0;

    return (s->frnum + uhci_idle_count(phase)) & 0x7ff;
}

static int64_t uhci_idle_take(UHCIState *s)
{
    UHCIIdle *idle = s->idle;
    uint64_t phase = idle ? qatomic_xchg(&idle->phase, 0) : 0;
    uint32_t advance = uhci_idle_count(phase);
    UHCIQueue *queue;

    if (!phase) {
        return -1;
    }
    idle->nak_ep = NULL;
    s->frnum = (s->frnum + advance) & 0x7ff;
    s->expire_time += advance * IDLE_FRAME_NS;
    queue = QTAILQ_FIRST(&s->queues);
    if (queue) {
        queue->valid = phase & IDLE_AGE_MASK;
        if (!queue->valid) {
            uhci_queue_free(queue, "idle-frames");
        }
    }
    return uhci_idle_alarm(idle, phase);
}

static void uhci_idle_revoke(UHCIState *s)
{
    int64_t alarm = uhci_idle_take(s);

    if (alarm >= 0) {
        timer_mod(s->frame_timer, alarm);
    }
}

static void uhci_idle_dma_begin(MemoryListener *listener)
{
    UHCIIdle *idle = container_of(listener, UHCIIdle, dma_listener);

    /*
     * Map transactions call begin under BQL, before publishing any new
     * FlatView. Invalidate the worker's commit even if it has finished
     * reading the old mapping.
     */
    uhci_idle_revoke(idle->owner);
}

static void uhci_endpoint_wakeup(USBBus *bus, USBEndpoint *ep, unsigned stream)
{
    UHCIState *s = container_of(bus, UHCIState, bus);

    if (s->idle && object_dynamic_cast(OBJECT(ep->dev), TYPE_USB_TABLET)) {
        /* The tablet publishes HID input under BQL, before notification. */
        g_assert(qemu_mutex_iothread_locked());
        if (s->idle->nak_ep == ep) {
            uhci_idle_revoke(s);
        }
    }
}

static bool uhci_idle_scan(AddressSpace *as, FlatView *view,
                           uint32_t frame_addr, unsigned *age,
                           uint32_t queue_addr, uint32_t queue_token,
                           uint8_t device_addr, bool tablet_nak, bool *nak)
{
    uint32_t link, qh_next = 0, curr_qh = 0;
    QhDb qhdb;
    const uint32_t *entry;

    dma_barrier(as, DMA_DIRECTION_TO_DEVICE);
    entry = flatview_ram_read_ptr(view, frame_addr, sizeof(*entry));
    if (!entry) {
        return false;
    }
    if (*age) {
        --*age;
    }
    link = le32_to_cpu(qatomic_read(entry));
    qhdb_reset(&qhdb);
    for (int remaining = FRAME_MAX_LOOPS; remaining && is_valid(link);
         --remaining) {
        uint32_t addr = link & ~0xf;

        dma_barrier(as, DMA_DIRECTION_TO_DEVICE);
        if (is_qh(link)) {
            uint32_t element;

            if (qhdb_insert(&qhdb, link)) {
                break;
            }
            entry = flatview_ram_read_ptr(view, addr, sizeof(UHCI_QH));
            if (!entry) {
                return false;
            }
            qh_next = le32_to_cpu(qatomic_read(&entry[0]));
            element = le32_to_cpu(qatomic_read(&entry[1]));
            curr_qh = is_valid(element) ? link : 0;
            link = curr_qh ? element : qh_next;
        } else {
            UHCI_TD td;
            bool found_queue, same_queue;

            entry = flatview_ram_read_ptr(view, addr, sizeof(td));
            if (!entry) {
                return false;
            }
            td.link = le32_to_cpu(qatomic_read(&entry[0]));
            td.ctrl = le32_to_cpu(qatomic_read(&entry[1]));
            td.token = le32_to_cpu(qatomic_read(&entry[2]));
            found_queue = queue_addr != UINT32_MAX &&
                          queue_token == uhci_queue_token(&td);
            same_queue = found_queue && queue_addr == curr_qh &&
                         ((queue_token >> 8) & 0x7f) == device_addr;

            if (td.ctrl & TD_CTRL_ACTIVE) {
                /*
                 * The first NAK writes back the TD. Isochronous NAK also
                 * clears Active. An ordinary repeated IN NAK has neither
                 * effect, even when the TD requests IOC.
                 */
                if (!tablet_nak || !same_queue || !(td.ctrl & TD_CTRL_NAK) ||
                    (td.ctrl & TD_CTRL_IOS) ||
                    (td.token & 0xff) != USB_TOKEN_IN) {
                    return false;
                }
                *nak = true;
            } else if (td.ctrl & TD_CTRL_IOC) {
                return false;
            }
            /*
             * An inactive TD can still refresh or invalidate a cached
             * queue. Age zero is also possible while walking the final
             * live frame, so distinguish expiry from an absent queue.
             */
            if (found_queue) {
                if (same_queue) {
                    *age = QH_VALID;
                } else {
                    *age = 0;
                    queue_addr = UINT32_MAX;
                }
            }
            link = curr_qh ? qh_next : td.link;
        }
    }
    return true;
}

static void uhci_idle_fallback(void *opaque)
{
    UHCIState *s = opaque;

    if (qatomic_read(&s->idle->phase) & IDLE_SLOW) {
        uhci_idle_revoke(s);
    }
}

static void uhci_idle_tick(void *opaque)
{
    UHCIState *s = opaque;
    UHCIIdle *idle = s->idle;
    uint64_t phase = qatomic_load_acquire(&idle->phase);
    int64_t now, alarm, last, frames, skip;
    uint32_t advance, base, queue_addr, queue_token;
    uint16_t frnum;
    uint8_t device_addr;
    unsigned age;
    unsigned nak_frames = 0;
    bool slot, tablet_nak;
    AddressSpace *as;
    FlatView *view;

    if (!phase || (phase & IDLE_SLOW)) {
        return;
    }
    g_assert(!qemu_mutex_iothread_locked());
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    alarm = uhci_idle_alarm(idle, phase);
    if (now < alarm) {
        timer_mod_anticipate_ns(idle->timer, alarm);
        return;
    }
    advance = uhci_idle_count(phase);
    last = qatomic_read(&idle->first_expire) +
           advance * IDLE_FRAME_NS - IDLE_FRAME_NS;
    if (migration_incoming_postcopy_advised() ||
        now < last || now - last < IDLE_FRAME_NS) {
        goto slow;
    }
    frames = (now - last) / IDLE_FRAME_NS;
    skip = MAX(0, frames - s->maxframes);
    frames = MIN(frames - skip, MAX_FRAMES_PER_TICK);
    if (!frames || skip + advance + frames > IDLE_COUNT_MASK) {
        goto slow;
    }

    base = qatomic_read(&idle->base);
    frnum = qatomic_read(&idle->first_frnum) + advance + skip;
    queue_addr = qatomic_read(&idle->queue_addr);
    queue_token = qatomic_read(&idle->queue_token);
    device_addr = qatomic_read(&idle->device_addr);
    tablet_nak = qatomic_read(&idle->tablet_nak) &&
        !trace_event_get_state_backends(TRACE_USB_PACKET_STATE_CHANGE) &&
        !trace_event_get_state_backends(TRACE_USB_UHCI_PACKET_ADD) &&
        !trace_event_get_state_backends(TRACE_USB_UHCI_PACKET_DEL) &&
        !trace_event_get_state_backends(TRACE_USB_UHCI_TD_LOAD) &&
        !trace_event_get_state_backends(TRACE_USB_UHCI_TD_NEXTQH) &&
        !trace_event_get_state_backends(TRACE_USB_UHCI_FRAME_START) &&
        !trace_event_get_state_backends(TRACE_USB_UHCI_QH_LOAD) &&
        !trace_event_get_state_backends(TRACE_USB_UHCI_QUEUE_DEL) &&
        !trace_event_get_state_backends(TRACE_USB_UHCI_FRAME_LOOP_STOP_IDLE);
    age = phase & IDLE_AGE_MASK;
    as = pci_get_address_space(&s->dev);
    {
        RCU_READ_LOCK_GUARD();
        view = address_space_to_flatview(as);
        for (int64_t i = 0; i < frames; ++i) {
            uint32_t addr = base + (((frnum + i) & 0x3ff) << 2);
            bool nak = false;

            if (!uhci_idle_scan(as, view, addr, &age,
                                age ? queue_addr : UINT32_MAX,
                                queue_token, device_addr, tablet_nak, &nak)) {
                goto slow;
            }
            nak_frames += nak;
        }
        if (view != address_space_to_flatview(as)) {
            goto slow;
        }
    }
    advance += skip + frames;
    slot = !(phase & IDLE_SLOT);
    alarm = now + IDLE_FRAME_NS;
    qatomic_set(&idle->alarms[slot], alarm);
    uint64_t result = (phase & IDLE_EPOCH_MASK) | ((uint64_t)advance << 8) |
                      (slot ? IDLE_SLOT : 0) | age;
    bool gated = qatomic_cmpxchg(&trial_gate, 1, 2) == 1;
    if (gated) {
        for (unsigned i = 0; i < 10000 && qatomic_read(&trial_gate) == 2; ++i) {
            g_usleep(100);
        }
    }
    bool committed = qatomic_cmpxchg(&idle->phase, phase, result) == phase;
    if (gated) {
        qatomic_set(&trial_gate, committed ? 5 : 4);
    }
    if (committed) {
        qatomic_add(&trial_worker_frames, frames);
        qatomic_inc(&trial_worker_ticks);
        if (nak_frames) {
            qatomic_add(&trial_nak_frames, nak_frames);
            qatomic_inc(&trial_nak_ticks);
        }
        timer_mod_anticipate_ns(idle->timer, alarm);
    }
    return;

slow:
    if (qatomic_cmpxchg(&idle->phase, phase, phase | IDLE_SLOW) == phase) {
        /* The virtual-clock stop operation waits for this timer under BQL. */
        qemu_bh_schedule(idle->fallback);
    }
}

static bool uhci_idle_arm(UHCIState *s, int64_t alarm)
{
    UHCIIdle *idle = s->idle;
    UHCIQueue *queue = QTAILQ_FIRST(&s->queues);
    uint64_t age = 0;

    if (!idle || !(s->cmd & UHCI_CMD_RS) || !s->frame_bandwidth ||
        migration_incoming_postcopy_advised() || idle->epoch == UINT32_MAX) {
        return false;
    }
    if (queue) {
        if (QTAILQ_NEXT(queue, next) || !QTAILQ_EMPTY(&queue->asyncs) ||
            USB_DEVICE_GET_CLASS(queue->ep->dev)->ep_stopped) {
            return false;
        }
        age = queue->valid;
        qatomic_set(&idle->queue_addr, queue->qh_addr);
        qatomic_set(&idle->queue_token, queue->token);
        qatomic_set(&idle->device_addr, queue->ep->dev->addr);
    }
    idle->nak_ep = trial_enable_nak && queue && usb_tablet_can_nak(queue->ep)
        ? queue->ep : NULL;
    qatomic_set(&idle->tablet_nak, idle->nak_ep != NULL);
    qatomic_set(&idle->first_alarm, alarm);
    qatomic_set(&idle->first_expire, s->expire_time);
    qatomic_set(&idle->first_frnum, s->frnum);
    qatomic_set(&idle->base, s->fl_base_addr);
    idle->epoch++;
    qatomic_store_release(&idle->phase, ((uint64_t)idle->epoch << 32) | age);
    timer_mod_anticipate_ns(idle->timer, alarm);
    return true;
}

static void uhci_idle_realize(UHCIState *s)
{
    const char *enable = getenv("LATS_UHCI_IDLE_WORKER");
    g_autofree char *name = NULL;
    Error *err = NULL;
    IOThread *thread;
    UHCIIdle *idle;

    if (!enable || strcmp(enable, qtest_enabled() ? "qtest" : "1") ||
        icount_enabled() || replay_mode != REPLAY_MODE_NONE || s->masterbus ||
        !object_dynamic_cast(OBJECT(s), TYPE_PIIX3_USB_UHCI)) {
        return;
    }
    name = g_strdup_printf("uhci-%p", s);
    thread = iothread_create(name, &err);
    if (!thread) {
        error_report_err(err);
        return;
    }
    idle = s->idle = g_new0(UHCIIdle, 1);
    idle->owner = s;
    idle->thread = thread;
    idle->fallback = qemu_bh_new_guarded(uhci_idle_fallback, s,
                                        &DEVICE(s)->mem_reentrancy_guard);
    idle->timer = aio_timer_new(iothread_get_aio_context(thread),
                               QEMU_CLOCK_VIRTUAL, SCALE_NS, uhci_idle_tick, s);
    idle->dma_listener.begin = uhci_idle_dma_begin;
    idle->dma_listener.name = "uhci-idle-dma";
    memory_listener_register(&idle->dma_listener,
                             pci_get_address_space(&s->dev));
}

static void uhci_idle_exit(UHCIState *s)
{
    UHCIIdle *idle = s->idle;

    if (idle) {
        uhci_idle_take(s);
        memory_listener_unregister(&idle->dma_listener);
        iothread_stop(idle->thread);
        timer_free(idle->timer);
        qemu_bh_delete(idle->fallback);
        iothread_destroy(idle->thread);
        g_free(idle);
        s->idle = NULL;
    }
}

#else

static void uhci_endpoint_wakeup(USBBus *bus, USBEndpoint *ep, unsigned stream)
{
}

static void uhci_idle_revoke(UHCIState *s)
{
}

static uint16_t uhci_idle_frnum(UHCIState *s)
{
    return s->frnum;
}

static bool uhci_idle_arm(UHCIState *s, int64_t alarm)
{
    return false;
}

static void uhci_idle_realize(UHCIState *s)
{
}

static void uhci_idle_exit(UHCIState *s)
{
}

#endif

static void uhci_frame_timer(void *opaque)
{
    UHCIState *s = opaque;
    uint64_t t_now, t_last_run;
    int i, frames;
    const uint64_t frame_t = NANOSECONDS_PER_SECOND / FRAME_TIMER_FREQ;

    g_assert(qemu_mutex_iothread_locked());
    trial_locks++;
    trial_active = true;
    trial_work = QTAILQ_EMPTY(&s->queues) ? 0 : TRIAL_QUEUES;
    s->completions_only = false;
    qemu_bh_cancel(s->bh);

    if (!(s->cmd & UHCI_CMD_RS)) {
        /* Full stop */
        trace_usb_uhci_schedule_stop();
        timer_del(s->frame_timer);
        uhci_async_cancel_all(s);
        /* set hchalted bit in status - UHCI11D 2.1.2 */
        s->status |= UHCI_STS_HCHALTED;
        trial_work |= TRIAL_STOP;
        trial_uhci_end();
        return;
    }

    /* We still store expire_time in our state, for migration */
    t_last_run = s->expire_time - frame_t;
    t_now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* Process up to MAX_FRAMES_PER_TICK frames */
    frames = (t_now - t_last_run) / frame_t;
    if (frames > s->maxframes) {
        int skipped = frames - s->maxframes;
        s->expire_time += skipped * frame_t;
        s->frnum = (s->frnum + skipped) & 0x7ff;
        frames -= skipped;
    }
    if (frames > MAX_FRAMES_PER_TICK) {
        frames = MAX_FRAMES_PER_TICK;
    }

    trial_frames += frames;
    if (!frames) {
        trial_work |= TRIAL_NO_FRAME;
    }
    for (i = 0; i < frames; i++) {
        s->frame_bytes = 0;
        trace_usb_uhci_frame_start(s->frnum);
        uhci_async_validate_begin(s);
        uhci_process_frame(s);
        uhci_async_validate_end(s);
        /* The spec says frnum is the frame currently being processed, and
         * the guest must look at frnum - 1 on interrupt, so inc frnum now */
        s->frnum = (s->frnum + 1) & 0x7ff;
        s->expire_time += frame_t;
    }

    /* Complete the previous frame(s) */
    if (s->pending_int_mask) {
        trial_work |= TRIAL_IRQ;
        s->status2 |= s->pending_int_mask;
        s->status  |= UHCI_STS_USBINT;
        uhci_update_irq(s);
    }
    s->pending_int_mask = 0;

    if (!uhci_idle_arm(s, t_now + frame_t)) {
        timer_mod(s->frame_timer, t_now + frame_t);
    }
    trial_uhci_end();
}

static const MemoryRegionOps uhci_ioport_ops = {
    .read  = uhci_port_read,
    .write = uhci_port_write,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 2,
    .impl.max_access_size = 2,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static USBPortOps uhci_port_ops = {
    .attach = uhci_attach,
    .detach = uhci_detach,
    .child_detach = uhci_child_detach,
    .wakeup = uhci_wakeup,
    .complete = uhci_async_complete,
};

static USBBusOps uhci_bus_ops = {
    .wakeup_endpoint = uhci_endpoint_wakeup,
};

void usb_uhci_common_realize(PCIDevice *dev, Error **errp)
{
    Error *err = NULL;
    UHCIPCIDeviceClass *u = UHCI_GET_CLASS(dev);
    UHCIState *s = UHCI(dev);
    uint8_t *pci_conf = s->dev.config;
    int i;

    pci_conf[PCI_CLASS_PROG] = 0x00;
    /* TODO: reset value should be 0. */
    pci_conf[USB_SBRN] = USB_RELEASE_1; /* release number */
    pci_config_set_interrupt_pin(pci_conf, u->info.irq_pin + 1);
    s->irq = pci_allocate_irq(dev);

    if (s->masterbus) {
        USBPort *ports[NB_PORTS];
        for(i = 0; i < NB_PORTS; i++) {
            ports[i] = &s->ports[i].port;
        }
        usb_register_companion(s->masterbus, ports, NB_PORTS,
                               s->firstport, s, &uhci_port_ops,
                               USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL,
                               &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }
    } else {
        usb_bus_new(&s->bus, sizeof(s->bus), &uhci_bus_ops, DEVICE(dev));
        for (i = 0; i < NB_PORTS; i++) {
            usb_register_port(&s->bus, &s->ports[i].port, s, i, &uhci_port_ops,
                              USB_SPEED_MASK_LOW | USB_SPEED_MASK_FULL);
        }
    }
    s->bh = qemu_bh_new_guarded(uhci_bh, s, &DEVICE(dev)->mem_reentrancy_guard);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, uhci_frame_timer, s);
    static bool trial_installed;
    if (!trial_installed) {
        trial_enable_nak = lats_nobql_enabled();
        trial_installed = true;
        qemu_add_vm_change_state_handler(trial_uhci_vmstate, NULL);
        atexit(trial_uhci_exit);
    }
    s->num_ports_vmstate = NB_PORTS;
    QTAILQ_INIT(&s->queues);
    uhci_idle_realize(s);

    memory_region_init_io(&s->io_bar, OBJECT(s), &uhci_ioport_ops, s,
                          "uhci", 0x20);

    /* Use region 4 for consistency with real hardware.  BSD guests seem
       to rely on this.  */
    pci_register_bar(&s->dev, 4, PCI_BASE_ADDRESS_SPACE_IO, &s->io_bar);
}

static void usb_uhci_exit(PCIDevice *dev)
{
    UHCIState *s = UHCI(dev);

    uhci_idle_exit(s);
    trace_usb_uhci_exit();

    if (s->frame_timer) {
        timer_free(s->frame_timer);
        s->frame_timer = NULL;
    }

    if (s->bh) {
        qemu_bh_delete(s->bh);
    }

    uhci_async_cancel_all(s);

    if (!s->masterbus) {
        usb_bus_release(&s->bus);
    }
}

static Property uhci_properties_companion[] = {
    DEFINE_PROP_STRING("masterbus", UHCIState, masterbus),
    DEFINE_PROP_UINT32("firstport", UHCIState, firstport, 0),
    DEFINE_PROP_UINT32("bandwidth", UHCIState, frame_bandwidth, 1280),
    DEFINE_PROP_UINT32("maxframes", UHCIState, maxframes, 128),
    DEFINE_PROP_END_OF_LIST(),
};
static Property uhci_properties_standalone[] = {
    DEFINE_PROP_UINT32("bandwidth", UHCIState, frame_bandwidth, 1280),
    DEFINE_PROP_UINT32("maxframes", UHCIState, maxframes, 128),
    DEFINE_PROP_END_OF_LIST(),
};

static void uhci_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->class_id  = PCI_CLASS_SERIAL_USB;
    dc->vmsd = &vmstate_uhci;
    dc->reset = uhci_reset;
    set_bit(DEVICE_CATEGORY_USB, dc->categories);
}

static const TypeInfo uhci_pci_type_info = {
    .name = TYPE_UHCI,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(UHCIState),
    .class_size    = sizeof(UHCIPCIDeviceClass),
    .abstract = true,
    .class_init = uhci_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

void uhci_data_class_init(ObjectClass *klass, void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    UHCIPCIDeviceClass *u = UHCI_CLASS(klass);
    UHCIInfo *info = data;

    k->realize = info->realize ? info->realize : usb_uhci_common_realize;
    k->exit = info->unplug ? usb_uhci_exit : NULL;
    k->vendor_id = info->vendor_id;
    k->device_id = info->device_id;
    k->revision  = info->revision;
    if (!info->unplug) {
        /* uhci controllers in companion setups can't be hotplugged */
        dc->hotpluggable = false;
        device_class_set_props(dc, uhci_properties_companion);
    } else {
        device_class_set_props(dc, uhci_properties_standalone);
    }
    if (info->notuser) {
        dc->user_creatable = false;
    }
    u->info = *info;
}

static UHCIInfo uhci_info[] = {
    {
        .name      = TYPE_PIIX3_USB_UHCI,
        .vendor_id = PCI_VENDOR_ID_INTEL,
        .device_id = PCI_DEVICE_ID_INTEL_82371SB_2,
        .revision  = 0x01,
        .irq_pin   = 3,
        .unplug    = true,
    },{
        .name      = TYPE_PIIX4_USB_UHCI,
        .vendor_id = PCI_VENDOR_ID_INTEL,
        .device_id = PCI_DEVICE_ID_INTEL_82371AB_2,
        .revision  = 0x01,
        .irq_pin   = 3,
        .unplug    = true,
    },{
        .name      = TYPE_ICH9_USB_UHCI(1), /* 00:1d.0 */
        .vendor_id = PCI_VENDOR_ID_INTEL,
        .device_id = PCI_DEVICE_ID_INTEL_82801I_UHCI1,
        .revision  = 0x03,
        .irq_pin   = 0,
        .unplug    = false,
    },{
        .name      = TYPE_ICH9_USB_UHCI(2), /* 00:1d.1 */
        .vendor_id = PCI_VENDOR_ID_INTEL,
        .device_id = PCI_DEVICE_ID_INTEL_82801I_UHCI2,
        .revision  = 0x03,
        .irq_pin   = 1,
        .unplug    = false,
    },{
        .name      = TYPE_ICH9_USB_UHCI(3), /* 00:1d.2 */
        .vendor_id = PCI_VENDOR_ID_INTEL,
        .device_id = PCI_DEVICE_ID_INTEL_82801I_UHCI3,
        .revision  = 0x03,
        .irq_pin   = 2,
        .unplug    = false,
    },{
        .name      = TYPE_ICH9_USB_UHCI(4), /* 00:1a.0 */
        .vendor_id = PCI_VENDOR_ID_INTEL,
        .device_id = PCI_DEVICE_ID_INTEL_82801I_UHCI4,
        .revision  = 0x03,
        .irq_pin   = 0,
        .unplug    = false,
    },{
        .name      = TYPE_ICH9_USB_UHCI(5), /* 00:1a.1 */
        .vendor_id = PCI_VENDOR_ID_INTEL,
        .device_id = PCI_DEVICE_ID_INTEL_82801I_UHCI5,
        .revision  = 0x03,
        .irq_pin   = 1,
        .unplug    = false,
    },{
        .name      = TYPE_ICH9_USB_UHCI(6), /* 00:1a.2 */
        .vendor_id = PCI_VENDOR_ID_INTEL,
        .device_id = PCI_DEVICE_ID_INTEL_82801I_UHCI6,
        .revision  = 0x03,
        .irq_pin   = 2,
        .unplug    = false,
    }
};

static void uhci_register_types(void)
{
    TypeInfo uhci_type_info = {
        .parent        = TYPE_UHCI,
        .class_init    = uhci_data_class_init,
    };
    int i;

    type_register_static(&uhci_pci_type_info);

    for (i = 0; i < ARRAY_SIZE(uhci_info); i++) {
        uhci_type_info.name = uhci_info[i].name;
        uhci_type_info.class_data = uhci_info + i;
        type_register(&uhci_type_info);
    }
}

type_init(uhci_register_types)
