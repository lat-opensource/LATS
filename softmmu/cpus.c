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
#include "qemu/lockdep-lite.h"
#include "target/i386/latx/include/latx-options.h"
#include "target/i386/latx/include/latx-sigrt-sys.h"
#include "monitor/monitor.h"
#include "qemu/coroutine-tls.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/qapi-events-run-state.h"
#include "qapi/qmp/qerror.h"
#include "exec/gdbstub.h"
#include "sysemu/hw_accel.h"
#include "exec/cpu-common.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/plugin.h"
#include "sysemu/cpus.h"
#include "qemu/guest-random.h"
#include "hw/nmi.h"
#include "sysemu/replay.h"
#include "sysemu/runstate.h"
#include "sysemu/tcg.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/whpx.h"
#include "hw/boards.h"
#include "hw/hw.h"
#include "trace.h"

#include "hamt-options.h"

int x86_cpu_pending_interrupt(CPUState *cs, int interrupt_request);
bool x86_cpu_has_wakeup_control_irq(int interrupt_request);


#ifdef CONFIG_LINUX

#include <sys/prctl.h>

#ifndef PR_MCE_KILL
#define PR_MCE_KILL 33
#endif

#ifndef PR_MCE_KILL_SET
#define PR_MCE_KILL_SET 1
#endif

#ifndef PR_MCE_KILL_EARLY
#define PR_MCE_KILL_EARLY 1
#endif

#endif /* CONFIG_LINUX */

/* Keep the lock address private to the CPU and main-loop APIs. */
static QemuMutex qemu_global_mutex;

/*
 * The chosen accelerator is supposed to register this.
 */
static const AccelOpsClass *cpus_accel;

bool cpu_is_stopped(CPUState *cpu)
{
    return qatomic_read(&cpu->stopped) || !runstate_is_running();
}

bool cpu_work_list_empty(CPUState *cpu)
{
    return QSIMPLEQ_EMPTY_ATOMIC(&cpu->work_list);
}

bool cpu_thread_is_idle(CPUState *cpu)
{
    if (qatomic_read(&cpu->stop) || !cpu_work_list_empty(cpu)) {
        return false;
    }
    if (cpu_is_stopped(cpu)) {
        return true;
    }
#ifdef CONFIG_LATX_OPT
    {
        int req = qatomic_read(&cpu->interrupt_request);
        if (!qatomic_read(&cpu->halted) ||
            (x86_cpu_pending_interrupt(cpu, req) != 0) ||
            (qemu_vcpu_statelock_enabled() &&
             x86_cpu_has_wakeup_control_irq(req))) {
            return false;
        }
    }
#else
    if (!cpu->halted || cpu_has_work(cpu)) {
        return false;
    }
#endif
    if (cpus_accel->cpu_thread_is_idle) {
        return cpus_accel->cpu_thread_is_idle(cpu);
    }
    return true;
}

bool all_cpu_threads_idle(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!cpu_thread_is_idle(cpu)) {
            return false;
        }
    }
    return true;
}

/***********************************************************/
void hw_error(const char *fmt, ...)
{
    va_list ap;
    CPUState *cpu;

    va_start(ap, fmt);
    fprintf(stderr, "qemu: hardware error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    CPU_FOREACH(cpu) {
        fprintf(stderr, "CPU #%d:\n", cpu->cpu_index);
        cpu_dump_state(cpu, stderr, CPU_DUMP_FPU);
    }
    va_end(ap);
    abort();
}

void cpu_synchronize_all_states(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_state(cpu);
    }
}

void cpu_synchronize_all_post_reset(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_post_reset(cpu);
    }
}

void cpu_synchronize_all_post_init(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_post_init(cpu);
    }
}

void cpu_synchronize_all_pre_loadvm(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        cpu_synchronize_pre_loadvm(cpu);
    }
}

void cpu_synchronize_state(CPUState *cpu)
{
    if (cpus_accel->synchronize_state) {
        cpus_accel->synchronize_state(cpu);
    }
}

void cpu_synchronize_post_reset(CPUState *cpu)
{
    if (cpus_accel->synchronize_post_reset) {
        cpus_accel->synchronize_post_reset(cpu);
    }
}

void cpu_synchronize_post_init(CPUState *cpu)
{
    if (cpus_accel->synchronize_post_init) {
        cpus_accel->synchronize_post_init(cpu);
    }
}

void cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    if (cpus_accel->synchronize_pre_loadvm) {
        cpus_accel->synchronize_pre_loadvm(cpu);
    }
}

