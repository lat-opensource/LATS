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

#include "qemu/osdep.h"
#include "hw/i386/hpet-irq0.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/replay.h"
#include "qemu/main-loop.h"
#include "block/aio.h"
#include "block/thread-pool.h"
#include "qemu/error-report.h"
#include "qemu/queue.h"
#include "qom/object.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifndef _WIN32

/* If we have signalfd, we mask out the signals we want to handle and then
 * use signalfd to listen for them.  We rely on whatever the current signal
 * handler is to dispatch the signals when we receive them.
 */
/*
 * Disable CFI checks.
 * We are going to call a signal hander directly. Such handler may or may not
 * have been defined in our binary, so there's no guarantee that the pointer
 * used to set the handler is a cfi-valid pointer. Since the handlers are
 * stored in kernel memory, changing the handler to an attacker-defined
 * function requires being able to call a sigaction() syscall,
 * which is not as easy as overwriting a pointer in memory.
 */
QEMU_DISABLE_CFI
static void sigfd_handler(void *opaque)
{
    int fd = (intptr_t)opaque;
    struct qemu_signalfd_siginfo info;
    struct sigaction action;
    ssize_t len;

    while (1) {
        len = RETRY_ON_EINTR(read(fd, &info, sizeof(info)));

        if (len == -1 && errno == EAGAIN) {
            break;
        }

        if (len != sizeof(info)) {
            error_report("read from sigfd returned %zd: %s", len,
                         g_strerror(errno));
            return;
        }

        sigaction(info.ssi_signo, NULL, &action);
        if ((action.sa_flags & SA_SIGINFO) && action.sa_sigaction) {
            sigaction_invoke(&action, &info);
        } else if (action.sa_handler) {
            action.sa_handler(info.ssi_signo);
        }
    }
}

static int qemu_signal_init(Error **errp)
{
    int sigfd;
    sigset_t set;

    /*
     * SIG_IPI must be blocked in the main thread and must not be caught
     * by sigwait() in the signal thread. Otherwise, the cpu thread will
     * not catch it reliably.
     */
    sigemptyset(&set);
    sigaddset(&set, SIG_IPI);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGALRM);
    sigaddset(&set, SIGBUS);
    /* SIGINT cannot be handled via signalfd, so that ^C can be used
     * to interrupt QEMU when it is being run under gdb.  SIGHUP and
     * SIGTERM are also handled asynchronously, even though it is not
     * strictly necessary, because they use the same handler as SIGINT.
     */
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    sigdelset(&set, SIG_IPI);
    sigfd = qemu_signalfd(&set);
    if (sigfd == -1) {
        error_setg_errno(errp, errno, "failed to create signalfd");
        return -errno;
    }

    g_unix_set_fd_nonblocking(sigfd, true, NULL);

    qemu_set_fd_handler(sigfd, sigfd_handler, NULL, (void *)(intptr_t)sigfd);

    return 0;
}

#else /* _WIN32 */

static int qemu_signal_init(Error **errp)
{
    return 0;
}
#endif

static AioContext *qemu_aio_context;
static QEMUBH *qemu_notify_bh;
static QEMUBH *main_timer_notify_bh;
static QEMUBH *main_context_notify_bh;
static bool main_context_notify_pending;
static __thread bool running_main_timers;
static bool main_timer_notify_pending;
#ifndef _WIN32
static EventNotifier deadline_event;
static bool deadline_repoll_enabled;
static bool serial_rx_poll_enabled;
static bool deadline_hpet_unlocked;
static bool deadline_hpet_repoll;
static unsigned deadline_hpet_repoll_count;
static bool deadline_hpet_wait;
static bool deadline_hpet_chain;
static unsigned deadline_hpet_chain_limit;
static bool deadline_timer_nobql;
static unsigned deadline_nobql_dispatched;
/* Odd while the top-level wait accepts timer wakeups through deadline_event. */
static unsigned deadline_poll_epoch;
static unsigned deadline_poll_skipped;
static unsigned deadline_late_timer_updates;
static unsigned deadline_eintr_retried;
#endif
static __thread bool poll_needs_prepare;
static NotifierList main_loop_poll_notifiers =
    NOTIFIER_LIST_INITIALIZER(main_loop_poll_notifiers);

void qemu_poll_needs_prepare(void)
{
    poll_needs_prepare = true;
}

static bool trial_cancel_main_timer_wakes;
static uint64_t trial_local_timer_wakes, trial_cancelled_timer_wakes;

static __thread bool trial_glib_dispatch;
static __thread unsigned trial_activity;

void trial_poll_aio_activity(unsigned activity);
void trial_poll_aio_activity(unsigned activity)
{
    if (trial_glib_dispatch) {
        trial_activity |= activity;
    }
}

static struct {
    uintptr_t pc;
    unsigned kind;
    unsigned count;
} trial_sources[128];

static void trial_count_source(unsigned kind, uintptr_t pc)
{
    /* The PC is the key. The same callback site always supplies the kind. */
    for (unsigned i = 0; i < ARRAY_SIZE(trial_sources); i++) {
        uintptr_t old = qatomic_read(&trial_sources[i].pc);
        if (!old) {
            old = qatomic_cmpxchg(&trial_sources[i].pc, 0, pc);
            if (!old) {
                qatomic_set(&trial_sources[i].kind, kind);
            }
        }
        if (!old || old == pc) {
            qatomic_inc(&trial_sources[i].count);
            return;
        }
    }
}

void trial_main_callback(unsigned kind, uintptr_t pc);
void trial_main_callback(unsigned kind, uintptr_t pc)
{
    if (trial_glib_dispatch) {
        trial_count_source(kind, pc);
    }
}

