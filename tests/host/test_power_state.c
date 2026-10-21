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
	assert(strcmp(papermono_wakeup_cause_name(PAPERMONO_WAKE_ESP_BUTTON),
		"esp_button") == 0);
	assert(strcmp(papermono_wakeup_cause_name(PAPERMONO_WAKE_PMIC_IRQ),
		"pmic_irq") == 0);
	assert(strcmp(papermono_wakeup_cause_name(
		(enum papermono_wakeup_cause)99), "other") == 0);
	const uint64_t buttons = (UINT64_C(1) << 2) | (UINT64_C(1) << 3);
	const uint64_t pmic = UINT64_C(1) << 1;

	assert(papermono_power_classify_ext1(UINT64_C(1) << 2, buttons, pmic) ==
		PAPERMONO_WAKE_ESP_BUTTON);
	assert(papermono_power_classify_ext1(UINT64_C(1) << 3, buttons, pmic) ==
		PAPERMONO_WAKE_ESP_BUTTON);
	assert(papermono_power_classify_ext1(buttons, buttons, pmic) ==
		PAPERMONO_WAKE_ESP_BUTTON);
	assert(papermono_power_classify_ext1(pmic, buttons, pmic) ==
		PAPERMONO_WAKE_PMIC_IRQ);
	assert(papermono_power_classify_ext1(pmic | buttons, buttons, pmic) ==
		PAPERMONO_WAKE_PMIC_IRQ);
	assert(papermono_power_classify_ext1(0, buttons, pmic) == PAPERMONO_WAKE_OTHER);
	assert(papermono_power_classify_ext1(UINT64_C(1) << 4, buttons, pmic) ==
		PAPERMONO_WAKE_OTHER);
	/* Classification must retain all 64 mask bits, including unknown inputs. */
	assert(papermono_power_classify_ext1(UINT64_C(1) << 40,
		UINT64_C(1) << 40, pmic) == PAPERMONO_WAKE_ESP_BUTTON);
	puts("power wake-cause labels and EXT1 classification: PASS");
	return 0;
}
