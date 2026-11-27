/*
 * Interface for configuring and controlling the state of tracing events.
 *
 * Copyright (C) 2014-2017 Lluís Vilanova <vilanova@ac.upc.edu>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/notify.h"
#include "cpu.h"
#include "trace/trace-root.h"
#include "trace/control.h"


static NotifierList enable_notifiers =
    NOTIFIER_LIST_INITIALIZER(enable_notifiers);
static GMutex state_lock;

void trace_add_enable_notifier(Notifier *notifier)
{
    g_mutex_lock(&state_lock);
    notifier_list_add(&enable_notifiers, notifier);
    g_mutex_unlock(&state_lock);
}

void trace_remove_enable_notifier(Notifier *notifier)
{
    g_mutex_lock(&state_lock);
    notifier_remove(notifier);
    g_mutex_unlock(&state_lock);
}

void trace_event_set_state_dynamic_init(TraceEvent *ev, bool state)
{
    trace_event_set_state_dynamic(ev, state);
}

void trace_event_set_state_dynamic(TraceEvent *ev, bool state)
{
    assert(trace_event_get_state_static(ev));
    g_mutex_lock(&state_lock);
    if (qatomic_read(ev->dstate) != state) {
        if (state) {
            qatomic_inc(&trace_events_enabled_count);
            qatomic_set(ev->dstate, 1);
            notifier_list_notify(&enable_notifiers, ev);
        } else {
            qatomic_set(ev->dstate, 0);
            qatomic_dec(&trace_events_enabled_count);
        }
    }
    g_mutex_unlock(&state_lock);
}