bool cpus_are_resettable(void)
{
    if (cpus_accel->cpus_are_resettable) {
        return cpus_accel->cpus_are_resettable();
    }
    return true;
}

int64_t cpus_get_virtual_clock(void)
{
    /*
     * XXX
     *
     * need to check that cpus_accel is not NULL, because qcow2 calls
     * qemu_get_clock_ns(CLOCK_VIRTUAL) without any accel initialized and
     * with ticks disabled in some io-tests:
     * 030 040 041 060 099 120 127 140 156 161 172 181 191 192 195 203 229 249 256 267
     *
     * is this expected?
     *
     * XXX
     */
    if (cpus_accel && cpus_accel->get_virtual_clock) {
        return cpus_accel->get_virtual_clock();
    }
    return cpu_get_clock();
}

/*
 * return the time elapsed in VM between vm_start and vm_stop.  Unless
 * icount is active, cpus_get_elapsed_ticks() uses units of the host CPU cycle
 * counter.
 */
int64_t cpus_get_elapsed_ticks(void)
{
    if (cpus_accel->get_elapsed_ticks) {
        return cpus_accel->get_elapsed_ticks();
    }
    return cpu_get_ticks();
}

static void generic_handle_interrupt(CPUState *cpu, int mask)
{
    qatomic_or(&cpu->interrupt_request, mask);

    if (!qemu_cpu_is_self(cpu)) {
        qemu_cpu_kick(cpu);
    }
}

void cpu_interrupt(CPUState *cpu, int mask)
{
    if (cpus_accel->handle_interrupt) {
        cpus_accel->handle_interrupt(cpu, mask);
    } else {
        generic_handle_interrupt(cpu, mask);
    }
}

static int do_vm_stop(RunState state, bool send_stop)
{
    int ret = 0;

    if (runstate_is_running()) {
        runstate_set(state);
        cpu_disable_ticks();
        pause_all_vcpus();
        vm_state_notify(0, state);
        if (send_stop) {
            qapi_event_send_stop();
        }
    }

    bdrv_drain_all();
    ret = bdrv_flush_all();
    trace_vm_stop_flush_all(ret);

    return ret;
}

/* Special vm_stop() variant for terminating the process.  Historically clients
 * did not expect a QMP STOP event and so we need to retain compatibility.
 */
int vm_shutdown(void)
{
    return do_vm_stop(RUN_STATE_SHUTDOWN, false);
}

bool cpu_can_run(CPUState *cpu)
{
    if (qatomic_read(&cpu->stop)) {
        return false;
    }
    if (cpu_is_stopped(cpu)) {
        return false;
    }
    return true;
}

void cpu_handle_guest_debug(CPUState *cpu)
{
    if (replay_running_debug()) {
        if (!cpu->singlestep_enabled) {
            /*
             * Report about the breakpoint and
             * make a single step to skip it
             */
            replay_breakpoint();
            cpu_single_step(cpu, SSTEP_ENABLE);
        } else {
            cpu_single_step(cpu, 0);
        }
    } else {
        gdb_set_stop_cpu(cpu);
        qemu_system_debug_request();
        qatomic_set(&cpu->stopped, true);
    }
}

#ifdef CONFIG_LINUX
static void sigbus_reraise(void)
{
    sigset_t set;
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    if (!sigaction(SIGBUS, &action, NULL)) {
        raise(SIGBUS);
        sigemptyset(&set);
        sigaddset(&set, SIGBUS);
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    }
    perror("Failed to re-raise SIGBUS!");
    abort();
}

static void sigbus_handler(int n, siginfo_t *siginfo, void *ctx)
{
    if (siginfo->si_code != BUS_MCEERR_AO && siginfo->si_code != BUS_MCEERR_AR) {
        sigbus_reraise();
    }

    if (current_cpu) {
        /* Called asynchronously in VCPU thread.  */
        if (kvm_on_sigbus_vcpu(current_cpu, siginfo->si_code, siginfo->si_addr)) {
            sigbus_reraise();
        }
    } else {
        /* Called synchronously (via signalfd) in main thread.  */
        if (kvm_on_sigbus(siginfo->si_code, siginfo->si_addr)) {
            sigbus_reraise();
        }
    }
}

