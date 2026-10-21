/* SPDX-License-Identifier: GPL-3.0-only */

#include <papermono/power.h>

enum papermono_wakeup_cause papermono_power_classify_ext1(
	uint64_t asserted, uint64_t button_mask, uint64_t pmic_mask)
{
	if ((asserted & pmic_mask) != 0U) {
		return PAPERMONO_WAKE_PMIC_IRQ;
	}
	if ((asserted & button_mask) != 0U) {
		return PAPERMONO_WAKE_ESP_BUTTON;
	}
	return PAPERMONO_WAKE_OTHER;
}

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
	case PAPERMONO_WAKE_ESP_BUTTON:
		return "esp_button";
	case PAPERMONO_WAKE_PMIC_IRQ:
		return "pmic_irq";
	case PAPERMONO_WAKE_OTHER:
	default:
		return "other";
	}
}
