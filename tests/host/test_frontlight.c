/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <papermono/frontlight.h>

int main(void)
{
	static const uint8_t percent[] = { 0U, 25U, 50U, 75U, 100U };
	static const uint8_t output[] = { 0U, 63U, 127U, 191U, 255U };
	uint8_t level;

	for (size_t index = 0U; index < sizeof(percent); ++index) {
		assert(papermono_frontlight_output_level(percent[index], &level) == 0);
		assert(level == output[index]);
	}
	assert(papermono_frontlight_output_level(101U, &level) == -EINVAL);
	assert(papermono_frontlight_output_level(25U, NULL) == -EINVAL);
	assert(papermono_frontlight_next(0U) == 25U);
	assert(papermono_frontlight_next(42U) == 50U);
	assert(papermono_frontlight_next(100U) == 0U);
	assert(papermono_frontlight_previous(100U) == 75U);
	assert(papermono_frontlight_previous(42U) == 25U);
	assert(papermono_frontlight_previous(0U) == 100U);
	puts("frontlight level policy: PASS");
	return 0;
}
