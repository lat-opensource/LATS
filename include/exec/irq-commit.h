/*
 * The words for an interrupt source and its local APIC can temporarily refer
 * to the same record. A single writer publishes the outcome before folding
 * either word back. Other writers can cancel an uncommitted attempt.
 */
#ifndef EXEC_IRQ_COMMIT_H
#define EXEC_IRQ_COMMIT_H

#include "qemu/atomic.h"

#define IRQ_COMMIT_TAG_MASK UINT64_C(0xffff000000000000)
#define IRQ_COMMIT_PAYLOAD  UINT64_C(0x0000ffffffffffff)
#define IRQ_COMMIT_TAG      UINT64_C(0xa523000000000000)

enum IRQCommitDecision {
    IRQ_COMMIT_PREPARING,
    IRQ_COMMIT_CANCELLED,
    IRQ_COMMIT_COMMITTED,
};
enum { IRQ_COMMIT_APIC, IRQ_COMMIT_SOURCE, IRQ_COMMIT_WORDS };

typedef struct IRQCommit {
    uint64_t serial;
    uint64_t decision;
    uint64_t before[IRQ_COMMIT_WORDS], after[IRQ_COMMIT_WORDS];
} IRQCommit;

extern IRQCommit irq_commit;

static inline bool irq_commit_tagged(uint64_t state)
{
    return (state & IRQ_COMMIT_TAG_MASK) == IRQ_COMMIT_TAG;
}

static inline uint64_t irq_commit_stamp(uint64_t tag,
                                         enum IRQCommitDecision decision)
{
    return ((tag & IRQ_COMMIT_PAYLOAD) << 2) | decision;
}

static inline uint64_t irq_commit_value(uint64_t decision, unsigned word)
{
    return (decision & 3) == IRQ_COMMIT_COMMITTED ?
           qatomic_read(&irq_commit.after[word]) :
           qatomic_read(&irq_commit.before[word]);
}

static inline void irq_commit_cancel(uint64_t tag)
{
    qatomic_cmpxchg(&irq_commit.decision,
                   irq_commit_stamp(tag, IRQ_COMMIT_PREPARING),
                   irq_commit_stamp(tag, IRQ_COMMIT_CANCELLED));
}

static inline void irq_commit_fold(uint64_t *state, uint64_t tag,
                                    unsigned word)
{
    uint64_t decision = qatomic_load_acquire(&irq_commit.decision);

    if ((decision >> 2) == (tag & IRQ_COMMIT_PAYLOAD)) {
        qatomic_cmpxchg(state, tag, irq_commit_value(decision, word));
    }
}

static inline uint64_t irq_commit_source_read(const uint64_t *state)
{
    for (;;) {
        uint64_t raw = qatomic_load_acquire(state);
        uint64_t decision, value;

        if (!irq_commit_tagged(raw)) {
            return raw;
        }
        decision = qatomic_load_acquire(&irq_commit.decision);
        if ((decision >> 2) != (raw & IRQ_COMMIT_PAYLOAD)) {
            continue;
        }
        value = irq_commit_value(decision, IRQ_COMMIT_SOURCE);
        smp_rmb();
        if (qatomic_read(state) == raw) {
            return value;
        }
    }
}

static inline uint64_t irq_commit_source_update(uint64_t *state,
                                                uint64_t clear, uint64_t set)
{
    for (;;) {
        uint64_t raw = qatomic_load_acquire(state);

        if (irq_commit_tagged(raw)) {
            irq_commit_cancel(raw);
            irq_commit_fold(state, raw, IRQ_COMMIT_SOURCE);
        } else {
            uint64_t next = (raw & ~clear) | set;

            g_assert(!irq_commit_tagged(next));
            if (qatomic_cmpxchg(state, raw, next) == raw) {
                return raw;
            }
        }
    }
}

#endif
