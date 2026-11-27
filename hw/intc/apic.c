/*
 *  APIC support
 *
 *  Copyright (c) 2004-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 */
#include "qemu/osdep.h"
#include "hw/i386/hpet-irq0.h"
#include "qemu/thread.h"
#include "qemu/error-report.h"
#include "hw/i386/apic_internal.h"
#include "hw/i386/apic.h"
#include "hw/intc/ioapic.h"
#include "hw/intc/i8259.h"
#include "hw/isa/i8259_internal.h"
#include "hw/intc/kvm_irqcount.h"
#include "hw/boards.h"
#include "exec/irq-commit.h"
#include "hw/pci/msi.h"
#include "qemu/host-utils.h"
#include "sysemu/kvm.h"
#include "sysemu/cpus.h"
#include "sysemu/replay.h"
#include "sysemu/runstate.h"
#include "sysemu/tcg.h"
#include "sysemu/cpu-timers.h"
#include "qemu/main-loop.h"
#include "trace.h"
#include "trace/trace-softmmu.h"
#include "hw/i386/apic-msidef.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "latx/include/latx-options.h"
#include "htimer.h"
#include "dune-kvm-init-value.h"
#include "latx-sigrt-sys.h"

#ifdef __loongarch__
#include "hamt-options.h"
#include "loongarch-csr.h"
#endif

#define MAX_APICS 255
#define MAX_APIC_WORDS 8

#define SYNC_FROM_VAPIC                 0x1
#define SYNC_TO_VAPIC                   0x2
#define SYNC_ISR_IRR_TO_VAPIC           0x4

static APICCommonState *local_apics[MAX_APICS + 1];
static unsigned int trial_msi_locked;
static unsigned int trial_msi_unlocked;

static void trial_msi_record(const MSIMessage *msi)
{
    if (!current_cpu && (msi->data & MSI_DATA_VECTOR_MASK) == 0x51) {
        if (qemu_mutex_iothread_locked()) {
            qatomic_inc(&trial_msi_locked);
        } else {
            qatomic_inc(&trial_msi_unlocked);
        }
    }
}

static void trial_msi_report(void)
{
    fprintf(stderr, "MSI_APIC_COUNTS locked=%u unlocked=%u\n",
            qatomic_read(&trial_msi_locked), qatomic_read(&trial_msi_unlocked));
}

static void __attribute__((constructor)) trial_msi_init(void)
{
    atexit(trial_msi_report);
}


/* Quiesce changes to target lifetime or pending-state ownership. vCPUs
 * publish only to their own reader slot and never wait for a writer. */
static unsigned ipi_quiesce_depth;
/* PCI MSI senders may run outside any vCPU thread. */
static unsigned msi_senders;
static bool msi_paused = true;
static uint64_t ipi_route_started;
static uint64_t ipi_route_completed;
static uint32_t local_apic_slots[MAX_APIC_WORDS];

static bool apic_ipi_enabled(void)
{
#ifdef CONFIG_LATX_OPT_BQL
    return option_irq_kick_fast && qemu_tcg_mode_2_lat() &&
           qemu_tcg_mttcg_enabled() && !icount_enabled() &&
           replay_mode == REPLAY_MODE_NONE;
#else
    return false;
#endif
}

static void apic_msi_drain(void)
{
    while (qatomic_load_acquire(&msi_senders)) {
        g_usleep(20);
    }
}

void apic_set_msi_paused(bool paused)
{
    if (paused) {
        /* Close the sender gate before inspecting the reader count. */
        qatomic_set_mb(&msi_paused, true);
        hpet_irq0_revoke_route();
        apic_msi_drain();
    } else {
        qatomic_store_release(&msi_paused, false);
    }
}

static QemuMutex apic_quiesce_mutex;
static gsize apic_quiesce_mutex_init;

static void apic_quiesce_lock_ensure(void)
{
    if (g_once_init_enter(&apic_quiesce_mutex_init)) {
        qemu_mutex_init(&apic_quiesce_mutex);
        g_once_init_leave(&apic_quiesce_mutex_init, 1);
    }
}

void apic_ipi_quiesce(void)
{
    if (!apic_ipi_enabled()) {
        return;
    }
    apic_quiesce_lock_ensure();
    qemu_mutex_lock(&apic_quiesce_mutex);
    if (qatomic_fetch_inc(&ipi_quiesce_depth) == 0) {
        /* Pairs with the sender's registration before it reads the gate. */
        smp_mb__after_rmw();
        apic_msi_drain();
        for (unsigned i = 0; i < ARRAY_SIZE(local_apics); ++i) {
            APICCommonState *s = local_apics[i];

            if (s) {
                while (qatomic_load_acquire(&s->ipi_sending)) {
                    g_usleep(20);
                }
            }
        }
    }
    qemu_mutex_unlock(&apic_quiesce_mutex);
}

void apic_ipi_resume(void)
{
    if (apic_ipi_enabled()) {
        apic_quiesce_lock_ensure();
        qemu_mutex_lock(&apic_quiesce_mutex);
        qatomic_dec(&ipi_quiesce_depth);
        qemu_mutex_unlock(&apic_quiesce_mutex);
    }
}

/*
 * Guest route-register writes already have BQL, except APIC-base MSRs.
 * The counters also handle overlapping writes to separate APICs and
 * keep a mixed route scan from publishing anything.
 */
static void apic_route_write(uint8_t *reg, uint8_t value)
{
    qatomic_inc(&ipi_route_started);
    smp_mb__after_rmw();
    /* A worker that has already validated this route has a live record.
     * A worker publishing its record later sees the changed route version. */
    uint64_t decision = qatomic_load_acquire(&irq_commit.decision);
    if ((decision >> 2) && (decision & 3) == IRQ_COMMIT_PREPARING) {
        irq_commit_cancel(IRQ_COMMIT_TAG | (decision >> 2));
    }
    qatomic_set(reg, value);
    qatomic_inc(&ipi_route_completed);
}

void apic_set_id(DeviceState *dev, uint8_t id)
{
    apic_route_write(&APIC_COMMON(dev)->id, id);
}


#define TYPE_APIC "apic"
/*This is reusing the APICCommonState typedef from APIC_COMMON */
DECLARE_INSTANCE_CHECKER(APICCommonState, APIC,
                         TYPE_APIC)

static void apic_set_irq(APICCommonState *s, int vector_num, int trigger_mode);
static void apic_update_irq(APICCommonState *s);
static void apic_get_delivery_bitmask(uint32_t *deliver_bitmask,
                                      uint8_t dest, uint8_t dest_mode);

/* Find first bit starting from msb */
static int apic_fls_bit(uint32_t value)
{
    return 31 - clz32(value);
}

/* Find first bit starting from lsb */
static int apic_ffs_bit(uint32_t value)
{
    return ctz32(value);
}

static inline void apic_reset_bit(uint32_t *tab, int index)
{
    int i;
    uint32_t mask;
    i = index >> 5;
    mask = 1U << (index & 0x1f);
    qatomic_and(&tab[i], ~mask);
}

/* return -1 if no bit is set */
static int get_highest_priority_int(uint32_t *tab)
{
    int i;
    for (i = 7; i >= 0; i--) {
        uint32_t value = qatomic_read(&tab[i]);
        if (value != 0) {
            return i * 32 + apic_fls_bit(value);
        }
    }
    return -1;
}

static void apic_expiry_trial(unsigned stage);
#define APIC_EXPIRY_TRIAL(stage) apic_expiry_trial(stage)

/*
 * Keep the existing 64-bit IRR/TMR word for ordinary values. Two header
 * values identify an in-flight IRQ commit or an alternate backing word.
 * Only a writer whose logical value would collide with a header needs the
 * alternate backing word. Move the word once, preserving all 64 bits.
 */
#define PENDING_ESCAPE UINT64_C(0xa524000000000000)
#define PENDING_SLOT_BITS 12
#define PENDING_SLOT_MASK ((1U << PENDING_SLOT_BITS) - 1)

typedef struct {
    uint64_t serial;
    uint64_t values[2];
} APICPendingWriter;

struct APICPendingExtension {
    unsigned actors;
    QemuSpin host_msi_lock;
    uint64_t alternate[8];
    APICPendingWriter writers[];
};

typedef struct {
    uint64_t *head;
    APICPendingWriter *writers;
} APICPendingCell;

static bool pending_is_tag(uint64_t value)
{
    uint64_t kind = value & IRQ_COMMIT_TAG_MASK;

    return kind == IRQ_COMMIT_TAG || kind == PENDING_ESCAPE;
}

static uint64_t pending_payload(APICPendingCell cell, uint64_t word)
{
    if (cell.writers) {
        return qatomic_read(&cell.writers[word & PENDING_SLOT_MASK].values[
                            (word >> PENDING_SLOT_BITS) & 1]);
    }
    return word;
}

static uint64_t pending_prepare(APICPendingCell cell, uint64_t old,
                                unsigned actor, uint64_t value)
{
    APICPendingWriter *writer = &cell.writers[actor];
    unsigned slot = ((old & PENDING_SLOT_MASK) == actor) ?
                    1 ^ ((old >> PENDING_SLOT_BITS) & 1) : 0;
    uint64_t serial = ++writer->serial;

    assert(serial < (IRQ_COMMIT_PAYLOAD >> (PENDING_SLOT_BITS + 2)));
    qatomic_set(&writer->values[slot], value);
    return (serial << (PENDING_SLOT_BITS + 1)) |
           ((uint64_t)slot << PENDING_SLOT_BITS) | actor;
}

static APICPendingCell pending_select(APICCommonState *s, unsigned index,
                                      uint64_t *raw)
{
    APICPendingCell cell = { .head = &s->pending[index] };

    *raw = qatomic_load_acquire(cell.head);
    if ((*raw & IRQ_COMMIT_TAG_MASK) == PENDING_ESCAPE) {
        struct APICPendingExtension *ext = s->pending_ext;
        uint64_t first = (*raw & IRQ_COMMIT_PAYLOAD) >> 1;

        cell.head = &ext->alternate[index];
        cell.writers = &ext->writers[index * ext->actors];
        *raw = qatomic_load_acquire(cell.head);
        if (!*raw) {
            *raw = qatomic_cmpxchg(cell.head, 0, first);
            if (!*raw) {
                *raw = first;
            }
        }
    }
    return cell;
}

static bool pending_promote(APICCommonState *s, unsigned index,
                            uint64_t raw, unsigned actor, uint64_t value)
{
    struct APICPendingExtension *ext = s->pending_ext;
    APICPendingCell cell = {
        .head = &ext->alternate[index],
        .writers = &ext->writers[index * ext->actors],
    };
    uint64_t token = pending_prepare(cell, 0, actor, value);
    uint64_t next = PENDING_ESCAPE | (token << 1) | 1;

    return qatomic_cmpxchg(&s->pending[index], raw, next) == raw;
}

