/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/kernel.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_KERNEL_H_
#define PAPERMONO_PREVIEW_ZEPHYR_KERNEL_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/atomic.h>

typedef int64_t k_timeout_t;
typedef int64_t k_ticks_t;

#define K_FOREVER (-1)
#define K_NO_WAIT 0
#define K_MSEC(ms) ((k_timeout_t)(ms))
#define K_SECONDS(s) ((k_timeout_t)((s) * 1000))
#define K_USEC(us) ((k_timeout_t)((us) / 1000))

#define ZRANGE(min, max, step) for (int _i = (min); _i < (max); _i += (step))

int64_t k_uptime_get(void);
uint32_t k_uptime_get_32(void);
void k_msleep(int32_t ms);
void k_sleep(k_timeout_t timeout);
void k_yield(void);

struct k_mutex {
	int unused;
};

#define K_MUTEX_DEFINE(name) struct k_mutex name = { 0 }

static inline int k_mutex_init(struct k_mutex *mutex)
{
	(void)mutex;
	return 0;
}

static inline int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
	(void)mutex;
	(void)timeout;
	return 0;
}

static inline int k_mutex_unlock(struct k_mutex *mutex)
{
	(void)mutex;
	return 0;
}

struct k_spinlock {
	int unused;
};

typedef uint32_t k_spinlock_key_t;

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
	(void)lock;
	return 0U;
}

static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	(void)lock;
	(void)key;
}

struct k_msgq {
	char *buffer_start;
	uint32_t buffer_size;
	uint32_t msg_size;
	uint32_t max_msgs;
	uint32_t used_msgs;
	uint32_t read_ptr;
	uint32_t write_ptr;
};

#define K_MSGQ_DEFINE(q_name, q_msg_size, q_max_msgs, q_align)              \
	static char __attribute__((aligned(8)))                             \
		__msgq_buf_##q_name[(q_msg_size) * (q_max_msgs)];           \
	static struct k_msgq q_name = {                                     \
		.buffer_start = __msgq_buf_##q_name,                        \
		.buffer_size = sizeof(__msgq_buf_##q_name),                 \
		.msg_size = (q_msg_size),                                   \
		.max_msgs = (q_max_msgs),                                   \
	}

int k_msgq_put(struct k_msgq *msgq, const void *data, k_timeout_t timeout);
int k_msgq_get(struct k_msgq *msgq, void *data, k_timeout_t timeout);
int k_msgq_num_used_get(struct k_msgq *msgq);

#endif /* PAPERMONO_PREVIEW_ZEPHYR_KERNEL_H_ */
