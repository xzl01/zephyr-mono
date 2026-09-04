#ifndef ZEPHYR_KERNEL_H_
#define ZEPHYR_KERNEL_H_

struct k_mutex { int unused; };

#define K_FOREVER 0
#define K_MUTEX_DEFINE(name) struct k_mutex name

static inline void k_mutex_lock(struct k_mutex *mutex, int timeout)
{
	(void)mutex;
	(void)timeout;
}

static inline void k_mutex_unlock(struct k_mutex *mutex)
{
	(void)mutex;
}

#endif