static void notify_event_cb(void *opaque)
{
    /* No need to do anything; this bottom half is only used to
     * kick the kernel out of ppoll/poll/WaitForMultipleObjects.
     */
    trial_poll_aio_activity(1u);
}

AioContext *qemu_get_aio_context(void)
{
    return qemu_aio_context;
}

void qemu_notify_event(void)
{
    if (!qemu_aio_context) {
        return;
    }
    trial_count_source(0, (uintptr_t)__builtin_return_address(0));
    qemu_bh_schedule(qemu_notify_bh);
}

void qemu_notify_event_for_main_context(void)
{
    if (!trial_glib_dispatch || !qemu_in_main_thread() ||
        icount_enabled() || replay_mode != REPLAY_MODE_NONE) {
        qemu_notify_event();
        return;
    }

    /* Keep a wake available to any nested AioContext wait. */
    main_context_notify_pending = true;
    qemu_bh_schedule_no_notify(main_context_notify_bh);
}

static void main_timer_notify_bh_cb(void *opaque)
{
    trial_poll_aio_activity(1u);
    main_timer_notify_pending = false;
}

static void main_loop_timer_notify(void *opaque, QEMUClockType type)
{
    if (running_main_timers && !icount_enabled() &&
        replay_mode == REPLAY_MODE_NONE) {
        ++trial_local_timer_wakes;
        main_timer_notify_pending = true;
        qemu_bh_schedule(main_timer_notify_bh);
    } else {
#ifndef _WIN32
        unsigned epoch = qatomic_read(&deadline_poll_epoch);

        if (epoch & 1) {
            event_notifier_set(&deadline_event);
            /* A late producer must still wake an intervening nested wait. */
            smp_mb();
            if (qatomic_read(&deadline_poll_epoch) == epoch) {
                return;
            }
        }
#endif
        qemu_timer_notify_cb(opaque, type);
    }
}

void main_loop_mod_virtual_timer_ns(QEMUTimer *ts, int64_t expire_time)
{
#ifndef _WIN32
    unsigned epoch = qatomic_read(&deadline_poll_epoch);

    if ((epoch & 1) && ts->timer_list == main_loop_tlg.tl[QEMU_CLOCK_VIRTUAL] &&
        timer_mod_ns_late_head(ts, expire_time)) {
        /* The current top-level wait will sample the new time or wake earlier. */
        smp_mb();
        if (qatomic_read(&deadline_poll_epoch) == epoch) {
            qatomic_inc(&deadline_late_timer_updates);
            return;
        }

        /* Preserve the original wake after a change of poll context. */
        timerlist_notify(ts->timer_list);
        return;
    }
#endif
    timer_mod_ns(ts, expire_time);
}

static GArray *gpollfds;

int qemu_init_main_loop(Error **errp)
{
    int ret;
    GSource *src;

    bool nobql = lats_nobql_enabled();

    trial_cancel_main_timer_wakes = nobql;
#ifndef _WIN32
    serial_rx_poll_enabled = nobql;
    deadline_repoll_enabled = nobql;
    deadline_timer_nobql = nobql;
    deadline_hpet_unlocked = x86_hpet_irq0_refresh && nobql;
    deadline_hpet_repoll = nobql;
    deadline_hpet_wait = deadline_hpet_repoll;
    deadline_hpet_chain = deadline_hpet_wait;
    if (deadline_repoll_enabled) {
        ret = event_notifier_init(&deadline_event, false);
        if (ret < 0) {
            error_setg_errno(errp, -ret, "cannot initialize timer poll event");
            return ret;
        }
    }
#endif
    init_clocks(main_loop_timer_notify);

    ret = qemu_signal_init(errp);
    if (ret) {
        return ret;
    }

    qemu_aio_context = aio_context_new(errp);
    if (!qemu_aio_context) {
        return -EMFILE;
    }
    qemu_set_current_aio_context(qemu_aio_context);
    qemu_notify_bh = qemu_bh_new(notify_event_cb, NULL);
    main_timer_notify_bh = qemu_bh_new(main_timer_notify_bh_cb, NULL);
    main_context_notify_bh = qemu_bh_new(notify_event_cb, NULL);
    gpollfds = g_array_new(FALSE, FALSE, sizeof(GPollFD));
    src = aio_get_g_source(qemu_aio_context);
    g_source_set_name(src, "aio-context");
    g_source_attach(src, NULL);
    g_source_unref(src);
    src = iohandler_get_g_source();
    g_source_set_name(src, "io-handler");
    g_source_attach(src, NULL);
    g_source_unref(src);
    return 0;
}

static void main_loop_update_params(EventLoopBase *base, Error **errp)
{
    ERRP_GUARD();

    if (!qemu_aio_context) {
        error_setg(errp, "qemu aio context not ready");
        return;
    }

    aio_context_set_aio_params(qemu_aio_context, base->aio_max_batch, errp);
    if (*errp) {
        return;
    }

    aio_context_set_thread_pool_params(qemu_aio_context, base->thread_pool_min,
                                       base->thread_pool_max, errp);
}

MainLoop *mloop;

static void main_loop_init(EventLoopBase *base, Error **errp)
{
    MainLoop *m = MAIN_LOOP(base);

    if (mloop) {
        error_setg(errp, "only one main-loop instance allowed");
        return;
    }

    main_loop_update_params(base, errp);

    mloop = m;
    return;
}

static bool main_loop_can_be_deleted(EventLoopBase *base)
{
    return false;
}

static void main_loop_class_init(ObjectClass *oc, void *class_data)
{
    EventLoopBaseClass *bc = EVENT_LOOP_BASE_CLASS(oc);

    bc->init = main_loop_init;
    bc->update_params = main_loop_update_params;
    bc->can_be_deleted = main_loop_can_be_deleted;
}

