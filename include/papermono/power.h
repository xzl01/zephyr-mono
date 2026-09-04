/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_POWER_H_
#define PAPERMONO_POWER_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum papermono_wakeup_cause {
	PAPERMONO_WAKE_COLD_BOOT,
	PAPERMONO_WAKE_TOUCH,
	PAPERMONO_WAKE_ESP_TIMER,
	PAPERMONO_WAKE_PMIC_TIMER,
	PAPERMONO_WAKE_PMIC_BUTTON,
	PAPERMONO_WAKE_PMIC_EXTERNAL,
	PAPERMONO_WAKE_OTHER,
};

int papermono_power_get_wakeup_cause(enum papermono_wakeup_cause *cause);
const char *papermono_wakeup_cause_name(enum papermono_wakeup_cause cause);
int papermono_power_acknowledge_wakeup(enum papermono_wakeup_cause cause);
int papermono_power_touch_wake_ready(uint32_t timeout_ms);
int papermono_power_enter_timed_deep_sleep(uint32_t seconds);
int papermono_power_enter_touch_deep_sleep(void);
int papermono_power_shutdown_button(void);
int papermono_power_shutdown_rtc(uint32_t delay_ms, uint32_t *actual_ms);
int papermono_power_shutdown_imu(void);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_POWER_H_ */
