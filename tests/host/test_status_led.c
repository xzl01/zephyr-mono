/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <papermono/status_led.h>

int main(void)
{
	struct papermono_status_led_snapshot snapshot = { .booting = true };
	struct papermono_status_led_color color;

	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_BOOTING);
	snapshot.booting = false;
	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_READY);
	snapshot.provisioning = true;
	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_CONNECTING);
	snapshot.wifi_connected = true;
	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_CONNECTED);
	snapshot.pairing = true;
	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_PAIRING);
	snapshot.busy = true;
	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_BUSY);
	snapshot.warning = true;
	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_WARNING);
	snapshot.hard_error = true;
	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_ERROR);
	snapshot.hard_error = false;
	snapshot.warning = false;
	snapshot.busy = false;
	snapshot.pairing = false;
	snapshot.runtime_fault = true;
	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_ERROR);
	snapshot.sleeping = true;
	assert(papermono_status_led_resolve(&snapshot) ==
		PAPERMONO_STATUS_LED_OFF);
	assert(papermono_status_led_resolve(NULL) == PAPERMONO_STATUS_LED_OFF);

	assert(papermono_status_led_color(PAPERMONO_STATUS_LED_READY, &color) == 0);
	assert(color.red == 0U && color.green == 255U && color.blue == 0U);
	assert(color.brightness < 100U);
	assert(papermono_status_led_color(PAPERMONO_STATUS_LED_ERROR, &color) == 0);
	assert(color.red == 255U && color.green == 0U && color.blue == 0U);
	assert(papermono_status_led_color(PAPERMONO_STATUS_LED_ERROR, NULL) ==
		-EINVAL);
	assert(papermono_status_led_color(
		(enum papermono_status_led_state)255, &color) == -EINVAL);
	puts("status LED priority and color policy: PASS");
	return 0;
}