static uint64_t apic_pending_load(APICCommonState *s, unsigned index)
{
    if (likely(!s->pending_ext)) {
        return qatomic_read(&s->pending[index]);
    }
    for (;;) {
        uint64_t raw, token, value;
        APICPendingCell cell = pending_select(s, index, &raw);

        token = raw;
        if ((raw & IRQ_COMMIT_TAG_MASK) == IRQ_COMMIT_TAG) {
            uint64_t decision = qatomic_load_acquire(&irq_commit.decision);

            if ((decision >> 2) != (raw & IRQ_COMMIT_PAYLOAD)) {
                continue;
            }
            token = irq_commit_value(decision, IRQ_COMMIT_APIC);
        }
        /* Check before decoding the token and again after the slot load.
         * A later attempt can reuse the record for a different target. */
        smp_rmb();
        if (qatomic_read(cell.head) != raw) {
            continue;
        }
        value = pending_payload(cell, token);
        smp_rmb();
        if (qatomic_read(cell.head) == raw) {
            return value;
        }
    }
}

static unsigned pending_actor(APICCommonState *s, bool timer)
{
    unsigned actors = s->pending_ext->actors;

    if (current_cpu) {
        unsigned actor = 2 * current_cpu->cpu_index + timer;

        g_assert(actor < actors - 3);
        return actor;
    }
    g_assert(!timer && qemu_mutex_iothread_locked());
    return actors - 3;
}

static uint64_t apic_pending_change_by(APICCommonState *s, unsigned index,
                                       uint64_t clear, uint64_t set,
                                       unsigned actor)
{
    for (;;) {
        uint64_t raw, old, next;
        APICPendingCell cell = pending_select(s, index, &raw);

        if ((raw & IRQ_COMMIT_TAG_MASK) == IRQ_COMMIT_TAG) {
            irq_commit_cancel(raw);
            irq_commit_fold(cell.head, raw, IRQ_COMMIT_APIC);
            continue;
        }
        old = pending_payload(cell, raw);
        next = (old & ~clear) | set;

        if (cell.writers) {
            next = pending_prepare(cell, raw, actor, next);
        } else if (pending_is_tag(next)) {
            if (pending_promote(s, index, raw, actor, next)) {
                return old;
            }
            continue;
        }
        if (qatomic_cmpxchg(cell.head, raw, next) == raw) {
            return old;
        }
    }
}

/*
 * Configuration owners serialize their existing register changes and
 * replace this lease before changing anything the expiry consumes.
 * The lease version must change even when the guest writes the same bits.
 */
void apic_expiry_change_lease(uint64_t *lease, uint64_t next);
void apic_expiry_change_lease(uint64_t *lease, uint64_t next)
{
    g_assert(!pending_is_tag(next));
    irq_commit_source_update(lease, UINT64_MAX, next);
}

uint64_t apic_expiry_lease_value(uint64_t *lease);
uint64_t apic_expiry_lease_value(uint64_t *lease)
{
    return irq_commit_source_read(lease);
}

/* One worker, no synchronous acquisition of BQL. */
bool apic_expiry_commit(DeviceState *target, unsigned vector, uint64_t *lease,
                        uint64_t expected, uint64_t retired);
static void scsi_commit_test_pin(unsigned vector)
{
    const char *dir = g_getenv("VIRTIO_TRIAL_PIN_DIR");
    g_autofree char *pinned = NULL;
    g_autofree char *release = NULL;

    if (!dir || vector != 0x59) {
        return;
    }
    pinned = g_build_filename(dir, "pinned", NULL);
    release = g_build_filename(dir, "release", NULL);
    g_assert(g_file_set_contents(pinned, "", 0, NULL));
    while (!g_file_test(release, G_FILE_TEST_EXISTS)) {
        g_usleep(1000);
    }
}

static bool apic_expiry_trial_active(void);

static bool apic_commit_edge(DeviceState *target, unsigned vector,
                             uint64_t *lease, uint64_t expected,
                             uint64_t retired, const uint64_t *route,
                            bool scsi)
{
    APICCommonState *s = APIC(target);
    unsigned index = vector >> 5;
    uint64_t bit = UINT64_C(1) << (vector & 31);
    unsigned actor;
    unsigned max_attempts;

    if (!s->pending_ext || vector < 16 || vector > 255 ||
        pending_is_tag(expected) || pending_is_tag(retired)) {
        return false;
    }
    actor = s->pending_ext->actors - 1;
    max_attempts = (apic_expiry_trial_active() ||
                    (scsi && g_getenv("LATS_SCSI_COMMIT_TEST_DIR"))) ? 1 : 8;

    for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
        uint64_t raw, before, after, tag, preparing, decision;
        APICPendingCell cell;

        if (attempt > 0) {
            cpu_relax();
        }
        cell = pending_select(s, index, &raw);
        if ((raw & IRQ_COMMIT_TAG_MASK) == IRQ_COMMIT_TAG) {
            continue;
        }
        before = pending_payload(cell, raw);
        after = before | bit | (bit << 32);
        if (!cell.writers && pending_is_tag(after)) {
            if (!pending_promote(s, index, raw, actor, before)) {
                continue;
            }
            cell = pending_select(s, index, &raw);
            if ((raw & IRQ_COMMIT_TAG_MASK) == IRQ_COMMIT_TAG) {
                continue;
            }
            after = pending_payload(cell, raw) | bit | (bit << 32);
        }
        if (cell.writers) {
            after = pending_prepare(cell, raw, actor, after);
        }

        g_assert(irq_commit.serial < (IRQ_COMMIT_PAYLOAD >> 1));
        tag = IRQ_COMMIT_TAG | (++irq_commit.serial << 1) | 1;
        preparing = irq_commit_stamp(tag, IRQ_COMMIT_PREPARING);
        decision = irq_commit_stamp(tag, IRQ_COMMIT_CANCELLED);
        qatomic_set(&irq_commit.before[IRQ_COMMIT_SOURCE], expected);
        qatomic_set(&irq_commit.after[IRQ_COMMIT_SOURCE], retired);
        qatomic_set(&irq_commit.before[IRQ_COMMIT_APIC], raw);
        qatomic_set(&irq_commit.after[IRQ_COMMIT_APIC], after);
        qatomic_store_release(&irq_commit.decision, preparing);
        /* Insert the APIC record first, then validate the live HPET lease. */
        APIC_EXPIRY_TRIAL(1);
        if (qatomic_cmpxchg(cell.head, raw, tag) == raw &&
            qatomic_cmpxchg(lease, expected, tag) == expected) {
            APIC_EXPIRY_TRIAL(2);
            /* The SCSI test may wait for a guest operation under the BQL. */
            if (scsi) {
                scsi_commit_test_pin(vector);
            }
            if (!route || *route == qatomic_load_acquire(&ipi_route_started)) {
                decision = irq_commit_stamp(tag, IRQ_COMMIT_COMMITTED);
            }
        }
        qatomic_cmpxchg(&irq_commit.decision, preparing, decision);
        bool committed = qatomic_read(&irq_commit.decision) ==
                         irq_commit_stamp(tag, IRQ_COMMIT_COMMITTED);
        if (committed) {
            CPUState *cpu = CPU(s->cpu);

            qatomic_or(&cpu->interrupt_request, CPU_INTERRUPT_POLL);
            smp_wmb();
        }
        APIC_EXPIRY_TRIAL(3);
        irq_commit_fold(lease, tag, IRQ_COMMIT_SOURCE);
        irq_commit_fold(cell.head, tag, IRQ_COMMIT_APIC);
        if (committed) {
            return true;
        }
        /* Do not retry if the source lease or APIC route actually changed. */
        if (qatomic_read(lease) != expected ||
            (route && *route != qatomic_load_acquire(&ipi_route_started))) {
            return false;
        }
    }
    return false;
}

static bool apic_commit_pinned_edge(DeviceState *target, unsigned vector,
                                    uint64_t *lease, uint64_t expected,
                                    uint64_t retired, const uint64_t *route,
                            bool scsi)
{
    static bool sender_busy;
    unsigned spins = 0;
    unsigned max_spins = (apic_expiry_trial_active() ||
                          (scsi && g_getenv("LATS_SCSI_COMMIT_TEST_DIR"))) ? 128 : 4096;
    bool handled;

    /*
     * Exactly one writer may reuse the shared record and its spare slot.
     * Keep the critical section free of blocking syscalls (halt_cond wake)
     * and spin briefly on transient cross-thread contention (HPET, SCSI, net)
     * before falling back so test pins or preemption never deadlock.
     */
    while (qatomic_xchg(&sender_busy, true)) {
        if (++spins >= max_spins) {
            return false;
        }
        while (qatomic_read(&sender_busy)) {
            if (++spins >= max_spins) {
                return false;
            }
            if ((spins & 511) == 0) {
                g_thread_yield();
            } else {
                cpu_relax();
            }
        }
    }
    handled = apic_commit_edge(target, vector, lease, expected, retired,
                               route, scsi);
    qatomic_store_release(&sender_busy, false);
    if (handled) {
        CPUState *cpu = CPU(APIC(target)->cpu);

        qemu_cond_broadcast_3(cpu->halt_cond);
        lats_request_tb_exit(cpu);
    }
    return handled;
}

bool apic_expiry_commit(DeviceState *target, unsigned vector, uint64_t *lease,
                        uint64_t expected, uint64_t retired)
{
    return apic_commit_pinned_edge(target, vector, lease, expected,
                                   retired, NULL, false);
}

/* A receiver can acknowledge a vector while another CPU or the local
 * hardware timer posts it. Publish the trigger mode and IRR together. */
static uint64_t apic_pending_change(APICCommonState *s, unsigned index,
                                    uint64_t clear, uint64_t set, bool timer)
{
    if (!current_cpu && !qemu_mutex_iothread_locked()) {
        struct APICPendingExtension *ext = s->pending_ext;
        uint64_t old;

        g_assert(!timer);
        qemu_spin_lock(&ext->host_msi_lock);
        old = apic_pending_change_by(s, index, clear, set, ext->actors - 2);
        qemu_spin_unlock(&ext->host_msi_lock);
        return old;
    }
    return apic_pending_change_by(s, index, clear, set, pending_actor(s, timer));
}

static bool apic_post_vector_mode(APICCommonState *s, unsigned vector,
                                  bool level, bool timer)
{
    uint64_t *word = &s->pending[vector >> 5];
    uint64_t bit = UINT64_C(1) << (vector & 31);
    uint64_t old;

    if (unlikely(s->pending_ext)) {
        old = apic_pending_change(s, vector >> 5, level ? bit << 32 : 0,
                                  bit | (level ? 0 : bit << 32), timer);
    } else if (!level) {
        old = qatomic_fetch_or(word, bit | (bit << 32));
    } else {
        old = qatomic_read(word);
        for (;;) {
            uint64_t next = (old | bit) & ~(bit << 32);
            uint64_t actual = qatomic_cmpxchg(word, old, next);
            if (actual == old) {
                break;
            }
            old = actual;
        }
    }
    return !(old & bit);
}

static bool apic_post_vector(APICCommonState *s, unsigned vector, bool level)
{
    return apic_post_vector_mode(s, vector, level, false);
}

static bool apic_clear_pending(APICCommonState *s, unsigned vector)
{
    uint64_t bit = UINT64_C(1) << (vector & 31);
    if (unlikely(s->pending_ext)) {
        return apic_pending_change(s, vector >> 5, bit, 0, false) & bit;
    }
    return qatomic_fetch_and(&s->pending[vector >> 5], ~bit) & bit;
}

static int apic_highest_pending(APICCommonState *s)
{
    for (int i = 7; i >= 0; i--) {
        uint32_t irr = apic_pending_load(s, i);
        if (irr) {
            return 32 * i + apic_fls_bit(irr);
        }
    }
    return -1;
}

