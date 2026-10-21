/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/sys/atomic.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_SYS_ATOMIC_H_
#define PAPERMONO_PREVIEW_ZEPHYR_SYS_ATOMIC_H_

#include <stdbool.h>
#include <stdint.h>

typedef long atomic_t;
typedef long atomic_val_t;

#define ATOMIC_INIT(value) (value)
#define ATOMIC_BITS (sizeof(atomic_val_t) * 8U)

static inline atomic_val_t atomic_get(const atomic_t *target)
{
	return __atomic_load_n(target, __ATOMIC_SEQ_CST);
}

static inline void atomic_set(atomic_t *target, atomic_val_t value)
{
	__atomic_store_n(target, value, __ATOMIC_SEQ_CST);
}

static inline atomic_val_t atomic_add(atomic_t *target, atomic_val_t value)
{
	return __atomic_fetch_add(target, value, __ATOMIC_SEQ_CST);
}

static inline atomic_val_t atomic_sub(atomic_t *target, atomic_val_t value)
{
	return __atomic_fetch_sub(target, value, __ATOMIC_SEQ_CST);
}

static inline atomic_val_t atomic_inc(atomic_t *target)
{
	return __atomic_fetch_add(target, 1, __ATOMIC_SEQ_CST);
}

static inline atomic_val_t atomic_dec(atomic_t *target)
{
	return __atomic_fetch_sub(target, 1, __ATOMIC_SEQ_CST);
}

static inline void atomic_clear(atomic_t *target)
{
	__atomic_store_n(target, 0, __ATOMIC_SEQ_CST);
}

static inline void atomic_or(atomic_t *target, atomic_val_t value)
{
	__atomic_fetch_or(target, value, __ATOMIC_SEQ_CST);
}

static inline void atomic_and(atomic_t *target, atomic_val_t value)
{
	__atomic_fetch_and(target, value, __ATOMIC_SEQ_CST);
}

static inline bool atomic_cas(atomic_t *target, atomic_val_t old_value,
	atomic_val_t new_value)
{
	return __atomic_compare_exchange_n(target, &old_value, new_value, false,
		__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline bool atomic_test_and_set_bit(atomic_t *target, int bit)
{
	const atomic_val_t mask = (atomic_val_t)(1UL << bit);

	return (__atomic_fetch_or(target, mask, __ATOMIC_SEQ_CST) & mask) != 0;
}

static inline void atomic_clear_bit(atomic_t *target, int bit)
{
	__atomic_fetch_and(target, ~(atomic_val_t)(1UL << bit),
		__ATOMIC_SEQ_CST);
}

static inline bool atomic_test_bit(const atomic_t *target, int bit)
{
	return (__atomic_load_n(target, __ATOMIC_SEQ_CST) &
		(atomic_val_t)(1UL << bit)) != 0;
}

#endif /* PAPERMONO_PREVIEW_ZEPHYR_SYS_ATOMIC_H_ */
