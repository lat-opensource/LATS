/*
 *  High Precision Event Timer emulation
 *
 *  Copyright (c) 2007 Alexander Graf
 *  Copyright (c) 2008 IBM Corporation
 *
 *  Authors: Beth Kon <bkon@us.ibm.com>
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
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * *****************************************************************
 *
 * This driver attempts to emulate an HPET device in software.
 */

#include "qemu/osdep.h"
#include "hw/i386/pc.h"
#include "hw/i386/hpet-irq0.h"
#include "hw/irq.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "sysemu/cpu-timers.h"
#include "sysemu/replay.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"
#include "sysemu/tcg.h"
#include "hw/qdev-properties.h"
#include "hw/timer/hpet.h"
#include "hw/sysbus.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/rtc/mc146818rtc_regs.h"
#include "migration/vmstate.h"
#include "hw/timer/i8254.h"
#include "exec/address-spaces.h"
#include "qom/object.h"

//#define HPET_DEBUG
#ifdef HPET_DEBUG
#define DPRINTF printf
#else
#define DPRINTF(...)
#endif

#define HPET_MSI_SUPPORT        0

/* Internal flag in a reserved bit of the published low configuration. */
#define HPET_SNAPSHOT_HAS_HIGH_WORDS (1U << 31)

OBJECT_DECLARE_SIMPLE_TYPE(HPETState, HPET)

struct HPETState;
typedef struct HPETTimer {  /* timers */
    uint8_t tn;             /*timer number*/
    QEMUTimer *qemu_timer;
    struct HPETState *state;
    /* Memory-mapped, software visible timer registers */
    uint64_t config;        /* configuration/cap */
    uint64_t cmp;           /* comparator */
    uint64_t fsb;           /* FSB route */
    /* Hidden register state */
    /*
     * Committed low config/comparator words for concurrent guest access.
     * The low config word also tells whether a 32-bit write truncates state.
     * Recreated from the migrated registers after loading.
     */
    uint64_t readable_low;
    uint64_t period;        /* Last value written to comparator */
    uint8_t wrap_flag;      /* timer pop will indicate wrap for one-shot 32-bit
                             * mode. Next pop will be actual timer expiration.
                             */
} HPETTimer;

struct HPETState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    uint64_t hpet_offset;
    uint64_t hpet_offset_ticks;
    uint64_t config;            /* configuration */
    uint64_t hpet_counter;      /* main counter */
    uint64_t hpet_counter_last; /* greatest tick returned by lockless reads */
    bool hpet_offset_saved;
    qemu_irq irqs[HPET_NUM_IRQ_ROUTES];
    uint32_t flags;
    uint8_t rtc_irq_level;
    qemu_irq pit_enabled;
    uint8_t num_timers;
    uint32_t intcap;
    HPETTimer timer[HPET_MAX_TIMERS];

    /* Memory-mapped, software visible registers */
    uint64_t capability;        /* capabilities */
//    uint64_t config;            /* configuration */
    uint64_t isr;               /* interrupt status reg */
//    uint64_t hpet_counter;      /* main counter */
    uint8_t  hpet_id;           /* instance id */
    /* Preserve the offsets used by compiled HPET reads. */
    QemuRecMutex mmio_lock;
    unsigned fsb_in_flight;
    bool local_mmio;
    bool late_rearm;
};

typedef HPETState HPETMMIOGuard;