static const TypeInfo main_loop_info = {
    .name = TYPE_MAIN_LOOP,
    .parent = TYPE_EVENT_LOOP_BASE,
    .class_init = main_loop_class_init,
    .instance_size = sizeof(MainLoop),
};

static void main_loop_register_types(void)
{
    type_register_static(&main_loop_info);
}

type_init(main_loop_register_types)

/* The main thread is the only writer to the totals. Timers in worker
 * threads never participate in a main-loop poll round. */
#define TRIAL_TIMER_SLOTS 63
static uintptr_t trial_timer_names[TRIAL_TIMER_SLOTS];
static unsigned trial_used_names;
static __thread bool trial_taken;
static __thread bool trial_glib;
static __thread uint64_t trial_timer_mask;
static uint64_t trial_main_total, trial_unstored;
static struct {
    uint64_t timers, count;
    bool glib;
    unsigned activity;
} trial_groups[2048];
static unsigned trial_used_groups;

void trial_timer_in_poll(uintptr_t pc);
void trial_timer_in_poll(uintptr_t pc)
{
    unsigned i;

    if (!trial_taken) {
        return;
    }
    for (i = 0; i < trial_used_names; i++) {
        if (trial_timer_names[i] == pc) {
            break;
        }
    }
    if (i == trial_used_names && i < TRIAL_TIMER_SLOTS) {
        trial_timer_names[trial_used_names++] = pc;
    }
    trial_timer_mask |= UINT64_C(1) << i;
}

static void trial_record_poll(void)
{
    unsigned i;

    if (!trial_taken) {
        return;
    }
    ++trial_main_total;
    for (i = 0; i < trial_used_groups; i++) {
        if (trial_groups[i].timers == trial_timer_mask &&
            trial_groups[i].glib == trial_glib &&
            trial_groups[i].activity == trial_activity) {
            ++trial_groups[i].count;
            return;
        }
    }
    if (i == ARRAY_SIZE(trial_groups)) {
        ++trial_unstored;
        return;
    }
    trial_groups[i].timers = trial_timer_mask;
    trial_groups[i].glib = trial_glib;
    trial_groups[i].activity = trial_activity;
    trial_groups[i].count = 1;
    ++trial_used_groups;
}

void trial_vnc_compare_census(const char *phase) __attribute__((weak));
void trial_report_polls(const char *phase);
void trial_report_polls(const char *phase)
{
    if (trial_vnc_compare_census) {
        trial_vnc_compare_census(phase);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(trial_sources); i++) {
        uintptr_t pc = qatomic_read(&trial_sources[i].pc);
        if (pc) {
            fprintf(stderr, "MAIN_SOURCE %s %u %" PRIxPTR " %u\n",
                    phase, qatomic_read(&trial_sources[i].kind), pc,
                    qatomic_read(&trial_sources[i].count));
        }
    }
    fprintf(stderr, "TIMER_SELF_WAKE %s candidates=%" PRIu64
                    " skipped=%" PRIu64 "\n",
            phase, trial_local_timer_wakes, trial_cancelled_timer_wakes);
#ifndef _WIN32
    fprintf(stderr, "DEADLINE_REPOLL %s skipped=%u\n",
            phase, deadline_poll_skipped);
    if (qatomic_read(&deadline_late_timer_updates)) {
        fprintf(stderr, "HPET_LATE_REARM %s skipped=%u\n", phase,
                qatomic_read(&deadline_late_timer_updates));
    }
    fprintf(stderr, "HPET_POST_REPOLL %s skipped=%u\n",
            phase, deadline_hpet_repoll_count);
    fprintf(stderr, "HPET_CHAIN_LIMIT %s count=%u\n",
            phase, deadline_hpet_chain_limit);
    if (deadline_nobql_dispatched) {
        fprintf(stderr, "TIMER_NOBQL_DISPATCH %s count=%u\n",
                phase, deadline_nobql_dispatched);
    }
    if (deadline_eintr_retried) {
        fprintf(stderr, "DEADLINE_EINTR_RETRY %s count=%u\n",
                phase, deadline_eintr_retried);
    }
#endif
    fprintf(stderr, "POLL_ROUNDS %s total=%" PRIu64
                    " unstored=%" PRIu64 " inflight=%u\n",
            phase, trial_main_total + trial_taken, trial_unstored, trial_taken);
    for (unsigned i = 0; i < trial_used_names; i++) {
        fprintf(stderr, "POLL_CALLBACK %s %u %" PRIxPTR "\n",
                phase, i, trial_timer_names[i]);
    }
    for (unsigned i = 0; i < trial_used_groups; i++) {
        fprintf(stderr, "POLL_GROUP %s mask=%016" PRIx64
                        " glib=%u count=%" PRIu64 "\n",
                phase, trial_groups[i].timers,
                trial_groups[i].glib, trial_groups[i].count);
        fprintf(stderr, "POLL_AIO %s timer=%u glib=%u activity=%x count=%" PRIu64 "\n",
                phase, trial_groups[i].timers != 0, trial_groups[i].glib,
                trial_groups[i].activity, trial_groups[i].count);
    }
    if (trial_taken) {
        fprintf(stderr, "POLL_GROUP %s mask=%016" PRIx64 " glib=%u count=1\n",
                phase, trial_timer_mask, trial_glib);
        fprintf(stderr, "POLL_AIO %s timer=%u glib=%u activity=%x count=1\n",
                phase, trial_timer_mask != 0, trial_glib, trial_activity);
    }
}

static int max_priority;

#ifndef _WIN32
static int glib_pollfds_idx;
static int glib_n_poll_fds;

