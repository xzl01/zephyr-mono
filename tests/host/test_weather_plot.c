/* SPDX-License-Identifier: GPL-3.0-only */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <papermono/weather_plot.h>

int main(void)
{
	const int32_t top = 464;
	const int32_t height = 96;
	const int32_t bottom = top + height - 1;

	/* A fixed plotting band includes both endpoints, without an extra pixel. */
	assert(papermono_weather_plot_y(0, 0, 40, top, height) == bottom);
	assert(papermono_weather_plot_y(40, 0, 40, top, height) == top);
	assert(papermono_weather_plot_y(20, 0, 40, top, height) == 511);
	assert(papermono_weather_plot_y(-10, -30, 10, top, height) == 511);
	assert(papermono_weather_plot_y(-30, -30, 10, top, height) == bottom);
	assert(papermono_weather_plot_y(10, -30, 10, top, height) == top);

	/* Flat and reversed ranges remain centred; tiny ranges still span the band. */
	assert(papermono_weather_plot_y(20, 20, 20, top, height) == 511);
	assert(papermono_weather_plot_y(20, 30, 10, top, height) == 511);
	assert(papermono_weather_plot_y(20, 20, 20, top, 95) == 511);
	assert(papermono_weather_plot_y(6, 6, 7, top, 3) == top + 2);
	assert(papermono_weather_plot_y(7, 6, 7, top, 3) == top);

	/* Bounds clamp before scaling, including values outside normal weather. */
	assert(papermono_weather_plot_y(INT32_MIN, -30, 10, top, height) == bottom);
	assert(papermono_weather_plot_y(INT32_MAX, -30, 10, top, height) == top);
	assert(papermono_weather_plot_y(INT8_MIN, INT8_MIN, INT8_MAX, 400, 101) == 500);
	assert(papermono_weather_plot_y(INT8_MAX, INT8_MIN, INT8_MAX, 400, 101) == 400);
	assert(papermono_weather_plot_y(0, INT8_MIN, INT8_MAX, 400, 101) == 450);
	int32_t previous = bottom;

	for (int32_t value = INT8_MIN; value <= INT8_MAX; ++value) {
		const int32_t y = papermono_weather_plot_y(value, INT8_MIN,
			INT8_MAX, top, height);

		assert(y >= top && y <= bottom);
		assert(y <= previous);
		previous = y;
	}

	/* Degenerate geometry never divides by zero or leaves its anchor. */
	assert(papermono_weather_plot_y(10, 0, 40, top, 0) == top);
	assert(papermono_weather_plot_y(10, 0, 40, top, -1) == top);
	assert(papermono_weather_plot_y(10, 0, 40, top, 1) == top);
	assert(papermono_weather_plot_y(20, 20, 20, top, 0) == top);

	/* Widened arithmetic also covers the extreme input and coordinate ranges. */
	assert(papermono_weather_plot_y(INT32_MIN, INT32_MIN, INT32_MAX,
		INT32_MIN, INT32_MAX) == -2);
	assert(papermono_weather_plot_y(INT32_MAX, INT32_MIN, INT32_MAX,
		INT32_MIN, INT32_MAX) == INT32_MIN);
	const int32_t middle = papermono_weather_plot_y(0, INT32_MIN,
		INT32_MAX, INT32_MIN, INT32_MAX);

	assert(middle > INT32_MIN && middle < -2);
	assert(papermono_weather_plot_y(0, 0, 10, INT32_MAX - 2, 100) == INT32_MAX);
	assert(papermono_weather_plot_y(10, 0, 10, INT32_MAX - 2, 100) == INT32_MAX - 2);
	assert(papermono_weather_plot_y(5, 5, 5, INT32_MAX - 2, 100) == INT32_MAX - 1);
	assert(papermono_weather_plot_y(0, 0, 10, INT32_MAX, 100) == INT32_MAX);
	puts("weather plot pixel bounds: PASS");
	return 0;
}