static uint32_t apic_tmr_word(APICCommonState *s, unsigned i)
{
    return ~(uint32_t)(apic_pending_load(s, i) >> 32);
}

static void apic_sync_vapic(APICCommonState *s, int sync_type)
{
    VAPICState vapic_state;
    size_t length;
    off_t start;
    int vector;

    if (!s->vapic_paddr) {
        return;
    }
    if (sync_type & SYNC_FROM_VAPIC) {
        cpu_physical_memory_read(s->vapic_paddr, &vapic_state,
                                 sizeof(vapic_state));
        s->cpu->env.tpr = vapic_state.tpr;
    }
    if (sync_type & (SYNC_TO_VAPIC | SYNC_ISR_IRR_TO_VAPIC)) {
        start = offsetof(VAPICState, isr);
        length = offsetof(VAPICState, enabled) - offsetof(VAPICState, isr);

        if (sync_type & SYNC_TO_VAPIC) {
            assert(qemu_cpu_is_self(CPU(s->cpu)));

            vapic_state.tpr = s->cpu->env.tpr;
            vapic_state.enabled = 1;
            start = 0;
            length = sizeof(VAPICState);
        }

        vector = get_highest_priority_int(s->isr);
        if (vector < 0) {
            vector = 0;
        }
        vapic_state.isr = vector & 0xf0;

        vapic_state.zero = 0;

        vector = apic_highest_pending(s);
        if (vector < 0) {
            vector = 0;
        }
        vapic_state.irr = vector & 0xff;

        address_space_write_rom(&address_space_memory,
                                s->vapic_paddr + start,
                                MEMTXATTRS_UNSPECIFIED,
                                ((void *)&vapic_state) + start, length);
    }
}

static void apic_vapic_base_update(APICCommonState *s)
{
    apic_sync_vapic(s, SYNC_TO_VAPIC);
}

static void apic_local_deliver(APICCommonState *s, int vector)
{
    uint32_t lvt = s->lvt[vector];
    int trigger_mode;

    trace_apic_local_deliver(vector, (lvt >> 8) & 7);

    if (lvt & APIC_LVT_MASKED)
        return;

    switch ((lvt >> 8) & 7) {
    case APIC_DM_SMI:
        cpu_interrupt(CPU(s->cpu), CPU_INTERRUPT_SMI);
        break;

    case APIC_DM_NMI:
        cpu_interrupt(CPU(s->cpu), CPU_INTERRUPT_NMI);
        break;

    case APIC_DM_EXTINT:
        cpu_interrupt(CPU(s->cpu), CPU_INTERRUPT_HARD);
        break;

    case APIC_DM_FIXED:
        trigger_mode = APIC_TRIGGER_EDGE;
        if ((vector == APIC_LVT_LINT0 || vector == APIC_LVT_LINT1) &&
            (lvt & APIC_LVT_LEVEL_TRIGGER))
            trigger_mode = APIC_TRIGGER_LEVEL;
        apic_set_irq(s, lvt & 0xff, trigger_mode);
#ifdef HTIMER_DEBUG
        if (vector == APIC_LVT_TIMER) {
            CPUState *cpu = CPU(s->cpu);
            HTIMER_LOG("[disable-htimer][cpu%d] %s\n",
                        cpu->cpu_index, __func__);
        }
#endif
    }
}

void apic_deliver_pic_intr(DeviceState *dev, int level)
{
    APICCommonState *s = APIC(dev);

    if (level) {
        apic_local_deliver(s, APIC_LVT_LINT0);
    } else {
        uint32_t lvt = s->lvt[APIC_LVT_LINT0];

        switch ((lvt >> 8) & 7) {
        case APIC_DM_FIXED:
            if (!(lvt & APIC_LVT_LEVEL_TRIGGER))
                break;
            apic_clear_pending(s, lvt & 0xff);
            /* fall through */
        case APIC_DM_EXTINT:
            apic_update_irq(s);
            break;
        }
    }
}

static void apic_external_nmi(APICCommonState *s)
{
    apic_local_deliver(s, APIC_LVT_LINT1);
}

#define foreach_apic(apic, deliver_bitmask, code) \
{\
    int __i, __j;\
    for(__i = 0; __i < MAX_APIC_WORDS; __i++) {\
        uint32_t __mask = deliver_bitmask[__i];\
        if (__mask) {\
            for(__j = 0; __j < 32; __j++) {\
                if (__mask & (1U << __j)) {\
                    apic = local_apics[__i * 32 + __j];\
                    if (apic) {\
                        code;\
                    }\
                }\
            }\
        }\
    }\
}

static int apic_lowpri_slot(const uint32_t *deliver_bitmask)
{
    /* Keep the existing first matching slot rule. */
    for (int word = 0; word < MAX_APIC_WORDS; ++word) {
        if (deliver_bitmask[word]) {
            return word * 32 + apic_ffs_bit(deliver_bitmask[word]);
        }
    }
    return -1;
}

static void apic_bus_deliver(const uint32_t *deliver_bitmask,
                             uint8_t delivery_mode, uint8_t vector_num,
                             uint8_t trigger_mode)
{
    APICCommonState *apic_iter;

    switch (delivery_mode) {
        case APIC_DM_LOWPRI: {
            int slot = apic_lowpri_slot(deliver_bitmask);

            if (slot >= 0 && local_apics[slot]) {
                apic_set_irq(local_apics[slot], vector_num, trigger_mode);
            }
            return;
        }

        case APIC_DM_FIXED:
            break;

        case APIC_DM_SMI:
            foreach_apic(apic_iter, deliver_bitmask,
                cpu_interrupt(CPU(apic_iter->cpu), CPU_INTERRUPT_SMI)
            );
            return;

        case APIC_DM_NMI:
            foreach_apic(apic_iter, deliver_bitmask,
                cpu_interrupt(CPU(apic_iter->cpu), CPU_INTERRUPT_NMI)
            );
            return;

        case APIC_DM_INIT:
            /* normal INIT IPI sent to processors */
            foreach_apic(apic_iter, deliver_bitmask,
                         cpu_interrupt(CPU(apic_iter->cpu),
                                       CPU_INTERRUPT_INIT)
            );
            return;

        case APIC_DM_EXTINT:
            /* handled in I/O APIC code */
            break;

        default:
            return;
    }

    foreach_apic(apic_iter, deliver_bitmask,
                 apic_set_irq(apic_iter, vector_num, trigger_mode) );
}

void apic_deliver_irq(uint8_t dest, uint8_t dest_mode, uint8_t delivery_mode,
                      uint8_t vector_num, uint8_t trigger_mode)
{
    uint32_t deliver_bitmask[MAX_APIC_WORDS];
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    bool unlocked = !qemu_mutex_iothread_locked();

    if (unlocked) {
        qatomic_inc(&msi_senders);
        smp_mb__after_rmw();
    }
#endif

    trace_apic_deliver_irq(dest, dest_mode, delivery_mode, vector_num,
                           trigger_mode);

    apic_get_delivery_bitmask(deliver_bitmask, dest, dest_mode);
    apic_bus_deliver(deliver_bitmask, delivery_mode, vector_num, trigger_mode);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    if (unlocked) {
        qatomic_dec(&msi_senders);
    }
#endif
}

static void apic_set_base(APICCommonState *s, uint64_t val)
{
    uint32_t base = (val & 0xfffff000) |
        (qatomic_read(&s->apicbase) &
         (MSR_IA32_APICBASE_BSP | MSR_IA32_APICBASE_ENABLE));
    bool disable = !(val & MSR_IA32_APICBASE_ENABLE);

    x86_hpet_irq0_external_begin();
    /* if disabled, cannot be enabled again */
    if (disable) {
        base &= ~MSR_IA32_APICBASE_ENABLE;
    }
    qatomic_set(&s->apicbase, base);
    if (disable) {
        cpu_clear_apic_feature(&s->cpu->env);
        s->spurious_vec &= ~APIC_SV_ENABLE;
        s->cpu->env.irrv = -1;
    }
    x86_hpet_irq0_external_end();
}

static int apic_irq_pending_fast_tpr(APICCommonState *s, int tpr);
static int apic_irq_pending(APICCommonState *s);
static void apic_set_tpr(APICCommonState *s, uint8_t val)
{
	int tpr = val << 4;
    qatomic_set(&s->cpu->env.tpr, tpr);
    CPUState *cpu;
    cpu = CPU(s->cpu);

    int irq_fast = apic_irq_pending_fast_tpr(s, tpr);
    if (irq_fast > 0) {
        qatomic_or(&cpu->interrupt_request, CPU_INTERRUPT_HARD);
        /* keep the request ordered before the exit flag (weak hosts) */
        smp_wmb();
        lats_request_tb_exit(cpu);
    }
}

/* CR8 belongs to the executing CPU. A VAPIC page allows a sender to
 * update TPR here; that configuration still uses the serialized setter. */
bool apic_try_set_local_tpr(DeviceState *dev, uint8_t value)
{
    if (!dev) {
        return true;
    }
    APICCommonState *s = APIC_COMMON(dev);
    if (s->vapic_paddr || APIC_COMMON_GET_CLASS(s)->set_tpr != apic_set_tpr ||
        !qemu_cpu_is_self(CPU(s->cpu))) {
        return false;
    }
    apic_set_tpr(s, value);
    return true;
}

int apic_get_highest_priority_irr(DeviceState *dev)
{
    APICCommonState *s;

    if (!dev) {
        /* no interrupts */
        return -1;
    }
    s = APIC_COMMON(dev);
    return apic_highest_pending(s);
}

static uint8_t apic_get_tpr(APICCommonState *s)
{
//    apic_sync_vapic(s, SYNC_FROM_VAPIC);
    return qatomic_read(&s->cpu->env.tpr) >> 4;
}

static inline void apic_update_isrv(APICCommonState *s)
{
    int isrv = get_highest_priority_int(s->isr);

    s->cpu->env.isrv = isrv < 0 ? 0 : (isrv & 0xf0);
}

int apic_get_ppr(APICCommonState *s)
{
    int tpr, isrv, ppr;

    tpr = qatomic_read(&s->cpu->env.tpr);
    isrv = get_highest_priority_int(s->isr);
    if (isrv < 0) {
        isrv = 0;
    }
    isrv >>= 4;
    if ((tpr >> 4) >= isrv)
        ppr = tpr;
    else
        ppr = isrv << 4;
    return ppr;
}

static int apic_get_arb_pri(APICCommonState *s)
{
    /* XXX: arbitration */
    return 0;
}


/*
 * <0 - low prio interrupt,
 * 0  - no interrupt,
 * >0 - interrupt number
 */
static int apic_irq_pending_fast_tpr(APICCommonState *s, int tpr)
{
    int irrv, ppr;
    irrv = s->cpu->env.irrv;
    if (irrv < 0) {
        return 0;
    }
    ppr = s->cpu->env.isrv;
    if (tpr >= ppr)
        ppr = tpr;

    if (ppr && (irrv & 0xf0) <= (ppr & 0xf0)) {
        return -1;
    }

    return irrv;
}

#ifdef __loongarch__
/* Exchange only TI. Leave IPI delivery and the exception vector spacing alone. */
static bool apic_exchange_local_timer(bool enable)
{
    unsigned value = enable ? ECFGF_TIMER : 0;

    asm volatile("csrxchg %0, %1, %2"
                 : "+r"(value)
                 : "r"(ECFGF_TIMER), "i"(LOONGARCH_CSR_ECFG)
                 : "memory");
    return value & ECFGF_TIMER;
}
#endif