static void glib_pollfds_query(GMainContext *context, int64_t *cur_timeout)
{
    int timeout = 0;
    int64_t timeout_ns;
    int n, capacity;

    glib_pollfds_idx = gpollfds->len;
    n = glib_n_poll_fds;
    do {
        GPollFD *pfds = NULL;

        capacity = n;
        g_array_set_size(gpollfds, glib_pollfds_idx + capacity);
        if (capacity) {
            pfds = &g_array_index(gpollfds, GPollFD, glib_pollfds_idx);
        }
        n = g_main_context_query(context, max_priority, &timeout, pfds,
                                 capacity);
    } while (n > capacity);
    glib_n_poll_fds = n;
    if (n < capacity) {
        g_array_set_size(gpollfds, glib_pollfds_idx + n);
    }

    if (timeout < 0) {
        timeout_ns = -1;
    } else {
        timeout_ns = (int64_t)timeout * (int64_t)SCALE_MS;
    }

    *cur_timeout = qemu_soonest_timeout(timeout_ns, *cur_timeout);
}

static void glib_pollfds_poll(void)
{
    GMainContext *context = g_main_context_default();
    GPollFD *pfds = &g_array_index(gpollfds, GPollFD, glib_pollfds_idx);

    if (g_main_context_check(context, max_priority, pfds, glib_n_poll_fds)) {
        bool nested = trial_glib_dispatch;

        trial_glib = true;
        trial_glib_dispatch = true;
        g_main_context_dispatch(context);
        trial_glib_dispatch = nested;
    }
}

#define MAX_MAIN_LOOP_SPIN (1000)

void *trial_vnc_request_poll_begin(int *fd) __attribute__((weak));
bool trial_vnc_request_poll_read(void *opaque, int fd) __attribute__((weak));

static int main_loop_poll_fds(GPollFD *fds, guint count, int64_t timeout,
                              void *vnc, int vnc_fd, int64_t *last_timeout)
{
    int64_t start;
    int64_t remaining = timeout;

    if (!vnc || timeout == 0) {
        *last_timeout = timeout;
        return qemu_poll_ns(fds, count, timeout);
    }

    start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    for (;;) {
        int ret = qemu_poll_ns(fds, count, remaining);
        int vnc_idx = -1;
        bool deadline_only_other = false;
        bool consumed = false;

        *last_timeout = remaining;
        if (ret < 1 || ret > 2 || remaining == 0) {
            return ret;
        }
        if (ret == 2) {
            int deadline_fd = event_notifier_get_fd(&deadline_event);
            if (count > (guint)(glib_pollfds_idx + glib_n_poll_fds) &&
                fds[count - 1].fd == deadline_fd &&
                fds[count - 1].revents == G_IO_IN) {
                deadline_only_other = true;
            } else {
                return ret;
            }
        }

        for (int i = glib_pollfds_idx; i < glib_pollfds_idx + glib_n_poll_fds;
             i++) {
            if (fds[i].fd == vnc_fd && fds[i].revents == G_IO_IN &&
                !(fds[i].events & G_IO_OUT)) {
                vnc_idx = i;
                consumed = trial_vnc_request_poll_read(vnc, vnc_fd);
                break;
            }
        }
        if (!consumed) {
            return ret;
        }
        if (deadline_only_other) {
            fds[vnc_idx].revents = 0;
            return 1;
        }

        /* Check every fd again, with the original deadline and priority. */
        if (timeout >= 0) {
            remaining = MAX(timeout -
                (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - start), 0);
        }
    }
}

static int64_t main_loop_hpet_deadline(int64_t timeout, int64_t clock_start,
                                       int64_t *now)
{
    int64_t timer = timerlistgroup_deadline_host_ns(&main_loop_tlg);
    int64_t external, remaining;

    *now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    external = timeout < 0 ? -1 : MAX(timeout - (*now - clock_start), 0);
    remaining = timer < 0 ? -1 : MAX(timer - *now, 0);
    return qemu_soonest_timeout(external, remaining);
}

void *virtio_serial_poll_rx_begin(void) __attribute__((weak));
bool virtio_serial_poll_rx_read(void *opaque, int fd) __attribute__((weak));
void virtio_serial_poll_rx_end(void *opaque) __attribute__((weak));

/* Return the next unhandled event, keeping the caller's entire poll set. */
static int main_loop_poll_serial_rx(void *serial_rx, int64_t budget,
                                    int64_t timeout, int64_t prepare_start,
                                    int64_t *last_timeout, void *vnc, int vnc_fd)
{
    GPollFD *fds = (GPollFD *)gpollfds->data;
    int64_t start = serial_rx && budget >= 0 ?
                    qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
    int64_t wait = budget;

    for (;;) {
        int64_t now, next;
        int i, ret;

        ret = main_loop_poll_fds(fds, gpollfds->len, wait, vnc, vnc_fd,
                                 last_timeout);
        if (!serial_rx || ret != 1 || *last_timeout == 0) {
            return ret;
        }
        for (i = glib_pollfds_idx; i < glib_pollfds_idx + glib_n_poll_fds; i++) {
            if (fds[i].revents == G_IO_IN && !(fds[i].events & G_IO_OUT)) {
                break;
            }
        }
        if (i == glib_pollfds_idx + glib_n_poll_fds) {
            return ret;
        }
        next = main_loop_hpet_deadline(timeout, prepare_start, &now);
        if (next == 0 || (budget >= 0 && now - start >= budget)) {
            return ret;
        }
        if (!virtio_serial_poll_rx_read(serial_rx, fds[i].fd)) {
            return ret;
        }

        /* The read can make a timer due or another fd ready. */
        next = main_loop_hpet_deadline(timeout, prepare_start, &now);
        wait = qemu_soonest_timeout(next, budget < 0 ? -1 :
                                    MAX(budget - (now - start), 0));
    }
}

