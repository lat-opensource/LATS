/*
 * Lightweight runtime lock-order checker (LATS_LOCKDEP=1) and
 * VM lifecycle lock for BQL-free LATS execution.
 */
#ifndef QEMU_LOCKDEP_LITE_H
#define QEMU_LOCKDEP_LITE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum LatsLockRank {
    LOCK_RANK_BQL          = 1,
    LOCK_RANK_VM_LIFECYCLE = 2,
    LOCK_RANK_MEM_TOPO     = 3,
    LOCK_RANK_DEVICE       = 4,
    LOCK_RANK_INTC         = 5,
    LOCK_RANK_LEAF         = 6,
} LatsLockRank;

extern int lats_lockdep_active;

void lats_lockdep_acquire_impl(const void *lock, LatsLockRank rank,
                               const char *name, const char *file, int line);
void lats_lockdep_release_impl(const void *lock, const char *file, int line);
uint64_t lats_lockdep_violations(void);

#define lats_lockdep_acquire(lock, rank, name)                       \
    do {                                                             \
        if (unlikely(lats_lockdep_active)) {                         \
            lats_lockdep_acquire_impl((lock), (rank), (name),        \
                                      __FILE__, __LINE__);           \
        }                                                            \
    } while (0)

#define lats_lockdep_release(lock)                                   \
    do {                                                             \
        if (unlikely(lats_lockdep_active)) {                         \
            lats_lockdep_release_impl((lock), __FILE__, __LINE__);   \
        }                                                            \
    } while (0)

/*
 * VM lifecycle lock (LOCK_RANK_VM_LIFECYCLE = 2):
 * Recursive mutex serializing VM start/stop/reset/shutdown transitions
 * without requiring BQL.
 */
void qemu_vm_lifecycle_lock(void);
void qemu_vm_lifecycle_unlock(void);
bool qemu_vm_lifecycle_locked(void);

#endif /* QEMU_LOCKDEP_LITE_H */
