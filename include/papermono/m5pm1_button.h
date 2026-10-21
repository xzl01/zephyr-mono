/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_M5PM1_BUTTON_H_
#define PAPERMONO_M5PM1_BUTTON_H_

#include <stdbool.h>
#include <stdint.h>

/* IRQ_STATUS3 (0x42), per the M5Stack M5PM1 register definition. */
#define M5PM1_BUTTON_IRQ_SINGLE_CLICK (1U << 0)
#define M5PM1_BUTTON_IRQ_WAKE         (1U << 1)
#define M5PM1_BUTTON_IRQ_DOUBLE_CLICK (1U << 2)

static inline bool m5pm1_button_is_short_click(uint8_t status)
{
	/* A retained wake click must never put a just-resumed device to sleep.
	 * Double-click shutdown remains owned by the PMIC, not the UI.
	 */
	return (status & (M5PM1_BUTTON_IRQ_SINGLE_CLICK | M5PM1_BUTTON_IRQ_WAKE |
			  M5PM1_BUTTON_IRQ_DOUBLE_CLICK)) ==
		M5PM1_BUTTON_IRQ_SINGLE_CLICK;
}

#endif /* PAPERMONO_M5PM1_BUTTON_H_ */