static void qemu_init_sigbus(void)
{
    struct sigaction action;

    /*
     * ALERT: when modifying this, take care that SIGBUS forwarding in
     * qemu_prealloc_mem() will continue working as expected.
     */
    memset(&action, 0, sizeof(action));
    action.sa_flags = SA_SIGINFO;
    action.sa_sigaction = sigbus_handler;
    sigaction(SIGBUS, &action, NULL);

    prctl(PR_MCE_KILL, PR_MCE_KILL_SET, PR_MCE_KILL_EARLY, 0, 0);
}
#else /* !CONFIG_LINUX */
static void qemu_init_sigbus(void)
{
}
#endif /* !CONFIG_LINUX */

static QemuThread io_thread;

/* cpu creation */
static QemuCond qemu_cpu_cond;
/* system init */
static QemuCond qemu_pause_cond;

#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
static bool trial_exclusive_cpu_work;
static bool trial_exclusive_cpu_work_all;
static uint64_t trial_exclusive_items, trial_exclusive_resumed;

void trial_exclusive_work_counts(const char *phase);
void trial_exclusive_work_counts(const char *phase)
{
    fprintf(stderr, "EXCLUSIVE_CPU_WORK %s items=%" PRIu64
                    " resumed=%" PRIu64 "\n",
            phase, qatomic_read(&trial_exclusive_items),
            qatomic_read(&trial_exclusive_resumed));
}
#endif

static bool io_thread_initialized;

void qemu_init_cpu_loop(void)
{
#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
    trial_exclusive_cpu_work_all =
        lats_nobql_enabled();
    trial_exclusive_cpu_work = trial_exclusive_cpu_work_all ||
        lats_nobql_enabled();
#endif
    qemu_init_sigbus();
    qemu_cond_init(&qemu_cpu_cond);
    qemu_cond_init(&qemu_pause_cond);
    qemu_mutex_init(&qemu_global_mutex);

    qemu_thread_get_self(&io_thread);
    qatomic_store_release(&io_thread_initialized, true);
}

void run_on_cpu(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data)
{
    do_run_on_cpu(cpu, func, data, &qemu_global_mutex);
}

void run_on_cpu_nobql(CPUState *cpu, run_on_cpu_func func, run_on_cpu_data data)
{
    if (qemu_vcpu_statelock_enabled()) {
        do_run_on_cpu_nobql(cpu, func, data, &qemu_global_mutex);
    } else {
        do_run_on_cpu(cpu, func, data, &qemu_global_mutex);
    }
}

static void qemu_cpu_stop(CPUState *cpu, bool exit)
{
    g_assert(qemu_cpu_is_self(cpu));
    qatomic_set(&cpu->stop, false);
    qatomic_set(&cpu->stopped, true);
    if (exit) {
        cpu_exit(cpu);
    }
    qemu_cond_broadcast_1(&qemu_pause_cond);
}

void qemu_wait_io_event_common(CPUState *cpu)
{
    qatomic_set_mb(&cpu->thread_kicked, false);
    if (qatomic_read(&cpu->stop)) {
        qemu_cpu_stop(cpu, false);
    }
    process_queued_cpu_work(cpu);
}

#ifdef CONFIG_LATX_PROFILER
extern uint64_t latx_profile_cpu_halt_time[];
#endif

extern uint64_t qcondwait3_max;

bool qemu_vcpu_statelock_enabled(void)
{
    static int enabled = -1;
    int val = qatomic_read(&enabled);

    if (unlikely(val < 0)) {
        val = lats_nobql_enabled();
        qatomic_set(&enabled, val);
    }
    return val != 0;
}

#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
/* Only the vCPU may enter the guest again. Management and work callbacks
 * retain their existing synchronization when this returns false. */
