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
#ifndef QEMU_NET_SLIRP_H
#define QEMU_NET_SLIRP_H


#include "qemu/thread.h"
#include "qemu/event_notifier.h"

#ifdef CONFIG_SLIRP

/*
 * The receiver runs with the backend mutex held. A return of zero keeps
 * the packet for the normal main-loop receive queue.
 */
typedef ssize_t SlirpDirectReceive(void *opaque, const uint8_t *data,
                                   size_t size);

bool net_slirp_attach_receiver(NetClientState *nc,
                               SlirpDirectReceive *receive,
                               void (*detach)(void *), void *opaque,
                               QemuRecMutex **mutex);
void net_slirp_detach_receiver(NetClientState *nc);
void net_slirp_receiver_ready(NetClientState *nc);
/* The caller holds the attached NIC/backend mutex. */
void net_slirp_set_rx_notifier(NetClientState *nc, EventNotifier *notifier,
                              void (*flush)(void *));
typedef enum SlirpKickQueue {
    SLIRP_KICK_TX,
    SLIRP_KICK_CTRL,
    SLIRP_KICK_COUNT,
} SlirpKickQueue;

void net_slirp_set_kick_notifier(NetClientState *nc, SlirpKickQueue queue,
                                 EventNotifier *notifier, void (*kick)(void *));
ssize_t net_slirp_transmit(NetClientState *nc, const struct iovec *iov,
                           int iovcnt);

void hmp_hostfwd_add(Monitor *mon, const QDict *qdict);
void hmp_hostfwd_remove(Monitor *mon, const QDict *qdict);

void hmp_info_usernet(Monitor *mon, const QDict *qdict);

#endif

#endif /* QEMU_NET_SLIRP_H */
