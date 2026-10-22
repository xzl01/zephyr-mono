/* SPDX-License-Identifier: GPL-3.0-only */

#ifndef PAPERMONO_SSD1677_H_
#define PAPERMONO_SSD1677_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#include <papermono/refresh.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_SSD1677_GRAY4_ROW_BYTES  100U
#define PAPERMONO_SSD1677_GRAY4_ROWS       480U
#define PAPERMONO_SSD1677_GRAY4_FRAME_SIZE \
	(PAPERMONO_SSD1677_GRAY4_ROW_BYTES * PAPERMONO_SSD1677_GRAY4_ROWS)

enum papermono_ssd1677_gray_plane {
	PAPERMONO_SSD1677_GRAY_PLANE_1 = 0,
	PAPERMONO_SSD1677_GRAY_PLANE_2 = 1,
};

/**
 * Fill a controller-native chunk of one four-gray image plane.
 *
 * The native layout has 480 rows of 100 bytes. Each byte contains eight
 * MSB-first pixels along the controller's 800-pixel RAM X axis. Plane bit
 * combinations are 00=white, 10=light gray, 01=dark gray and 11=black.
 */
typedef int (*papermono_ssd1677_gray4_reader_t)(
	void *context, enum papermono_ssd1677_gray_plane plane,
	size_t offset, uint8_t *buffer, size_t length);

/** Run the panel's built-in full four-gray OTP waveform. */
int papermono_ssd1677_refresh_gray4(
	const struct device *dev, papermono_ssd1677_gray4_reader_t reader,
	void *context);

/**
 * Quiesce the panel and remove controller power before board power-off.
 *
 * If the controller is responsive, its analog supply and clock are stopped
 * first.  The panel power gate is then disabled unconditionally so a refresh
 * interrupted with BUSY asserted cannot survive an M5PM1 shutdown/wake cycle.
 */
int papermono_ssd1677_prepare_poweroff(const struct device *dev);

/** Invalidate the partial-refresh baseline so the next update is full. */
int papermono_ssd1677_force_full_next(const struct device *dev);

/**
 * Record the refresh intent for the next blanking-off update.
 *
 * PAPERMONO_REFRESH_FAST keeps page turns on the partial path when the
 * maintenance budget is one refresh short of its limit; with
 * CONFIG_PAPERMONO_MAINTENANCE_OPPORTUNISTIC the maintenance full refresh is
 * deferred to the next AUTO/FULL render instead. PAPERMONO_REFRESH_FULL is
 * handled through papermono_ssd1677_force_full_next().
 */
int papermono_ssd1677_set_refresh_hint(
	const struct device *dev, enum papermono_refresh_quality quality);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_SSD1677_H_ */