bool qemu_try_resume_vcpu_unlocked(CPUState *cpu, bool guest_halted)
{
    unsigned exclusive_items = 0;
    bool statelock = qemu_vcpu_statelock_enabled();
    bool resume;
#ifdef CONFIG_LATX_PROFILER
    uint64_t start = latx_host_rdtime();
#endif

    assert(!qemu_mutex_iothread_locked());
    qatomic_set_mb(&cpu->exit_request, 0);
    for (;;) {
        unsigned seq = qatomic_load_acquire(&cpu->halt_cond->futex_seq);
        bool halted;
        int req;

        /*
         * Check the persistent requests after reading the sequence. Their
         * producers publish the state before waking the vCPU. An arrival
         * after this check changes the futex value or the TB exit flag.
         */
        if (!cpu_can_run(cpu) || qatomic_read(&cpu->unplug)) {
            resume = false;
            break;
        }
        if (!cpu_work_list_empty(cpu)) {
            if (statelock && process_nobql_cpu_work_unlocked(cpu, true)) {
                continue;
            }
            if (trial_exclusive_cpu_work &&
                (trial_exclusive_cpu_work_all ||
                 (!guest_halted && !qatomic_read(&cpu->halted) &&
                  !qatomic_read(&cpu->interrupt_request))) &&
                process_exclusive_cpu_work_unlocked(
                    cpu, trial_exclusive_cpu_work_all)) {
                ++exclusive_items;
                continue;
            }
            resume = false;
            break;
        }
        if (statelock) {
            lats_lockdep_acquire(&cpu->state_lock, LOCK_RANK_LEAF,
                                 "cpu->state_lock");
            qemu_spin_lock(&cpu->state_lock);
            halted = qatomic_read(&cpu->halted) != 0;
            req = qatomic_read(&cpu->interrupt_request);
            qemu_spin_unlock(&cpu->state_lock);
            lats_lockdep_release(&cpu->state_lock);
        } else {
            halted = cpu->halted != 0;
            req = qatomic_read(&cpu->interrupt_request);
        }
        if (!halted || x86_cpu_pending_interrupt(cpu, req) ||
            (statelock && x86_cpu_has_wakeup_control_irq(req))) {
            resume = true;
            break;
        }
        if ((!guest_halted && !statelock) ||
            test_bit(QEMU_PLUGIN_EV_VCPU_IDLE, cpu->plugin_mask) ||
            test_bit(QEMU_PLUGIN_EV_VCPU_RESUME, cpu->plugin_mask)) {
            resume = false;
            break;
        }
        qemu_cond_wait_2_deadline_unlocked(cpu->halt_cond, seq, NULL, qcondwait3_max);
    }
#ifdef CONFIG_LATX_PROFILER
    qatomic_add(&latx_profile_cpu_halt_time[cpu->cpu_index],
                latx_host_rdtime() - start);
#endif
    if (exclusive_items) {
        qatomic_add(&trial_exclusive_items, exclusive_items);
        if (resume) {
            qatomic_inc(&trial_exclusive_resumed);
        }
    }
    return resume;
}
#endif

void qemu_wait_io_event(CPUState *cpu)
{
    bool slept = false;

#ifdef CONFIG_LATX_PROFILER
    uint64_t cpu_halt_st = latx_host_rdtime();
#endif
    for (;;) {
#ifdef CONFIG_LATX_OPT_BQL
        /* A kick can publish work without holding the global lock. */
        unsigned seq = qatomic_load_acquire(&cpu->halt_cond->futex_seq);
#endif
        if (!cpu_thread_is_idle(cpu)) {
            break;
        }
        if (!slept) {
            slept = true;
            qemu_plugin_vcpu_idle_cb(cpu);
        }
#ifdef CONFIG_LATX_OPT_BQL
        qemu_cond_wait_2_deadline(cpu->halt_cond, &qemu_global_mutex,
                                  seq, NULL, qcondwait3_max);
#else
        qemu_cond_wait(cpu->halt_cond, &qemu_global_mutex);
#endif
    }
#ifdef CONFIG_LATX_PROFILER
    uint64_t cpu_halt_ed = latx_host_rdtime();
    uint64_t *cpu_halt_time_ptr = &latx_profile_cpu_halt_time[cpu->cpu_index];
    qatomic_add(cpu_halt_time_ptr, (cpu_halt_ed - cpu_halt_st));
#endif
	
    if (slept) {
        qemu_plugin_vcpu_resume_cb(cpu);
    }

#ifdef _WIN32
    /* Eat dummy APC queued by cpus_kick_thread. */
    if (hax_enabled()) {
        SleepEx(0, TRUE);
    }
#endif
    qemu_wait_io_event_common(cpu);
}

