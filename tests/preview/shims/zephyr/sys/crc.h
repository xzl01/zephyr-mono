/* SPDX-License-Identifier: GPL-3.0-only */

/* Preview shim: zephyr/sys/crc.h */
#ifndef PAPERMONO_PREVIEW_ZEPHYR_SYS_CRC_H_
#define PAPERMONO_PREVIEW_ZEPHYR_SYS_CRC_H_

#include <stddef.h>
#include <stdint.h>

static inline uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *data,
	size_t length)
{
	static const uint32_t table[16] = {
		0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
		0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
		0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
		0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU,
	};

	crc = ~crc;
	for (size_t index = 0U; index < length; ++index) {
		const uint8_t byte = data[index];

		crc = (crc >> 4U) ^ table[(crc ^ byte) & 0x0fU];
		crc = (crc >> 4U) ^
			table[(crc ^ ((uint32_t)byte >> 4U)) & 0x0fU];
	}
	return ~crc;
}

static inline uint32_t crc32_ieee(const uint8_t *data, size_t length)
{
	return crc32_ieee_update(0U, data, length);
}

#endif /* PAPERMONO_PREVIEW_ZEPHYR_SYS_CRC_H_ */
