#ifndef ZEPHYR_KERNEL_H_
#define ZEPHYR_KERNEL_H_

#include <stdint.h>

struct k_mutex { int unused; };

#define K_FOREVER 0
#define K_MUTEX_DEFINE(name) struct k_mutex name

static inline int k_mutex_lock(struct k_mutex *mutex, int timeout)
{
	(void)mutex;
	(void)timeout;
	return 0;
}

static inline void k_mutex_unlock(struct k_mutex *mutex)
{
	(void)mutex;
}

static inline int64_t k_uptime_get(void)
{
	return 0;
}

#endif