#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
void qemu_wait_io_event_unlocked(CPUState *cpu)
{
    bool slept = false;
#ifdef CONFIG_LATX_PROFILER
    uint64_t cpu_halt_st = latx_host_rdtime();
#endif

    assert(!qemu_mutex_iothread_locked());
    for (;;) {
        unsigned seq = qatomic_load_acquire(&cpu->halt_cond->futex_seq);
        if (!cpu_thread_is_idle(cpu)) {
            break;
        }
        if (!slept) {
            slept = true;
            if (unlikely(test_bit(QEMU_PLUGIN_EV_VCPU_IDLE,
                                  cpu->plugin_mask))) {
                qemu_mutex_lock_iothread();
                qemu_plugin_vcpu_idle_cb(cpu);
                qemu_mutex_unlock_iothread();
            }
        }
        qemu_cond_wait_2_deadline_unlocked(cpu->halt_cond, seq, NULL,
                                           qcondwait3_max);
    }
#ifdef CONFIG_LATX_PROFILER
    uint64_t cpu_halt_ed = latx_host_rdtime();
    uint64_t *cpu_halt_time_ptr = &latx_profile_cpu_halt_time[cpu->cpu_index];
    qatomic_add(cpu_halt_time_ptr, (cpu_halt_ed - cpu_halt_st));
#endif
    if (slept && unlikely(test_bit(QEMU_PLUGIN_EV_VCPU_RESUME,
                                   cpu->plugin_mask))) {
        qemu_mutex_lock_iothread();
        qemu_plugin_vcpu_resume_cb(cpu);
        qemu_mutex_unlock_iothread();
    }

    qatomic_set_mb(&cpu->thread_kicked, false);
    if (qatomic_read(&cpu->stop)) {
        qemu_cpu_stop(cpu, false);
    }
    process_queued_cpu_work_unlocked(cpu);
}
#endif

void cpus_kick_thread(CPUState *cpu)
{
    if (cpu->thread_kicked) {
        return;
    }
    cpu->thread_kicked = true;

#ifndef _WIN32
    int err = pthread_kill(cpu->thread->thread, SIG_IPI);
    if (err && err != ESRCH) {
        fprintf(stderr, "qemu:%s: %s", __func__, strerror(err));
        exit(1);
    }
#else
    qemu_sem_post(&cpu->sem);
#endif
}

void qemu_cpu_kick(CPUState *cpu)
{
    qemu_cond_broadcast_3(cpu->halt_cond);
    if (cpus_accel->kick_vcpu_thread) {
        cpus_accel->kick_vcpu_thread(cpu);
    } else { /* default */
        cpus_kick_thread(cpu);
    }
}

QEMU_DEFINE_STATIC_CO_TLS(bool, iothread_locked)

void qemu_cpu_kick_self(void)
{
    assert(current_cpu);
    cpus_kick_thread(current_cpu);
}

bool qemu_cpu_is_self(CPUState *cpu)
{
    return qemu_thread_is_self(cpu->thread);
}

bool qemu_in_vcpu_thread(void)
{
    return current_cpu && qemu_cpu_is_self(current_cpu);
}

bool qemu_mutex_iothread_locked(void)
{
    return get_iothread_locked();
}

bool qemu_in_main_thread(void)
{
    if (lats_nobql_enabled() && qatomic_load_acquire(&io_thread_initialized) &&
        qemu_thread_is_self(&io_thread)) {
        return true;
    }
    return qemu_mutex_iothread_locked();
}

/*
 * The BQL is taken from so many places that it is worth profiling the
 * callers directly, instead of funneling them all through a single function.
 */
void qemu_mutex_lock_iothread_impl(const char *file, int line)
{
    QemuMutexLockFunc bql_lock = qatomic_read(&qemu_bql_mutex_lock_func);

    g_assert(!qemu_mutex_iothread_locked());
    lats_lockdep_acquire(&qemu_global_mutex, LOCK_RANK_BQL, "bql");
#ifdef CONFIG_LATX_OPT_BQL
    if (likely(bql_lock == qemu_bql_lock_impl)) {
        pthread_spin_lock(&qemu_global_mutex.spin);
    } else {
        bql_lock(&qemu_global_mutex, file, line);
    }
#else
    bql_lock(&qemu_global_mutex, file, line);
#endif
    set_iothread_locked(true);
}

#ifdef CONFIG_LATX_PROFILER
static __thread uint64_t mmio_bql_profile_ticks;

uint64_t qemu_mmio_bql_profile_ticks(void)
{
    return mmio_bql_profile_ticks;
}

#endif

