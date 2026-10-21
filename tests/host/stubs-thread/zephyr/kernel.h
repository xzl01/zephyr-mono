/* SPDX-License-Identifier: GPL-3.0-only */
/* Host kernel shim with a REAL mutex, used only by the settings concurrency
 * regression. The default tests/host/stubs shim makes locking a no-op, which
 * cannot demonstrate lost updates. */
#ifndef ZEPHYR_KERNEL_H_
#define ZEPHYR_KERNEL_H_

#include <pthread.h>

#define K_FOREVER 0

struct k_mutex {
	pthread_mutex_t mutex;
};

#define K_MUTEX_DEFINE(name) \
	struct k_mutex name = { .mutex = PTHREAD_MUTEX_INITIALIZER }

static inline void k_mutex_lock(struct k_mutex *mutex, int timeout)
{
	(void)timeout;
	pthread_mutex_lock(&mutex->mutex);
}

static inline void k_mutex_unlock(struct k_mutex *mutex)
{
	pthread_mutex_unlock(&mutex->mutex);
}

#endif