static void hpet_unlock_mmio(HPETMMIOGuard *guard)
{
    qemu_rec_mutex_unlock(&guard->mmio_lock);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(HPETMMIOGuard, hpet_unlock_mmio)

static HPETMMIOGuard *hpet_lock_mmio(HPETState *s)
{
    if (!s->local_mmio) {
        return NULL;
    }
    qemu_rec_mutex_lock(&s->mmio_lock);
    return s;
}

void *hpet_global_ptr;
int hpet_offset_hpet_offset_ticks;
int hpet_offset_hpet_counter;
int hpet_offset_hpet_counter_last;
int hpet_offset_config;

static uint32_t hpet_in_legacy_mode(HPETState *s)
{
    return qatomic_load_acquire(&s->config) & HPET_CFG_LEGACY;
}

static uint32_t timer_int_route(struct HPETTimer *timer)
{
    return (timer->config & HPET_TN_INT_ROUTE_MASK) >> HPET_TN_INT_ROUTE_SHIFT;
}

static uint32_t timer_fsb_route(HPETTimer *t)
{
    return t->config & HPET_TN_FSB_ENABLE;
}

static uint32_t hpet_enabled(HPETState *s)
{
    return qatomic_load_acquire(&s->config) & HPET_CFG_ENABLE;
}

static uint32_t timer_is_periodic(HPETTimer *t)
{
    return t->config & HPET_TN_PERIODIC;
}

static uint32_t timer_enabled(HPETTimer *t)
{
    return t->config & HPET_TN_ENABLE;
}

/*
 * Move an expired periodic comparator to its first non-expired value.
 * The comparator is in the past when its modular distance from the
 * counter is in the upper half of the timer's range.
 */
static uint64_t hpet_advance_periodic(uint64_t cmp, uint64_t current,
                                      uint64_t period, uint64_t mask)
{
    uint64_t half = (mask >> 1) + 1;
    uint64_t distance = (cmp - current) & mask;

    if (distance < half) {
        return cmp;
    }

    /* The next period normally suffices. */
    cmp = (cmp + period) & mask;
    distance = (cmp - current) & mask;
    if (distance < half) {
        return cmp;
    }

    if (period <= half) {
        /* Round the lag up to a multiple of the period. */
        uint64_t remainder = ((current - cmp) & mask) % period;

        distance = remainder ? period - remainder : 0;
    } else {
        /* A large step moves backward within the expired half. */
        uint64_t step = (-period) & mask;

        distance = half - step + (distance - half) % step;
    }
    return (current + distance) & mask;
}

static uint64_t ticks_to_ns(uint64_t value)
{
    return value * HPET_CLK_PERIOD;
}

static uint64_t ns_to_ticks(uint64_t value)
{
    return value / HPET_CLK_PERIOD;
}

static uint64_t hpet_fixup_reg(uint64_t new, uint64_t old, uint64_t mask)
{
    new &= mask;
    new |= old & ~mask;
    return new;
}

static int activating_bit(uint64_t old, uint64_t new, uint64_t mask)
{
    return (!(old & mask) && (new & mask));
}

static int deactivating_bit(uint64_t old, uint64_t new, uint64_t mask)
{
    return ((old & mask) && !(new & mask));
}

static uint64_t hpet_get_ticks(HPETState *s)
{
    return ns_to_ticks(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                       qatomic_read_u64(&s->hpet_offset));
}

static uint64_t hpet_get_ticks_monotonic(HPETState *s)
{
    uint64_t ticks = hpet_get_ticks(s);
    uint64_t last = qatomic_read_u64(&s->hpet_counter_last);

    while (ticks > last) {
        uint64_t old = qatomic_cmpxchg(&s->hpet_counter_last, last, ticks);

        if (old == last) {
            return ticks;
        }
        last = old;
    }
    return last;
}

/*
 * calculate diff between comparator value and current ticks
 */
static inline uint64_t hpet_calculate_diff(HPETTimer *t, uint64_t current)
{

    if (t->config & HPET_TN_32BIT) {
        uint32_t diff, cmp;

        cmp = (uint32_t)t->cmp;
        diff = cmp - (uint32_t)current;
        diff = (int32_t)diff > 0 ? diff : (uint32_t)1;
        return (uint64_t)diff;
    } else {
        uint64_t diff, cmp;

        cmp = t->cmp;
        diff = cmp - current;
        diff = (int64_t)diff > 0 ? diff : (uint64_t)1;
        return diff;
    }
}

typedef enum HPETIRQKind {
    HPET_IRQ_NONE,
    HPET_IRQ_LOWER,
    HPET_IRQ_RAISE,
    HPET_IRQ_PULSE,
    HPET_IRQ_FSB,
} HPETIRQKind;

typedef struct HPETIRQAction {
    HPETIRQKind kind;
    qemu_irq output;
    uint64_t fsb;
} HPETIRQAction;

static HPETIRQAction hpet_update_irq_state(struct HPETTimer *timer, int set)
{
    uint64_t mask;
    HPETState *s;
    int route;

    if (timer->tn <= 1 && hpet_in_legacy_mode(timer->state)) {
        /* if LegacyReplacementRoute bit is set, HPET specification requires
         * timer0 be routed to IRQ0 in NON-APIC or IRQ2 in the I/O APIC,
         * timer1 be routed to IRQ8 in NON-APIC or IRQ8 in the I/O APIC.
         */
        route = (timer->tn == 0) ? 0 : RTC_ISA_IRQ;
    } else {
        route = timer_int_route(timer);
    }
    s = timer->state;
    mask = 1 << timer->tn;
    if (!set || !timer_enabled(timer) || !hpet_enabled(timer->state)) {
        s->isr &= ~mask;
        if (!timer_fsb_route(timer)) {
            return (HPETIRQAction){ .kind = HPET_IRQ_LOWER,
                                    .output = s->irqs[route] };
        }
        return (HPETIRQAction){ .kind = HPET_IRQ_NONE };
    }
    if (timer_fsb_route(timer)) {
        return (HPETIRQAction){ .kind = HPET_IRQ_FSB, .fsb = timer->fsb };
    }
    if (timer->config & HPET_TN_TYPE_LEVEL) {
        s->isr |= mask;
        return (HPETIRQAction){ .kind = HPET_IRQ_RAISE,
                                .output = s->irqs[route] };
    }
    s->isr &= ~mask;
    return (HPETIRQAction){ .kind = HPET_IRQ_PULSE, .output = s->irqs[route] };
}

static void hpet_send_irq(HPETIRQAction action)
{
    switch (action.kind) {
    case HPET_IRQ_NONE:
        break;
    case HPET_IRQ_LOWER:
        qemu_irq_lower(action.output);
        break;
    case HPET_IRQ_RAISE:
        qemu_irq_raise(action.output);
        break;
    case HPET_IRQ_PULSE:
        qemu_irq_pulse(action.output);
        break;
    case HPET_IRQ_FSB:
        address_space_stl_le(&address_space_memory, action.fsb >> 32,
                             action.fsb & UINT32_MAX, MEMTXATTRS_UNSPECIFIED,
                             NULL);
        break;
    }
}

static void update_irq(struct HPETTimer *timer, int set)
{
    hpet_send_irq(hpet_update_irq_state(timer, set));
}

static int hpet_pre_save(void *opaque)
{
    HPETState *s = opaque;

    /* save current counter value */
    if (hpet_enabled(s)) {
        qatomic_set_u64(&s->hpet_counter, hpet_get_ticks_monotonic(s));
    }

    return 0;
}

static int hpet_pre_load(void *opaque)
{
    HPETState *s = opaque;

    /* version 1 only supports 3, later versions will load the actual value */
    s->num_timers = HPET_MIN_TIMERS;
    return 0;
}

static bool hpet_validate_num_timers(void *opaque, int version_id)
{
    HPETState *s = opaque;

    if (s->num_timers < HPET_MIN_TIMERS) {
        return false;
    } else if (s->num_timers > HPET_MAX_TIMERS) {
        return false;
    }
    return true;
}

static void hpet_publish_timer(HPETTimer *timer)
{
    uint32_t config = timer->config;

    if ((timer->cmp | timer->period) >> 32) {
        config |= HPET_SNAPSHOT_HAS_HIGH_WORDS;
    }
    qatomic_set_u64(&timer->readable_low,
                    config | ((uint64_t)(uint32_t)timer->cmp << 32));
}

static int hpet_post_load(void *opaque, int version_id)
{
    HPETState *s = opaque;

    /* Recalculate the offset between the main counter and guest time */
    if (!s->hpet_offset_saved) {
        uint64_t offset =
            ticks_to_ns(qatomic_read_u64(&s->hpet_counter)) -
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        qatomic_set_u64(&s->hpet_offset, offset);
        s->hpet_offset_ticks = (int64_t)offset / 10;
    }
    qatomic_set_u64(&s->hpet_counter_last,
                    qatomic_read_u64(&s->hpet_counter));

    /* Push number of timers into capability returned via HPET_ID */
    s->capability &= ~HPET_ID_NUM_TIM_MASK;
    s->capability |= (s->num_timers - 1) << HPET_ID_NUM_TIM_SHIFT;
    hpet_cfg.hpet[s->hpet_id].event_timer_block_id = (uint32_t)s->capability;

    for (unsigned i = 0; i < s->num_timers; i++) {
        hpet_publish_timer(&s->timer[i]);
    }
    /* Derive HPET_MSI_SUPPORT from the capability of the first timer. */
    s->flags &= ~(1 << HPET_MSI_SUPPORT);
    if (s->timer[0].config & HPET_TN_FSB_CAP) {
        s->flags |= 1 << HPET_MSI_SUPPORT;
    }
    return 0;
}

static bool hpet_offset_needed(void *opaque)
{
    HPETState *s = opaque;

    return hpet_enabled(s) && s->hpet_offset_saved;
}

static bool hpet_rtc_irq_level_needed(void *opaque)
{
    HPETState *s = opaque;

    return s->rtc_irq_level != 0;
}

static const VMStateDescription vmstate_hpet_rtc_irq_level = {
    .name = "hpet/rtc_irq_level",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = hpet_rtc_irq_level_needed,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(rtc_irq_level, HPETState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_hpet_offset = {
    .name = "hpet/offset",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = hpet_offset_needed,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(hpet_offset, HPETState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_hpet_timer = {
    .name = "hpet_timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(tn, HPETTimer),
        VMSTATE_UINT64(config, HPETTimer),
        VMSTATE_UINT64(cmp, HPETTimer),
        VMSTATE_UINT64(fsb, HPETTimer),
        VMSTATE_UINT64(period, HPETTimer),
        VMSTATE_UINT8(wrap_flag, HPETTimer),
        VMSTATE_TIMER_PTR(qemu_timer, HPETTimer),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_hpet = {
    .name = "hpet",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = hpet_pre_save,
    .pre_load = hpet_pre_load,
    .post_load = hpet_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(config, HPETState),
        VMSTATE_UINT64(isr, HPETState),
        VMSTATE_UINT64(hpet_counter, HPETState),
        VMSTATE_UINT8_V(num_timers, HPETState, 2),
        VMSTATE_VALIDATE("num_timers in range", hpet_validate_num_timers),
        VMSTATE_STRUCT_VARRAY_UINT8(timer, HPETState, num_timers, 0,
                                    vmstate_hpet_timer, HPETTimer),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription*[]) {
        &vmstate_hpet_rtc_irq_level,
        &vmstate_hpet_offset,
        NULL
    }
};

static void hpet_arm(HPETTimer *t, uint64_t ticks)
{
    if (ticks < ns_to_ticks(INT64_MAX / 2)) {
        int64_t expire_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              ticks_to_ns(ticks);

        if (t->state->late_rearm) {
            main_loop_mod_virtual_timer_ns(t->qemu_timer, expire_time);
        } else {
            timer_mod(t->qemu_timer, expire_time);
        }
    } else {
        timer_del(t->qemu_timer);
    }
}

/*
 * timer expiration callback
 */
static const char *const hpet_delivery_names[] = {
    "lower", "msi", "level", "pulse",
};
static uint64_t hpet_expirations[HPET_MAX_TIMERS][2][2]
                                [ARRAY_SIZE(hpet_delivery_names)][HPET_NUM_IRQ_ROUTES];
/* The restricted unlocked callback is always a timer-0 legacy one-shot
 * pulse on IRQ0. Keep the BQL-owned census separate from this writer. */
static uint64_t hpet_unlocked_expirations;

static void count_hpet_expiration(HPETTimer *timer)
{
    bool legacy = timer->tn <= 1 && hpet_in_legacy_mode(timer->state);
    unsigned irq = legacy ? (timer->tn == 0 ? 0 : RTC_ISA_IRQ)
                          : timer_int_route(timer);
    unsigned delivery = 0;

    if (timer_enabled(timer) && hpet_enabled(timer->state)) {
        if (timer_fsb_route(timer)) {
            delivery = 1;
        } else {
            delivery = (timer->config & HPET_TN_TYPE_LEVEL) ? 2 : 3;
        }
    }
    qatomic_inc(&hpet_expirations[timer->tn][legacy][!!timer_is_periodic(timer)][delivery][irq]);
}

void trial_report_polls(const char *phase);

static void report_hpet_expirations(const char *phase)
{
    for (unsigned tn = 0; tn < HPET_MAX_TIMERS; tn++) {
        for (unsigned legacy = 0; legacy < 2; legacy++) {
            for (unsigned periodic = 0; periodic < 2; periodic++) {
                for (unsigned mode = 0; mode < ARRAY_SIZE(hpet_delivery_names); mode++) {
                    for (unsigned irq = 0; irq < HPET_NUM_IRQ_ROUTES; irq++) {
                        uint64_t count = qatomic_read(&hpet_expirations[tn][legacy][periodic][mode][irq]);

                        if (tn == 0 && legacy && !periodic && mode == 3 && irq == 0) {
                            count += qatomic_read(&hpet_unlocked_expirations);
                        }
                        if (count) {
                            fprintf(stderr, "HPET_COUNT %s timer=%u legacy=%u "
                                    "periodic=%u delivery=%s irq=%u count=%" PRIu64 "\n",
                                    phase, tn, legacy, periodic, hpet_delivery_names[mode],
                                    irq, count);
                        }
                    }
                }
            }
        }
    }
}

static unsigned hpet_local_writes;
static bool hpet_local_enabled;

static void report_hpet_local_mmio(const char *phase)
{
    if (hpet_local_enabled) {
        fprintf(stderr, "HPET_LOCAL_MMIO %s writes=%u\n",
                phase, qatomic_read(&hpet_local_writes));
    }
}

static void report_hpet_exit(void)
{
    report_hpet_local_mmio("exit");
    report_hpet_expirations("exit");
    if (x86_hpet_irq0_report) {
        x86_hpet_irq0_report("exit");
    }
    trial_report_polls("exit");
}

static void report_hpet_state(void *opaque, bool running, RunState state)
{
    if (!running && state == RUN_STATE_PAUSED) {
        report_hpet_local_mmio("menu");
        report_hpet_expirations("menu");
        if (x86_hpet_irq0_report) {
            x86_hpet_irq0_report("menu");
        }
        trial_report_polls("menu");
    }
}

static void hpet_timer(void *opaque)
{
    HPETTimer *t = opaque;
    HPETState *s = t->state;
    /* The timer queue acquires the device lock before selecting this timer. */
    g_autoptr(HPETMMIOGuard) guard = s->local_mmio ? s : NULL;
    uint64_t diff;
    bool normal_irq0 = guard && t->tn == 0 && hpet_in_legacy_mode(s) &&
        (t->config & HPET_TN_32BIT) && !t->wrap_flag && !timer_is_periodic(t);


    uint64_t mask = t->config & HPET_TN_32BIT ? UINT32_MAX : UINT64_MAX;
    uint64_t period = t->period & mask;
    uint64_t cur_tick = hpet_get_ticks_monotonic(t->state);

    if (timer_is_periodic(t) && period != 0) {
        t->cmp = hpet_advance_periodic(t->cmp, cur_tick, period, mask);
        diff = hpet_calculate_diff(t, cur_tick);
        hpet_arm(t, diff);
        hpet_publish_timer(t);
    } else if (t->config & HPET_TN_32BIT && !timer_is_periodic(t)) {
        if (t->wrap_flag) {
            diff = hpet_calculate_diff(t, cur_tick);
            hpet_arm(t, diff);
            t->wrap_flag = 0;
        }
    }
    count_hpet_expiration(t);
    HPETIRQAction action = hpet_update_irq_state(t, 1);
    if (qemu_mutex_iothread_locked() && normal_irq0 &&
        action.kind == HPET_IRQ_PULSE && x86_hpet_irq0_try_pulse &&
        x86_hpet_irq0_try_pulse()) {
        return;
    }

    /* FSB is a memory write and can reenter this device. */
    bool guard_fsb = guard && action.kind == HPET_IRQ_FSB;
    if (guard_fsb) {
        ++s->fsb_in_flight;
    }
    g_clear_pointer(&guard, hpet_unlock_mmio);
    hpet_send_irq(action);
    if (guard_fsb) {
        guard = hpet_lock_mmio(s);
        --s->fsb_in_flight;
    }
}

static void hpet_set_timer(HPETTimer *t)
{
    uint64_t diff;
    uint32_t wrap_diff;  /* how many ticks until we wrap? */
    uint64_t cur_tick = hpet_get_ticks_monotonic(t->state);

    if (t->tn == 0) {
        hpet_irq0_revoke_timer();
    }

    /* whenever new timer is being set up, make sure wrap_flag is 0 */
    t->wrap_flag = 0;
    diff = hpet_calculate_diff(t, cur_tick);

    /* hpet spec says in one-shot 32-bit mode, generate an interrupt when
     * counter wraps in addition to an interrupt with comparator match.
     */
    if (t->config & HPET_TN_32BIT && !timer_is_periodic(t)) {
        wrap_diff = 0xffffffff - (uint32_t)cur_tick;
        if (wrap_diff < (uint32_t)diff) {
            diff = wrap_diff;
            t->wrap_flag = 1;
        }
    }
    hpet_arm(t, diff);
}

static void hpet_del_timer(HPETTimer *t)
{
    if (t->tn == 0) {
        hpet_irq0_revoke_timer();
    }
    timer_del(t->qemu_timer);
    update_irq(t, 0);
}

static uint64_t hpet_read_register(void *opaque, hwaddr addr, bool locked)
{
    HPETState *s = opaque;
    uint64_t cur_tick, index;

    DPRINTF("qemu: Enter hpet_ram_readl at %" PRIx64 "\n", addr);
    index = addr;
    /*address range of all TN regs*/
    if (index >= 0x100 && index <= 0x3ff) {
        uint8_t timer_id = (addr - 0x100) / 0x20;
        HPETTimer *timer = &s->timer[timer_id];

        if (timer_id > s->num_timers) {
            DPRINTF("qemu: timer id out of range\n");
            return 0;
        }

        switch ((addr - 0x100) % 0x20) {
        case HPET_TN_CFG:
            if (locked) {
                return timer->config;
            }
            return (uint32_t)qatomic_read_u64(&timer->readable_low) &
                   ~HPET_SNAPSHOT_HAS_HIGH_WORDS;
        case HPET_TN_CFG + 4: // Interrupt capabilities
            return timer->config >> 32;
        case HPET_TN_CMP: // comparator register
            return locked ? timer->cmp :
                   qatomic_read_u64(&timer->readable_low) >> 32;
        case HPET_TN_CMP + 4:
            return timer->cmp >> 32;
        case HPET_TN_ROUTE:
            return timer->fsb;
        case HPET_TN_ROUTE + 4:
            return timer->fsb >> 32;
        default:
            DPRINTF("qemu: invalid hpet_ram_readl\n");
            break;
        }
    } else {
        switch (index) {
        case HPET_ID:
            return s->capability;
        case HPET_PERIOD:
            return s->capability >> 32;
        case HPET_CFG:
            return qatomic_load_acquire(&s->config);
        case HPET_CFG + 4:
            DPRINTF("qemu: invalid HPET_CFG + 4 hpet_ram_readl\n");
            return 0;
        case HPET_COUNTER:
            if (hpet_enabled(s)) {
                cur_tick = hpet_get_ticks_monotonic(s);
            } else {
                cur_tick = qatomic_read_u64(&s->hpet_counter);
            }
            DPRINTF("qemu: reading counter  = %" PRIx64 "\n", cur_tick);
            return cur_tick;
        case HPET_COUNTER + 4:
            if (hpet_enabled(s)) {
                cur_tick = hpet_get_ticks_monotonic(s);
            } else {
                cur_tick = qatomic_read_u64(&s->hpet_counter);
            }
            DPRINTF("qemu: reading counter + 4  = %" PRIx64 "\n", cur_tick);
            return cur_tick >> 32;
        case HPET_STATUS:
            return s->isr;
        default:
            DPRINTF("qemu: invalid hpet_ram_readl\n");
            break;
        }
    }
    return 0;
}

static uint64_t hpet_ram_read(void *opaque, hwaddr addr, unsigned size)
{
    HPETState *s = opaque;
    g_autoptr(HPETMMIOGuard) guard = NULL;
    bool locked = qemu_mutex_iothread_locked();
    unsigned reg = (addr - 0x100) % 0x20;

    if (size == 8 && addr == HPET_COUNTER) {
        return hpet_read_register(s, addr, false);
    }
    if (size == 8) {
        /* An aligned wide access must see one device register state. */
        guard = hpet_lock_mmio(s);
        uint32_t low = hpet_read_register(s, addr, true);
        uint32_t high = hpet_read_register(s, addr + 4, true);

        return low | ((uint64_t)high << 32);
    }
    if (s->local_mmio &&
        addr >= 0x100 && (reg == HPET_TN_CFG || reg == HPET_TN_CMP)) {
        /* These low words already have a concurrently published snapshot. */
        return hpet_read_register(s, addr, false);
    }
    if (addr != HPET_COUNTER && addr != HPET_COUNTER + 4) {
        guard = hpet_lock_mmio(s);
    }
    return hpet_read_register(s, addr, guard || locked);
}

static bool hpet_independent_register_write(HPETState *s, hwaddr addr,
                                            unsigned size)
{
    if (size != 4 && (size != 8 || (addr & 7) || addr > HPET_LEN - 8)) {
        return false;
    }
    if (addr == HPET_COUNTER || addr == HPET_COUNTER + 4) {
        return true;
    }
    if (addr < 0x100 || addr >= 0x100 + 0x20 * s->num_timers) {
        return false;
    }
    switch ((addr - 0x100) % 0x20) {
    case HPET_TN_CMP:
    case HPET_TN_CMP + 4:
    case HPET_TN_ROUTE:
    case HPET_TN_ROUTE + 4:
        return true;
    default:
        return false;
    }
}

static bool hpet_config_write_is_noop(HPETState *s, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    hwaddr reg = addr - 0x100;
    hwaddr timer = reg / 0x20;
    uint32_t published;

    if (size != 4 || timer >= s->num_timers || reg % 0x20 != HPET_TN_CFG) {
        return false;
    }

    /*
     * The configuration and truncation flag must come from the same
     * committed value. A write with no effect can complete at this read.
     */
    published = qatomic_read_u64(&s->timer[timer].readable_low);
    return !((published ^ value) & HPET_TN_CFG_WRITE_MASK) &&
           (!(value & HPET_TN_32BIT) ||
            !(published & HPET_SNAPSHOT_HAS_HIGH_WORDS));
}

static void hpet_write_register(HPETState *s, hwaddr addr, uint64_t value,
                                bool wide);

static void hpet_ram_write(void *opaque, hwaddr addr,
                           uint64_t value, unsigned size)
{
    HPETState *s = opaque;
    g_autoptr(IOThreadLockAuto) lock = NULL;
    g_autoptr(HPETMMIOGuard) guard = NULL;
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    bool mr_dev_locked = qatomic_read(&s->iomem.device_lock) != NULL;
#else
    bool mr_dev_locked = false;
#endif
    bool independent = hpet_independent_register_write(s, addr, size);
    bool local = s->local_mmio && (independent || mr_dev_locked);

    if (hpet_config_write_is_noop(s, addr, value, size)) {
        return;
    }

    if (!mr_dev_locked) {
        if (local) {
            guard = hpet_lock_mmio(s);
        }
        /* Join the BQL order for writes that can overlap a reentrant FSB write. */
        if (!qemu_mutex_iothread_locked() && (!local || s->fsb_in_flight)) {
            g_clear_pointer(&guard, hpet_unlock_mmio);
            qemu_mutex_lock_iothread_mmio_profile_impl(&s->iomem,
                                                      memory_region_name(&s->iomem),
                                                      addr, true, __FILE__, __LINE__);
            lock = (IOThreadLockAuto *)(uintptr_t)1;
        }
        if (!guard) {
            guard = hpet_lock_mmio(s);
        }
    }
    hpet_irq0_revoke_timer();
    if (local && !qemu_mutex_iothread_locked()) {
        qatomic_inc(&hpet_local_writes);
    }

    if (size == 8 && addr == HPET_COUNTER) {
        hpet_write_register(s, addr, value, true);
    } else {
        /*
         * Keep the legacy register write order under the same mutex.
         * For a periodic comparator the low word consumes SETVAL, so
         * the following high word still updates the period.
         */
        hpet_write_register(s, addr, (uint32_t)value, false);
        if (size == 8) {
            hpet_write_register(s, addr + 4, value >> 32, false);
        }
    }
}

static void hpet_write_register(HPETState *s, hwaddr addr, uint64_t value,
                                bool wide)
{
    int i;
    uint64_t old_val, new_val, val, index;

    DPRINTF("qemu: Enter hpet_ram_writel at %" PRIx64 " = 0x%" PRIx64 "\n",
            addr, value);
    index = addr;
    old_val = hpet_read_register(s, addr, true);
    new_val = value;

    /*address range of all TN regs*/
    if (index >= 0x100 && index <= 0x3ff) {
        uint8_t timer_id = (addr - 0x100) / 0x20;
        HPETTimer *timer = &s->timer[timer_id];

        DPRINTF("qemu: hpet_ram_writel timer_id = 0x%x\n", timer_id);
        if (timer_id > s->num_timers) {
            DPRINTF("qemu: timer id out of range\n");
            return;
        }
        switch ((addr - 0x100) % 0x20) {
        case HPET_TN_CFG:
            DPRINTF("qemu: hpet_ram_writel HPET_TN_CFG\n");
            if (activating_bit(old_val, new_val, HPET_TN_FSB_ENABLE)) {
                update_irq(timer, 0);
            }
            val = hpet_fixup_reg(new_val, old_val, HPET_TN_CFG_WRITE_MASK);
            timer->config = (timer->config & 0xffffffff00000000ULL) | val;
            if (new_val & HPET_TN_32BIT) {
                timer->cmp = (uint32_t)timer->cmp;
                timer->period = (uint32_t)timer->period;
            }
            if (activating_bit(old_val, new_val, HPET_TN_ENABLE) &&
                hpet_enabled(s)) {
                hpet_set_timer(timer);
            } else if (deactivating_bit(old_val, new_val, HPET_TN_ENABLE)) {
                hpet_del_timer(timer);
            }
            break;
        case HPET_TN_CFG + 4: // Interrupt capabilities
            DPRINTF("qemu: invalid HPET_TN_CFG+4 write\n");
            break;
        case HPET_TN_CMP: // comparator register
            DPRINTF("qemu: hpet_ram_writel HPET_TN_CMP\n");
            if (timer->config & HPET_TN_32BIT) {
                new_val = (uint32_t)new_val;
            }
            if (!timer_is_periodic(timer)
                || (timer->config & HPET_TN_SETVAL)) {
                timer->cmp = (timer->cmp & 0xffffffff00000000ULL) | new_val;
            }
            if (timer_is_periodic(timer)) {
                /*
                 * FIXME: Clamp period to reasonable min value?
                 * Clamp period to reasonable max value
                 */
                new_val &= (timer->config & HPET_TN_32BIT ? ~0u : ~0ull) >> 1;
                timer->period =
                    (timer->period & 0xffffffff00000000ULL) | new_val;
            }
            timer->config &= ~HPET_TN_SETVAL;
            if (hpet_enabled(s)) {
                hpet_set_timer(timer);
            }
            break;
        case HPET_TN_CMP + 4: // comparator register high order
            DPRINTF("qemu: hpet_ram_writel HPET_TN_CMP + 4\n");
            if (!timer_is_periodic(timer)
                || (timer->config & HPET_TN_SETVAL)) {
                timer->cmp = (timer->cmp & 0xffffffffULL) | new_val << 32;
            } else {
                /*
                 * FIXME: Clamp period to reasonable min value?
                 * Clamp period to reasonable max value
                 */
                new_val &= (timer->config & HPET_TN_32BIT ? ~0u : ~0ull) >> 1;
                timer->period =
                    (timer->period & 0xffffffffULL) | new_val << 32;
                }
                timer->config &= ~HPET_TN_SETVAL;
                if (hpet_enabled(s)) {
                    hpet_set_timer(timer);
                }
                break;
        case HPET_TN_ROUTE:
            timer->fsb = (timer->fsb & 0xffffffff00000000ULL) | new_val;
            break;
        case HPET_TN_ROUTE + 4:
            timer->fsb = (new_val << 32) | (timer->fsb & 0xffffffff);
            break;
        default:
            DPRINTF("qemu: invalid hpet_ram_writel\n");
            break;
        }
        hpet_publish_timer(timer);
        return;
    } else {
        switch (index) {
        case HPET_ID:
            return;
        case HPET_CFG:
            val = hpet_fixup_reg(new_val, old_val, HPET_CFG_WRITE_MASK);
            val |= qatomic_read_u64(&s->config) & 0xffffffff00000000ULL;
            if (activating_bit(old_val, new_val, HPET_CFG_ENABLE)) {
                /* Enable main counter and interrupt generation. */
                uint64_t offset =
                    ticks_to_ns(qatomic_read_u64(&s->hpet_counter)) -
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

                qatomic_set_u64(&s->hpet_offset, offset);
                qatomic_set_u64(&s->hpet_counter_last,
                                qatomic_read_u64(&s->hpet_counter));
                s->hpet_offset_ticks = (int64_t)offset / 10;
                qatomic_store_release(&s->config, val);
                for (i = 0; i < s->num_timers; i++) {
                    if ((&s->timer[i])->cmp != ~0ULL) {
                        hpet_set_timer(&s->timer[i]);
                    }
                }
            } else if (deactivating_bit(old_val, new_val, HPET_CFG_ENABLE)) {
                /* Halt main counter and disable interrupt generation. */
                uint64_t counter = hpet_get_ticks_monotonic(s);

                qatomic_set_u64(&s->hpet_counter, counter);
                qatomic_set_u64(&s->hpet_counter_last, counter);
                qatomic_store_release(&s->config, val);
                for (i = 0; i < s->num_timers; i++) {
                    hpet_del_timer(&s->timer[i]);
                }
            } else {
                qatomic_store_release(&s->config, val);
            }
            /* i8254 and RTC output pins are disabled
             * when HPET is in legacy mode */
            if (activating_bit(old_val, new_val, HPET_CFG_LEGACY)) {
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
                if (!qemu_mutex_iothread_locked() &&
                    qatomic_read(&s->iomem.device_lock) != NULL) {
                    qemu_rec_mutex_unlock(&s->mmio_lock);
                    if (lats_nobql_enabled()) {
                        qemu_set_irq(s->pit_enabled, 0);
                    } else {
                        qemu_mutex_lock_iothread();
                        qemu_set_irq(s->pit_enabled, 0);
                        qemu_mutex_unlock_iothread();
                    }
                    qemu_rec_mutex_lock(&s->mmio_lock);
                } else
#endif
                {
                    qemu_set_irq(s->pit_enabled, 0);
                }
                qemu_irq_lower(s->irqs[0]);
                qemu_irq_lower(s->irqs[RTC_ISA_IRQ]);
            } else if (deactivating_bit(old_val, new_val, HPET_CFG_LEGACY)) {
                qemu_irq_lower(s->irqs[0]);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
                if (!qemu_mutex_iothread_locked() &&
                    qatomic_read(&s->iomem.device_lock) != NULL) {
                    qemu_rec_mutex_unlock(&s->mmio_lock);
                    if (lats_nobql_enabled()) {
                        qemu_set_irq(s->pit_enabled, 1);
                    } else {
                        qemu_mutex_lock_iothread();
                        qemu_set_irq(s->pit_enabled, 1);
                        qemu_mutex_unlock_iothread();
                    }
                    qemu_rec_mutex_lock(&s->mmio_lock);
                } else
#endif
                {
                    qemu_set_irq(s->pit_enabled, 1);
                }
                qemu_set_irq(s->irqs[RTC_ISA_IRQ], s->rtc_irq_level);
            }
            break;
        case HPET_CFG + 4:
            DPRINTF("qemu: invalid HPET_CFG+4 write\n");
            break;
        case HPET_STATUS:
            val = new_val & s->isr;
            for (i = 0; i < s->num_timers; i++) {
                if (val & (1 << i)) {
                    update_irq(&s->timer[i], 0);
                }
            }
            break;
        case HPET_COUNTER:
            if (hpet_enabled(s)) {
                DPRINTF("qemu: Writing counter while HPET enabled!\n");
            }
            val = wide ? value :
                (qatomic_read_u64(&s->hpet_counter) & 0xffffffff00000000ULL) |
                value;
            qatomic_set_u64(&s->hpet_counter, val);
            qatomic_set_u64(&s->hpet_counter_last, val);
            DPRINTF("qemu: HPET counter written. ctr = 0x%" PRIx64 " -> "
                    "%" PRIx64 "\n", value,
                    qatomic_read_u64(&s->hpet_counter));
            break;
        case HPET_COUNTER + 4:
            if (hpet_enabled(s)) {
                DPRINTF("qemu: Writing counter while HPET enabled!\n");
            }
            qatomic_set_u64(&s->hpet_counter,
                (qatomic_read_u64(&s->hpet_counter) & 0xffffffffULL) |
                (((uint64_t)value) << 32));
            qatomic_set_u64(&s->hpet_counter_last,
                            qatomic_read_u64(&s->hpet_counter));
            DPRINTF("qemu: HPET counter + 4 written. ctr = 0x%" PRIx64 " -> "
                    "%" PRIx64 "\n", value,
                    qatomic_read_u64(&s->hpet_counter));
            break;
        default:
            DPRINTF("qemu: invalid hpet_ram_writel\n");
            break;
        }
    }
}

static bool hpet_ram_access_lockless(void *opaque, hwaddr addr,
                                     unsigned size, bool is_write,
                                     MemTxAttrs attrs)
{
    HPETState *s = opaque;

    if (s->local_mmio && !is_write &&
        (size == 4 || (size == 8 && addr <= HPET_LEN - 8 && !(addr & 7)))) {
        return true;
    }
    if (s->local_mmio && is_write &&
        hpet_independent_register_write(s, addr, size)) {
        return true;
    }
    if (size != 4) {
        return false;
    }
    if (addr == HPET_COUNTER || addr == HPET_COUNTER + 4) {
        return !is_write;
    }
    if (addr < 0x100 || addr >= 0x100 + 0x20 * s->num_timers) {
        return false;
    }
    unsigned reg = (addr - 0x100) % 0x20;
    /*
     * The configuration write callback checks the value before deciding
     * whether to grab BQL. The access predicate cannot inspect write data.
     */
    return reg == HPET_TN_CFG || (!is_write && reg == HPET_TN_CMP);
}

static const MemoryRegionOps hpet_ram_ops = {
    .read = hpet_ram_read,
    .write = hpet_ram_write,
    .access_lockless = hpet_ram_access_lockless,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void hpet_reset(DeviceState *d)
{
    HPETState *s = HPET(d);
    SysBusDevice *sbd = SYS_BUS_DEVICE(d);
    g_autoptr(HPETMMIOGuard) guard = hpet_lock_mmio(s);
    int i;

    for (i = 0; i < s->num_timers; i++) {
        HPETTimer *timer = &s->timer[i];

        hpet_del_timer(timer);
        timer->cmp = ~0ULL;
        timer->config = HPET_TN_PERIODIC_CAP | HPET_TN_SIZE_CAP;
        if (s->flags & (1 << HPET_MSI_SUPPORT)) {
            timer->config |= HPET_TN_FSB_CAP;
        }
        /* advertise availability of ioapic int */
        timer->config |=  (uint64_t)s->intcap << 32;
        timer->period = 0ULL;
        timer->wrap_flag = 0;
        hpet_publish_timer(timer);
    }

    qemu_set_irq(s->pit_enabled, 1);
    qatomic_set_u64(&s->hpet_counter, 0);
    qatomic_set_u64(&s->hpet_counter_last, 0);
    qatomic_set_u64(&s->hpet_offset, 0);
    s->hpet_offset_ticks = 0ULL;
    qatomic_store_release(&s->config, 0);
    hpet_cfg.hpet[s->hpet_id].event_timer_block_id = (uint32_t)s->capability;
    hpet_cfg.hpet[s->hpet_id].address = sbd->mmio[0].addr;

    /* to document that the RTC lowers its output on reset as well */
    s->rtc_irq_level = 0;
}

static void hpet_handle_legacy_irq(void *opaque, int n, int level)
{
    HPETState *s = HPET(opaque);

    if (n == HPET_LEGACY_PIT_INT) {
        if (!hpet_in_legacy_mode(s)) {
            qemu_set_irq(s->irqs[0], level);
        }
    } else {
        s->rtc_irq_level = level;
        if (!hpet_in_legacy_mode(s)) {
            qemu_set_irq(s->irqs[RTC_ISA_IRQ], level);
        }
    }
}

static void hpet_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    HPETState *s = HPET(obj);

    qemu_rec_mutex_init(&s->mmio_lock);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    /* icount may inspect all vCPUs synchronously while rearming the queue. */
    s->local_mmio = !icount_enabled() && replay_mode == REPLAY_MODE_NONE &&
        (memory_region_device_lock_enabled() ||
         lats_nobql_enabled());
#endif
    hpet_global_ptr = s;
    hpet_offset_hpet_offset_ticks = offsetof(HPETState, hpet_offset_ticks);
    hpet_offset_hpet_counter = offsetof(HPETState, hpet_counter);
    hpet_offset_hpet_counter_last = offsetof(HPETState, hpet_counter_last);
    hpet_offset_config = offsetof(HPETState, config);

    /* HPET Area */
    memory_region_init_io(&s->iomem, obj, &hpet_ram_ops, s, "hpet", HPET_LEN);
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    /* Counter reads may run concurrently on multiple vCPU threads. */
    s->iomem.disable_reentrancy_guard = true;
    if (s->local_mmio) {
        memory_region_set_device_lock(&s->iomem, &s->mmio_lock);
    }
#endif
    sysbus_init_mmio(sbd, &s->iomem);
}

static void hpet_finalize(Object *obj)
{
    HPETState *s = HPET(obj);

    for (unsigned i = 0; i < HPET_MAX_TIMERS; i++) {
        timer_free(s->timer[i].qemu_timer);
    }
    qemu_rec_mutex_destroy(&s->mmio_lock);
}

/* Called with the HPET and timer-list locks held. Do not rearm or take BQL. */
static bool hpet_timer_try_unlocked(void *opaque)
{
    HPETTimer *t = opaque;
    HPETState *s = t->state;

    if (!hpet_enabled(s) || !hpet_in_legacy_mode(s) || !timer_enabled(t) ||
        !(t->config & HPET_TN_32BIT) || t->wrap_flag || timer_is_periodic(t) ||
        timer_fsb_route(t) || (t->config & HPET_TN_TYPE_LEVEL) || (s->isr & 1) ||
        !x86_hpet_irq0_try_cached()) {
        return false;
    }
    qatomic_inc(&hpet_unlocked_expirations);
    return true;
}

static void hpet_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    HPETState *s = HPET(dev);
    int i;
    HPETTimer *timer;

    s->late_rearm = tcg_enabled() &&
        lats_nobql_enabled();

    if (!s->intcap) {
        warn_report("Hpet's intcap not initialized");
    }
    if (hpet_cfg.count == UINT8_MAX) {
        /* first instance */
        hpet_cfg.count = 0;
    }

    if (hpet_cfg.count == 8) {
        error_setg(errp, "Only 8 instances of HPET is allowed");
        return;
    }

    s->hpet_id = hpet_cfg.count++;
    hpet_local_enabled |= s->local_mmio;

    for (i = 0; i < HPET_NUM_IRQ_ROUTES; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }

    if (s->num_timers < HPET_MIN_TIMERS) {
        s->num_timers = HPET_MIN_TIMERS;
    } else if (s->num_timers > HPET_MAX_TIMERS) {
        s->num_timers = HPET_MAX_TIMERS;
    }
    int timer_attrs = 0;
#ifdef CONFIG_LATX_OPT_MMIO_LOCKLESS
    if (s->local_mmio && memory_region_device_lock_enabled() &&
        lats_nobql_enabled()) {
        timer_attrs = QEMU_TIMER_ATTR_NO_BQL;
    }
#endif
    for (i = 0; i < HPET_MAX_TIMERS; i++) {
        timer = &s->timer[i];
        timer->qemu_timer = timer_new_full(NULL, QEMU_CLOCK_VIRTUAL, SCALE_NS,
                                           timer_attrs, hpet_timer, timer);
        if (s->local_mmio) {
            timer->qemu_timer->dispatch_lock = &s->mmio_lock;
            if (i == 0 && x86_hpet_irq0_try_cached &&
                lats_nobql_enabled()) {
                timer->qemu_timer->try_dispatch_unlocked = hpet_timer_try_unlocked;
            }
        }
        timer->tn = i;
        timer->state = s;
    }

    /* 64-bit main counter; LegacyReplacementRoute. */
    s->capability = 0x8086a001ULL;
    s->capability |= (s->num_timers - 1) << HPET_ID_NUM_TIM_SHIFT;
    s->capability |= ((uint64_t)(HPET_CLK_PERIOD * FS_PER_NS) << 32);

    qdev_init_gpio_in(dev, hpet_handle_legacy_irq, 2);
    qdev_init_gpio_out(dev, &s->pit_enabled, 1);
    static bool report_registered;

    if (!report_registered) {
        report_registered = true;
        qemu_add_vm_change_state_handler(report_hpet_state, NULL);
        atexit(report_hpet_exit);
    }
}

static Property hpet_device_properties[] = {
    DEFINE_PROP_UINT8("timers", HPETState, num_timers, HPET_MIN_TIMERS),
    DEFINE_PROP_BIT("msi", HPETState, flags, HPET_MSI_SUPPORT, false),
    DEFINE_PROP_UINT32(HPET_INTCAP, HPETState, intcap, 0),
    DEFINE_PROP_BOOL("hpet-offset-saved", HPETState, hpet_offset_saved, true),
    DEFINE_PROP_END_OF_LIST(),
};

static void hpet_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = hpet_realize;
    dc->reset = hpet_reset;
    dc->vmsd = &vmstate_hpet;
    device_class_set_props(dc, hpet_device_properties);
}

static const TypeInfo hpet_device_info = {
    .name          = TYPE_HPET,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HPETState),
    .instance_init = hpet_init,
    .instance_finalize = hpet_finalize,
    .class_init    = hpet_device_class_init,
};

static void hpet_register_types(void)
{
    type_register_static(&hpet_device_info);
}

type_init(hpet_register_types)