static inline void qemu_mutex_lock_iothread_mmio(const void *mr, const char *name,
                                                  uint64_t offset, bool is_write,
                                                  const char *file, int line,
                                                  bool profile_tb)
{
    QemuMutexLockFunc bql_lock = qatomic_read(&qemu_bql_mutex_lock_func);
    bool profiling = unlikely(bql_lock != qemu_bql_lock_impl);
    int64_t start = 0;

    g_assert(!qemu_mutex_iothread_locked());
    lats_lockdep_acquire(&qemu_global_mutex, LOCK_RANK_BQL,
                         name ? name : "bql-mmio");
    if (profiling) {
        start = get_clock();
    }
#ifdef CONFIG_LATX_PROFILER
    uint64_t tb_start = profile_tb ? latx_host_rdtime() : 0;
#endif
#ifdef CONFIG_LATX_OPT_BQL
    if (likely(!profiling)) {
        pthread_spin_lock(&qemu_global_mutex.spin);
    } else {
        bql_lock(&qemu_global_mutex, file, line);
    }
#else
    bql_lock(&qemu_global_mutex, file, line);
#endif
#ifdef CONFIG_LATX_PROFILER
    if (profile_tb) {
        mmio_bql_profile_ticks += latx_host_rdtime() - tb_start;
    }
#endif
    if (profiling) {
        qsp_record_mmio_bql(mr, name, offset, is_write,
                            get_clock() - start);
    }
    set_iothread_locked(true);
}

void qemu_mutex_lock_iothread_mmio_impl(const void *mr, const char *name,
                                       uint64_t offset, bool is_write,
                                       const char *file, int line)
{
    qemu_mutex_lock_iothread_mmio(mr, name, offset, is_write, file, line, false);
}

#ifdef CONFIG_LATX_PROFILER
void qemu_mutex_lock_iothread_mmio_profile_impl(const void *mr, const char *name,
                                                uint64_t offset, bool is_write,
                                                const char *file, int line)
{
    qemu_mutex_lock_iothread_mmio(mr, name, offset, is_write, file, line, true);
}
#endif

void qemu_mutex_unlock_iothread(void)
{
    g_assert(qemu_mutex_iothread_locked());
    set_iothread_locked(false);
#ifdef CONFIG_LATX_OPT_BQL
    pthread_spin_unlock(&qemu_global_mutex.spin);
#else
    qemu_mutex_unlock(&qemu_global_mutex);
#endif
    lats_lockdep_release(&qemu_global_mutex);
    if (option_int_opt == IRQ_CHECK_ASYNC_NOTIFY && !current_cpu) {
        lats_notify_after_iothread_unlock();
    }
}

void qemu_cond_wait_iothread(QemuCond *cond)
{
    qemu_cond_wait_1(cond, &qemu_global_mutex);
}

void qemu_cond_wait_2_iothread(QemuCond *cond)
{
    qemu_cond_wait_2(cond, &qemu_global_mutex);
}

void qemu_cond_timedwait_iothread(QemuCond *cond, int ms)
{
		printf("%s \n", __func__);
    pthread_spin_unlock(&qemu_global_mutex.spin);
    qemu_cond_timedwait(cond, &qemu_global_mutex, ms);
}

/* signal CPU creation */
void cpu_thread_signal_created(CPUState *cpu)
{
    qatomic_store_release(&cpu->created, true);
    qemu_cond_signal_1(&qemu_cpu_cond);
}

/* signal CPU destruction */
void cpu_thread_signal_destroyed(CPUState *cpu)
{
    qatomic_store_release(&cpu->created, false);
    qemu_cond_signal_1(&qemu_cpu_cond);
}


static bool all_vcpus_paused(void)
{
    CPUState *cpu;

    CPU_FOREACH(cpu) {
        if (!qatomic_read(&cpu->stopped)) {
            return false;
        }
    }

    return true;
}

void pause_all_vcpus(void)
{
    CPUState *cpu;

    if (cpus_accel->set_irq_delivery_paused) {
        cpus_accel->set_irq_delivery_paused(true);
    }
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, false);
    CPU_FOREACH(cpu) {
        if (qemu_cpu_is_self(cpu)) {
            qemu_cpu_stop(cpu, true);
        } else {
            qatomic_set(&cpu->stop, true);
            qemu_cpu_kick(cpu);
        }
    }

    /* We need to drop the replay_lock so any vCPU threads woken up
     * can finish their replay tasks
     */
    replay_mutex_unlock();