static int main_loop_poll_deadlines(int64_t timeout, bool try_hpet,
                                    bool try_nobql,
                                    int64_t prepare_start, void *serial_rx,
                                    void *vnc, int vnc_fd)
{
    guint count = gpollfds->len;
    GPollFD wake = {
        .fd = event_notifier_get_fd(&deadline_event),
        .events = G_IO_IN,
    };
    GPollFD *fds;
    bool can_wait = (deadline_hpet_wait || try_nobql) && prepare_start >= 0;
    bool use_prepared_deadline = can_wait || serial_rx;
    bool allow_chain = deadline_hpet_chain || try_nobql;
    int64_t clock_start = use_prepared_deadline ? prepare_start :
                          qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t first_poll_start = clock_start;
    int64_t original_timeout;
    int64_t remaining, last_timeout;
    int ret;

    g_array_append_val(gpollfds, wake);
    fds = (GPollFD *)gpollfds->data;
    qatomic_inc(&deadline_poll_epoch);
    /* A producer that observed the previous epoch used the original wake. */
    smp_mb();
    if (use_prepared_deadline) {
        original_timeout = main_loop_hpet_deadline(timeout, clock_start,
                                                   &first_poll_start);
    } else {
        original_timeout = qemu_soonest_timeout(
            timeout, timerlistgroup_deadline_ns(&main_loop_tlg));
    }

    remaining = original_timeout;
    ret = main_loop_poll_serial_rx(serial_rx, remaining, timeout,
                                   prepare_start, &last_timeout,
                                   vnc, vnc_fd);
    for (unsigned eintr_budget = 8;
         eintr_budget && ret < 0 && errno == EINTR;
         --eintr_budget) {
        int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - first_poll_start;
        int64_t original_remaining = original_timeout < 0 ? -1 :
            MAX(original_timeout - elapsed, 0);

        remaining = qemu_soonest_timeout(
            original_remaining, timerlistgroup_deadline_ns(&main_loop_tlg));
        ++deadline_eintr_retried;
        if (remaining == 0) {
            ret = 0;
            break;
        }
        ret = main_loop_poll_serial_rx(serial_rx, remaining, timeout,
                                       prepare_start, &last_timeout,
                                       vnc, vnc_fd);
    }
    for (unsigned dl_budget = 64;
         dl_budget && ret == 1 && fds[count].revents == G_IO_IN;
         --dl_budget) {
        event_notifier_test_and_clear(&deadline_event);
        fds[count].revents = 0;
        if (original_timeout == 0) {
            ret = 0;
            break;
        }
        int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - first_poll_start;
        int64_t original_remaining = original_timeout < 0 ? -1 :
            MAX(original_timeout - elapsed, 0);

        /*
         * Reuse the GLib and poll-notifier fd arrays while honoring their
         * original deadline, including when the timer head moves multiple
         * times. A ready fd or expired timer takes the normal path.
         */
        remaining = qemu_soonest_timeout(
            original_remaining, timerlistgroup_deadline_ns(&main_loop_tlg));
        if (remaining != 0) {
            ++deadline_poll_skipped;
            ret = main_loop_poll_serial_rx(serial_rx, remaining, timeout,
                                           prepare_start, &last_timeout,
                                           vnc, vnc_fd);
        } else {
            ret = 0;
            break;
        }
    }

    /*
     * Check the queue and all external fds again between unlocked deliveries.
     * Return to the regular loop after a bounded number of deliveries.
     */
    unsigned hpet_budget = allow_chain ? 8 : 1;
    unsigned late_repoll_budget = can_wait ? 16 : 0;
    while ((try_hpet || try_nobql) && ret == 0 &&
           (timeout < 0 ||
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - clock_start < timeout)) {
        bool dispatched = false;

        if (hpet_budget) {
            if (try_hpet) {
                dispatched = qemu_clock_try_virtual_timer_unlocked();
            }
            if (!dispatched && try_nobql &&
                qemu_clock_try_nobql_timer_unlocked()) {
                dispatched = true;
                ++deadline_nobql_dispatched;
            }
        }

        if (!dispatched && late_repoll_budget) {
            /*
             * A virtual timer moved later via main_loop_mod_virtual_timer_ns()
             * does not wake deadline_event, so the poll can time out early
             * while no timer or external timeout is due. Re-poll with the
             * updated deadline instead of taking BQL for an empty timeout.
             * If the timer crossed expiry between the two clock reads, retry
             * unlocked dispatch once before falling back to BQL.
             */
            int64_t now;

            if (main_loop_hpet_deadline(timeout, clock_start, &now) == 0) {
                if (hpet_budget) {
                    if (try_hpet) {
                        dispatched = qemu_clock_try_virtual_timer_unlocked();
                    }
                    if (!dispatched && try_nobql &&
                        qemu_clock_try_nobql_timer_unlocked()) {
                        dispatched = true;
                        ++deadline_nobql_dispatched;
                    }
                }
                if (!dispatched) {
                    break;
                }
            } else {
                --late_repoll_budget;
            }
        } else if (!dispatched) {
            break;
        }

        if (dispatched) {
            /*
             * A NO_BQL timer callback may rearm itself via timer_mod(),
             * signalling deadline_event. Clear it before recomputing the
             * next deadline so we do not incur a spurious immediate wakeup.
             */
            event_notifier_test_and_clear(&deadline_event);
            smp_mb();
            if ((deadline_hpet_wait || try_nobql) && !can_wait) {
                /* A source needs a fresh prepare before another blocking poll. */
                break;
            }
            /* A timed sleep checked the complete fd set. Bound back-to-back
             * deliveries if the guest keeps programming an already due timer. */
            if (can_wait && last_timeout > 0) {
                hpet_budget = 8;
                late_repoll_budget = 16;
            }
            if (--hpet_budget == 0 && allow_chain) {
                ++deadline_hpet_chain_limit;
                break;
            }
        } else if (can_wait && last_timeout > 0) {
            late_repoll_budget = 16;
        }

        int64_t post_start;

        remaining = main_loop_hpet_deadline(timeout, clock_start, &post_start);

        ret = main_loop_poll_serial_rx(serial_rx, remaining, timeout,
                                       prepare_start, &last_timeout,
                                       vnc, vnc_fd);
        /*
         * The guest can rearm HPET multiple times in response to the IRQ we
         * just sent. Consume bounded isolated timer notifications in the same
         * poll epoch while keeping the original budget and all external fds.
         */
        for (unsigned post_dl_budget = 64;
             post_dl_budget && deadline_hpet_repoll && ret == 1 &&
             fds[count].revents == G_IO_IN;
             --post_dl_budget) {
            event_notifier_test_and_clear(&deadline_event);
            fds[count].revents = 0;
            if (remaining == 0) {
                ret = 0;
                break;
            }
            int64_t now;
            int64_t next = main_loop_hpet_deadline(timeout, clock_start, &now);
            int64_t budget = remaining < 0 ? -1 :
                MAX(remaining - (now - post_start), 0);

            remaining = qemu_soonest_timeout(budget, next);
            if (remaining > 0) {
                ++deadline_hpet_repoll_count;
                post_start = now;
                ret = main_loop_poll_serial_rx(serial_rx, remaining, timeout,
                                               prepare_start, &last_timeout,
                                               vnc, vnc_fd);
            } else {
                ret = 0;
                break;
            }
        }
    }

    qatomic_inc(&deadline_poll_epoch);
    smp_mb();
    /*
     * The main loop is already returning to run timers and recompute
     * deadlines; clearing deadline_event here avoids scheduling a redundant
     * notify_event_cb / aio_context_notifier_cb wakeup on the next poll.
     */
    event_notifier_test_and_clear(&deadline_event);
    g_array_set_size(gpollfds, count);
    return ret;
}

