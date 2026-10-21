/* SPDX-License-Identifier: GPL-3.0-only */

#include <errno.h>
#include <stddef.h>

#include <papermono/frontlight.h>
#include <papermono/settings.h>

int papermono_frontlight_output_level(uint8_t percent, uint8_t *level)
{
	if (level == NULL || percent > PAPERMONO_FRONTLIGHT_MAX) {
		return -EINVAL;
	}
	*level = (uint8_t)((uint32_t)percent * UINT8_MAX /
		PAPERMONO_FRONTLIGHT_MAX);
	return 0;
}

uint8_t papermono_frontlight_next(uint8_t percent)
{
	if (percent >= PAPERMONO_FRONTLIGHT_MAX) {
		return 0U;
	}
	return (uint8_t)(((percent / PAPERMONO_FRONTLIGHT_STEP) + 1U) *
		PAPERMONO_FRONTLIGHT_STEP);
}

uint8_t papermono_frontlight_previous(uint8_t percent)
{
	if (percent == 0U || percent > PAPERMONO_FRONTLIGHT_MAX) {
		return PAPERMONO_FRONTLIGHT_MAX;
	}
	return (uint8_t)(((percent - 1U) / PAPERMONO_FRONTLIGHT_STEP) *
		PAPERMONO_FRONTLIGHT_STEP);
}