#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
    if (qemu_vcpu_statelock_enabled() && qemu_tcg_mode_2_lat() &&
        tcg_enabled() && replay_mode == REPLAY_MODE_NONE &&
        !icount_enabled() && !option_htimer) {
        /*
         * Unlocked MTTCG vCPUs execute qemu_cpu_stop() without BQL. Wait
         * without dropping BQL first; fall back to dropping BQL after
         * bounded retries if any vCPU is blocked on BQL in legacy MMIO.
         */
        for (int attempt = 0; attempt < 64 && !all_vcpus_paused(); ++attempt) {
            unsigned long ticket = qemu_cond_wait_prepare_1(&qemu_pause_cond);
            if (all_vcpus_paused()) {
                break;
            }
            if (!qemu_cond_wait_1_n_unlocked(&qemu_pause_cond, ticket, 10000)) {
                CPU_FOREACH(cpu) {
                    if (!qatomic_read(&cpu->stopped)) {
                        qemu_cpu_kick(cpu);
                    }
                }
                if (attempt >= 2) {
                    g_usleep(100);
                }
            }
        }
    }
#endif

    while (!all_vcpus_paused()) {
        if (!qemu_mutex_iothread_locked()) {
            unsigned long ticket = qemu_cond_wait_prepare_1(&qemu_pause_cond);
            if (all_vcpus_paused()) {
                break;
            }
            if (!qemu_cond_wait_1_n_unlocked(&qemu_pause_cond, ticket, 10000)) {
                g_usleep(100);
            }
        } else {
            qemu_cond_wait_1(&qemu_pause_cond, &qemu_global_mutex);
        }
        CPU_FOREACH(cpu) {
            qemu_cpu_kick(cpu);
        }
    }

    if (replay_mode != REPLAY_MODE_NONE) {
        bool had_bql = qemu_mutex_iothread_locked();
        if (had_bql) {
            qemu_mutex_unlock_iothread();
        }
        replay_mutex_lock();
        if (had_bql) {
            qemu_mutex_lock_iothread();
        }
    }
}

void cpu_resume(CPUState *cpu)
{
    if (cpus_accel->set_irq_delivery_paused) {
        cpus_accel->set_irq_delivery_paused(false);
    }
    qatomic_set(&cpu->stop, false);
    qatomic_set(&cpu->stopped, false);
    qemu_cpu_kick(cpu);
}

void resume_all_vcpus(void)
{
    CPUState *cpu;

    if (!runstate_is_running()) {
        return;
    }

    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    CPU_FOREACH(cpu) {
        cpu_resume(cpu);
    }
}

void cpu_remove_sync(CPUState *cpu)
{
    bool had_bql = qemu_mutex_iothread_locked();

    qatomic_set(&cpu->stop, true);
    qatomic_set(&cpu->unplug, true);
    qemu_cpu_kick(cpu);
    if (had_bql) {
        qemu_mutex_unlock_iothread();
    }
    qemu_thread_join(cpu->thread);
    if (had_bql) {
        qemu_mutex_lock_iothread();
    }
}

void cpus_register_accel(const AccelOpsClass *ops)
{
    assert(ops != NULL);
    assert(ops->create_vcpu_thread != NULL); /* mandatory */
    cpus_accel = ops;
}

const AccelOpsClass *cpus_get_accel(void)
{
    /* broken if we call this early */
    assert(cpus_accel);
    return cpus_accel;
}

void qemu_init_vcpu(CPUState *cpu)
{
    MachineState *ms = MACHINE(qdev_get_machine());

    cpu->nr_cores = ms->smp.cores;
    cpu->nr_threads =  ms->smp.threads;
    qatomic_set(&cpu->stopped, true);
    cpu->random_seed = qemu_guest_random_seed_thread_part1();

    if (!cpu->as) {
        /* If the target cpu hasn't set up any address spaces itself,
         * give it the default one.
         */
        cpu->num_ases = 1;
        cpu_address_space_init(cpu, 0, "cpu-memory", cpu->memory);
    }

    /* accelerators all implement the AccelOpsClass */
    g_assert(cpus_accel != NULL && cpus_accel->create_vcpu_thread != NULL);

    offline_cpu = cpu;

    cpus_accel->create_vcpu_thread(cpu);

    while (!qatomic_load_acquire(&cpu->created)) {
#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
        if (qemu_vcpu_statelock_enabled() && qemu_tcg_mode_2_lat() &&
            tcg_enabled() && replay_mode == REPLAY_MODE_NONE &&
            !icount_enabled() && !option_htimer) {
            unsigned long ticket = qemu_cond_wait_prepare_1(&qemu_cpu_cond);
            if (qatomic_load_acquire(&cpu->created)) {
                break;
            }
            if (!qemu_cond_wait_1_n_unlocked(&qemu_cpu_cond, ticket, 10000)) {
                g_usleep(50);
            }
            continue;
        }
#endif
#ifdef CONFIG_LATX_OPT
        qemu_cond_wait_1(&qemu_cpu_cond, &qemu_global_mutex);
#else
        qemu_cond_wait(&qemu_cpu_cond, &qemu_global_mutex);
#endif
    }
}