static int os_host_main_loop_wait(int64_t timeout, bool refresh_timers,
                                  bool repoll_deadline, bool repoll_serial)
{
    GMainContext *context = g_main_context_default();
    void *vnc = NULL;
    int vnc_fd = -1;
    int ret;
    bool try_hpet;
    bool try_nobql;
    int64_t prepare_start = -1;
    bool ready;
    void *serial_rx = NULL;

    g_main_context_acquire(context);

    /* Source prepare callbacks can access device state. */
    if ((((deadline_hpet_wait || deadline_timer_nobql) && repoll_deadline) ||
         repoll_serial) &&
        notifier_list_empty(&main_loop_poll_notifiers)) {
        prepare_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        poll_needs_prepare = false;
    }
    ready = g_main_context_prepare(context, &max_priority);
    if (poll_needs_prepare || !notifier_list_empty(&main_loop_poll_notifiers)) {
        prepare_start = -1;
    }
    if (!ready && prepare_start >= 0 && repoll_serial &&
        virtio_serial_poll_rx_begin) {
        serial_rx = virtio_serial_poll_rx_begin();
    }
    try_hpet = !ready && repoll_deadline && deadline_hpet_unlocked;
    try_nobql = !ready && repoll_deadline && deadline_timer_nobql;
    if (try_hpet) {
        x86_hpet_irq0_refresh();
    }
    if (refresh_timers && !repoll_deadline) {
        timeout = qemu_soonest_timeout(
            timeout, timerlistgroup_deadline_ns(&main_loop_tlg));
    }
    if (!ready && repoll_deadline &&
        trial_vnc_request_poll_begin && trial_vnc_request_poll_read) {
        vnc = trial_vnc_request_poll_begin(&vnc_fd);
    }

    bool had_bql = qemu_mutex_iothread_locked();
    if (had_bql) {
        qemu_mutex_unlock_iothread();
    }
    replay_mutex_unlock();

    glib_pollfds_query(context, &timeout);
    if (repoll_deadline) {
        ret = main_loop_poll_deadlines(timeout, try_hpet, try_nobql,
                                       prepare_start, serial_rx,
                                       vnc, vnc_fd);
    } else {
        int64_t last_timeout;

        ret = main_loop_poll_serial_rx(serial_rx, timeout, timeout,
                                       prepare_start, &last_timeout, NULL, -1);
    }
    replay_mutex_lock();
    if (had_bql) {
        qemu_mutex_lock_iothread();
    }
    if (serial_rx) {
        virtio_serial_poll_rx_end(serial_rx);
    }
    trial_taken = true;
    if (try_hpet && x86_hpet_irq0_flush_stats) {
        x86_hpet_irq0_flush_stats();
    }

    glib_pollfds_poll();

    g_main_context_release(context);

    return ret;
}
#else
/***********************************************************/
/* Polling handling */

typedef struct PollingEntry {
    PollingFunc *func;
    void *opaque;
    struct PollingEntry *next;
} PollingEntry;

static PollingEntry *first_polling_entry;

int qemu_add_polling_cb(PollingFunc *func, void *opaque)
{
    PollingEntry **ppe, *pe;
    pe = g_new0(PollingEntry, 1);
    pe->func = func;
    pe->opaque = opaque;
    for(ppe = &first_polling_entry; *ppe != NULL; ppe = &(*ppe)->next);
    *ppe = pe;
    return 0;
}

void qemu_del_polling_cb(PollingFunc *func, void *opaque)
{
    PollingEntry **ppe, *pe;
    for(ppe = &first_polling_entry; *ppe != NULL; ppe = &(*ppe)->next) {
        pe = *ppe;
        if (pe->func == func && pe->opaque == opaque) {
            *ppe = pe->next;
            g_free(pe);
            break;
        }
    }
}

