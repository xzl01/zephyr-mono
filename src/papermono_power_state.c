/* SPDX-License-Identifier: MIT */

#include <papermono/power.h>

const char *papermono_wakeup_cause_name(enum papermono_wakeup_cause cause)
{
	switch (cause) {
	case PAPERMONO_WAKE_COLD_BOOT:
		return "cold_boot";
	case PAPERMONO_WAKE_TOUCH:
		return "touch";
	case PAPERMONO_WAKE_ESP_TIMER:
		return "esp_timer";
	case PAPERMONO_WAKE_PMIC_TIMER:
		return "pmic_timer";
	case PAPERMONO_WAKE_PMIC_BUTTON:
		return "pmic_button";
	case PAPERMONO_WAKE_PMIC_EXTERNAL:
		return "pmic_external";
	case PAPERMONO_WAKE_OTHER:
	default:
		return "other";
	}
}
