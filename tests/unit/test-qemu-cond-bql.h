/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEST_QEMU_COND_BQL_H
#define TEST_QEMU_COND_BQL_H

#include <pthread.h>

/* GNU linker wrapping hooks used only by this regression test. */
int __real_pthread_spin_unlock(pthread_spinlock_t *spin);
int __wrap_pthread_spin_unlock(pthread_spinlock_t *spin);

#endif