static int apic_irq_pending(APICCommonState *s)
{
    int irrv, ppr;

    if (!(s->spurious_vec & APIC_SV_ENABLE)) {
        s->cpu->env.irrv = -1;
        return 0;
    }

#ifdef __loongarch__
    /* A local timer can publish a new IRR vector between the scan and
     * its cached result, even while this vCPU holds BQL. */
    bool restore_timer = option_htimer && hamt_is_running() &&
                         qemu_cpu_is_self(CPU(s->cpu)) &&
                         apic_exchange_local_timer(false);
#endif
    irrv = apic_highest_pending(s);
    s->cpu->env.irrv = irrv;
#ifdef __loongarch__
    if (restore_timer) {
        apic_exchange_local_timer(true);
    }
#endif
    if (irrv < 0) {
        return 0;
    }
    ppr = apic_get_ppr(s);
    if (ppr && (irrv & 0xf0) <= (ppr & 0xf0)) {
        return -1;
    }

    return irrv;
}

static void apic_update_irq(APICCommonState *s)
{
    CPUState *cpu;
    DeviceState *dev = (DeviceState *)s;

    cpu = CPU(s->cpu);
    if (!qemu_cpu_is_self(cpu)) {
        cpu_interrupt(cpu, CPU_INTERRUPT_POLL);
    } else if (apic_irq_pending(s) > 0) {
        cpu_interrupt(cpu, CPU_INTERRUPT_HARD);
    } else if (!apic_accept_pic_intr(dev) || !pic_get_output(isa_pic)) {
        cpu_reset_interrupt(cpu, CPU_INTERRUPT_HARD);
        /* A local timer interrupt may have arrived before the clear. */
        if (apic_irq_pending(s) > 0 ||
            (apic_accept_pic_intr(dev) && pic_get_output(isa_pic))) {
            cpu_interrupt(cpu, CPU_INTERRUPT_HARD);
        }
    }
}

void apic_local_deliver_timer(void)
{
    DeviceState *dev;
    APICCommonState *s;
    CPUState *cpu;

    dev = cpu_get_current_apic();
    if (!dev) {
        HTIMER_LOG("%s ERROR dev NULL!\n", __func__);
        return;
    }
    s = APIC(dev);
    cpu = CPU(s->cpu);
    HTIMER_LOG("[cpu%d] %s\n", cpu->cpu_index, __func__);

    uint32_t lvt = s->lvt[APIC_LVT_TIMER];
    int vector_num;

    if (lvt & APIC_LVT_MASKED)
        return;

    assert(((lvt >> 8) & 7) == APIC_DM_FIXED);

    vector_num = lvt & 0xff;
    apic_post_vector_mode(s, vector_num, false, true);
    int request;
    if (apic_irq_pending(s) > 0) {
        request = CPU_INTERRUPT_HARD;
    } else if (qatomic_read(&cpu->interrupt_request) & CPU_INTERRUPT_HARD) {
        /* CR8 may have risen. Reconcile HARD with both APIC and PIC under BQL. */
        request = CPU_INTERRUPT_POLL;
    } else {
        return;
    }
    g_assert(qemu_cpu_is_self(cpu));
    qatomic_or(&cpu->interrupt_request, request);
    smp_wmb();
    lats_request_tb_exit(cpu);
}

/* The running target vCPU owns its APIC priority state. Remote senders
 * publish the IRR bit before setting POLL. Claim POLL before scanning so
 * a concurrent sender either contributes to this scan or leaves a new POLL.
 */
static bool trial_combined_poll;
static bool trial_local_spurious;

bool apic_try_poll_irq(DeviceState *dev, int request)
{
    if (!dev) {
        return false;
    }
    APICCommonState *s = APIC(dev);
    CPUState *cpu = CPU(s->cpu);

    if (s->vapic_paddr) {
        return false;
    }
    if (qemu_vcpu_statelock_enabled() && cpu == current_cpu &&
        (request & CPU_INTERRUPT_POLL)) {
        /* Clear only POLL so concurrent HARD/PIC/control bits stay intact. */
        qatomic_and(&cpu->interrupt_request, ~CPU_INTERRUPT_POLL);
        if (apic_irq_pending(s) > 0) {
            qatomic_or(&cpu->interrupt_request, CPU_INTERRUPT_HARD);
        }
        return true;
    }
    if (request != CPU_INTERRUPT_POLL &&
        (!trial_combined_poll ||
         request != (CPU_INTERRUPT_POLL | CPU_INTERRUPT_HARD) ||
         cpu != current_cpu ||
         !(s->apicbase & MSR_IA32_APICBASE_ENABLE) ||
         !(s->lvt[APIC_LVT_LINT0] & APIC_LVT_MASKED))) {
        return false;
    }
    if (qatomic_cmpxchg(&cpu->interrupt_request, request, 0) != request) {
        return false;
    }
    /* A combined claim is allowed only with no PIC input. Scan IRR after
     * the claim and never clear any later request, even if IRR is masked. */
    if (apic_irq_pending(s) > 0) {
        qatomic_or(&cpu->interrupt_request, CPU_INTERRUPT_HARD);
    }
    return true;
}

/* Claim one ordinary LAPIC interrupt on its own running vCPU. A legacy PIC
 * can withdraw a level-triggered LINT0 vector, so keep PIC arbitration on
 * the serialized path whenever this CPU accepts external PIC interrupts. */
int apic_try_ack_local_hard(DeviceState *dev)
{
    if (!dev) {
        return -1;
    }
    APICCommonState *s = APIC(dev);
    CPUState *cpu = CPU(s->cpu);
    if (current_cpu != cpu || s->vapic_paddr ||
        !(s->apicbase & MSR_IA32_APICBASE_ENABLE) ||
        !(s->spurious_vec & APIC_SV_ENABLE) ||
        !(s->lvt[APIC_LVT_LINT0] & APIC_LVT_MASKED)) {
        return -1;
    }

#ifdef __loongarch__
    /* TI can update IRR and cached priority between any two host insns. */
    bool restore_timer = option_htimer && hamt_is_running() &&
                         apic_exchange_local_timer(false);
#endif
    int vector = apic_irq_pending(s);
    if (vector == 0 || (vector < 0 && !qatomic_read(&trial_local_spurious)) ||
        qatomic_cmpxchg(&cpu->interrupt_request, CPU_INTERRUPT_HARD, 0) !=
        CPU_INTERRUPT_HARD) {
        vector = -1;
        goto out;
    }

    if (vector < 0) {
        /*
         * Match apic_get_interrupt() after claiming the request. A sender
         * may have published a higher-priority vector since the first scan.
         */
        vector = apic_irq_pending(s);
        if (vector < 0) {
            vector = s->spurious_vec & 0xff;
            goto out;
        }
    }
    if (vector == 0 || !apic_clear_pending(s, vector)) {
        /* No vector was consumed. Restore the request for full arbitration. */
        qatomic_or(&cpu->interrupt_request, CPU_INTERRUPT_HARD);
        vector = -1;
        goto out;
    }

    apic_set_bit(s->isr, vector);
    s->cpu->env.isrv = vector & 0xf0;
    if (apic_irq_pending(s) > 0) {
        qatomic_or(&cpu->interrupt_request, CPU_INTERRUPT_HARD);
    }

out:
#ifdef __loongarch__
    if (restore_timer) {
        apic_exchange_local_timer(true);
    }
#endif
    return vector;
}

/*
 * BQL-free arbitration for legacy PIC (APIC disabled or LINT0 unmasked) and
 * mixed LAPIC+PIC delivery under pic_shared_lock().
 */
int apic_try_ack_pic_or_local_hard(DeviceState *dev, CPUState *cs)
{
    APICCommonState *s = dev ? APIC(dev) : NULL;
    int vector = -1;

    if (!qemu_vcpu_statelock_enabled() || current_cpu != cs ||
        (s && s->vapic_paddr)) {
        return -1;
    }

    qemu_rec_mutex_lock(pic_shared_lock());
    qatomic_and(&cs->interrupt_request, ~(CPU_INTERRUPT_HARD |
                                          CPU_INTERRUPT_VIRQ));

    if (!s || !(qatomic_read(&s->apicbase) & MSR_IA32_APICBASE_ENABLE) ||
        !(s->spurious_vec & APIC_SV_ENABLE)) {
        if (dev ? apic_accept_pic_intr(dev) : (isa_pic != NULL)) {
            vector = pic_read_irq(isa_pic);
        }
        qemu_rec_mutex_unlock(pic_shared_lock());
        return vector;
    }

#ifdef __loongarch__
    bool restore_timer = option_htimer && hamt_is_running() &&
                         apic_exchange_local_timer(false);
#endif
    int intno = apic_irq_pending(s);
    if (intno == 0 || (apic_accept_pic_intr(dev) && pic_get_output(isa_pic))) {
        if (apic_accept_pic_intr(dev)) {
            vector = pic_read_irq(isa_pic);
        }
        /* Preserve any pending LAPIC vector even if PIC lowered INTR. */
        if (apic_irq_pending(s) > 0) {
            qatomic_or(&cs->interrupt_request, CPU_INTERRUPT_HARD);
        }
    } else if (intno < 0) {
        vector = s->spurious_vec & 0xff;
    } else if (apic_clear_pending(s, intno)) {
        apic_set_bit(s->isr, intno);
        s->cpu->env.isrv = intno & 0xf0;
        if (apic_irq_pending(s) > 0) {
            qatomic_or(&cs->interrupt_request, CPU_INTERRUPT_HARD);
        }
        vector = intno;
    } else {
        qatomic_or(&cs->interrupt_request, CPU_INTERRUPT_HARD);
        vector = -1;
    }
#ifdef __loongarch__
    if (restore_timer) {
        apic_exchange_local_timer(true);
    }
#endif
    qemu_rec_mutex_unlock(pic_shared_lock());
    return vector;
}

void apic_poll_irq(DeviceState *dev)
{
    APICCommonState *s = APIC(dev);

    apic_sync_vapic(s, SYNC_FROM_VAPIC);
    apic_update_irq(s);
}

static void apic_set_irq(APICCommonState *s, int vector_num, int trigger_mode)
{
    kvm_report_irq_delivered(apic_post_vector(s, vector_num, trigger_mode));
    if (s->vapic_paddr) {
        apic_sync_vapic(s, SYNC_ISR_IRR_TO_VAPIC);
        /*
         * The vcpu thread needs to see the new IRR before we pull its current
         * TPR value. That way, if we miss a lowering of the TRP, the guest
         * has the chance to notice the new IRR and poll for IRQs on its own.
         */
        smp_wmb();
        apic_sync_vapic(s, SYNC_FROM_VAPIC);
    }
    apic_update_irq(s);
}

static bool apic_eoi_needs_broadcast(APICCommonState *s, int vector)
{
    return !(s->spurious_vec & APIC_SV_DIRECTED_IO) &&
           (apic_tmr_word(s, vector >> 5) & (1u << (vector & 31)));
}

static IOThreadLockAuto *apic_eoi_lock(APICCommonState *s, hwaddr addr)
{
    qemu_mutex_lock_iothread_mmio_profile_impl(&s->io_memory, "apic-msi",
                                              addr, true, __FILE__, __LINE__);
    return (IOThreadLockAuto *)(uintptr_t)1;
}