/***********************************************************/
/* Wait objects support */
typedef struct WaitObjects {
    int num;
    int revents[MAXIMUM_WAIT_OBJECTS];
    HANDLE events[MAXIMUM_WAIT_OBJECTS];
    WaitObjectFunc *func[MAXIMUM_WAIT_OBJECTS];
    void *opaque[MAXIMUM_WAIT_OBJECTS];
} WaitObjects;

static WaitObjects wait_objects = {0};

int qemu_add_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    int i;
    WaitObjects *w = &wait_objects;

    if (w->num >= MAXIMUM_WAIT_OBJECTS) {
        return -1;
    }

    for (i = 0; i < w->num; i++) {
        /* check if the same handle is added twice */
        if (w->events[i] == handle) {
            return -1;
        }
    }

    w->events[w->num] = handle;
    w->func[w->num] = func;
    w->opaque[w->num] = opaque;
    w->revents[w->num] = 0;
    w->num++;
    return 0;
}

void qemu_del_wait_object(HANDLE handle, WaitObjectFunc *func, void *opaque)
{
    int i, found;
    WaitObjects *w = &wait_objects;

    found = 0;
    for (i = 0; i < w->num; i++) {
        if (w->events[i] == handle) {
            found = 1;
        }
        if (found && i < (MAXIMUM_WAIT_OBJECTS - 1)) {
            w->events[i] = w->events[i + 1];
            w->func[i] = w->func[i + 1];
            w->opaque[i] = w->opaque[i + 1];
            w->revents[i] = w->revents[i + 1];
        }
    }
    if (found) {
        w->num--;
    }
}

static int pollfds_fill(GArray *pollfds, fd_set *rfds, fd_set *wfds,
                        fd_set *xfds)
{
    int nfds = -1;
    int i;

    for (i = 0; i < pollfds->len; i++) {
        GPollFD *pfd = &g_array_index(pollfds, GPollFD, i);
        int fd = pfd->fd;
        int events = pfd->events;
        if (events & G_IO_IN) {
            FD_SET(fd, rfds);
            nfds = MAX(nfds, fd);
        }
        if (events & G_IO_OUT) {
            FD_SET(fd, wfds);
            nfds = MAX(nfds, fd);
        }
        if (events & G_IO_PRI) {
            FD_SET(fd, xfds);
            nfds = MAX(nfds, fd);
        }
    }
    return nfds;
}

static void pollfds_poll(GArray *pollfds, int nfds, fd_set *rfds,
                         fd_set *wfds, fd_set *xfds)
{
    int i;

    for (i = 0; i < pollfds->len; i++) {
        GPollFD *pfd = &g_array_index(pollfds, GPollFD, i);
        int fd = pfd->fd;
        int revents = 0;

        if (FD_ISSET(fd, rfds)) {
            revents |= G_IO_IN;
        }
        if (FD_ISSET(fd, wfds)) {
            revents |= G_IO_OUT;
        }
        if (FD_ISSET(fd, xfds)) {
            revents |= G_IO_PRI;
        }
        pfd->revents = revents & pfd->events;
    }
}

static int os_host_main_loop_wait(int64_t timeout, bool refresh_timers,
                                  bool repoll_deadline, bool repoll_serial)
{
    GMainContext *context = g_main_context_default();
    GPollFD poll_fds[1024 * 2]; /* this is probably overkill */
    int select_ret = 0;
    int g_poll_ret, ret, i, n_poll_fds;
    PollingEntry *pe;
    WaitObjects *w = &wait_objects;
    gint poll_timeout;
    int64_t poll_timeout_ns;
    static struct timeval tv0;
    fd_set rfds, wfds, xfds;
    int nfds;

    g_main_context_acquire(context);

    /* XXX: need to suppress polling by better using win32 events */
    ret = 0;
    for (pe = first_polling_entry; pe != NULL; pe = pe->next) {
        ret |= pe->func(pe->opaque);
    }
    if (ret != 0) {
        g_main_context_release(context);
        return ret;
    }

    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&xfds);
    nfds = pollfds_fill(gpollfds, &rfds, &wfds, &xfds);
    if (nfds >= 0) {
        select_ret = select(nfds + 1, &rfds, &wfds, &xfds, &tv0);
        if (select_ret != 0) {
            timeout = 0;
        }
        if (select_ret > 0) {
            pollfds_poll(gpollfds, nfds, &rfds, &wfds, &xfds);
        }
    }

    g_main_context_prepare(context, &max_priority);
    if (refresh_timers) {
        timeout = qemu_soonest_timeout(
            timeout, timerlistgroup_deadline_ns(&main_loop_tlg));
    }
    n_poll_fds = g_main_context_query(context, max_priority, &poll_timeout,
                                      poll_fds, ARRAY_SIZE(poll_fds));
    g_assert(n_poll_fds + w->num <= ARRAY_SIZE(poll_fds));

    for (i = 0; i < w->num; i++) {
        poll_fds[n_poll_fds + i].fd = (DWORD_PTR)w->events[i];
        poll_fds[n_poll_fds + i].events = G_IO_IN;
    }

    if (poll_timeout < 0) {
        poll_timeout_ns = -1;
    } else {
        poll_timeout_ns = (int64_t)poll_timeout * (int64_t)SCALE_MS;
    }

    poll_timeout_ns = qemu_soonest_timeout(poll_timeout_ns, timeout);

    qemu_mutex_unlock_iothread();

    replay_mutex_unlock();

    g_poll_ret = qemu_poll_ns(poll_fds, n_poll_fds + w->num, poll_timeout_ns);

    replay_mutex_lock();

    qemu_mutex_lock_iothread();
    if (g_poll_ret > 0) {
        for (i = 0; i < w->num; i++) {
            w->revents[i] = poll_fds[n_poll_fds + i].revents;
        }
        for (i = 0; i < w->num; i++) {
            if (w->revents[i] && w->func[i]) {
                w->func[i](w->opaque[i]);
            }
        }
    }

    if (g_main_context_check(context, max_priority, poll_fds, n_poll_fds)) {
        g_main_context_dispatch(context);
    }

    g_main_context_release(context);

    return select_ret || g_poll_ret;
}
#endif

