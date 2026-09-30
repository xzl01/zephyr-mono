/* SPDX-License-Identifier: MIT */
#ifndef PAPERMONO_READER_LAYOUT_H_
#define PAPERMONO_READER_LAYOUT_H_

#include <stdint.h>
#include <papermono/settings.h>
#include <papermono/text_layout.h>

#define PAPERMONO_READER_BODY_TOP 88
#define PAPERMONO_READER_BODY_HEIGHT 636
#define PAPERMONO_READER_FONT_HEIGHT 30

struct papermono_reader_layout {
	uint8_t margin;
	uint8_t line_space;
	uint8_t columns;
	uint8_t lines;
};

/* Pagination and LVGL must share the same geometry. The 1-bit CJK font
 * advances 24 pixels and uses a calibrated 30 pixel line box.
 * Every complete page must fit above the footer without hidden final lines.
 */
static inline struct papermono_reader_layout papermono_reader_layout_for(
	enum papermono_reader_density density)
{
	switch (density) {
	case PAPERMONO_READER_DENSITY_COMPACT:
		return (struct papermono_reader_layout) { 16, 4, 16, 18 };
	case PAPERMONO_READER_DENSITY_RELAXED:
		return (struct papermono_reader_layout) { 32, 14, 15, 14 };
	case PAPERMONO_READER_DENSITY_STANDARD:
	default:
		return (struct papermono_reader_layout) { 24, 10, 16, 16 };
	}
}

#endif