static void apic_eoi(APICCommonState *s, hwaddr addr)
{
    CPUState *cpu = CPU(s->cpu);
    bool have_bql = qemu_mutex_iothread_locked();
    g_autoptr(IOThreadLockAuto) guard = NULL;
    int isrv = get_highest_priority_int(s->isr);
    bool broadcast;

    if (!have_bql && (!qemu_cpu_is_self(cpu) || cpu->singlestep_enabled ||
                      s->vapic_paddr ||
                      (!lats_nobql_enabled() && isrv >= 0 &&
                       apic_eoi_needs_broadcast(s, isrv)))) {
        guard = apic_eoi_lock(s, addr);
        isrv = get_highest_priority_int(s->isr);
    }
    if (isrv < 0) {
        return;
    }

    apic_reset_bit(s->isr, isrv);
    apic_update_isrv(s);

    /*
     * Read TMR after clearing ISR, as in the serialized path. A concurrent
     * level post can require a broadcast even if the first check saw edge.
     * In that case finish this vector, without clearing another ISR bit.
     */
    broadcast = apic_eoi_needs_broadcast(s, isrv);
    if (!have_bql && !guard && !broadcast) {
        /*
         * Only the owner or its hardware timer changes cached IRR. A
         * concurrent sender publishes its own interrupt recheck request.
         */
        if (!option_htimer && apic_highest_pending(s) < 0 &&
            !(qatomic_read(&cpu->interrupt_request) & CPU_INTERRUPT_HARD)) {
            s->cpu->env.irrv = -1;
        } else {
            qatomic_or(&cpu->interrupt_request, CPU_INTERRUPT_POLL);
            /* Publish POLL before asking this CPU to leave the TB. */
            smp_wmb();
            lats_request_tb_exit(cpu);
        }
        return;
    }
    if (!have_bql && !guard && !lats_nobql_enabled()) {
        guard = apic_eoi_lock(s, addr);
        /* Same-vector edge IPIs can change TMR while BQL is contended. */
        broadcast = apic_eoi_needs_broadcast(s, isrv);
    }
    if (broadcast) {
        ioapic_eoi_broadcast(isrv);
    }
    apic_sync_vapic(s, SYNC_FROM_VAPIC | SYNC_TO_VAPIC);
    apic_update_irq(s);
}

static int apic_find_dest(uint8_t dest)
{
    APICCommonState *apic = local_apics[dest];
    int i;

    if (apic && qatomic_read(&apic->id) == dest)
        return dest;  /* shortcut in case apic->id == local_apics[dest]->id */

    for (i = 0; i < MAX_APICS; i++) {
        apic = local_apics[i];
        if (apic && qatomic_read(&apic->id) == dest)
            return i;
        if (!apic)
            break;
    }

    return -1;
}

static void apic_get_delivery_bitmask(uint32_t *deliver_bitmask,
                                      uint8_t dest, uint8_t dest_mode)
{
    APICCommonState *apic_iter;
    int i;

    if (dest_mode == 0) {
        if (dest == 0xff) {
            memset(deliver_bitmask, 0xff, MAX_APIC_WORDS * sizeof(uint32_t));
        } else {
            int idx = apic_find_dest(dest);
            memset(deliver_bitmask, 0x00, MAX_APIC_WORDS * sizeof(uint32_t));
            if (idx >= 0)
                apic_set_bit(deliver_bitmask, idx);
        }
    } else {
        /* XXX: cluster mode */
        memset(deliver_bitmask, 0x00, MAX_APIC_WORDS * sizeof(uint32_t));
        for(i = 0; i < MAX_APICS; i++) {
            apic_iter = local_apics[i];
            if (apic_iter) {
                uint8_t mode = qatomic_read(&apic_iter->dest_mode);
                uint8_t logical = qatomic_read(&apic_iter->log_dest);

                if (mode == 0xf) {
                    if (dest & logical) {
                        apic_set_bit(deliver_bitmask, i);
                    }
                } else if (mode == 0x0) {
                    if ((dest & 0xf0) == (logical & 0xf0) &&
                        (dest & logical & 0x0f)) {
                        apic_set_bit(deliver_bitmask, i);
                    }
                }
            } else {
                break;
            }
        }
    }
}

static void apic_startup(APICCommonState *s, int vector_num)
{
    s->sipi_vector = vector_num;
    cpu_interrupt(CPU(s->cpu), CPU_INTERRUPT_SIPI);
}

void apic_sipi(DeviceState *dev)
{
    APICCommonState *s = APIC(dev);

    cpu_reset_interrupt(CPU(s->cpu), CPU_INTERRUPT_SIPI);

    if (!s->wait_for_sipi)
        return;
    cpu_x86_load_seg_cache_sipi(s->cpu, s->sipi_vector);
    s->wait_for_sipi = 0;
}

static void apic_icr_destinations(APICCommonState *s, uint32_t low,
                                  uint32_t high, uint32_t *mask)
{
    switch ((low >> 18) & 3) {
    case 0:
        apic_get_delivery_bitmask(mask, high >> 24, (low >> 11) & 1);
        break;
    case 1:
        memset(mask, 0, sizeof(local_apic_slots));
        apic_set_bit(mask, qatomic_read(&s->id));
        break;
    case 2:
        memcpy(mask, local_apic_slots, sizeof(local_apic_slots));
        break;
    case 3:
        memcpy(mask, local_apic_slots, sizeof(local_apic_slots));
        apic_reset_bit(mask, qatomic_read(&s->id));
        break;
    }
}

static void apic_deliver(APICCommonState *s, uint32_t low, uint32_t high)
{
    uint32_t deliver_bitmask[MAX_APIC_WORDS];
    uint8_t delivery_mode = (low >> 8) & 7;
    uint8_t vector_num = low;
    uint8_t trigger_mode = (low >> 15) & 1;
    APICCommonState *apic_iter;

    apic_icr_destinations(s, low, high, deliver_bitmask);

    switch (delivery_mode) {
        case APIC_DM_INIT:
            {
                int level = (low >> 14) & 1;
                if (level == 0 && trigger_mode == 1) {
                    foreach_apic(apic_iter, deliver_bitmask,
                                 apic_iter->arb_id = qatomic_read(&apic_iter->id) );
                    return;
                }
            }
            break;

        case APIC_DM_SIPI:
            foreach_apic(apic_iter, deliver_bitmask,
                         apic_startup(apic_iter, vector_num) );
            return;
    }

    apic_bus_deliver(deliver_bitmask, delivery_mode, vector_num, trigger_mode);
}

static void apic_post_fixed_edge(APICCommonState *target, unsigned vector)
{
    CPUState *cpu = CPU(target->cpu);
    int request = CPU_INTERRUPT_POLL;

    /* The RTC's synchronous delivery count belongs to its own IRQ. */
    if (target->pending_ext && !current_cpu && !qemu_mutex_iothread_locked()) {
        struct APICPendingExtension *ext = target->pending_ext;
        uint64_t bit = UINT64_C(1) << (vector & 31);

        /* Only ordinary I/O senders use this slot. Never take BQL here. */
        qemu_spin_lock(&ext->host_msi_lock);
        apic_pending_change_by(target, vector >> 5, 0, bit | (bit << 32),
                               ext->actors - 2);
        qemu_spin_unlock(&ext->host_msi_lock);
    } else {
        apic_post_vector(target, vector, false);
    }
    if (cpu == current_cpu && apic_irq_pending(target) > 0) {
        request = CPU_INTERRUPT_HARD;
    }
    qatomic_or(&cpu->interrupt_request, request);
    /* Publish the request before waking the destination. */
    smp_wmb();
    if (cpu != current_cpu) {
        qemu_cond_broadcast_3(cpu->halt_cond);
    }
    lats_request_tb_exit(cpu);
}

static bool apic_try_send_icr(APICCommonState *s, uint32_t low, uint32_t high)
{
    APICCommonState *targets[MAX_APICS + 1];
    uint32_t mask[MAX_APIC_WORDS];
    unsigned count = 0;
    uint8_t delivery_mode = (low >> 8) & 7;
    uint8_t trigger_mode = (low >> 15) & 1;
    uint8_t level = (low >> 14) & 1;
    bool handled = false;

    if (current_cpu != CPU(s->cpu) || qatomic_read(&ipi_quiesce_depth)) {
        return false;
    }
    if (delivery_mode == APIC_DM_FIXED) {
        if (trigger_mode) {
            return false;
        }
    } else if (delivery_mode == APIC_DM_INIT) {
        if (!(level == 0 && trigger_mode == 1) && !lats_nobql_enabled()) {
            return false;
        }
    } else if (delivery_mode == APIC_DM_SIPI) {
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        if (!memory_region_device_lock_enabled()) {
            return false;
        }
#else
        return false;
#endif
    } else {
        return false;
    }

    /* Paired with closing admission before the control side reads slots. */
    qatomic_set_mb(&s->ipi_sending, true);
    if (qatomic_load_acquire(&ipi_quiesce_depth) ||
        local_apics[(uint8_t)s->initial_apic_id] != s || s->ipi_unplugged) {
        goto out;
    }

    uint64_t route = qatomic_load_acquire(&ipi_route_completed);
    apic_icr_destinations(s, low, high, mask);
    smp_rmb();
    if (route != qatomic_read(&ipi_route_started)) {
        goto out;
    }

    for (unsigned word = 0; word < ARRAY_SIZE(mask); ++word) {
        uint32_t bits = mask[word] & local_apic_slots[word];

        while (bits) {
            APICCommonState *target = local_apics[word * 32 + ctz32(bits)];

            if (target->vapic_paddr || target->ipi_unplugged ||
                qatomic_read(&CPU(target->cpu)->singlestep_enabled)) {
                goto out;
            }
            targets[count++] = target;
            bits &= bits - 1;
        }
    }

    qatomic_set(&s->icr[0], low);
    if (delivery_mode == APIC_DM_FIXED) {
        for (unsigned i = 0; i < count; ++i) {
            apic_post_fixed_edge(targets[i], low & 0xff);
        }
    } else if (delivery_mode == APIC_DM_INIT) {
        if (level == 0 && trigger_mode == 1) {
            for (unsigned i = 0; i < count; ++i) {
                targets[i]->arb_id = qatomic_read(&targets[i]->id);
            }
        } else {
            for (unsigned i = 0; i < count; ++i) {
                cpu_interrupt(CPU(targets[i]->cpu), CPU_INTERRUPT_INIT);
            }
        }
    } else if (delivery_mode == APIC_DM_SIPI) {
        for (unsigned i = 0; i < count; ++i) {
            apic_startup(targets[i], low & 0xff);
        }
    }
    handled = true;
out:
    qatomic_store_release(&s->ipi_sending, false);
    return handled;
}

void apic_ipi_unplug(DeviceState *dev)
{
    if (dev && apic_ipi_enabled()) {
        apic_ipi_quiesce();
        APIC_COMMON(dev)->ipi_unplugged = true;
        apic_ipi_resume();
    }
}

static bool apic_check_pic(APICCommonState *s)
{
    DeviceState *dev = (DeviceState *)s;

    if (!apic_accept_pic_intr(dev) || !pic_get_output(isa_pic)) {
        return false;
    }
    apic_deliver_pic_intr(dev, 1);
    return true;
}

