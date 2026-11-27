/*
 * QEMU TCG Multi Threaded vCPUs implementation
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 * Copyright (c) 2014 Red Hat Inc.
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
#include "sysemu/tcg.h"
#include "sysemu/replay.h"
#include "sysemu/cpu-timers.h"
#include "qemu/main-loop.h"
#include "qemu/notify.h"
#include "qemu/guest-random.h"
#include "qemu/cutils.h"
#include "exec/exec-all.h"
#include "hw/boards.h"
#include "tcg/tcg.h"
#include "tcg-accel-ops.h"
#include "tcg-accel-ops-mttcg.h"

#include "latx-sys-env-init-func.h"
#include "latx-sigfpe-sys.h"
#include "latx-ras.h"
#include "hamt-cpu.h"
#include "hamt-enter.h"
#include "hamt-options.h"
#include "latx-sigrt-sys.h"
#include "latx/include/latx-options.h"

typedef struct MttcgForceRcuNotifier {
    Notifier notifier;
    CPUState *cpu;
} MttcgForceRcuNotifier;

typedef struct HamtHostCPU {
    int id;
    int package_id;
    int core_id;
    bool primary_thread;
} HamtHostCPU;

static gsize hamt_taskset_topology_initialized;
static int hamt_taskset_cpus[CPU_SETSIZE];
static int hamt_taskset_nosmt_cpus[CPU_SETSIZE];
static int hamt_taskset_cpu_count;
static int hamt_taskset_nosmt_cpu_count;

static bool hamt_read_topology_id(int cpu, const char *name, int *value)
{
    g_autofree char *path = g_strdup_printf(
        "/sys/devices/system/cpu/cpu%d/topology/%s", cpu, name);
    g_autofree char *contents = NULL;
    const char *end;

    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        return false;
    }

    if (qemu_strtoi(contents, &end, 10, value) < 0 || *value < 0) {
        return false;
    }
    while (g_ascii_isspace(*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    return true;
}

static void hamt_init_taskset_topology(void)
{
    HamtHostCPU host_cpus[CPU_SETSIZE];
    cpu_set_t allowed;
    int nr_host_cpus = 0;
    bool topology_complete = true;

    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) < 0) {
        fprintf(stderr, "HAMT taskset: sched_getaffinity failed: %s\n",
                strerror(errno));
        return;
    }

    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        HamtHostCPU *host_cpu;

        if (!CPU_ISSET(cpu, &allowed)) {
            continue;
        }

        host_cpu = &host_cpus[nr_host_cpus++];
        host_cpu->id = cpu;
        hamt_taskset_cpus[nr_host_cpus - 1] = cpu;
        host_cpu->primary_thread = true;
        if (!hamt_read_topology_id(cpu, "physical_package_id",
                                   &host_cpu->package_id) ||
            !hamt_read_topology_id(cpu, "core_id", &host_cpu->core_id)) {
            topology_complete = false;
            host_cpu->package_id = 0;
            host_cpu->core_id = cpu;
        }

        for (int i = 0; i < nr_host_cpus - 1; i++) {
            if (host_cpus[i].package_id == host_cpu->package_id &&
                host_cpus[i].core_id == host_cpu->core_id) {
                host_cpu->primary_thread = false;
                break;
            }
        }
    }

    if (nr_host_cpus == 0) {
        fprintf(stderr, "HAMT taskset: no CPU is available\n");
        return;
    }

    if (!topology_complete) {
        fprintf(stderr, "HAMT taskset: incomplete sysfs topology; "
                "treating each logical CPU as a separate core\n");
        for (int i = 0; i < nr_host_cpus; i++) {
            host_cpus[i].primary_thread = true;
        }
    }

    /* Use one thread per physical core before assigning SMT siblings. */
    for (int i = 0; i < nr_host_cpus; i++) {
        if (host_cpus[i].primary_thread) {
            hamt_taskset_nosmt_cpus[hamt_taskset_nosmt_cpu_count++] =
                host_cpus[i].id;
        }
    }
    for (int i = 0; i < nr_host_cpus; i++) {
        if (!host_cpus[i].primary_thread) {
            hamt_taskset_nosmt_cpus[hamt_taskset_nosmt_cpu_count++] =
                host_cpus[i].id;
        }
    }

    hamt_taskset_cpu_count = nr_host_cpus;
}

static int hamt_taskset_cpu(int vcpu_index, bool nosmt)
{
    int *cpus;
    int nr_cpus;

    if (g_once_init_enter(&hamt_taskset_topology_initialized)) {
        hamt_init_taskset_topology();
        g_once_init_leave(&hamt_taskset_topology_initialized, 1);
    }

    cpus = nosmt ? hamt_taskset_nosmt_cpus : hamt_taskset_cpus;
    nr_cpus = nosmt ? hamt_taskset_nosmt_cpu_count : hamt_taskset_cpu_count;
    if (nr_cpus == 0) {
        return -1;
    }

    /* Once every cached CPU is used, sharing CPUs is unavoidable. */
    return cpus[vcpu_index % nr_cpus];
}

