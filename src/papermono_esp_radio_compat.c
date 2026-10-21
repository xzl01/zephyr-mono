/* SPDX-License-Identifier: GPL-3.0-only */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <esp_attr.h>

/* Kept in DRAM for debugger inspection without logging in a radio ISR. */
static atomic_t deferred_isr_yields;

/* Only the affected HAL adapter translation units redirect k_yield here.
 * Semaphore/queue operations already make their waiters ready; Zephyr's ISR
 * exit performs the switch. Calling k_yield within the ISR corrupts its
 * nesting/context bookkeeping. Thread-context yields retain normal behavior.
 */
void IRAM_ATTR papermono_esp_radio_yield(void)
{
	if (k_is_in_isr()) {
		atomic_inc(&deferred_isr_yields);
		return;
	}
	k_yield();
}
