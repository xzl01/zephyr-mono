/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <stdio.h>

#include <papermono/m5pm1_button.h>

int main(void)
{
	assert(!m5pm1_button_is_short_click(0));
	assert(m5pm1_button_is_short_click(M5PM1_BUTTON_IRQ_SINGLE_CLICK));
	assert(!m5pm1_button_is_short_click(M5PM1_BUTTON_IRQ_WAKE));
	assert(!m5pm1_button_is_short_click(M5PM1_BUTTON_IRQ_DOUBLE_CLICK));
	assert(!m5pm1_button_is_short_click(M5PM1_BUTTON_IRQ_SINGLE_CLICK |
		M5PM1_BUTTON_IRQ_WAKE));
	assert(!m5pm1_button_is_short_click(M5PM1_BUTTON_IRQ_SINGLE_CLICK |
		M5PM1_BUTTON_IRQ_DOUBLE_CLICK));
	/* Reserved status bits neither invent nor suppress a real single click. */
	for (unsigned int status = 0; status <= UINT8_MAX; ++status) {
		assert(m5pm1_button_is_short_click(status) == ((status & 7U) == 1U));
	}
	puts("M5PM1 awake click / retained wake / double-click policy: PASS");
	return 0;
}