static void do_nothing(CPUState *cpu, run_on_cpu_data d)
{
}

static void mttcg_force_rcu(Notifier *notify, void *data)
{
    CPUState *cpu = container_of(notify, MttcgForceRcuNotifier, notifier)->cpu;

    /*
     * Called with rcu_registry_lock held, using async_run_on_cpu() ensures
     * that there are no deadlocks.
     */
    async_run_on_cpu_nobql(cpu, do_nothing, RUN_ON_CPU_NULL);
}

/*
 * In the multi-threaded case each vCPU has its own thread. The TLS
 * variable current_cpu can be used deep in the code to find the
 * current CPUState for a given thread.
 */

#include "latx/include/latx-native-asm.h"
#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
static bool mttcg_atomic_unlocked_enabled(void)
{
    static int enabled = -1;
    int val = qatomic_read(&enabled);

    if (unlikely(val < 0)) {
        val = lats_nobql_enabled();
        qatomic_set(&enabled, val);
    }
    return val != 0;
}
#endif

static void *mttcg_cpu_thread_fn(void *arg)
{
    MttcgForceRcuNotifier force_rcu;
    CPUState *cpu = arg;

    if (hamt_do_taskset()) {
        pthread_t tid = pthread_self();
        assert(tid == cpu->thread->thread);
        int host_cpu = hamt_taskset_cpu(cpu->cpu_index,
                                        hamt_do_taskset_nosmt());
        cpu_set_t cpuset;
        int ret;

        g_assert(host_cpu >= 0);
        CPU_ZERO(&cpuset);
        CPU_SET(host_cpu, &cpuset);
        ret = pthread_setaffinity_np(tid, sizeof(cpu_set_t), &cpuset);
        if (ret != 0) {
            fprintf(stderr,
                    "HAMT taskset: failed to bind vCPU %d to host CPU %d: %s\n",
                    cpu->cpu_index, host_cpu, strerror(ret));
            g_assert_not_reached();
        }
        printf("MTTCG enter HAMT taskset vcpu %d host cpu %d "
               "pthread_t 0x%lx%s\n",
               cpu->cpu_index, host_cpu, (uint64_t)tid,
               hamt_do_taskset_nosmt() ? " nosmt" : "");
    }

    assert(tcg_enabled());
    g_assert(!icount_enabled());

    rcu_register_thread();
    force_rcu.notifier.notify = mttcg_force_rcu;
    force_rcu.cpu = cpu;
    rcu_add_force_rcu_notifier(&force_rcu.notifier);
    tcg_register_thread();

#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
    bool statelock_mttcg = qemu_vcpu_statelock_enabled() &&
                           qemu_tcg_mode_2_lat() &&
                           replay_mode == REPLAY_MODE_NONE &&
                           !option_htimer;
    if (!statelock_mttcg) {
        qemu_mutex_lock_iothread();
    }
#else
    qemu_mutex_lock_iothread();
#endif
    qemu_thread_get_self(cpu->thread);

    cpu->thread_id = qemu_get_thread_id();
    cpu->can_do_io = 1;
    current_cpu = cpu;
#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
    if (!statelock_mttcg) {
        cpu_thread_signal_created(cpu);
    }
#else
    cpu_thread_signal_created(cpu);
#endif
    qemu_guest_random_seed_thread_part2(cpu->random_seed);

    /* process any pending work */
    cpu->exit_request = 1;
    if (qemu_tcg_mode_2_lat()) {
        latx_lsenv_init(cpu->env_ptr);
        latxs_init_rr_thread_sigfpe(cpu);
        lats_init_rr_thread_sigrtmin(cpu);
        /*latxs_init_rr_thread_sigsegv(cpu);*/
    }

    cpu_enter_hamt(cpu, HAMT_MODE_MT, cpu->cpu_index);

#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
    if (statelock_mttcg) {
        cpu_thread_signal_created(cpu);
        do {
            if (cpu_can_run(cpu)) {
                int r;
                do {
                    r = tcg_cpus_exec(cpu);
#ifdef CONFIG_LATX_OPT_NOFP
                    if (qemu_tcg_mode_2_lat()) {
                        __nofp_sync_fpr_to_env(cpu, 0);
                    }
#endif
                    if (r == EXCP_ATOMIC && mttcg_atomic_unlocked_enabled() &&
                        option_irq_kick_fast && qemu_tcg_mode_2_lat() &&
                        !option_htimer && replay_mode == REPLAY_MODE_NONE &&
                        !cpu->singlestep_enabled) {
                        cpu_exec_step_atomic(cpu);
                        r = EXCP_INTERRUPT;
                    }
                } while (option_irq_kick_fast && qemu_tcg_mode_2_lat() &&
                         !option_htimer && replay_mode == REPLAY_MODE_NONE &&
                         !cpu->singlestep_enabled &&
                         (r == EXCP_INTERRUPT || r == EXCP_HLT || r == EXCP_HALTED) &&
                         qemu_try_resume_vcpu_unlocked(cpu, r != EXCP_INTERRUPT));
                switch (r) {
                case EXCP_DEBUG:
                    qemu_mutex_lock_iothread();
                    cpu_handle_guest_debug(cpu);
                    qemu_mutex_unlock_iothread();
                    break;
                case EXCP_HALTED:
                    break;
                case EXCP_ATOMIC:
                    cpu_exec_step_atomic(cpu);
                    break;
                default:
                    break;
                }
            }

            qatomic_set_mb(&cpu->exit_request, 0);
            qemu_wait_io_event_unlocked(cpu);
        } while (!qatomic_read(&cpu->unplug) || cpu_can_run(cpu));

        qemu_mutex_lock_iothread();
        tcg_cpus_destroy(cpu);
        qemu_mutex_unlock_iothread();
        rcu_remove_force_rcu_notifier(&force_rcu.notifier);
        rcu_unregister_thread();
        return NULL;
    }
#endif

    do {
        if (cpu_can_run(cpu)) {
            int r;
            qemu_mutex_unlock_iothread();
            do {
                r = tcg_cpus_exec(cpu);
#ifdef CONFIG_LATX_OPT_NOFP
                if (qemu_tcg_mode_2_lat()) {
                    __nofp_sync_fpr_to_env(cpu, 0);
                }
#endif
#if defined(CONFIG_LATX_OPT_BQL) && defined(CONFIG_LATX_OPT)
                if (r == EXCP_ATOMIC && mttcg_atomic_unlocked_enabled() &&
                    option_irq_kick_fast && qemu_tcg_mode_2_lat() &&
                    !option_htimer && replay_mode == REPLAY_MODE_NONE &&
                    !cpu->singlestep_enabled) {
                    cpu_exec_step_atomic(cpu);
                    r = EXCP_INTERRUPT;
                }
            } while (option_irq_kick_fast && qemu_tcg_mode_2_lat() &&
                     !option_htimer && replay_mode == REPLAY_MODE_NONE &&
                     !cpu->singlestep_enabled &&
                     (r == EXCP_INTERRUPT || r == EXCP_HLT || r == EXCP_HALTED) &&
                     qemu_try_resume_vcpu_unlocked(cpu, r != EXCP_INTERRUPT));
#else
            } while (false);
#endif
            qemu_mutex_lock_iothread();
            switch (r) {
            case EXCP_DEBUG:
                cpu_handle_guest_debug(cpu);
                break;
            case EXCP_HALTED:
                /*
                 * Usually cpu->halted is set, but may have already been
                 * reset by another thread by the time we arrive here.
                 */
                break;
            case EXCP_ATOMIC:
                qemu_mutex_unlock_iothread();
                cpu_exec_step_atomic(cpu);
                qemu_mutex_lock_iothread();
            default:
                /* Ignore everything else? */
                break;
            }
        }

        qatomic_set_mb(&cpu->exit_request, 0);
        qemu_wait_io_event(cpu);
    } while (!cpu->unplug || cpu_can_run(cpu));

    tcg_cpus_destroy(cpu);
    qemu_mutex_unlock_iothread();
    rcu_remove_force_rcu_notifier(&force_rcu.notifier);
    rcu_unregister_thread();
    return NULL;
}

void mttcg_kick_vcpu_thread(CPUState *cpu)
{
    cpu_exit(cpu);
}

void mttcg_start_vcpu_thread(CPUState *cpu)
{
    char thread_name[VCPU_THREAD_NAME_SIZE];

    g_assert(tcg_enabled());
    tcg_cpu_init_cflags(cpu, current_machine->smp.max_cpus > 1);

    cpu->thread = g_new0(QemuThread, 1);
    cpu->halt_cond = g_malloc0(sizeof(QemuCond));
    qemu_cond_init(cpu->halt_cond);

    /* create a thread per vCPU with TCG (MTTCG) */
    snprintf(thread_name, VCPU_THREAD_NAME_SIZE, "CPU %d/TCG",
             cpu->cpu_index);

    qemu_thread_create(cpu->thread, thread_name, mttcg_cpu_thread_fn,
                       cpu, QEMU_THREAD_JOINABLE);
}
