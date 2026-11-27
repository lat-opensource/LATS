/*
 * Lightweight runtime lock-order checker (LATS_LOCKDEP=1) and
 * VM lifecycle lock for BQL-free LATS execution.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/lockdep-lite.h"
#include "qemu/thread.h"

#define LATS_LOCKDEP_MAX_DEPTH 64

typedef struct LatsLockdepEntry {
    const void *lock;
    LatsLockRank rank;
    const char *name;
    const char *file;
    int line;
} LatsLockdepEntry;

int lats_lockdep_active;
static uint64_t lats_lockdep_violation_count;

static __thread LatsLockdepEntry lockdep_stack[LATS_LOCKDEP_MAX_DEPTH];
static __thread unsigned lockdep_depth;

static const char *lats_lock_rank_name(LatsLockRank rank)
{
    switch (rank) {
    case LOCK_RANK_BQL:          return "BQL(1)";
    case LOCK_RANK_VM_LIFECYCLE: return "VM_LIFECYCLE(2)";
    case LOCK_RANK_MEM_TOPO:     return "MEM_TOPO(3)";
    case LOCK_RANK_DEVICE:       return "DEVICE(4)";
    case LOCK_RANK_INTC:         return "INTC(5)";
    case LOCK_RANK_LEAF:         return "LEAF(6)";
    default:                     return "UNKNOWN";
    }
}

static void __attribute__((constructor)) lats_lockdep_init(void)
{
    const char *env = getenv("LATS_LOCKDEP");
    if (env && strcmp(env, "1") == 0) {
        lats_lockdep_active = 1;
    }
}

void lats_lockdep_acquire_impl(const void *lock, LatsLockRank rank,
                               const char *name, const char *file, int line)
{
    unsigned i;
    bool recursive = false;

    for (i = 0; i < lockdep_depth; i++) {
        if (lockdep_stack[i].lock == lock) {
            recursive = true;
            break;
        }
    }

    if (!recursive) {
        for (i = 0; i < lockdep_depth; i++) {
            if (rank < lockdep_stack[i].rank) {
                unsigned j;
                qatomic_inc(&lats_lockdep_violation_count);
                fprintf(stderr,
                        "\n[LATS_LOCKDEP] LOCK ORDER VIOLATION!\n"
                        "  acquiring: %s (%p) rank=%s at %s:%d\n"
                        "  while holding higher-ranked lock: %s (%p) rank=%s at %s:%d\n"
                        "  full held lock stack (depth=%u):\n",
                        name ? name : "?", lock, lats_lock_rank_name(rank),
                        file ? file : "?", line,
                        lockdep_stack[i].name ? lockdep_stack[i].name : "?",
                        lockdep_stack[i].lock,
                        lats_lock_rank_name(lockdep_stack[i].rank),
                        lockdep_stack[i].file ? lockdep_stack[i].file : "?",
                        lockdep_stack[i].line,
                        lockdep_depth);
                for (j = 0; j < lockdep_depth; j++) {
                    fprintf(stderr, "    [%u] %s (%p) rank=%s at %s:%d\n",
                            j,
                            lockdep_stack[j].name ? lockdep_stack[j].name : "?",
                            lockdep_stack[j].lock,
                            lats_lock_rank_name(lockdep_stack[j].rank),
                            lockdep_stack[j].file ? lockdep_stack[j].file : "?",
                            lockdep_stack[j].line);
                }
                fflush(stderr);
                abort();
            }
        }
    }

    if (unlikely(lockdep_depth >= LATS_LOCKDEP_MAX_DEPTH)) {
        fprintf(stderr,
                "[LATS_LOCKDEP] lock stack overflow (depth=%u) at %s:%d (%s)\n",
                lockdep_depth, file ? file : "?", line, name ? name : "?");
        fflush(stderr);
        abort();
    }

    lockdep_stack[lockdep_depth++] = (LatsLockdepEntry){
        .lock = lock,
        .rank = rank,
        .name = name,
        .file = file,
        .line = line,
    };
}

void lats_lockdep_release_impl(const void *lock, const char *file, int line)
{
    unsigned i;

    for (i = lockdep_depth; i > 0; i--) {
        if (lockdep_stack[i - 1].lock == lock) {
            unsigned idx = i - 1;
            if (idx + 1 < lockdep_depth) {
                memmove(&lockdep_stack[idx], &lockdep_stack[idx + 1],
                        (lockdep_depth - idx - 1) * sizeof(lockdep_stack[0]));
            }
            lockdep_depth--;
            return;
        }
    }
}

uint64_t lats_lockdep_violations(void)
{
    return qatomic_read(&lats_lockdep_violation_count);
}

/* VM lifecycle recursive mutex (LOCK_RANK_VM_LIFECYCLE = 2) */
static QemuRecMutex vm_lifecycle_mutex;
static gsize vm_lifecycle_mutex_init_once;
static __thread unsigned vm_lifecycle_lock_depth;

static void vm_lifecycle_mutex_ensure_init(void)
{
    if (g_once_init_enter(&vm_lifecycle_mutex_init_once)) {
        qemu_rec_mutex_init(&vm_lifecycle_mutex);
        g_once_init_leave(&vm_lifecycle_mutex_init_once, 1);
    }
}

void qemu_vm_lifecycle_lock(void)
{
    vm_lifecycle_mutex_ensure_init();
    lats_lockdep_acquire(&vm_lifecycle_mutex, LOCK_RANK_VM_LIFECYCLE,
                         "vm_lifecycle");
    qemu_rec_mutex_lock(&vm_lifecycle_mutex);
    ++vm_lifecycle_lock_depth;
}

void qemu_vm_lifecycle_unlock(void)
{
    assert(vm_lifecycle_lock_depth > 0);
    --vm_lifecycle_lock_depth;
    qemu_rec_mutex_unlock(&vm_lifecycle_mutex);
}

bool qemu_vm_lifecycle_locked(void)
{
    return vm_lifecycle_lock_depth > 0;
}
