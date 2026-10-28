/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PAPERMONO_WEATHER_PLOT_H_
#define PAPERMONO_WEATHER_PLOT_H_

#include <stdint.h>

/**
 * Map a temperature to an inclusive pixel band [top, top + height - 1].
 * Higher values appear nearer the top; values outside the data range clamp
 * to the nearest endpoint. Flat or reversed ranges use the band's centre
 * (the upper pixel when its height is even). Nonpositive height returns top.
 * The bottom endpoint saturates at INT32_MAX if the coordinate band overflows.
 * Mapping uses nearest-pixel rounding and widened arithmetic so all int8_t
 * weather samples, and even the full int32_t input range, are safe.
 */
static inline int32_t papermono_weather_plot_y(int32_t value,
	int32_t minimum, int32_t maximum, int32_t top, int32_t height)
{
	if (height <= 1) {
		return top;
	}
	int64_t bottom = (int64_t)top + height - 1;

	if (bottom > INT32_MAX) {
		bottom = INT32_MAX;
	}
	const int64_t span = bottom - top;

	if (maximum <= minimum) {
		return (int32_t)((int64_t)top + span / 2);
	}
	int64_t clamped = value;

	if (clamped < minimum) {
		clamped = minimum;
	} else if (clamped > maximum) {
		clamped = maximum;
	}
	const int64_t range = (int64_t)maximum - minimum;
	const int64_t distance = (clamped - minimum) * span;

	return (int32_t)(bottom - (distance + range / 2) / range);
}

#endif /* PAPERMONO_WEATHER_PLOT_H_ */