int apic_get_interrupt(DeviceState *dev)
{
    APICCommonState *s = APIC(dev);
    int intno;

    /* if the APIC is installed or enabled, we let the 8259 handle the
       IRQs */
    if (!s)
        return -1;
    if (!(s->spurious_vec & APIC_SV_ENABLE))
        return -1;

    apic_sync_vapic(s, SYNC_FROM_VAPIC);
    intno = apic_irq_pending(s);

    /* if there is an interrupt from the 8259, let the caller handle
     * that first since ExtINT interrupts ignore the priority.
     */
    if (intno == 0 || apic_check_pic(s)) {
        apic_sync_vapic(s, SYNC_TO_VAPIC);
        return -1;
    } else if (intno < 0) {
        apic_sync_vapic(s, SYNC_TO_VAPIC);
        return s->spurious_vec & 0xff;
    }
    apic_clear_pending(s, intno);
    apic_set_bit(s->isr, intno);
    s->cpu->env.isrv = intno & 0xf0;
    apic_sync_vapic(s, SYNC_TO_VAPIC);

    apic_update_irq(s);

    return intno;
}

int apic_accept_pic_intr(DeviceState *dev)
{
    APICCommonState *s = APIC(dev);
    uint32_t lvt0;

    if (!s)
        return -1;

    lvt0 = s->lvt[APIC_LVT_LINT0];

    if ((qatomic_read(&s->apicbase) & MSR_IA32_APICBASE_ENABLE) == 0 ||
        (lvt0 & APIC_LVT_MASKED) == 0)
        return isa_pic != NULL;

    return 0;
}

static void apic_timer_update(APICCommonState *s, int64_t current_time)
{
    if (apic_next_timer(s, current_time)) {
        timer_mod(s->timer, s->next_time);
    } else {
        timer_del(s->timer);
    }
}

static void apic_timer(void *opaque)
{
    APICCommonState *s = opaque;

    apic_local_deliver(s, APIC_LVT_TIMER);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    qemu_rec_mutex_lock(&s->timer_lock);
#endif
    apic_timer_update(s, s->next_time);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    qemu_rec_mutex_unlock(&s->timer_lock);
#endif
}

static uint64_t apic_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    DeviceState *dev;
    APICCommonState *s;
    uint32_t val;
    int index;

    if (size < 4) {
        return 0;
    }

    dev = cpu_get_current_apic();
    if (!dev) {
        return 0;
    }
    s = APIC(dev);
#ifdef HTIMER_DEBUG
    CPUState *cpu = CPU(s->cpu);
#endif

    index = (addr >> 4) & 0xff;
    switch(index) {
    case 0x02: /* id */
        val = (uint32_t)qatomic_read(&s->id) << 24;
        break;
    case 0x03: /* version */
        val = s->version | ((APIC_LVT_NB - 1) << 16);
        break;
    case 0x08:
        apic_sync_vapic(s, SYNC_FROM_VAPIC);
        if (apic_report_tpr_access) {
            cpu_report_tpr_access(&s->cpu->env, TPR_ACCESS_READ);
        }
        val = qatomic_read(&s->cpu->env.tpr);
        break;
    case 0x09:
        val = apic_get_arb_pri(s);
        break;
    case 0x0a:
        /* ppr */
        val = apic_get_ppr(s);
        break;
    case 0x0b:
        val = 0;
        break;
    case 0x0d:
        val = s->log_dest << 24;
        break;
    case 0x0e:
        val = (s->dest_mode << 28) | 0xfffffff;
        break;
    case 0x0f:
        val = s->spurious_vec;
        break;
    case 0x10 ... 0x17:
        val = qatomic_read(&s->isr[index & 7]);
        break;
    case 0x18 ... 0x1f:
        val = apic_tmr_word(s, index & 7);
        break;
    case 0x20 ... 0x27:
        val = (uint32_t)apic_pending_load(s, index & 7);
        break;
    case 0x28:
        val = s->esr;
        break;
    case 0x30:
    case 0x31:
        val = qatomic_read(&s->icr[index & 1]);
        break;
    case 0x32 ... 0x37:
        val = s->lvt[index - 0x32];
        break;
    case 0x38:
        val = s->initial_count;
        break;
    case 0x39:
    /*
     * Refer to Intel SDM vol.3A Table 12-1:
     *     FEE0 0390H | Current Count Register (for Timer) | Read Only
     */
        if (option_htimer) {
            val = get_current_count_from_timertick(s);
        } else {
            val = apic_get_current_count(s);
            HTIMER_LOG("[disable-htimer][cpu%d] %s read x86_count = 0x%x\n",
                        cpu->cpu_index, __func__, val);
        }
        break;
    case 0x3e:
        val = s->divide_conf;
        break;
    default:
        s->esr |= APIC_ESR_ILLEGAL_ADDRESS;
        val = 0;
        break;
    }
    trace_apic_mem_readl(addr, val);
    return val;
}

static bool apic_is_msi_offset(hwaddr addr)
{
    return addr > 0xfff || !(addr & 0xff0);
}

static bool apic_msi_fast_enabled(void)
{
    /* The asynchronous exit notifier may take its timer mutex. */
    return apic_ipi_enabled() && option_int_opt != IRQ_CHECK_ASYNC_NOTIFY;
}

static bool apic_expiry_requested(void)
{
    return lats_nobql_enabled() &&
           !option_sys_aot && !option_aot_load && !option_aot_save;
}

bool apic_msi_commit_configured(void);
bool apic_msi_commit_configured(void)
{
    return apic_msi_fast_enabled() && apic_expiry_requested();
}

static int apic_msi_single_target(const uint32_t *mask)
{
    int slot = -1;

    for (int word = 0; word < MAX_APIC_WORDS; ++word) {
        uint32_t bits = mask[word];

        if (!bits) {
            continue;
        }
        if (slot >= 0 || (bits & (bits - 1))) {
            return -1;
        }
        slot = word * 32 + apic_ffs_bit(bits);
    }
    return slot;
}

static int apic_msi_target_slot(unsigned dest, bool logical, unsigned delivery)
{
    uint32_t mask[MAX_APIC_WORDS];

    if (!logical && dest != 0xff) {
        return apic_find_dest(dest);
    }
    apic_get_delivery_bitmask(mask, dest, logical);
    /* A fixed logical interrupt is multicast, not lowest priority.
     * A source commit can represent it only with exactly one target. */
    return delivery == APIC_DM_FIXED ? apic_msi_single_target(mask) :
                                       apic_lowpri_slot(mask);
}

static bool apic_try_send_msi(const MSIMessage *msi, uint64_t *source,
                              uint64_t before, uint64_t after, bool scsi)
{
    unsigned dest = (msi->address & MSI_ADDR_DEST_ID_MASK) >>
                    MSI_ADDR_DEST_ID_SHIFT;
    unsigned delivery = (msi->data >> MSI_DATA_DELIVERY_MODE_SHIFT) & 7;
    bool logical = msi->address & (1U << MSI_ADDR_DEST_MODE_SHIFT);
    bool handled = false;
    int slot;

    if (!apic_msi_fast_enabled() ||
        (delivery != APIC_DM_FIXED && delivery != APIC_DM_LOWPRI) ||
        (delivery == APIC_DM_FIXED && (dest == 0xff || (logical && !source))) ||
        (msi->data & (1U << MSI_DATA_TRIGGER_SHIFT)) ||
        !runstate_is_running() || qatomic_read(&msi_paused) ||
        qatomic_read(&ipi_quiesce_depth)) {
        return false;
    }

    /*
     * Pin the target against reset, unplug and VAPIC changes. The writer
     * closes admission before waiting for senders to leave.
     */
    qatomic_inc(&msi_senders);
    /* Publish the reader count before sampling either admission gate. */
    smp_mb__after_rmw();
    if (qatomic_load_acquire(&ipi_quiesce_depth) ||
        qatomic_load_acquire(&msi_paused) || !runstate_is_running()) {
        goto out;
    }

    uint64_t route = qatomic_load_acquire(&ipi_route_completed);

    slot = apic_msi_target_slot(dest, logical, delivery);

    /* Validate the route after reading the destination ID or logical mask. */
    smp_rmb();
    if (slot < 0 || route != qatomic_read(&ipi_route_started)) {
        goto out;
    }

    APICCommonState *target = local_apics[slot];

    if (target && !target->vapic_paddr && !target->ipi_unplugged &&
        !qatomic_read(&CPU(target->cpu)->singlestep_enabled)) {
        unsigned vector = msi->data & MSI_DATA_VECTOR_MASK;

        if (source) {
            handled = apic_commit_pinned_edge(DEVICE(target), vector,
                                              source, before, after,
                                              &route, scsi);
        } else {
            if (target->pending_ext && !current_cpu) {
                trace_apic_deliver_irq(dest, logical, delivery, vector, 0);
            }
            apic_post_fixed_edge(target, vector);
            handled = true;
        }
    }
out:
    qatomic_dec(&msi_senders);
    return handled;
}

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static bool apic_try_send_msi_dag(const MSIMessage *msi)
{
    uint64_t addr = msi->address;
    uint32_t data = msi->data;
    uint8_t dest = (addr & MSI_ADDR_DEST_ID_MASK) >> MSI_ADDR_DEST_ID_SHIFT;
    uint8_t dest_mode = (addr >> MSI_ADDR_DEST_MODE_SHIFT) & 0x1;
    uint8_t delivery = (data >> MSI_DATA_DELIVERY_MODE_SHIFT) & 0x7;
    uint8_t vector = (data & MSI_DATA_VECTOR_MASK) >> MSI_DATA_VECTOR_SHIFT;
    uint8_t trigger_mode = (data >> MSI_DATA_TRIGGER_SHIFT) & 0x1;
    uint32_t deliver_bitmask[MAX_APIC_WORDS];
    APICCommonState *apic_iter;
    bool handled = false;

    if (!memory_region_device_lock_enabled() || kvm_enabled() ||
        !runstate_is_running() || qatomic_read(&msi_paused) ||
        qatomic_read(&ipi_quiesce_depth)) {
        return false;
    }

    qatomic_inc(&msi_senders);
    smp_mb__after_rmw();
    if (qatomic_load_acquire(&ipi_quiesce_depth) ||
        qatomic_load_acquire(&msi_paused) || !runstate_is_running()) {
        goto out;
    }

    apic_get_delivery_bitmask(deliver_bitmask, dest, dest_mode);
    foreach_apic(apic_iter, deliver_bitmask, {
        if (apic_iter->vapic_paddr || apic_iter->ipi_unplugged) {
            goto out;
        }
    });

    trace_apic_deliver_irq(dest, dest_mode, delivery, vector, trigger_mode);
    apic_bus_deliver(deliver_bitmask, delivery, vector, trigger_mode);
    handled = true;

out:
    qatomic_dec(&msi_senders);
    return handled;
}
#endif

static void apic_send_msi(MSIMessage *msi)
{
    uint64_t addr = msi->address;
    uint32_t data = msi->data;
    uint8_t dest = (addr & MSI_ADDR_DEST_ID_MASK) >> MSI_ADDR_DEST_ID_SHIFT;
    uint8_t vector = (data & MSI_DATA_VECTOR_MASK) >> MSI_DATA_VECTOR_SHIFT;
    uint8_t dest_mode = (addr >> MSI_ADDR_DEST_MODE_SHIFT) & 0x1;
    uint8_t trigger_mode = (data >> MSI_DATA_TRIGGER_SHIFT) & 0x1;
    uint8_t delivery = (data >> MSI_DATA_DELIVERY_MODE_SHIFT) & 0x7;
    /* XXX: Ignore redirection hint. */
    apic_deliver_irq(dest, dest_mode, delivery, vector, trigger_mode);
}