void main_loop_poll_add_notifier(Notifier *notify)
{
    notifier_list_add(&main_loop_poll_notifiers, notify);
#ifndef _WIN32
    if (deadline_hpet_wait) {
        g_main_context_wakeup(g_main_context_default());
    }
#endif
}

void main_loop_poll_remove_notifier(Notifier *notify)
{
    notifier_remove(notify);
}

void main_loop_wait(int nonblocking)
{
    MainLoopPoll mlpoll = {
        .state = MAIN_LOOP_POLL_FILL,
        .timeout = UINT32_MAX,
        .pollfds = gpollfds,
    };
    int ret;
    int64_t timeout_ns;
    bool nested_timer_loop = running_main_timers;
    bool cancel_timer_wake;
    bool repoll_deadline = false;
    bool repoll_serial = false;

#ifndef _WIN32
    if ((deadline_repoll_enabled || serial_rx_poll_enabled) && !nonblocking &&
        !nested_timer_loop && !g_main_context_is_owner(g_main_context_default()) &&
        !icount_enabled() && replay_mode == REPLAY_MODE_NONE) {
        repoll_deadline = deadline_repoll_enabled;
        repoll_serial = serial_rx_poll_enabled;
    }
#endif

    if (nonblocking) {
        mlpoll.timeout = 0;
    }

    /* poll any events */
    g_array_set_size(gpollfds, 0); /* reset for new iteration */
    /* XXX: separate device handlers from system ones */
    notifier_list_notify(&main_loop_poll_notifiers, &mlpoll);

    if (mlpoll.timeout == UINT32_MAX) {
        timeout_ns = -1;
    } else {
        timeout_ns = (uint64_t)mlpoll.timeout * (int64_t)(SCALE_MS);
    }

    /*
     * The next default-context prepare will recheck the source before
     * polling. Keep the bottom half pending during nested main-loop waits.
     */
    if (main_context_notify_pending && !trial_glib_dispatch &&
        !nested_timer_loop && !g_main_context_is_owner(g_main_context_default())) {
        qemu_bh_cancel(main_context_notify_bh);
        main_context_notify_pending = false;
    }

    /*
     * This outer iteration is about to read all of the new deadlines.
     * Keep the wake available until here: a timer callback can enter
     * another main-loop iteration or AioContext poll before returning.
     */
    cancel_timer_wake = trial_cancel_main_timer_wakes &&
        !nested_timer_loop && main_timer_notify_pending;
    if (cancel_timer_wake) {
        ++trial_cancelled_timer_wakes;
        qemu_bh_cancel(main_timer_notify_bh);
        main_timer_notify_pending = false;
    } else if (!repoll_deadline) {
        timeout_ns = qemu_soonest_timeout(
            timeout_ns, timerlistgroup_deadline_ns(&main_loop_tlg));
    }

    trial_timer_mask = 0;
    trial_activity = 0;
    trial_glib = false;
    trial_taken = false;
    ret = os_host_main_loop_wait(timeout_ns, cancel_timer_wake,
                                 repoll_deadline, repoll_serial);
    mlpoll.state = ret < 0 ? MAIN_LOOP_POLL_ERR : MAIN_LOOP_POLL_OK;
    notifier_list_notify(&main_loop_poll_notifiers, &mlpoll);

    if (icount_enabled()) {
        /*
         * CPU thread can infinitely wait for event after
         * missing the warp
         */
        icount_start_warp_timer();
    }
    running_main_timers = true;
    qemu_clock_run_all_timers();
    running_main_timers = nested_timer_loop;
    trial_record_poll();
    trial_taken = false;
}

/* Functions to operate on the main QEMU AioContext.  */

QEMUBH *qemu_bh_new_full(QEMUBHFunc *cb, void *opaque, const char *name,
                         MemReentrancyGuard *reentrancy_guard)
{
    return aio_bh_new_full(qemu_aio_context, cb, opaque, name,
                           reentrancy_guard);
}

/*
 * Functions to operate on the I/O handler AioContext.
 * This context runs on top of main loop. We can't reuse qemu_aio_context
 * because iohandlers mustn't be polled by aio_poll(qemu_aio_context).
 */
static AioContext *iohandler_ctx;

static void iohandler_init(void)
{
    if (!iohandler_ctx) {
        iohandler_ctx = aio_context_new(&error_abort);
    }
}

AioContext *iohandler_get_aio_context(void)
{
    iohandler_init();
    return iohandler_ctx;
}

GSource *iohandler_get_g_source(void)
{
    iohandler_init();
    return aio_get_g_source(iohandler_ctx);
}

void qemu_set_fd_handler(int fd,
                         IOHandler *fd_read,
                         IOHandler *fd_write,
                         void *opaque)
{
    iohandler_init();
    aio_set_fd_handler(iohandler_ctx, fd, fd_read, fd_write, NULL, NULL,
                       opaque);
}

void event_notifier_set_handler(EventNotifier *e,
                                EventNotifierHandler *handler)
{
    iohandler_init();
    aio_set_event_notifier(iohandler_ctx, e, handler, NULL, NULL);
}
