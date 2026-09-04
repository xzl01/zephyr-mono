/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_STATUS_LED_H_
#define PAPERMONO_STATUS_LED_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum papermono_status_led_state {
	PAPERMONO_STATUS_LED_OFF,
	PAPERMONO_STATUS_LED_BOOTING,
	PAPERMONO_STATUS_LED_READY,
	PAPERMONO_STATUS_LED_CONNECTING,
	PAPERMONO_STATUS_LED_CONNECTED,
	PAPERMONO_STATUS_LED_BUSY,
	PAPERMONO_STATUS_LED_PAIRING,
	PAPERMONO_STATUS_LED_WARNING,
	PAPERMONO_STATUS_LED_ERROR,
};

struct papermono_status_led_snapshot {
	bool sleeping;
	bool booting;
	bool startup_fault;
	bool runtime_fault;
	bool hard_error;
	bool warning;
	bool busy;
	bool pairing;
	bool provisioning;
	bool wifi_connected;
	bool ble_connected;
};

struct papermono_status_led_color {
	uint8_t red;
	uint8_t green;
	uint8_t blue;
	uint8_t brightness;
};

enum papermono_status_led_state papermono_status_led_resolve(
	const struct papermono_status_led_snapshot *snapshot);
int papermono_status_led_color(enum papermono_status_led_state state,
	struct papermono_status_led_color *color);
const char *papermono_status_led_state_name(
	enum papermono_status_led_state state);

int papermono_status_led_init(void);
void papermono_status_led_mark_ready(void);
void papermono_status_led_request_refresh(void);
void papermono_status_led_report_startup_fault(bool fault);
void papermono_status_led_report_runtime_fault(bool fault);
int papermono_status_led_prepare_sleep(void);
void papermono_status_led_resume(void);
enum papermono_status_led_state papermono_status_led_get_state(void);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_STATUS_LED_H_ */