static void apic_mem_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    DeviceState *dev;
    APICCommonState *s;
    int index = (addr >> 4) & 0xff;

    if (size < 4) {
        return;
    }
    /*printf("%s:%d addr %016lx val %lx size %x\n", __func__, __LINE__,*/
            /*addr, val, size);*/

    if (apic_is_msi_offset(addr)) {
        /* MSI and MMIO APIC are at the same memory location,
         * but actually not on the global bus: MSI is on PCI bus
         * APIC is connected directly to the CPU.
         * Mapping them on the global bus happens to work because
         * MSI registers are reserved in APIC MMIO and vice versa. */
        MSIMessage msi = { .address = addr, .data = val };
        g_autoptr(IOThreadLockAuto) guard = NULL;

        if (!qemu_mutex_iothread_locked()) {
            APICCommonState *region_owner = opaque;

            if (apic_try_send_msi(&msi, NULL, 0, 0, false)) {
                trial_msi_record(&msi);
                return;
            }
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
            if (apic_try_send_msi_dag(&msi)) {
                trial_msi_record(&msi);
                return;
            }
#endif
            qemu_mutex_lock_iothread_mmio_profile_impl(
                &region_owner->io_memory, "apic-msi", addr, true, __FILE__,
                __LINE__);
            guard = (IOThreadLockAuto *)(uintptr_t)1;
        }
        trial_msi_record(&msi);
        apic_send_msi(&msi);
        return;
    }

    dev = cpu_get_current_apic();
    if (!dev) {
        return;
    }
    s = APIC(dev);
#ifdef HTIMER_DEBUG
    CPUState *cpu = CPU(s->cpu);
#endif

    trace_apic_mem_writel(addr, val);

    switch(index) {
    case 0x02:
        apic_set_id(dev, val >> 24);
        break;
    case 0x03:
        break;
    case 0x08: {
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        g_autoptr(IOThreadLockAuto) guard = NULL;
        if (!qemu_mutex_iothread_locked() && !lats_nobql_enabled() &&
            (apic_report_tpr_access ||
             (qatomic_read(&s->vapic_paddr) &&
              !memory_region_device_lock_enabled()))) {
            qemu_mutex_lock_iothread_mmio_profile_impl(&s->io_memory, "apic-msi",
                                                      addr, true, __FILE__, __LINE__);
            guard = (IOThreadLockAuto *)(uintptr_t)1;
        }
#endif
        if (apic_report_tpr_access) {
            cpu_report_tpr_access(&s->cpu->env, TPR_ACCESS_WRITE);
        }
        qatomic_set(&s->cpu->env.tpr, val);
        apic_sync_vapic(s, SYNC_TO_VAPIC);
        apic_update_irq(s);
        break;
    }
    case 0x09:
    case 0x0a:
        break;
    case 0x0b: /* EOI */
        apic_eoi(s, addr);
        break;
    case 0x0d:
        apic_route_write(&s->log_dest, val >> 24);
        break;
    case 0x0e:
        apic_route_write(&s->dest_mode, val >> 28);
        break;
    case 0x0f: {
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        g_autoptr(IOThreadLockAuto) guard = NULL;
        if (!qemu_mutex_iothread_locked() &&
            (qatomic_read(&s->vapic_paddr) ||
             (!(val & APIC_SV_ENABLE) &&
              (qatomic_read(&s->spurious_vec) & APIC_SV_ENABLE)))) {
            qemu_mutex_lock_iothread_mmio_profile_impl(&s->io_memory, "apic-msi",
                                                      addr, true, __FILE__, __LINE__);
            guard = (IOThreadLockAuto *)(uintptr_t)1;
        }
#endif
        qatomic_set(&s->spurious_vec, val & 0x1ff);
        apic_update_irq(s);
        break;
    }
    case 0x10 ... 0x17:
    case 0x18 ... 0x1f:
    case 0x20 ... 0x27:
    case 0x28:
        break;
    case 0x30: {
        uint32_t low = val;
        uint32_t high = qatomic_read(&s->icr[1]);

        if (!qemu_mutex_iothread_locked() && apic_ipi_enabled() &&
            apic_try_send_icr(s, low, high)) {
            break;
        }

        g_autoptr(IOThreadLockAuto) guard = NULL;
        if (!qemu_mutex_iothread_locked()) {
            qemu_mutex_lock_iothread_mmio_profile_impl(&s->io_memory, "apic-msi",
                                                      addr, true, __FILE__, __LINE__);
            guard = (IOThreadLockAuto *)(uintptr_t)1;
        }
        qatomic_set(&s->icr[0], low);
        apic_deliver(s, low, high);
        break;
    }
    case 0x31:
        qatomic_set(&s->icr[1], val);
        break;
    case 0x32 ... 0x37:
    /*
     * Refer to Intel SDM vol.3A 12.5.1:
     *     FEE0 0320H : LVT Timer Register
     *     FEE0 0330H : LVT Thermal Monitor Register
     *     FEE0 0340H : LVT Performance Counter Register
     *     FEE0 0350H : LVT LINT0 Register
     *     FEE0 0360H : LVT LINT1 Register
     *     FEE0 0370H : LVT Error Register
     */
        {
            int n = index - 0x32;
            if (n == APIC_LVT_LINT0) {
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
                if (!qemu_mutex_iothread_locked()) {
                    hpet_irq0_external_begin();
                    s->lvt[n] = val;
                    hpet_irq0_external_end();
                } else
#endif
                {
                    hpet_irq0_revoke_route();
                    s->lvt[n] = val;
                }
            } else if (n == APIC_LVT_TIMER) {
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
                qemu_rec_mutex_lock(&s->timer_lock);
#endif
                s->lvt[n] = val;
                if (option_htimer) {
                    update_tcfg(s);
                } else {
                    HTIMER_LOG("[disable-htimer][cpu%d] %s lvtt = 0x%lx\n",
                                cpu->cpu_index, __func__, val);
                    apic_timer_update(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
                }
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
                qemu_rec_mutex_unlock(&s->timer_lock);
#endif
            } else {
                s->lvt[n] = val;
            }
            if (n == APIC_LVT_LINT0 && apic_check_pic(s)) {
                apic_update_irq(s);
            }
        }
        break;
    case 0x38:
    /*
     * Refer to Intel SDM vol.3A Table 12-1:
     *     FEE0 0380H | Initial Count Register (for Timer) | Read/Write
     */
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        qemu_rec_mutex_lock(&s->timer_lock);
#endif
        s->initial_count = val;
        if (option_htimer) {
            update_tcfg(s);
        } else {
            HTIMER_LOG("[disable-htimer][cpu%d] %s initial_count = 0x%x\n",
                        cpu->cpu_index, __func__, s->initial_count);
            s->initial_count_load_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            apic_timer_update(s, s->initial_count_load_time);
        }
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        qemu_rec_mutex_unlock(&s->timer_lock);
#endif
        break;
    case 0x39:
        break;
    case 0x3e:
        {
            int v;
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
            qemu_rec_mutex_lock(&s->timer_lock);
#endif
            s->divide_conf = val & 0xb;
            v = (s->divide_conf & 3) | ((s->divide_conf >> 1) & 4);
            s->count_shift = (v + 1) & 7;
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
            qemu_rec_mutex_unlock(&s->timer_lock);
#endif
        }
        break;
    default:
        s->esr |= APIC_ESR_ILLEGAL_ADDRESS;
        break;
    }
}

bool latx_apic_try_write_unlocked(uint64_t addr, uint64_t val, unsigned size);
bool latx_apic_try_write_unlocked(uint64_t addr, uint64_t val, unsigned size)
{
    DeviceState *dev;
    APICCommonState *s;
    uint64_t offset = addr & 0xfff;

    if (size < 4 || qemu_mutex_iothread_locked()) {
        return false;
    }
    if (offset == 0x300) {
        if (size != 32 || !apic_ipi_enabled()) {
            return false;
        }
        dev = cpu_get_current_apic();
        if (!dev) {
            return true;
        }
        s = APIC(dev);
        if (!apic_try_send_icr(s, val, qatomic_read(&s->icr[1]))) {
            return false;
        }
        trace_apic_mem_writel(0x300, val);
        return true;
    }
    if (offset == 0x310) {
        dev = cpu_get_current_apic();
        if (!dev) {
            return true;
        }
        s = APIC(dev);
        trace_apic_mem_writel(0x310, val);
        qatomic_set(&s->icr[1], val);
        return true;
    }
    if (offset == 0xb0) {
        dev = cpu_get_current_apic();
        if (!dev) {
            return true;
        }
        s = APIC(dev);
        trace_apic_mem_writel(0xb0, val);
        apic_eoi(s, 0xb0);
        return true;
    }
    return false;
}

void latx_apic_mem_write_locked(uint64_t addr, uint64_t val, unsigned size);
void latx_apic_mem_write_locked(uint64_t addr, uint64_t val, unsigned size)
{
    apic_mem_write(NULL, addr, val, size);
}

static void apic_pending_store(APICCommonState *s, unsigned index, uint64_t word)
{
    if (unlikely(s->pending_ext)) {
        apic_pending_change(s, index, UINT64_MAX, word, false);
    } else {
        qatomic_set(&s->pending[index], word);
    }
}

static void apic_reset_pending(APICCommonState *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(s->pending); i++) {
        apic_pending_store(s, i, UINT64_C(0xffffffff00000000));
    }
}

static void apic_read_pending(APICCommonState *s, uint32_t *irr, uint32_t *tmr)
{
    for (unsigned i = 0; i < ARRAY_SIZE(s->pending); i++) {
        uint64_t word = apic_pending_load(s, i);
        irr[i] = (uint32_t)word;
        tmr[i] = ~(uint32_t)(word >> 32);
    }
}

static void apic_pre_save(APICCommonState *s)
{
    apic_sync_vapic(s, SYNC_FROM_VAPIC);
    s->tpr = s->cpu->env.tpr;
    apic_read_pending(s, s->irr, s->tmr);
}

static void apic_post_load(APICCommonState *s)
{
    for (unsigned i = 0; i < ARRAY_SIZE(s->pending); i++) {
        uint64_t edge = (uint32_t)~s->tmr[i];
        apic_pending_store(s, i, (edge << 32) | s->irr[i]);
    }
    s->cpu->env.tpr = s->tpr;
    apic_update_isrv(s);
    s->cpu->env.irrv = (s->spurious_vec & APIC_SV_ENABLE)
                       ? apic_highest_pending(s) : -1;
    if (s->timer_expiry != -1) {
        timer_mod(s->timer, s->timer_expiry);
    } else {
        timer_del(s->timer);
    }
}