void cpu_stop_current(void)
{
    if (current_cpu) {
        qatomic_set(&current_cpu->stop, true);
        cpu_exit(current_cpu);
    }
}

int vm_stop(RunState state)
{
    if (qemu_in_vcpu_thread()) {
        qemu_system_vmstop_request_prepare();
        qemu_system_vmstop_request(state);
        /*
         * FIXME: should not return to device code in case
         * vm_stop() has been requested.
         */
        cpu_stop_current();
        return 0;
    }

    return do_vm_stop(state, true);
}

/**
 * Prepare for (re)starting the VM.
 * Returns -1 if the vCPUs are not to be restarted (e.g. if they are already
 * running or in case of an error condition), 0 otherwise.
 */
int vm_prepare_start(bool step_pending)
{
    RunState requested;

    qemu_vmstop_requested(&requested);
    if (runstate_is_running() && requested == RUN_STATE__MAX) {
        return -1;
    }

    /* Ensure that a STOP/RESUME pair of events is emitted if a
     * vmstop request was pending.  The BLOCK_IO_ERROR event, for
     * example, according to documentation is always followed by
     * the STOP event.
     */
    if (runstate_is_running()) {
        qapi_event_send_stop();
        qapi_event_send_resume();
        return -1;
    }

    /*
     * WHPX accelerator needs to know whether we are going to step
     * any CPUs, before starting the first one.
     */
    if (cpus_accel->synchronize_pre_resume) {
        cpus_accel->synchronize_pre_resume(step_pending);
    }

    /* We are sending this now, but the CPUs will be resumed shortly later */
    qapi_event_send_resume();

    cpu_enable_ticks();
    runstate_set(RUN_STATE_RUNNING);
    vm_state_notify(1, RUN_STATE_RUNNING);
    return 0;
}

void vm_start(void)
{
    if (!vm_prepare_start(false)) {
        resume_all_vcpus();
    }
}

/* does a state transition even if the VM is already stopped,
   current state is forgotten forever */
int vm_stop_force_state(RunState state)
{
    if (runstate_is_running()) {
        return vm_stop(state);
    } else {
        int ret;
        runstate_set(state);

        bdrv_drain_all();
        /* Make sure to return an error if the flush in a previous vm_stop()
         * failed. */
        ret = bdrv_flush_all();
        trace_vm_stop_flush_all(ret);
        return ret;
    }
}

void qmp_memsave(int64_t addr, int64_t size, const char *filename,
                 bool has_cpu, int64_t cpu_index, Error **errp)
{
    FILE *f;
    uint32_t l;
    CPUState *cpu;
    uint8_t buf[1024];
    int64_t orig_addr = addr, orig_size = size;

    if (!has_cpu) {
        cpu_index = 0;
    }

    cpu = qemu_get_cpu(cpu_index);
    if (cpu == NULL) {
        error_setg(errp, QERR_INVALID_PARAMETER_VALUE, "cpu-index",
                   "a CPU number");
        return;
    }

    f = fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        return;
    }

    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        if (cpu_memory_rw_debug(cpu, addr, buf, l, 0) != 0) {
            error_setg(errp, "Invalid addr 0x%016" PRIx64 "/size %" PRId64
                             " specified", orig_addr, orig_size);
            goto exit;
        }
        if (fwrite(buf, 1, l, f) != l) {
            error_setg(errp, QERR_IO_ERROR);
            goto exit;
        }
        addr += l;
        size -= l;
    }

exit:
    fclose(f);
}

void qmp_pmemsave(int64_t addr, int64_t size, const char *filename,
                  Error **errp)
{
    FILE *f;
    uint32_t l;
    uint8_t buf[1024];

    f = fopen(filename, "wb");
    if (!f) {
        error_setg_file_open(errp, errno, filename);
        return;
    }

    while (size != 0) {
        l = sizeof(buf);
        if (l > size)
            l = size;
        cpu_physical_memory_read(addr, buf, l);
        if (fwrite(buf, 1, l, f) != l) {
            error_setg(errp, QERR_IO_ERROR);
            goto exit;
        }
        addr += l;
        size -= l;
    }

exit:
    fclose(f);
}

void qmp_inject_nmi(Error **errp)
{
    nmi_monitor_handle(monitor_get_cpu_index(monitor_cur()), errp);
}
