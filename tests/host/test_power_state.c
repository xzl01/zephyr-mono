/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <papermono/power.h>

int main(void)
{
	assert(strcmp(papermono_wakeup_cause_name(PAPERMONO_WAKE_COLD_BOOT),
		"cold_boot") == 0);
	assert(strcmp(papermono_wakeup_cause_name(PAPERMONO_WAKE_TOUCH),
		"touch") == 0);
	assert(strcmp(papermono_wakeup_cause_name(PAPERMONO_WAKE_ESP_TIMER),
		"esp_timer") == 0);
	assert(strcmp(papermono_wakeup_cause_name(PAPERMONO_WAKE_PMIC_TIMER),
		"pmic_timer") == 0);
	assert(strcmp(papermono_wakeup_cause_name(PAPERMONO_WAKE_PMIC_BUTTON),
		"pmic_button") == 0);
	assert(strcmp(papermono_wakeup_cause_name(PAPERMONO_WAKE_PMIC_EXTERNAL),
		"pmic_external") == 0);
	assert(strcmp(papermono_wakeup_cause_name(PAPERMONO_WAKE_OTHER),
		"other") == 0);
	assert(strcmp(papermono_wakeup_cause_name(
		(enum papermono_wakeup_cause)99), "other") == 0);
	puts("power wake-cause labels: PASS");
	return 0;
}