/* The local ISR, TMR, IRR and ICR words are read atomically. */
static bool apic_access_lockless(void *opaque, hwaddr addr, unsigned size,
                                 bool is_write, MemTxAttrs attrs)
{
    if (is_write) {
        if (size == 4 && !(addr & 3) && apic_is_msi_offset(addr)) {
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
            if (memory_region_device_lock_enabled()) {
                return true;
            }
#endif
            return apic_msi_fast_enabled();
        }
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
        if (size == 4 && !attrs.memory && memory_region_device_lock_enabled() &&
            !(addr & 0xf) &&
            (addr == 0x80 || addr == 0xd0 || addr == 0xe0 || addr == 0xf0 ||
             addr == 0x310 || (addr >= 0x320 && addr <= 0x380) ||
             addr == 0x3e0)) {
            return true;
        }
#endif
        return size == 4 && apic_ipi_enabled() &&
               (addr == 0x300 ||
                (addr == 0xb0 && option_int_opt != IRQ_CHECK_ASYNC_NOTIFY));
    }
    return size == 4 && !(addr & 0xf) &&
           ((addr >= 0x100 && addr <= 0x270) ||
            addr == 0x300 || addr == 0x310);
}

static bool apic_try_commit_msi(void *opaque, hwaddr addr, uint32_t data,
                                uint64_t *source, uint64_t before,
                                uint64_t after, bool scsi)
{
    MSIMessage msi = { .address = addr, .data = data };

    return !(addr & 3) && apic_is_msi_offset(addr) &&
           apic_try_send_msi(&msi, source, before, after, scsi);
}

#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
static bool trial_local_reads;

static bool apic_try_read_lockless(void *opaque, hwaddr addr, MemOp op,
                                   MemTxAttrs attrs, uint64_t *value)
{
    DeviceState *dev;
    APICCommonState *s;
    unsigned size = memop_size(op);
    uint32_t val;
    int index;

    if (!qatomic_read(&trial_local_reads) || attrs.memory ||
        trace_event_get_state_backends(TRACE_APIC_MEM_READL) ||
        trace_event_get_state_backends(TRACE_MEMORY_REGION_OPS_READ)) {
        return false;
    }

    if (size < 4) {
        *value = 0;
        return true;
    }

    dev = cpu_get_current_apic();
    if (!dev) {
        *value = 0;
        return true;
    }
    s = APIC(dev);

    index = (addr >> 4) & 0xff;
    switch (index) {
    case 0x02: /* id */
        val = (uint32_t)qatomic_read(&s->id) << 24;
        break;
    case 0x03: /* version */
        val = s->version | ((APIC_LVT_NB - 1) << 16);
        break;
    case 0x08: /* tpr */
        if (apic_report_tpr_access) {
            if (!lats_nobql_enabled()) {
                return false;
            }
            cpu_report_tpr_access(&s->cpu->env, TPR_ACCESS_READ);
        }
        if (qatomic_read(&s->vapic_paddr)) {
            if (!memory_region_device_lock_enabled()) {
                return false;
            }
            apic_sync_vapic(s, SYNC_FROM_VAPIC);
        }
        val = qatomic_read(&s->cpu->env.tpr);
        break;
    case 0x09: /* arb pri */
        val = apic_get_arb_pri(s);
        break;
    case 0x0a: /* ppr */
        val = apic_get_ppr(s);
        break;
    case 0x0b: /* eoi */
        val = 0;
        break;
    case 0x0d: /* ldr */
        val = s->log_dest << 24;
        break;
    case 0x0e: /* dfr */
        val = (s->dest_mode << 28) | 0xfffffff;
        break;
    case 0x0f: /* spurious */
        val = qatomic_read(&s->spurious_vec);
        break;
    case 0x10 ... 0x17: /* isr */
        val = qatomic_read(&s->isr[index & 7]);
        break;
    case 0x18 ... 0x1f: /* tmr */
        val = apic_tmr_word(s, index & 7);
        break;
    case 0x20 ... 0x27: /* irr */
        val = (uint32_t)apic_pending_load(s, index & 7);
        break;
    case 0x28: /* esr */
        val = s->esr;
        break;
    case 0x30:
    case 0x31: /* icr */
        val = qatomic_read(&s->icr[index & 1]);
        break;
    case 0x32 ... 0x37: /* lvt */
        val = s->lvt[index - 0x32];
        break;
    case 0x38: /* timer initial count */
        val = s->initial_count;
        break;
    case 0x39: /* timer current count */
        if (option_htimer) {
            val = get_current_count_from_timertick(s);
        } else {
            if (icount_enabled()) {
                return false;
            }
            val = apic_get_current_count(s);
        }
        break;
    case 0x3e: /* timer divide conf */
        val = s->divide_conf;
        break;
    default:
        /* Illegal address mutates s->esr; fall back to BQL. */
        return false;
    }

    *value = val;
    return true;
}
#endif

static const MemoryRegionOps apic_io_ops = {
    .try_commit_msi = apic_try_commit_msi,
    .read = apic_mem_read,
    .write = apic_mem_write,
    .access_lockless = apic_access_lockless,
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    .try_read_lockless = apic_try_read_lockless,
#endif
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void apic_realize(DeviceState *dev, Error **errp)
{
    APICCommonState *s = APIC(dev);

    if (s->id >= MAX_APICS) {
        error_setg(errp, "%s initialization failed. APIC ID %d is invalid",
                   object_get_typename(OBJECT(dev)), s->id);
        return;
    }

    if (kvm_enabled()) {
        warn_report("Userspace local APIC is deprecated for KVM.");
        warn_report("Do not use kernel-irqchip except for the -M isapc machine type.");
    }

    memory_region_init_io(&s->io_memory, OBJECT(s), &apic_io_ops, s, "apic-msi",
                          APIC_SPACE_SIZE);

    /*
     * apic-msi's apic_mem_write can call into ioapic_eoi_broadcast, which can
     * write back to apic-msi. As such mark the apic-msi region re-entrancy
     * safe.
     */
    s->io_memory.disable_reentrancy_guard = true;

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, apic_timer, s);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    qemu_rec_mutex_init(&s->timer_lock);
    qatomic_set(&trial_local_reads, tcg_enabled() &&
        lats_nobql_enabled());
#endif
    if (option_htimer) {
        s->la_ecfg = INIT_VALUE_ECFG;
    }
    /*
     * An older AOT translation can read the raw EOI word. Keep the test
     * mode off whenever such a native translation could be in use.
     */
    trial_combined_poll =
        lats_nobql_enabled();
    qatomic_set(&trial_local_spurious,
        lats_nobql_enabled());
    if (apic_expiry_requested()) {
        unsigned actors = MACHINE(qdev_get_machine())->smp.max_cpus * 2 + 3;

        g_assert(actors <= PENDING_SLOT_MASK + 1);
        s->pending_ext = g_malloc0(sizeof(*s->pending_ext) +
            actors * ARRAY_SIZE(s->pending) * sizeof(APICPendingWriter));
        s->pending_ext->actors = actors;
        qemu_spin_init(&s->pending_ext->host_msi_lock);
    }
    apic_reset_pending(s);
    apic_ipi_quiesce();
    local_apics[s->id] = s;
    apic_set_bit(local_apic_slots, s->id);
    apic_ipi_resume();

    msi_nonbroken = true;
}

static void apic_unrealize(DeviceState *dev)
{
    APICCommonState *s = APIC(dev);

    /* The guest can rewrite its xAPIC ID, but the array slot stays put. */
    unsigned slot = (uint8_t)s->initial_apic_id;

    apic_ipi_quiesce();
    if (local_apics[slot] == s) {
        local_apics[slot] = NULL;
        apic_reset_bit(local_apic_slots, slot);
    }
    apic_ipi_resume();
    timer_free(s->timer);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    qemu_rec_mutex_destroy(&s->timer_lock);
#endif
    g_clear_pointer(&s->pending_ext, g_free);
}

/* Diagnostic source used only in the qtest build. */
static struct {
    QemuThread thread;
    QemuSemaphore entered, resumed;
    unsigned stage;
    DeviceState *apic;
    uint64_t lease;
    uint64_t expected;
    bool committed;
    bool live;
} pending_trial;

static bool apic_expiry_trial_active(void)
{
    return pending_trial.stage != 0;
}

static void apic_expiry_trial(unsigned stage)
{
    if (stage == pending_trial.stage) {
        qemu_sem_post(&pending_trial.entered);
        qemu_sem_wait(&pending_trial.resumed);
    }
}

static void *pending_trial_worker(void *opaque)
{
    g_assert(!qemu_mutex_iothread_locked());
    pending_trial.committed = apic_expiry_commit(
        pending_trial.apic, 0xd1, &pending_trial.lease, pending_trial.expected,
        pending_trial.expected & ~UINT64_C(1));
    return NULL;
}

uint64_t qtest_apic_expiry(unsigned op, uint64_t val);
uint64_t qtest_apic_expiry(unsigned op, uint64_t val)
{
    APICCommonState *s = local_apics[0];

    g_assert(s && s->pending_ext && qemu_mutex_iothread_locked());
    switch (op) {
    case 0:
        return apic_pending_load(s, 0xd1 >> 5);
    case 1:
        apic_pending_change(s, 0xd1 >> 5, UINT64_MAX, val, false);
        return 0;
    case 2:
        apic_expiry_change_lease(&pending_trial.lease, val);
        return 0;
    case 3:
        g_assert(!pending_trial.live && val >= 1 && val <= 3);
        pending_trial.apic = DEVICE(s);
        pending_trial.expected = qatomic_read(&pending_trial.lease);
        pending_trial.stage = val;
        pending_trial.live = true;
        qemu_sem_init(&pending_trial.entered, 0);
        qemu_sem_init(&pending_trial.resumed, 0);
        qemu_thread_create(&pending_trial.thread, "apic-expiry",
                           pending_trial_worker, NULL, QEMU_THREAD_JOINABLE);
        qemu_sem_wait(&pending_trial.entered);
        return 0;
    case 4:
        g_assert(pending_trial.live);
        qemu_sem_post(&pending_trial.resumed);
        qemu_thread_join(&pending_trial.thread);
        qemu_sem_destroy(&pending_trial.entered);
        qemu_sem_destroy(&pending_trial.resumed);
        pending_trial.live = false;
        pending_trial.stage = 0;
        return pending_trial.committed;
    case 5:
        return apic_clear_pending(s, 0xd1);
    case 6:
        return apic_post_vector(s, 0xd1, true);
    case 7:
        return s->pending[0xd1 >> 5];
    case 8:
        return apic_expiry_lease_value(&pending_trial.lease);
    default:
        g_assert_not_reached();
    }
}

static void apic_class_init(ObjectClass *klass, void *data)
{
    APICCommonClass *k = APIC_COMMON_CLASS(klass);

    k->realize = apic_realize;
    k->unrealize = apic_unrealize;
    k->set_base = apic_set_base;
    k->set_tpr = apic_set_tpr;
    k->get_tpr = apic_get_tpr;
    k->vapic_base_update = apic_vapic_base_update;
    k->external_nmi = apic_external_nmi;
    k->read_pending = apic_read_pending;
    k->reset = apic_reset_pending;
    k->pre_save = apic_pre_save;
    k->post_load = apic_post_load;
    k->send_msi = apic_send_msi;
}

static const TypeInfo apic_info = {
    .name          = TYPE_APIC,
    .instance_size = sizeof(APICCommonState),
    .parent        = TYPE_APIC_COMMON,
    .class_init    = apic_class_init,
};

static void apic_register_types(void)
{
    type_register_static(&apic_info);
}

type_init(apic_register_types)
