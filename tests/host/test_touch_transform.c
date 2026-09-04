/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include <papermono/touch_transform.h>

int main(void)
{
	struct papermono_touch_point point = { 0 };

	/* Official UserDemo rotation 0 and CrossMux's composed transform both
	 * preserve the FT6336 portrait coordinates on Paper Mono.
	 */
	assert(papermono_touch_map_portrait(0, 0, 480, 800, &point) == 0);
	assert(point.x == 0 && point.y == 0);
	assert(papermono_touch_map_portrait(240, 120, 480, 800, &point) == 0);
	assert(point.x == 240 && point.y == 120);
	assert(papermono_touch_map_portrait(479, 799, 480, 800, &point) == 0);
	assert(point.x == 479 && point.y == 799);

	assert(papermono_touch_map_portrait(480, 0, 480, 800, &point) == -ERANGE);
	assert(papermono_touch_map_portrait(0, 800, 480, 800, &point) == -ERANGE);
	assert(papermono_touch_map_portrait(0, 0, 0, 800, &point) == -EINVAL);
	assert(papermono_touch_map_portrait(0, 0, 480, 800, NULL) == -EINVAL);

	puts("Paper Mono touch coordinate transform: PASS");
	return 0;
}
